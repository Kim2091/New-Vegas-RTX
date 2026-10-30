#pragma once

// Cheap per-frame routing statistics, always collected, independent of the
// [Diagnostics] module. When the engine monitor's game state changes (main
// menu, loading, in game, menu), the route histogram, the RT0 targets and the
// Remix-API instance counts are logged for the previous frame, the change
// frame and the 3 after it ([Frame] lines in remix-comp.log), with [Logging]
// Verbose=1. Rate-limited; nothing is logged while the state stays put.
namespace comp::game::frame_stats
{
	// One routed draw. `tag` is the renderer's route name (a string literal).
	void note_route(const char* tag);

	// The route noted since the last call (nullptr if none), for the draw's
	// time in the [Perf] line's split by route.
	const char* take_route();

	// RT0 bind as the wrapper classified it (after game::on_set_render_target).
	void note_render_target(IDirect3DSurface9* surface);

	// Present, after api_submit::on_present (reads its rolled-over counters).
	void on_present();

	// Device reset: forget every surface address.
	void on_reset();
}
