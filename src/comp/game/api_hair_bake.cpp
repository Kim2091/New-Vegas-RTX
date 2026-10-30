#include "std_include.hpp"
#include "api_hair_bake.hpp"
#include "api_identity.hpp"
#include "api_textures.hpp"
#include "ps_interp.hpp"
#include "shader_bake.hpp"

#include <condition_variable>
#include <deque>

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"

// See api_hair_bake.hpp.

namespace comp::game::api::hair_bake
{
	namespace
	{
		// Bakes queued or finished and not uploaded yet (each holds its
		// captured or baked bytes, about 1.5 MB).
		constexpr size_t kMaxPending = 4;
		// Bounds on the caches; past them, new keys wait for the eviction.
		constexpr size_t kMaxDrawKeys = 4096;
		constexpr size_t kMaxBakes = 512;
		// Bake lines and refusal lines in the log, per session.
		constexpr uint32_t kBakeLogCap = 64;
		constexpr uint32_t kRefusalLogCap = 64;

		// What a hair PS allows, read once per PS hash.
		struct PsInfo
		{
			std::string why;                    // empty: bakeable
			std::vector<uint8_t> bytecode;
			std::vector<uint8_t> material_regs;  // float registers whose draw values the bake keeps
		};
		std::unordered_map<uint32_t, PsInfo> g_ps;

		// Pointer key -> content key (0: refused before one).
		struct DrawKey
		{
			uint64_t content = 0;
			UINT last_seen = 0;
		};
		std::unordered_map<uint64_t, DrawKey> g_draws;

		enum class State : uint8_t { Queued, Ready, Uploaded, Refused };
		struct Bake
		{
			State state = State::Queued;
			uint32_t ps_hash = 0;
			uint64_t hash = 0;                               // the upload (State::Uploaded)
			std::unique_ptr<shader_bake::Result> result;     // State::Ready
			UINT last_used = 0;
		};
		std::unordered_map<uint64_t, Bake> g_bakes;
		size_t g_pending = 0;   // Queued + Ready

		struct Counters
		{
			uint64_t queued = 0, done = 0, refused = 0, failed = 0, uploads = 0;
			uint64_t served = 0;          // draws given a baked albedo
			uint64_t content_hits = 0;    // new pointer keys whose content was already baked or baking
			// Render thread: sampled hashes of a new pointer key, and captures.
			uint64_t keys = 0, captures = 0;
			double key_ms = 0.0, key_max = 0.0, capture_ms = 0.0, capture_max = 0.0;
			// Bake thread.
			uint64_t baked = 0;
			double worker_ms = 0.0, worker_max = 0.0;
			std::string last_refusal;
		} g_c;

		UINT g_capture_frame = 0xFFFFFFFFu;   // the frame of the last capture
		UINT g_sweep_tick = 0;
		uint32_t g_bake_lines = 0;
		std::unordered_set<uint64_t> g_refusals_logged;
		std::unordered_set<uint32_t> g_residual_logged;

		// ---- Worker ----------------------------------------------------------------

		struct Work
		{
			uint64_t key = 0;
			std::unique_ptr<shader_bake::Job> job;
		};
		struct Done
		{
			uint64_t key = 0;
			std::unique_ptr<shader_bake::Result> result;
		};

		// Shared with the worker; never destroyed (the worker outlives static
		// teardown).
		struct Shared
		{
			std::mutex mu;
			std::condition_variable cv;
			std::deque<Work> queue;
			std::deque<Done> done;
			bool started = false;
			bool start_failed = false;
		};
		Shared& sh()
		{
			static Shared* s = new Shared;
			return *s;
		}

		DWORD WINAPI worker_main(LPVOID)
		{
			auto& s = sh();
			for (;;)
			{
				Work w;
				{
					std::unique_lock lk(s.mu);
					s.cv.wait(lk, [&] { return !s.queue.empty(); });
					w = std::move(s.queue.front());
					s.queue.pop_front();
				}
				auto r = std::make_unique<shader_bake::Result>();
				try
				{
					*r = shader_bake::bake(*w.job);
				}
				catch (const std::bad_alloc&)
				{
					r->ok = false;
					r->refusal = "out of memory";
				}
				catch (...)
				{
					r->ok = false;
					r->refusal = "the bake threw an exception";
				}
				w.job.reset();
				std::lock_guard lk(s.mu);
				s.done.push_back({ w.key, std::move(r) });
			}
		}

		bool start_worker()
		{
			auto& s = sh();
			if (s.started) return true;
			if (s.start_failed) return false;
			const HANDLE t = CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
			if (!t)
			{
				s.start_failed = true;
				shared::common::log("HairBake", "failed to start the bake thread; hair keeps the LayerMap material",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return false;
			}
			SetThreadPriority(t, THREAD_PRIORITY_LOWEST);
			CloseHandle(t);
			s.started = true;
			return true;
		}

		// ---- Logging ---------------------------------------------------------------

		// Logged once per (shader, reason).
		void log_refusal(uint32_t ps_hash, const std::string& why)
		{
			g_c.last_refusal = why;
			Hash64 h;
			h.mix(ps_hash);
			h.add(why.data(), why.size());
			if (g_refusals_logged.size() >= kRefusalLogCap || !g_refusals_logged.insert(h.get()).second) return;
			shared::common::log_event("HairBake", std::format("PS 0x{:08X}: no baked albedo ({}) -- this hair keeps the "
				"LayerMap material{}", ps_hash, why, g_refusals_logged.size() == kRefusalLogCap ? " (last refusal line)" : ""),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		void refusal(uint32_t ps_hash, const std::string& why)
		{
			g_c.refused++;
			log_refusal(ps_hash, why);
		}

		// ---- PS --------------------------------------------------------------------

		const PsInfo& ps_info(uint32_t ps_hash, IDirect3DPixelShader9* ps)
		{
			if (const auto it = g_ps.find(ps_hash); it != g_ps.end()) return it->second;
			PsInfo& pi = g_ps[ps_hash];
			UINT size = 0;
			if (FAILED(ps->GetFunction(nullptr, &size)) || !size) { pi.why = "no bytecode"; return pi; }
			pi.bytecode.resize(size);
			if (FAILED(ps->GetFunction(pi.bytecode.data(), &size))) { pi.why = "no bytecode"; return pi; }
			const ps_interp::Program p = ps_interp::parse(pi.bytecode.data(), pi.bytecode.size());
			if (!p.error.empty()) pi.why = p.error;
			else if (p.flow) pi.why = "flow control (not specialised)";
			else if (!p.find("HairTint")) pi.why = "no HairTint constant";
			for (const auto& e : p.ctab)
			{
				if (e.regset != 2 || e.name == "AmbientColor" || e.name == "MatAlpha" || e.name == "EyePosition" ||
					e.name == "LightData" || e.name == "PSLightColor" || e.name == "ToggleNumLights" || e.name == "HairTint")
					continue;
				for (int r = 0; r < e.count; r++)
				{
					if (e.reg + r >= 32) { pi.why = std::format("{} (c{}) is beyond the tracked constants", e.name, e.reg); break; }
					pi.material_regs.push_back(static_cast<uint8_t>(e.reg + r));
				}
			}
			if (!pi.why.empty())
			{
				refusal(ps_hash, pi.why);
				pi.bytecode.clear();
			}
			return pi;
		}

		// ---- Textures --------------------------------------------------------------

		bool tex_format(D3DFORMAT f, shader_bake::TexFormat& out)
		{
			switch (f)
			{
			case D3DFMT_DXT1:     out = shader_bake::TexFormat::Dxt1; return true;
			case D3DFMT_DXT3:     out = shader_bake::TexFormat::Dxt3; return true;
			case D3DFMT_DXT5:     out = shader_bake::TexFormat::Dxt5; return true;
			case D3DFMT_A8R8G8B8: out = shader_bake::TexFormat::Bgra8; return true;
			case D3DFMT_X8R8G8B8: out = shader_bake::TexFormat::Bgrx8; return true;
			default:              return false;
			}
		}

		// A texture the bake can read: 2D, a CPU copy, a format it decodes.
		IDirect3DTexture9* readable(IDirect3DBaseTexture9* base, D3DSURFACE_DESC& d, shader_bake::TexFormat& f, const char*& why)
		{
			if (base->GetType() != D3DRTYPE_TEXTURE) { why = "not a 2D texture"; return nullptr; }
			auto* t = static_cast<IDirect3DTexture9*>(base);
			if (FAILED(t->GetLevelDesc(0, &d))) { why = "GetLevelDesc failed"; return nullptr; }
			if (d.Pool == D3DPOOL_DEFAULT || (d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)))
			{
				why = "a texture has no CPU copy (D3DPOOL_DEFAULT; hair textures loaded with ShaderBakeHair on are kept MANAGED)";
				return nullptr;
			}
			if (!tex_format(d.Format, f)) { why = "a texture format the bake doesn't decode"; return nullptr; }
			return t;
		}

		// Desc and 64 spots of 16 bytes of level 0, from one READONLY lock.
		bool sample_hash(IDirect3DTexture9* t, const D3DSURFACE_DESC& d, shader_bake::TexFormat f, Hash64& h)
		{
			const size_t bytes = shader_bake::level_bytes(f, d.Width, d.Height);
			const bool block = f == shader_bake::TexFormat::Dxt1 || f == shader_bake::TexFormat::Dxt3 ||
				f == shader_bake::TexFormat::Dxt5;
			const uint32_t rows = block ? (std::max)(1u, (d.Height + 3) / 4) : d.Height;
			const uint32_t row = static_cast<uint32_t>(bytes / rows);
			D3DLOCKED_RECT lr = {};
			if (FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return false;
			if (lr.Pitch < static_cast<INT>(row)) { t->UnlockRect(0); return false; }
			h.mix(d.Width); h.mix(d.Height); h.mix(d.Format); h.mix(t->GetLevelCount());
			const auto* p = static_cast<const uint8_t*>(lr.pBits);
			constexpr uint32_t kSpots = 64, kSpan = 16;
			for (uint32_t k = 0; k < kSpots; k++)
			{
				const uint32_t r = rows > 1 ? (k * (rows - 1)) / (kSpots - 1) : 0u;
				const uint32_t col = row > kSpan ? (k * 2654435761u) % (row - kSpan) : 0u;
				h.add(p + static_cast<size_t>(r) * lr.Pitch + col, (std::min)(kSpan, row));
			}
			t->UnlockRect(0);
			return true;
		}

		// The levels a bake bake_w wide reads, packed without pitch padding.
		bool capture(IDirect3DTexture9* t, const D3DSURFACE_DESC& d, shader_bake::TexFormat f, uint32_t bake_w,
			shader_bake::Texture& out)
		{
			out.format = f;
			out.width = d.Width;
			out.height = d.Height;
			out.levels = (std::max)(1ul, t->GetLevelCount());
			uint32_t first = 0, count = 0;
			shader_bake::levels_needed(d.Width, out.levels, bake_w, first, count);
			const bool block = f == shader_bake::TexFormat::Dxt1 || f == shader_bake::TexFormat::Dxt3 ||
				f == shader_bake::TexFormat::Dxt5;
			for (uint32_t l = first; l < first + count; l++)
			{
				shader_bake::Level lv;
				lv.index = l;
				lv.width = (std::max)(1u, d.Width >> l);
				lv.height = (std::max)(1u, d.Height >> l);
				const size_t bytes = shader_bake::level_bytes(f, lv.width, lv.height);
				const uint32_t rows = block ? (std::max)(1u, (lv.height + 3) / 4) : lv.height;
				const size_t row = bytes / rows;
				D3DLOCKED_RECT lr = {};
				if (FAILED(t->LockRect(l, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return false;
				if (lr.Pitch < static_cast<INT>(row)) { t->UnlockRect(l); return false; }
				lv.bytes.resize(bytes);
				const auto* src = static_cast<const uint8_t*>(lr.pBits);
				if (lr.Pitch == static_cast<INT>(row)) std::memcpy(lv.bytes.data(), src, bytes);
				else
					for (uint32_t r = 0; r < rows; r++) std::memcpy(lv.bytes.data() + r * row, src + static_cast<size_t>(r) * lr.Pitch, row);
				t->UnlockRect(l);
				out.captured.push_back(std::move(lv));
			}
			return true;
		}

		double now_ms()
		{
			LARGE_INTEGER f, c;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&c);
			return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
		}

		// The content key of a new pointer key, and the capture when its bake
		// is new. Returns the content key, or 0 with `why`.
		uint64_t resolve(uint32_t ps_hash, const PsInfo& pi, IDirect3DBaseTexture9* base_tex, IDirect3DBaseTexture9* layer_tex,
			const float* pc, const char*& why)
		{
			D3DSURFACE_DESC bd = {}, ld = {};
			shader_bake::TexFormat bf = shader_bake::TexFormat::Bgra8, lf = shader_bake::TexFormat::Bgra8;
			IDirect3DTexture9* base = readable(base_tex, bd, bf, why);
			IDirect3DTexture9* layer = base ? readable(layer_tex, ld, lf, why) : nullptr;
			if (!base || !layer) return 0;
			Hash64 h;
			h.mix(ps_hash);
			for (const uint8_t r : pi.material_regs) h.add(pc + r * 4, 16);
			if (!sample_hash(base, bd, bf, h) || !sample_hash(layer, ld, lf, h)) { why = "texture lock failed"; return 0; }
			return h.get();
		}

		bool capture_job(uint32_t ps_hash, const PsInfo& pi, IDirect3DBaseTexture9* base_tex,
			IDirect3DBaseTexture9* layer_tex, const float* pc, std::unique_ptr<shader_bake::Job>& job, const char*& why)
		{
			D3DSURFACE_DESC bd = {}, ld = {};
			shader_bake::TexFormat bf = shader_bake::TexFormat::Bgra8, lf = shader_bake::TexFormat::Bgra8;
			IDirect3DTexture9* base = readable(base_tex, bd, bf, why);
			IDirect3DTexture9* layer = base ? readable(layer_tex, ld, lf, why) : nullptr;
			if (!base || !layer) return false;
			job = std::make_unique<shader_bake::Job>();
			job->ps_hash = ps_hash;
			job->bytecode = pi.bytecode;
			std::memcpy(job->constants, pc, sizeof(job->constants));
			shader_bake::bake_size(bd.Width, bd.Height, job->width, job->height);
			if (!capture(base, bd, bf, job->width, job->base) || !capture(layer, ld, lf, job->width, job->layer))
			{
				why = "texture lock failed";
				job.reset();
				return false;
			}
			return true;
		}

		void finish(Done& d)
		{
			const auto it = g_bakes.find(d.key);
			if (it == g_bakes.end()) return;
			Bake& b = it->second;
			const shader_bake::Result& r = *d.result;
			g_c.baked++;
			g_c.worker_ms += r.ms;
			g_c.worker_max = (std::max)(g_c.worker_max, r.ms);
			if (!r.ok)
			{
				b.state = State::Refused;
				g_pending--;
				refusal(b.ps_hash, r.refusal);
				return;
			}
			g_c.done++;
			if (g_bake_lines < kBakeLogCap)
			{
				g_bake_lines++;
				shared::common::log_event("HairBake", std::format("PS 0x{:08X}: baked {}x{} ({} mips, {}) in {:.0f} ms on the bake "
					"thread: {} of {} instructions live, {} per texel; probes: emissive {:.2g}, linearity {:.2g}, vertex colour "
					"{:.2g}, specialisation {:.2g}, alpha {:.2g}{}", b.ps_hash, r.width, r.height, r.mips, r.bc3 ? "BC3" : "BGRA8",
					r.ms, r.live, r.instructions, r.varying, r.emissive, r.linearity, r.separability, r.plan_error, r.alpha_error,
					g_bake_lines == kBakeLogCap ? " (last bake line)" : ""), shared::common::LOG_TYPE::LOG_TYPE_GREEN);
			}
			if (g_residual_logged.insert(b.ps_hash).second)
				shared::common::log_event("HairBake", std::format("PS 0x{:08X}: albedo program: {}", b.ps_hash, r.residual),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
			b.state = State::Ready;
			b.result = std::move(d.result);
		}

		void upload_one()
		{
			if (textures::upload_budget_spent()) return;
			for (auto& [key, b] : g_bakes)
			{
				if (b.state != State::Ready) continue;
				const shader_bake::Result& r = *b.result;
				const char* reject = nullptr;
				const uint64_t hash = textures::acquire_generated(r.width, r.height, r.mips, r.data.data(), r.data.size(),
					r.bc3 ? REMIXAPI_FORMAT_BC3_UNORM : REMIXAPI_FORMAT_B8G8R8A8_UNORM, reject);
				if (!hash)
				{
					// Over this frame's hard cap: the next frame tries again.
					if (reject && std::strstr(reject, "budget")) return;
					g_c.failed++;
					b.state = State::Refused;
					b.result.reset();
					g_pending--;
					log_refusal(b.ps_hash, std::format("upload failed: {}", reject ? reject : "?"));
					return;
				}
				g_c.uploads++;
				b.state = State::Uploaded;
				b.hash = hash;
				b.result.reset();
				g_pending--;
				return;
			}
		}

		void evict(UINT now)
		{
			const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.texture_evict_frames);
			for (auto it = g_bakes.begin(); it != g_bakes.end();)
			{
				Bake& b = it->second;
				const bool idle = b.state == State::Uploaded || b.state == State::Refused;
				if (idle && now - b.last_used > keep)
				{
					if (b.hash) textures::release(b.hash);
					it = g_bakes.erase(it);
				}
				else ++it;
			}
			std::erase_if(g_draws, [&](const auto& e) { return now - e.second.last_seen > keep; });
		}
	}

	uint64_t albedo(const shared::common::PsSlotMap* psmap, IDirect3DPixelShader9* ps, IDirect3DBaseTexture9* base,
		IDirect3DBaseTexture9* layer)
	{
		if (!shared::common::config::get().remix_api.shader_bake_hair || !psmap || !ps || !base || !layer) return 0;
		const PsInfo& pi = ps_info(psmap->ps_hash, ps);
		if (!pi.why.empty()) return 0;
		const UINT now = frame();
		const float* pc = shared::common::ffp_state::get().ps_const_data();

		Hash64 dk;
		dk.mix(psmap->ps_hash);
		dk.ptr(base);
		dk.mix(identity::texture_gen(base));
		dk.ptr(layer);
		dk.mix(identity::texture_gen(layer));
		for (const uint8_t r : pi.material_regs) dk.add(pc + r * 4, 16);
		const uint64_t draw_key = dk.get();

		uint64_t content = 0;
		if (const auto it = g_draws.find(draw_key); it != g_draws.end())
		{
			it->second.last_seen = now;
			content = it->second.content;
			if (!content) return 0;
		}
		else
		{
			if (g_draws.size() >= kMaxDrawKeys) return 0;
			const double t0 = now_ms();
			const char* why = nullptr;
			content = resolve(psmap->ps_hash, pi, base, layer, pc, why);
			const double dt = now_ms() - t0;
			g_c.keys++;
			g_c.key_ms += dt;
			g_c.key_max = (std::max)(g_c.key_max, dt);
			g_draws[draw_key] = { content, now };
			if (!content)
			{
				refusal(psmap->ps_hash, why ? why : "?");
				return 0;
			}
			if (g_bakes.contains(content)) g_c.content_hits++;
		}

		if (const auto it = g_bakes.find(content); it != g_bakes.end())
		{
			it->second.last_used = now;
			if (it->second.state != State::Uploaded) return 0;
			g_c.served++;
			return it->second.hash;
		}

		// A new bake: one capture per frame, a bounded queue.
		if (g_pending >= kMaxPending || g_capture_frame == now || g_bakes.size() >= kMaxBakes || !start_worker()) return 0;
		g_capture_frame = now;
		const double t0 = now_ms();
		std::unique_ptr<shader_bake::Job> job;
		const char* why = nullptr;
		const bool ok = capture_job(psmap->ps_hash, pi, base, layer, pc, job, why);
		const double dt = now_ms() - t0;
		g_c.captures++;
		g_c.capture_ms += dt;
		g_c.capture_max = (std::max)(g_c.capture_max, dt);
		Bake& b = g_bakes[content];
		b.ps_hash = psmap->ps_hash;
		b.last_used = now;
		if (!ok)
		{
			b.state = State::Refused;
			refusal(psmap->ps_hash, why ? why : "?");
			return 0;
		}
		b.state = State::Queued;
		g_pending++;
		g_c.queued++;
		auto& s = sh();
		{
			std::lock_guard lk(s.mu);
			s.queue.push_back({ content, std::move(job) });
		}
		s.cv.notify_one();
		return 0;
	}

	void on_present()
	{
		auto& s = sh();
		if (!s.started) return;
		std::deque<Done> done;
		{
			std::lock_guard lk(s.mu);
			done.swap(s.done);
		}
		for (Done& d : done) finish(d);
		upload_one();
		if (++g_sweep_tick < 60) return;
		g_sweep_tick = 0;
		evict(frame());
	}

	std::string status()
	{
		if (!g_c.keys) return {};
		size_t live = 0;
		for (const auto& [key, b] : g_bakes)
			if (b.state == State::Uploaded) live++;
		auto avg = [](double sum, uint64_t n) { return n ? sum / static_cast<double>(n) : 0.0; };
		return std::format("hair bake: queued={} done={} refused={} failed={} pending={} uploads={} live={} served={} "
			"contentHits={} | bake thread avg {:.0f} ms max {:.0f} ms | render thread: keys avg {:.3f} ms max {:.3f} ms, "
			"captures avg {:.3f} ms max {:.3f} ms{}",
			g_c.queued, g_c.done, g_c.refused, g_c.failed, g_pending, g_c.uploads, live, g_c.served, g_c.content_hits,
			avg(g_c.worker_ms, g_c.baked), g_c.worker_max, avg(g_c.key_ms, g_c.keys), g_c.key_max,
			avg(g_c.capture_ms, g_c.captures), g_c.capture_max,
			g_c.last_refusal.empty() ? std::string() : std::format(" [last refusal: {}]", g_c.last_refusal));
	}
}
