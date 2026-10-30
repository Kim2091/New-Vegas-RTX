#include "std_include.hpp"
#include "session_zip.hpp"

#include <algorithm>
#include <shlobj.h>

#include "session_files.hpp"
#include "zip_store.hpp"

namespace comp::game::session_zip
{
	using namespace session_files;

	namespace
	{
		// A file written up to this long before the recorded start still
		// belongs to the session (the start is taken in DllMain, after the
		// game's launcher may have written).
		constexpr uint64_t kSlack = 60 * kFtSecond;
		constexpr size_t kMaxText = 64u << 20;
		constexpr size_t kMaxDump = 128u << 20;
		constexpr size_t kPiece = 1u << 20;

		const wchar_t* log_suffix(Files f)
		{
			return f == Files::Prev ? L".prev.log" : L".log";
		}

		// Written during the session: not before its start (when known), not
		// at or after the next one's.
		bool in_window(const Source& s, uint64_t t)
		{
			return t && (!s.start || t + kSlack >= s.start) && t < s.end;
		}

		// A component log is the session's when it is not older than its
		// start; its owner may still have written to it after the next
		// session began.
		bool own_log_of(const Source& s, uint64_t t)
		{
			return t && (!s.start || t + kSlack >= s.start);
		}

		bool ends_with(const std::wstring& s, const wchar_t* suffix)
		{
			const size_t n = wcslen(suffix);
			return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
		}

		// ---- The user name scrub (the zip's copies only) ----

		struct Scrub
		{
			std::vector<std::string> text;  // replaced by <user> in text files, longest first
			std::wstring name;              // the user name, masked in dumps
		};

		Scrub make_scrub()
		{
			Scrub s;
			wchar_t profile[MAX_PATH] = {};
			if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, SHGFP_TYPE_CURRENT, profile)) && profile[0])
			{
				const std::string p = utf8(profile);
				std::string fwd = p, json;
				std::replace(fwd.begin(), fwd.end(), '\\', '/');
				for (const char c : p)
				{
					json += c;
					if (c == '\\') json += '\\';
				}
				s.text = { json, p, fwd };
			}
			wchar_t user[256] = {};
			DWORD n = static_cast<DWORD>(std::size(user));
			if (GetUserNameW(user, &n) && user[0]) s.name = user;
			// A name this short would hit ordinary words.
			if (s.name.size() >= 3) s.text.push_back(utf8(s.name));
			std::sort(s.text.begin(), s.text.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
			return s;
		}

		char ascii_lower(char c)
		{
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		}

		bool starts_ci(const char* p, size_t left, const std::string& needle)
		{
			if (needle.empty() || needle.size() > left) return false;
			for (size_t i = 0; i < needle.size(); i++)
				if (ascii_lower(p[i]) != ascii_lower(needle[i])) return false;
			return true;
		}

		// The bytes a match can need past its first one: a piece holds them
		// back for the next.
		size_t text_hold(const Scrub& s)
		{
			return s.text.empty() ? 0 : s.text.front().size() - 1;
		}

		size_t dump_hold(const Scrub& s)
		{
			return s.name.size() < 3 ? 0 : 2 * s.name.size() - 1;
		}

		// The names in `in` from its start up to `limit` (a match may run
		// past it) replaced by <user>, appended to `out`. Returns where it
		// stopped.
		size_t scrub_text(const std::string& in, size_t limit, const Scrub& s, std::string& out)
		{
			size_t i = 0;
			while (i < limit && i < in.size())
			{
				bool hit = false;
				for (const std::string& n : s.text)
				{
					if (!starts_ci(in.data() + i, in.size() - i, n)) continue;
					out += "<user>";
					i += n.size();
					hit = true;
					break;
				}
				if (!hit) out += in[i++];
			}
			return i;
		}

		// The user name in a dump (module paths, environment), as 8-bit and
		// UTF-16 text, overwritten with as many 'x', at the positions before
		// `limit`: the dump's layout stays the same.
		void mask_dump(std::string& d, size_t limit, const Scrub& s)
		{
			if (s.name.size() < 3) return;
			const std::wstring& w = s.name;
			bool ascii = true;
			for (const wchar_t c : w)
				if (c >= 0x80) ascii = false;
			const size_t n = w.size();
			for (size_t i = 0; i < limit && i < d.size(); i++)
			{
				if (ascii && i + n <= d.size())
				{
					size_t k = 0;
					while (k < n && ascii_lower(d[i + k]) == ascii_lower(static_cast<char>(w[k]))) k++;
					if (k == n)
					{
						for (k = 0; k < n; k++) d[i + k] = 'x';
						continue;
					}
				}
				if (i + 2 * n <= d.size())
				{
					size_t k = 0;
					for (; k < n; k++)
					{
						const wchar_t c = static_cast<wchar_t>(static_cast<uint8_t>(d[i + 2 * k]) |
							(static_cast<uint8_t>(d[i + 2 * k + 1]) << 8));
						if (towlower(c) != towlower(w[k])) break;
					}
					if (k == n)
						for (k = 0; k < n; k++)
						{
							d[i + 2 * k] = 'x';
							d[i + 2 * k + 1] = '\0';
						}
				}
			}
		}

		// ---- What goes into the zip ----

		enum class Kind : uint8_t { Text, Binary, Dump, CrashLog };

		struct Item
		{
			std::string zip_name;
			std::wstring path;
			Kind kind;
			uint64_t mtime;
		};

		std::wstring crash_log_dir(const Settings& s)
		{
			if (s.crash_log_folder.empty()) return {};
			std::wstring w = wide(s.crash_log_folder);
			const bool absolute = (w.size() > 1 && w[1] == L':') || w.starts_with(L"\\\\");
			if (!absolute) w = s.game + w;
			if (w.back() != L'\\' && w.back() != L'/') w += L'\\';
			return w;
		}

		// The session's own files from `src.dir`, and the files in the logs
		// folder written during it.
		std::vector<Item> collect(const Source& src, const Settings& s)
		{
			const std::wstring& dir = s.logs;
			const bool prev = src.files == Files::Prev;
			const wchar_t* suffix = log_suffix(src.files);
			const size_t suffix_len = wcslen(suffix);
			std::vector<Item> items;
			for (const wchar_t* name : kLogs)
			{
				const std::wstring path = src.dir + name + suffix;
				const uint64_t t = mtime(path);
				if (own_log_of(src, t)) items.push_back({ utf8(name) + ".log", path, Kind::Text, t });
				for (const std::wstring& pid : pid_logs(src.dir, name, suffix))
				{
					const uint64_t pt = mtime(src.dir + pid);
					if (own_log_of(src, pt))
						items.push_back({ utf8(pid.substr(0, pid.size() - suffix_len)) + ".log", src.dir + pid, Kind::Text, pt });
				}
			}
			// Other logs in the folder written during the session.
			each_file(dir, L"*.log", [&](const std::wstring& name, uint64_t t)
				{
					const std::wstring l = lower(name);
					if (!ends_with(l, L".log") || ends_with(l, L".prev.log") || !in_window(src, t)) return;
					for (const wchar_t* own : kLogs)
						if (l == lower(std::wstring(own) + L".log") || is_pid_log(name, own, L".log")) return;
					items.push_back({ utf8(name), dir + name, Kind::Text, t });
				});
			for (const Sidecar& sc : kSidecars)
			{
				const std::wstring path = src.dir + (prev ? sc.prev : sc.now);
				const uint64_t t = mtime(path);
				if (in_window(src, t)) items.push_back({ sc.zip_name, path, Kind::Text, t });
			}
			// The config folder holds this session's copies only (the next
			// start clears config.prev and moves config there).
			const std::wstring config = src.dir + (prev ? L"config.prev\\" : L"config\\");
			each_file(config, L"*", [&](const std::wstring& name, uint64_t t)
				{
					items.push_back({ "config/" + utf8(name), config + name, Kind::Text, t });
				});
			each_file(dir, L"hang-*.dmp", [&](const std::wstring& name, uint64_t t)
				{
					if (in_window(src, t)) items.push_back({ utf8(name), dir + name, Kind::Dump, t });
				});
			// The wrapper's own crash logs (crash_log.hpp).
			each_file(dir, L"crash-*.txt", [&](const std::wstring& name, uint64_t t)
				{
					if (in_window(src, t)) items.push_back({ utf8(name), dir + name, Kind::CrashLog, t });
				});
			// The runtime's crash dumps: its crashpad handler writes one when the renderer crashes.
			const std::wstring reports = dir + L".sentry-native\\reports\\";
			each_file(reports, L"*.dmp", [&](const std::wstring& name, uint64_t t)
				{
					if (in_window(src, t)) items.push_back({ "renderer-crash/" + utf8(name), reports + name, Kind::Dump, t });
				});
			each_file(dir + L"screenshots\\", L"*.png", [&](const std::wstring& name, uint64_t t)
				{
					if (in_window(src, t)) items.push_back({ "screenshots/" + utf8(name), dir + L"screenshots\\" + name, Kind::Binary, t });
				});
			// Crash Logger copies each crash log into its folder at the crash.
			if (const std::wstring cdir = crash_log_dir(s); !cdir.empty())
				each_file(cdir, L"*", [&](const std::wstring& name, uint64_t t)
					{
						const std::wstring l = lower(name);
						if ((ends_with(l, L".log") || ends_with(l, L".txt")) && in_window(src, t))
							items.push_back({ "crash-logs/" + utf8(name), cdir + name, Kind::Text, t });
					});
			return items;
		}

		// A text file or dump streamed into the zip in pieces, with the user
		// name scrubbed (text) or masked (dump). A file over `max` is left out.
		bool add_scrubbed(zip_store::writer& zip, const Item& item, const Scrub& s, uint64_t max)
		{
			const HANDLE in = CreateFileW(item.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (in == INVALID_HANDLE_VALUE) return false;
			LARGE_INTEGER size = {};
			const bool text = item.kind != Kind::Dump;
			// "<user>" is longer than a short user name.
			bool ok = GetFileSizeEx(in, &size) && static_cast<uint64_t>(size.QuadPart) <= max &&
				zip.begin_entry(item.zip_name, ft(item.mtime), 2 * static_cast<uint64_t>(size.QuadPart) + 64);
			const size_t hold = text ? text_hold(s) : dump_hold(s);
			std::string buf, out;
			std::vector<char> piece(kPiece);
			// The size read now bounds the copy; a file still being written
			// may end up shorter.
			uint64_t left = ok ? static_cast<uint64_t>(size.QuadPart) : 0;
			while (ok)
			{
				DWORD got = 0;
				const DWORD want = static_cast<DWORD>((std::min)(static_cast<uint64_t>(kPiece), left));
				if (want && !ReadFile(in, piece.data(), want, &got, nullptr)) got = 0;
				left -= got;
				buf.append(piece.data(), got);
				const bool last = got == 0 || left == 0;
				const size_t limit = last ? buf.size() : (buf.size() > hold ? buf.size() - hold : 0);
				size_t done = 0;
				if (text)
				{
					out.clear();
					done = scrub_text(buf, limit, s, out);
					ok = zip.write_entry(out.data(), out.size());
				}
				else
				{
					mask_dump(buf, limit, s);
					done = limit;
					ok = zip.write_entry(buf.data(), done);
				}
				buf.erase(0, done);
				if (last) break;
			}
			CloseHandle(in);
			return zip.end_entry() && ok;
		}

		// Which of `needles` the file holds, read in pieces (a match may cross
		// two of them).
		template <size_t N>
		std::array<bool, N> find_lines(const std::wstring& path, const std::array<std::string_view, N>& needles, bool& read)
		{
			std::array<bool, N> found = {};
			read = false;
			const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (h == INVALID_HANDLE_VALUE) return found;
			read = true;
			size_t hold = 0;
			for (const auto& n : needles) hold = (std::max)(hold, n.size());
			std::string buf;
			std::vector<char> piece(kPiece);
			for (;;)
			{
				DWORD got = 0;
				if (!ReadFile(h, piece.data(), static_cast<DWORD>(piece.size()), &got, nullptr) || got == 0) break;
				buf.append(piece.data(), got);
				for (size_t i = 0; i < N; i++)
					found[i] = found[i] || buf.find(needles[i]) != std::string::npos;
				if (buf.size() > hold) buf.erase(0, buf.size() - hold);
			}
			CloseHandle(h);
			return found;
		}

		// The first `max` bytes of a file.
		std::string read_head(const std::wstring& path, DWORD max)
		{
			std::string s;
			const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) return s;
			s.resize(max);
			DWORD got = 0;
			if (!ReadFile(h, s.data(), max, &got, nullptr)) got = 0;
			s.resize(got);
			CloseHandle(h);
			return s;
		}

		// The release tag the session recorded, else the one in its
		// remix-comp log's first line ("release 1.0.4, commit ..."), else
		// the caller's.
		std::string version_of(const Source& src, const Settings& s)
		{
			std::string v = json_string(src.json, "mod_version");
			if (v.empty())
			{
				const std::string head = read_head(src.dir + L"remix-comp" + log_suffix(src.files), 4096);
				const size_t at = head.find("release ");
				if (at != std::string::npos)
				{
					const size_t from = at + 8;
					const size_t to = head.find_first_of(", \r\n", from);
					v = head.substr(from, to == std::string::npos ? std::string::npos : to - from);
				}
			}
			if (v.empty()) v = s.version;
			for (char& c : v)
			{
				const bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' ||
					c == '-' || c == '_';
				if (!keep) c = '_';
			}
			return v.empty() ? std::string("unknown") : v;
		}

		struct Ending
		{
			std::string end;
			std::string detail;
		};

		std::string exit_text(int64_t code)
		{
			const char* what = nullptr;
			switch (static_cast<uint32_t>(code))
			{
			case 0xC0000409: what = "a fail-fast: the process stopped itself (a security check, an abort or a fatal runtime error)"; break;
			case 0xC0000005: what = "an access violation"; break;
			case 0xC00000FD: what = "a stack overflow"; break;
			case 0xC0000374: what = "heap corruption"; break;
			case 0xC0000017: what = "out of memory"; break;
			case 0xC000001D: what = "an illegal instruction"; break;
			default: break;
			}
			if (code > 0xFFFF)
				return what ? std::format("0x{:08X}, {}", static_cast<uint32_t>(code), what)
					: std::format("0x{:08X}", static_cast<uint32_t>(code));
			return std::to_string(code);
		}

		// `crash_logs`: the wrapper's crash logs of the session, oldest first.
		// `exit_code`: the process's, -1 when unknown.
		Ending ending(const std::string& json, bool hang_dump, const std::vector<std::string>& crash_logs, int64_t exit_code)
		{
			const std::string end = json_string(json, "end");
			const std::string code = exit_code < 0 ? std::string() : std::format(" (process exit code {})", exit_text(exit_code));
			if (!crash_logs.empty())
			{
				const std::string& last = crash_logs.back();
				if (end == "clean")
					return { "clean", std::format("the process exited normally; {} exception(s) went unhandled and another "
						"crash handler resumed the game (crash log {}){}", crash_logs.size(), last, code) };
				if (end == "exiting")
					return { "crash", std::format("the game crashed while quitting ({}): see {}{}", json_string(json, "quit"),
						last, code) };
				return { "crash", std::format("the game crashed: see {}{}", last, code) };
			}
			if (end == "running")
			{
				if (hang_dump)
					return { "hang", "the game stopped presenting frames (a hang dump was written) and the process ended "
						"without quitting" + code };
				if (is_exception_exit(exit_code))
					return { "crash", "the process ended on an exception without a crash log" + code };
				// An exit call that skipped xNVSE's quit message and the wrapper's exit step.
				if (exit_code == 0)
					return { "unknown", "the process exited (code 0) without xNVSE reporting a quit: a mod's fast exit, or "
						"the game closed from its window" };
				return { "crash", "the process ended without quitting: a crash, a forced kill, or a hang that ended it" + code };
			}
			if (end == "exiting")
			{
				if (is_exception_exit(exit_code))
					return { "crash", std::format("the game crashed while quitting ({}){}", json_string(json, "quit"), code) };
				return { "clean", std::format("the game quit ({}) and the process ended without the wrapper's exit "
					"step: a fast exit, or a crash during shutdown{}", json_string(json, "quit"), code) };
			}
			if (end == "clean") return { "clean", "the process exited normally" + code };
			return { "unknown", "the session wrote no session.json" + code };
		}

		// The session.json with its end state settled and what the runtime's
		// log says about the renderer at the end added.
		std::string archived_json(const std::string& json, const Ending& e, const std::string& alive, const std::string& lost,
			const std::vector<std::string>& crash_logs, int64_t exit_code)
		{
			std::string body = json;
			// session.json has one field per line, "end" on its own.
			if (const size_t at = body.find("\n  \"end\": "); at != std::string::npos)
			{
				const size_t eol = body.find('\n', at + 1);
				body.erase(at, eol == std::string::npos ? std::string::npos : eol - at);
			}
			const size_t brace = body.find('{');
			const std::string rest = brace == std::string::npos ? std::string("\n  \"session_json\": false\n}\n")
				: body.substr(brace + 1);
			std::string logs = "[";
			for (size_t i = 0; i < crash_logs.size(); i++) logs += std::format("{}\"{}\"", i ? ", " : "", json_escape(crash_logs[i]));
			logs += "]";
			const std::string last = crash_logs.empty() ? std::string("null")
				: std::format("\"{}\"", json_escape(crash_logs.back()));
			return std::format("{{\n  \"end\": \"{}\",\n  \"end_detail\": \"{}\",\n  \"crash_log\": {},\n"
				"  \"crash_logs\": {},\n  \"renderer_alive_at_end\": {},\n  \"renderer_device_lost\": {},\n"
				"  \"exit_code\": {},{}", e.end, json_escape(e.detail), last, logs, alive, lost,
				exit_code < 0 ? std::string("null") : std::to_string(exit_code), rest);
		}
	}

	Result zip(const Settings& s, const Request& r)
	{
		Result res;
		const Source& src = r.src;
		std::vector<Item> items = collect(src, s);

		bool hang_dump = false;
		uint64_t newest = 0;
		std::vector<std::pair<uint64_t, std::string>> crashes;
		for (const Item& i : items)
		{
			if (i.kind == Kind::Dump) hang_dump = hang_dump || i.zip_name.starts_with("hang-");
			if (i.kind == Kind::CrashLog) crashes.emplace_back(i.mtime, i.zip_name);
			newest = (std::max)(newest, i.mtime);
		}
		std::sort(crashes.begin(), crashes.end());
		std::vector<std::string> crash_logs;
		for (const auto& c : crashes) crash_logs.push_back(c.second);
		Ending e = ending(src.json, hang_dump, crash_logs, r.exit_code);
		if (!r.end.empty()) e = { r.end, r.end_detail };
		res.end = e.end;

		// The bridge server writes "[Session] clean shutdown" into the
		// runtime's log when the game process ends while the renderer is
		// alive, crash or not; "[Session] device lost" is a GPU crash.
		// A runtime that found remix-dxvk.log still open in another process
		// wrote remix-dxvk.<process id>.log, the newest of which is read.
		std::string alive = "null";
		std::string lost = "null";
		{
			const wchar_t* suffix = log_suffix(src.files);
			std::wstring rt = src.dir + L"remix-dxvk" + suffix;
			uint64_t newest_pid = 0;
			for (const std::wstring& pid : pid_logs(src.dir, L"remix-dxvk", suffix))
			{
				const uint64_t t = mtime(src.dir + pid);
				if (own_log_of(src, t) && t > newest_pid)
				{
					newest_pid = t;
					rt = src.dir + pid;
				}
			}
			bool read = false;
			if (own_log_of(src, mtime(rt)))
			{
				const auto found = find_lines<2>(rt, { "[Session] clean shutdown", "[Session] device lost" }, read);
				if (read)
				{
					alive = found[0] ? "true" : "false";
					lost = found[1] ? "true" : "false";
				}
			}
		}
		if (!r.renderer_alive.empty()) alive = r.renderer_alive;

		const std::string version = version_of(src, s);
		const uint64_t start = src.start ? src.start : newest;
		res.name = std::format("NewVegasRTX-{}_{}{}.zip", version, start ? local_text(start, true) : "unknown", r.name_suffix);
		const std::wstring adir = s.logs + L"archive\\";
		CreateDirectoryW(adir.c_str(), nullptr);
		res.path = adir + wide(res.name);
		const std::wstring tmp_path = res.path + L".tmp";

		const Scrub scrub = s.scrub_user_name ? make_scrub() : Scrub{};
		zip_store::writer zip;
		if (r.deadline_ms) zip.set_deadline(r.deadline_ms);
		if (!zip.open(tmp_path))
		{
			res.warn = true;
			res.line = std::format("could not create rtx-remix\\logs\\archive\\{} (error {}): {} is not archived", res.name,
				GetLastError(), r.what);
			return res;
		}

		uint32_t skipped = 0;
		std::string json;
		scrub_text(archived_json(src.json, e, alive, lost, crash_logs, r.exit_code), SIZE_MAX, scrub, json);
		if (!zip.add("session.json", json.data(), json.size(), ft(start ? start : now_ft()))) skipped++;
		for (const Item& i : items)
		{
			if (zip.timed_out()) break;
			bool ok = false;
			if ((i.kind == Kind::Text || i.kind == Kind::CrashLog) && s.scrub_user_name)
				ok = add_scrubbed(zip, i, scrub, kMaxText);
			else if (i.kind == Kind::Dump && s.scrub_user_name)
				// A dump is copied only with the user name masked.
				ok = add_scrubbed(zip, i, scrub, kMaxDump);
			else
				// Screenshots are PNGs, compressed already.
				ok = zip.add_file(i.zip_name, i.path,
					i.kind == Kind::Binary ? zip_store::Method::Store : zip_store::Method::Deflate);
			if (!ok) skipped++;
		}
		const size_t entries = zip.entries();
		const uint64_t bytes = zip.bytes();
		const uint64_t raw = zip.raw_bytes();
		const bool timed_out = zip.timed_out();
		if (timed_out || !zip.close() || !MoveFileExW(tmp_path.c_str(), res.path.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			const DWORD error = GetLastError();
			zip.close();
			DeleteFileW(tmp_path.c_str());
			res.warn = true;
			res.line = timed_out
				? std::format("rtx-remix\\logs\\archive\\{} was not done in time: {} is not archived yet", res.name, r.what)
				: std::format("writing rtx-remix\\logs\\archive\\{} failed (error {}): {} is not archived", res.name, error,
					r.what);
			return res;
		}
		res.ok = true;
		res.warn = e.end != "clean" && e.end != "running";
		res.line = std::format("{} (release {}, started {}): ended {} ({}); renderer alive at the end: {}{} | archived to "
			"rtx-remix\\logs\\archive\\{}: {} files, {:.1f} MiB ({:.1f} MiB unpacked){}{}",
			r.what, version, start ? local_text(start) : std::string("unknown"), e.end, e.detail,
			alive == "null" ? std::string("unknown (no runtime log)") : alive, lost == "true" ? ", device lost" : "", res.name,
			entries, static_cast<double>(bytes) / (1024.0 * 1024.0), static_cast<double>(raw) / (1024.0 * 1024.0),
			skipped ? std::format(", {} files could not be added", skipped) : std::string(),
			s.scrub_user_name ? ", user name scrubbed" : "");
		return res;
	}

	void prune(const std::wstring& archive_dir, int keep)
	{
		std::vector<std::pair<uint64_t, std::wstring>> zips;
		each_file(archive_dir, L"NewVegasRTX-*.zip", [&](const std::wstring& name, uint64_t t) { zips.emplace_back(t, name); });
		std::sort(zips.begin(), zips.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		for (size_t i = static_cast<size_t>(keep); i < zips.size(); i++) DeleteFileW((archive_dir + zips[i].second).c_str());
		// A zip left half-written by a crash during archiving. One being
		// written now is open without delete sharing and stays.
		each_file(archive_dir, L"*.tmp", [&](const std::wstring& name, uint64_t) { DeleteFileW((archive_dir + name).c_str()); });
	}

	void remove_archived(const std::wstring& dir, uint64_t cutoff)
	{
		for (const wchar_t* pattern : { L"hang-*.dmp", L"crash-*.txt" })
			each_file(dir, pattern, [&](const std::wstring& name, uint64_t t)
				{
					if (t < cutoff) DeleteFileW((dir + name).c_str());
				});
		const std::wstring shots = dir + L"screenshots\\";
		each_file(shots, L"*.png", [&](const std::wstring& name, uint64_t t)
			{
				if (t < cutoff) DeleteFileW((shots + name).c_str());
			});
	}

	std::wstring held_folder(const std::wstring& logs, uint64_t start)
	{
		return logs + kHeldDir + std::format(L"{:020}", start ? start : now_ft()) + L"\\";
	}

	HANDLE mark_zipping(const std::wstring& held)
	{
		const std::wstring folder = held.substr(0, held.size() - 1);
		CreateDirectoryW(folder.substr(0, folder.find_last_of(L'\\')).c_str(), nullptr);
		return CreateFileW((held.substr(0, held.size() - 1) + kZippingSuffix).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	}

	bool zipping(const std::wstring& held)
	{
		return GetFileAttributesW((held.substr(0, held.size() - 1) + kZippingSuffix).c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	bool move_aside(const std::wstring& from, const std::wstring& to)
	{
		if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
		if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
		const DWORD error = GetLastError();
		return (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) &&
			CopyFileW(from.c_str(), to.c_str(), FALSE);
	}

	void hold_current(const std::wstring& logs, const std::wstring& held, uint64_t end)
	{
		CreateDirectoryW((logs + kHeldDir).c_str(), nullptr);
		CreateDirectoryW(held.c_str(), nullptr);
		for (const wchar_t* name : kLogs)
		{
			move_aside(logs + name + L".log", held + name + L".prev.log");
			for (const std::wstring& pid : pid_logs(logs, name, L".log"))
				move_aside(logs + pid, held + pid.substr(0, pid.size() - 4) + L".prev.log");
		}
		move_aside(logs + L"session.json", held + L"session.prev.json");
		for (const Sidecar& sc : kSidecars)
			move_aside(logs + sc.now, held + sc.prev);
		MoveFileExW((logs + L"config").c_str(), (held + L"config.prev").c_str(), 0);
		write_file(held + kHeldEnd, std::to_string(end));
	}

	void unhold(const std::wstring& held, const std::wstring& logs)
	{
		each_file(held, L"*", [&](const std::wstring& name, uint64_t)
			{
				if (name != kHeldEnd) MoveFileExW((held + name).c_str(), (logs + name).c_str(), MOVEFILE_REPLACE_EXISTING);
			});
		std::error_code ec;
		if (GetFileAttributesW((held + L"config.prev").c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			std::filesystem::remove_all(logs + L"config.prev", ec);
			MoveFileExW((held + L"config.prev").c_str(), (logs + L"config.prev").c_str(), 0);
		}
		std::filesystem::remove_all(held, ec);
	}

	bool is_exception_exit(int64_t code)
	{
		return code >= 0xC0000000ll && code <= 0xFFFFFFFFll;
	}

	bool is_snapshot(const std::wstring& name)
	{
		return lower(name).find(L"-snapshot-") != std::wstring::npos;
	}

	// ---- Who zips ----

	HANDLE take_lock(const std::wstring& logs)
	{
		return CreateFileW((logs + kLockFile).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	}

	HANDLE mark_reporter_busy(const std::wstring& logs)
	{
		return CreateFileW((logs + kReporterBusyFile).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	}

	bool reporter_busy(const std::wstring& logs)
	{
		return GetFileAttributesW((logs + kReporterBusyFile).c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	void mark_archived(const std::wstring& logs, uint64_t start, const std::string& name)
	{
		write_file(logs + kArchivedFile, std::format("{}\n{}\n", start, name));
	}

	std::string archived(const std::wstring& logs, uint64_t start)
	{
		const std::wstring path = logs + kArchivedFile;
		std::string text;
		if (!start || !read_file(path, text, 4096)) return {};
		const size_t eol = text.find('\n');
		if (eol == std::string::npos || _strtoui64(text.c_str(), nullptr, 10) != start) return {};
		std::string name = text.substr(eol + 1);
		while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
		const uint64_t at = mtime(path);
		bool newer = false;
		for (const wchar_t* pattern : { L"crash-*.txt", L"hang-*.dmp" })
			each_file(logs, pattern, [&](const std::wstring&, uint64_t t) { newer = newer || t > at; });
		return newer ? std::string() : name;
	}

	// ---- Files ----

	void each_file(const std::wstring& dir, const wchar_t* pattern,
		const std::function<void(const std::wstring& name, uint64_t mtime)>& f)
	{
		WIN32_FIND_DATAW fd;
		const HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) f(std::wstring(fd.cFileName), ft64(fd.ftLastWriteTime));
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}

	std::vector<std::wstring> folders(const std::wstring& dir)
	{
		std::vector<std::wstring> out;
		WIN32_FIND_DATAW fd;
		const HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return out;
		do
		{
			const std::wstring name = fd.cFileName;
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") out.push_back(name);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		std::sort(out.begin(), out.end());
		return out;
	}

	bool is_pid_log(const std::wstring& name, const wchar_t* base, const wchar_t* suffix)
	{
		const std::wstring l = lower(name);
		const std::wstring head = lower(std::wstring(base)) + L".";
		const std::wstring tail = lower(std::wstring(suffix));
		if (l.size() <= head.size() + tail.size() || !l.starts_with(head) || !l.ends_with(tail)) return false;
		for (size_t i = head.size(); i < l.size() - tail.size(); i++)
			if (l[i] < L'0' || l[i] > L'9') return false;
		return true;
	}

	std::vector<std::wstring> pid_logs(const std::wstring& dir, const wchar_t* base, const wchar_t* suffix)
	{
		std::vector<std::wstring> out;
		each_file(dir, (std::wstring(base) + L".*" + suffix).c_str(), [&](const std::wstring& name, uint64_t)
			{
				if (is_pid_log(name, base, suffix)) out.push_back(name);
			});
		return out;
	}

	// ---- Where the files really are ----

	std::wstring final_path(const std::wstring& path)
	{
		const HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE) return path;
		wchar_t buf[1024] = {};
		const DWORD n = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		CloseHandle(h);
		if (n == 0 || n >= std::size(buf)) return path;
		std::wstring out(buf, n);
		if (out.starts_with(L"\\\\?\\UNC\\")) out = L"\\\\" + out.substr(8);
		else if (out.starts_with(L"\\\\?\\")) out = out.substr(4);
		return out;
	}

	namespace
	{
		using VfsDumpFn = BOOL(WINAPI*)(LPSTR, size_t*);

		BOOL call_vfs_dump(VfsDumpFn fn, LPSTR buffer, size_t* size)
		{
			__try { return fn(buffer, size); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
		}
	}

	std::string vfs_dump(bool& loaded)
	{
		const HMODULE m = GetModuleHandleW(L"usvfs_x86.dll");
		loaded = m != nullptr;
		if (!m) return {};
		auto fn = reinterpret_cast<VfsDumpFn>(GetProcAddress(m, "usvfsCreateVFSDump"));
		if (!fn) fn = reinterpret_cast<VfsDumpFn>(GetProcAddress(m, "CreateVFSDump"));
		if (!fn) return {};
		size_t size = 0;
		call_vfs_dump(fn, nullptr, &size);
		for (int i = 0; i < 3 && size; i++)
		{
			std::string buf(size + 65536, '\0');
			size_t n = buf.size();
			if (call_vfs_dump(fn, buf.data(), &n))
			{
				buf.resize(strnlen(buf.data(), buf.size()));
				return buf;
			}
			size = n;
		}
		return {};
	}

	std::vector<std::wstring> redirected(const std::string& dump, const std::vector<std::wstring>& paths)
	{
		std::vector<std::string> wanted;
		for (const auto& p : paths) wanted.push_back(lower(utf8(p)));
		std::vector<std::wstring> out(paths.size());
		std::vector<std::string> stack;
		size_t at = 0;
		while (at < dump.size())
		{
			size_t eol = dump.find('\n', at);
			if (eol == std::string::npos) eol = dump.size();
			std::string_view line(dump.data() + at, eol - at);
			at = eol + 1;
			if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
			size_t level = 0;
			while (level < line.size() && line[level] == ' ') level++;
			line.remove_prefix(level);
			const size_t arrow = line.find(" -> ");
			if (arrow == std::string_view::npos) continue;
			stack.resize(level);
			stack.emplace_back(line.substr(0, arrow));
			std::string data(line.substr(arrow + 4));
			while (!data.empty() && (data.back() == ' ' || data.back() == '\t')) data.pop_back();
			if (data.empty()) continue;
			std::string path;
			for (const auto& part : stack)
			{
				if (part.empty()) continue;
				if (!path.empty()) path += '\\';
				path += part;
			}
			const std::string l = lower(path);
			for (size_t i = 0; i < wanted.size(); i++)
				if (out[i].empty() && l.size() >= wanted[i].size() && l.compare(l.size() - wanted[i].size(), wanted[i].size(), wanted[i]) == 0)
					out[i] = wide(data);
		}
		return out;
	}

	std::wstring real_path(const std::wstring& path, bool& unknown)
	{
		unknown = false;
		bool vfs = false;
		const std::string dump = vfs_dump(vfs);
		if (!vfs) return final_path(path);
		if (dump.empty())
		{
			unknown = true;
			return path;
		}
		// The file itself, else the nearest folder above it the tree redirects.
		std::vector<std::wstring> chain = { path };
		for (std::wstring p = path; ;)
		{
			const size_t slash = p.find_last_of(L"\\/");
			if (slash == std::wstring::npos || slash < 3) break;
			p.resize(slash);
			chain.push_back(p);
		}
		const std::vector<std::wstring> real = redirected(dump, chain);
		for (size_t i = 0; i < chain.size(); i++)
			if (!real[i].empty()) return real[i] + path.substr(chain[i].size());
		return path;
	}
}
