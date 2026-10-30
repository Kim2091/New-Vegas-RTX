#include "std_include.hpp"
#include "session_archive.hpp"

#include <algorithm>
#include <atomic>

#include "session_files.hpp"
#include "session_zip.hpp"

#include "shared/common/config.hpp"

namespace comp::game::session_archive
{
	using namespace session_files;
	using session_zip::kLogs;
	using session_zip::kSidecars;
	using session_zip::kHeldDir;
	using session_zip::kHeldEnd;
	using session_zip::move_aside;

	namespace
	{
		constexpr uint64_t kSlack = 60 * kFtSecond;

		// Stands while *.prev files wait for their zip.
		constexpr const wchar_t* kPending = L"archive.pending";
		constexpr size_t kMaxHeld = 3;

		HANDLE g_lock = INVALID_HANDLE_VALUE;
		bool g_took_over = false;
		std::string g_prev_json;
		uint64_t g_prev_start = 0;
		uint64_t g_this_start = 0;
		bool g_have_prev = false;
		// The previous session's zip, made when it ended (session_zip.hpp).
		std::string g_prev_zip;

		// A zip this process made or found, and the session span it holds.
		struct Made
		{
			uint64_t from = 0;
			uint64_t to = 0;
			std::wstring path;
		};
		std::vector<Made> g_made;
		std::wstring g_archive_real;
		bool g_folder_unknown = false;
		std::atomic<bool> g_finished { false };

		// The real archive folder and zip paths, and one line about where
		// rtx-remix\logs is under a virtual file system.
		void locate()
		{
			const std::wstring& dir = shared::common::log_dir();
			bool vfs = false;
			const std::string dump = session_zip::vfs_dump(vfs);
			if (!vfs)
			{
				g_archive_real = session_zip::final_path(dir + L"archive") + L"\\";
				for (Made& m : g_made) m.path = session_zip::final_path(m.path);
				return;
			}
			if (dump.empty())
			{
				g_folder_unknown = true;
				g_archive_real = dir + L"archive\\";
				shared::common::log("Session", "Mod Organizer 2's virtual file system is active and its redirections could "
					"not be read: new files in rtx-remix\\logs may be in MO2's Overwrite folder instead",
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			std::vector<std::wstring> paths = { dir + L"remix-comp.log" };
			for (const Made& m : g_made) paths.push_back(m.path);
			const std::vector<std::wstring> real = session_zip::redirected(dump, paths);
			const std::wstring& log_real = real[0];
			const size_t slash = log_real.find_last_of(L"\\/");
			const std::wstring logs = slash == std::wstring::npos ? dir : log_real.substr(0, slash + 1);
			g_archive_real = logs + L"archive\\";
			for (size_t i = 0; i < g_made.size(); i++)
			{
				const std::wstring& r = real[i + 1];
				const std::wstring name = g_made[i].path.substr(g_made[i].path.find_last_of(L'\\') + 1);
				g_made[i].path = r.empty() ? g_archive_real + name : r;
			}
			shared::common::log_event("Session", log_real.empty()
				? "Mod Organizer 2's virtual file system is active; it does not redirect rtx-remix\\logs"
				: std::format("Mod Organizer 2's virtual file system writes rtx-remix\\logs to {} (Explorer finds the files "
					"there, not in the game folder)", utf8(logs)),
				log_real.empty() ? shared::common::LOG_TYPE::LOG_TYPE_STATUS : shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		// Zips one session; `what` names it in the log line. True when the
		// zip was written.
		bool archive_one(const session_zip::Source& src, const char* what)
		{
			session_zip::Request r;
			r.src = src;
			r.what = what;
			const session_zip::Result res = session_zip::zip(settings(), r);
			shared::common::log_event("Session", res.line,
				res.warn ? shared::common::LOG_TYPE::LOG_TYPE_WARN : shared::common::LOG_TYPE::LOG_TYPE_STATUS);
			if (!res.ok) return false;
			g_made.push_back({ src.start > kSlack ? src.start - kSlack : 0, src.end, res.path });
			return true;
		}

		// Zips the held sessions, then the previous one, and clears what the
		// zips now hold. archive.pending goes once every session is zipped.
		void archive_all()
		{
			const auto& cfg = shared::common::config::get().session_report;
			const std::wstring& dir = shared::common::log_dir();
			const std::wstring held_root = dir + kHeldDir;
			std::error_code ec;
			if (g_took_over)
				shared::common::log_event("Session", std::format("NewVegasRTX-Reporter.exe held rtx-remix\\logs\\session.lock "
					"at startup: this session's report started once it let go, and this log is "
					"remix-comp.{}.log", GetCurrentProcessId()));
			if (!cfg.archive)
			{
				std::filesystem::remove_all(held_root, ec);
				DeleteFileW((dir + kPending).c_str());
				return;
			}

			// Files older than the newest earlier session's start belong to
			// sessions whose zip exists; that session's own stay one start longer.
			uint64_t newest_start = g_prev_start;
			for (const std::wstring& name : session_zip::folders(held_root))
				newest_start = (std::max)(newest_start, _wcstoui64(name.c_str(), nullptr, 10));
			uint64_t cutoff = newest_start > kSlack ? newest_start - kSlack : 0;
			bool all = true;
			for (const std::wstring& name : session_zip::folders(held_root))
			{
				session_zip::Source held;
				held.dir = held_root + name + L"\\";
				// The reporter zips this one: its zip, or the folder back when it fails.
				if (session_zip::zipping(held.dir))
				{
					const uint64_t until = GetTickCount64() + session_zip::kZippingWaitMs;
					while (session_zip::zipping(held.dir) && GetTickCount64() < until) Sleep(250);
					if (session_zip::zipping(held.dir))
					{
						shared::common::log("Session", std::format("NewVegasRTX-Reporter.exe still zips the session in "
							"rtx-remix\\logs\\unarchived\\{}: it is left to it", shared::utils::convert_wstring(name)),shared::common::LOG_TYPE::LOG_TYPE_WARN);
						all = false;
						continue;
					}
					if (GetFileAttributesW(held.dir.c_str()) == INVALID_FILE_ATTRIBUTES)
					{
						shared::common::log_event("Session", "NewVegasRTX-Reporter.exe zipped the previous session when it ended");
						continue;
					}
				}
				read_file(held.dir + L"session.prev.json", held.json, 1u << 20);
				held.start = json_u64(held.json, "start_filetime");
				std::string end;
				if (read_file(held.dir + kHeldEnd, end, 64)) held.end = _strtoui64(end.c_str(), nullptr, 10);
				if (!held.end) held.end = g_prev_start ? g_prev_start : g_this_start;
				if (archive_one(held, "an earlier session that was not archived yet"))
					std::filesystem::remove_all(held.dir, ec);
				else
				{
					all = false;
					cutoff = (std::min)(cutoff, held.start > kSlack ? held.start - kSlack : 0);
				}
			}
			RemoveDirectoryW(held_root.c_str());

			if (!g_prev_zip.empty())
			{
				g_made.push_back({ g_prev_start > kSlack ? g_prev_start - kSlack : 0, g_this_start,
					dir + L"archive\\" + wide(g_prev_zip) });
				shared::common::log_event("Session", std::format("previous session (started {}): zipped when it ended, "
					"rtx-remix\\logs\\archive\\{}", local_text(g_prev_start), g_prev_zip));
			}
			else if (g_have_prev)
			{
				session_zip::Source prev;
				prev.dir = dir;
				prev.json = g_prev_json;
				prev.start = g_prev_start;
				prev.end = g_this_start;
				if (!archive_one(prev, "previous session"))
				{
					all = false;
					cutoff = 0;
				}
			}
			else
				shared::common::log_event("Session", "no previous session files to archive");

			session_zip::prune(dir + L"archive\\", cfg.archive_keep);
			if (cutoff) session_zip::remove_archived(dir, cutoff);
			if (all) DeleteFileW((dir + kPending).c_str());
		}

		// stage(): the *.prev files of a session whose zip was not made move
		// to unarchived\<start>\, with the next session's start beside them.
		void hold_unarchived(const std::wstring& dir)
		{
			std::string json;
			read_file(dir + L"session.prev.json", json, 1u << 20);
			const uint64_t start = json_u64(json, "start_filetime");
			const std::wstring root = dir + kHeldDir;
			const std::wstring held = root + std::format(L"{:020}", start ? start : now_ft()) + L"\\";
			CreateDirectoryW(root.c_str(), nullptr);
			CreateDirectoryW(held.c_str(), nullptr);
			for (const wchar_t* name : kLogs)
			{
				move_aside(dir + name + L".prev.log", held + name + L".prev.log");
				for (const std::wstring& pid : session_zip::pid_logs(dir, name, L".prev.log"))
					move_aside(dir + pid, held + pid);
			}
			move_aside(dir + L"session.prev.json", held + L"session.prev.json");
			for (const session_zip::Sidecar& s : kSidecars)
				move_aside(dir + s.prev, held + s.prev);
			MoveFileExW((dir + L"config.prev").c_str(), (held + L"config.prev").c_str(), 0);
			write_file(held + kHeldEnd, std::to_string(g_prev_start ? g_prev_start : g_this_start));

			// The oldest go when sessions keep ending before their zip is made
			// (not one the reporter zips now).
			std::error_code ec;
			const std::vector<std::wstring> all = session_zip::folders(root);
			for (size_t i = 0; i + kMaxHeld < all.size(); i++)
				if (!session_zip::zipping(root + all[i] + L"\\")) std::filesystem::remove_all(root + all[i], ec);
		}

		// With session.lock taken: keeps the previous session.json in memory
		// and moves the previous session's files aside. `own_pid_log`: this
		// process writes <name>.<its id>.log already, which stays.
		void move_previous(bool own_pid_log)
		{
			const std::wstring& dir = shared::common::log_dir();
			read_file(dir + L"session.json", g_prev_json, 1u << 20);
			g_prev_start = json_u64(g_prev_json, "start_filetime");
			g_prev_zip = session_zip::archived(dir, g_prev_start);
			DeleteFileW((dir + session_zip::kArchivedFile).c_str());
			if (GetFileAttributesW((dir + kPending).c_str()) != INVALID_FILE_ATTRIBUTES) hold_unarchived(dir);

			const std::wstring own = std::format(L".{}.log", GetCurrentProcessId());
			bool moved = false;
			for (const wchar_t* name : kLogs)
			{
				if (move_aside(dir + name + L".log", dir + name + L".prev.log")) moved = true;
				// A component that found <name>.log still open in another process
				// wrote <name>.<process id>.log. Those of earlier sessions are in
				// their zips or were held above.
				for (const std::wstring& pid : session_zip::pid_logs(dir, name, L".prev.log"))
					DeleteFileW((dir + pid).c_str());
				for (const std::wstring& pid : session_zip::pid_logs(dir, name, L".log"))
				{
					if (own_pid_log && pid.ends_with(own)) continue;
					if (move_aside(dir + pid, dir + pid.substr(0, pid.size() - 4) + L".prev.log")) moved = true;
				}
			}
			move_aside(dir + L"session.json", dir + L"session.prev.json");
			for (const auto& s : kSidecars)
				if (move_aside(dir + s.now, dir + s.prev)) moved = true;
			std::error_code ec;
			std::filesystem::remove_all(dir + L"config.prev", ec);
			MoveFileExW((dir + L"config").c_str(), (dir + L"config.prev").c_str(), 0);
			// A session zipped when it ended has nothing left waiting for a zip.
			g_have_prev = g_prev_zip.empty() && (moved || !g_prev_json.empty());
			if (g_have_prev) write_file(dir + kPending, std::to_string(g_prev_start));
		}
	}

	session_zip::Settings settings()
	{
		const auto& cfg = shared::common::config::get().session_report;
		session_zip::Settings s;
		s.logs = shared::common::log_dir();
		s.game = game_dir();
		s.crash_log_folder = cfg.crash_log_folder;
		s.scrub_user_name = cfg.scrub_user_name;
		return s;
	}

	Stage stage(uint64_t this_start)
	{
		g_this_start = this_start;
		const std::wstring& dir = shared::common::log_dir();
		// Held for the whole process; the system deletes the file when the
		// process ends, crash or not.
		g_lock = session_zip::take_lock(dir);
		if (g_lock == INVALID_HANDLE_VALUE)
			return session_zip::reporter_busy(dir) ? Stage::Reporter : Stage::Other;
		move_previous(false);
		return Stage::Owner;
	}

	bool take_over()
	{
		const std::wstring& dir = shared::common::log_dir();
		const uint64_t until = GetTickCount64() + session_zip::kReporterWaitMs;
		while ((g_lock = session_zip::take_lock(dir)) == INVALID_HANDLE_VALUE && GetTickCount64() < until) Sleep(50);
		if (g_lock == INVALID_HANDLE_VALUE) return false;
		g_took_over = true;
		// The reporter moved its session's files to unarchived\ unless that
		// session was zipped when it quit; what has the plain names without a
		// session.json is this process's own (a log opened meanwhile).
		if (GetFileAttributesW((dir + L"session.json").c_str()) != INVALID_FILE_ATTRIBUTES) move_previous(true);
		else if (GetFileAttributesW((dir + kPending).c_str()) != INVALID_FILE_ATTRIBUTES) hold_unarchived(dir);
		return true;
	}

	void archive_previous()
	{
		try
		{
			archive_all();
		}
		catch (const std::exception& ex)
		{
			shared::common::log("Session", std::format("archiving the previous session failed: {}", ex.what()),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}
		catch (...)
		{
			shared::common::log("Session", "archiving the previous session failed", shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}
		try
		{
			locate();
		}
		catch (...)
		{
			g_archive_real = shared::common::log_dir() + L"archive\\";
		}
		g_finished.store(true, std::memory_order_release);
	}

	void skip()
	{
		g_archive_real = shared::common::log_dir() + L"archive\\";
		g_finished.store(true, std::memory_order_release);
	}

	bool finished()
	{
		return g_finished.load(std::memory_order_acquire);
	}

	std::wstring zip_for(uint64_t when)
	{
		if (!finished()) return {};
		for (const Made& m : g_made)
			if (when >= m.from && when < m.to) return m.path;
		// Made by an earlier process: sessions are zipped in order, so the
		// first end-of-session zip written after `when` holds it.
		std::wstring name;
		uint64_t first = 0;
		session_zip::each_file(shared::common::log_dir() + L"archive\\", L"NewVegasRTX-*.zip", [&](const std::wstring& n, uint64_t t)
			{
				if (t > when && (!first || t < first) && !session_zip::is_snapshot(n))
				{
					first = t;
					name = n;
				}
			});
		return name.empty() ? std::wstring() : archive_folder() + name;
	}

	std::wstring archive_folder()
	{
		if (!finished() || g_archive_real.empty()) return shared::common::log_dir() + L"archive\\";
		return g_archive_real;
	}

	bool folder_unknown()
	{
		return finished() && g_folder_unknown;
	}

	std::wstring newest_zip()
	{
		std::wstring name;
		uint64_t newest = 0;
		session_zip::each_file(shared::common::log_dir() + L"archive\\", L"NewVegasRTX-*.zip", [&](const std::wstring& n, uint64_t t)
			{
				if (t >= newest)
				{
					newest = t;
					name = n;
				}
			});
		return name.empty() ? std::wstring() : archive_folder() + name;
	}
}
