#pragma once
#include "api_common.hpp"

// The material builder shared by the API paths: the statics (draw capture,
// api_statics.cpp, describes a material from the draw's PS slots and
// constants; the scene graph, scene_statics.cpp, from the geometry's
// properties) and the skinned, foliage, unlit FX, particle and first-person
// paths.

namespace comp::game::api::material
{
	// Where a material's addressing came from ([Addressing] log only).
	enum class AddressSource : uint8_t
	{
		Default,   // nobody set it: repeat on both axes
		Stage,     // the albedo's own sampler stage (sampler_from_stage)
		Engine,    // the geometry's shader property (its texture clamp mode)
	};

	// Desc::clamp_raw when no clamp mode was read.
	constexpr uint32_t kNoClampMode = 0xFFFFFFFFu;

	// What the protocol-v2 payload would carry for a lit object draw, with the
	// textures the runtime would read.
	struct Desc
	{
		IDirect3DBaseTexture9* diffuse = nullptr;
		IDirect3DBaseTexture9* normal = nullptr;
		IDirect3DBaseTexture9* glow = nullptr;
		IDirect3DBaseTexture9* height = nullptr;
		// FaceGenMap0 / FaceGenMap1 of a FaceGen draw: a head, body or hand
		// (both or neither). The FaceGen PSes (SKIN2000-2003 / 2010-2012, SLS1017-1020, SM3008 / 009 /
		// 020 / 021 and their LL twins) compute their albedo in gamma space as
		// 4 FaceGenMap1 (BaseMap + 2 FaceGenMap0 - 1). build reproduces it with
		// two terrain layers: BaseMap and the half-float offset 2 FaceGenMap0 - 1
		// at weight 1, times the half-float tint 4 FaceGenMap1, all at the mesh
		// UV. The runtime clamps the sum at 0 and linearises it once. Set only
		// when textures::half_float_supported().
		IDirect3DBaseTexture9* facegen0 = nullptr;
		IDirect3DBaseTexture9* facegen1 = nullptr;
		// The hair PS's LayerMap (PsSlotMap::hair_layer_slot). The PS's albedo
		// is lerp(BaseMap, LayerMap, LayerMap.a) with the BaseMap's alpha: build
		// blends the two as terrain layers, weighted by the LayerMap's alpha
		// (hair_layer_textures widens it to the BaseMap's strand edges).
		IDirect3DBaseTexture9* hair_layer = nullptr;
		// The hair PS's albedo baked from the PS itself (api_hair_bake.hpp),
		// an API texture hash; 0 = none. Set instead of hair_layer: build
		// takes it as the albedo, layer 0 alone with a normal map (the
		// BaseMap stays the material's albedo texture, for the opacity), else
		// the material's albedo texture (its alpha is the BaseMap's).
		uint64_t hair_bake = 0;
		bool normal_role = false;   // PS declares a NormalMap (texture may be null)
		bool gloss_alpha = false;   // kNormalAlphaIsSpecMask
		float gloss = 0.0f;         // Toggles.z, 0 = absent
		uint8_t alpha_func = 0;     // D3DCMPFUNC, 0 = alpha test off
		uint8_t alpha_ref = 0;
		// The one sampler the API material has, for every texture it names
		// (albedo, normal, emissive, height): the albedo's (sampler_from_stage,
		// overridden by the geometry's engine clamp mode, clamp_from_geometry).
		// lss::Mdl::WrapMode per axis; filter lss::Mdl::Filter (0 nearest,
		// 1 linear) from MAGFILTER, as the runtime's D3D9 route takes it
		// (min / mip filters have no API field). In the key: the wrap pair
		// is the clamp mode (wrap_from_clamp_mode is one-to-one).
		uint8_t wrap_u = 1, wrap_v = 1;
		uint8_t filter = 1;
		// Not in the key, for the [Addressing] log: the albedo's sampler stage
		// and what it said, where the addressing came from, the shader
		// property's class and raw clamp mode (kNoClampMode: none read), and
		// the path that built it.
		uint8_t albedo_stage = 0;
		uint8_t stage_wrap_u = 1, stage_wrap_v = 1;
		AddressSource address_source = AddressSource::Default;
		const char* clamp_class = nullptr;
		uint32_t clamp_raw = kNoClampMode;
		const char* path = "?";
		bool plain = false;         // StaticsDebugPlainMaterial: albedo only
		// The draw blends (scene statics, SceneClaimAlphaBlend): the material
		// takes its alpha state (test and blend) from the instance's
		// InstanceInfoBlendEXT (useDrawCallAlphaState), as the runtime's legacy
		// material does for the same draw through D3D9. alpha_func / alpha_ref then ride on
		// the instance (blend_ext), not in the material.
		bool blend = false;
		// Emissive intensity factor (a grey EmittanceColor, SceneGlowGreyTint).
		float glow_scale = 1.0f;
		// The glow map's tint (a coloured EmittanceColor; the unlit FX's
		// MaterialColor): emissiveColorConstant, which the runtime multiplies
		// into the emissive texture only with rtx.fnvProtocol.apiGlowTint
		// (glow_tint_ready()). White = untinted.
		float glow_tint[3] = { 1.0f, 1.0f, 1.0f };
		// >= 0: the emissive intensity itself instead of
		// rtx.fnvProtocol.glowEmissiveIntensity * glow_scale (unlit FX).
		float emissive_intensity = -1.0f;
		// MaterialInfoOpaqueAlbedoTintEXT (0..16 per channel, gamma space):
		// the part of a tint above 1 that vertex colours can't hold. Chained
		// only when not white and the bridge client forwards the struct
		// (albedo_tint_supported()).
		float albedo_tint[3] = { 1.0f, 1.0f, 1.0f };
	};

	// A plain alpha-over blend (SrcAlpha / InvSrcAlpha) with a GREATER /
	// GREATEREQUAL alpha test becomes a cutout: the test stays on the
	// material and the blend is dropped. The test already removes what the
	// game cuts out, and what passes it is mostly alpha 255, so the game
	// shows such a surface as opaque with holes. As a blended surface the
	// runtime would trace it as translucent (stochastic alpha blend, alpha-
	// blend object mask, blended opacity micromaps) instead of as an
	// alpha-tested occluder. Call after alpha_func and blend are set from a plain
	// alpha-over state; a draw-state blend (no alpha_func) is left as is.
	// True when it changed `m`.
	bool blend_test_as_cutout(Desc& m);

	// Cache key: texture pointers + creation generations + the scalars.
	uint64_t key(const Desc& m);

	// D3DTEXTUREADDRESS -> lss::Mdl::WrapMode (CLAMP 0, WRAP / MIRRORONCE 1
	// Repeat, MIRROR 2, BORDER 3 Clip).
	uint8_t wrap_mode(DWORD address);

	// The albedo's own sampler: ADDRESSU / ADDRESSV / MAGFILTER of `stage`,
	// the sampler stage the PS names as Diffuse (psmap->slot(Diffuse)), not
	// stage 0. The API material samples every texture with it. Sets
	// wrap_u / wrap_v / filter, albedo_stage, address_source = Stage.
	void sampler_from_stage(IDirect3DDevice9* dev, uint8_t stage, Desc& m);

	// Gamebryo TexClampMode (the shader property's "Texture Clamp Mode";
	// S = U, T = V, as the NiDX9 table 0x126F0B0 maps it): 0 CLAMP_S_CLAMP_T,
	// 1 CLAMP_S_WRAP_T, 2 WRAP_S_CLAMP_T, 3 WRAP_S_WRAP_T. False (m
	// untouched) for anything else.
	bool wrap_from_clamp_mode(uint32_t mode, Desc& m);

	// The geometry's engine clamp mode: its shade property (GEO_props slot
	// kPropShade) read by exact class (scene_graph::read_clamp_mode:
	// BSShaderPPLightingProperty / Lighting30ShaderProperty +0xD8,
	// BSShaderNoLightingProperty +0x6C). A mode 0..3 overrides the wrap
	// pair (address_source = Engine); call after sampler_from_stage. Records
	// clamp_class / clamp_raw whenever the property was read. False (wrap
	// untouched: the stage sampler stays) for a null geometry, another
	// class, an unreadable property or a mode above 3. `geo` must be alive:
	// the dispatcher's current_geometry(), or inside begin_resolve.
	bool clamp_from_geometry(uintptr_t geo, Desc& m);

	const char* wrap_name(uint8_t wrap);
	const char* source_name(AddressSource s);

	// [Addressing] mesh line (shared 500-line cap with build's material
	// lines): a mesh drawn with a material that doesn't repeat on some axis,
	// its TEXCOORD0 range and which non-repeating axes it leaves [0, 1] on,
	// where a clamp smears the edge texels into stripes. [Logging] Verbose only.
	void log_mesh_addressing(const char* path, uintptr_t object, uint64_t albedo_d3d9, uint8_t wrap_u,
		uint8_t wrap_v, const std::vector<remixapi_HardcodedVertex>& verts);

	enum class Decline : uint8_t { None, RtxConf, Texture, Build };

	struct Built
	{
		remixapi_MaterialHandle handle = nullptr;
		std::vector<uint64_t> textures;   // references taken on api::textures (kept on a decline too: the caller releases)
		// The albedo is in rtx.ignoreBakedLightingTextures (or the runtime
		// ignores all vertex colour): no vertex colour for its meshes.
		bool no_vertex_color = false;
		// A list naming the albedo (Decline::RtxConf: the D3D9-only one).
		const char* list = nullptr;
		// The instance categories of the lists naming the albedo, and whether
		// the material dropped its alpha state for one (ignoreAlphaOnTextures:
		// no test, no blend; the instance must not blend either).
		uint32_t categories = 0;
		bool alpha_dropped = false;
		// The albedo's D3D9 image hash (the one the Remix texture picker and
		// rtx.conf lists show), 0 when it can't be reproduced.
		uint64_t albedo_d3d9 = 0;
		Decline why = Decline::None;
	};

	// Builds the API material of `m` (hash = next_object_hash(key, domain)),
	// or declines it with a reason. A built material that doesn't repeat on
	// some axis, or whose engine clamp mode overrode a different stage
	// sampler, logs one [Addressing] line (at most 500 in all).
	void build(const Desc& m, uint64_t key, uint64_t domain, Built& out);

	// Stage 0 of the FFP route: COLOR = TEXTURE * DIFFUSE (vertex colour),
	// ALPHA = TEXTURE; no blending, no draw-call alpha test (the material
	// carries the cutout).
	remixapi_InstanceInfoBlendEXT vertex_color_blend();

	// A blended draw's instance alpha state (Desc::blend), as the FFP route
	// gives the same draw through D3D9: plain alpha blending (SrcAlpha, InvSrcAlpha for
	// colour and alpha), the draw's alpha test (D3DCMPFUNC, 0 = off) and,
	// with vertex colour, ALPHA = TEXTURE * DIFFUSE (the route's stage 0
	// MODULATE; without it, TEXTURE alone, since the mesh's colour is white).
	remixapi_InstanceInfoBlendEXT alpha_blend(uint8_t alpha_func, uint8_t alpha_ref, bool vertex_color);

	// The draw's own raster state as an instance's alpha state, for a
	// material built with Desc::blend (useDrawCallAlphaState): blend enable,
	// factors and op (separate alpha factors when the device has them), the
	// alpha test, the colour write mask and TEXTUREFACTOR; stage 0 COLOR =
	// TEXTURE x VertexColor0 and ALPHA = TEXTURE x VertexColor0 (as the
	// unlit / particle FFP routes set them) when `vertex_color`, else
	// TEXTURE alone; the vertex colour is not baked lighting (unlit FX and
	// particles carry real colour in it). `drop_alpha` (rtx.conf
	// ignoreAlphaOnTextures): no blend, no test. False, with `reject`, for a
	// factor / op / function the instance can't carry (BOTHSRCALPHA,
	// BLENDFACTOR, ...).
	bool draw_state_blend(IDirect3DDevice9* dev, bool vertex_color, bool drop_alpha,
		remixapi_InstanceInfoBlendEXT& out, const char*& reject);

	// ---- Decals ----
	// Remix blends a surface onto the one under it only when its instance
	// carries DECAL_STATIC / DECAL_DYNAMIC. An untagged overlay coplanar with
	// a floor or wall is traced as a surface of its own: primary rays keep
	// the wall under its cut-out texels (rtx.resolveKeepsCoplanarSurfaces),
	// indirect rays traced with TraceRay pass through it.
	// decal_of decides it the same way on every path that carries world
	// geometry (scene statics, draw capture, unlit), first rule that applies:
	//   * EngineDecal / EngineDynamicDecal / EngineAlphaDecal: the geometry's
	//     shader property (any BSShaderProperty subclass, by RTTI) has flags1
	//     Decal_Single_Pass (bit 26) / Dynamic_Decal_Single_Pass (bit 27) or
	//     flags2 Alpha_Decal (bit 21). The engine monitor plugin sets bit 26 at
	//     load on the blended overlays its path rules name (NVStain and the
	//     like), so those arrive here with the engine's own flag.
	// The rtx.conf decal lists (rtx.decalTextures, ...) already ride on the
	// material's categories (Built::categories); decal_of reports them as rule
	// RtxConf when nothing else applied. DECAL_DYNAMIC for the dynamic-decal
	// bit or a draw that moves; DECAL_STATIC otherwise. [RemixApi]
	// SceneDecalCategory off: no decal rule tags anything (the lists still do).
	enum class DecalRule : uint8_t { None, EngineDecal, EngineDynamicDecal, EngineAlphaDecal, RtxConf };
	const char* decal_rule_name(DecalRule r);

	struct Decal
	{
		uint32_t categories = 0;      // DECAL_STATIC or DECAL_DYNAMIC (0: not a decal by these rules)
		DecalRule rule = DecalRule::None;
		// What was read, for the [Decal] log.
		const char* cls = nullptr;    // the shader property's class; null: none / not a BSShaderProperty
		uint32_t flags1 = 0, flags2 = 0;
		bool blended = false;
		bool state = false;           // the D3D9 state below was read
		DWORD src = 0, dst = 0, zwrite = 0, depth_bias = 0, slope_bias = 0;
	};

	// `geo`: the draw's NiGeometry, alive (the dispatcher's current_geometry()
	// or inside begin_resolve); 0 = none (D3D9 rule only). `dev`: the device
	// with the draw's state bound; null = no D3D9 rule. `material_categories`:
	// the material's rtx.conf categories (rule RtxConf when they hold a decal
	// bit and nothing else applied). `moving`: the draw's placement changed
	// (DECAL_DYNAMIC).
	Decal decal_of(uintptr_t geo, IDirect3DDevice9* dev, uint32_t material_categories, bool moving);

	// One [Decal] line per (path, object, albedo), at most 512 a session,
	// for an alpha-blended draw or one decal_of tagged: path, object (the
	// NiGeometry, else the path's draw key), the albedo's D3D9 hash (the
	// Remix texture picker's), shader class, flags1 / flags2, blend factors,
	// z-write, depth bias, and the categories with the rule that set them.
	// [Logging] Verbose only.
	void log_decal(const char* path, uintptr_t object, uint64_t albedo_d3d9, const Decal& d);

	// The instance categories hold DECAL_STATIC / DECAL_DYNAMIC (decal_of or
	// an rtx.conf decal list).
	bool is_decal(uint32_t categories);

	// A decal layer of a model the engine monitor's rules never see (the
	// first-person weapon, skinned parts): the geometry's diffuse (its
	// shader property's PPL texture) has "decal" in its file name, the usual
	// naming of overlay meshes (an engraving, a logo) laid over a part of the
	// same model. Its transparency comes from the texture.
	bool named_decal_layer(uintptr_t geo);

	// ---- Hair ----
	// The hair PS (SM3002...: a HairTint constant) colours its texture by
	// 1 + g (2 HairTint - 1), g = the vertex colour's green. The factor varies
	// per vertex, which a material-wide tint can't express, so every API path
	// bakes it into the mesh's vertex colours.
	// hair_tint: the PS being drawn is a hair PS; returns its tint from the
	// bound pixel-shader constants.
	bool hair_tint(const shared::common::PsSlotMap* psmap, float (&tint)[3]);
	// One vertex's colour for the hair PS (alpha 255: its alpha ignores the
	// vertex colour), as the runtime's B8G8R8A8. The factor reaches 2 (a light
	// HairTint); a vertex colour holds at most 1, so `scale` (per channel, or
	// null for 1) is the part the material carries (Desc::albedo_tint).
	uint32_t hair_color(float green, const float (&tint)[3], const float* scale = nullptr);
	// That part: max(1, 2 HairTint) per channel when the runtime takes an
	// albedo tint (albedo_tint_supported), else 1. Returns whether any is > 1.
	bool hair_scale(const float (&tint)[3], float (&scale)[3]);
	// The hair material's albedo tint: hair_scale times [Hair] Brightness (the
	// vertex colours divide by hair_scale alone). False when the runtime
	// takes no albedo tint.
	bool hair_material_tint(const float (&tint)[3], float (&albedo_tint)[3]);
	// One [Hair] line per (path, PS, tint): what the hair got. [Logging] Verbose only.
	void log_hair(const char* path, uint32_t ps_hash, const char* texture, const float (&tint)[3]);

	// The vertex colour's alpha joins a material's opacity only when the
	// shader property has Vertex_Alpha (flags1 bit 3); the engine's shaders
	// ignore it otherwise. Many meshes have vertex colours (flags2
	// Vertex_Colors) without Vertex_Alpha: decals (paper litter, The Tops wall
	// decal) and glass, whose vertex alpha can be 0. alpha_blend /
	// draw_state_blend would modulate their opacity by an alpha the game never
	// shows. Turns `ext`'s TEXTURE x VertexColor0 alpha into TEXTURE alone for
	// such a geometry; true when it did. A geometry whose shader property
	// could not be read (Decal::cls null) is left as is.
	bool shader_alpha(const Decal& d, remixapi_InstanceInfoBlendEXT& ext);

	// The runtime blends a decal only where the ray hits its FRONT face
	// (resolve.slangh: rayInteraction.frontHit). A decal the game draws
	// two-sided (cull mode NONE) is visible from both sides in D3D9, whatever
	// its winding: its mesh carries every triangle a second time, wound the
	// other way, so one of the two faces the camera (append_reversed). A
	// single-sided decal keeps its triangles.
	void append_reversed(std::vector<uint32_t>& tris);

	// What a decal mesh was built from, for the [DecalRoute] line.
	struct MeshFacts
	{
		// Triangles whose winding normal cross(b - a, c - a) agrees /
		// disagrees with their vertex normals (before append_reversed).
		uint32_t wind_agree = 0, wind_disagree = 0;
		// The vertex colours' alpha range (255..255 without vertex colours).
		uint8_t alpha_lo = 255, alpha_hi = 255;
		bool reversed_copy = false;
	};
	MeshFacts mesh_facts(const std::vector<remixapi_HardcodedVertex>& verts, const std::vector<uint32_t>& tris,
		bool vertex_color);

	// One [DecalRoute] line per (path, object, mesh), at most 512 a session,
	// for every decal-tagged instance: what the runtime gets and how it will
	// trace it. The runtime's own [DecalInstance] line (remix-dxvk.log) shows
	// what it did with it.
	struct DecalRoute
	{
		const char* path = "";
		uintptr_t object = 0;
		uint64_t albedo_d3d9 = 0;
		uint64_t mesh_key = 0;
		uint32_t categories = 0;
		const char* instance = "";            // "retained" / "per frame"
		bool draw_alpha = false;              // the material has useDrawCallAlphaState (the instance's ext is its alpha)
		uint8_t alpha_func = 0, alpha_ref = 0;   // the material's own test (D3DCMPFUNC; 0 none), when !draw_alpha
		const remixapi_InstanceInfoBlendEXT* ext = nullptr;
		bool vertex_alpha_dropped = false;    // shader_alpha changed ext
		bool double_sided = false;
		MeshFacts mesh;
	};
	// One [DecalRoute] line per routed decal. [Logging] Verbose only.
	void log_decal_route(const DecalRoute& r);

	// The draw blends additively (ONE / SRCALPHA source, ONE destination):
	// the runtime already turns such a surface into emission.
	bool draw_is_additive(IDirect3DDevice9* dev);

	// The instance's blend is alpha-over (SRCALPHA / INVSRCALPHA, or the
	// inverted pair): a translucent card such as smoke, dust or steam.
	// The runtime keeps such a surface in the primary TLAS, where only
	// stochastic alpha blending (off in the Medium and Low graphics presets)
	// keeps it translucent; tagged PARTICLE it is resolved in the unordered
	// pass at every preset.
	bool translucent_card(const remixapi_InstanceInfoBlendEXT& blend);

	// Desc::glow_tint is honoured: [RemixApi] ApiGlowTint is on and
	// rtx.fnvProtocol.apiGlowTint was pushed to the runtime a few frames
	// ago. glow_tint_pending(): on, but the push is not settled yet (a
	// caller that would otherwise refuse for good waits instead).
	bool glow_tint_ready();
	bool glow_tint_pending();

	// The bridge client forwards MaterialInfoOpaqueAlbedoTintEXT (checked
	// once, by its log text in the client image). An older client drops the
	// struct (and logs a warning per material), so it is never chained there.
	bool albedo_tint_supported();

	// Once per frame (api::end_frame): pushes rtx.fnvProtocol.apiGlowTint
	// to the runtime whenever [RemixApi] ApiGlowTint changes.
	void push_runtime_options();
}
