#pragma once

// VATS hit chance with ranged weapons ([VATS] in remix-comp.ini).
//
// The engine scales a gun's hit chance on a limb by that limb's visibility:
// chance x ((1 - f) + f x visibility) with f = fVATSScreenPercentFactor
// (1.0). The visibility is partial / entire pixels of two occlusion queries
// over the engine's own draw of the target (VATSEffect, fnv_exe
// vats_limb_visibility). Under Remix the target goes out through the Remix
// API and is never rasterised, so both counts are 0 and every limb reads 0%.
//
// The hook answers that function with a line-of-sight estimate: rays from
// the player (the engine's Actor line of sight to a point, fnv_exe
// actor_los_to_point) to the limb's world position and to points on a ring
// around it, facing the player's eye; visibility = the share that is not
// blocked. A whole-object target (limb -1, e.g. a mine) uses the front of
// its world bound and half its radius. Without a player, a target or a
// position the visibility is 1. Main thread (the VATS menu update).
namespace comp::game::vats
{
	// Game-code patch time: hooks the limb visibility when [VATS] Enabled=1.
	void install();

	// True when the hook is in place (Host Settings: Enabled is live then).
	bool hook_installed();

	// End of frame: writes the verbose per-limb line of this frame's targets.
	void on_frame_end();
}
