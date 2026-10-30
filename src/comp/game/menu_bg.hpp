#pragma once

// Menu backgrounds ([MenuBackground] in remix-comp.ini).
//
// FNV renders the world once into a menu-background render target when a
// menu opens over the world (barter, containers, terminals, the lockpick, the
// pause menu and the rest) and then pastes that frozen texture behind the
// menu every frame. Under Remix the world is path traced and never
// rasterised, so the capture comes out nearly empty.
//
// Behind the Pip-Boy the world is path traced live: while it is up this
// module
//  1. re-runs the engine's own background capture every frame (the
//     Pip-Boy's capture request, a one-byte write);
//  2. during that capture pass, routes draws into the menu-background RT as
//     on-screen world geometry (or binds the real backbuffer in its place);
//  3. drops the paste quad that samples the menu-background texture, and
//     optionally the ISMENUBG sepia pass, so they can't cover the path-traced
//     image.
//
// Every other menu keeps the engine's frozen background, filled with the
// runtime's thumbnail of the last path-traced frame (save_thumbnail.hpp).
// Those frames are rasterised as the game draws them (menu_scene.hpp), so no
// path tracing runs behind such a menu.
namespace comp::game::menu_bg
{
	// Game-code patch time: installs the capture guard (menu_bg.cpp). Without
	// it the Pip-Boy's live background stays off.
	void install();

	// d3d9 SetRenderTarget, before game::on_set_render_target. Returns the
	// surface to actually bind (the game's, or the backbuffer in its place).
	IDirect3DSurface9* filter_render_target(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* surface);

	// d3d9 SetRenderTarget, after game::on_set_render_target: marks a
	// redirected-in-place menu RT as on-screen for the FFP routing.
	void post_render_target(DWORD idx);

	// d3d9 SetDepthStencilSurface. Returns the surface to actually bind.
	IDirect3DSurface9* filter_depth_stencil(IDirect3DSurface9* ds);

	// d3d9 StretchRect: learns copies out of the capture target (fallback for
	// a capture RT that is a plain surface rather than a texture level).
	void on_stretch_rect(IDirect3DSurface9* src, IDirect3DSurface9* dst);

	// Every draw entry point, before anything else: a new frozen capture is
	// filled before the first draw that reads it. True = drop the draw.
	bool should_skip_draw(IDirect3DDevice9* dev);

	// Present (end of frame): samples the menu state and re-arms the
	// Pip-Boy's capture for the next frame.
	void on_present();

	// Device reset: forget every surface/texture address.
	void on_reset();

	// The Pip-Boy state machine (InterfaceManager +0x4BC, 0-5, from the engine
	// monitor) as last sampled at Present, 0xFFFFFFFF before the first
	// reading. Diagnostics only.
	uint32_t last_pipboy_mode();

	// A menu is up over the engine's frozen background, with the Pip-Boy
	// closed: the frame shows no live world.
	bool frozen_menu();
}
