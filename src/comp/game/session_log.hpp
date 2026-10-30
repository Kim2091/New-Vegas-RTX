#pragma once

// [Session] lines in rtx-remix\logs\remix-comp.log for bug reports, all cheap:
//   - once, at the first Present: Windows version, GPU, driver and VRAM, the
//     desktop mode, the Remix bridge version, the DLLs loaded from the
//     game folder (NVSE plugins, ENB / ReShade and other d3d9 / dxgi chains)
//     and NVTF's frame-timing and threading switches from nvtf.ini;
//   - on a change: the top menu (opened / closed);
//   - every 60 s one status line: frame rate, average and worst frame time,
//     game state, interior / exterior grid, weather, lights sent, live
//     retained instances, the address space used and its largest free block,
//     and the warnings, errors and held-back lines since the previous status
//     line;
//   - with it one [Memory] line: the address space by region type (images,
//     mapped views, private memory), the bridge client's CPU copies of
//     textures and buffers, and how the D3DPOOL_DEFAULT textures were handled
//     (tex_pool.hpp);
//   - every 30 s of in-game frames one [Perf] line: a verdict (the game
//     thread or the renderer holds the frame rate), frame time percentiles,
//     the game thread's frame split into engine, wrapper draws, wrapper end of
//     frame and the wait in the bridge's Present, the wrapper draws by route
//     and the end of frame by part, its CPU time, a sampled
//     profile of the game thread by module and wait (sampler.hpp), the place
//     and the scene size; a slow window adds one [PerfSlow] line (a few per
//     session) with the detailed profile, and [PerfSummary] sums up every 10
//     minutes and at exit.
// It also installs the crash filter: on an unhandled exception it has the
// crash log written (crash_log.hpp), then writes the exception code and
// address, for a C++ exception the thrown type, the module that threw it and
// its what() text, the address space and the [Memory] breakdown, the call
// chain, the code addresses on the stack and the crash log's name, then the
// last 64 log lines (written or held back), and the filter that was there
// before runs.
// Per frame it costs about 30 QueryPerformanceCounter calls (two per draw, one per
// end-of-frame part), a route-table lookup per draw, a vector append and a few
// compares, a few microseconds.
namespace comp::game::session_log
{
	// Present, after api_submit::on_present.
	void on_present();

	// Present, after the bridge's Present returns. The times are QueryPerformanceCounter ticks:
	// `end_of_frame` is the wrapper's work before the bridge's Present,
	// `present_wait` is the time the bridge's Present blocked.
	void note_present(int64_t end_of_frame, int64_t present_wait);

	// A draw call's time through the wrapper, bridge send included, in QueryPerformanceCounter ticks.
	void note_draw(int64_t ticks);

	// One named part of the wrapper's end-of-frame work. `name` must be a string literal.
	void note_part(const char* name, int64_t ticks);

	// Process exit: writes [PerfSummary].
	void on_exit();

	// The first Direct3DCreate9: logs the NVSE plugins and game-folder DLLs, so a crash while the
	// renderer starts still leaves the list in remix-comp.log.
	void on_d3d_create();

	// Times one draw call for note_draw.
	struct draw_timer
	{
		LARGE_INTEGER start;
		draw_timer() { QueryPerformanceCounter(&start); }
		~draw_timer()
		{
			LARGE_INTEGER end;
			QueryPerformanceCounter(&end);
			note_draw(end.QuadPart - start.QuadPart);
		}
		draw_timer(const draw_timer&) = delete;
		draw_timer& operator=(const draw_timer&) = delete;
	};
}
