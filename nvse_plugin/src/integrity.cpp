#include "common.hpp"
#include "integrity.hpp"
#include "config.hpp"
#include "monitor.hpp"
#include "shared/hook_forms.hpp"

namespace fnvr::integrity
{
	namespace
	{
		// What a site may hold.
		//   Xnvse:   an E8 / E9 into nvse_1_4.dll (xNVSE's event hook);
		//   Wrapper: vanilla (not installed yet), `alt` exactly, the wrapper's
		//            hook (FnvRemix_HookChain names it and what it chained
		//            after), or another module's hook into executable code the
		//            wrapper chains with: a detour at a function entry, an E8
		//            call at a call site (`call`);
		//   Vanilla: the executable's own bytes.
		// The address and the vanilla bytes come from the address table
		// (shared/fnv_exe.hpp); the first `len` of them are judged.
		enum class Kind : uint8_t { Xnvse, Wrapper, Vanilla };

		using Field = fnv_exe::Code fnv_exe::Addresses::*;

		struct Site
		{
			Field code;
			uint8_t len;
			uint32_t feature;
			Kind kind;
			const char* what;
			uint8_t alt[2];
			bool has_alt;
			bool call;
		};

		constexpr Site kSites[] = {
			// xNVSE 6.4.8's event hooks.
			{ &fnv_exe::Addresses::xnvse_cell_state, 5, FNVR_F_CELLS, Kind::Xnvse, "cell SetState (OnCellStateChange)", {}, false, false },
			{ &fnv_exe::Addresses::xnvse_refs_loaded, 5, FNVR_F_CELLS, Kind::Xnvse, "all refs loaded (OnCellRefsLoaded)", {}, false, false },
			{ &fnv_exe::Addresses::xnvse_set_3d, 5, FNVR_F_OWNERS, Kind::Xnvse, "ref Set3D (OnRefSet3D)", {}, false, false },
			{ &fnv_exe::Addresses::xnvse_unset_3d, 5, FNVR_F_OWNERS, Kind::Xnvse, "ref Unset3D (OnRefUnset3D)", {}, false, false },
			{ &fnv_exe::Addresses::xnvse_ref_attach, 5, FNVR_F_OWNERS, Kind::Xnvse, "ref attach (OnRefAttach)", {}, false, false },
			{ &fnv_exe::Addresses::xnvse_main_present, 5, FNVR_F_LOADING, Kind::Xnvse, "main present (OnFramePresent 0)", {}, false, false },
			{ &fnv_exe::Addresses::xnvse_loading_present, 5, FNVR_F_LOADING, Kind::Xnvse, "loading present (OnFramePresent 1)", {}, false, false },
			{ &fnv_exe::Addresses::xnvse_main_loop, 5, FNVR_F_LOADING, Kind::Xnvse, "main loop (MainGameLoop)", {}, false, false },
			// The wrapper's hooks.
			{ &fnv_exe::Addresses::render_pass_dispatch, 5, FNVR_F_HOOK_P1, Kind::Wrapper, "P1 render-pass dispatcher", {}, false, false },
			{ &fnv_exe::Addresses::ni_geometry_dtor, 5, FNVR_F_HOOK_P2, Kind::Wrapper, "P2 ~NiGeometry", {}, false, false },
			{ &fnv_exe::Addresses::grass_batch_append, 5, FNVR_F_HOOK_P3, Kind::Wrapper, "P3 grass batch append", {}, false, false },
			{ &fnv_exe::Addresses::grass_batch_remove_at, 5, FNVR_F_HOOK_P3, Kind::Wrapper, "P3 grass batch remove", {}, false, false },
			{ &fnv_exe::Addresses::grass_batch_destroy, 5, FNVR_F_HOOK_P3, Kind::Wrapper, "P3 grass batch destroy", {}, false, false },
			{ &fnv_exe::Addresses::skin_path_jz, 2, FNVR_F_HOOK_P5, Kind::Wrapper, "P5 skinned-path jz", { 0xEB, 0x58 }, true, false },
			{ &fnv_exe::Addresses::skin_render_call, 5, FNVR_F_HOOK_P5, Kind::Wrapper, "P5 skinned batch call", {}, false, true },
			{ &fnv_exe::Addresses::skin_reset_call, 5, FNVR_F_HOOK_P5, Kind::Wrapper, "P5 bone setup call", {}, false, true },
			{ &fnv_exe::Addresses::set_d3d_texture, 5, 0, Kind::Wrapper, "P4 SetD3DTexture (texture packs)", {}, false, false },
			{ &fnv_exe::Addresses::ui_boundary_call, 5, 0, Kind::Wrapper, "UI boundary (HUD 3D render call)", {}, false, true },
			{ &fnv_exe::Addresses::world_scene_call_a, 5, 0, Kind::Wrapper, "UI boundary (RenderWorldScene call, gameplay)", {}, false, true },
			{ &fnv_exe::Addresses::world_scene_call_c, 5, 0, Kind::Wrapper, "UI boundary (RenderWorldScene call, Pip-Boy)", {}, false, true },
			{ &fnv_exe::Addresses::pipboy_screen_call, 5, 0, Kind::Wrapper, "menu background capture guard (Pip-Boy screen render call)", {}, false, true },
			{ &fnv_exe::Addresses::texture_from_file, 5, 0, Kind::Wrapper, "texture file load (FaceGen maps kept MANAGED)", {}, false, false },
			{ &fnv_exe::Addresses::vats_limb_visibility, 5, 0, Kind::Wrapper, "VATS limb visibility (line-of-sight estimate)", {}, false, false },
			// The decal path (decals.cpp writes a decal flag only while these are vanilla).
			{ &fnv_exe::Addresses::decal_pass_builder, 5, FNVR_F_DECALS, Kind::Vanilla, "decal pass builder", {}, false, false },
			{ &fnv_exe::Addresses::accum_draw_loop, 5, FNVR_F_DECALS, Kind::Vanilla, "accumulator draw loop", {}, false, false },
			{ &fnv_exe::Addresses::accum_decal_test, 5, FNVR_F_DECALS, Kind::Vanilla, "accumulator decal test", {}, false, false },
			{ &fnv_exe::Addresses::zwrite_state, 5, FNVR_F_DECALS, Kind::Vanilla, "ZWRITE render-state wrapper", {}, false, false },
			{ &fnv_exe::Addresses::depthbias_state, 5, FNVR_F_DECALS, Kind::Vanilla, "DEPTHBIAS render-state wrapper", {}, false, false },
			// The menu-background capture the wrapper re-runs every menu frame.
			{ &fnv_exe::Addresses::menu_capture_call, 5, FNVR_F_MENU_BG, Kind::Vanilla, "menu capture call", {}, false, false },
			{ &fnv_exe::Addresses::pipboy_request_test, 5, FNVR_F_MENU_BG, Kind::Vanilla, "Pip-Boy capture request test", {}, false, false },
			{ &fnv_exe::Addresses::pipboy_request_clear, 5, FNVR_F_MENU_BG, Kind::Vanilla, "Pip-Boy capture request clear", {}, false, false },
			{ &fnv_exe::Addresses::pipboy_capture_call, 5, FNVR_F_MENU_BG, Kind::Vanilla, "Pip-Boy capture call", {}, false, false },
			{ &fnv_exe::Addresses::live_bg_test, 5, FNVR_F_MENU_BG, Kind::Vanilla, "live / frozen background test", {}, false, false },
			{ &fnv_exe::Addresses::bg_ready_mark, 5, FNVR_F_MENU_BG, Kind::Vanilla, "capture marks background ready", {}, false, false },
		};
		constexpr size_t kSiteCount = std::size(kSites);

		uintptr_t site_addr(size_t i)
		{
			return (fnv_exe::addr().*kSites[i].code).addr;
		}

		struct SiteState
		{
			uint8_t bytes[8] = {};
			bool read = false;
			bool ok = false;
			std::string verdict;
		};
		SiteState g_state[kSiteCount];
		uint32_t g_ok_bits = 0;
		uint32_t g_last_check = 0;
		std::string g_status[32];
		bool g_reported = false;

		std::string hex(const uint8_t* p, size_t n)
		{
			std::string s;
			for (size_t i = 0; i < n; i++) s += std::format("{}{:02X}", i ? " " : "", p[i]);
			return s;
		}

		std::string module_name(HMODULE m)
		{
			char path[MAX_PATH] = {};
			const DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
			std::string p(path, n);
			const size_t slash = p.find_last_of("\\/");
			return slash == std::string::npos ? p : p.substr(slash + 1);
		}

		std::string lower(std::string s)
		{
			for (auto& c : s)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			return s;
		}

		// Where a jump at `at` (E8 / E9 rel32, or FF 25 [abs]) lands; 0 for
		// anything else.
		uintptr_t jump_target(uintptr_t at)
		{
			uint8_t op[6] = {};
			if (!guard::read(at, op, sizeof(op))) return 0;
			if (op[0] == 0xE8 || op[0] == 0xE9)
			{
				int32_t rel = 0;
				std::memcpy(&rel, op + 1, 4);
				return at + 5 + static_cast<uintptr_t>(rel);
			}
			if (op[0] == 0xFF && op[1] == 0x25)
			{
				uint32_t slot = 0, dst = 0;
				std::memcpy(&slot, op + 2, 4);
				if (guard::get(slot, dst)) return dst;
			}
			return 0;
		}

		// Follows jump stubs (MinHook-style relays, trampolines) to the code
		// that runs, at most four hops.
		uintptr_t follow(uintptr_t target)
		{
			for (int hop = 0; hop < 4 && target; hop++)
			{
				uint8_t op[2] = {};
				if (!guard::read(target, op, sizeof(op))) break;
				if (op[0] != 0xE9 && !(op[0] == 0xFF && op[1] == 0x25)) break;
				const uintptr_t next = jump_target(target);
				if (!next) break;
				target = next;
			}
			return target;
		}

		// The owner of an address: its module's file name, "private memory",
		// or "unmapped".
		std::string owner_of(uintptr_t addr, HMODULE* out_module = nullptr)
		{
			HMODULE m = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(addr), &m) && m)
			{
				if (out_module) *out_module = m;
				return module_name(m);
			}
			if (out_module) *out_module = nullptr;
			MEMORY_BASIC_INFORMATION mbi;
			if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
				return "private memory";
			return "unmapped";
		}

		bool executable(uintptr_t addr)
		{
			MEMORY_BASIC_INFORMATION mbi = {};
			if (!addr || !VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi))) return false;
			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
			return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		}

		bool read_u32(uintptr_t addr, uint32_t& out)
		{
			return guard::get(addr, out);
		}

		// The wrapper's FnvRemix_HookChain, found in whichever loaded module
		// exports it; null when none does.
		PFN_FnvRemix_HookChain hook_chain()
		{
			static PFN_FnvRemix_HookChain fn = nullptr;
			if (!fn) fn = reinterpret_cast<PFN_FnvRemix_HookChain>(monitor::wrapper_export("FnvRemix_HookChain"));
			return fn;
		}

		// The wrapper's record of its hook at `site`; false when it has none.
		bool wrapper_hook(uintptr_t site, FnvRemixHook& out)
		{
			const PFN_FnvRemix_HookChain fn = hook_chain();
			if (!fn) return false;
			FnvRemixHook recs[64];
			const uint32_t n = (std::min)(fn(recs, static_cast<uint32_t>(std::size(recs))), static_cast<uint32_t>(std::size(recs)));
			for (uint32_t i = 0; i < n; i++)
				if (recs[i].site == site)
				{
					out = recs[i];
					return true;
				}
			return false;
		}

		// One site's verdict from its bytes now.
		void judge(size_t i)
		{
			const Site& s = kSites[i];
			const fnv_exe::Code& c = fnv_exe::addr().*s.code;
			SiteState& st = g_state[i];
			std::memset(st.bytes, 0, sizeof(st.bytes));
			st.read = c.addr && guard::read(c.addr, st.bytes, s.len);
			// The longer hook forms (FF 25, push/ret, mov/jmp) need up to 7 bytes.
			if (st.read && s.len < sizeof(st.bytes)) guard::read(c.addr + s.len, st.bytes + s.len, sizeof(st.bytes) - s.len);
			if (!st.read)
			{
				st.ok = false;
				st.verdict = "unreadable";
				return;
			}
			const bool vanilla = std::memcmp(st.bytes, c.bytes, s.len) == 0;
			const uintptr_t target = (st.bytes[0] == 0xE8 || st.bytes[0] == 0xE9) && s.len == 5 ? jump_target(c.addr) : 0;
			const uintptr_t final_target = target ? follow(target) : 0;
			const std::string owner = final_target ? owner_of(final_target) : std::string();
			switch (s.kind)
			{
			case Kind::Xnvse:
				st.ok = final_target && lower(owner) == "nvse_1_4.dll";
				st.verdict = st.ok ? "xNVSE" : vanilla ? "vanilla: xNVSE's hook is missing"
					: final_target ? std::format("foreign: jumps into {} (0x{:X})", owner, final_target)
					: std::format("foreign: [{}]", hex(st.bytes, s.len));
				break;
			case Kind::Wrapper:
			{
				const hook_forms::Decoded d = hook_forms::decode(st.bytes, sizeof(st.bytes), c.addr, &read_u32);
				const bool hook_form = s.call ? d.form == hook_forms::Form::CallRel32 : hook_forms::is_detour(d.form);
				const uintptr_t lead = hook_form ? d.target : 0;
				const uintptr_t lead_final = lead ? follow(lead) : 0;
				const std::string lead_owner = lead_final ? owner_of(lead_final) : std::string();
				FnvRemixHook rec = {};
				const bool installed = wrapper_hook(c.addr, rec);
				if (vanilla) { st.ok = true; st.verdict = "vanilla (not installed)"; }
				else if (s.has_alt && std::memcmp(st.bytes, s.alt, s.len) == 0) { st.ok = true; st.verdict = "ours"; }
				else if (installed && lead && (lead == rec.ours || lead_final == rec.ours))
				{
					st.ok = true;
					st.verdict = rec.chained ? std::format("ours, chained after {} (0x{:X})", rec.previous_owner, rec.previous)
						: std::string("ours");
				}
				else if (!installed && lead_final && lower(lead_owner) == "d3d9.dll") { st.ok = true; st.verdict = "ours (d3d9.dll)"; }
				else if (lead && executable(lead))
				{
					// Another module's hook: patched after the wrapper's, or
					// there first, and the wrapper chains after it when it hooks.
					st.ok = true;
					st.verdict = installed
						? std::format("chained: {} (0x{:X}) patched it after the wrapper{}", lead_owner, lead_final,
							rec.chained ? std::format(", which is chained after {}", rec.previous_owner) : std::string())
						: std::format("another mod's hook: {} (0x{:X}); the wrapper chains after it", lead_owner, lead_final);
				}
				else
				{
					st.ok = false;
					st.verdict = final_target ? std::format("foreign: jumps into {} (0x{:X})", owner, final_target)
						: std::format("foreign: [{}]", hex(st.bytes, s.len));
				}
				break;
			}
			case Kind::Vanilla:
				st.ok = vanilla;
				st.verdict = vanilla ? "vanilla" : final_target ? std::format("foreign: jumps into {} (0x{:X})", owner, final_target)
					: std::format("foreign: [{}]", hex(st.bytes, s.len));
				break;
			}
		}

		void recompute_bits()
		{
			uint32_t bad = 0, seen = 0;
			for (size_t i = 0; i < kSiteCount; i++)
			{
				if (!kSites[i].feature) continue;
				seen |= kSites[i].feature;
				if (!g_state[i].ok) bad |= kSites[i].feature;
			}
			g_ok_bits = seen & ~bad;
			for (uint32_t b = 0; b < 32; b++)
			{
				const uint32_t bit = 1u << b;
				if (!(seen & bit)) continue;
				std::string line;
				for (size_t i = 0; i < kSiteCount; i++)
					if (kSites[i].feature == bit && !g_state[i].ok)
						line += std::format("{}0x{:X} {}: {}", line.empty() ? "" : "; ", site_addr(i), kSites[i].what,
							g_state[i].verdict);
				g_status[b] = line.empty() ? std::string("ok") : "OFF: " + line;
			}
		}

		const char* feature_name(uint32_t bit)
		{
			switch (bit)
			{
			case FNVR_F_CELLS: return "cells";
			case FNVR_F_LOADING: return "loading";
			case FNVR_F_OWNERS: return "owners";
			case FNVR_F_DECALS: return "decals";
			case FNVR_F_MENU_BG: return "menu background";
			case FNVR_F_HOOK_P1: return "P1";
			case FNVR_F_HOOK_P2: return "P2";
			case FNVR_F_HOOK_P3: return "P3";
			case FNVR_F_HOOK_P5: return "P5";
			default: return "report only";
			}
		}

		// The whole .text against FalloutNV.exe on disk. Not on a SteamStub-
		// wrapped executable: its .text on disk is encrypted, so there is no
		// plain reference; the site table's vanilla bytes are the reference
		// there.
		void text_diff()
		{
			if (fnv_exe::identity().steamstub)
			{
				log::e("Integrity", "{} is SteamStub-wrapped: .text on disk is encrypted, so no .text diff; the sites "
					"below are judged against the address table's bytes", fnv_exe::identity().variant->name);
				return;
			}
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
			const IMAGE_SECTION_HEADER* text = nullptr;
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
				if (std::memcmp(sec[i].Name, ".text", 5) == 0) text = &sec[i];
			if (!text)
			{
				log::e("Integrity", "no .text section in the running image: no diff");
				return;
			}
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE)
			{
				log::e("Integrity", "FalloutNV.exe could not be opened (error {}): no .text diff", GetLastError());
				return;
			}
			LARGE_INTEGER size = {};
			GetFileSizeEx(f, &size);
			std::vector<uint8_t> file(static_cast<size_t>(size.QuadPart));
			DWORD got = 0;
			const bool ok = ReadFile(f, file.data(), static_cast<DWORD>(file.size()), &got, nullptr) && got == file.size();
			CloseHandle(f);
			if (!ok)
			{
				log::e("Integrity", "FalloutNV.exe could not be read: no .text diff");
				return;
			}
			uint64_t h = 0xCBF29CE484222325ull;
			for (const uint8_t b : file) h = (h ^ b) * 0x100000001B3ull;
			log::e("Integrity", "FalloutNV.exe on disk: {} bytes, FNV-1a 0x{:016X}, image base 0x{:X}", file.size(), h, base);

			const size_t len = (std::min)(static_cast<size_t>(text->Misc.VirtualSize), static_cast<size_t>(text->SizeOfRawData));
			if (text->PointerToRawData + len > file.size())
			{
				log::e("Integrity", ".text extends past the file: no diff");
				return;
			}
			const uint8_t* disk = file.data() + text->PointerToRawData;
			const uintptr_t mem = base + text->VirtualAddress;

			struct Range { uintptr_t start; size_t len; };
			std::vector<Range> ranges;
			size_t diff_bytes = 0;
			constexpr size_t kChunk = 0x10000;
			constexpr size_t kGap = 8;
			std::vector<uint8_t> buf(kChunk);
			for (size_t off = 0; off < len; off += kChunk)
			{
				const size_t n = (std::min)(kChunk, len - off);
				if (!guard::read(mem + off, buf.data(), n)) continue;
				for (size_t k = 0; k < n; k++)
				{
					if (buf[k] == disk[off + k]) continue;
					diff_bytes++;
					const uintptr_t a = mem + off + k;
					if (!ranges.empty() && a <= ranges.back().start + ranges.back().len + kGap)
						ranges.back().len = a - ranges.back().start + 1;
					else
						ranges.push_back({ a, 1 });
				}
			}
			if (diff_bytes > len / 10)
			{
				log::e("Integrity", ".text differs from the file in {} of {} bytes: the file on disk is not the running "
					"code (SteamStub-protected or another build) -- no range list", diff_bytes, len);
				return;
			}

			std::unordered_map<std::string, std::pair<uint32_t, size_t>> by_owner;
			std::vector<std::string> lines;
			for (const Range& r : ranges)
			{
				uintptr_t target = 0, at = 0;
				for (uintptr_t p = r.start >= 2 ? r.start - 2 : r.start; p <= r.start && !target; p++)
					if (const uintptr_t t = jump_target(p); t)
					{
						uint8_t op = 0;
						guard::get(p, op);
						// Only an E8 / E9 / FF 25 that the changed bytes belong to.
						if (op == 0xE8 || op == 0xE9 || op == 0xFF)
						{
							target = t;
							at = p;
						}
					}
				const uintptr_t fin = target ? follow(target) : 0;
				const std::string owner = fin ? owner_of(fin) : std::string("(no jump)");
				auto& o = by_owner[owner];
				o.first++;
				o.second += r.len;
				if (lines.size() < config::get().text_diff_ranges)
					lines.push_back(fin ? std::format("  0x{:X} +{} -> {} 0x{:X}{}", r.start, r.len, owner, fin,
						at != r.start ? std::format(" (jump at 0x{:X})", at) : std::string())
						: std::format("  0x{:X} +{} {}", r.start, r.len, owner));
			}
			std::string summary;
			for (const auto& [owner, c] : by_owner)
				summary += std::format("{}{}: {} ranges / {} bytes", summary.empty() ? "" : ", ", owner, c.first, c.second);
			log::e("Integrity", ".text vs disk: {} ranges, {} bytes changed -- {}", ranges.size(), diff_bytes,
				summary.empty() ? std::string("none") : summary);
			for (const auto& l : lines) log::line("Integrity", l);
			if (lines.size() < ranges.size())
				log::e("Integrity", "  ... {} more ranges ([Report] TextDiffRanges)", ranges.size() - lines.size());
		}
	}

	void report()
	{
		text_diff();
		for (size_t i = 0; i < kSiteCount; i++)
		{
			judge(i);
			log::e("Integrity", "site 0x{:X} {} [{}] ({}): {}", site_addr(i), kSites[i].what,
				hex(g_state[i].bytes, kSites[i].len), feature_name(kSites[i].feature), g_state[i].verdict);
		}
		recompute_bits();
		for (uint32_t b = 0; b < 32; b++)
			if (!g_status[b].empty())
				log::e("Integrity", "feature {}: {}", feature_name(1u << b), g_status[b]);
		g_reported = true;
	}

	void tick(uint32_t frame)
	{
		if (!g_reported || frame - g_last_check < config::get().monitor_period) return;
		g_last_check = frame;
		const uint32_t before = g_ok_bits;
		for (size_t i = 0; i < kSiteCount; i++)
		{
			uint8_t now[sizeof(SiteState::bytes)] = {};
			const bool read = site_addr(i) && guard::read(site_addr(i), now, kSites[i].len);
			if (read == g_state[i].read && (!read || std::memcmp(now, g_state[i].bytes, kSites[i].len) == 0)) continue;
			const std::string old_bytes = hex(g_state[i].bytes, kSites[i].len);
			const std::string old_verdict = g_state[i].verdict;
			judge(i);
			log::e("Integrity", "site 0x{:X} {} changed: [{}] -> [{}], {} -> {}", site_addr(i), kSites[i].what,
				old_bytes, hex(g_state[i].bytes, kSites[i].len), old_verdict, g_state[i].verdict);
		}
		recompute_bits();
		if (g_ok_bits != before)
			for (uint32_t b = 0; b < 32; b++)
				if ((before ^ g_ok_bits) & (1u << b))
					log::e("Integrity", "feature {} is now {}", feature_name(1u << b), g_status[b]);
	}

	bool reported()
	{
		return g_reported;
	}

	uint32_t features_ok()
	{
		return g_ok_bits;
	}

	const char* status(uint32_t feature_bit)
	{
		for (uint32_t b = 0; b < 32; b++)
			if (feature_bit == (1u << b)) return g_status[b].c_str();
		return "";
	}
}
