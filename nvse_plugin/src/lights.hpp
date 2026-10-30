#pragma once
#include <cstdint>
#include <vector>

// The light registry. Every light it lists is pinned, so its NiLight can't be
// freed and reused for another light while listed (heap replacers recycle a
// freed light's memory at once):
//   placed  lights found in a reference's 3D (the owners walk), owned by that
//           reference until it lets go of its 3D;
//   effect  lights in the ShadowSceneNode's active lists (+0xB4 point,
//           +0xC0 shadow-casting) that no reference owns, released at the
//           main loop once only the plugin's pin holds them (the engine let go);
//   player  the player's LightEffect light (the Pip-Boy light), replaced
//           when the engine builds a new one, released when it is gone.
// The lists are scanned when the wrapper asks (its first world draw of the
// frame, after the engine's update and the queue drain at 0xB5FD60); values
// are copied, the wrapper never reads a light. The position is the parent's
// world transform applied to the light's local one, so a light whose own
// world transform was not refreshed yet still follows what it hangs on.
// Main thread only.

namespace fnvr::lights
{
	// The owners walk found these lights under `ref`'s 3D (all of them).
	void set_placed(uintptr_t ref, const std::vector<uintptr_t>& found);
	void on_owner_dropped(uintptr_t ref);
	// Every light dropped (loads, new game, main menu), before pins::release_all.
	void clear();
	// Main loop: effect lights the engine let go of; the status line.
	void tick(uint32_t frame);

	// The ShadowSceneNode is readable this frame.
	bool ok();
	// Scans the engine's lists and copies the registry (player first, then
	// the lights the engine lists this frame, then the rest).
	uint32_t copy(FnvRemixLight* out, uint32_t max);
	// Lights whose own world position disagreed with parent x local by more
	// than a unit when copied (since start).
	uint32_t fixups();
}
