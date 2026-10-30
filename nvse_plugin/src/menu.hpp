#pragma once
#include <cstdint>

// The interface state, polled from InterfaceManager (fnv_exe interface_manager): menu mode
// (+0x00C != 1), the top of the open-menu stack (+0x114, u32[10] menu IDs,
// zero-terminated; GetTopMenuID 0x714F00) and the Pip-Boy state machine
// (+0x4BC, 0x70EE80):
//   0 closed, 1 open requested, 2 opening, 3 open, 4 close requested,
//   5 closing (then 0).
// xNVSE sends no menu message; this is a read, at the main loop and at
// every facts() call. Each Pip-Boy state and top-menu change is logged.

namespace fnvr::menu
{
	struct State
	{
		bool ok = false;           // InterfaceManager readable and initialised
		bool menu_mode = false;
		uint32_t pipboy_raw = 0;
		uint16_t top_menu = 0;     // 0: the stack is empty
	};

	State poll();

	// FnvRemixPipBoy for a raw state.
	uint8_t pipboy_of(uint32_t raw);
}
