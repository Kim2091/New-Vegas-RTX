#pragma once

namespace shared::common
{
	struct PsSlotMap;
	struct VsDesc;
}

// What the engine is drawing, read from the engine at draw time instead of
// from the bound D3D shaders.
//
// The per-pass dispatcher hook (scene_graph, 0xB994F0) records the pass it
// is rendering: its NiGeometry (pass+0) and its pass type (pass+4, a u16).
// The pass type is the engine's own BSShaderManager pass enum (BSSM_*); the
// shader that renders a pass, its VS / PS pair and its constant layout all
// follow from it. FalloutNV.exe names every value in its pass-name switch
// (0xB5342C, 0x2F5 entries), and the table in engine_class.cpp groups them
// into the kinds below. A draw outside the dispatcher, or with a pass type
// the table leaves Unknown, has no engine answer; the routes then fall back
// to the shader reflection (vs_reflect.hpp / ps_reflect.hpp).
//
// The shader that renders the pass is the geometry's (NiGeometry+0xC0, set
// at attach from the shade property's NIF shader type); its BSShader+0x68 is
// a class id each constructor writes (shader_class). Only the questions the
// pass type can't answer read it.
//
// Draws outside the dispatcher: the volumetric fog (0xB651E0) and the
// immediate-mode accumulator (0xB63320) draw through the geometry
// themselves, and non-BSShader draws (clears, the menus' own renderers)
// have no pass. They get no engine answer.
//
// Cost per draw: two loads of hook-written globals and one byte-table load,
// cached per draw; the shader class adds two guarded reads when asked.
namespace comp::game::engine_class
{
	enum class Pass : uint8_t
	{
		Unknown = 0,          // no pass, or a type the table doesn't name
		Depth,                // z-only, depth / shadow maps, normals, velocity, local map, water depth
		Lit,                  // a lit pass that binds the base texture on s0 (AMBIENT, AMBDIFFTEX, ADT*, AD2/3, TEXTURE, 3XLIGHTING)
		LightOnly,            // a lighting pass without it, s0 = NormalMap (DIFFUSE*, SPECULAR*, AMBDIFFDIRANDPT, ENVMAP, TEXEFFECT, FOG)
		Land,                 // LAND*, LANDAD*, LANDO_Am
		LandLod,              // ADT_Mn, ADT_MnShp, LANDLO_A
		Grass,                // GRASS_* (not the grass shadow passes)
		Leaves,               // LEAVES_AD / ADPt (SpeedTree leaf cards)
		Fronds,               // FRONDS
		DistantTree,          // DISTANT_TREE_BASE
		NoLighting,           // NOLIGHTING, _TexVC, _FALLOFF, _S (textured)
		NoLightingUntextured, // NOLIGHTING_VC / _MAT (vertex colour or material colour only)
		NoLightingPsys,       // NOLIGHTING_PSYS / _PREMULT_ALPHA / STRIP_PSYS
		ParticleSubTex,       // NOLIGHTING_PSYS_SUBTEX_OFFSET(_PREMULT_ALPHA), STRIP_PSYS_SUBTEX
		Particle,             // PARTICLE, PARTICLE_PREPASS (GPU particles)
		Sky,                  // SKY*, SELFILLUM_SKY*, SKYBASEPRE / POST
		Water,                // WATER*, WATER_WADING*, WATER_INTERIOR*, WATER_UNDERWATER
		WaterLod,             // WATER_LOD*
		WaterLight,           // WATER*_SPECULAR_LIGHTING (additive point-light specular)
		WaterStencil,         // WATER_STENCIL*
		Decal,                // GEOMDECAL*, BLOOD_SPLATTER*
		Effect,               // BOLT, BEAM, PRECIPITATION_RAIN
		Tile,                 // TILE
		Count,
	};

	const char* pass_name(Pass p);

	// Pass flags, from the pass's name.
	constexpr uint8_t kFaceGen = 1u << 0;     // an "Fg" variant: the FaceGen head maps are bound
	constexpr uint8_t kPremultiplied = 1u << 1; // NOLIGHTING_PSYS*_PREMULT_ALPHA

	struct Draw
	{
		Pass pass = Pass::Unknown;
		uint8_t flags = 0;
		uint16_t type = 0xFFFF;       // the raw pass type; 0xFFFF outside the dispatcher
		uintptr_t geometry = 0;       // the pass's NiGeometry; 0 outside the dispatcher

		bool known() const { return pass != Pass::Unknown; }
		bool facegen() const { return (flags & kFaceGen) != 0; }
	};

	// The draw in progress (render thread; cached per draw).
	const Draw& current();

	// BSShader+0x68 class ids (each shader's constructor).
	enum class ShaderClass : uint32_t
	{
		Unknown = 0,
		ShadowLight = 1,
		Hair = 2,
		Parallax = 3,
		Skin = 4,
		SpeedTreeBranch = 5,
		TallGrass = 6,
		DistantLod = 7,
		SpeedTreeFrond = 8,
		SpeedTreeLeaf = 9,
		Sky = 0xB,
		GeometryDecal = 0xC,
		Water = 0xD,
		Particle = 0xE,
		Bolt = 0xF,
		Beam = 0x10,
		Lighting30 = 0x11,
		Precipitation = 0x12,
		VolumetricFog = 0x13,
		Tile = 0x14,
		NoLighting = 0x16,
		BloodSplatter = 0x17,
		DistantTree = 0x19,
	};
	// The shader rendering the draw in progress; Unknown outside the
	// dispatcher or when unreadable. Cached per draw.
	ShaderClass shader_class();

	// The pass applies a base (albedo) texture. Unknown when the engine has
	// no answer.
	enum class Tri : uint8_t { Unknown, No, Yes };
	Tri has_base(const Draw& d);

	// ---- Engine versus shader ---------------------------------------------------
	//
	// Every route that asks the engine first notes both answers here: the
	// engine's verdict (or Unknown) and the shader reflection's. Where the
	// engine answered, it decided; the shader answer is only compared. Every
	// [RemixApi] StatsIntervalFrames frames one "[EngineClass]" line gives,
	// per route, the draws decided by the engine, by the shader fallback, and
	// the disagreements. Each disagreement and each fallback is logged once
	// per (route, pass type, VS, PS) with what each side said.
	enum class Route : uint8_t
	{
		Water,
		WaterLod,
		WaterLight,
		Grass,
		Leaf,
		DistTree,
		DistLod,
		ParticleSubTex,
		Unlit,
		Premultiplied,
		Land,
		FarLod,
		NearLod,
		Base,         // the pass carries the albedo (else a lighting-only pass)
		Head,         // a FaceGen head pass
		Count,
	};

	// One routing decision: `engine` (Unknown = no answer: the shader decided)
	// and the shader reflection's answer. Returns the answer the route takes.
	bool decide(Route r, Tri engine, bool shader);

	// The routes' questions, each answered by the engine where it knows and
	// by the bound PS's reflection otherwise (both counted by decide).

	// The pass carries the albedo; false: a lighting-only, depth or untextured
	// pass (FX, normal-only, environment), which no route traces.
	bool carries_base(const shared::common::PsSlotMap* psmap);

	// A FaceGen head pass (the head maps are bound).
	bool facegen_head(const shared::common::PsSlotMap* psmap);

	// The pass outputs premultiplied alpha (NOLIGHTING_PSYS*_PREMULT_ALPHA,
	// the NOLIGHTTEXVCPMA flames).
	bool premultiplied(const shared::common::PsSlotMap* psmap);

	// Land drawn at full detail (the land route): the pass is a LAND* pass;
	// `shader` = the PS reflection's verdict (a BaseMap[N] / NormalMap[N]
	// land layout).
	bool land(bool shader);

	// Land LOD (ADT_Mn, ADT_MnShp, LANDLO_A, or a PS with LOD samplers). With a
	// NORMAL it is the near land LOD, without one the far LOD.
	bool land_lod(const shared::common::PsSlotMap* psmap, bool with_normal);

	enum class Water : uint8_t
	{
		None,
		Near,       // a water surface with the cell's water type
		Distant,    // LOD water (the worldspace's default look)
		Light,      // an additive point-light specular pass
	};
	Water water(const shared::common::PsSlotMap* psmap);

	// The PS slot map the routes read: the reflected one when the bound PS
	// carries a constant table, else, when the engine names the pass, the
	// vanilla layout of that pass (albedo on stage 0, the normal map on 1,
	// FaceGen maps, water and NoLighting constants at the registers every
	// shader package uses). A PS compiled without reflection data then still
	// routes and binds its albedo. Logged once per (PS, pass type).
	const shared::common::PsSlotMap* ps_map(const shared::common::PsSlotMap* reflected);

	// Present: the periodic summary line.
	void on_present();
}
