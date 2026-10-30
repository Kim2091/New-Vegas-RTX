#pragma once

// The hair albedo baked from the game's own pixel shader (ps_interp.hpp), on
// the CPU. No D3D9 or bridge calls: everything here runs on the bake worker
// thread (api_hair_bake.cpp), and the offline tests build it on its own.
//
// A bake runs the draw's PS once per texel of the BaseMap's UV space with the
// lighting neutralised, by CTAB name:
//   * LightData, PSLightColor, ToggleNumLights: 0; EyePosition: (0, 0, 100, 1);
//   * AmbientColor: (1, 1, 1, 1); MatAlpha: (1, 0, 0, 0);
//   * every other declared constant keeps the draw's value;
//   * inputs: TEXCOORD0 is the UV, TEXCOORD3..5 the identity tangent frame,
//     the vertex colour g = 0, everything else (position, fog) 0.
// The program is first specialised for those constants (ps_interp::specialize):
// for the hair PS only the BaseMap / LayerMap taps and their lerp are left,
// T(uv) = lerp(BaseMap, LayerMap, LayerMap.a), alpha BaseMap.a. With g = 0 the
// per-vertex factor 1 + g (2 HairTint - 1) is 1, so the bake doesn't depend
// on HairTint and the factor stays in the vertex colours and the material
// tint.
//
// A bake is refused, with the reason, unless the probes pass on a grid of
// texels, each run unspecialised:
//   * no emissive residue: out(ambient 0) = 0;
//   * linear in the ambient term: out(ambient 1/2) = out(ambient 1) / 2;
//   * separable in the vertex colour: out(g = 1) = T (2 HairTint);
//   * the specialised program gives the unspecialised output;
//   * the output's alpha is the BaseMap's.
// The residual may sample only BaseMap and LayerMap.

#include <cstdint>
#include <string>
#include <vector>

namespace comp::game::shader_bake
{
	enum class TexFormat : uint8_t { Dxt1, Dxt3, Dxt5, Bgra8, Bgrx8 };

	// Bytes of one level as the D3D9 lock lays it out without pitch padding:
	// block rows for DXT, texel rows otherwise.
	size_t level_bytes(TexFormat f, uint32_t width, uint32_t height);

	// One captured mip level (level_bytes() bytes).
	struct Level
	{
		uint32_t index = 0;
		uint32_t width = 0, height = 0;
		std::vector<uint8_t> bytes;
	};

	// A texture as the bake samples it: the whole chain's shape and the levels
	// a bake reads (levels_needed).
	struct Texture
	{
		TexFormat format = TexFormat::Bgra8;
		uint32_t width = 0, height = 0, levels = 0;
		std::vector<Level> captured;
	};

	// The bake's size for a BaseMap of base_w x base_h: the BaseMap's own,
	// halved until neither side is above 1024.
	void bake_size(uint32_t base_w, uint32_t base_h, uint32_t& w, uint32_t& h);

	// The levels a bake bake_w texels wide reads of a texture tex_w wide with
	// `levels` levels: [first, first + count), count 1 or 2 (trilinear).
	void levels_needed(uint32_t tex_w, uint32_t levels, uint32_t bake_w, uint32_t& first, uint32_t& count);

	struct Job
	{
		uint32_t ps_hash = 0;
		std::vector<uint8_t> bytecode;
		float constants[32 * 4] = {};   // the draw's PS float constants c0..c31
		Texture base, layer;             // the samplers named BaseMap and LayerMap
		uint32_t width = 0, height = 0;  // bake_size() of the BaseMap
	};

	struct Result
	{
		bool ok = false;
		std::string refusal;            // why not, when !ok
		// The residual program, instructions separated by "; ".
		std::string residual;
		int instructions = 0, live = 0, varying = 0;
		// Worst probe errors (see the file comment).
		float emissive = 0.0f, linearity = 0.0f, separability = 0.0f, plan_error = 0.0f, alpha_error = 0.0f;
		// The baked chain: B8G8R8A8 texels, or BC3 blocks when `bc3`.
		uint32_t width = 0, height = 0, mips = 0;
		bool bc3 = false;
		std::vector<uint8_t> data;
		double ms = 0.0;                 // wall time of the bake
	};

	// The bake, probes and encoding. Any thread.
	Result bake(const Job& job);

	// The specialised program over the job's bake grid with AmbientColor
	// (a, a, a, 1) and vertex colour (g, g, g, 1): RGBA floats per texel, row
	// by row, alpha 0 where texkill fired. False, with `why`, when the program
	// can't be specialised. For the offline tests.
	bool render(const Job& job, float ambient, float g, std::vector<float>& rgba, std::string& why);
}
