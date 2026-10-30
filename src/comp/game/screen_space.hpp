#pragma once

// Screen-space draws and the path-traced scene ([FFP] HideScreenSpaceDraws,
// [Diagnostics] ScreenSpaceLog).
//
// An orthographic draw (PASS_2D: HUD, menus, the Pip-Boy's cursor and
// highlight quads, full-screen overlays) goes to the device with the game's own
// vertex shader. With rtx.useVertexCapture the runtime captures the VS output
// of every such draw made before RTX injection and turns it back into a world
// position with the device's FFP VIEW / PROJECTION (d3d9_rtx.cpp
// prepareVertexCapture: viewToWorld = inverse(VIEW), invProj = inverse(PROJ)).
// A shader draw carries none of its own: the device still holds whatever the
// last FFP-engaged draw set: a perspective projection, often the zoomed
// Pip-Boy / viewmodel one. The orthographic quad comes out as a small
// rectangle at that projection's near plane, a few units in front of the
// player's eye: a ray-traced object that casts shadows (onto the Pip-Boy
// screen) and that the main camera, with its own FOV, shows away from where
// the rasterised UI puts it. The runtime's orthographic-is-UI test only runs
// for fixed-function draws, so nothing else stops it.
//
// HideScreenSpaceDraws tags those draws InstanceCategories::Hidden through the
// protocol's RS 42: the runtime keeps the draw's rasterisation (a vertex-
// capture draw always keeps its original draw) and builds its instance
// hidden, so it is never traced and casts nothing. The runtime only reads RS 42
// when a texture is bound at stage 0; an untextured quad can't be tagged and is
// reported by the log below.
//
// ScreenSpaceLog logs each distinct such draw once (route, PS / VS hash, the
// stage-0 texture's Remix hash and which rtx.conf list has it, the projection
// the runtime will un-project it with, what was done), and each distinct small
// FFP-engaged draw (<= 8 vertices) near the camera or drawn through the
// viewmodel projection, the other way a quad can float at the player.
namespace comp::game::screen_space
{
	// Instead of remix_protocol::none() before a screen-space passthrough
	// draw (FFP already disengaged). Tags it Hidden when the switch is on and
	// a texture is bound at stage 0; logs it (first sighting of its identity).
	void tag_draw(IDirect3DDevice9* dev, const char* route, UINT verts, UINT prims);

	// After an FFP-engaged draw's route is chosen (transforms applied):
	// logs it when it is a small draw near the camera. Cheap when not.
	void note_ffp_draw(IDirect3DDevice9* dev, const char* route, UINT verts, UINT prims);

	// game::apply_transforms: the projection the device now holds (what a
	// later shader draw would be un-projected with).
	void note_applied_projection(const float* proj);

	// Present: the per-interval summary.
	void on_present();
}
