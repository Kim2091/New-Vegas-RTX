#pragma once
#include <cstdint>

// The water state at the camera, read live (no hooks, no calls). The
// globals are fnv_exe water_*; the addresses below are 1.4.0.525's:
//   underwater  the water system's "camera below the water surface" byte
//               0x11C7A59 (getter 0x4E2180), written each frame by
//               TESWaterSystem::Update 0x4E1BC0: placeable water entered
//               (0x11C7A58) or camera Z < the player's cell water height.
//               It is left stale while the water system is off (0x1189624)
//               or the water setting is off (0x11C7ADC); then it reads false.
//   height      inside placeable water (0x11C7A58 set) its surface Z
//               (0x11C7A5C); otherwise TESObjectCELL::GetWaterHeight 0x5471E0
//               of the player's parent cell, recomputed from its fields:
//               none for an interior or a cell without water (flags +0x24
//               bits 0 / 1), else the cell's +0x50 unless FLT_MAX, else the
//               worldspace's +0xCC unless FLT_MAX, else 0.
// Main thread only.

namespace fnvr::water
{
	struct State
	{
		bool ok = false;           // the globals were readable
		bool underwater = false;
		bool height_valid = false;
		float height = 0.0f;
	};

	State read();
}
