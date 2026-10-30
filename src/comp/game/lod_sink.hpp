#pragma once

namespace comp::game::lod_sink
{
	// Exact per-vertex replication of FNV's distant-terrain sink.
	//
	// The distant-terrain vertex shader (SLS2002.vso, paired with the no-normal
	// far-LOD pixel shader 0x36E87D02) lowers a vertex only when it falls inside
	// the loaded high-detail rectangle, so the coarse LOD tucks under
	// the real terrain there and stays put everywhere else:
	//
	//   morphedZ = lerp(TEXCOORD1.x, POSITION.z, GeomorphParams.x)
	//   cx       = dot(ObjToCubeSpace[0], (x, y, morphedZ, w))
	//   cy       = dot(ObjToCubeSpace[1], (x, y, morphedZ, w))
	//   inside   = |cx - HighDetailRange.x| < HighDetailRange.z
	//           && |cy - HighDetailRange.y| < HighDetailRange.w
	//   finalZ   = morphedZ - inside * GeomorphParams.y
	//
	// The Remix-API far-LOD path (api_terrain.cpp) runs the shader's arithmetic
	// on the CPU into its mesh positions. Constants come from ffp_state's mirror
	// of the game's VS constant writes:
	//   c8,c9 = ObjToCubeSpace rows 0-1   c12 = HighDetailRange   c19 = GeomorphParams

	// The shader's inputs, for consumers that run the sink themselves (the
	// Remix-API far-LOD path builds its mesh positions with it). 16 floats:
	// c8, c9 (ObjToCubeSpace rows 0-1), c12 (HighDetailRange), c19
	// (GeomorphParams), read from ffp_state's VS constant mirror.
	constexpr int kNumConstants = 16;
	void gather_constants(float out[kNumConstants]);

	// SLS2002.vso's final object-space Z for one vertex (see the formula
	// above). `coarse_z` is the vertex's TEXCOORD1.x; pw is POSITION.w (1
	// for FLOAT3 positions).
	float sunk_z(const float* consts, float px, float py, float pz, float pw, float coarse_z);

	// The shader's morphed Z (lerp(TEXCOORD1.x, POSITION.z, GeomorphParams.x))
	// and its inside test on the morphed position, as sunk_z computes them.
	float morphed_z(const float* consts, float pz, float coarse_z);
	bool inside(const float* consts, float px, float py, float morphed_z, float pw);

	// TEXCOORD1.x as the shader reads it, for the decl types FNV's LOD uses.
	float read_coarse_z(const unsigned char* p, int decl_type);
}

// Per-frame counts of each distant-terrain LOD class, so it is possible to tell
// which one is actually on screen (and therefore which one is z-fighting)
// without guessing from shader hashes.
namespace comp::game::lod_debug
{
	void count_far();
	// A near (with-normal) LOD draw; records its NiGeometry when the
	// dispatcher hook is in.
	void count_near();
	// A land-shaped draw (API_TERRAIN / FFP_TERRAIN): its NiGeometry, so
	// the [LOD] line can say how many near-LOD draws are passes over land
	// quads drawn the same frame (nearOverLand).
	void note_land();
	int far_draws();
	int near_draws();
	void on_present();
}
