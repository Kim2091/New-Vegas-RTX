#pragma once
#include <cstdint>

// The only refcount code in the plugin. A pin is exactly what an engine
// NiPointer holds: InterlockedIncrement on NiRefObject+0x04 to take it, and
// the engine's own DecRefCount (fnv_exe dec_ref_count: InterlockedDecrement, and at zero
// the delete through vtable slot 1) to give it back, so a pinned object
// can't be freed or its address reused while a map is keyed by it.
//
// Each pin is (object, tag); the tag is its owner (a reference, or one of
// the kTag constants). Releases run on the main thread only: at the next
// main loop after they are scheduled (so the wrapper can still read the
// object while it handles the event that announced its end), or at once for
// release_all (PreLoadGame, NewGame, ExitToMainMenu). The live count is in
// the facts; it must reach zero at the main menu.

namespace fnvr::pins
{
	constexpr uintptr_t kTagEffect = 1;    // an active-list light no reference owns
	constexpr uintptr_t kTagPlayer = 2;    // the player's light

	// Any thread (the OnRefSet3D handler): one reference taken now, adopted
	// by the main thread later.
	void take_raw(uintptr_t obj);
	// Main thread: records a reference taken by take_raw under `tag`.
	void adopt(uintptr_t obj, uintptr_t tag);
	// Main thread: takes and records a reference. False when the object is
	// not a readable NiRefObject (vtable outside the image), or already
	// pinned under that tag.
	bool pin(uintptr_t obj, uintptr_t tag);
	// Main thread: schedules the release of one pin.
	void release(uintptr_t obj, uintptr_t tag);
	// Main thread: every pin, released now.
	void release_all(const char* why);
	// Main loop: runs the scheduled releases; re-checks every pinned
	// object's vtable (an invariant: a pinned object can't have been freed).
	void sweep(uint32_t frame);

	// NiRefObject+0x04 as read now (0 when unreadable).
	uint32_t refcount(uintptr_t obj);
	// References the plugin holds: recorded, scheduled for release, and taken but not
	// adopted yet.
	uint32_t live();
}
