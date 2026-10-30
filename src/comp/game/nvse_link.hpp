#pragma once
#include "shared/fnv_remix_plugin_api.h"

// The wrapper's side of the engine monitor plugin
// (Data\NVSE\Plugins\fnv_remix_nvse.dll): it finds the plugin, checks the
// ABI, caches the plugin's facts once per frame and drains its events at
// Present. The plugin is found, never the reverse.
//
// Without the plugin (or with a mismatched one) every fact reads as zero,
// features_ok is 0, and the engine features that need it are off: scene
// statics, keep-culled residency, the stable origin, the Pip-Boy live
// background. One loud log line and an overlay header line say so.
//
// The plugin answers on the game's main thread only, and only once its
// startup report ran (xNVSE's DeferredInit). Until the first ready answer
// nothing is known (has() is false); after it, an answer that isn't ready
// (the loading screen renders on its own thread) leaves the last ready
// facts and cells standing, and owner_of / lights / the event drain do
// nothing on that thread (live() is false).

namespace comp::game::nvse_link
{
	// First BeginScene of each frame: looks for the plugin until it is found
	// or the search gives up (kSearchFrames).
	void on_begin_scene();

	// Present, before the scene-graph ticks: drains the plugin's events,
	// logs them and hands them to their consumers.
	void on_present();

	bool linked();

	// Linked, ready, and this is the thread the plugin answers on.
	bool live();

	// This frame's facts (read from the plugin at the frame's first call).
	const FnvRemixFacts& facts();

	// linked() and every bit of `features` holds this frame.
	bool has(uint32_t features);

	// The attached cells, roots read now. 0 without the plugin.
	uint32_t cells(FnvRemixCell* out, uint32_t max);

	// The reference that owns geometry `geo`, checked now (its chain still
	// reaches the pinned root, app-culled, disabled). Call it while geo
	// can't die: inside scene_graph::begin_resolve(geo), or while the engine
	// renders it. False: unknown (no owner map, or the geometry was never
	// seen under a reference).
	bool owner_of(uintptr_t geo, FnvRemixOwner& out);

	// The geometries the owner map gained since the last call (all of them
	// at the first call): the loaded scene graph's membership, fed by the
	// plugin's reference walks. 0 without the plugin's owner map.
	uint32_t take_members(FnvRemixMember* out, uint32_t max);
	// The next take_members hands over every owned geometry again. False:
	// not asked (no owner map on this thread).
	bool resend_members();

	// The light registry, values copied now. 0 without the plugin.
	uint32_t lights(FnvRemixLight* out, uint32_t max);

	// The player's cell and position, read now. False without the plugin,
	// off its thread, or with no player in a cell.
	bool place(FnvRemixPlace& out);

	// The engine's loaded plugin files in load order, "IX Name.esp" per
	// line. Empty without the plugin, off its thread, or before the game's
	// data is loaded.
	std::string load_order();

	// A line in the plugin's log (fnv_remix_nvse.log). Any thread; nothing
	// without the plugin.
	void plugin_log(const char* tag, const std::string& text);

	// The plugin is missing, refused or mismatched: the line for the overlay
	// header ("" when linked or still searching).
	const std::string& problem();

	// Calls into the plugin since startup and their time in
	// QueryPerformanceCounter ticks, for the [Perf] line.
	void call_totals(uint64_t& calls, int64_t& ticks);
}
