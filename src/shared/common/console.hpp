#pragma once
#include "shared/globals.hpp"

namespace shared::common
{
	inline bool g_external_console_created = false;
    inline void console()
    {
        if (!g_external_console_created)
        {
			g_external_console_created = true;
            
            setvbuf(stdout, nullptr, _IONBF, 0);
            if (AllocConsole())
            {
                FILE* file = nullptr;
                freopen_s(&file, "CONIN$", "r", stdin);
                freopen_s(&file, "CONOUT$", "w", stdout);
                freopen_s(&file, "CONOUT$", "w", stderr);
                SetConsoleTitleA("RTX-Comp Debug Console");
            }

			HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
			CONSOLE_SCREEN_BUFFER_INFO info;
			GetConsoleScreenBufferInfo(hOut, &info);

			SHORT newWidth = 500;
			SHORT newHeight = std::max((SHORT)(info.srWindow.Bottom + 1), (SHORT)300);

			// Shrink the window first or SetConsoleScreenBufferSize fails.
			SMALL_RECT rect = { 0, 0, 1, 1 };
			SetConsoleWindowInfo(hOut, TRUE, &rect);

			COORD newSize = { newWidth, newHeight };
			SetConsoleScreenBufferSize(hOut, newSize);

			rect = { 0, 0, (SHORT)(120 - 1), (SHORT)(40 - 1) };
			SetConsoleWindowInfo(hOut, TRUE, &rect);
        }
    }

	inline void set_console_color_red(bool highlight = false)
	{
		if (g_external_console_created) 
		{
			WORD color = FOREGROUND_RED;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	inline void set_console_color_green(bool highlight = false)
	{
		if (g_external_console_created) 
		{
			WORD color = FOREGROUND_GREEN;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	inline void set_console_color_blue(bool highlight = false)
	{
		if (g_external_console_created) 
		{
			WORD color = FOREGROUND_BLUE;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	inline void set_console_color_yellow(bool highlight = false)
	{
		if (g_external_console_created)
		{
			WORD color = FOREGROUND_RED | FOREGROUND_GREEN;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	inline void set_console_color_default(bool highlight = false)
	{
		if (g_external_console_created)
		{
			WORD color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
			if (highlight) {
				color |= FOREGROUND_INTENSITY;
			}
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
		}
	}

	enum class LOG_TYPE
	{
		LOG_TYPE_DEFAULT,
		LOG_TYPE_STATUS,
		LOG_TYPE_GREEN,
		LOG_TYPE_WARN,
		LOG_TYPE_ERROR,
    };

	inline const char* log_type_to_string(LOG_TYPE type)
	{
		switch (type)
		{
		case LOG_TYPE::LOG_TYPE_DEFAULT: return "INFO";
		case LOG_TYPE::LOG_TYPE_STATUS:  return "STATUS";
		case LOG_TYPE::LOG_TYPE_GREEN:   return "OK";
		case LOG_TYPE::LOG_TYPE_WARN:    return "WARN";
		case LOG_TYPE::LOG_TYPE_ERROR:   return "ERROR";
		default:                         return "UNKNOWN";
		}
	}

	// The wrapper log, rtx-remix\logs\remix-comp.log (shared/common/log.cpp).
	// Every line carries the local time, as the runtime and bridge logs do. The
	// previous session's file is kept as remix-comp.prev.log. The first line
	// names the folder the file really is in, since a mod manager's virtual
	// file system can move it out of the game folder.
	//
	// With [Logging] Verbose=0 a line kind ([Module] plus its first two words)
	// is written for its first 10 lines (warnings and errors: 50), then at most
	// once every 5 minutes with a count of the lines held back. Verbose=1
	// writes every line, plus the diagnostics gated on log_verbose(). Past
	// 20 MB only errors are written. The last 64 lines, written or held back,
	// stay in memory for log_write_recent (the crash filter in
	// comp/game/session_log.cpp).

	inline bool g_log_verbose = false;

	// The folder all logs go to (the runtime's and the bridge's too):
	// <game folder>\rtx-remix\logs\ with a trailing backslash. The game folder
	// is the exe's folder. The folder is created on the first call.
	const std::wstring& log_dir();

	// [Logging] Verbose: dev diagnostics that are off in a release log.
	inline bool log_verbose() { return g_log_verbose; }

	// Called once from DllMain after the config is read.
	void log_configure(bool verbose, bool console_window);

	// No log file for this process: the file is never opened, so the last
	// session's remix-comp.log is not moved to remix-comp.prev.log. Called
	// from DllMain before any line is logged.
	void log_disable();

	// remix-comp.log is not touched: this process writes
	// remix-comp.<process id>.log instead (another process still owns the
	// previous session's files). Called from DllMain before any line is logged.
	void log_keep_previous();

	void log(const std::string_view& module_str, const std::string_view& msg, LOG_TYPE type = LOG_TYPE::LOG_TYPE_DEFAULT,
		bool highlight = false, bool newline_infront = false);

	// A line the repeat limit never holds back: state changes, the startup
	// report and lines their caller already rate-limits.
	void log_event(const std::string_view& module_str, const std::string_view& msg,
		LOG_TYPE type = LOG_TYPE::LOG_TYPE_STATUS);

	// Warnings and errors since startup (held-back ones included), and the
	// lines the repeat limit held back.
	uint64_t log_warning_count();
	uint64_t log_error_count();
	uint64_t log_held_back_count();

	// Writes the header line, then the detail lines, then the in-memory last
	// lines. Safe to call from an exception filter: it takes the log lock only
	// if it is free within a short wait and allocates nothing. Long lines are
	// cut.
	void log_write_recent(const char* header, const char* const* details = nullptr, size_t detail_count = 0);

	// The last line of a session whose process exits (DLL_PROCESS_DETACH); a
	// log without it ended in a crash or a forced kill.
	void log_session_end();
}
