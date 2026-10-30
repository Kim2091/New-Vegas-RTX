#pragma once

// Bug-report lines in fnv_remix_nvse.log:
//   [Location] when the player's interior cell or worldspace changes: its
//              name and form ID (one line per change, never held back);
//   [Status]   every 60 s: the location, attached cells, load state, cut
//              count, pins, owners, read faults and held-back log lines.
// Main thread. Per frame it compares the cell serial and the clock; the
// engine is read only when the attached cells changed or a line is due.
namespace fnvr::status
{
	void tick();

	// The player's cell, its worldspace and the player's position, read now.
	// False when there is no player or it has no cell.
	bool place(FnvRemixPlace& out);
}
