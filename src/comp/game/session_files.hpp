#pragma once
#include <cwctype>

// Helpers shared by session_report.cpp and session_archive.cpp: paths,
// times, whole-file reads and writes, and the few JSON bits session.json
// needs.
namespace comp::game::session_files
{
	inline uint64_t ft64(const FILETIME& t)
	{
		return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
	}

	inline FILETIME ft(uint64_t v)
	{
		FILETIME t;
		t.dwLowDateTime = static_cast<DWORD>(v);
		t.dwHighDateTime = static_cast<DWORD>(v >> 32);
		return t;
	}

	inline uint64_t now_ft()
	{
		FILETIME t;
		GetSystemTimeAsFileTime(&t);
		return ft64(t);
	}

	constexpr uint64_t kFtSecond = 10000000ull;

	// A UTC file time in local time: "2026-10-20 12:34:56", or with
	// `for_file` "2026-10-20_12-34-56".
	inline std::string local_text(uint64_t utc, bool for_file = false)
	{
		const FILETIME f = ft(utc);
		SYSTEMTIME s = {}, l = {};
		if (!FileTimeToSystemTime(&f, &s) || !SystemTimeToTzSpecificLocalTime(nullptr, &s, &l)) return "unknown";
		if (for_file)
			return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute, l.wSecond);
		return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute, l.wSecond);
	}

	inline std::string utf8(const std::wstring& w)
	{
		if (w.empty()) return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
		if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
		return s;
	}

	inline std::wstring wide(const std::string& s)
	{
		if (s.empty()) return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
		if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
		return w;
	}

	// The game folder (FalloutNV.exe's), with a trailing backslash.
	inline const std::wstring& game_dir()
	{
		static const std::wstring dir = []
			{
				std::wstring exe(32768, L'\0');
				const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
				exe.resize(n < exe.size() ? n : 0);
				const size_t slash = exe.find_last_of(L"\\/");
				return slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1);
			}();
		return dir;
	}

	// The whole file, up to `max` bytes. Reads go through CreateFileW, so a
	// mod manager's virtual file system answers them as it does the game's.
	inline bool read_file(const std::wstring& path, std::string& out, size_t max)
	{
		out.clear();
		const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(h, &size) && static_cast<uint64_t>(size.QuadPart) <= max;
		if (ok)
		{
			out.resize(static_cast<size_t>(size.QuadPart));
			size_t got = 0;
			while (got < out.size())
			{
				DWORD n = 0;
				const DWORD want = static_cast<DWORD>((std::min)(out.size() - got, static_cast<size_t>(1u << 20)));
				if (!ReadFile(h, out.data() + got, want, &n, nullptr) || n == 0) break;
				got += n;
			}
			out.resize(got);
		}
		CloseHandle(h);
		return ok;
	}

	// Writes `data` to `path` through a temporary file, so a reader never
	// sees half a file.
	inline bool write_file(const std::wstring& path, const std::string& data)
	{
		const std::wstring tmp = path + L".tmp";
		const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		DWORD written = 0;
		const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
			written == data.size();
		CloseHandle(h);
		if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			DeleteFileW(tmp.c_str());
			return false;
		}
		return true;
	}

	// The file's last write time (UTC), 0 when it doesn't exist.
	inline uint64_t mtime(const std::wstring& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA a = {};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
		return ft64(a.ftLastWriteTime);
	}

	inline std::string json_escape(std::string_view s)
	{
		std::string o;
		o.reserve(s.size() + 2);
		for (const char c : s)
		{
			if (c == '"' || c == '\\') { o += '\\'; o += c; }
			else if (static_cast<unsigned char>(c) < 0x20) o += std::format("\\u{:04x}", static_cast<unsigned>(c));
			else o += c;
		}
		return o;
	}

	// `"key": "value"` in a session.json, unescaped; empty when absent.
	inline std::string json_string(const std::string& json, const char* key)
	{
		const std::string k = std::format("\"{}\": \"", key);
		size_t i = json.find(k);
		if (i == std::string::npos) return {};
		i += k.size();
		std::string v;
		for (; i < json.size() && json[i] != '"'; i++)
		{
			if (json[i] == '\\' && i + 1 < json.size()) i++;
			v += json[i];
		}
		return v;
	}

	// `"key": 123` in a session.json; 0 when absent.
	inline uint64_t json_u64(const std::string& json, const char* key)
	{
		const std::string k = std::format("\"{}\": ", key);
		const size_t i = json.find(k);
		if (i == std::string::npos) return 0;
		return _strtoui64(json.c_str() + i + k.size(), nullptr, 10);
	}

	inline std::string lower(std::string s)
	{
		for (char& c : s)
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		return s;
	}

	inline std::wstring lower(std::wstring s)
	{
		for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
		return s;
	}
}
