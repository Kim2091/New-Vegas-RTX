#include "std_include.hpp"
#include "session_report.hpp"

#include <algorithm>
#include <atomic>
#include <psapi.h>
#include <shlobj.h>
#pragma warning(push)
#pragma warning(disable: 4091)
#include <dbghelp.h>
#pragma warning(pop)

#include "api_submit.hpp"
#include "crash_log.hpp"
#include "fault_report.hpp"
#include "hang_dump.hpp"
#include "nvse_link.hpp"
#include "session_archive.hpp"
#include "session_files.hpp"
#include "session_zip.hpp"

#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

#pragma comment(lib, "version.lib")

#ifndef FNVR_COMMIT
#define FNVR_COMMIT unknown
#endif
#ifndef FNVR_RELEASE
#define FNVR_RELEASE dev
#endif
#define FNVR_STR2(x) #x
#define FNVR_STR(x) FNVR_STR2(x)

namespace comp::game::session_report
{
	using namespace session_files;

	namespace
	{
		constexpr uint64_t kHeartbeatMs = 10000;
		constexpr DWORD kWatchMs = 1000;
		constexpr size_t kMaxHangDumps = 3;
		// Frames between two tries at the load order while the game's data loads.
		constexpr uint32_t kLoadOrderRetryFrames = 60;

		std::atomic<bool> g_owner { false };
		// NewVegasRTX-Reporter.exe held session.lock at startup: start() takes over later.
		bool g_after_reporter = false;
		uint64_t g_start = 0;
		std::string g_mark_prefix;              // the session's start, "YYYYMMDD-HHMMSS"
		std::atomic<uint32_t> g_frames { 0 };
		std::atomic<bool> g_loading { false };
		std::atomic<bool> g_quitting { false };
		std::atomic<bool> g_dirty { false };
		int g_mark_vk = 0;
		bool g_mark_down = false;
		bool g_load_order_taken = false;
		uint32_t g_load_order_frame = 0;
		uint64_t g_place_ms = 0;
		std::atomic<bool> g_quit_zipped { false };

		// What save_now() last did, under g_saved_mutex.
		std::atomic<bool> g_saving { false };
		std::mutex g_saved_mutex;
		Saved g_saved;
		std::wstring g_saved_path;

		// The newest report zip as Explorer finds it, under g_report_mutex,
		// for rtx.gui.reportFile.
		std::mutex g_report_mutex;
		std::string g_report_file;
		std::atomic<bool> g_report_dirty { false };
		// Frames before the first push, so the bridge has finished its handshake.
		constexpr uint32_t kReportWarmupFrames = 120;

		// What session.json says, under g_mutex (the heartbeat thread writes it).
		struct Live
		{
			std::string end = "running";
			std::string quit;
			FnvRemixPlace place = {};
			bool place_valid = false;
			std::string game_state = "starting";
			uint32_t marks = 0;
			std::vector<std::string> hang_dumps;
			std::vector<std::string> incompatible;
		};
		std::mutex g_mutex;
		Live g_live;

		using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
			PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
		MiniDumpWriteDumpFn g_write_dump = nullptr;

		// The hang dump channel to the reporter (hang_dump.hpp), set up before
		// it starts, and the reporter's process, to stop waiting if it is gone.
		hang_dump::Shared* g_dump_channel = nullptr;
		HANDLE g_dump_request = nullptr;
		HANDLE g_dump_done = nullptr;
		HANDLE g_reporter_process = nullptr;
		// The log folder, wide, copied when the channel is made: the hang path
		// allocates nothing.
		wchar_t g_dump_dir[MAX_PATH] = {};

		void open_dump_channel()
		{
			FILETIME created, exited, kernel, user;
			if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return;
			const DWORD pid = GetCurrentProcessId();
			wchar_t name[96];
			hang_dump::object_name(name, pid, ft64(created), L"map");
			const HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
				static_cast<DWORD>(sizeof(hang_dump::Shared)), name);
			if (!map) return;
			auto* view = static_cast<hang_dump::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hang_dump::Shared)));
			if (!view) { CloseHandle(map); return; }
			hang_dump::object_name(name, pid, ft64(created), L"request");
			g_dump_request = CreateEventW(nullptr, FALSE, FALSE, name);
			hang_dump::object_name(name, pid, ft64(created), L"done");
			g_dump_done = CreateEventW(nullptr, FALSE, FALSE, name);
			if (!g_dump_request || !g_dump_done)
			{
				UnmapViewOfFile(view);
				CloseHandle(map);
				return;
			}
			view->version = hang_dump::kVersion;
			g_dump_channel = view;   // the mapping stays open for the process's life
		}

		// Asks the reporter for the dump. Returns false when it could not be
		// asked or did not answer; `ok` / `error` are its result otherwise.
		bool reporter_dump(const wchar_t* path, bool& ok, DWORD& error)
		{
			if (!g_dump_channel || !g_reporter_process || WaitForSingleObject(g_reporter_process, 0) != WAIT_TIMEOUT) return false;
			wcsncpy_s(g_dump_channel->path, path, _TRUNCATE);
			const LONG request = InterlockedIncrement(&g_dump_channel->request);
			SetEvent(g_dump_request);
			const HANDLE wait[] = { g_dump_done, g_reporter_process };
			for (;;)
			{
				if (WaitForMultipleObjects(2, wait, FALSE, 60000) != WAIT_OBJECT_0) return false;
				if (InterlockedCompareExchange(&g_dump_channel->done, 0, 0) == request) break;
			}
			ok = g_dump_channel->ok != 0;
			error = g_dump_channel->error;
			return true;
		}

		std::string json_list(const std::vector<std::string>& v)
		{
			std::string s = "[";
			for (size_t i = 0; i < v.size(); i++) s += std::format("{}\"{}\"", i ? ", " : "", json_escape(v[i]));
			return s + "]";
		}

		// One field per line, "end" on its own: the archive step rewrites that line.
		std::string session_json(const Live& l)
		{
			const uint64_t now = now_ft();
			const bool p = l.place_valid;
			return std::format(
				"{{\n"
				"  \"mod\": \"New Vegas RTX\",\n"
				"  \"mod_version\": \"{}\",\n"
				"  \"commit\": \"{}\",\n"
				"  \"pid\": {},\n"
				"  \"start\": \"{}\",\n"
				"  \"start_filetime\": {},\n"
				"  \"end\": \"{}\",\n"
				"  \"quit\": \"{}\",\n"
				"  \"heartbeat\": \"{}\",\n"
				"  \"duration_s\": {},\n"
				"  \"frames\": {},\n"
				"  \"game_state\": \"{}\",\n"
				"  \"cell\": \"{}\",\n"
				"  \"worldspace\": \"{}\",\n"
				"  \"interior\": {},\n"
				"  \"cell_xy\": [{}, {}],\n"
				"  \"position\": [{:.0f}, {:.0f}, {:.0f}],\n"
				"  \"marks\": {},\n"
				"  \"hang_dumps\": {},\n"
				"  \"incompatible\": {}\n"
				"}}\n",
				FNVR_STR(FNVR_RELEASE), FNVR_STR(FNVR_COMMIT), GetCurrentProcessId(), local_text(g_start), g_start, l.end,
				json_escape(l.quit), local_text(now), (now - g_start) / kFtSecond, g_frames.load(std::memory_order_relaxed),
				l.game_state, p ? json_escape(std::string_view(l.place.cell)) : std::string(),
				p ? json_escape(std::string_view(l.place.worldspace)) : std::string(),
				p ? (l.place.interior ? "true" : "false") : "null", l.place.cell_x, l.place.cell_y, l.place.pos[0], l.place.pos[1],
				l.place.pos[2], l.marks, json_list(l.hang_dumps), json_list(l.incompatible));
		}

		void write_session_locked()
		{
			write_file(shared::common::log_dir() + L"session.json", session_json(g_live));
		}

		void write_session()
		{
			std::lock_guard lock(g_mutex);
			write_session_locked();
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

		std::string place_text(const FnvRemixPlace& p, bool valid)
		{
			if (!valid) return "unknown (no engine monitor, or no player in a cell yet)";
			const std::string_view cell = p.cell, world = p.worldspace;
			if (p.interior) return std::format("{} (interior)", cell);
			return std::format("{} ({}, {}) in {}", cell, p.cell_x, p.cell_y, world);
		}

		// The foreground window is one of this process's.
		bool game_in_front()
		{
			const HWND fg = GetForegroundWindow();
			DWORD pid = 0;
			if (fg) GetWindowThreadProcessId(fg, &pid);
			return fg && pid == GetCurrentProcessId();
		}

		// A hung window that Windows replaced with its "not responding" ghost counts as in front.
		bool game_or_ghost_in_front()
		{
			if (game_in_front()) return true;
			char cls[32] = {};
			const HWND fg = GetForegroundWindow();
			return fg && GetClassNameA(fg, cls, sizeof(cls)) && std::strcmp(cls, "Ghost") == 0;
		}

		// "F8", "F12", a letter or digit, or a virtual-key code ("0x77", "119"). 0: off or not understood.
		int parse_key(std::string s)
		{
			s.erase(0, s.find_first_not_of(" \t"));
			s.erase(s.find_last_not_of(" \t") + 1);
			if (s.empty()) return 0;
			if ((s[0] == 'F' || s[0] == 'f') && s.size() > 1 && std::all_of(s.begin() + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; }))
			{
				const int n = atoi(s.c_str() + 1);
				return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
			}
			if (s.size() == 1 && ((s[0] >= '0' && s[0] <= '9') || (s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')))
				return s[0] >= 'a' ? s[0] - 'a' + 'A' : s[0];
			char* end = nullptr;
			const long v = strtol(s.c_str(), &end, 0);
			return end && *end == '\0' && v > 0 && v < 255 ? static_cast<int>(v) : 0;
		}

		// ---- Place and mark ----

		void update_place()
		{
			FnvRemixPlace p = {};
			const bool valid = nvse_link::place(p);
			const FnvRemixFacts& f = nvse_link::facts();
			const char* state = nvse_link::linked() && f.ready ? game_state_name(f.game_state) : "unknown";
			std::lock_guard lock(g_mutex);
			// The last place read stays when none can be read now (the loading screen's thread).
			if (valid)
			{
				g_live.place = p;
				g_live.place_valid = true;
				crash_log::note_place(p);
			}
			g_live.game_state = state;
		}

		void mark()
		{
			FnvRemixPlace p = {};
			bool valid = nvse_link::place(p);
			uint32_t n = 0;
			{
				std::lock_guard lock(g_mutex);
				if (valid)
				{
					g_live.place = p;
					g_live.place_valid = true;
				}
				else
				{
					p = g_live.place;
					valid = g_live.place_valid;
				}
				n = ++g_live.marks;
			}
			SYSTEMTIME t;
			GetLocalTime(&t);
			const std::string text = std::format("#{} at {:02}:{:02}:{:02}, cell {}", n, t.wHour, t.wMinute, t.wSecond,
				place_text(p, valid));
			shared::common::log_event("Mark", text, shared::common::LOG_TYPE::LOG_TYPE_GREEN);
			nvse_link::plugin_log("Mark", text);
			api_submit::request_terrain_dump();

			// The runtime saves rtx-remix\logs\screenshots\<stem>.png and logs "[Mark] screenshot saved".
			const std::string stem = std::format("mark-{}-{:02}", g_mark_prefix, n);
			auto& api = shared::common::remix_api::get();
			if (shared::common::remix_api::is_initialized() && api.m_bridge.SetConfigVariable)
			{
				api.m_bridge.SetConfigVariable("rtx.diagnostics.screenshotRequest", stem.c_str());
				shared::common::log_event("Mark", std::format("screenshot requested: rtx-remix\\logs\\screenshots\\{}.png", stem));
			}
			else
				shared::common::log_event("Mark", "no screenshot: the Remix API is not up");
			g_dirty = true;
		}

		void poll_mark_key()
		{
			if (!g_mark_vk) return;
			const bool down = (GetAsyncKeyState(g_mark_vk) & 0x8000) != 0;
			const bool pressed = down && !g_mark_down;
			g_mark_down = down;
			if (pressed && game_in_front()) mark();
		}

		// ---- Load order ----

		struct Module
		{
			std::string name, version, product, path;
		};

		void version_info(const std::wstring& path, Module& m)
		{
			DWORD handle = 0;
			const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
			if (!size) return;
			std::vector<uint8_t> buf(size);
			if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return;
			VS_FIXEDFILEINFO* fi = nullptr;
			UINT len = 0;
			if (VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) && fi && len >= sizeof(*fi))
				m.version = std::format("{}.{}.{}.{}", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
					HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
			struct Translation { WORD language, codepage; };
			Translation* tr = nullptr;
			if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) && tr &&
				len >= sizeof(*tr))
			{
				wchar_t* text = nullptr;
				const std::wstring key = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\ProductName", tr->language, tr->codepage);
				if (VerQueryValueW(buf.data(), key.c_str(), reinterpret_cast<void**>(&text), &len) && text && len)
					m.product = utf8(std::wstring(text, wcsnlen(text, len)));
			}
		}

		// Every module in the process: NVSE plugins (any DLL in an
		// NVSE\Plugins folder, which under Mod Organizer 2 is the mod's own),
		// the rest, and those from the Windows folder.
		void list_modules(std::vector<Module>& nvse, std::vector<Module>& other, std::vector<Module>& windows)
		{
			std::vector<HMODULE> mods(1024);
			DWORD need = 0;
			if (!EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &need))
				return;
			if (need > mods.size() * sizeof(HMODULE))
			{
				mods.resize(need / sizeof(HMODULE) + 64);
				if (!EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &need))
					return;
			}
			const size_t count = (std::min)(static_cast<size_t>(need / sizeof(HMODULE)), mods.size());

			wchar_t win[MAX_PATH] = {};
			GetWindowsDirectoryW(win, MAX_PATH);
			const std::wstring windir = lower(std::wstring(win)) + L"\\";
			for (size_t i = 0; i < count; i++)
			{
				std::wstring path(32768, L'\0');
				const DWORD n = GetModuleFileNameW(mods[i], path.data(), static_cast<DWORD>(path.size()));
				path.resize(n < path.size() ? n : 0);
				if (path.empty()) continue;
				const size_t slash = path.find_last_of(L"\\/");
				Module m;
				m.name = utf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
				m.path = utf8(path);
				version_info(path, m);
				const std::wstring l = lower(path);
				if (l.find(L"\\nvse\\plugins\\") != std::wstring::npos) nvse.push_back(std::move(m));
				else if (l.starts_with(windir)) windows.push_back(std::move(m));
				else other.push_back(std::move(m));
			}
			for (auto* v : { &nvse, &other, &windows })
				std::sort(v->begin(), v->end(), [](const Module& a, const Module& b) { return lower(a.name) < lower(b.name); });
		}

		// The launcher's active plugins (%LOCALAPPDATA%\FalloutNV\plugins.txt,
		// as the game's virtual file system shows it), "-- Name.esp" per line.
		std::string plugins_txt()
		{
			wchar_t local[MAX_PATH] = {};
			std::string text;
			if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local)) ||
				!read_file(std::wstring(local) + L"\\FalloutNV\\plugins.txt", text, 1u << 20))
				return {};
			std::string out;
			size_t at = 0;
			while (at < text.size())
			{
				size_t eol = text.find('\n', at);
				if (eol == std::string::npos) eol = text.size();
				std::string line = text.substr(at, eol - at);
				at = eol + 1;
				line.erase(0, line.find_first_not_of(" \t*"));
				line.erase(line.find_last_not_of(" \t\r") + 1);
				if (!line.empty() && line[0] != '#') out += "-- " + line + "\n";
			}
			return out;
		}

		// A loadorder.txt read back, for the comparison with the previous one.
		struct Listing
		{
			std::string header;
			std::vector<std::string> plugins;
			std::map<std::string, std::pair<std::string, std::string>> nvse, dlls;   // lower-case name -> name, version
		};

		Listing parse_listing(const std::string& text)
		{
			Listing out;
			enum { kNone, kPlugins, kNvse, kDlls, kOther } section = kNone;
			size_t at = 0;
			while (at < text.size())
			{
				size_t eol = text.find('\n', at);
				if (eol == std::string::npos) eol = text.size();
				std::string line = text.substr(at, eol - at);
				at = eol + 1;
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (line.empty()) continue;
				if (line[0] == '[')
				{
					section = line.starts_with("[Plugins]") ? kPlugins : line.starts_with("[NVSE plugins]") ? kNvse
						: line.starts_with("[DLLs]") ? kDlls : kOther;
					continue;
				}
				if (section == kNone)
				{
					if (out.header.empty()) out.header = line;
				}
				else if (section == kPlugins)
				{
					const size_t sp = line.find(' ');
					if (sp != std::string::npos) out.plugins.push_back(line.substr(sp + 1));
				}
				else if (section == kNvse || section == kDlls)
				{
					const size_t a = line.find(" | ");
					const size_t b = a == std::string::npos ? std::string::npos : line.find(" | ", a + 3);
					if (a == std::string::npos) continue;
					const std::string name = line.substr(0, a);
					const std::string version = line.substr(a + 3, b == std::string::npos ? std::string::npos : b - a - 3);
					(section == kNvse ? out.nvse : out.dlls)[lower(name)] = { name, version };
				}
			}
			return out;
		}

		std::string join(const std::vector<std::string>& v)
		{
			std::string s;
			for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
			return s;
		}

		// Plugins kept from the previous load order that are not in the
		// longest run that kept its relative order: the ones that moved.
		std::vector<std::string> moved_plugins(const std::vector<std::string>& prev, const std::vector<std::string>& cur)
		{
			std::map<std::string, size_t> prev_index;
			for (size_t i = 0; i < prev.size(); i++) prev_index.emplace(lower(prev[i]), i);
			std::vector<size_t> idx;
			std::vector<std::string> names;
			for (const auto& c : cur)
				if (const auto it = prev_index.find(lower(c)); it != prev_index.end())
				{
					idx.push_back(it->second);
					names.push_back(c);
				}
			const size_t n = idx.size();
			std::vector<size_t> len(n, 1), from(n, SIZE_MAX);
			size_t best = SIZE_MAX;
			for (size_t i = 0; i < n; i++)
			{
				for (size_t j = 0; j < i; j++)
					if (idx[j] < idx[i] && len[j] + 1 > len[i])
					{
						len[i] = len[j] + 1;
						from[i] = j;
					}
				if (best == SIZE_MAX || len[i] > len[best]) best = i;
			}
			std::vector<bool> kept(n, false);
			for (size_t i = best; i != SIZE_MAX; i = from[i]) kept[i] = true;
			std::vector<std::string> moved;
			for (size_t i = 0; i < n; i++)
				if (!kept[i]) moved.push_back(names[i]);
			return moved;
		}

		// loadorder-changes.txt's text; `summary` gets the counts for remix-comp.log.
		std::string changes(const Listing& prev, const Listing& cur, std::string& summary)
		{
			std::string out = std::format("Changes since the previous session ({})\n\n", prev.header);
			auto section = [&out](const char* what, const std::vector<std::string>& v)
				{
					if (!v.empty()) out += std::format("{} ({}): {}\n", what, v.size(), join(v));
				};

			std::set<std::string> prev_l, cur_l;
			for (const auto& p : prev.plugins) prev_l.insert(lower(p));
			for (const auto& p : cur.plugins) cur_l.insert(lower(p));
			std::vector<std::string> added, removed;
			for (const auto& p : cur.plugins)
				if (!prev_l.contains(lower(p))) added.push_back(p);
			for (const auto& p : prev.plugins)
				if (!cur_l.contains(lower(p))) removed.push_back(p);
			const std::vector<std::string> moved = moved_plugins(prev.plugins, cur.plugins);
			section("Plugins added", added);
			section("Plugins removed", removed);
			section("Plugins that moved in the load order", moved);
			summary = std::format("plugins +{} -{} moved {}", added.size(), removed.size(), moved.size());

			auto modules = [&](const char* what, const auto& a, const auto& b)
				{
					std::vector<std::string> plus, minus, updated;
					for (const auto& [key, v] : b)
						if (const auto it = a.find(key); it == a.end()) plus.push_back(v.first);
						else if (it->second.second != v.second) updated.push_back(std::format("{} {} -> {}", v.first, it->second.second, v.second));
					for (const auto& [key, v] : a)
						if (!b.contains(key)) minus.push_back(v.first);
					section(std::format("{} added", what).c_str(), plus);
					section(std::format("{} removed", what).c_str(), minus);
					section(std::format("{} with a new version", what).c_str(), updated);
					summary += std::format(", {} +{} -{} updated {}", what, plus.size(), minus.size(), updated.size());
					return plus.size() + minus.size() + updated.size();
				};
			size_t module_changes = modules("NVSE plugins", prev.nvse, cur.nvse);
			module_changes += modules("DLLs", prev.dlls, cur.dlls);
			if (added.empty() && removed.empty() && moved.empty() && module_changes == 0)
				out += "No changes in the plugins, NVSE plugins or DLLs (Windows DLLs are not compared).\n";
			return out;
		}

		struct LoadOrderJob
		{
			std::string plugins;
			std::string source;
		};

		void load_order_report(const LoadOrderJob& job)
		{
			std::vector<Module> nvse, other, windows;
			list_modules(nvse, other, windows);

			// Known-incompatible mods, by plugin file and by DLL.
			std::vector<std::string> incompatible;
			auto note = [&incompatible](const std::string& file, const char* product)
				{
					if (const char* mod = fault_report::check_module(file.c_str(), product))
					{
						const std::string entry = std::format("{} ({})", mod, file);
						if (std::find(incompatible.begin(), incompatible.end(), entry) == incompatible.end())
							incompatible.push_back(entry);
					}
				};
			std::vector<std::string> plugin_names;
			{
				size_t at = 0;
				while (at < job.plugins.size())
				{
					size_t eol = job.plugins.find('\n', at);
					if (eol == std::string::npos) eol = job.plugins.size();
					const std::string line = job.plugins.substr(at, eol - at);
					at = eol + 1;
					const size_t sp = line.find(' ');
					if (sp != std::string::npos) plugin_names.push_back(line.substr(sp + 1));
				}
			}
			for (const auto& p : plugin_names) note(p, nullptr);
			for (const auto* v : { &nvse, &other })
				for (const Module& m : *v) note(m.name, m.product.empty() ? nullptr : m.product.c_str());
			{
				std::lock_guard lock(g_mutex);
				g_live.incompatible = incompatible;
			}
			g_dirty = true;

			std::string text = std::format("New Vegas RTX {} (commit {}), session started {}\n\n", FNVR_STR(FNVR_RELEASE),
				FNVR_STR(FNVR_COMMIT), local_text(g_start));
			text += std::format("[Plugins] {} in load order, from {}\n", plugin_names.size(), job.source);
			text += job.plugins;
			if (!job.plugins.empty() && job.plugins.back() != '\n') text += '\n';
			auto modules = [&text](const std::vector<Module>& v)
				{
					for (const Module& m : v)
						text += std::format("{} | {} | {}\n", m.name, m.version.empty() ? "-" : m.version, m.path);
				};
			text += std::format("\n[NVSE plugins] {} (file | version | path)\n", nvse.size());
			modules(nvse);
			text += std::format("\n[DLLs] {} other modules in the process\n", other.size());
			modules(other);
			text += std::format("\n[Windows DLLs] {} from the Windows folder\n", windows.size());
			modules(windows);
			text += std::format("\n[Known incompatible] {}\n", incompatible.empty() ? std::string("none") : std::to_string(incompatible.size()));
			for (const auto& i : incompatible) text += i + "\n";

			std::string summary = "no previous load order to compare with";
			if (shared::common::config::get().session_report.snapshots)
			{
				const std::wstring& dir = shared::common::log_dir();
				write_file(dir + L"loadorder.txt", text);
				std::string prev_text;
				std::string diff = "No previous load order (loadorder.prev.txt) to compare with.\n";
				if (read_file(dir + L"loadorder.prev.txt", prev_text, 4u << 20))
					diff = changes(parse_listing(prev_text), parse_listing(text), summary);
				write_file(dir + L"loadorder-changes.txt", diff);
			}
			shared::common::log_event("Session", std::format("load order: {} plugins from {}, {} NVSE plugins, {} other DLLs | "
				"since the previous session: {}{}", plugin_names.size(), job.source, nvse.size(), other.size(), summary,
				incompatible.empty() ? std::string() : " | known incompatible: " + join(incompatible)));
		}

		DWORD WINAPI load_order_thread(LPVOID param)
		{
			std::unique_ptr<LoadOrderJob> job(static_cast<LoadOrderJob*>(param));
			try
			{
				load_order_report(*job);
			}
			catch (...)
			{
				shared::common::log("Session", "the load-order snapshot failed", shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
			return 0;
		}

		// Once the engine's plugin list can be read (the game's data is
		// loaded), or plugins.txt once the plugin is known to be missing.
		void take_load_order()
		{
			if (g_load_order_taken) return;
			const uint32_t frame = g_frames.load(std::memory_order_relaxed);
			if (frame - g_load_order_frame < kLoadOrderRetryFrames) return;
			g_load_order_frame = frame;

			LoadOrderJob job;
			if (nvse_link::live())
			{
				job.plugins = nvse_link::load_order();
				if (job.plugins.empty()) return;
				job.source = "the engine (DataHandler)";
				crash_log::set_load_order(job.plugins);
			}
			else if (!nvse_link::linked() && !nvse_link::problem().empty())
			{
				job.plugins = plugins_txt();
				job.source = "plugins.txt (the engine monitor plugin is not running: the launcher's list, not what the "
					"engine loaded)";
			}
			else return;

			g_load_order_taken = true;
			auto* heap = new LoadOrderJob(std::move(job));
			if (const HANDLE t = CreateThread(nullptr, 0, load_order_thread, heap, 0, nullptr)) CloseHandle(t);
			else delete heap;
		}

		// ---- Config snapshot ----

		// The files the game and the renderer read, as this process sees them
		// (Mod Organizer 2's profile INIs included), into rtx-remix\logs\config.
		void snapshot_configs()
		{
			const std::wstring dst = shared::common::log_dir() + L"config\\";
			CreateDirectoryW(dst.c_str(), nullptr);
			wchar_t docs[MAX_PATH] = {};
			const std::wstring my_games = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))
				? std::wstring(docs) + L"\\My Games\\FalloutNV\\" : std::wstring();
			wchar_t dll[MAX_PATH] = {};
			GetModuleFileNameW(shared::globals::dll_hmodule, dll, MAX_PATH);
			std::wstring dll_dir = dll;
			dll_dir.resize(dll_dir.find_last_of(L"\\/") + 1);

			const std::pair<std::wstring, const wchar_t*> files[] = {
				{ my_games + L"Fallout.ini", L"Fallout.ini" },
				{ my_games + L"FalloutPrefs.ini", L"FalloutPrefs.ini" },
				{ my_games + L"FalloutCustom.ini", L"FalloutCustom.ini" },
				{ game_dir() + L"Data\\NVSE\\Plugins\\nvtf.ini", L"nvtf.ini" },
				{ dll_dir + L"remix-comp.ini", L"remix-comp.ini" },
				{ game_dir() + L"rtx.conf", L"rtx.conf" },
				{ game_dir() + L"user.conf", L"user.conf" },
				{ game_dir() + L"dxvk.conf", L"dxvk.conf" },
			};
			std::vector<std::string> copied, missing;
			for (const auto& [src, name] : files)
			{
				std::string data;
				if (!my_games.empty() || src.find(L"\\My Games\\") == std::wstring::npos)
					if (read_file(src, data, 16u << 20) && write_file(dst + name, data))
					{
						copied.push_back(utf8(name));
						continue;
					}
				missing.push_back(utf8(name));
			}
			shared::common::log_event("Session", std::format("config snapshot in rtx-remix\\logs\\config: {}{}", join(copied),
				missing.empty() ? std::string() : " | not found: " + join(missing)));
		}

		// ---- Zips of this session ----

		// The session so far, from its own files under their current names.
		// `json`: its session.json as it stands.
		session_zip::Request current_request(const std::string& json)
		{
			session_zip::Request r;
			r.src.dir = shared::common::log_dir();
			r.src.files = session_zip::Files::Current;
			r.src.json = json;
			r.src.start = g_start;
			r.src.end = now_ft() + 60 * kFtSecond;
			// The bridge server writes "[Session] clean shutdown" only once the
			// game process has ended; while the API is up, the renderer is.
			if (shared::common::remix_api::is_initialized()) r.renderer_alive = "true";
			return r;
		}

		session_zip::Result zip_current(const session_zip::Request& r)
		{
			try
			{
				return session_zip::zip(session_archive::settings(), r);
			}
			catch (const std::exception& ex)
			{
				session_zip::Result res;
				res.warn = true;
				res.line = std::format("zipping {} failed: {}", r.what, ex.what());
				return res;
			}
			catch (...)
			{
				session_zip::Result res;
				res.warn = true;
				res.line = std::format("zipping {} failed", r.what);
				return res;
			}
		}

		void set_report_file(const std::wstring& path)
		{
			if (path.empty()) return;
			{
				std::lock_guard lock(g_report_mutex);
				g_report_file = utf8(path);
			}
			g_report_dirty = true;
		}

		// The runtime's config parser takes no empty value, so only a known
		// file is sent.
		void send_report_file()
		{
			auto& api = shared::common::remix_api::get();
			if (!g_report_dirty.load() || !shared::common::remix_api::is_initialized() || !api.m_bridge.SetConfigVariable ||
				!g_report_dirty.exchange(false))
				return;
			std::string path;
			{
				std::lock_guard lock(g_report_mutex);
				path = g_report_file;
			}
			if (!path.empty()) api.m_bridge.SetConfigVariable("rtx.gui.reportFile", path.c_str());
		}

		// The game quits: this session's zip, at most kQuitZipMs. The zip's
		// session.json says "clean"; the one on disk stays "exiting" until the
		// process exit step, so a crash on the way out is still told apart.
		void zip_at_quit(const std::string& json, const char* how)
		{
			session_zip::Request r = current_request(json);
			r.what = "this session";
			r.end = "clean";
			r.end_detail = std::format("the game quit ({}); this zip was written as it shut down", how);
			r.deadline_ms = GetTickCount64() + session_zip::kQuitZipMs;
			const session_zip::Result res = zip_current(r);
			if (res.ok)
			{
				session_zip::mark_archived(shared::common::log_dir(), g_start, res.name);
				shared::common::log_event("Session", res.line, shared::common::LOG_TYPE::LOG_TYPE_STATUS);
				bool unknown = false;
				set_report_file(session_zip::real_path(res.path, unknown));
				send_report_file();
			}
			else
				shared::common::log_event("Session", res.line + "; NewVegasRTX-Reporter.exe or the next start zips it",
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		DWORD WINAPI save_thread(LPVOID)
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			std::string json;
			{
				std::lock_guard lock(g_mutex);
				json = session_json(g_live);
			}
			session_zip::Request r = current_request(json);
			r.what = "this session so far";
			r.name_suffix = "-snapshot-" + local_text(now_ft(), true);
			r.end = "running";
			r.end_detail = "saved with Save Report Now while the game ran";
			const session_zip::Result res = zip_current(r);
			std::wstring shown;
			if (res.ok)
			{
				bool unknown = false;
				shown = session_zip::real_path(res.path, unknown);
				shared::common::log_event("Session", std::format("report saved: {}{}", utf8(shown), unknown
					? " (Mod Organizer 2 runs the game: if Explorer does not show it there, look in MO2's Overwrite folder)"
					: ""), shared::common::LOG_TYPE::LOG_TYPE_GREEN);
				shared::common::log_event("Session", res.line);
			}
			else
				shared::common::log_event("Session", "Save Report Now: " + res.line, shared::common::LOG_TYPE::LOG_TYPE_WARN);
			{
				std::lock_guard lock(g_saved_mutex);
				g_saved.generation++;
				g_saved.text = res.ok ? utf8(shown) : std::string("not saved: see remix-comp.log");
				g_saved_path = shown;
			}
			if (res.ok) set_report_file(shown);
			g_saving = false;
			return 0;
		}

		DWORD WINAPI show_thread(LPVOID param)
		{
			std::unique_ptr<std::wstring> args(static_cast<std::wstring*>(param));
			ShellExecuteW(nullptr, L"open", L"explorer.exe", args->c_str(), nullptr, SW_SHOWNORMAL);
			return 0;
		}

		// ---- The reporter ----

		// One argument as CommandLineToArgvW reads it back.
		std::wstring quote_arg(const std::wstring& a)
		{
			std::wstring q = L"\"";
			size_t slashes = 0;
			for (const wchar_t c : a)
			{
				if (c == L'\\')
				{
					slashes++;
					continue;
				}
				q.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
				slashes = 0;
				q += c;
			}
			q.append(2 * slashes, L'\\');
			return q + L"\"";
		}

		std::wstring without_slash(std::wstring p)
		{
			while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
			return p;
		}

		// NewVegasRTX-Reporter.exe, next to d3d9.dll: it waits for this
		// process to end and zips the session then (src/reporter). Under Mod
		// Organizer 2 it inherits the game's virtual file system, so it sees
		// the same rtx-remix\logs.
		void start_reporter()
		{
			const auto& cfg = shared::common::config::get().session_report;
			if (!cfg.archive || !cfg.reporter) return;
			wchar_t dll[MAX_PATH] = {};
			const DWORD n = GetModuleFileNameW(shared::globals::dll_hmodule, dll, MAX_PATH);
			if (!n || n >= MAX_PATH) return;
			std::wstring dir = dll;
			dir.resize(dir.find_last_of(L"\\/") + 1);
			const std::wstring exe = dir + L"NewVegasRTX-Reporter.exe";
			if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
			{
				shared::common::log("Session", "NewVegasRTX-Reporter.exe is not next to d3d9.dll: a session that ends "
					"without a quit is zipped at the next start", shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			FILETIME created, exited, kernel, user;
			if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return;
			// Out of the game's job when the job allows it, so whatever watches
			// that job (a launcher's "running" state) sees the game end with
			// it. Not under Mod Organizer 2: its virtual file system follows
			// the process tree, and the reporter needs it.
			DWORD breakaway = 0;
			BOOL in_job = FALSE;
			if (!GetModuleHandleW(L"usvfs_x86.dll") && IsProcessInJob(GetCurrentProcess(), nullptr, &in_job) && in_job)
			{
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
				if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr) &&
					(limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK))
					breakaway = CREATE_BREAKAWAY_FROM_JOB;
			}
			std::wstring cmd = std::format(L"{} --pid {} --created {} --logs {} --game {} --version {} --scrub {} "
				L"--crash-folder {}", quote_arg(exe), GetCurrentProcessId(), ft64(created),
				quote_arg(without_slash(shared::common::log_dir())), quote_arg(without_slash(game_dir())),
				quote_arg(wide(FNVR_STR(FNVR_RELEASE))), cfg.scrub_user_name ? 1 : 0, quote_arg(wide(cfg.crash_log_folder)));
			STARTUPINFOW si = {};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi = {};
			const std::wstring cwd = without_slash(game_dir());
			const DWORD flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
			bool started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags | breakaway, nullptr,
				cwd.c_str(), &si, &pi) != FALSE;
			if (!started && breakaway)
			{
				breakaway = 0;
				started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, cwd.c_str(), &si,
					&pi) != FALSE;
			}
			if (!started)
			{
				shared::common::log("Session", std::format("NewVegasRTX-Reporter.exe did not start (error {}): a session that "
					"ends without a quit is zipped at the next start", GetLastError()), shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			CloseHandle(pi.hThread);
			g_reporter_process = pi.hProcess;   // kept: the hang dump checks the reporter still runs
			shared::common::log_event("Session", std::format("NewVegasRTX-Reporter.exe runs (process {}{}): it zips this "
				"session when the game process ends, however it ends", pi.dwProcessId,
				breakaway ? ", outside the game's job" : in_job ? ", in the game's job" : ""));
		}

		// ---- Threads ----

		DWORD WINAPI archive_thread(LPVOID)
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			session_archive::archive_previous();
			set_report_file(session_archive::newest_zip());
			if (shared::common::config::get().session_report.snapshots)
			{
				try
				{
					snapshot_configs();
				}
				catch (...)
				{
					shared::common::log("Session", "the config snapshot failed", shared::common::LOG_TYPE::LOG_TYPE_WARN);
				}
			}
			return 0;
		}

		// Runs with the game hung, perhaps with its address space full: the dump
		// is written by the reporter when it runs, in this process only
		// otherwise, and the path and the log line are built without
		// allocating where that can be avoided.
		void write_hang_dump(uint64_t stalled_ms)
		{
			{
				std::lock_guard lock(g_mutex);
				if (g_live.hang_dumps.size() >= kMaxHangDumps) return;
			}
			SYSTEMTIME t = {};
			GetLocalTime(&t);
			wchar_t name[64];
			swprintf_s(name, L"hang-%04u-%02u-%02u_%02u-%02u-%02u.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
			wchar_t path[MAX_PATH];
			swprintf_s(path, L"%s%s", g_dump_dir, name);

			bool ok = false;
			DWORD error = 0;
			const char* by = "the reporter";
			if (!reporter_dump(path, ok, error))
			{
				by = "the game";
				if (g_write_dump)
				{
					const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (file != INVALID_HANDLE_VALUE)
					{
						ok = g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
							static_cast<MINIDUMP_TYPE>(hang_dump::kDumpType), nullptr, nullptr, nullptr) != FALSE;
						error = ok ? 0 : GetLastError();
						CloseHandle(file);
						if (!ok) DeleteFileW(path);
					}
					else error = GetLastError();
				}
			}

			char narrow[64] = {};
			WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr);
			char text[256] = {};
			if (!ok && (g_write_dump || error)) hang_dump::error_text(error, text, sizeof(text));
			char line[512];
			const double seconds = static_cast<double>(stalled_ms) / 1000.0;
			if (ok)
				sprintf_s(line, "no frame for %.0f s with the game in front and no load running: %s wrote rtx-remix\\logs\\%s",
					seconds, by, narrow);
			else if (!g_write_dump && !error)
				sprintf_s(line, "no frame for %.0f s with the game in front and no load running: no dump (no reporter, and "
					"dbghelp.dll not loaded)", seconds);
			else
				sprintf_s(line, "no frame for %.0f s with the game in front and no load running: %s could not write a dump "
					"(error 0x%08lX: %s)", seconds, by, static_cast<unsigned long>(error), text[0] ? text : "no text");
			try
			{
				if (ok)
				{
					std::lock_guard lock(g_mutex);
					g_live.hang_dumps.push_back(narrow);
					write_session_locked();
				}
				shared::common::log_event("Hang", line, shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
			catch (...)
			{
				// Out of memory: the dump (if any) is on disk, and the next status line can't be written either.
			}
		}

		// Every second: the hang watchdog. Every 10 s (and after a mark or the
		// load-order snapshot): session.json.
		DWORD WINAPI watch_thread(LPVOID)
		{
			const int hang_s = shared::common::config::get().session_report.hang_seconds;
			if (hang_s)
			{
				wcsncpy_s(g_dump_dir, shared::common::log_dir().c_str(), _TRUNCATE);
				open_dump_channel();
			}
			start_reporter();
			// Loaded now: loading it while the game hangs could wait on the loader lock the hung thread holds.
			if (hang_s)
				if (const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
					g_write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));

			uint32_t last_frames = 0;
			uint64_t last_progress = GetTickCount64();
			uint64_t last_beat = 0;
			bool in_front = false;
			bool hung = false;
			for (;;)
			{
				Sleep(kWatchMs);
				const uint64_t now = GetTickCount64();
				const uint32_t frames = g_frames.load(std::memory_order_relaxed);
				if (frames != last_frames)
				{
					if (hung)
						shared::common::log_event("Hang", std::format("frames again after {:.0f} s",
							static_cast<double>(now - last_progress) / 1000.0));
					hung = false;
					last_frames = frames;
					last_progress = now;
					const HWND w = shared::globals::main_window;
					in_front = w && !IsIconic(w) && game_in_front();
				}
				else if (hang_s && frames && !hung && in_front && !g_quitting && !g_loading && !IsDebuggerPresent() &&
					now - last_progress >= static_cast<uint64_t>(hang_s) * 1000 && game_or_ghost_in_front())
				{
					hung = true;
					write_hang_dump(now - last_progress);
				}
				if (now - last_beat >= kHeartbeatMs || g_dirty.exchange(false))
				{
					last_beat = now;
					write_session();
				}
			}
		}
	}

	namespace
	{
		// This process owns the session files: its session.json.
		void begin_session()
		{
			std::string prefix = local_text(g_start, true);   // "YYYY-MM-DD_HH-MM-SS"
			std::erase(prefix, '-');
			std::replace(prefix.begin(), prefix.end(), '_', '-');
			g_mark_prefix = prefix;
			g_owner = true;
			write_session();
		}

		// The archive and the hang watchdog / heartbeat threads.
		void start_threads()
		{
			const auto& cfg = shared::common::config::get().session_report;
			g_mark_vk = parse_key(cfg.mark_key);
			if (!g_mark_vk && !cfg.mark_key.empty() && cfg.mark_key != "0")
				shared::common::log("Session", std::format("[SessionReport] MarkKey='{}' is not a key (F1-F24, a letter or digit, or a "
					"virtual-key code): no mark key", cfg.mark_key), shared::common::LOG_TYPE::LOG_TYPE_WARN);
			shared::common::log_event("Session", std::format("session report: rtx-remix\\logs\\session.json, archive {}, snapshots {}, "
				"mark key {}, hang watchdog {}", cfg.archive ? std::format("on (keeps {})", cfg.archive_keep) : std::string("off"),
				cfg.snapshots ? "on" : "off", g_mark_vk ? cfg.mark_key : std::string("off"),
				cfg.hang_seconds ? std::format("{} s", cfg.hang_seconds) : std::string("off")));

			if (const HANDLE t = CreateThread(nullptr, 0, archive_thread, nullptr, 0, nullptr)) CloseHandle(t);
			else session_archive::skip();
			if (const HANDLE t = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr)) CloseHandle(t);
		}

		// Started from DllMain when the reporter held session.lock: takes
		// the lock and the session files over once it lets go.
		DWORD WINAPI take_over_thread(LPVOID)
		{
			if (session_archive::take_over())
			{
				begin_session();
				start_threads();
				return 0;
			}
			session_archive::skip();
			shared::common::log_event("Session", std::format("NewVegasRTX-Reporter.exe kept rtx-remix\\logs\\session.lock: this "
				"session keeps no session.json, archive, snapshots or hang watchdog; its log is remix-comp.{}.log",
				GetCurrentProcessId()), shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return 0;
		}
	}

	void stage()
	{
		g_start = now_ft();
		switch (session_archive::stage(g_start))
		{
		case session_archive::Stage::Owner:
			begin_session();
			break;
		case session_archive::Stage::Reporter:
			// The previous session's remix-comp.log is the reporter's to move.
			g_after_reporter = true;
			shared::common::log_keep_previous();
			break;
		default:
			break;
		}
	}

	void start()
	{
		if (g_after_reporter)
		{
			if (const HANDLE t = CreateThread(nullptr, 0, take_over_thread, nullptr, 0, nullptr)) CloseHandle(t);
			else session_archive::skip();
			return;
		}
		if (!g_owner)
		{
			session_archive::skip();
			shared::common::log_event("Session", "another game instance holds rtx-remix\\logs\\session.lock: this one keeps no "
				"session.json, archive, snapshots or hang watchdog", shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return;
		}
		start_threads();
	}

	void on_present()
	{
		g_frames.fetch_add(1, std::memory_order_relaxed);
		if (!g_owner) return;
		const FnvRemixFacts& f = nvse_link::facts();
		g_loading = nvse_link::linked() && f.ready && f.loading;
		const uint64_t now = GetTickCount64();
		if (now - g_place_ms >= kHeartbeatMs)
		{
			g_place_ms = now;
			update_place();
		}
		poll_mark_key();
		take_load_order();
		if (g_frames.load(std::memory_order_relaxed) >= kReportWarmupFrames) send_report_file();
	}

	void on_exit()
	{
		if (!g_owner) return;
		// The heartbeat thread is gone at process exit and may have died holding the lock.
		bool locked = false;
		for (int i = 0; i < 25 && !(locked = g_mutex.try_lock()); i++) Sleep(2);
		g_live.end = "clean";
		write_session_locked();
		if (locked) g_mutex.unlock();
	}

	void on_quit(const char* how)
	{
		if (!g_owner) return;
		g_quitting = true;
		std::string json;
		{
			std::lock_guard lock(g_mutex);
			g_live.end = "exiting";
			g_live.quit = how ? how : "";
			write_session_locked();
			json = session_json(g_live);
		}
		shared::common::log_event("Session", std::format("the game is quitting ({})", how ? how : "?"));
		if (shared::common::config::get().session_report.archive && !g_quit_zipped.exchange(true))
			zip_at_quit(json, how ? how : "?");
	}

	bool save_now()
	{
		if (!g_owner || g_saving.exchange(true)) return false;
		{
			std::lock_guard lock(g_saved_mutex);
			g_saved.generation++;
			g_saved.text = "saving...";
		}
		if (const HANDLE t = CreateThread(nullptr, 0, save_thread, nullptr, 0, nullptr))
		{
			CloseHandle(t);
			return true;
		}
		{
			std::lock_guard lock(g_saved_mutex);
			g_saved.generation++;
			g_saved.text = "not saved: no thread";
		}
		g_saving = false;
		return false;
	}

	Saved saved()
	{
		std::lock_guard lock(g_saved_mutex);
		return g_saved;
	}

	void show_saved()
	{
		std::wstring path;
		{
			std::lock_guard lock(g_saved_mutex);
			path = g_saved_path;
		}
		auto* args = new std::wstring(path.empty() ? quote_arg(without_slash(session_archive::archive_folder()))
			: L"/select," + quote_arg(path));
		if (const HANDLE t = CreateThread(nullptr, 0, show_thread, args, 0, nullptr)) CloseHandle(t);
		else delete args;
	}
}

extern "C" void FnvRemix_SessionExit(const char* how)
{
	comp::game::session_report::on_quit(how);
}
