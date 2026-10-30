#include "std_include.hpp"
#include "tex_pack.hpp"
#include "tex_archive.hpp"
#include "call_chain.hpp"
#include "scene_graph.hpp"
#include "shared/fnv_exe.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

#include <atomic>
#include <deque>
#include <condition_variable>

/*
 * Texture-pack height maps.
 *
 * Path capture
 * ------------
 * NiDX9TextureData::SetD3DTexture(IDirect3DBaseTexture9*) (fnv_exe
 * set_d3d_texture) is the NiDX9SourceTextureData override (fnv_exe
 * vtbl_source_texture_data, slot 0xB0; the rendered / dynamic texture data
 * share a different override at 0xE8A120). At entry
 * ECX = the renderer data, [ecx+0x08] = the owning NiSourceTexture and
 * [owner+0x34] = its file name ("textures\foo\bar.dds"). The hook calls the
 * original and then records texture pointer -> normalised path. It runs on
 * the engine's loader threads as well as the main thread, so the map is
 * behind an SRW lock; the render thread takes it shared once per protocol
 * draw.
 *
 * Address reuse: a texture freed on cell unload can have its address handed
 * to the next allocation. Every texture the game creates goes through the
 * proxy's Create*Texture (D3DX included: it calls back into the device it
 * was given), which drops the stale entry before SetD3DTexture records the
 * new one.
 *
 * Loading
 * -------
 * One worker thread resolves and reads files (loose first, then the BSAs,
 * see tex_archive.cpp) so a cell load never blocks the render thread on disk
 * or inflate. Texture creation needs the device, so it happens in
 * on_present() on the render thread, capped per frame by count and bytes; everything
 * crossing the bridge (the level data) is bounded by that cap. MANAGED pool:
 * survives Reset, and stays readable for a later Remix-API upload.
 *
 * States of one diffuse path:
 *   Idle -> Queued (render thread asked) -> Missing (no height file)
 *                                        -> Loaded (bytes read) -> Ready | Failed
 *   Ready -> Idle when unused for EvictFrames (texture released; re-read on
 *   the next use).
 */

namespace comp::game::tex_pack
{
	namespace
	{
		using shared::common::log;
		using shared::common::LOG_TYPE;

		enum State : uint8_t { kIdle = 0, kQueued, kMissing, kLoaded, kReady, kFailed };

		struct PathRec
		{
			std::string diffuse;            // normalised diffuse path (immutable)
			std::atomic<uint8_t> state{ kIdle };
			// Written by the worker before state -> kLoaded / kMissing.
			std::string height;
			std::vector<uint8_t> bytes;
			TextureFs::Source source = TextureFs::Source::None;
			// Render thread only.
			IDirect3DTexture9* tex = nullptr;
			UINT last_used = 0;
			bool logged_bound = false;
		};

		struct Stats
		{
			std::atomic<uint32_t> hook_calls{ 0 };
			std::atomic<uint32_t> hook_recorded{ 0 };
			std::atomic<uint32_t> hook_other_vtbl{ 0 };
			std::atomic<uint32_t> hook_bad_path{ 0 };
			std::atomic<uint32_t> created_erase{ 0 };
			std::atomic<uint32_t> queued{ 0 };
			std::atomic<uint32_t> found_loose{ 0 };
			std::atomic<uint32_t> found_archive{ 0 };
			std::atomic<uint32_t> missing{ 0 };
			std::atomic<uint32_t> read_failed{ 0 };
			uint32_t created = 0;          // render thread
			uint32_t create_failed = 0;
			uint32_t evicted = 0;
			uint64_t created_bytes = 0;
			uint32_t draws_bound = 0;      // since last stats line
			uint32_t draws_bound_total = 0;
		};

		struct Global
		{
			SRWLOCK lock = SRWLOCK_INIT;   // guards paths / by_tex
			std::unordered_map<std::string, std::unique_ptr<PathRec>> paths;
			std::unordered_map<IDirect3DBaseTexture9*, PathRec*> by_tex;
			std::atomic<uint32_t> epoch{ 0 };

			// Worker queue.
			std::mutex qmu;
			std::condition_variable qcv;
			std::deque<PathRec*> queue;
			bool worker_started = false;

			// Worker -> render thread.
			std::mutex dmu;
			std::deque<PathRec*> done;
			std::atomic<uint64_t> done_bytes{ 0 };

			std::vector<std::string> suffixes;
			Stats st;

			// Render-thread one-entry memo for the per-draw lookup.
			IDirect3DBaseTexture9* memo_tex = nullptr;
			PathRec* memo_rec = nullptr;
			uint32_t memo_epoch = 0xFFFFFFFFu;

			UINT last_stats_frame = 0;
			bool logged_first_found = false;
			bool logged_first_bound = false;
			std::atomic<bool> logged_path_form{ false };   // hook threads
		};

		// Leaked on purpose: loader threads and the worker may still touch it
		// while the process tears down.
		Global& g()
		{
			static Global* s = new Global();
			return *s;
		}

		bool g_hook_installed = false;

		// The worker stops reading ahead while more than this many bytes wait
		// for creation on the render thread.
		constexpr uint64_t kMaxPendingBytes = 96ull * 1024 * 1024;

		// NiDX9SourceTextureData's vtable slot of SetD3DTexture.
		constexpr unsigned int kSetD3DTextureSlot = 0xB0;

		// What a module that patches the function or its slot after the hook means for it.
		constexpr const char* kIfReplaced = "Texture-pack height maps keep finding texture paths only if that code "
			"calls what it replaced.";

		UINT frame_now() { return shared::common::ffp_state::get().frame_count(); }

		// ---- Path normalisation ------------------------------------------------

		// Engine form is "textures\foo\bar.dds" (TESLandTexture paths gain a
		// "Data\Textures\" prefix; absolute paths are possible from mods).
		// Result: lower case, backslashes, starting "textures\", ending ".dds".
		bool normalize(const char* raw, std::string& out)
		{
			std::string s;
			s.reserve(128);
			char prev = 0;
			for (const char* p = raw; *p; ++p)
			{
				char c = *p;
				if (c == '/') c = '\\';
				else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
				if (c == '\\' && prev == '\\') continue;
				s.push_back(c);
				prev = c;
			}
			while (s.rfind(".\\", 0) == 0) s.erase(0, 2);
			while (!s.empty() && s[0] == '\\') s.erase(0, 1);

			if (const size_t d = s.find("data\\textures\\"); d != std::string::npos && (d == 0 || s[d - 1] == '\\'))
				s.erase(0, d + 5);
			else if (s.rfind("textures\\", 0) == 0) {}
			else if (const size_t t = s.find("\\textures\\"); t != std::string::npos)
				s.erase(0, t + 1);
			else
				s = "textures\\" + s;

			if (s.size() < 14 || s.compare(s.size() - 4, 4, ".dds") != 0) return false;
			out = std::move(s);
			return true;
		}

		// ---- Hook ----------------------------------------------------------------

		using SetD3DTexture_t = int(__fastcall*)(void* self, void* edx, IDirect3DBaseTexture9* tex);
		SetD3DTexture_t g_orig_set_d3d_texture = nullptr;

		// SEH-guarded copy of the owner's file name. No C++ objects here.
		// Returns 1 = copied, 0 = no path, -1 = not a NiDX9SourceTextureData.
		int read_owner_path(void* self, char* buf, size_t n)
		{
			__try
			{
				const auto* obj = static_cast<const unsigned char*>(self);
				if (*reinterpret_cast<const uintptr_t*>(obj) != fnv_exe::addr().vtbl_source_texture_data) return -1;
				const auto* owner = *reinterpret_cast<const unsigned char* const*>(obj + 0x08);
				if (!owner) return 0;
				const char* path = *reinterpret_cast<const char* const*>(owner + 0x34);
				if (!path) return 0;
				size_t i = 0;
				for (; i + 1 < n && path[i]; i++) buf[i] = path[i];
				buf[i] = '\0';
				return (i > 0 && i + 1 < n) ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return 0;
			}
		}

		void record(IDirect3DBaseTexture9* tex, const char* raw)
		{
			auto& G = g();
			std::string norm;
			if (!normalize(raw, norm))
			{
				G.st.hook_bad_path++;
				return;
			}

			if (!G.logged_path_form.exchange(true))
			{
				log("TexPack", std::format("first texture path seen at SetD3DTexture: '{}' -> '{}'", raw, norm),
					LOG_TYPE::LOG_TYPE_STATUS, false);
			}

			AcquireSRWLockExclusive(&G.lock);
			auto& slot = G.paths[norm];
			if (!slot)
			{
				slot = std::make_unique<PathRec>();
				slot->diffuse = norm;
			}
			G.by_tex[tex] = slot.get();
			G.epoch.fetch_add(1, std::memory_order_release);
			ReleaseSRWLockExclusive(&G.lock);
			G.st.hook_recorded++;
		}

		int __fastcall set_d3d_texture_hk(void* self, void* edx, IDirect3DBaseTexture9* tex)
		{
			const int r = g_orig_set_d3d_texture(self, edx, tex);
			auto& G = g();
			G.st.hook_calls++;
			if (tex)
			{
				char buf[512];
				const int got = read_owner_path(self, buf, sizeof(buf));
				if (got > 0) record(tex, buf);
				else if (got < 0) G.st.hook_other_vtbl++;
			}
			return r;
		}

		// ---- Worker --------------------------------------------------------------

		DWORD WINAPI worker_main(LPVOID)
		{
			auto& G = g();
			const auto& cfg = shared::common::config::get().texture_packs;

			char exe[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			std::string data_dir = std::filesystem::path(exe).parent_path().string() + "\\Data\\";

			std::string detail;
			auto archives = discover_archives(data_dir, cfg.extra_archives, detail);
			log("TexPack", std::format("archive order: {} -> {} archive(s): {}", detail, archives.size(),
				[&] { std::string s; for (const auto& a : archives) s += (s.empty() ? "" : ", ") + a; return s; }()),
				LOG_TYPE::LOG_TYPE_STATUS, false);

			auto* fs = new TextureFs();   // leaked with the rest (see g())
			fs->configure(data_dir, archives);

			for (;;)
			{
				PathRec* rec = nullptr;
				{
					std::unique_lock lk(G.qmu);
					G.qcv.wait(lk, [&] { return !G.queue.empty(); });
					rec = G.queue.front();
					G.queue.pop_front();
				}

				// Back-pressure: don't read ahead of what the render thread creates.
				while (G.done_bytes.load(std::memory_order_acquire) > kMaxPendingBytes) Sleep(5);

				const bool first_index = !fs->indexed();
				const std::string base = rec->diffuse.substr(0, rec->diffuse.size() - 4);
				bool found = false;
				for (const auto& suf : G.suffixes)
				{
					const std::string cand = base + suf + ".dds";
					if (fs->locate(cand) == TextureFs::Source::None) continue;
					std::string err;
					TextureFs::Source src;
					if (!fs->read(cand, rec->bytes, src, err))
					{
						G.st.read_failed++;
						log("TexPack", std::format("height map '{}' found but unreadable: {}", cand, err),
							LOG_TYPE::LOG_TYPE_WARN, false);
						continue;
					}
					rec->height = cand;
					rec->source = src;
					found = true;
					break;
				}

				if (first_index && fs->indexed())
				{
					const auto& s = fs->stats();
					log("TexPack", std::format("archives indexed: {} opened, {} failed, {} texture entries",
						s.archives_opened, s.archives_failed, s.texture_entries), LOG_TYPE::LOG_TYPE_GREEN, false);
					for (const auto& line : fs->archive_report())
						log("TexPack", std::format("  {}", line), LOG_TYPE::LOG_TYPE_DEFAULT, false);
				}

				if (!found)
				{
					G.st.missing++;
					rec->state.store(kMissing, std::memory_order_release);
					continue;
				}

				if (rec->source == TextureFs::Source::Loose) G.st.found_loose++;
				else G.st.found_archive++;
				if (!G.logged_first_found || cfg.log_resolves)
				{
					G.logged_first_found = true;
					log("TexPack", std::format("height map found: '{}' for '{}' ({}, {} KB)", rec->height, rec->diffuse,
						rec->source == TextureFs::Source::Loose ? "loose" : "archive", rec->bytes.size() / 1024),
						LOG_TYPE::LOG_TYPE_GREEN, false);
				}

				G.done_bytes.fetch_add(rec->bytes.size(), std::memory_order_acq_rel);
				rec->state.store(kLoaded, std::memory_order_release);
				std::lock_guard lk(G.dmu);
				G.done.push_back(rec);
			}
		}

		void start_worker()
		{
			auto& G = g();
			if (G.worker_started) return;
			G.worker_started = true;
			const HANDLE t = CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
			if (t)
			{
				SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
				CloseHandle(t);
			}
			else
			{
				log("TexPack", "failed to start the loader thread; height maps disabled", LOG_TYPE::LOG_TYPE_ERROR, true);
			}
		}

		void enqueue(PathRec* rec)
		{
			auto& G = g();
			start_worker();
			rec->state.store(kQueued, std::memory_order_release);
			G.st.queued++;
			{
				std::lock_guard lk(G.qmu);
				G.queue.push_back(rec);
			}
			G.qcv.notify_one();
		}

		void create_pending(IDirect3DDevice9* dev)
		{
			auto& G = g();
			const auto& cfg = shared::common::config::get().texture_packs;
			const uint64_t byte_cap = static_cast<uint64_t>(cfg.max_load_kb_per_frame) * 1024;
			uint64_t bytes = 0;
			int count = 0;

			while (count < cfg.max_loads_per_frame)
			{
				PathRec* rec = nullptr;
				{
					std::lock_guard lk(G.dmu);
					if (G.done.empty()) break;
					// Always create at least one per frame, however large.
					if (count > 0 && bytes + G.done.front()->bytes.size() > byte_cap) break;
					rec = G.done.front();
					G.done.pop_front();
				}

				const size_t n = rec->bytes.size();
				IDirect3DTexture9* tex = nullptr;
				D3DXIMAGE_INFO info = {};
				const HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(dev, rec->bytes.data(), static_cast<UINT>(n),
					D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, D3DX_FROM_FILE, 0, D3DFMT_FROM_FILE,
					D3DPOOL_MANAGED, D3DX_FILTER_NONE, D3DX_DEFAULT, 0, &info, nullptr, &tex);

				rec->bytes.clear();
				rec->bytes.shrink_to_fit();
				G.done_bytes.fetch_sub(n, std::memory_order_acq_rel);
				bytes += n;
				count++;

				if (FAILED(hr) || !tex)
				{
					G.st.create_failed++;
					rec->state.store(kFailed, std::memory_order_release);
					log("TexPack", std::format("height map '{}' failed to load (hr=0x{:08X})", rec->height,
						static_cast<unsigned>(hr)), LOG_TYPE::LOG_TYPE_WARN, false);
					continue;
				}

				if (G.st.created == 0)
					log("TexPack", std::format("first height map created: '{}' {}x{} fmt={} mips={}", rec->height,
						info.Width, info.Height, static_cast<int>(info.Format), info.MipLevels), LOG_TYPE::LOG_TYPE_GREEN, false);
				G.st.created++;
				G.st.created_bytes += n;
				rec->tex = tex;
				rec->last_used = frame_now();
				rec->state.store(kReady, std::memory_order_release);
			}
		}

		void evict(IDirect3DDevice9* dev)
		{
			auto& G = g();
			const UINT now = frame_now();
			const UINT keep = static_cast<UINT>(shared::common::config::get().texture_packs.evict_frames);
			std::vector<PathRec*> stale;

			AcquireSRWLockShared(&G.lock);
			for (auto& kv : G.paths)
			{
				PathRec* r = kv.second.get();
				if (r->state.load(std::memory_order_acquire) == kReady && r->tex && now - r->last_used > keep)
					stale.push_back(r);
			}
			ReleaseSRWLockShared(&G.lock);
			if (stale.empty()) return;

			// Unbind first: a sticky extra stage may hold one of these.
			shared::common::ffp_state::get().flush_extra_textures(dev);
			for (PathRec* r : stale)
			{
				r->tex->Release();
				r->tex = nullptr;
				r->state.store(kIdle, std::memory_order_release);
				G.st.evicted++;
			}
		}

		void log_stats()
		{
			auto& G = g();
			auto& s = G.st;
			uint32_t mapped, paths;
			AcquireSRWLockShared(&G.lock);
			mapped = static_cast<uint32_t>(G.by_tex.size());
			paths = static_cast<uint32_t>(G.paths.size());
			ReleaseSRWLockShared(&G.lock);
			log("TexPack", std::format(
				"stats: hook calls={} recorded={} otherVtbl={} badPath={} createdErase={} | textures mapped={} paths={} | "
				"height queued={} found loose={} archive={} missing={} readFail={} | created={} ({} MB) failed={} evicted={} | "
				"draws bound={} (total {})",
				s.hook_calls.load(), s.hook_recorded.load(), s.hook_other_vtbl.load(), s.hook_bad_path.load(),
				s.created_erase.load(), mapped, paths, s.queued.load(), s.found_loose.load(), s.found_archive.load(),
				s.missing.load(), s.read_failed.load(), s.created, s.created_bytes >> 20, s.create_failed, s.evicted,
				s.draws_bound, s.draws_bound_total), LOG_TYPE::LOG_TYPE_STATUS, false);
			s.draws_bound = 0;
		}

		std::vector<std::string> parse_suffixes(const std::string& list)
		{
			std::vector<std::string> out;
			size_t start = 0;
			while (start <= list.size())
			{
				size_t end = list.find(',', start);
				if (end == std::string::npos) end = list.size();
				std::string item;
				for (size_t i = start; i < end; i++)
				{
					char c = list[i];
					if (c == ' ' || c == '\t' || c == '"') continue;
					if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
					item.push_back(c);
				}
				if (!item.empty() && item.find_first_of("\\/.") == std::string::npos) out.push_back(item);
				start = end + 1;
			}
			return out;
		}
	}

	bool enabled()
	{
		return g_hook_installed && shared::common::config::get().texture_packs.height_maps;
	}

	bool hook_installed()
	{
		return g_hook_installed;
	}

	void install_hook()
	{
		const auto& cfg = shared::common::config::get().texture_packs;
		if (!cfg.height_maps)
		{
			log("TexPack", "height maps: hook SKIPPED (config disabled)");
			return;
		}

		auto& G = g();
		G.suffixes = parse_suffixes(cfg.height_suffixes);
		if (G.suffixes.empty())
		{
			log("TexPack", "height maps: no usable HeightSuffixes - disabled", LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}

		if (!fnv_exe::known())
		{
			log("TexPack", "height maps: hook SKIPPED (unknown executable)", LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}

		// The function runs for every caller, the slot's included: a module
		// that replaced the slot reaches it when its code calls what it
		// replaced. The slot is tracked either way.
		const auto& addr = fnv_exe::addr();
		const fnv_exe::Code& fn = addr.set_d3d_texture;
		const uintptr_t slot = addr.vtbl_source_texture_data + kSetD3DTextureSlot;
		uint32_t slot_now = 0;
		if (!scene_graph::read_u32(slot, slot_now))
		{
			log("TexPack", std::format("NiDX9SourceTextureData vtable slot 0x{:X} is not readable - NOT hooking. "
				"Texture-pack height maps disabled.", slot), LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		call_chain::Site in_slot;
		in_slot.state = slot_now == fn.addr ? call_chain::State::Vanilla : call_chain::State::Chained;
		in_slot.target = slot_now;
		in_slot.owner = call_chain::owner_of(slot_now);
		if (slot_now != fn.addr)
			log("TexPack", std::format("NiDX9SourceTextureData vtable slot 0x{:X} holds 0x{:X} in {} instead of "
				"SetD3DTexture 0x{:X}: another mod replaced it; the hook at the function runs when that mod's code "
				"calls the function it replaced.", slot, slot_now, in_slot.owner, fn.addr));

		if (!call_chain::hook_entry(fn, reinterpret_cast<void*>(&set_d3d_texture_hk),
			reinterpret_cast<void**>(&g_orig_set_d3d_texture), "TexPack", "NiDX9SourceTextureData::SetD3DTexture",
			kIfReplaced))
		{
			log("TexPack", "SetD3DTexture is not hooked - height maps disabled", LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		call_chain::track(call_chain::Kind::Slot, slot, slot_now, in_slot, "TexPack",
			"NiDX9SourceTextureData vtable slot of SetD3DTexture", kIfReplaced);

		g_hook_installed = true;
		std::string sfx;
		for (const auto& s : G.suffixes) sfx += (sfx.empty() ? "" : ",") + s;
		log("TexPack", std::format("height maps: hooked NiDX9SourceTextureData::SetD3DTexture @0x{:X}, suffixes {}",
			fn.addr, sfx), LOG_TYPE::LOG_TYPE_GREEN, false);
	}

	void on_texture_created(IDirect3DBaseTexture9* tex)
	{
		if (!g_hook_installed || !tex) return;
		auto& G = g();
		AcquireSRWLockExclusive(&G.lock);
		if (G.by_tex.erase(tex))
		{
			G.epoch.fetch_add(1, std::memory_order_release);
			G.st.created_erase++;
		}
		ReleaseSRWLockExclusive(&G.lock);
	}

	IDirect3DTexture9* height_for(IDirect3DBaseTexture9* diffuse, bool* pending)
	{
		if (pending) *pending = false;
		if (!diffuse || !enabled()) return nullptr;
		auto& G = g();

		PathRec* rec;
		const uint32_t ep = G.epoch.load(std::memory_order_acquire);
		if (diffuse == G.memo_tex && ep == G.memo_epoch)
		{
			rec = G.memo_rec;
		}
		else
		{
			AcquireSRWLockShared(&G.lock);
			const auto it = G.by_tex.find(diffuse);
			rec = it != G.by_tex.end() ? it->second : nullptr;
			ReleaseSRWLockShared(&G.lock);
			G.memo_tex = diffuse;
			G.memo_rec = rec;
			G.memo_epoch = ep;
		}
		if (!rec) return nullptr;

		switch (rec->state.load(std::memory_order_acquire))
		{
		case kIdle:
			enqueue(rec);
			if (pending) *pending = true;
			return nullptr;
		case kReady:
			rec->last_used = frame_now();
			return rec->tex;
		case kQueued:
		case kLoaded:
			if (pending) *pending = true;
			return nullptr;
		default:   // kMissing / kFailed
			return nullptr;
		}
	}

	void note_bound(IDirect3DBaseTexture9* diffuse, UINT stage)
	{
		auto& G = g();
		G.st.draws_bound++;
		G.st.draws_bound_total++;
		if (G.logged_first_bound) return;
		G.logged_first_bound = true;
		PathRec* rec = (diffuse == G.memo_tex) ? G.memo_rec : nullptr;
		log("TexPack", std::format("first draw with a pack height map: '{}' at s{} (RS 149 height nibble set)",
			rec ? rec->height : std::string("?"), stage), LOG_TYPE::LOG_TYPE_GREEN, false);
	}

	void on_present(IDirect3DDevice9* dev)
	{
		if (!enabled() || !dev) return;
		auto& G = g();
		create_pending(dev);

		const UINT now = frame_now();
		if ((now & 127u) == 0) evict(dev);

		const auto interval = static_cast<UINT>(shared::common::config::get().texture_packs.stats_interval_frames);
		if (interval && now - G.last_stats_frame >= interval)
		{
			G.last_stats_frame = now;
			log_stats();
		}
	}
}
