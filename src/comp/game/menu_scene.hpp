#pragma once

// A menu's own 3D scene: the lockpick's lock and pins, the surgery and
// SPECIAL book models, the casino tables, Caravan and the RaceSex head.
//
// FNV draws these through the ordinary BSShader DrawScene with a menu camera,
// over the frozen menu background (Render3DMenuScene 0x872940, branch B of
// Main::Render) or into a menu render target. Their geometry hangs under the
// menu shadow scene nodes, not the world's: the ShadowSceneNode array
// 0x011F91C8 holds [1] "Fallout Menu Object 3D Scene Graph" and [3] "Fallout
// Menu Player 3D Scene Graph" (named by 0x5BA8F0; LockPickMenu attaches its
// root *0x011DA24C to [1] and detaches it in its destructor 0x78E0F0).
// HUDMainMenu's 3D element (*0x011D9770, the aimed weapon's scope model, drawn
// by 0x77F0D0) also hangs under [1]; it is not a menu scene. In a frame that
// renders the world it is drawn after the UI boundary (ui_boundary.hpp).
//
// Such a draw goes to the device with the game's shaders. It takes no world
// route, sets no frame camera (stable origin, world FOV, lights, the Remix
// fallback camera) and is never suppressed as a claimed static.
//
// A frame over the engine's frozen menu background (menu_bg::frozen_menu),
// and any frame that draws a menu scene, is rasterised as the game draws it,
// with no path tracing. The runtime commits every draw made before its RTX injection
// (vertex capture keeps the raster too) and takes the Main camera from any of
// them with a perspective projection (for these shader draws, the device's
// last FFP transforms). The injection, at the first UI draw (the menu's 2D
// interface, after the 3D scene), then path traces from that camera and
// writes the result over the target, hiding the frozen background and the
// menu's scene. So the frame's first draw to a screen-sized target triggers
// the injection first (ui_boundary::trigger_injection: a degenerate
// fixed-function draw with an orthographic projection, which the runtime's
// isRenderingUI takes as UI): no draw is committed before it, the frame has
// no camera and the runtime traces nothing; every later draw is rasterised
// as it is. The fallback camera is not sent in such a frame, so the injection
// at Present finds no camera either.
//
// The test reads the geometry the engine is rendering (the render-pass
// dispatcher hook, scene_graph::current_geometry) and walks its parents to a
// menu scene node. In game, outside menu mode, the walk only runs when the
// camera of the scene being drawn (0x011F917C, written by DrawScene 0xB6C0EF)
// is not the world SceneGraph's camera (+0xAC): the world's own draws skip it.
namespace comp::game::menu_scene
{
	// The geometry hangs under a menu scene node.
	bool geometry_in_menu_scene(uintptr_t geo);

	// The draw being made now renders a menu scene node's geometry. Counts it
	// for this frame.
	bool draw_is_menu_scene();

	// A menu scene draw happened since the last Present.
	bool drawn_this_frame();

	// Every draw, before its route: in a raster frame, triggers the runtime's
	// RTX injection at the first draw to a screen-sized target. `menu_draw`:
	// the draw is a menu scene draw (draw_is_menu_scene).
	void before_draw(IDirect3DDevice9* dev, bool menu_draw);

	// This frame is rasterised as the game draws it (the injection ran with no
	// camera): no Remix camera is sent.
	bool raster_frame();

	// Present: per-interval log line, then the frame's flag is cleared.
	void on_present();
}
