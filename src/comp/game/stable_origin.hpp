#pragma once

// Stable render origin ([RemixApi] StableOrigin).
//
// FNV renders camera-relative: the NiDX9Renderer WORLD translation is object
// minus camera and the VIEW has no translation, so every transform Remix sees
// changes whenever the camera moves. This module hands Remix a world frame
// anchored at O instead: the camera's world position snapped to a coarse
// grid, re-snapped only once the camera strays more than StableOriginRadius
// from it. With D = camera - O, fixed for the frame:
//
//   * every WORLD (and WORLDMATRIX(n)) the device gets is W * T(D), and the
//     VIEW is T(-D) * V, so WORLD * VIEW (all the rasteriser uses) is
//     unchanged, and camera-anchored draws (sky, viewmodel, Pip-Boy, UI)
//     keep following the camera: the camera itself sits at D;
//   * FFP light positions (camera-relative) move by D;
//   * API draws are submitted at camera-relative + D.
//
// The camera is the engine's camera global (game::camera_position_ptr), what
// the frame's WORLD matrices are relative to, taken at the first BeginScene
// and again at the frame's first world draw (at_first_world_draw) if it
// moved meanwhile. The engine monitor's world camera, which can be a frame
// old, only decides when the global is the Pip-Boy's menu camera (then the
// last world reading holds); a cut (load, door, fast travel, teleport) snaps
// O. Every draw of a frame uses one D. Without the plugin the feature is
// inactive.
//
// [RemixApi] PublishWorldOrigin: begin_frame also tells the runtime O
// (remixapi_SetGameValue __mcrtx.world_origin.x/y/z, on change and after a
// reset / API init), with render position + O = world position, for the
// runtime's rtx.worldOrigin.enable (history shift on a re-snap, clouds and
// their ground shadows anchored to the world).
//
// Off (the default), D is zero (set_transform() still remembers the raw
// WORLD / VIEW, for a switch-on).
//
// World unit scale ([RemixApi] WorldUnitScale, s, fixed for the session):
// the render frame is also scaled by s, render = s * (world - O), so Remix
// works in centimetres instead of FNV units. Everything inside the wrapper
// stays in FNV units; the scale is applied where values leave it:
//
//   * WORLD (and WORLDMATRIX(n)) is W * T(D) * S, VIEW is S^-1 * T(-D) * V * S
//     (still a rigid view, its translation scaled) and PROJECTION is S^-1 * P
//     (for a perspective one, near / far scaled), so WORLD * VIEW *
//     PROJECTION, what the rasteriser uses, is unchanged;
//   * API instance transforms go out through to_render(), lights through
//     light_to_render(), the published origin and the retained replay origin
//     are s * O;
//   * world-unit values the wrapper pushes (water height, medium distances,
//     the cloud camera) are multiplied by scale().
//
// The runtime's own world-unit options in rtx.conf are in render units.

namespace comp::game::stable_origin
{
	// Config switch ([RemixApi] StableOrigin).
	bool enabled();

	// True when this frame's D is in use (enabled and a camera was read).
	// When false every shift below is zero.
	bool active();

	// Computes this frame's O and D (once per frame; later calls return at
	// once) and, when D changed, re-issues the raw WORLD / VIEW last set with
	// the new D so the pair on the device never mixes two frames' shifts.
	// dev must be the device the wrapper's transforms go to (the bridge's).
	void begin_frame(IDirect3DDevice9* dev);

	// The frame's first world draw, before the Remix camera, the lights and
	// any API draw of the frame: if the camera global moved since
	// begin_frame (and isn't the menu camera), D is re-taken from it and the
	// device pair re-issued. O stays.
	void at_first_world_draw(IDirect3DDevice9* dev);

	// Replacement for dev->SetTransform on every path that sets WORLD,
	// WORLDMATRIX(n), VIEW or PROJECTION. Other states pass straight through.
	HRESULT set_transform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m);

	// The VIEW the device gets for this raw VIEW right now (what set_transform
	// passes on: S^-1 * T(-D) * V * S with the D the device's pair carries).
	D3DMATRIX device_view(const D3DMATRIX& raw);

	// The PROJECTION the device gets for this raw PROJECTION.
	D3DMATRIX device_projection(const D3DMATRIX& raw);

	// The raw WORLD / VIEW / PROJECTION last set, for the proxy's
	// GetTransform: a caller that saves and restores a transform must get
	// back what it set. False when the value should come from the device.
	bool get_raw_transform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* out);

	// Render units per FNV unit ([RemixApi] WorldUnitScale).
	double scale();

	// An API transform in FNV units (camera-relative + D, or absolute) as the
	// runtime gets it: every entry times scale().
	remixapi_Transform to_render(const remixapi_Transform& t);

	// This frame's D, O and camera (double, FNV units; zero when inactive).
	const double* shift();
	const double* origin();
	const double* camera();
	// s * O in render units, for DrawRetainedInstances and the published
	// origin.
	const float* origin_render();

	// True when the engine's camera global right now is the camera this
	// frame's D was computed from (not mid Pip-Boy menu render). Also feeds
	// the per-600-frame "camera check" log line.
	bool camera_matches_now();

	// A camera-relative point / spot light into the render frame: position
	// (+ D) and range times scale(), attenuation for the same falloff over
	// the scaled distances.
	void light_to_render(D3DLIGHT9& light);

	// The value the device gets for a render state: D3DRS_FOGSTART / FOGEND
	// times scale(), D3DRS_FOGDENSITY divided by it (the fog is applied over
	// render-unit distances), anything else unchanged. Remembers the raw fog
	// values for get_raw_fog_state.
	DWORD set_fog_state(D3DRENDERSTATETYPE state, DWORD value);

	// The raw fog value last set, for the proxy's GetRenderState. False for
	// any other state, or one not set through set_fog_state.
	bool get_raw_fog_state(D3DRENDERSTATETYPE state, DWORD* out);

	// Device reset: the device's transforms are identity again.
	void on_reset();

	// Switches the feature at run time (Host Settings). Call between frames.
	// Off puts the raw WORLD / VIEW back on dev and zeroes every shift; on
	// snaps a fresh origin at the next frame's first BeginScene. Retained
	// instances need the origin: the caller drops them before switching off.
	void set_enabled(IDirect3DDevice9* dev, bool on);
}
