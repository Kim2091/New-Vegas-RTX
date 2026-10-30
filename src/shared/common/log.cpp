#include "std_include.hpp"

#include <atomic>
#include <unordered_map>

namespace shared::common
{
	namespace
	{
		// The file stops taking anything but errors at kMaxBytes, and errors
		// stop kErrorReserveBytes later.
		constexpr uint64_t kMaxBytes = 20ull << 20;
		constexpr uint64_t kErrorReserveBytes = 1ull << 20;
		// A kind is written freely this many times, then once per kRepeatMs.
		constexpr uint32_t kFreeLines = 10;
		constexpr uint32_t kFreeWarnLines = 50;
		constexpr uint64_t kRepeatMs = 5ull * 60ull * 1000ull;
		// Kinds beyond this many are not tracked and always written.
		constexpr size_t kMaxKinds = 4096;

		// The last lines, written or held back, as fixed slots.
		constexpr size_t kRecentLines = 64;
		constexpr size_t kRecentChars = 256;

		struct RepeatState
		{
			uint32_t written = 0;
			uint32_t held_back = 0;
			uint64_t last_write_ms = 0;
		};

		std::mutex g_mutex;
		HANDLE g_file = INVALID_HANDLE_VALUE;
		bool g_file_tried = false;
		bool g_keep_previous = false;
		bool g_announce_folder = false;
		bool g_console = false;
		uint64_t g_bytes = 0;
		bool g_cap_reached = false;
		std::unordered_map<std::string, RepeatState> g_kinds;

		char g_recent[kRecentLines][kRecentChars] = {};
		size_t g_recent_next = 0;
		size_t g_recent_count = 0;

		std::atomic<uint64_t> g_warnings{ 0 };
		std::atomic<uint64_t> g_errors{ 0 };
		std::atomic<uint64_t> g_held_back{ 0 };

		uint64_t now_ms()
		{
			return GetTickCount64();
		}

		// "[hh:mm:ss.mmm] " in local time, as the runtime and bridge logs.
		int time_prefix(char (&out)[32])
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			return sprintf_s(out, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		}

		void open_file()
		{
			if (g_file_tried) return;
			g_file_tried = true;

			const std::wstring& dir = log_dir();
			const std::wstring path = g_keep_previous
				? dir + std::format(L"remix-comp.{}.log", GetCurrentProcessId()) : dir + L"remix-comp.log";
			// The previous session's log stays as remix-comp.prev.log, so a
			// player who relaunches after a crash still has that session's log.
			if (!g_keep_previous)
				MoveFileExW(path.c_str(), (dir + L"remix-comp.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
			g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			g_announce_folder = g_file != INVALID_HANDLE_VALUE;
		}

		// The folder the file system really gave the open log file, in UTF-8.
		// A mod manager's virtual file system can put it outside the game folder.
		std::string real_folder()
		{
			wchar_t buf[1024] = {};
			const DWORD n = GetFinalPathNameByHandleW(g_file, buf, static_cast<DWORD>(std::size(buf)),
				FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			std::wstring path = n > 0 && n < std::size(buf) ? std::wstring(buf, n) : log_dir() + L"remix-comp.log";
			if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
			else if (path.starts_with(L"\\\\?\\")) path = path.substr(4);
			path = std::filesystem::path(path).parent_path().wstring();

			const int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0,
				nullptr, nullptr);
			std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
			if (len > 0)
				WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), out.data(), len, nullptr,
					nullptr);
			return out;
		}

		void write_raw(const char* text, size_t len)
		{
			if (g_file == INVALID_HANDLE_VALUE || len == 0) return;
			DWORD written = 0;
			WriteFile(g_file, text, static_cast<DWORD>(len), &written, nullptr);
			g_bytes += written;
		}

		// One formatted line to the file (and the console when it is open).
		void write_line(LOG_TYPE type, const std::string_view& module_str, const std::string_view& msg)
		{
			char prefix[32];
			const int n = time_prefix(prefix);
			std::string line;
			line.reserve(msg.size() + module_str.size() + 48);
			line.append(prefix, n);
			line += '[';
			line += log_type_to_string(type);
			line += "] [";
			line += module_str;
			line += "] ";
			line += msg;
			// Messages that end in a newline keep a single one.
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
			line += "\r\n";
			write_raw(line.data(), line.size());
		}

		void remember(const std::string_view& module_str, const std::string_view& msg)
		{
			char* slot = g_recent[g_recent_next];
			char prefix[32];
			const int n = time_prefix(prefix);
			_snprintf_s(slot, kRecentChars, _TRUNCATE, "%.*s[%.*s] %.*s", n, prefix,
				static_cast<int>(module_str.size()), module_str.data(), static_cast<int>(msg.size()), msg.data());
			g_recent_next = (g_recent_next + 1) % kRecentLines;
			if (g_recent_count < kRecentLines) g_recent_count++;
		}

		// "[Module] first second" without digits, a leading 0x value or
		// anything after '=', ':' (but the colon itself), '(' or ','.
		std::string kind_of(const std::string_view& module_str, const std::string_view& msg)
		{
			std::string kind(module_str);
			kind += '|';
			size_t i = 0;
			while (i < msg.size() && msg[i] == ' ') i++;
			for (int word = 0; word < 2 && i < msg.size(); word++)
			{
				if (msg.substr(i, 2) == "0x") break;
				size_t n = 0;
				for (; i < msg.size() && n < 24; i++, n++)
				{
					const char c = msg[i];
					if (c == ' ' || c == '=' || c == '(' || c == ',' || c == '\n') break;
					if (c == ':') { kind += c; i++; break; }
					if (c < '0' || c > '9') kind += c;
				}
				if (i >= msg.size() || msg[i] == '=' || msg[i] == '(' || msg[i] == ',') break;
				while (i < msg.size() && msg[i] == ' ') i++;
				kind += ' ';
			}
			return kind;
		}

		// Decides whether a line of this kind is written now; `note` gets the
		// held-back count when a line is written after a hold.
		bool admit(LOG_TYPE type, const std::string_view& module_str, const std::string_view& msg, std::string& note)
		{
			std::string kind = kind_of(module_str, msg);
			auto it = g_kinds.find(kind);
			if (it == g_kinds.end())
			{
				if (g_kinds.size() >= kMaxKinds) return true;
				it = g_kinds.emplace(std::move(kind), RepeatState{}).first;
			}
			RepeatState& st = it->second;
			const uint64_t now = now_ms();
			const bool warning = type == LOG_TYPE::LOG_TYPE_WARN || type == LOG_TYPE::LOG_TYPE_ERROR;
			const uint32_t free_lines = warning ? kFreeWarnLines : kFreeLines;
			if (st.written < free_lines)
			{
				st.written++;
				st.last_write_ms = now;
				return true;
			}
			if (now - st.last_write_ms >= kRepeatMs)
			{
				if (st.held_back)
					note = std::format(" [{} more lines of this kind held back since the last one]", st.held_back);
				st.written++;
				st.held_back = 0;
				st.last_write_ms = now;
				return true;
			}
			if (st.held_back == 0 && st.written == free_lines)
			{
				const std::string shown = it->first.substr(it->first.find('|') + 1);
				write_line(LOG_TYPE::LOG_TYPE_DEFAULT, "Log", std::format("'[{}] {}' lines are now written at most every "
					"{} minutes ([Logging] Verbose=1 writes all)", module_str, shown, kRepeatMs / 60000));
			}
			st.held_back++;
			return false;
		}

		void console_line(const std::string_view& module_str, const std::string_view& msg, LOG_TYPE type,
			bool highlight, bool newline_infront)
		{
			auto colorize = [](const LOG_TYPE& t, const bool h)
				{
					switch (t)
					{
					case LOG_TYPE::LOG_TYPE_DEFAULT: set_console_color_default(h); break;
					case LOG_TYPE::LOG_TYPE_STATUS:  set_console_color_blue(h); break;
					case LOG_TYPE::LOG_TYPE_GREEN:   set_console_color_green(h); break;
					case LOG_TYPE::LOG_TYPE_WARN:    set_console_color_yellow(h); break;
					case LOG_TYPE::LOG_TYPE_ERROR:   set_console_color_red(h); break;
					default: break;
					}
				};

			constexpr int inner_width = 14;
			std::cout << (newline_infront ? "\n" : "")
				<< std::setw(2) << (type == LOG_TYPE::LOG_TYPE_ERROR ? "!" : " ") << "[ ";
			colorize(type, true);
			std::cout << std::format("{:>{}}", module_str, inner_width);
			set_console_color_default();
			std::cout << " ]  ";
			colorize(type, highlight);
			std::cout << msg << '\n';
			set_console_color_default();
		}

		void emit(const std::string_view& module_str, const std::string_view& msg, LOG_TYPE type, bool highlight,
			bool newline_infront, bool limit)
		{
			if (type == LOG_TYPE::LOG_TYPE_WARN) g_warnings.fetch_add(1, std::memory_order_relaxed);
			else if (type == LOG_TYPE::LOG_TYPE_ERROR) g_errors.fetch_add(1, std::memory_order_relaxed);

			std::lock_guard lock(g_mutex);
			open_file();
			if (g_announce_folder)
			{
				g_announce_folder = false;
				const bool vfs = GetModuleHandleW(L"usvfs_x86.dll") != nullptr;
				write_line(LOG_TYPE::LOG_TYPE_STATUS, "Session", std::format("log folder: {}{}", real_folder(),
					vfs ? " (a virtual file system is active)" : ""));
			}
			remember(module_str, msg);

			std::string note;
			if (limit && !g_log_verbose && !admit(type, module_str, msg, note))
			{
				g_held_back.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			if (g_console) console_line(module_str, msg, type, highlight, newline_infront);

			if (g_cap_reached)
			{
				if (type != LOG_TYPE::LOG_TYPE_ERROR || g_bytes >= kMaxBytes + kErrorReserveBytes) return;
			}
			else if (g_bytes >= kMaxBytes)
			{
				g_cap_reached = true;
				write_line(LOG_TYPE::LOG_TYPE_ERROR, "Log",
					std::format("this log reached {} MB: from here on only errors are written", kMaxBytes >> 20));
				if (type != LOG_TYPE::LOG_TYPE_ERROR) return;
			}

			if (note.empty()) write_line(type, module_str, msg);
			else write_line(type, module_str, std::string(msg) + note);
		}
	}

	const std::wstring& log_dir()
	{
		static const std::wstring dir = []
			{
				std::wstring exe(32768, L'\0');
				const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
				exe.resize(n < exe.size() ? n : 0);
				const std::filesystem::path logs = std::filesystem::path(exe).parent_path() / L"rtx-remix" / L"logs";
				std::error_code ec;
				std::filesystem::create_directories(logs, ec);
				return logs.wstring() + L"\\";
			}();
		return dir;
	}

	void log_configure(bool verbose, bool console_window)
	{
		g_log_verbose = verbose;
		if (console_window)
		{
			console();
			g_console = true;
		}
	}

	void log_keep_previous()
	{
		std::lock_guard lock(g_mutex);
		g_keep_previous = true;
	}

	void log_disable()
	{
		std::lock_guard lock(g_mutex);
		g_file_tried = true;
	}

	void log(const std::string_view& module_str, const std::string_view& msg, LOG_TYPE type, bool highlight,
		bool newline_infront)
	{
		emit(module_str, msg, type, highlight, newline_infront, true);
	}

	void log_event(const std::string_view& module_str, const std::string_view& msg, LOG_TYPE type)
	{
		emit(module_str, msg, type, false, false, false);
	}

	uint64_t log_warning_count() { return g_warnings.load(std::memory_order_relaxed); }
	uint64_t log_error_count() { return g_errors.load(std::memory_order_relaxed); }
	uint64_t log_held_back_count() { return g_held_back.load(std::memory_order_relaxed); }

	void log_write_recent(const char* header, const char* const* details, size_t detail_count)
	{
		// The faulting thread may hold the lock (a fault inside the logger);
		// after a short wait the lines are written without it.
		bool locked = false;
		for (int i = 0; i < 50 && !(locked = g_mutex.try_lock()); i++) Sleep(2);

		if (g_file != INVALID_HANDLE_VALUE)
		{
			char line[1024];
			char prefix[32];
			time_prefix(prefix);
			// A cut line still ends in CRLF.
			auto write_crash_line = [&](const char* text)
				{
					int n = _snprintf_s(line, _TRUNCATE, "%s[ERROR] [Crash] %s\r\n", prefix, text);
					if (n < 0)
					{
						n = static_cast<int>(sizeof(line) - 1);
						line[n - 2] = '\r';
						line[n - 1] = '\n';
					}
					write_raw(line, static_cast<size_t>(n));
				};
			write_crash_line(header);
			for (size_t k = 0; details && k < detail_count; k++)
				if (details[k] && details[k][0]) write_crash_line(details[k]);
			int len = sprintf_s(line, "%s[ERROR] [Crash] the last %u log lines, written or held back:\r\n", prefix,
				static_cast<unsigned>(g_recent_count));
			if (len > 0) write_raw(line, static_cast<size_t>(len));
			const size_t first = (g_recent_next + kRecentLines - g_recent_count) % kRecentLines;
			for (size_t k = 0; k < g_recent_count; k++)
			{
				len = sprintf_s(line, "    %s\r\n", g_recent[(first + k) % kRecentLines]);
				if (len > 0) write_raw(line, static_cast<size_t>(len));
			}
			FlushFileBuffers(g_file);
		}

		if (locked) g_mutex.unlock();
	}

	void log_session_end()
	{
		bool locked = false;
		for (int i = 0; i < 25 && !(locked = g_mutex.try_lock()); i++) Sleep(2);
		if (g_file != INVALID_HANDLE_VALUE)
		{
			char line[160];
			char prefix[32];
			time_prefix(prefix);
			const int len = sprintf_s(line, "%s[STATUS] [Main] the process is exiting (a log without this line ended "
				"in a crash or a forced kill)\r\n", prefix);
			if (len > 0) write_raw(line, static_cast<size_t>(len));
		}
		if (locked) g_mutex.unlock();
	}
}
