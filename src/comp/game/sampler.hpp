#pragma once

// A sampling profiler for the game thread, always on, for the [Perf] lines.
// A background thread wakes 25 times a second (30 to 50 ms apart), and
// while the game is in game (not menus or loading) it suspends the game
// thread, reads its instruction pointer and the top 4 KiB of its stack,
// resumes it, and then counts the sample:
//   - in a module's own code: by module, and by 256-byte block of that
//     module's code (module+RVA, resolved offline against the exe or a PDB);
//   - in system code (ntdll, kernel32, kernelbase, win32u, user32, the CRT):
//     by kind (sleep, wait, lock, message wait, file I/O, other kernel call
//     or plain system code) and by the first return address on the stack
//     that is in a non-system module, the caller.
// A sample is held until the frame it falls in ends: frame_done() adds it to
// the window when that frame is counted in game, and drops it otherwise
// (menus, loading, a frame over 1 s such as the game idling in the
// background without presenting), so the profile covers the same frames as
// the window's frame times and its rate is per counted second.
// While the game thread is suspended the sampler only calls GetThreadContext
// and copies stack memory inside the thread's committed stack range: no
// allocation, no locks, no logging. A sample stops the game thread for
// about 100 us (a WOW64 GetThreadContext waits for the suspension to land),
// about 2.5 ms per second; the rate is set by that cost. A 30 s window holds
// about 750 samples.
namespace comp::game::sampler
{
	// The first Present, on the game thread: starts sampling this thread.
	void start();

	// Each Present: samples are taken only while `on`.
	void set_active(bool on);

	// Each Present: the samples taken since the last one go into the window
	// when the frame that just ended is `counted`, else they are dropped.
	void frame_done(bool counted);

	// Starts a [Perf] window.
	void begin_window();

	struct Text
	{
		std::string compact;  // for [Perf]
		std::string detail;   // for [PerfSlow]
	};

	// Ends a [Perf] window of `frames` frames over `seconds`, and adds it to the session totals.
	Text end_window(uint32_t frames, double seconds);

	// The session totals for [PerfSummary].
	std::string session_detail();

	// DLL detach: takes no more samples.
	void stop();
}
