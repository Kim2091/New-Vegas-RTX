#pragma once
#include <cstdint>

// The attached cells, from xNVSE's OnCellStateChange / OnCellRefsLoaded
// (main thread, applied from the incoming queue), verified against the
// engine's TES grid and interior at every cut and every monitor period.

namespace fnvr::cells
{
	void on_state(uintptr_t cell, uint32_t new_state, uint32_t prev_state);
	void on_refs_loaded(uintptr_t cell);

	// Main loop: refs-loaded fallbacks (queue counters, timeout), periodic resync.
	void tick(uint32_t frame);
	// Compares the tracked set with the engine's grid and interior; adds and
	// drops what disagrees (logged with why).
	void resync(const char* why);

	// Every attached cell has its references loaded (event, poll or timeout).
	bool all_refs_loaded();
	// Registers the references of every attached cell again and tells the wrapper.
	// A load that keeps the same cells drops every owner but sends no 3D or cell events.
	void adopt_all(const char* why);
	uint32_t serial();
	uint32_t count();
	bool interior();
	// The TES grid's centre and size, read now. False when unreadable.
	bool grid(int32_t& cx, int32_t& cy, uint32_t& dim);
	// The attached set, roots read now.
	uint32_t copy(FnvRemixCell* out, uint32_t max);
	// The attached cell whose 3D root is `root`, or 0.
	uintptr_t cell_of_root(uintptr_t root);
	bool attached(uintptr_t cell);
}
