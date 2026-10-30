#include "std_include.hpp"
#include <psapi.h>

#include "comp.hpp"
#include "d3d9_proxy.hpp"
#include "shared/common/flags.hpp"
#include "shared/common/config.hpp"
#include "game/weather.hpp"
#include "game/sampler.hpp"
#include "game/session_log.hpp"
#include "game/crash_log.hpp"
#include "game/session_report.hpp"

#ifndef FNVR_COMMIT
#define FNVR_COMMIT unknown
#endif
#ifndef FNVR_RELEASE
#define FNVR_RELEASE dev
#endif
#define FNVR_STR2(x) #x
#define FNVR_STR(x) FNVR_STR2(x)

namespace comp
{
	std::unordered_set<HWND> wnd_class_list;

	#define WINDOW_CLASS_NAME "Fallout: New Vegas"

	BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lParam)
	{
		DWORD window_pid, target_pid = static_cast<DWORD>(lParam);
		GetWindowThreadProcessId(hwnd, &window_pid);

		if (window_pid == target_pid && IsWindowVisible(hwnd))
		{
			char class_name[256];
			GetClassNameA(hwnd, class_name, sizeof(class_name));

			if (!wnd_class_list.contains(hwnd))
			{
				char debug_msg[256];
				wsprintfA(debug_msg, "> HWND: %p, PID: %u, Class: %s, Visible: %d \n", hwnd, window_pid, class_name, IsWindowVisible(hwnd));
				shared::common::log("Main", debug_msg, shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				wnd_class_list.insert(hwnd);
			}

			if (std::string_view(class_name).contains(WINDOW_CLASS_NAME))
			{
				shared::globals::main_window = hwnd;
				return FALSE;
			}
		}

		return TRUE;
	}

	// The host process is FalloutNVLauncher.exe, the game's launcher.
	bool is_launcher_process()
	{
		wchar_t path[MAX_PATH] = {};
		const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
		if (!n || n >= MAX_PATH) return false;
		const wchar_t* slash = wcsrchr(path, L'\\');
		return _wcsicmp(slash ? slash + 1 : path, L"FalloutNVLauncher.exe") == 0;
	}

	// Without [Logging] Console=1 the process keeps no console window, whichever module opened it.
	void close_console_window()
	{
		if (shared::common::config::get().logging.console) return;
		const HWND console_hwnd = GetConsoleWindow();
		if (!console_hwnd) return;
		char title[256] = {};
		GetWindowTextA(console_hwnd, title, sizeof(title));
		ShowWindow(console_hwnd, SW_HIDE);
		FreeConsole();
		shared::common::log("Main", std::format("Closed the console window '{}' ([Logging] Console=0)", title),
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	DWORD WINAPI find_game_window([[maybe_unused]] LPVOID lpParam)
	{
		PROFILE_THREAD_NAME("comp-main");
		std::uint32_t T = 0;

		shared::common::log("Main", "Waiting for window with classname containing '" WINDOW_CLASS_NAME "' ...", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		{
			while (!shared::globals::main_window)
			{
				close_console_window();
				EnumWindows(enum_windows_proc, static_cast<LPARAM>(GetCurrentProcessId()));
				if (!shared::globals::main_window) {
					Sleep(1u); T += 1u;
				}

				if (T >= 30000)
				{
					Beep(300, 100); Sleep(100); Beep(200, 100);
					shared::common::log("Main", "Could not find '" WINDOW_CLASS_NAME "' Window. Not loading RTX Compatibility Mod.", shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
					return TRUE;
				}
			}
		}

		close_console_window();
		if (!shared::common::flags::has_flag("nobeep")) {
			Beep(523, 100);
		}


		// Patch the game's code here, not from DllMain. The window existing means
		// the exe's entry point has run, which on the retail Steam build is what
		// decrypts .text (SteamStub); patching before that corrupts real code.
		// Runs before the post-load DLLs so these patches go in first.
		comp::game::install_game_patches();

		d3d9_proxy::load_postload_dlls();

		comp::main();
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE hmodule, const DWORD ul_reason_for_call, LPVOID)
{
	if (ul_reason_for_call == DLL_PROCESS_DETACH)
	{
		comp::game::sampler::stop();
		comp::game::session_log::on_exit();
		comp::game::session_report::on_exit();
		shared::common::log_session_end();
		return TRUE;
	}

	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		// The game's launcher loads d3d9.dll from the game folder too. It gets
		// the system d3d9 and nothing else: no Remix bridge (whose server and
		// runtime would start and replace their logs), no log file (opening
		// one moves the last game session's log to remix-comp.prev.log) and no
		// hooks.
		if (comp::is_launcher_process())
		{
			shared::common::log_disable();
			d3d9_proxy::init_system();
			return TRUE;
		}

		// Before anything logs: opening a log moves the last session's file to
		// *.prev.log, and the session report moves them all first.
		comp::game::session_report::stage();

		shared::globals::setup_dll_module(hmodule);
		shared::globals::setup_exe_module();
		shared::globals::setup_homepath();

		// The build line goes first; the config's own lines follow it.
		shared::common::log_event("Main", std::format("FNV RTX Remix wrapper: release {}, commit {}, built {} {}",
			FNVR_STR(FNVR_RELEASE), FNVR_STR(FNVR_COMMIT), __DATE__, __TIME__));
		shared::common::config::get().load(shared::globals::root_path + "\\remix-comp.ini");
		const auto& logging = shared::common::config::get().logging;
		shared::common::log_configure(logging.verbose, logging.console);
		comp::close_console_window();

		// The archive and hang watchdog threads; they start running once DllMain returns.
		comp::game::session_report::start();
		// The crash log's buffers and writer thread.
		comp::game::crash_log::start();

		// Before the Remix runtime starts and reads its weather preset folder.
		comp::game::weather::bind_presets();

		// Before the d3d9 chain is loaded.
		d3d9_proxy::load_preload_dlls();

		if (!d3d9_proxy::init())
			return TRUE;

		if (const auto MH_INIT_STATUS = MH_Initialize(); MH_INIT_STATUS != MH_STATUS::MH_OK)
		{
			shared::common::log("Main", std::format("MinHook failed to initialize with code: {:d}", static_cast<int>(MH_INIT_STATUS)), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return TRUE;
		}

		comp::game::init_game_addresses();

		if (const auto t = CreateThread(nullptr, 0, comp::find_game_window, nullptr, 0, nullptr); t) {
			CloseHandle(t);
		}
	}

	return TRUE;
}
