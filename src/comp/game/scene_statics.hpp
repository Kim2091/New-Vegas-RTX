#pragma once

namespace shared::common { struct PsSlotMap; }
namespace comp::game::api { struct DrawDesc; }

// Rigid statics through the Remix API ([RemixApi] SceneGraphStatics). A
// static is in the ray-traced scene while it is in the loaded scene graph
// and the game doesn't hide it, whether or not the engine draws it.
//
//   * Membership: the engine monitor's member feed (nvse_link::take_members)
//     lists every geometry of a loaded non-actor reference, from the
//     plugin's walk of the reference's 3D at OnRefSet3D / OnRefAttach and of
//     every reference of a cell that attaches. Each member is seeded once:
//     classified as below, placed (owner, attached cell, base type, not
//     under a tree / billboard node) and recorded from the scene graph alone:
//     the packed buffers (NiGeometryData+0x34) and their declaration for the
//     mesh, the properties for the material (PPL role 0 / 1 textures,
//     NiMaterialProperty gloss, NiAlphaProperty test, NiStencilProperty
//     two-sided, the engine clamp mode, the texture pack's height map). The
//     resolve claims it like a drawn one; the claim is provisional until its
//     first base pass reaches the renderer once, which confirms it or
//     replaces it with the draw's record in the same tick (the "scene
//     members" status line counts each field that differed). Members that
//     can't be recorded yet (app-culled, disabled or detached; no attached
//     cell; not packed; no D3D texture yet; no build slot; under a switch /
//     LOD node that shows another child) are held and looked at again;
//     land-shaped geometry is left to its draw (land or a placed object).
//   * Discovery at the render-pass dispatcher (0xB994F0) as well: the first
//     pass of a geometry classifies it once (NiTriShape / NiTriStrips, no skin,
//     BSShaderPPLightingProperty, not water / land; alpha-blended only as
//     SceneClaimAlphaBlend allows: plain SrcAlpha / InvSrcAlpha blending with
//     an alpha test by default, never additive / effect blends) and makes it
//     a candidate. Every later pass is a sighting: before the verdict on the
//     pass it stamps the frame and, for a claimed geometry, samples its world
//     pose (movers).
//   * Material: the candidate's first base pass (not blended, depth write on;
//     for a blended candidate its SrcAlpha / InvSrcAlpha pass, depth write
//     either way) that reaches the renderer's FFP_WORLD branch (or a
//     land-shaped draw of a placed object, SceneLandShapeStatics) is recorded
//     with its vertex layout, buffers and draw window. The PS roles,
//     constants and textures that pass binds are built into an API material
//     by api_material (the builder draw capture uses) right there. A PS
//     without Toggles is taken and gets what the protocol gives it (no
//     gloss-in-alpha, no gloss).
//     A blended pass's alpha test and blend ride on the instance
//     (InstanceInfoBlendEXT, the material with useDrawCallAlphaState).
//     rtx.conf lists the API can express become instance categories, the
//     decal rules of api_material decal_of DECAL_STATIC / DYNAMIC
//     (SceneDecalCategory: the shader's decal flags, which the engine monitor
//     sets on named overlays), and a grey glow tint an intensity
//     (SceneGlowGreyTint).
//   * Mesh: between frames, from the geometry's packed D3D9 buffers (the
//     engine frees the CPU UV / colour arrays): NiGeometryData+0x34 ->
//     NiGeometryBufferData must name the very VB / IB the recorded pass
//     bound, and the recorded window is copied from them (bridge CPU shadow,
//     READONLY). One mesh per geometry data + buffers + window + material:
//     clones of one NIF share it.
//   * Scope: references whose base form type is in SceneBaseTypes (by
//     default STAT, SCOL, ACTI, TACT, TERM, CONT, DOOR, LIGH, MSTT, FLOR,
//     FURN and the loose items ARMO, BOOK, CLOT, INGR, MISC, WEAP, AMMO,
//     KEYM, ALCH, NOTE), from the engine monitor's owner map (the reference
//     whose 3D the geometry was found in at OnRefSet3D / OnRefAttach, and the
//     attached cell that 3D hangs in, its anchor). Geometry no reference owns
//     is not claimed, nor geometry an actor owns (hair, hats, eyes, weapons:
//     they move with the skinned body, and draw capture draws them per frame
//     with it), nor anything under a tree / billboard node. Skinned,
//     effect-blended, water and land geometry are refused and counted (per
//     reason, with the passes and FFP_WORLD draws each bucket still costs
//     per frame).
//   * Lifetime is the engine's: ~NiGeometry (dead set, drained each tick),
//     the reference letting go of its 3D (FNVR_EV_OWNER_GONE) and the anchor
//     cell detaching (or its 3D root changing) destroy, in one
//     DestroyRetainedInstances per tick. Culling never hides: an instance
//     stays in the scene while off screen. Hides come from a round-robin
//     poll of engine data only: the owner check (app-culled in the
//     reference's 3D or above it, detached, disabled / deleted, 3D swapped)
//     and, for leaves under a switch / LOD node, the node's active child
//     (NiSwitchNode+0xB0). A hidden instance the engine draws again is shown
//     the same tick.
//   * Movers: the pose sampled at a sighting is compared bitwise; a change
//     goes out as one batched transform update, past a 0.01-unit deadband.
//   * Race: every read of a geometry between frames runs inside
//     scene_graph::begin_resolve / end_resolve (the resolve / destroy
//     handshake), so a destructor on a loader thread can't free it mid-read.
//   * Suppression: once its CreateRetainedInstances is queued the geometry is
//     claimed, and from the next frame its passes are skipped in the
//     dispatcher, only while the retained path can draw this frame and the
//     instance is visible; otherwise the pass reaches the renderer, whose
//     draw capture (api_statics.cpp) draws it through the API that frame.

namespace comp::game::scene_statics
{
	// [RemixApi] SceneGraphStatics.
	bool enabled();

	// Enabled, and RetainedInstances + StableOrigin are on: the scene graph
	// takes the statics it can (draw capture takes the rest per draw).
	bool owns_statics();

	// Present, before the retained end of frame (render thread).
	void tick();

	// The renderer, for an FFP_WORLD-shaped indexed draw (the pass being
	// rendered is sg::current_geometry()): records the candidate's material,
	// layout and buffers from its base pass, or refuses it for what the draw
	// shows (shader, layout, alpha function, glow tint, rtx.conf lists); a
	// provisional claim's base pass confirms or replaces it. True: the
	// renderer drops the draw (its geometry's instance draws it this frame).
	bool on_static_draw(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap, const api::DrawDesc& d);

	// The renderer, for a world draw the statics shape excludes: land-shaped
	// (colour + two texcoords), BLENDINDICES-shaped, or far LOD. The
	// candidate being drawn is refused (terrain / layout / lod), except a
	// land-shaped draw whose PS isn't a land shader and whose geometry hangs
	// under a placed reference (SceneLandShapeStatics): true, and the
	// renderer records it with on_static_draw like any static.
	enum class ShapedDraw : uint8_t { Terrain, BlendIndices, FarLod };
	bool on_shaped_draw(ShapedDraw kind, const shared::common::PsSlotMap* psmap);

	// The pass being drawn belongs to a provisional claim whose passes reach
	// the renderer this frame (to be recorded by on_static_draw): any other
	// route drops it, its instance draws it.
	bool instance_draws_now();

	// The geometry is claimed (it has its scene instance): draw capture drops
	// any retained instance of its own for it.
	bool claimed(uintptr_t geo);

	// Draw capture asks before it makes a stable draw of `geo` a retained
	// instance kept while culled (api_statics.cpp): the geometry is known,
	// not claimed, and owned (not culled, detached or disabled, not under a
	// tree / billboard / switch / LOD node) by a non-actor reference in an
	// attached cell (an actor's rigid parts move with a body that is drawn
	// per frame). Render thread; reads the parent chain under the resolve
	// handshake.
	bool capture_may_retain(uintptr_t geo);

	// The engine monitor's FNVR_EV_OWNER_GONE: reference `ref` let go of its
	// 3D root `root`; every geometry claimed under it is unclaimed.
	void on_owner_gone(uintptr_t ref, uintptr_t root);

	// The engine monitor's FNVR_EV_CELL_REFS_LOADED: the cell's references
	// are in the owner map, so land-shaped geometries refused as terrain for
	// want of an owner are candidates again.
	void on_cell_refs_loaded(uintptr_t cell);

	// Destroys every scene instance (one batch), mesh and material and
	// forgets every geometry; the next frame's statics go through draw
	// capture until they are claimed again.
	void drop_all(const char* why);

	// Host Settings: SceneDebugDrawInstance changed (drops the retained
	// instances; each tick then draws or re-creates as the mode says).
	void on_draw_instance_switched();
}
