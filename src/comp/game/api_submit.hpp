#pragma once
#include "api_common.hpp"
#include "engine_class.hpp"

namespace shared::common { struct PsSlotMap; struct VsDesc; }

// Renderer-facing entry points of the Remix-API geometry paths ([RemixApi] in
// remix-comp.ini; docs/fnv-api-only.md). Each try_* is called at the moment the
// game issues the draw, touches no device state, and tells the renderer to
// SKIP the game's D3D9 draw whenever it handled it: drawn through the API (a
// cached mesh with DrawInstance / a retained instance), a pass intentionally
// dropped, or (land, far LOD, water) a draw of the family it recognised but
// could not draw this frame. A traced draw no path takes is not drawn
// (the renderer counts it: game/traced.hpp); only a refusal for the Pip-Boy
// keeps the D3D9 route. Draws whose albedo is in a list the runtime reads only
// on D3D9 draws (rtx.lightConverter, ...: rtxconf::d3d9_only) reach no try_*
// here except try_skinned / try_foliage (which have no D3D9 route and refuse
// them as rtxconf): the renderer keeps them on D3D9.

namespace comp::game::api_submit
{
	// FNV land (SLS2092-2147 family): VS SLS2100/2101 declaration
	// (NORMAL, TEXCOORD0, COLOR0, TEXCOORD1, TEXCOORD2) with a BaseMap[N] at
	// s0 (and NormalMap[N] at s7). Submits a TerrainLayersEXT material whose
	// per-layer weights / vertex tint are baked into small textures. False:
	// not a land draw (a land-shaped placed object, for draw capture).
	bool try_terrain(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap, const api::DrawDesc& d);

	// Far land LOD (SLS2002/2005 VS + 0x36E87D02 PS): positions run through
	// lod_sink's per-vertex sink, albedo = the block's own s0 texture.
	bool try_far_lod(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap, const api::DrawDesc& d);

	// A far-LOD block (shade property LOD_Landscape) drawn by a lit pass,
	// which the engine uses for the blocks around the loaded cells: built
	// like the far LOD, with the sink constants of the frame's far-LOD draws
	// and ObjToCubeSpace from the block's WORLD; not drawn when it can't be.
	// is_lod_block_draw: the engine's current geometry is such a block.
	bool is_lod_block_draw();
	void try_lit_lod_block(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap, const api::DrawDesc& d);

	// Logs every land quad and far-LOD block drawn at the next Present
	// ([TerrainDump]); any thread.
	void request_terrain_dump();

	// Records the route of a land-LOD draw for the two frames after a dump
	// ([LodTrace]); render thread, from frame_stats::note_route.
	void terrain_note_route(const char* route);

	// Water, as engine_class::water names the draw (not None). Surfaces
	// become translucent ANIMATED_WATER meshes; the additive point-light
	// passes are dropped. False: not a water draw.
	bool try_water(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap, engine_class::Water kind,
		const api::DrawDesc& d);

	// The medium of the near water in view (the water type drawn most this
	// frame, settled): the transmittance colour and measurement distance its
	// surface material uses. False before any near water was drawn.
	bool water_near_medium(float color[3], float& distance);

	// Draw capture (api_statics.cpp): every traced world draw no dedicated
	// path takes (FFP_WORLD leftovers, BLENDINDICES / land-shaped /
	// non-indexed / no-NORMAL world draws, the first-person weapon). Taken:
	// drawn this frame, as its retained instance (a stable world draw of a
	// placed NiGeometry, kept while culled) or a DrawInstance (skip the D3D9
	// draw). Skipped: a pass the traced instance replaces (a later pass of
	// the same object, a lit shader's additive / multiply lighting pass, a
	// depth-only pass): skip the D3D9 draw too. Refused: `why` names the
	// reason; the draw is not drawn, except "pipboy" (the renderer keeps the
	// D3D9 route).
	enum class Capture
	{
		Taken,
		Skipped,
		Refused,
	};
	Capture try_static(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap, const api::DrawDesc& d,
		const char*& why);

	// The draw-capture part of the periodic status line.
	std::string statics_status();

	// ~NiGeometry since the last call (the scene-graph dead set): draw
	// capture's retained instances of those geometries are destroyed.
	// Render thread, at Present.
	void statics_on_deaths(const std::vector<uintptr_t>& dead);

	// Skinned actors, from the renderer's SKINNED route for a pass with a
	// diffuse (lighting-only passes are dropped there), with the object's
	// bone palette captured (game::bone_rows / num_bones). One primary RT
	// representation per object and frame: once the API claims an object,
	// its other passes are dropped. See api_skinned.cpp.
	enum class SkinnedPass
	{
		Refused, // not drawn; `why` names the reason
		Taken,   // this frame's DrawInstance went out: skip the draw
		Skip,    // an extra pass of an API object: skip the draw
	};
	SkinnedPass try_skinned(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap, const api::DrawDesc& d,
		const char*& why);

	// The skinned part of the periodic status line.
	std::string skinned_status();

	// Grass batches, SpeedTree leaves and the instanced distant objects /
	// trees (DistLod / DistTree), from the renderer's vertex-expander route.
	// Retained placements with RetainedInstances + StableOrigin, else a
	// DrawInstance per draw. See api_foliage.cpp.
	enum class Foliage
	{
		Declined,   // refused (counted): the draw is not drawn (the Pip-Boy's own draws: the D3D9 expander route)
		Taken,      // drawn through the API this frame: skip the D3D9 draw
		Culled,     // every instance is faded out (nothing to draw): skip the draw
	};
	Foliage try_foliage(IDirect3DDevice9* dev, const shared::common::VsDesc& vs,
		const shared::common::PsSlotMap* psmap, const api::DrawDesc& d);

	// The grass / leaves part of the periodic status line.
	std::string foliage_status();

	// NiGeometry objects destroyed since the last call (the scene-graph dead
	// set): grass batches and leaf placements of those geometries are
	// destroyed. Render thread, at Present.
	void foliage_on_deaths(const std::vector<uintptr_t>& dead);

	// Unlit NoLighting FX, from the renderer's vertex-expander route for a
	// NoLightUv draw: true when the API drew it (skip the D3D9 draw). Logs
	// each distinct draw once ([RemixApi] UnlitDebugLog) whichever way it
	// went. See api_unlit.cpp.
	bool try_unlit(IDirect3DDevice9* dev, const shared::common::VsDesc& vs,
		const shared::common::PsSlotMap* psmap, const api::DrawDesc& d);
	std::string unlit_status();

	// Flipbook particles (and NoLightUv systems streamed into a dynamic VB),
	// from the renderer's vertex-expander route: true when this frame's
	// DrawInstance of the system went out (skip the D3D9 draw). See
	// api_particles.cpp.
	bool try_particles(IDirect3DDevice9* dev, const shared::common::VsDesc& vs,
		const shared::common::PsSlotMap* psmap, const api::DrawDesc& d);
	std::string particles_status();
	// ~NiGeometry since the last call: those systems' meshes are destroyed.
	void particles_on_deaths(const std::vector<uintptr_t>& dead);

	// Once per frame, before Present and before the frame counter moves on:
	// the retained instances' end of frame (hide what wasn't drawn, one
	// batched update, DrawRetainedInstances). No-op while RetainedInstances
	// is off.
	void end_frame_retained();

	// From every BeginScene, after stable_origin::begin_frame: the frame's
	// DrawRetainedInstances(O), once, at its first BeginScene
	// ([RemixApi] RetainedReplayAtFrameStart). No-op otherwise.
	void begin_frame_retained();

	// Once per frame (Present): eviction, texture sweep, stats.
	void on_present();

	// Device reset. API objects live in the Remix runtime, not in the D3D9
	// device, so they survive; only per-frame bookkeeping is dropped.
	void on_reset();

	// Run-time switches (Host Settings), between frames: after the game's
	// Present, before the next frame draws.
	//
	// RetainedInstances (or StableOrigin, which it needs) went off: destroys
	// every retained instance. Land, far LOD, water and unlit FX keep their
	// meshes and draw with DrawInstance from the next draw; grass and leaves
	// are dropped and rebuilt (they draw per frame meanwhile).
	void retained_switched_off();

	// Destroys every API mesh, material and retained instance of every
	// feature, then every texture nothing holds any more, so all of it is
	// built again from the current settings (and texture path) as the game
	// draws it. Draws over the build budget are not drawn until then.
	void rebuild_everything();
}
