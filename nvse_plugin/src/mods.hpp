#pragma once
#include <cstdint>

// The plugin files (.esm / .esp) the engine loaded, from its DataHandler,
// for the session report. The list is the engine's own, so it is right
// under a mod manager's virtual file system too. Main thread.
namespace fnvr::mods
{
	// One "IX Name.esp" line per loaded file, in load order (IX: the load
	// index in hex), copied to `out` as far as `size` allows and
	// NUL-terminated. Returns the length of the whole list without the NUL;
	// 0 while the DataHandler holds no files.
	uint32_t load_order(char* out, uint32_t size);
}
