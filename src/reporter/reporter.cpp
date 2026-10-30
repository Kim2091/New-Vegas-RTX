#include "std_include.hpp"

#include <commctrl.h>
#include <tlhelp32.h>

#include "comp/game/hang_dump.hpp"
#include "comp/game/session_files.hpp"
#include "comp/game/session_zip.hpp"

// Task dialogs need version 6 of the common controls.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// NewVegasRTX-Reporter.exe: the wrapper starts it once per game process
// (session_report.cpp, [SessionReport] Reporter). It waits for the game
// process to end, however it ends, gives the Remix bridge server up to
// kBridgeWaitMs to write its last lines, and zips the session the way the
// next start would (session_zip.hpp says who zips when). When the end was a
// crash or a hang, it shows the zip's path. It never changes the game: it
// waits on its process handle, reads it only to write a hang dump the game
// asks for (hang_dump.hpp), and reads and writes rtx-remix\logs.
//
//   NewVegasRTX-Reporter.exe --pid <game process id> --created <its creation
//     file time> --logs <rtx-remix\logs> --game <game folder> --version <tag>
//     --scrub 0|1 --crash-folder <CrashLogFolder>
namespace
{
	using namespace comp::game;
	using namespace comp::game::session_files;

	constexpr DWORD kBridgeWaitMs = 10000;
	constexpr int kShowFile = 100;
	constexpr int kCopyPath = 101;

	struct Args
	{
		DWORD pid = 0;
		uint64_t created = 0;
		std::wstring logs;          // with a trailing backslash
		std::wstring game;          // with a trailing backslash
		std::string version;
		bool scrub = true;
		std::string crash_folder;
	};

	bool parse_args(Args& a)
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv) return false;
		for (int i = 1; i + 1 < argc; i += 2)
		{
			const std::wstring key = argv[i];
			const std::wstring value = argv[i + 1];
			if (key == L"--pid") a.pid = static_cast<DWORD>(wcstoul(value.c_str(), nullptr, 10));
			else if (key == L"--created") a.created = _wcstoui64(value.c_str(), nullptr, 10);
			else if (key == L"--logs") a.logs = value + L"\\";
			else if (key == L"--game") a.game = value + L"\\";
			else if (key == L"--version") a.version = utf8(value);
			else if (key == L"--scrub") a.scrub = value != L"0";
			else if (key == L"--crash-folder") a.crash_folder = utf8(value);
		}
		LocalFree(argv);
		return a.pid && a.created && a.logs.size() > 1 && a.game.size() > 1;
	}

	// The game process, when the id still names it (its creation time matches),
	// opened with `access` on top of what waiting on it needs.
	HANDLE open_game(const Args& a, DWORD access = 0)
	{
		const HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | access, FALSE, a.pid);
		if (!h) return nullptr;
		FILETIME created, exited, kernel, user;
		if (!GetProcessTimes(h, &created, &exited, &kernel, &user) || ft64(created) != a.created)
		{
			CloseHandle(h);
			return nullptr;
		}
		return h;
	}

	// Waits up to kBridgeWaitMs for the processes the game started (the
	// Remix bridge server, which writes its last lines when the game is
	// gone). Returns the milliseconds waited.
	uint64_t wait_for_children(const Args& a)
	{
		const uint64_t t0 = GetTickCount64();
		std::vector<HANDLE> children;
		const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			PROCESSENTRY32W pe = {};
			pe.dwSize = sizeof(pe);
			for (BOOL more = Process32FirstW(snap, &pe); more && children.size() < static_cast<size_t>(MAXIMUM_WAIT_OBJECTS);
				more = Process32NextW(snap, &pe))
			{
				if (pe.th32ParentProcessID != a.pid || pe.th32ProcessID == GetCurrentProcessId()) continue;
				const HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
				if (!h) continue;
				// One started before the game has an earlier process with the same id as its parent.
				FILETIME created, exited, kernel, user;
				if (GetProcessTimes(h, &created, &exited, &kernel, &user) && ft64(created) >= a.created) children.push_back(h);
				else CloseHandle(h);
			}
			CloseHandle(snap);
		}
		if (!children.empty())
			WaitForMultipleObjects(static_cast<DWORD>(children.size()), children.data(), TRUE, kBridgeWaitMs);
		for (const HANDLE h : children) CloseHandle(h);
		return GetTickCount64() - t0;
	}

	// One line at the end of the session's remix-comp log `path`, in its format.
	void log_line(const std::wstring& path, const char* type, const std::string& msg)
	{
		const HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		const std::string line = std::format("[{:02}:{:02}:{:02}.{:03}] [{}] [Reporter] {}\r\n", t.wHour, t.wMinute, t.wSecond,
			t.wMilliseconds, type, msg);
		DWORD written = 0;
		WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
		CloseHandle(h);
	}

	// ---- Hang dumps (hang_dump.hpp) ----

	struct DumpService
	{
		DWORD pid = 0;
		HANDLE game = nullptr;
		hang_dump::Shared* view = nullptr;
		HANDLE request = nullptr;
		HANDLE done = nullptr;
	};

	// Writes a dump of the game each time it asks, until it ends.
	DWORD WINAPI dump_thread(LPVOID param)
	{
		auto* s = static_cast<DumpService*>(param);
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, void*, void*, void*);
		WriteDumpFn write_dump = nullptr;
		if (const HMODULE m = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
			write_dump = reinterpret_cast<WriteDumpFn>(GetProcAddress(m, "MiniDumpWriteDump"));
		const DWORD load_error = write_dump ? 0 : GetLastError();

		const HANDLE wait[] = { s->request, s->game };
		while (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0)
		{
			const LONG request = InterlockedCompareExchange(&s->view->request, 0, 0);
			wchar_t path[MAX_PATH];
			wcsncpy_s(path, s->view->path, _TRUNCATE);
			bool ok = false;
			DWORD error = load_error;
			if (write_dump)
			{
				const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file != INVALID_HANDLE_VALUE)
				{
					ok = write_dump(s->game, s->pid, file, hang_dump::kDumpType, nullptr, nullptr, nullptr) != FALSE;
					error = ok ? 0 : GetLastError();
					CloseHandle(file);
					if (!ok) DeleteFileW(path);
				}
				else error = GetLastError();
			}
			s->view->ok = ok ? 1 : 0;
			s->view->error = error;
			InterlockedExchange(&s->view->done, request);
			SetEvent(s->done);
		}
		return 0;
	}

	// Serves the game's hang dump channel, when the game made one (its hang
	// watchdog is on).
	void start_dump_service(const Args& a)
	{
		static DumpService s;
		wchar_t name[96];
		hang_dump::object_name(name, a.pid, a.created, L"map");
		const HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
		if (!map) return;
		s.view = static_cast<hang_dump::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hang_dump::Shared)));
		hang_dump::object_name(name, a.pid, a.created, L"request");
		s.request = OpenEventW(SYNCHRONIZE, FALSE, name);
		hang_dump::object_name(name, a.pid, a.created, L"done");
		s.done = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
		s.game = open_game(a, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ);
		s.pid = a.pid;
		if (!s.view || s.view->version != hang_dump::kVersion || !s.request || !s.done || !s.game) return;
		if (const HANDLE t = CreateThread(nullptr, 0, dump_thread, &s, 0, nullptr)) CloseHandle(t);
	}

	// ---- The window ----

	struct Dialog
	{
		std::wstring path;
	};

	void show_file(const std::wstring& path)
	{
		const std::wstring args = L"/select,\"" + path + L"\"";
		ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
	}

	void copy_text(HWND owner, const std::wstring& text)
	{
		if (!OpenClipboard(owner)) return;
		EmptyClipboard();
		const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
		bool placed = false;
		if (const HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
		{
			if (void* p = GlobalLock(mem))
			{
				memcpy(p, text.c_str(), bytes);
				GlobalUnlock(mem);
				placed = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
			}
			if (!placed) GlobalFree(mem);
		}
		CloseClipboard();
	}

	HRESULT CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM, LONG_PTR data)
	{
		const auto* d = reinterpret_cast<const Dialog*>(data);
		if (msg == TDN_CREATED) SetForegroundWindow(hwnd);
		else if (msg == TDN_BUTTON_CLICKED && wparam == static_cast<WPARAM>(kShowFile))
		{
			show_file(d->path);
			return S_FALSE;     // the window stays open
		}
		else if (msg == TDN_BUTTON_CLICKED && wparam == static_cast<WPARAM>(kCopyPath))
		{
			copy_text(hwnd, d->path);
			return S_FALSE;
		}
		return S_OK;
	}

	using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

	// "New Vegas RTX closed unexpectedly." with the zip's path and the
	// buttons Show File, Copy Path and Close.
	void show_dialog(const std::wstring& path, bool folder_unknown)
	{
		Dialog d{ path };
		std::wstring text = L"A report of this session is ready to upload:\n\n" + path;
		if (folder_unknown)
			text += L"\n\nMod Organizer 2 runs the game: if Explorer does not show the file there, look in Mod Organizer "
				L"2's Overwrite folder.";

		const HMODULE comctl = LoadLibraryW(L"comctl32.dll");
		const auto task_dialog = comctl
			? reinterpret_cast<TaskDialogIndirectFn>(GetProcAddress(comctl, "TaskDialogIndirect")) : nullptr;
		if (task_dialog)
		{
			const TASKDIALOG_BUTTON buttons[] = { { kShowFile, L"Show File" }, { kCopyPath, L"Copy Path" } };
			TASKDIALOGCONFIG c = {};
			c.cbSize = sizeof(c);
			c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
			c.dwCommonButtons = TDCBF_CLOSE_BUTTON;
			c.pszWindowTitle = L"New Vegas RTX";
			c.pszMainIcon = TD_WARNING_ICON;
			c.pszMainInstruction = L"New Vegas RTX closed unexpectedly.";
			c.pszContent = text.c_str();
			c.cButtons = static_cast<UINT>(std::size(buttons));
			c.pButtons = buttons;
			c.nDefaultButton = kShowFile;
			c.pfCallback = dialog_proc;
			c.lpCallbackData = reinterpret_cast<LONG_PTR>(&d);
			if (SUCCEEDED(task_dialog(&c, nullptr, nullptr, nullptr))) return;
		}
		const std::wstring plain = L"New Vegas RTX closed unexpectedly.\n\n" + text + L"\n\nShow the file in Explorer?";
		if (MessageBoxW(nullptr, plain.c_str(), L"New Vegas RTX", MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND) == IDYES)
			show_file(path);
	}

	int run()
	{
		Args a;
		if (!parse_args(a)) return 1;

		// One reporter per game process.
		const std::wstring once_name = std::format(L"Local\\NewVegasRTX-Reporter-{}-{}", a.pid, a.created);
		const HANDLE once = CreateMutexW(nullptr, FALSE, once_name.c_str());
		if (!once || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

		start_dump_service(a);

		int64_t exit_code = -1;
		if (const HANDLE game = open_game(a))
		{
			WaitForSingleObject(game, INFINITE);
			DWORD code = 0;
			if (GetExitCodeProcess(game, &code)) exit_code = code;
			CloseHandle(game);
		}
		const uint64_t waited_ms = wait_for_children(a);

		// reporter.busy first, then the lock: a game that starts now waits
		// for the lock instead of taking the session files over.
		const HANDLE busy = session_zip::mark_reporter_busy(a.logs);
		const HANDLE lock = session_zip::take_lock(a.logs);
		auto release = [&]
			{
				if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
				if (busy != INVALID_HANDLE_VALUE) CloseHandle(busy);
			};
		// A new game session took the lock first: it zips this one as its previous session.
		if (lock == INVALID_HANDLE_VALUE)
		{
			release();
			return 0;
		}

		std::string json;
		read_file(a.logs + L"session.json", json, 1u << 20);
		// The files are another game instance's.
		if (json_u64(json, "pid") != a.pid)
		{
			release();
			return 0;
		}
		const uint64_t start = json_u64(json, "start_filetime");
		const std::wstring log = a.logs + L"remix-comp.log";
		log_line(log, "STATUS", std::format("the game process ended (exit code {}); waited {:.1f} s for the processes it "
			"started", exit_code < 0 ? std::string("unknown") : std::format("0x{:08X}", static_cast<uint32_t>(exit_code)),
			static_cast<double>(waited_ms) / 1000.0));

		// Zipped when the game quit, and nothing went wrong after that.
		if (const std::string done = session_zip::archived(a.logs, start); !done.empty() && !session_zip::is_exception_exit(exit_code))
		{
			log_line(log, "STATUS", std::format("this session was zipped when the game quit: rtx-remix\\logs\\archive\\{}", done));
			release();
			return 0;
		}

		// The session's files move to unarchived\<start>\ under the lock (a
		// few renames), so a game started now finds the plain names free and
		// never waits for the zip. unarchived\<start>.zipping tells the next
		// start's archive thread to wait for this zip.
		const uint64_t end = now_ft() + 60 * kFtSecond;
		const std::wstring held = session_zip::held_folder(a.logs, start);
		const HANDLE zipping = session_zip::mark_zipping(held);
		if (zipping == INVALID_HANDLE_VALUE)
		{
			release();
			return 0;
		}
		session_zip::hold_current(a.logs, held, end);
		release();

		session_zip::Settings s;
		s.logs = a.logs;
		s.game = a.game;
		s.crash_log_folder = a.crash_folder;
		s.scrub_user_name = a.scrub;
		s.version = a.version;
		session_zip::Request r;
		r.src.dir = held;
		r.src.files = session_zip::Files::Prev;
		r.src.json = json;
		r.src.start = start;
		r.src.end = end;
		r.what = "this session";
		r.exit_code = exit_code;
		session_zip::Result res;
		try
		{
			res = session_zip::zip(s, r);
		}
		catch (...)
		{
			res.warn = true;
			res.line = "zipping this session failed: the next start zips it";
		}
		// Its files go back to their *.prev names; a failed zip leaves them
		// held for the next start.
		log_line(held + L"remix-comp.prev.log", res.warn ? "WARN" : "STATUS", res.line);
		if (res.ok) session_zip::unhold(held, a.logs);
		CloseHandle(zipping);

		// No window for a quit, even one that crashed on the way out.
		const bool bad = res.end == "crash" || res.end == "hang";
		if (!res.ok || !bad || json_string(json, "end") == "exiting") return 0;
		bool unknown = false;
		show_dialog(session_zip::real_path(res.path, unknown), unknown);
		return 0;
	}
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
	return run();
}
