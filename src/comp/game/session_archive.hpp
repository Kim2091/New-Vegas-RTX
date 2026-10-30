#pragma once

#include "session_zip.hpp"

// The previous session's files: moved aside at startup and zipped by a
// thread (session_report.hpp describes the files, session_zip.hpp who zips
// a session).
namespace comp::game::session_archive
{
	enum class Stage : uint8_t
	{
		Owner,      // this process holds session.lock and moved the previous session aside
		Reporter,   // NewVegasRTX-Reporter.exe holds the lock for a moment: take_over() later
		Other,      // another game instance holds it: nothing is touched
	};

	// DllMain, before any component opens its log; never waits. Takes
	// rtx-remix\logs\session.lock for this process, keeps the previous
	// session.json in memory and moves the previous session's files to their
	// *.prev names: the logs of the wrapper, the plugin, the bridge and the
	// runtime (each would move its own the same way when it opens it),
	// session.json, loadorder.txt, loadorder-changes.txt and the folder
	// config\ itself. A log another process still holds open without delete
	// sharing (a bridge server of an older build that outlived its game) is
	// copied. The runtime and the bridge do not rotate their logs while
	// session.lock or archive.pending stands.
	// A component that found its log still open in another process wrote
	// <name>.<process id>.log: it moves to <name>.<process id>.prev.log and
	// goes into the zip as well.
	//
	// A session zipped when it ended (session.archived names it) has its
	// files moved aside the same way, and nothing waits for a zip.
	//
	// rtx-remix\logs\archive.pending stands while *.prev files wait for their
	// zip. When it is still there at the next start (the archive thread did
	// not finish: the game crashed or quit first), those files move to
	// unarchived\<start>\ before the new ones take their names, and the next
	// archive thread zips them first. A held folder NewVegasRTX-Reporter.exe
	// zips (unarchived\<start>.zipping stands) is waited for, up to
	// kZippingWaitMs, on the archive thread.
	Stage stage(uint64_t this_start);

	// A background thread, after stage() returned Stage::Reporter (the
	// reporter moves its session's files aside under the lock, a matter of
	// milliseconds): waits up to kReporterWaitMs for session.lock, then does
	// what stage() does, leaving this process's own <name>.<process id>.log
	// in place. False: the lock stayed held; nothing is touched.
	bool take_over();

	// Archive thread: zips each held unarchived\ session, then what stage()
	// moved aside unless it was zipped when it ended, with the crash logs,
	// hang dumps, renderer crash dumps, screenshots and Crash Logger logs
	// written during that session, into
	// archive\NewVegasRTX-<version>_<start>.zip, keeps the newest
	// [SessionReport] ArchiveKeep zips and logs one line per zip. Under Mod
	// Organizer 2's virtual file system it also logs where rtx-remix\logs
	// really is.
	void archive_previous();

	// No archive thread runs in this process (another instance owns the logs).
	void skip();

	// The archive thread is done, or never runs: zip_for() is final.
	bool finished();

	// The zip that holds the session running at UTC file time `when`, as
	// Explorer finds it (a mod manager's real folder): the one this process
	// made or found for that session, else the first end-of-session zip
	// written after `when`; empty when there is none.
	std::wstring zip_for(uint64_t when);

	// The archive folder as Explorer finds it, with a trailing backslash.
	std::wstring archive_folder();

	// The newest zip in the archive folder, whoever wrote it (this process,
	// an earlier one or NewVegasRTX-Reporter.exe), as Explorer finds it;
	// empty when there is none.
	std::wstring newest_zip();

	// Mod Organizer 2's virtual file system is loaded and its redirections
	// could not be read, so the real folder of new files is unknown.
	bool folder_unknown();

	// The zip settings from [SessionReport] and this process's folders.
	session_zip::Settings settings();
}
