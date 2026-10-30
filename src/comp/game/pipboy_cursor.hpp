#pragma once

// The mouse cursor while the Pip-Boy is up.
//
// How the game does it (FalloutNV.exe 1.4.0.525):
//   * The cursor moves by DirectInput deltas only (InterfaceManager_UpdateCursor
//     0x7118D0); its screen pixel is InterfaceManager +0x38 / +0x40, clamped to
//     the whole screen.
//   * It is a 2D tile (IM+0x28 under the cursor root IM+0x84) drawn by the
//     interface pass (0x7134D0) on the backbuffer, never into the Pip-Boy's
//     interface render target (FORenderedMenu_Render 0x7FBA00 culls it there).
//   * The tile pick does not use that pixel directly while the Pip-Boy is up:
//     FOPipboyManager_PickScreen 0x7F8720 casts a ray through the cursor pixel
//     with the world scene graph camera at the Pip-Boy's first-person FOV onto
//     the "pipboyscreen" mesh, and the hit's UV becomes the interface position
//     the tiles are tested against (IM+0x4AC / +0x4B0).
//
// The Pip-Boy here is drawn with the world camera (the fallback camera rule),
// wider than the camera the game picks with, so the screen mesh shows smaller
// and nearer the centre than the game's ray assumes, and the 2D cursor points
// at a different part of the screen than the one the click hits.
//
// The remap moves the cursor's drawing, nothing the game reads. The game's ray
// through its cursor pixel leaves the Pip-Boy camera's eye in direction d; the
// Pip-Boy's draws are placed relative to that eye, and Remix shows them from
// the same relative eye with the world camera's VIEW and PROJECTION. So the
// point the game picks shows in the Remix render along d too, whatever its depth:
//   d = ((ndc.x - P20) / P00, (ndc.y - P21) / P11, 1) * inverse(Vpip)
//   o = d * Vworld;  ndc' = (Pw00 * o.x / o.z + Pw20, Pw11 * o.y / o.z + Pw21)
// with the 3x3 rotations of the raw (camera-relative) renderer VIEWs. The
// cursor's draw is translated in clip space by ndc' - ndc, so its tip sits on
// the displayed spot the game will hit; the game's input and picking are
// untouched.
//
// Inputs, per frame: the world camera the wrapper sends Remix (api_camera),
// the Pip-Boy's own draws' VIEW / PROJECTION (renderer state at those draws),
// the cursor pixel (IM+0x38 / +0x40, guarded reads). The cursor's draw is the
// screen-space draw whose object origin, through its ModelViewProj (c0-c3),
// lands on the cursor pixel. Anything missing (no engine monitor, Pip-Boy
// closed, no camera this frame or the last, unreadable cursor, a ray behind
// the camera) means no remap. One [PipBoyCursor] line per Pip-Boy open gives
// the mapping, and one at its close the counts.
namespace comp::game::pipboy_cursor
{
	// api_camera: the world camera sent to Remix this frame (raw renderer
	// VIEW and PROJECTION, 16 floats each).
	void note_world_camera(const float* view, const float* proj);

	// From each draw the Pip-Boy owns (api::pipboy_own_draw): its camera is
	// the renderer's current VIEW and PROJECTION.
	void note_pipboy_draw();

	// Screen-space (PASS_2D) draw about to be issued on `dev` (the real
	// device): when it is the cursor's while the Pip-Boy is up, sets the
	// translated ModelViewProj and returns true; after_2d_draw then puts the
	// game's back.
	bool before_2d_draw(IDirect3DDevice9* dev);
	void after_2d_draw(IDirect3DDevice9* dev);

	// Present: the session's open / close lines.
	void on_present();
}
