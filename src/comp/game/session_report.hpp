#pragma once

// What a player sends with a bug report: one zip per game session, plus files that say
// what the session ran with. All files are in rtx-remix\logs\ ([SessionReport]
// in remix-comp.ini):
//   - session.json: the mod version, the start time, the end state
//     ("running" while the game runs; "exiting" once xNVSE reports a quit;
//     "clean" at a normal process exit), a heartbeat every 10 s (time,
//     duration, frame count, the player's cell, worldspace and position),
//     mark count, hang dumps and known-incompatible mods;
//   - loadorder.txt: the engine's loaded plugins in load order, the NVSE
//     plugins and every other DLL in the process with its file version and
//     path; loadorder-changes.txt: what was added, removed or updated since
//     the previous session (loadorder.prev.txt);
//   - config\: the game's INIs as the game reads them (Mod Organizer 2's
//     profile INIs included), nvtf.ini, remix-comp.ini and the runtime's
//     rtx.conf, user.conf and dxvk.conf;
//   - crash-<time>.txt: the readable crash log (crash_log.hpp);
//   - hang-<time>.dmp: a minidump written when no frame came for
//     HangSeconds while the game was in front and not loading;
//   - the mark key writes "[Mark] #N" to remix-comp.log and the plugin's
//     log and asks the runtime for a screenshot (screenshots\<stem>.png).
//
// Each session's logs, these files, its crash logs, hang dumps, renderer
// crash dumps, screenshots and the Crash Logger logs written during it are
// zipped into archive\NewVegasRTX-<version>_<start time>.zip by the first
// of (session_zip.hpp):
//   - the game itself when xNVSE reports a quit, if it takes at most
//     kQuitZipMs;
//   - NewVegasRTX-Reporter.exe ([SessionReport] Reporter), which the game
//     starts and which zips the session once the game process has ended,
//     whatever ended it (crash, fail-fast, Task Manager, a killed hang),
//     and then shows the zip's path after a crash or hang (not one
//     while quitting);
//   - the next start: before any component opens its log, the previous
//     session's logs are moved to *.prev.log (what each component would do
//     itself) and its session files to *.prev.*, and a thread zips them
//     unless that session already has its zip. Files whose zip was not made
//     before the process ended wait for the start after
//     (session_archive.hpp).
// "Save Report Now" in the Remix menu's Host Settings zips the session so
// far into archive\NewVegasRTX-<version>_<start time>-snapshot-<time>.zip.
// The newest zip goes to the runtime as rtx.gui.reportFile, which the log
// section of the Remix menu names with a button that shows it: the newest
// one in the archive folder once the archive thread is done, then each zip
// this process writes.
// A lock file keeps a second game instance from touching the files.
// Nothing here can stop the game from starting: a failure is logged and
// skipped.
namespace comp::game::session_report
{
	// DllMain, game process, before the first log line: takes the lock,
	// moves the previous session's files aside and writes this session's
	// session.json. Never waits: when NewVegasRTX-Reporter.exe holds the lock
	// that moment, this process's log goes to remix-comp.<process id>.log and
	// start() takes over later.
	void stage();

	// DllMain, after the config is loaded: starts the archive thread and the
	// heartbeat / hang watchdog thread, or, after the reporter held the lock,
	// a thread that takes the lock over (session_archive::take_over) and
	// starts them then.
	void start();

	// Present: the frame count, the place for the heartbeat, the mark key and
	// the load-order snapshot once the game's data is loaded.
	void on_present();

	// DLL_PROCESS_DETACH: the session ended cleanly.
	void on_exit();

	// xNVSE reported that the game is quitting (`how`: "menu" or "console"):
	// session.json says "exiting", and this session is zipped (at most
	// kQuitZipMs; past that it is left to the reporter or the next start).
	void on_quit(const char* how);

	// What save_now() last did: `text` is the saved zip's path as Explorer
	// finds it, "saving..." or why it was not saved; `generation` changes
	// with it.
	struct Saved
	{
		uint32_t generation = 0;
		std::string text;
	};

	// Host Settings' "Save Report Now": zips the session so far on a thread
	// and logs "[Session] report saved: <path>". False: one is being written,
	// or another game instance owns the session files.
	bool save_now();

	Saved saved();

	// Explorer with the last report save_now() wrote selected, else the
	// archive folder.
	void show_saved();
}

// The plugin calls this when xNVSE reports that the game is quitting.
extern "C" void FnvRemix_SessionExit(const char* how);
