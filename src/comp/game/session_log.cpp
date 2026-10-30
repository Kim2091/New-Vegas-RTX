#include "std_include.hpp"
#include "session_log.hpp"

#include <algorithm>
#include <cwctype>
// The DirectX SDK's dxgi.h repeats the Windows SDK's DXGI status codes.
#pragma warning(push)
#pragma warning(disable: 4005)
#include <dxgi.h>
#pragma warning(pop)
#include <tlhelp32.h>

#include "api_mesh.hpp"
#include "api_textures.hpp"
#include "crash_log.hpp"
#include "fault_report.hpp"
#include "frame_stats.hpp"
#include "game.hpp"
#include "nvse_link.hpp"
#include "sampler.hpp"
#include "tex_pool.hpp"
#include "weather.hpp"
#include "shared/common/config.hpp"

namespace comp::game::session_log
{
	namespace
	{
		constexpr double kStatusSeconds = 60.0;
		// DXGI_ADAPTER_FLAG_SOFTWARE (DXGI 1.2; the DirectX SDK headers predate it).
		constexpr UINT kAdapterFlagSoftware = 2;

		bool g_started = false;
		LARGE_INTEGER g_freq = {};
		LARGE_INTEGER g_last_frame = {};
		LARGE_INTEGER g_last_status = {};
		uint32_t g_frames = 0;
		double g_frame_ms_sum = 0.0;
		double g_frame_ms_worst = 0.0;
		uint64_t g_warnings = 0;
		uint64_t g_errors = 0;
		uint64_t g_held_back = 0;
		uint16_t g_top_menu = 0;

		LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
		volatile LONG g_crash_written = 0;

		double seconds_between(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
		{
			return static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(g_freq.QuadPart);
		}

		// The module that holds `address` and the offset into it, without
		// allocating (the crash filter calls it).
		void module_of(const void* address, char (&name)[MAX_PATH], uintptr_t& offset)
		{
			HMODULE mod = nullptr;
			name[0] = '\0';
			offset = reinterpret_cast<uintptr_t>(address);
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCSTR>(address), &mod) && mod)
			{
				char path[MAX_PATH] = {};
				GetModuleFileNameA(mod, path, MAX_PATH);
				const char* slash = strrchr(path, '\\');
				strcpy_s(name, slash ? slash + 1 : path);
				offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(mod);
			}
			else
			{
				strcpy_s(name, "?");
			}
		}

		// The process's virtual address space: used and total, and the largest
		// free block, in MiB, and the used part by region type (reserved and
		// committed): module images, mapped views (the bridge's shared-memory
		// channels among them) and private memory (every heap). Allocates
		// nothing.
		struct AddressSpace
		{
			uint32_t used_mib = 0;
			uint32_t total_mib = 0;
			uint32_t largest_free_mib = 0;
			uint32_t image_mib = 0;
			uint32_t mapped_mib = 0;
			uint32_t private_mib = 0;
		};

		AddressSpace address_space()
		{
			AddressSpace a;
			MEMORYSTATUSEX ms = {};
			ms.dwLength = sizeof(ms);
			if (!GlobalMemoryStatusEx(&ms)) return a;
			a.total_mib = static_cast<uint32_t>(ms.ullTotalVirtual >> 20);
			a.used_mib = static_cast<uint32_t>((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20);
			SYSTEM_INFO si = {};
			GetSystemInfo(&si);
			uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
			const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
			size_t largest = 0;
			uint64_t image = 0, mapped = 0, priv = 0;
			MEMORY_BASIC_INFORMATION mbi = {};
			while (p < end && VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
			{
				if (mbi.State == MEM_FREE) largest = (std::max)(largest, static_cast<size_t>(mbi.RegionSize));
				else if (mbi.Type == MEM_IMAGE) image += mbi.RegionSize;
				else if (mbi.Type == MEM_MAPPED) mapped += mbi.RegionSize;
				else if (mbi.Type == MEM_PRIVATE) priv += mbi.RegionSize;
				const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				if (next <= p) break;
				p = next;
			}
			a.largest_free_mib = static_cast<uint32_t>(largest >> 20);
			a.image_mib = static_cast<uint32_t>(image >> 20);
			a.mapped_mib = static_cast<uint32_t>(mapped >> 20);
			a.private_mib = static_cast<uint32_t>(priv >> 20);
			return a;
		}

		// True when `address` is in executable code of a loaded module.
		bool is_module_code(uintptr_t address)
		{
			MEMORY_BASIC_INFORMATION mbi = {};
			if (address < 0x10000 || !VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) return false;
			constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE && (mbi.Protect & kExec);
		}

		// Appends " module+0xOFFSET" to `out` (no allocation).
		void append_location(char* out, size_t out_n, uintptr_t address)
		{
			char mod[MAX_PATH];
			uintptr_t offset = 0;
			module_of(reinterpret_cast<const void*>(address), mod, offset);
			const size_t used = strlen(out);
			if (used + 1 < out_n)
				_snprintf_s(out + used, out_n - used, _TRUNCATE, " %s+0x%IX", mod, offset);
		}

		// The return addresses along the EBP chain, then code addresses found
		// on the stack (a raw scan, so some are stale). Both start at the
		// faulting context.
		void describe_stack_unguarded(const CONTEXT* ctx, char* chain, size_t chain_n, char* scan, size_t scan_n)
		{
			const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
			const uintptr_t lo = reinterpret_cast<uintptr_t>(tib->StackLimit);
			const uintptr_t hi = reinterpret_cast<uintptr_t>(tib->StackBase);

			_snprintf_s(chain, chain_n, _TRUNCATE, "call chain:");
			append_location(chain, chain_n, ctx->Eip);
			uintptr_t ebp = ctx->Ebp;
			for (int i = 0; i < 16 && ebp >= lo && ebp + 8 <= hi && (ebp & 3) == 0; i++)
			{
				const auto* frame = reinterpret_cast<const uintptr_t*>(ebp);
				const uintptr_t ret = frame[1];
				if (!is_module_code(ret)) break;
				append_location(chain, chain_n, ret);
				if (frame[0] <= ebp) break;
				ebp = frame[0];
			}

			_snprintf_s(scan, scan_n, _TRUNCATE, "code addresses on the stack:");
			int found = 0;
			for (uintptr_t p = ctx->Esp; p >= lo && p + 4 <= hi && p < ctx->Esp + 4096 && found < 24; p += 4)
			{
				const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
				if (!is_module_code(v)) continue;
				append_location(scan, scan_n, v);
				found++;
			}
		}

		void describe_stack(const CONTEXT* ctx, char* chain, size_t chain_n, char* scan, size_t scan_n)
		{
			__try { describe_stack_unguarded(ctx, chain, chain_n, scan, scan_n); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		constexpr DWORD kCxxExceptionCode = 0xE06D7363;

		// The block in remix-comp.log and the marker the next session reports.
		// Its buffers are in its own frame, not the crash filter's, so the crash
		// log is written first even on a thread whose stack overflowed.
		constexpr double kMiB = 1024.0 * 1024.0;

		// What of the address space is the mod's: the bridge client's CPU copies
		// of textures and buffers, and the D3DPOOL_DEFAULT textures kept without
		// one (tex_pool.hpp). Allocates nothing (the crash block uses it).
		void memory_breakdown(const AddressSpace& a, char* out, size_t out_n)
		{
			char bridge[160] = {};
			tex_pool::BridgeMemory bm;
			if (tex_pool::bridge_memory(bm))
				_snprintf_s(bridge, _TRUNCATE, "bridge client CPU copies %.0f MiB (textures %.0f MiB in %llu surfaces, "
					"buffers %.0f MiB in %llu)", static_cast<double>(bm.surface_bytes + bm.buffer_bytes) / kMiB,
					static_cast<double>(bm.surface_bytes) / kMiB, static_cast<unsigned long long>(bm.surfaces),
					static_cast<double>(bm.buffer_bytes) / kMiB, static_cast<unsigned long long>(bm.buffers));
			else
				strcpy_s(bridge, "bridge client CPU copies n/a (the bridge does not export BridgeClient_GetMemoryStats)");
			const tex_pool::Stats ts = tex_pool::stats();
			_snprintf_s(out, out_n, _TRUNCATE, "memory: images %u MiB, mapped %u MiB, private %u MiB | %s | "
				"DEFAULT-pool textures kept %llu (%.0f MiB), made MANAGED %llu (%.0f MiB)",
				a.image_mib, a.mapped_mib, a.private_mib, bridge, static_cast<unsigned long long>(ts.kept),
				static_cast<double>(ts.kept_bytes) / kMiB, static_cast<unsigned long long>(ts.promoted),
				static_cast<double>(ts.promoted_bytes) / kMiB);
		}

		__declspec(noinline) void write_crash_block(EXCEPTION_POINTERS* ep, const char* crash_file)
		{
			const EXCEPTION_RECORD* er = ep->ExceptionRecord;
			char mod[MAX_PATH];
			uintptr_t offset = 0;
			module_of(er->ExceptionAddress, mod, offset);
			char header[512];
			sprintf_s(header, "unhandled exception 0x%08lX at 0x%p (%s+0x%IX), thread %lu", er->ExceptionCode,
				er->ExceptionAddress, mod, offset, GetCurrentThreadId());

			char cxx[512] = {};
			if (er->ExceptionCode == kCxxExceptionCode) crash_log::describe_cxx_exception(er, cxx, sizeof(cxx));
			char memory[160] = {};
			const AddressSpace a = address_space();
			_snprintf_s(memory, _TRUNCATE, "address space: %u of %u MiB used, largest free block %u MiB",
				a.used_mib, a.total_mib, a.largest_free_mib);
			char ours[256] = {};
			memory_breakdown(a, ours, sizeof(ours));
			char chain[1024] = {};
			char scan[1024] = {};
			if (ep->ContextRecord) describe_stack(ep->ContextRecord, chain, sizeof(chain), scan, sizeof(scan));
			char crash_line[160] = {};
			if (crash_file) _snprintf_s(crash_line, _TRUNCATE, "crash log: rtx-remix\\logs\\%s", crash_file);

			const char* details[] = { cxx, memory, ours, chain, scan, crash_line };
			shared::common::log_write_recent(header, details, std::size(details));
			fault_report::on_crash(header, crash_file);
		}

		LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
		{
			if (!ep || !ep->ExceptionRecord) return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
			const char* crash_file = crash_log::on_crash(ep);
			if (InterlockedExchange(&g_crash_written, 1) == 0) write_crash_block(ep, crash_file);
			const LONG verdict = g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
			crash_log::after_chain(verdict, reinterpret_cast<const void*>(g_prev_filter));
			return verdict;
		}

		std::string windows_version()
		{
			using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
			const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			const auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
			OSVERSIONINFOW v = {};
			v.dwOSVersionInfoSize = sizeof(v);
			if (!fn || fn(&v) != 0) return "unknown";
			return std::format("{}.{} build {}", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
		}

		// Every adapter DXGI lists, with its driver (user-mode driver version) and dedicated VRAM.
		std::string gpus()
		{
			using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
			const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
			const auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
			IDXGIFactory1* factory = nullptr;
			if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
				return "unknown (no DXGI)";

			std::string out;
			IDXGIAdapter1* adapter = nullptr;
			for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
			{
				DXGI_ADAPTER_DESC1 d = {};
				if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & kAdapterFlagSoftware))
				{
					char name[128] = {};
					WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
					std::string driver = "?";
					LARGE_INTEGER umd = {};
					if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
					{
						const uint32_t a = HIWORD(umd.HighPart), b = LOWORD(umd.HighPart);
						const uint32_t c = HIWORD(umd.LowPart), e = LOWORD(umd.LowPart);
						driver = std::format("{}.{}.{}.{}", a, b, c, e);
						// NVIDIA's own numbering is the last five digits: 32.0.15.6614 is 566.14.
						if (d.VendorId == 0x10DE)
						{
							const uint32_t nv = (c % 10) * 10000 + e;
							driver += std::format(" (NVIDIA {}.{:02})", nv / 100, nv % 100);
						}
					}
					out += std::format("{}{} [{:04X}:{:04X}] driver {} VRAM {} MiB", out.empty() ? "" : "; ", name,
						d.VendorId, d.DeviceId, driver, d.DedicatedVideoMemory >> 20);
				}
				adapter->Release();
			}
			factory->Release();
			return out.empty() ? "none listed" : out;
		}

		std::string desktop_mode()
		{
			DEVMODEW dm = {};
			dm.dmSize = sizeof(dm);
			if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return "unknown";
			return std::format("{}x{} @ {} Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
		}

		// The first printable run that starts with `needle` in a module's
		// read-only data, or false.
		bool find_text_unguarded(const uint8_t* base, const char* needle, char* out, size_t out_n)
		{
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
			const size_t n = std::strlen(needle);
			const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
			{
				if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)) continue;
				if (sec->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE)) continue;
				const uint8_t* p = base + sec->VirtualAddress;
				const size_t len = sec->Misc.VirtualSize;
				for (size_t k = 0; k + n <= len; k++)
				{
					if (p[k] != static_cast<uint8_t>(needle[0]) || std::memcmp(p + k, needle, n) != 0) continue;
					size_t m = 0;
					while (m + 1 < out_n && k + m < len && p[k + m] >= 0x21 && p[k + m] < 0x7F)
					{
						out[m] = static_cast<char>(p[k + m]);
						m++;
					}
					out[m] = '\0';
					return true;
				}
			}
			return false;
		}

		bool find_text(const uint8_t* base, const char* needle, char* out, size_t out_n)
		{
			__try { return base && find_text_unguarded(base, needle, out, out_n); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		std::string bridge_version()
		{
			char version[64] = {};
			const auto* base = reinterpret_cast<const uint8_t*>(shared::globals::d3d9_chain_module);
			if (find_text(base, "remix-main+", version, sizeof(version))) return version;
			return base ? "not found in the bridge client" : "no Remix bridge loaded";
		}

		// The NVSE plugins, then the other DLLs loaded from the game folder. A
		// plugin is any DLL in an NVSE\Plugins folder: a mod manager's virtual
		// file system (Mod Organizer 2) loads plugins from the mod's own folder
		// outside the game folder.
		std::string game_folder_modules()
		{
			const std::filesystem::path root = std::filesystem::path(shared::globals::root_path);
			std::wstring root_l = root.wstring();
			std::transform(root_l.begin(), root_l.end(), root_l.begin(), ::towlower);

			const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (snap == INVALID_HANDLE_VALUE) return "unknown";
			std::vector<std::string> plugins, others;
			MODULEENTRY32W me = {};
			me.dwSize = sizeof(me);
			for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
			{
				std::wstring path_l = me.szExePath;
				std::transform(path_l.begin(), path_l.end(), path_l.begin(), ::towlower);
				const bool plugin = path_l.find(L"\\nvse\\plugins\\") != std::wstring::npos;
				if (!plugin && path_l.rfind(root_l, 0) != 0) continue;
				if (path_l.size() > 4 && path_l.substr(path_l.size() - 4) == L".exe") continue;
				char name[MAX_PATH] = {};
				WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof(name) - 1, nullptr, nullptr);
				(plugin ? plugins : others).emplace_back(name);
			}
			CloseHandle(snap);

			auto join = [](const std::vector<std::string>& v)
				{
					std::string s;
					for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
					return s.empty() ? std::string("none") : s;
				};
			return std::format("NVSE plugins ({}): {} | other DLLs from the game folder: {}", plugins.size(),
				join(plugins), join(others));
		}

		// NVTF's switches that change the game thread's frame timing and locking.
		// Without bGTCFix the engine's GetTickCount frame timer sleeps on frames shorter than 10 ms.
		std::string nvtf_switches()
		{
			const std::filesystem::path ini = std::filesystem::path(shared::globals::root_path) / "Data" / "NVSE" / "Plugins" / "nvtf.ini";
			std::error_code ec;
			if (!std::filesystem::exists(ini, ec)) return "NVTF: no Data\\NVSE\\Plugins\\nvtf.ini";
			const std::string p = ini.string();
			auto v = [&p](const char* section, const char* key)
				{
					const int x = static_cast<int>(GetPrivateProfileIntA(section, key, -1, p.c_str()));
					return std::format("{}={}", key, x < 0 ? std::string("unset") : std::to_string(x));
				};
			return std::format("NVTF: {} {} {} {} {}", v("Main", "bGTCFix"), v("GTC", "bFPSFix"),
				v("Main", "bEnableThreadingTweaks"), v("Main", "bModifyDirectXBehavior"), v("DirectX", "bUseFlipModel"));
		}


		const char* game_state_name(uint8_t s)
		{
			switch (s)
			{
			case FNVR_GS_MAIN_MENU: return "main menu";
			case FNVR_GS_LOADING:   return "loading";
			case FNVR_GS_IN_GAME:   return "in game";
			case FNVR_GS_MENU:      return "menu";
			default:                return "?";
			}
		}

		std::string location(const FnvRemixFacts& f)
		{
			if (!nvse_link::linked() || !f.ready) return "unknown (no engine monitor)";
			if (f.interior) return "interior";
			if (f.grid_dim) return std::format("exterior grid ({}, {})", f.grid_cx, f.grid_cy);
			return "no cells";
		}

		void startup_report()
		{
			shared::common::log_event("Session", std::format("Windows {} | GPU {} | desktop {}", windows_version(), gpus(),
				desktop_mode()));
			shared::common::log_event("Session", std::format("Remix bridge {} (the runtime's version is in "
				"rtx-remix\\logs\\remix-dxvk.log)", bridge_version()));
			shared::common::log_event("Session", game_folder_modules());
			shared::common::log_event("Session", nvtf_switches());
		}

		// [Memory]: the breakdown of the status line's address space, and how the
		// D3DPOOL_DEFAULT textures were handled ([RemixApi] KeepDefaultPoolTextures).
		void memory_line(const AddressSpace& a)
		{
			char breakdown[512] = {};
			memory_breakdown(a, breakdown, sizeof(breakdown));
			const tex_pool::Stats ts = tex_pool::stats();
			const api::textures::DefaultStats ds = api::textures::default_stats();
			const bool keep = shared::common::config::get().remix_api.keep_default_pool_textures;
			shared::common::log_event("Memory", std::format(
				"{} | KeepDefaultPoolTextures={}, import {} | made MANAGED because: switch {}, FaceGen face map {}, format {}, "
				"autogen mips {}, import unavailable or not yet asked {} | DEFAULT textures: captured {} (table {}), "
				"imported {}, refused: no capture {}, import {}{}",
				breakdown, keep ? 1 : 0, api::textures::import_available() ? "available" : "unavailable or not asked",
				ts.by_switch, ts.by_facegen, ts.by_format, ts.by_autogen, ts.by_import, ds.captured, ds.entries, ds.imports,
				ds.uncaptured, ds.refused, api::textures::capture_failed() ? " | no capture ever worked: new ones are created MANAGED" : ""));
		}

		void status_line(double seconds)
		{
			const FnvRemixFacts& f = nvse_link::facts();
			const uint64_t warnings = shared::common::log_warning_count();
			const uint64_t errors = shared::common::log_error_count();
			const uint64_t held_back = shared::common::log_held_back_count();
			const double avg = g_frames ? g_frame_ms_sum / g_frames : 0.0;
			const std::string& w = weather::current();
			const AddressSpace a = address_space();
			shared::common::log_event("Session", std::format(
				"status: {:.0f} s, {} frames, {:.1f} fps, frame ms avg {:.1f} worst {:.1f} | {}, {} | weather {} | "
				"lights {}, retained instances {} | owners {}, owned geometries {} | address space {} of {} MiB used, "
				"largest free block {} MiB | since the last status: warnings +{}, errors +{}, held back +{}",
				seconds, g_frames, avg > 0.0 ? 1000.0 / avg : 0.0, avg, g_frame_ms_worst,
				nvse_link::linked() && f.ready ? game_state_name(f.game_state) : "unknown", location(f),
				w.empty() ? std::string("not read") : w, device_light_count, api::retained::live_count(), f.owners, f.owner_geos,
				a.used_mib, a.total_mib, a.largest_free_mib, warnings - g_warnings, errors - g_errors, held_back - g_held_back));
			memory_line(a);
			g_warnings = warnings;
			g_errors = errors;
			g_held_back = held_back;
		}

		// ------------------------------------------------------------------
		// [Perf]: where the game thread's in-game frames went, every 30 s of
		// in-game frames, with a verdict. [PerfSlow] adds detail for a slow
		// window, [PerfSummary] closes the session.
		// ------------------------------------------------------------------

		constexpr double kPerfWindowMs = 30000.0;
		constexpr double kPerfSummarySeconds = 600.0;
		constexpr double kPerfSlowMinSeconds = 120.0;
		constexpr uint32_t kPerfSlowMaxLines = 8;
		// A frame this long is a hitch (a load, a save, alt-tab), not part of the frame rate.
		constexpr double kPerfGapMs = 1000.0;
		constexpr uint32_t kMaxParts = 32;
		constexpr uint32_t kMaxRoutes = 48;
		// Routes and end-of-frame parts named in the [Perf] line.
		constexpr size_t kPerfTopRoutes = 8;
		constexpr size_t kPerfTopParts = 5;

		enum Verdict : uint8_t { kRenderer, kGameThread, kBoth, kVerdictCount };
		constexpr const char* kVerdictNames[kVerdictCount] = { "renderer", "game thread", "both" };

		struct Part
		{
			const char* name = nullptr;
			int64_t ticks = 0;
		};

		// One draw route's time and draw count.
		struct RouteTime
		{
			const char* name = nullptr;
			int64_t ticks = 0;
			uint32_t draws = 0;
		};

		// The frame being built: draws, end-of-frame parts and the Present wait.
		struct PendingFrame
		{
			int64_t draw_ticks = 0;
			uint32_t draws = 0;
			int64_t end_of_frame = 0;
			int64_t present_wait = 0;
			Part parts[kMaxParts];
			uint32_t part_count = 0;
			RouteTime routes[kMaxRoutes];
			uint32_t route_count = 0;
		} g_pending;

		struct PerfWindow
		{
			std::vector<float> frame_ms;
			double frame_ms_sum = 0.0;
			int64_t draw_ticks = 0;
			uint64_t draws = 0;
			int64_t end_of_frame = 0;
			int64_t present_wait = 0;
			Part parts[kMaxParts];
			uint32_t part_count = 0;
			RouteTime routes[kMaxRoutes];
			uint32_t route_count = 0;
			uint64_t plugin_calls = 0;
			int64_t plugin_ticks = 0;
			uint32_t hitches = 0;
			uint32_t frames_out_of_game = 0;
			uint64_t thread_cpu = 0;        // 100 ns, over the counted frames
			bool thread_cpu_ok = true;
		} g_perf;
		uint64_t g_thread_cpu_last = 0;     // at the last Present, 0 when not read

		struct SessionWindow
		{
			float fps;
			uint8_t verdict;
			char time[16];
			char place[40];
		};
		std::vector<SessionWindow> g_session;
		size_t g_session_at_summary = 0;
		LARGE_INTEGER g_last_summary = {};
		LARGE_INTEGER g_last_slow = {};
		uint32_t g_slow_logged = 0;

		uint64_t thread_cpu_100ns()
		{
			FILETIME creation, exit, kernel, user;
			if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0;
			const auto u64 = [](const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
			return u64(kernel) + u64(user);
		}

		double ticks_ms(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_freq.QuadPart); }

		float sorted_at(const std::vector<float>& v, double q)
		{
			if (v.empty()) return 0.f;
			return v[(std::min)(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
		}

		void add_part(Part (&parts)[kMaxParts], uint32_t& count, const char* name, int64_t ticks)
		{
			for (uint32_t i = 0; i < count; i++)
			{
				if (parts[i].name == name) { parts[i].ticks += ticks; return; }
			}
			if (count < kMaxParts) parts[count++] = { name, ticks };
		}

		// Route tags are string literals; the same text can sit at different
		// addresses in different translation units.
		void add_route(RouteTime (&routes)[kMaxRoutes], uint32_t& count, const char* name, int64_t ticks, uint32_t draws)
		{
			uint32_t i = 0;
			while (i < count && routes[i].name != name) i++;
			if (i == count)
			{
				i = 0;
				while (i < count && std::strcmp(routes[i].name, name) != 0) i++;
			}
			if (i < count)
			{
				routes[i].ticks += ticks;
				routes[i].draws += draws;
			}
			else if (count < kMaxRoutes) routes[count++] = { name, ticks, draws };
		}

		// "name ms (draws), ..." per frame, for the routes taking the most time.
		std::string route_split(const PerfWindow& w, double frames)
		{
			std::vector<RouteTime> r(w.routes, w.routes + w.route_count);
			std::sort(r.begin(), r.end(), [](const RouteTime& a, const RouteTime& b) { return a.ticks > b.ticks; });
			std::string s;
			for (size_t i = 0; i < r.size() && i < kPerfTopRoutes; i++)
				s += std::format("{}{} {:.2f} ({:.0f})", i ? ", " : "", r[i].name, ticks_ms(r[i].ticks) / frames,
					static_cast<double>(r[i].draws) / frames);
			return s.empty() ? std::string("-") : s;
		}

		// "name ms, ..." per frame, for the end-of-frame parts taking the most time.
		std::string part_split(const Part* parts, uint32_t count, double frames, size_t top)
		{
			std::vector<Part> p(parts, parts + count);
			std::sort(p.begin(), p.end(), [](const Part& a, const Part& b) { return a.ticks > b.ticks; });
			std::string s;
			for (size_t i = 0; i < p.size() && i < top; i++)
				s += std::format("{}{} {:.2f}", i ? ", " : "", p[i].name, ticks_ms(p[i].ticks) / frames);
			return s.empty() ? std::string("-") : s;
		}

		void local_time(char (&out)[16])
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			sprintf_s(out, "%02u:%02u:%02u", static_cast<unsigned>(t.wHour), static_cast<unsigned>(t.wMinute), static_cast<unsigned>(t.wSecond));
		}

		bool in_game()
		{
			if (!nvse_link::linked()) return true;
			const FnvRemixFacts& f = nvse_link::facts();
			return !f.ready || (f.game_state == FNVR_GS_IN_GAME && !f.loading);
		}

		void start_perf_window()
		{
			g_perf = PerfWindow();
			g_perf.frame_ms.reserve(4096);
			sampler::begin_window();
		}

		void log_perf_summary(const char* when)
		{
			QueryPerformanceCounter(&g_last_summary);
			if (g_session.size() == g_session_at_summary) return;
			g_session_at_summary = g_session.size();

			std::vector<float> fps;
			uint32_t verdicts[kVerdictCount] = {};
			for (const auto& w : g_session)
			{
				fps.push_back(w.fps);
				verdicts[(std::min)(static_cast<uint32_t>(w.verdict), kVerdictCount - 1u)]++;
			}
			std::sort(fps.begin(), fps.end());
			uint32_t top = 0;
			for (uint32_t i = 1; i < kVerdictCount; i++)
				if (verdicts[i] > verdicts[top]) top = i;
			std::string shares;
			for (uint32_t i = 0; i < kVerdictCount; i++)
			{
				if (!verdicts[i]) continue;
				shares += std::format("{}{} {:.0f}%", shares.empty() ? "" : ", ", kVerdictNames[i],
					100.0 * verdicts[i] / static_cast<double>(g_session.size()));
			}
			std::vector<const SessionWindow*> slowest;
			for (const auto& w : g_session) slowest.push_back(&w);
			std::sort(slowest.begin(), slowest.end(), [](const SessionWindow* a, const SessionWindow* b) { return a->fps < b->fps; });
			std::string worst;
			for (size_t i = 0; i < slowest.size() && i < 3; i++)
			{
				worst += std::format("{}{:.1f} fps at {} in {} ({})", i ? ", " : "", slowest[i]->fps, slowest[i]->time,
					slowest[i]->place, kVerdictNames[(std::min)(static_cast<uint32_t>(slowest[i]->verdict), kVerdictCount - 1u)]);
			}
			shared::common::log_event("PerfSummary", std::format(
				"{}: {} windows of 30 s in game, median {:.1f} fps, worst window {:.1f} fps | the frame rate was held most often by the {} "
				"({}) | slowest windows: {}",
				when, g_session.size(), sorted_at(fps, 0.5), fps.front(), kVerdictNames[top], shares, worst));
			shared::common::log_event("PerfSummary", std::format("{}: game thread {}", when, sampler::session_detail()));
		}

		void end_perf_window(const LARGE_INTEGER& now)
		{
			PerfWindow& w = g_perf;
			const double n = static_cast<double>(w.frame_ms.size());
			const double avg = w.frame_ms_sum / n;
			const double fps = 1000.0 / (std::max)(avg, 0.001);
			std::vector<float> sorted = w.frame_ms;
			std::sort(sorted.begin(), sorted.end());
			const double draw_ms = ticks_ms(w.draw_ticks) / n;
			const double eof_ms = ticks_ms(w.end_of_frame) / n;
			const double wait_ms = ticks_ms(w.present_wait) / n;
			const double engine_ms = (std::max)(0.0, avg - draw_ms - eof_ms - wait_ms);
			// The thread's CPU time over the counted frames only.
			const bool cpu_valid = w.thread_cpu_ok;
			const double cpu_ms = cpu_valid ? static_cast<double>(w.thread_cpu) / 1e4 / n : 0.0;

			Verdict verdict;
			std::string why;
			if (wait_ms >= 0.3 * avg)
			{
				verdict = kRenderer;
				why = std::format("the game thread waits {:.1f} ms per frame in Present for the renderer: the [Perf] line in "
					"remix-dxvk.log names the GPU, V-Sync, the frame limit or the render thread", wait_ms);
			}
			else
			{
				verdict = wait_ms < 0.1 * avg ? kGameThread : kBoth;
				const char* biggest = engine_ms >= draw_ms && engine_ms >= eof_ms ? "the engine (game code, NVSE plugins and the "
					"wrapper's engine hooks)" : draw_ms >= eof_ms ? "the wrapper's draw handling and the bridge sends" : "the wrapper's end of frame";
				why = std::format("{}: most of the game thread's frame is {}", verdict == kGameThread ? "the game thread holds the frame rate (CPU)"
					: "the game thread and the renderer both hold the frame rate", biggest);
				if (cpu_valid && cpu_ms < 0.7 * (avg - wait_ms))
				{
					why += std::format(", and the thread is on the CPU only {:.1f} of those {:.1f} ms, so it sleeps or waits "
						"(the engine's frame timer without NVTF's bGTCFix, or locks shared with other threads)", cpu_ms, avg - wait_ms);
				}
			}

			const sampler::Text profile = sampler::end_window(static_cast<uint32_t>(w.frame_ms.size()), w.frame_ms_sum / 1000.0);
			const FnvRemixFacts& f = nvse_link::facts();
			const std::string place = nvse_link::linked() && f.ready ? location(f) : std::string("unknown");
			const std::string& weather_id = weather::current();
			shared::common::log_event("Perf", std::format(
				"{} | {:.0f} s in game, {} frames, {:.1f} fps, frame ms p50/p95/p99 {:.1f}/{:.1f}/{:.1f}, hitches over 1 s {} | game thread ms per "
				"frame: engine {:.1f}, wrapper draws {:.1f} ({:.0f} draws, bridge sends included), wrapper end of frame {:.1f}, waiting in "
				"Present {:.1f}; thread CPU {} | wrapper draws by route, ms (draws) per frame: {} | wrapper end of frame by part, ms "
				"per frame: {} | wrapper calls into the engine monitor plugin (counted in the times above) {:.2f} ms ({:.0f} calls) per frame | {} | {}, "
				"weather {} | retained instances {}, lights {}, owners {}",
				why, w.frame_ms_sum / 1000.0, w.frame_ms.size(), fps, sorted_at(sorted, 0.5), sorted_at(sorted, 0.95), sorted_at(sorted, 0.99),
				w.hitches, engine_ms, draw_ms, static_cast<double>(w.draws) / n, eof_ms, wait_ms,
				cpu_valid ? std::format("{:.1f} ms", cpu_ms) : std::string("not measured"),
				route_split(w, n), part_split(w.parts, w.part_count, n, kPerfTopParts), ticks_ms(w.plugin_ticks) / n,
				static_cast<double>(w.plugin_calls) / n, profile.compact, place, weather_id.empty() ? std::string("not read") : weather_id, api::retained::live_count(), device_light_count, f.owners));

			// A slow window gets one detailed snapshot, a few per session.
			std::vector<float> session_fps;
			for (const auto& sw : g_session) session_fps.push_back(sw.fps);
			std::sort(session_fps.begin(), session_fps.end());
			const float median = sorted_at(session_fps, 0.5);
			const bool slow = fps < 30.0 || (session_fps.size() >= 4 && fps < 0.7 * median);
			if (slow && g_slow_logged < kPerfSlowMaxLines && (g_slow_logged == 0 || seconds_between(g_last_slow, now) >= kPerfSlowMinSeconds))
			{
				g_slow_logged++;
				g_last_slow = now;
				shared::common::log_event("PerfSlow", std::format(
					"{:.1f} fps against the session median {:.1f} ({} of at most {}) in {}: worst frame {:.1f} ms | wrapper end of frame "
					"ms per frame: {} | frames outside the game inside the window {} | game thread {}",
					fps, median, g_slow_logged, kPerfSlowMaxLines, place, sorted.back(), part_split(w.parts, w.part_count, n, 8),
					w.frames_out_of_game, profile.detail));
			}

			SessionWindow record = {};
			record.fps = static_cast<float>(fps);
			record.verdict = verdict;
			local_time(record.time);
			strncpy_s(record.place, place.c_str(), _TRUNCATE);
			if (g_session.size() < 100000) g_session.push_back(record);
			if (seconds_between(g_last_summary, now) >= kPerfSummarySeconds) log_perf_summary("so far");
		}

		// The engine monitor plugin's call totals at the previous Present.
		uint64_t g_plugin_calls_seen = 0;
		int64_t g_plugin_ticks_seen = 0;

		// Called at each Present with the frame that just ended.
		void perf_frame(double frame_ms, const LARGE_INTEGER& now)
		{
			const bool game = in_game();
			const bool counted = game && frame_ms <= kPerfGapMs;
			sampler::frame_done(counted);
			sampler::set_active(game);
			// The thread's CPU time is read at every Present, so a counted
			// frame's share is the difference across it.
			const uint64_t cpu = thread_cpu_100ns();
			const bool cpu_known = cpu && g_thread_cpu_last && cpu >= g_thread_cpu_last;
			const uint64_t cpu_frame = cpu_known ? cpu - g_thread_cpu_last : 0;
			g_thread_cpu_last = cpu;
			PendingFrame p = g_pending;
			uint64_t plugin_calls = 0;
			int64_t plugin_ticks = 0;
			nvse_link::call_totals(plugin_calls, plugin_ticks);
			const uint64_t frame_plugin_calls = plugin_calls - std::exchange(g_plugin_calls_seen, plugin_calls);
			const int64_t frame_plugin_ticks = plugin_ticks - std::exchange(g_plugin_ticks_seen, plugin_ticks);
			g_pending.draw_ticks = 0;
			g_pending.draws = 0;
			g_pending.end_of_frame = 0;
			g_pending.present_wait = 0;
			g_pending.part_count = 0;
			g_pending.route_count = 0;

			if (!game)
			{
				g_perf.frames_out_of_game++;
				return;
			}
			if (frame_ms > kPerfGapMs)
			{
				g_perf.hitches++;
				return;
			}
			PerfWindow& w = g_perf;
			w.thread_cpu += cpu_frame;
			if (!cpu_known) w.thread_cpu_ok = false;
			if (w.frame_ms.size() < 65536) w.frame_ms.push_back(static_cast<float>(frame_ms));
			w.frame_ms_sum += frame_ms;
			w.draw_ticks += p.draw_ticks;
			w.draws += p.draws;
			w.end_of_frame += p.end_of_frame;
			w.present_wait += p.present_wait;
			w.plugin_calls += frame_plugin_calls;
			w.plugin_ticks += frame_plugin_ticks;
			for (uint32_t i = 0; i < p.part_count; i++) add_part(w.parts, w.part_count, p.parts[i].name, p.parts[i].ticks);
			for (uint32_t i = 0; i < p.route_count; i++)
				add_route(w.routes, w.route_count, p.routes[i].name, p.routes[i].ticks, p.routes[i].draws);
			if (w.frame_ms_sum < kPerfWindowMs) return;
			if (w.frame_ms.size() >= 30) end_perf_window(now);
			start_perf_window();
		}
	}

	void on_present()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);

		if (!g_started)
		{
			g_started = true;
			QueryPerformanceFrequency(&g_freq);
			g_last_frame = g_last_status = g_last_summary = now;
			sampler::start();
			g_thread_cpu_last = thread_cpu_100ns();
			start_perf_window();
			// Installed at the first frame, after xNVSE and its plugins set
			// theirs, so this one runs first and then hands over to them.
			crash_log::note_render_thread();
			g_prev_filter = SetUnhandledExceptionFilter(crash_filter);
			startup_report();
			return;
		}

		const double frame_ms = seconds_between(g_last_frame, now) * 1000.0;
		g_last_frame = now;
		g_frames++;
		g_frame_ms_sum += frame_ms;
		g_frame_ms_worst = (std::max)(g_frame_ms_worst, frame_ms);
		perf_frame(frame_ms, now);

		if (nvse_link::linked())
		{
			const FnvRemixFacts& f = nvse_link::facts();
			if (f.ready && f.top_menu != g_top_menu)
			{
				shared::common::log_event("Session", f.top_menu
					? std::format("menu {} on top{}", f.top_menu, g_top_menu ? std::format(" (was {})", g_top_menu) : "")
					: std::format("menu {} closed", g_top_menu));
				g_top_menu = f.top_menu;
			}
		}

		const double since_status = seconds_between(g_last_status, now);
		if (since_status >= kStatusSeconds)
		{
			status_line(since_status);
			g_last_status = now;
			g_frames = 0;
			g_frame_ms_sum = 0.0;
			g_frame_ms_worst = 0.0;
		}
	}

	void note_present(int64_t end_of_frame, int64_t present_wait)
	{
		g_pending.end_of_frame += end_of_frame;
		g_pending.present_wait += present_wait;
	}

	void note_draw(int64_t ticks)
	{
		g_pending.draw_ticks += ticks;
		g_pending.draws++;
		const char* route = frame_stats::take_route();
		add_route(g_pending.routes, g_pending.route_count, route ? route : "untagged", ticks, 1);
	}

	void note_part(const char* name, int64_t ticks)
	{
		add_part(g_pending.parts, g_pending.part_count, name, ticks);
	}

	void on_exit()
	{
		if (g_started) log_perf_summary("at exit");
	}

	void on_d3d_create()
	{
		static bool logged = false;
		if (logged) return;
		logged = true;
		shared::common::log_event("Session", "at renderer start: " + game_folder_modules());
	}
}
