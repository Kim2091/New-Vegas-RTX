#pragma once

struct FnvRemixPlace;

// The readable crash log: on an unhandled exception the crash filter
// (session_log.cpp) has rtx-remix\logs\crash-<YYYY-MM-DD_HH-MM-SS>.txt
// written ([SessionReport] CrashLog), then hands the exception on to the
// filter that was there before (Crash Logger, NVAC, the game's own). It
// holds, each part on its own so one that fails leaves the others:
//   - the plugins (.esm / .esp) whose forms were found in the registers and
//     on the stack, in the order they were found, and the module that
//     crashed;
//   - the exception: code and name, the module + offset it was raised at,
//     the address an access violation touched, the thread and its name;
//   - every register, annotated: module + offset, the class of the object
//     it points at (MSVC RTTI of the object's vtable), and for a game form
//     its form ID, form type, name and plugin, with a reference's base form
//     and cell;
//   - the call stack (dbghelp's StackWalk64, function names and lines where
//     a PDB sits next to the module);
//   - the top kScanSlots stack slots that hold an object, a form or a code
//     address, annotated the same way;
//   - the mod's and the game's versions, the player's last place, the
//     address space, the engine's load order and every loaded module with
//     its file version.
//
// The text is formatted into a buffer reserved at startup and written with
// WriteFile; every read of game memory is checked with VirtualQuery and runs
// under SEH. The work runs on a thread started at startup (its own stack,
// so a stack overflow can be described too) while the crashing thread waits
// for it, at most kWaitMs. The crashing thread writes it itself when that
// thread is gone (a crash during process exit).
namespace comp::game::crash_log
{
	// DllMain, after the config is read: reserves the buffers and starts the
	// writer thread, which loads dbghelp.dll.
	void start();

	// Present: the thread that renders (FNV's main thread), named in the log.
	void note_render_thread();

	// The engine's load order, "IX Name.esp" per line (nvse_link::load_order),
	// for naming a form's plugin from its form ID's top byte.
	void set_load_order(const std::string& lines);

	// The player's place as last read, for the log.
	void note_place(const FnvRemixPlace& place);

	// The crash filter, on the crashing thread, before it chains on. Writes
	// the crash log once (again after a handler resumed the game, up to
	// kMaxLogs per session) and returns its file name, or null when none was
	// written. Allocates nothing on this thread.
	const char* on_crash(EXCEPTION_POINTERS* ep);

	// The crash filter, after the previous filter returned `verdict`: a
	// handler that resumed the game (EXCEPTION_CONTINUE_EXECUTION) is noted
	// at the end of the log, and the next crash gets a log of its own.
	void after_chain(LONG verdict, const void* previous_filter);

	// An MSVC C++ exception (0xE06D7363): the thrown type, the module that
	// threw it and, for a std::exception, its what() text. Allocation free.
	void describe_cxx_exception(const EXCEPTION_RECORD* er, char* out, size_t out_n);
}
