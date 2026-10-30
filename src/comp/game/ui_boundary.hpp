#pragma once

// The frame's UI boundary: where the engine stops drawing the world and
// starts drawing its HUD and 2D interface.
//
// The runtime path traces the frame at its RTX injection and writes the
// result over the target; whatever was rasterised on it before is lost. It
// injects at the first draw it takes as UI (d3d9_rtx.cpp isRenderingUI): an
// orthographic fixed-function draw, or a draw whose texture is in
// rtx.uiTextures. FNV's HUD and menus are shader draws, so without help the
// injection waits for a listed texture, and every 2D draw before it (any
// element whose texture a UI mod replaced) comes out transparent.
//
// FNV's Main_Render (0x86FF70) ends every branch the same way: all branches
// (world, Pip-Boy, menus over a frozen background, menu 3D scenes, the main
// menu) meet at 0x8703F1 after their 3D and image-space work, then draw the
// HUD's 3D element (call at 0x870403 to 0x709B40 -> HUDMainMenu 0x77F0D0) and
// then the whole 2D interface through Interface_Render 0x7028B0 (call at
// 0x870417): tiles, HUD, menus, cursor. The HUD's 3D element is the aimed
// weapon's scope model (TESObjectWEAP+0x1E8, meshes\interface\hud\*.nif:
// sniper scopes, binoculars), attached to the HUD node *0x011D9770 while the
// player aims through it. It is a screen overlay: the HUD draws it with its
// own camera (*0x011D975C) over a cleared depth buffer, after the world's
// image-space work. The Pip-Boy and terminal screens are rendered into a
// texture earlier, before the frame's scene begins. Present follows at
// 0x87055E.
//
// The call at 0x870403 is redirected to a thunk that marks the boundary and
// jumps to the HUD's 3D render. Main_Render's two calls to RenderWorldScene
// 0x8706B0 (0x870244 gameplay, 0x8702A9 Pip-Boy) are redirected the same way
// to mark that the frame rendered the world to the screen; the menu
// background capture's own call (0x8721A4, into a render target) is not.
// Each site stays a call: when another mod already redirected it (JohnnyGuitar
// NVSE hooks both RenderWorldScene calls), the thunk jumps to that mod's code
// instead, and a mod that hooks the site later finds a call to chain from
// (call_chain.hpp).
//
// In a frame that rendered the world, every draw from the boundary to Present
// is UI: the wrapper triggers the injection at the first one that targets the
// back buffer (a degenerate orthographic fixed-function draw, which the
// runtime takes as UI) and rasterises every one as the game draws it, never
// traced and never hidden, whatever its texture: the scope overlay lands on
// the path-traced image, and the HUD and interface over it. Draws before the
// boundary keep their routing.
//
// A menu-mode frame without the world (the main menu, the pause screen and
// other menus over a frozen background with its tint, terminals, the menu 3D
// scenes) draws its background before the boundary with the game's shaders.
// An injection at the boundary would write the path-traced image over it, so
// such frames use menu_scene's raster frames and rtx.uiTextures instead.
//
// Not covered: loading screens (drawn and presented by
// InterfaceManager_Render outside Main_Render) and the modal message-box loop
// 0x704010. A site that holds anything but a call to executable code leaves
// the hook off and rtx.uiTextures in charge.
namespace comp::game::ui_boundary
{
	// Installs the three call-site redirects once (all or none), chaining
	// in front of another mod's call where one is found. Logs one line, and
	// one more per chained or foreign site.
	void install();

	// The boundary was passed in a frame that rendered the world: the draw
	// being made is UI.
	bool in_ui();

	// Every draw while in_ui(), before it is made: triggers the runtime's RTX
	// injection at the first one to a screen-sized target and counts it.
	void before_draw(IDirect3DDevice9* dev);

	// The injection already ran this frame (here or for a menu scene).
	bool injected();

	// A draw the runtime takes as UI (no vertex shader, orthographic
	// projection, z-writes off): makes the runtime inject now. Draws nothing.
	bool trigger_injection(IDirect3DDevice9* dev);

	// Present, before ffp_state's, while the flags still belong to this frame:
	// clears them and writes the per-interval log line. The three sites are
	// tracked by call_chain, which logs once when another module patches one
	// after the install.
	void on_present();
}
