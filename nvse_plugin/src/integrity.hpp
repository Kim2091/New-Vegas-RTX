#pragma once
#include <cstdint>

// Code integrity: one site table (xNVSE's event hooks, the wrapper's own
// hooks, the decal path), a startup report and a periodic re-check.
//
// report(): at DeferredInit. The whole .text of FalloutNV.exe against the
// file on disk, coalesced into ranges with the module each patch jumps
// into, then every site's verdict.
// tick(): every [Monitor] Period main-loop frames, each site's bytes again;
// a change is logged (old and new bytes, the new owner) and its feature's
// bit follows the verdict.
//
// Nothing is ever patched back. The pre-write check (never overwrite
// foreign bytes) stays with whoever writes; this only tells after the fact.

namespace fnvr::integrity
{
	// At DeferredInit: xNVSE writes its event hooks during its own start-up,
	// after the plugins load, so no site is judged before this.
	void report();
	// The startup report ran: the feature bits mean something.
	bool reported();
	void tick(uint32_t frame);

	// FnvRemixFeature bits whose sites all hold an accepted state.
	uint32_t features_ok();
	// The verdict line of the sites behind one feature bit ("" when none).
	const char* status(uint32_t feature_bit);
}
