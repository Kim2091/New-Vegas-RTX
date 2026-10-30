#pragma once

namespace shared::common { struct PsSlotMap; }

// PS-classifier-driven protocol-v2 setup for an FFP-engaged draw (see
// shared/common/remix_protocol.hpp for the wire format). Used by the
// renderer's Pip-Boy-frame FFP routes (world draws and the expander FX).

namespace comp::game
{
	// Rebinds stage 0 to the PS's diffuse sampler and writes the RS 149 / 169 /
	// 177 / 42 payload for `map`. Call after ffp.setup_albedo_texture(dev, map)
	// and before the draw; the caller still owes ffp.restore_textures.
	//
	// A null map, or a PS without a diffuse role, writes `categories` alone
	// (or clears the protocol when categories is 0) and leaves stage 0 alone.
	//
	// `pack_height`: bind a texture-pack height map ([TexturePacks]) for PSs
	// that don't declare a HeightMap.
	void apply_ps_protocol(IDirect3DDevice9* dev, const shared::common::PsSlotMap* map,
		uint32_t categories = 0, bool pack_height = false);
}
