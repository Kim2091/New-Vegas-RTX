#pragma once

// Fallback camera ([RemixApi] FallbackCamera).
//
// Remix infers its camera from the D3D9 draws. Where FNV's room / portal
// culling leaves a frame with almost no world draws and the whole world comes
// from retained API instances, no draw gives it a main camera and it skips
// path tracing: a flat clear colour under the HUD. So once per frame, at the
// frame's first world draw (on the backbuffer, perspective, not sky; the
// world renders before the first-person view model), the game's world camera
// goes to the runtime with remixapi_SetupCamera and the FALLBACK frame event:
// the NiDX9Renderer VIEW and PROJECTION in the render frame (stable origin
// and world unit scale), as the device gets them, the matrices that same
// draw hands Remix through D3D9. The runtime holds it and makes it the main camera only
// when no draw set one this frame; a D3D9 world draw's camera always wins.
//
// The scene statics suppress the engine passes of the statics they draw
// through the API, so an interior view that holds only claimed statics has
// no D3D9 world draw at all. The camera then goes out at the frame's first
// suppressed static pass instead (on_claimed_static_pass): the engine's
// renderer holds the world VIEW and PROJECTION while it dispatches the pass.
//
// A bridge that does not forward SetupCamera leaves the interface slot null:
// logged once, nothing sent.

// View-model camera ([RemixApi] ViewModelCamera). First-person API draws
// (arms, weapon, unlit FX) carry REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
// before the first of them each frame, the zoomed projection they are drawn
// with goes to the runtime as its VIEW_MODEL camera (SetupCamera, same VIEW
// as the world camera with the stable origin's shift). With
// rtx.viewModel.enable the runtime perspective-corrects them like a D3D9 view
// model; off, it traces them as world geometry, as the D3D9 route does.

namespace comp::game::api_camera
{
	// From each draw call, before it is routed (API or D3D9).
	void on_draw(IDirect3DDevice9* dev);

	// From the scene statics' pass filter, for an engine render pass of a
	// claimed static that it suppresses (no D3D9 draw follows): the frame's
	// first world pass work (game::on_world_draw) and its world camera, when
	// no draw sent them yet this frame.
	void on_claimed_static_pass();

	// From Present, before the frame counter advances: counts the frames that
	// got no world camera and logs the camera sources once per 600 frames.
	void on_present();

	// For a first-person (viewmodel-projection) draw about to go to the API:
	// sends the frame's VIEW_MODEL camera once (at its first such draw) and
	// returns the categories the draw adds: VIEW_MODEL when the camera went
	// out this frame, else 0 (feature off, not a viewmodel draw, no
	// SetupCamera in the bridge, or a projection that isn't a plain
	// perspective; the part stays a world instance).
	remixapi_InstanceCategoryFlags view_model_categories(IDirect3DDevice9* dev);
}
