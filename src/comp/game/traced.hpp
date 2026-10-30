#pragma once

// Accounting for ray-traced draws that did not go through the Remix API
// (docs/fnv-api-only.md). Every traced draw goes through an API path; one no
// path can take is either
//   * dropped: not drawn at all (the normal case), or
//   * drawn through D3D9, the allowed exceptions: the Pip-Boy's own
//     first-person draws while it is not closed, and every draw at the main
//     menu or while loading (reason "pipboy", api::pipboy_draw); and a draw
//     whose albedo is in a runtime list only D3D9 draws can carry (reason:
//     the list, e.g. "rtx.lightConverter"; see d3d9_only in api_rtxconf.hpp).
// Each (route, reason) pair is logged once when first seen, and every
// [RemixApi] StatsIntervalFrames frames one "traced->D3D9" status line gives
// the per-frame averages by route and reason, and the build budget's queue
// (api::budget_status). Outside the Pip-Boy, d3d9= on that line may show only
// those lists (and DP_PASS:no declaration).
namespace comp::game::traced
{
	// `route` and `reason` must be string literals (or other static strings):
	// they are kept by pointer.
	void note_dropped(const char* route, const char* reason);
	void note_d3d9(const char* route, const char* reason);

	// Present: the periodic status line.
	void on_present();
}
