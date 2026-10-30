#include "common.hpp"
#include <cstdio>
#include <io.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fnvr::log
{
	namespace
	{
		constexpr uint64_t kMaxBytes = 20ull << 20;
		constexpr uint32_t kFreeLines = 10;
		constexpr uint64_t kRepeatMs = 5ull * 60ull * 1000ull;
		constexpr size_t kMaxKinds = 4096;

		struct RepeatState
		{
			uint32_t written = 0;
			uint32_t held_back = 0;
			uint64_t last_write_ms = 0;
		};

		std::mutex g_lock;
		FILE* g_file = nullptr;
		std::wstring g_dir;
		uint64_t g_bytes = 0;
		bool g_cap_reached = false;
		bool g_verbose = false;
		uint64_t g_held_back = 0;
		std::unordered_map<std::string, RepeatState> g_kinds;

		// The tag and the text's first two words, without digits, a leading
		// 0x value or anything after '=', '(' or ','.
		std::string kind_of(std::string_view tag, std::string_view text)
		{
			std::string kind(tag);
			kind += '|';
			size_t i = 0;
			while (i < text.size() && text[i] == ' ') i++;
			for (int word = 0; word < 2 && i < text.size(); word++)
			{
				if (text.substr(i, 2) == "0x") break;
				for (size_t n = 0; i < text.size() && n < 24; i++, n++)
				{
					const char c = text[i];
					if (c == ' ' || c == '=' || c == '(' || c == ',') break;
					if (c == ':') { kind += c; i++; break; }
					if (c < '0' || c > '9') kind += c;
				}
				if (i >= text.size() || text[i] == '=' || text[i] == '(' || text[i] == ',') break;
				while (i < text.size() && text[i] == ' ') i++;
				kind += ' ';
			}
			return kind;
		}

		void write(std::string_view tag, std::string_view text)
		{
			if (!g_file || g_cap_reached) return;
			SYSTEMTIME t;
			GetLocalTime(&t);
			std::string s = std::format("[{:02}:{:02}:{:02}.{:03}] [{}] {}\n", t.wHour, t.wMinute, t.wSecond,
				t.wMilliseconds, tag, text);
			if (g_bytes + s.size() > kMaxBytes)
			{
				g_cap_reached = true;
				s = std::format("[{:02}:{:02}:{:02}.{:03}] [Log] this log reached {} MB: nothing more is written\n",
					t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, kMaxBytes >> 20);
			}
			std::fwrite(s.data(), 1, s.size(), g_file);
			std::fflush(g_file);
			g_bytes += s.size();
		}

		// Whether a line of this kind is written now; `note` gets the
		// held-back count when a line is written after a hold.
		bool admit(std::string_view tag, std::string_view text, std::string& note)
		{
			std::string kind = kind_of(tag, text);
			auto it = g_kinds.find(kind);
			if (it == g_kinds.end())
			{
				if (g_kinds.size() >= kMaxKinds) return true;
				it = g_kinds.emplace(std::move(kind), RepeatState{}).first;
			}
			RepeatState& st = it->second;
			const uint64_t now = GetTickCount64();
			if (st.written < kFreeLines || now - st.last_write_ms >= kRepeatMs)
			{
				if (st.held_back)
					note = std::format(" [{} more lines of this kind held back since the last one]", st.held_back);
				st.written++;
				st.held_back = 0;
				st.last_write_ms = now;
				return true;
			}
			if (st.held_back == 0 && st.written == kFreeLines)
				write("Log", std::format("'[{}] {}' lines are now written at most every {} minutes ([Logging] Verbose=1 "
					"writes all)", tag, it->first.substr(it->first.find('|') + 1), kRepeatMs / 60000));
			st.held_back++;
			g_held_back++;
			return false;
		}

		// <game folder>\rtx-remix\logs\ with a trailing backslash, created if
		// missing. The game folder is the exe's folder, where the runtime and
		// the wrapper write their logs too.
		std::wstring log_dir()
		{
			std::wstring exe(32768, L'\0');
			const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
			exe.resize(n < exe.size() ? n : 0);
			const size_t slash = exe.find_last_of(L"\\/");
			std::wstring dir = slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1);
			dir += L"rtx-remix\\";
			CreateDirectoryW(dir.c_str(), nullptr);
			dir += L"logs\\";
			CreateDirectoryW(dir.c_str(), nullptr);
			return dir;
		}

		// The folder the file system really gave the open log file, in UTF-8.
		// A mod manager's virtual file system can put it outside the game
		// folder. Returns `fallback` when the path cannot be read.
		std::string real_folder(const std::wstring& fallback)
		{
			std::wstring path = fallback;
			const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(g_file)));
			wchar_t buf[1024] = {};
			const DWORD n = h != INVALID_HANDLE_VALUE
				? GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
				: 0;
			if (n > 0 && n < std::size(buf))
			{
				path.assign(buf, n);
				if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
				else if (path.starts_with(L"\\\\?\\")) path = path.substr(4);
				const size_t slash = path.find_last_of(L'\\');
				if (slash != std::wstring::npos) path.resize(slash);
			}

			const int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0,
				nullptr, nullptr);
			std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
			if (len > 0)
				WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), out.data(), len, nullptr,
					nullptr);
			return out;
		}
	}

	void open(HMODULE self)
	{
		wchar_t path[MAX_PATH] = {};
		const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
		std::wstring p(path, n);
		const size_t slash = p.find_last_of(L"\\/");
		g_dir = slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
		std::lock_guard lk(g_lock);
		if (!g_file)
		{
			const std::wstring logs = log_dir();
			// The previous session's log stays as fnv_remix_nvse.prev.log, so a
			// player who relaunches after a crash still has that session's log.
			MoveFileExW((logs + L"fnv_remix_nvse.log").c_str(), (logs + L"fnv_remix_nvse.prev.log").c_str(),
				MOVEFILE_REPLACE_EXISTING);
			g_file = _wfsopen((logs + L"fnv_remix_nvse.log").c_str(), L"w", _SH_DENYWR);
			if (g_file)
			{
				const bool vfs = GetModuleHandleW(L"usvfs_x86.dll") != nullptr;
				write("Plugin", std::format("log folder: {}{}", real_folder(logs),
					vfs ? " (a virtual file system is active)" : ""));
			}
		}
	}

	const std::wstring& dir()
	{
		return g_dir;
	}

	void line(std::string_view tag, std::string_view text)
	{
		std::lock_guard lk(g_lock);
		std::string note;
		if (!g_verbose && !admit(tag, text, note)) return;
		if (note.empty()) write(tag, text);
		else write(tag, std::string(text) + note);
	}

	void event(std::string_view tag, std::string_view text)
	{
		std::lock_guard lk(g_lock);
		write(tag, text);
	}

	void set_verbose(bool on)
	{
		std::lock_guard lk(g_lock);
		g_verbose = on;
	}

	bool verbose()
	{
		return g_verbose;
	}

	uint64_t held_back()
	{
		std::lock_guard lk(g_lock);
		return g_held_back;
	}
}
