#pragma once
#include <cstdint>

// The owner map: which reference a geometry belongs to, from one walk of the
// reference's 3D when xNVSE says it was set (OnRefSet3D) or attached
// (OnRefAttach). The 3D root is pinned from Set3D until the reference lets
// go of it (OnRefUnset3D, OnNonPersistentFormUnload, a new 3D, its cell
// detaching), so the map's keys can't be recycled under it. A reference's
// end is announced to the wrapper (FNVR_EV_OWNER_GONE) before the root's
// pin goes, at the next main loop.
//
// Main thread only; the handlers only queue.

namespace fnvr::owners
{
	void on_set3d(uintptr_t ref, uintptr_t root);   // root already taken by pins::take_raw
	void on_unset3d(uintptr_t ref);
	void on_attach(uintptr_t ref);
	void on_form_unload(uintptr_t form);
	void on_cell_detached(uintptr_t cell);
	void on_cell_attached(uintptr_t cell);
	// Every reference in the cell's list whose 3D is live and that no event
	// put in the map is walked as an attach would. A cell re-attached from
	// the cell buffer keeps its references' 3D, so xNVSE sends no Set3D.
	void adopt_cell(uintptr_t cell, const char* how);
	// Every owner dropped (loads, new game, main menu), before pins::release_all.
	void clear(const char* why);
	// Main loop: the event-arrival invariant and the status line.
	void tick(uint32_t frame);

	// The owner of one geometry, checked now.
	bool owner_of(uintptr_t geo, FnvRemixOwner& out);

	// The member feed: geometries of non-actor references found by walks
	// since the last call. The first call (and the first after an overflow)
	// queues every owned geometry.
	uint32_t take_members(FnvRemixMember* out, uint32_t max);
	// The next take_members queues every owned geometry again.
	void resend_members();

	// Ref 3D events still arrive after cells attach (the invariant holds).
	bool events_ok();
	uint32_t count();
	uint32_t geometry_count();

	// The reference is disabled or deleted now (TESForm flags 0x800 / 0x20).
	bool disabled(uintptr_t ref);
}
