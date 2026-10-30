#pragma once
#include "structs.hpp"

namespace shared::common { struct PsSlotMap; }

namespace comp::game
{
	// --- Game addresses (resolved in init_game_addresses) ---

	// Both null when the executable is unknown (shared/fnv_exe.hpp).
	inline void** renderer_singleton_ptr = nullptr;  // NiDX9Renderer** (fnv_exe renderer)
	// NiPoint3* (fnv_exe camera_position): the renderer's camera position (BSShader side),
	// what the WORLD matrices are relative to. It follows the Pip-Boy's menu
	// camera; the world camera is the engine monitor's (nvse_link facts).
	inline float* camera_position_ptr    = nullptr;

	// --- NiDX9Renderer helpers ---

	// Read a 4x4 row-major matrix from NiDX9Renderer at the given byte offset.
	float* get_renderer_matrix(unsigned int byte_offset);

	// Orthographic projection (Pip-Boy, menus, UI overlays)
	bool is_2d();

	// NiShadeProperty::m_eShaderType == kProp_Sky (0x0D)
	bool is_sky();

	// NiShadeProperty::m_eShaderType == kProp_NoLighting (0x15)
	bool is_no_lighting();

	// The current projection's FOV term differs (> 5%) from the frame's world
	// projection, latched from the first FFP world draw of the frame: the
	// first-person viewmodel / Pip-Boy, which FNV renders zoomed. False until
	// a world projection was seen.
	bool uses_viewmodel_projection();

	// True if this draw is a fake-shadow overlay (NOLIGHTING + all-grayscale
	// vertex colors with a dark vertex) that should be skipped, unless its PS is
	// on the keep-list. Reads stream-0 vertex colors (cached per VB+base).
	bool should_skip_fake_shadow(IDirect3DDevice9* dev, INT base_vtx, UINT num_verts,
		IDirect3DPixelShader9* ps);

	// Clear the fake-shadow result cache (call on device reset).
	void clear_fake_shadow_cache();

	// Per-frame maintenance; call from Present. Ages out the fake-shadow cache
	// so a vertex buffer whose address gets recycled by a later allocation can't
	// keep inheriting the previous mesh's verdict indefinitely.
	void on_frame_end();

	// Apply World/View/Projection from NiDX9Renderer directly (no VS constant decomposition)
	void apply_transforms(IDirect3DDevice9* dev);

	// Every draw, before its routing: the frame's first world draw latches
	// the world FOV and the camera forward, and sends the lights (the engine
	// has updated the frame's transforms by then). claimed_static: called for
	// an engine render pass of a claimed static that the scene statics
	// suppress (no D3D9 draw follows it). The pass is known to be world
	// geometry, so the sky test, which reads the previous pass's shade
	// property at that point, is skipped.
	void on_world_draw(IDirect3DDevice9* dev, bool claimed_static = false);

	// The device the wrapper's draws and transforms go to (the Remix bridge's),
	// set at every BeginScene. For per-frame work that runs outside a draw
	// call (the engine's render-pass dispatcher).
	inline IDirect3DDevice9* frame_device = nullptr;

	// --- Render target tracking ---

	// Track whether current RT is the backbuffer (skip FFP for off-screen draws)
	inline UINT  backbuffer_width  = 0;
	inline UINT  backbuffer_height = 0;
	inline bool  rendering_to_backbuffer = true;

	// Swap-chain backbuffer surface, kept for pointer identity only: never
	// dereferenced and not AddRef'd (holding a reference to a D3DPOOL_DEFAULT
	// surface would make IDirect3DDevice9::Reset fail). Cleared on reset and
	// re-acquired by init_backbuffer_tracking.
	//
	// FNV binds several full-resolution offscreen targets (water reflection,
	// post-process chains) that a width / height comparison would take for the
	// backbuffer. Identity is exact.
	inline IDirect3DSurface9* backbuffer_surface = nullptr;

	// Set once the engine is observed binding `backbuffer_surface` to RT0. Until
	// then a width / height comparison decides, so an unexpected swap-chain /
	// bridge arrangement can't classify every draw as off-screen.
	inline bool  backbuffer_surface_seen = false;

	// The device's default render target (RT0 right after CreateDevice or
	// Reset), kept for pointer identity only like backbuffer_surface. The
	// Remix bridge hands out two client objects for the one server
	// backbuffer: the swap chain's (GetBackBuffer, backbuffer_surface) and the
	// device's implicit render target (GetRenderTarget, what the engine binds).
	inline IDirect3DSurface9* default_render_target = nullptr;

	// Records default_render_target from a device whose RT0 is still the
	// implicit one (the real device, at creation and after a Reset).
	void note_default_render_target(IDirect3DDevice9* dev);

	// The surface is the swap chain's backbuffer under either identity.
	bool is_screen_surface(IDirect3DSurface9* s);

	void init_backbuffer_tracking(IDirect3DDevice9* dev);
	void on_set_render_target(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* surface);

	// RT0 is an off-screen target with the backbuffer's size: the Pip-Boy
	// screen at full resolution, image-space and menu-background targets.
	// dxvk-remix takes any such target for the screen (isRenderTargetPrimary
	// compares sizes), so a draw into it whose texture is in rtx.uiTextures,
	// or an orthographic fixed-function draw, triggers the frame's RTX
	// injection there: the path-traced image lands in that target and every
	// later draw of the frame, the Pip-Boy's model included, is rasterised.
	inline bool offscreen_screen_sized = false;

	// Every draw entry point, around the draw: an orthographic draw into a
	// screen-sized off-screen target is drawn inside an occlusion query. The
	// runtime rasterises a draw made while an occlusion query is active before
	// its UI test, so such a draw never triggers the injection. begin returns
	// true when it began the query; end then ends it.
	bool begin_raster_only(IDirect3DDevice9* dev);
	void end_raster_only(IDirect3DDevice9* dev);

	// The wrapper's own RTX injection draw (ui_boundary::trigger_injection)
	// is made outside the query: pause ends an active one and returns true,
	// resume begins it again.
	bool pause_raster_only(IDirect3DDevice9* dev);
	void resume_raster_only(IDirect3DDevice9* dev);

	// --- Point lights ---

	// The engine monitor plugin's light registry -> D3D9 SetLight / LightEnable
	// (once per frame; Remix takes the fixed-function lights).
	void update_lights(IDirect3DDevice9* dev);

	// Light config (read from INI)
	inline bool  lights_enabled        = true;
	inline float light_intensity       = 1.0f;
	inline int   light_range_mode      = 0;
	// [Lights] EffectLightForward: the player's light (the Pip-Boy light, found
	// through the player's LightEffect) is placed this many units ahead of the eye
	// along the view, a headlamp (0 = where the engine hangs it, on the body).
	inline float effect_light_forward     = 1.0f;
	// [Lights] EffectLightThirdPersonForward: in third person (the eye is behind
	// the player, and a light there is shadowed by the body) the player's light
	// stays where the engine hangs it, plus this many units along the view's
	// horizontal direction (out of the body, ahead of the player; 0 = the
	// engine's position).
	inline float effect_light_third_person_forward = 20.0f;
	// [Lights] FollowerIntensityPercent: extra multiplier on the Pip-Boy light.
	inline float pipboy_light_intensity   = 1.0f;
	inline bool  lights_updated_frame  = false;
	inline int   device_light_count    = 0;   // light slots 0..n-1 are enabled on the device

	// --- Skinning (bone capture + game hooks) ---

	// A skinned pass the renderer handled without a D3D9 draw (the Remix API
	// drew it, or it was dropped). Leaves the bone state as a drawn pass did:
	// the palette stays intact for a later pass of the same object that
	// doesn't re-upload, and the next bone upload starts a fresh palette.
	void skinned_pass_done();

	// Called from d3d9ex SetVertexShaderConstantF for immediate bone upload.
	// Returns true when the upload was consumed as a skinned bone (captured into
	// bone_rows for the Remix-API skinned path); the caller then skips
	// forwarding the raw constant to the device (flush_bones_to_device puts it
	// back for a passthrough draw that needs it).
	bool on_set_vs_const_f(IDirect3DDevice9* dev, UINT start_reg, const float* data, UINT count);

	// Re-upload the cached bone constant range to the real device. Called from
	// passthrough draws of a skinned declaration (offscreen targets: shadow
	// maps, FaceGen renders), which run the game's real skinning VS and need
	// the bones that on_set_vs_const_f swallowed.
	void flush_bones_to_device(IDirect3DDevice9* dev);

	// The current object's bone palette as the engine uploaded it: slot n is
	// the three vertex-shader constant registers of bone n (c44 + 3n ..),
	// raw, 12 floats. Each register is one row of a 3x4 column-vector matrix
	// (x' = dot(c[3n+0], (p, 1)), ...) mapping skin (bind) space to
	// camera-relative world. Valid for slots < num_bones. Read by the
	// Remix-API skinned path (api_skinned.cpp).
	inline float bone_rows[MAX_FFP_BONES][12] = {};

	// Skinning state
	inline int  num_bones          = 0;
	inline int  prev_num_bones     = 0;
	inline bool bones_drawn        = false;

	void on_reset();

	// --- Sky reads (SEH-guarded) ---

	// get_sun_orientation / get_masser_orientation behind a structured-exception
	// guard. The sky chain is freed and rebuilt on every worldspace change, and
	// is_live_object can only reject what it can see; an access violation here
	// becomes "no reading this frame" instead of a crash.
	bool try_get_sun_orientation(float& elev_deg, float& rot_deg, float* hour, bool* night);
	bool try_get_masser_orientation(float& elev_deg, float& rot_deg);

	// The live Sky object, or null when there is none (menus, interiors, mid-
	// rebuild). Its address identifies one Sky lifetime across frames.
	const void* try_get_sky();

	// get_weather_state behind the same guard.
	bool try_get_weather_state(WeatherState& out);

	// --- Init ---

	// Detect the executable, resolve engine globals and read config. Safe from
	// DllMain: reads only the PE headers, .rsrc and the import names, and
	// writes nothing in the game's own code.
	extern void init_game_addresses();

	// Apply the address-table patches to FalloutNV.exe's .text. Must not be
	// called from DllMain: the Steam executable's .text is still SteamStub-
	// encrypted at that point and patching it corrupts real code once the .bind
	// stub decrypts. Call once the game window exists. Does nothing for an
	// unknown executable.
	extern void install_game_patches();
}
