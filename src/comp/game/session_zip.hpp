#pragma once

// One game session's zip, rtx-remix\logs\archive\NewVegasRTX-<version>_<start>.zip
// (session_report.hpp lists what goes in), and the files that say who makes
// it. Shared by the wrapper (session_archive.cpp, session_report.cpp) and
// NewVegasRTX-Reporter.exe (src/reporter): nothing here reads the wrapper's
// config or writes its log; the caller passes the settings and logs the line
// it gets back.
//
// A session's zip is written once, by the first of:
//   1. the game, when xNVSE reports a quit (session_report.cpp), within
//      kQuitZipMs, else not at all;
//   2. the reporter, once the game process has ended in any way, when the
//      quit did not zip it, or the session crashed after that zip;
//   3. the next start's archive thread (session_archive.cpp).
// session.archived names the session that has its zip and the zip, so the
// later ones only move its files aside. It does not count once a crash log
// or hang dump is newer than it (a crash while the game shut down): that
// session is zipped again under the same name.
//
// Only the holder of session.lock touches a session's files under their
// current names: the game for its whole life, then the reporter for the
// moment it moves them aside. The reporter creates reporter.busy, takes the
// lock, moves the session's files to unarchived\<start>\ under their *.prev
// names (with unarchived\<start>.zipping held beside the folder), lets go of
// the lock and zips the folder. Once the zip is written it moves the files
// back to their *.prev names in rtx-remix\logs and removes the folder; when
// it fails, the folder stays for the next start's archive thread, which
// zips held folders and waits (bounded) for one that is still .zipping.
// A game that took the lock first leaves the reporter nothing to do, and
// zips that session itself as its previous one. A game that starts in the
// moment the reporter holds the lock waits nowhere in DllMain: its log goes
// to remix-comp.<process id>.log and a background thread takes the lock
// and the session report over within kReporterWaitMs
// (session_archive.hpp).
namespace comp::game::session_zip
{
	// The component logs: <name>.log while the session runs, <name>.prev.log
	// once the next session has started.
	inline constexpr const wchar_t* kLogs[] = { L"remix-comp", L"fnv_remix_nvse", L"bridge32", L"bridge64", L"remix-dxvk" };

	// The session's own files besides the logs, and their names in the zip.
	struct Sidecar
	{
		const wchar_t* now;
		const wchar_t* prev;
		const char* zip_name;
	};
	inline constexpr Sidecar kSidecars[] = {
		{ L"loadorder.txt", L"loadorder.prev.txt", "loadorder.txt" },
		{ L"loadorder-changes.txt", L"loadorder-changes.prev.txt", "loadorder-changes.txt" },
	};

	inline constexpr const wchar_t* kLockFile = L"session.lock";
	inline constexpr const wchar_t* kReporterBusyFile = L"reporter.busy";
	inline constexpr const wchar_t* kArchivedFile = L"session.archived";
	// Sessions whose zip was not made yet, one folder each, named by the start.
	inline constexpr const wchar_t* kHeldDir = L"unarchived\\";
	// The start of the session after a held one, in its folder.
	inline constexpr const wchar_t* kHeldEnd = L"next-start.txt";
	// Beside a held folder (unarchived\<start>.zipping) while the reporter zips it.
	inline constexpr const wchar_t* kZippingSuffix = L".zipping";

	// The game's zip when it quits stops after this long.
	inline constexpr uint64_t kQuitZipMs = 4000;
	// A game that started while the reporter held session.lock takes it over
	// from a background thread within this long.
	inline constexpr DWORD kReporterWaitMs = 10000;
	// The archive thread waits this long for a held folder the reporter zips.
	inline constexpr DWORD kZippingWaitMs = 60000;

	struct Settings
	{
		std::wstring logs;                  // rtx-remix\logs\, with a trailing backslash
		std::wstring game;                  // the game folder, with a trailing backslash
		std::string crash_log_folder;       // [SessionReport] CrashLogFolder
		bool scrub_user_name = true;        // [SessionReport] ScrubUserName
		std::string version;                // the release tag, when the session's own files name none
	};

	// Which names the session's own files have.
	enum class Files : uint8_t
	{
		Prev,       // *.prev.log, session.prev.json, loadorder.prev.txt, config.prev\: moved aside at the next start
		Current,    // *.log, session.json, loadorder.txt, config\: the session runs, or its process just ended
	};

	struct Source
	{
		std::wstring dir;                   // the folder with the session's own files
		Files files = Files::Prev;
		std::string json;                   // its session.json
		uint64_t start = 0;                 // UTC file time; 0: unknown
		uint64_t end = 0;                   // files written at or after this are not the session's
	};

	struct Request
	{
		Source src;
		const char* what = "session";       // names the session in the log line
		std::string name_suffix;            // after the zip's name, before ".zip"
		std::string end;                    // the end state to record; empty: worked out from the files
		std::string end_detail;
		std::string renderer_alive;         // "true" / "false"; empty: from the runtime's log
		int64_t exit_code = -1;             // the game process's exit code; -1: unknown
		uint64_t deadline_ms = 0;           // GetTickCount64() time the zip must be done by; 0: none
	};

	struct Result
	{
		bool ok = false;
		std::wstring path;                  // the zip, as this process names it
		std::string name;                   // its file name
		std::string end;                    // clean / crash / hang / unknown / running
		std::string line;                   // the log line
		bool warn = false;                  // log the line as a warning
	};

	// Zips one session into Settings::logs + archive\. The zip is written as
	// <name>.tmp and renamed when complete.
	Result zip(const Settings& s, const Request& r);

	// Deletes all but the newest `keep` zips in `archive_dir`, and the *.tmp
	// zips that were left half-written.
	void prune(const std::wstring& archive_dir, int keep);

	// Deletes the hang dumps, crash logs and screenshots in the logs folder
	// `dir` written before `cutoff`.
	void remove_archived(const std::wstring& dir, uint64_t cutoff);

	// A snapshot zip ("Save Report Now"), not the end-of-session one.
	bool is_snapshot(const std::wstring& name);

	// A process exit code that is an NTSTATUS error: the process ended on an
	// exception (a fail-fast included), not by an exit call.
	bool is_exception_exit(int64_t code);

	// ---- Who zips ----

	// session.lock in `logs`, held without sharing and deleted when closed.
	// INVALID_HANDLE_VALUE: another process holds it.
	HANDLE take_lock(const std::wstring& logs);

	// reporter.busy in `logs`, deleted when closed.
	HANDLE mark_reporter_busy(const std::wstring& logs);
	bool reporter_busy(const std::wstring& logs);

	// Records that the session that started at `start` has its zip `name`.
	void mark_archived(const std::wstring& logs, uint64_t start, const std::string& name);

	// The zip session.archived names for the session that started at `start`;
	// empty when it names another session, or a crash log or hang dump is
	// newer than it.
	std::string archived(const std::wstring& logs, uint64_t start);

	// unarchived\<start>\ in `logs`, with a trailing backslash.
	std::wstring held_folder(const std::wstring& logs, uint64_t start);

	// unarchived\<start>.zipping beside `held`, held without sharing and
	// deleted when closed; zipping() says whether it stands.
	HANDLE mark_zipping(const std::wstring& held);
	bool zipping(const std::wstring& held);

	// Moves the session's own files from their current names in `logs` into
	// `held` under their *.prev names, as the next start would move them
	// aside: the component logs (the <name>.<process id>.log ones too),
	// session.json, loadorder*.txt and config\, and writes `end` (the time
	// its files stop being the session's) as next-start.txt.
	void hold_current(const std::wstring& logs, const std::wstring& held, uint64_t end);

	// Moves a held session's files back to their *.prev names in `logs`,
	// replacing what is there, and removes `held`.
	void unhold(const std::wstring& held, const std::wstring& logs);

	// Renames `from` to `to`, replacing it. A file another process holds
	// open without delete sharing (a bridge server of an older build that
	// outlived its game) is copied, and its owner keeps the original.
	bool move_aside(const std::wstring& from, const std::wstring& to);

	// ---- Files ----

	// Every file in `dir` (trailing backslash) that matches `pattern`, with
	// its last write time.
	void each_file(const std::wstring& dir, const wchar_t* pattern,
		const std::function<void(const std::wstring& name, uint64_t mtime)>& f);

	// The folders in `dir` (trailing backslash), sorted by name.
	std::vector<std::wstring> folders(const std::wstring& dir);

	// `name` is <base>.<process id><suffix>, the log of a component that
	// found <base>.log still open in another process when it started.
	bool is_pid_log(const std::wstring& name, const wchar_t* base, const wchar_t* suffix);

	// The names of the <base>.<process id><suffix> logs in `dir`.
	std::vector<std::wstring> pid_logs(const std::wstring& dir, const wchar_t* base, const wchar_t* suffix);

	// ---- Where the files really are ----

	// `path` as the file system resolves it (junctions, symbolic links,
	// subst drives), without the \\?\ prefix; `path` itself when it cannot.
	std::wstring final_path(const std::wstring& path);

	// Mod Organizer 2's virtual file system (usvfs) answers this process's
	// file calls, GetFinalPathNameByHandle included, with the game folder's
	// paths. Its redirection tree, one node per line, "<one space per
	// level><name> -> <real path>"; empty when usvfs is not loaded (`loaded`
	// false) or gives no tree.
	std::string vfs_dump(bool& loaded);

	// The real path the tree gives each of `paths` (full paths), empty for
	// one it does not redirect.
	std::vector<std::wstring> redirected(const std::string& dump, const std::vector<std::wstring>& paths);

	// `path` as Explorer finds it: under Mod Organizer 2 the file's real place
	// (`unknown` true when the redirections could not be read), else its
	// final path.
	std::wstring real_path(const std::wstring& path, bool& unknown);
}
