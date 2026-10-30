#pragma once
#include <cstdint>
#include <vector>

// The decal flag. Runs inside the owners walk:
// an alpha-blended overlay the path rules in fnv_remix_nvse.ini name gets
// flags1 bit 26 (Decal_Single_Pass) on its shader property, so the engine
// draws it as it draws real decals (no z-write, the decal depth bias) and
// every wrapper path tags it DECAL_STATIC through the engine-flag rule it
// already has.
//
// A property gets the bit only when all of these hold, so the data keeps
// the engine's meaning:
//   * its class is BSShaderPPLightingProperty, Lighting30ShaderProperty or
//     BSShaderNoLightingProperty (exact vtable);
//   * none of Decal_Single_Pass, Dynamic_Decal_Single_Pass, Alpha_Decal is
//     set already (a NIF or another mod decided);
//   * every geometry of the walk that uses it is blended SrcAlpha /
//     InvSrcAlpha with no alpha test, and matches a rule: its diffuse
//     texture path (PPL / Lighting30) or its reference's STAT model path;
//   * the decal path's code is vanilla (FNVR_F_DECALS).
// Blended STAT / MSTT geometry that matches no rule but is flat is logged
// as a candidate, never written. Main thread only.

namespace fnvr::decals
{
	void load_config();
	// One reference's walk: its geometry.
	void on_walk(uintptr_t ref, uint8_t base_type, uintptr_t base_form, const std::vector<uintptr_t>& geos);
	// Main loop: the status line.
	void tick(uint32_t frame);
}
