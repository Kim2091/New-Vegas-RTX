#pragma once
#include <cstdint>
#include <vector>

// The game's own fog, mist and haze cards are hidden, so Remix's volumetrics
// are the only fog in the path-traced scene. Runs inside the owners walk and
// classifies each geometry from its own render data only (no file paths, so
// a mod's fog is handled however it names or places it). A geometry gets
// APP_CULLED (NiAVObject flags bit 0) when all of these hold:
//   * unlit: its shader property is BSShaderNoLightingProperty;
//   * alpha-over: its NiAlphaProperty blends SrcAlpha / InvSrcAlpha (or the
//     inverted pair). Additive glow (light shafts, holograms, neon) stays;
//   * UV-animated: the geometry or one of its properties carries a texture
//     transform controller (NiTextureTransformController, NiUVController);
//   * it is not a particle system (NiParticles): fire, torch, chimney and
//     explosion smoke stay;
//   * its reference is a plain placed reference (REFR), any base form type:
//     actors and projectiles stay;
//   * it is not app-culled already (a NIF or script decided).
// Z-write off (shader flags2 bit 0 clear) is logged as a supporting signal.
// The engine then neither renders it nor draws it into any Remix route.
// Geometry that misses exactly one of the first three signals (and is not
// additive) is logged as a candidate, never hidden. Main thread only.

namespace fnvr::fog
{
	void load_config();
	// One reference's walk: its geometry.
	void on_walk(uintptr_t ref, uint8_t ref_type, uint8_t base_type, uintptr_t base_form,
		const std::vector<uintptr_t>& geos);
	// Main loop: the per-minute status line.
	void tick();
}
