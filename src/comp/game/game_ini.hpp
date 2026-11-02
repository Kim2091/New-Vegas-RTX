#pragma once

// The game's own graphics settings that the mod relies on, set in memory.
//
// The engine keeps every Fallout.ini / FalloutPrefs.ini value in a Setting
// object { vtable, value, "name:Section" } listed in its collection. The game
// reads both files into those objects early in WinMain and calls
// Direct3DCreate9 right after; the renderer, shadow, water and imagespace code
// reads the values later. apply() runs at that first Direct3DCreate9, finds
// each Setting by name in its collection and writes the mod's value. The engine
// writes collections back to the INI files, so install_hook() puts the player's
// values back around each write.
//
// [GameSettings] Apply=0 in remix-comp.ini leaves the game's values alone.
// An unknown executable (shared/fnv_exe.hpp) gets nothing.

namespace comp::game::game_ini
{
	// Once, from the d3d9.dll Direct3DCreate9 / Direct3DCreate9Ex exports,
	// before the call is forwarded. Later calls do nothing.
	void apply();

	// Hooks the engine's INI write (fnv_exe settings_write). Window thread, with
	// the game patches.
	void install_hook();
}
