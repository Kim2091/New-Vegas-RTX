#pragma once

// Texture-pack support: maps the game's D3D textures back to their file
// paths and finds the extra maps a pack ships next to a diffuse that vanilla
// FNV never binds. The one handled is the height map ("foo.dds" ->
// "foo_p.dds"): vanilla only binds a HeightMap for the PAR* parallax shaders,
// so a pack's height maps for every other material never reach the GPU.
//
//   * install_hook() detours NiDX9TextureData::SetD3DTexture (fnv_exe
//     set_d3d_texture) for NiDX9SourceTextureData objects and records D3D
//     texture -> the owning NiSourceTexture's file name. Runs on whatever
//     thread the engine loads textures on.
//   * height_for() (render thread, per draw) returns the loaded height map for
//     a diffuse, or nullptr. The first request for a path queues it; a worker
//     thread resolves it (loose Data\Textures first, then the loaded BSAs,
//     see tex_archive.hpp) and reads the bytes; on_present() creates the D3D
//     textures (MANAGED) within a per-frame budget.
//
// [TexturePacks] in remix-comp.ini.

namespace comp::game::tex_pack
{
	// Window thread (see install_game_patches), through call_chain::hook_entry.
	void install_hook();

	// The proxy created a texture object: any path recorded for a previous
	// object at the same address is stale.
	void on_texture_created(IDirect3DBaseTexture9* tex);

	// Loaded height map for this diffuse (nullptr: none, or not yet loaded).
	// `pending` (optional) is set when nullptr only means "not loaded yet": a
	// lookup for this diffuse is queued or loading and may still find a map.
	IDirect3DTexture9* height_for(IDirect3DBaseTexture9* diffuse, bool* pending = nullptr);

	// A draw actually carried a pack height map (stats / one-shot log).
	void note_bound(IDirect3DBaseTexture9* diffuse, UINT stage);

	// Once per frame, before the real Present: budgeted texture creation,
	// eviction and periodic stats.
	void on_present(IDirect3DDevice9* dev);

	bool enabled();

	// True when the texture hook went in at startup (HeightMaps was on then):
	// only then can HeightMaps be switched at run time.
	bool hook_installed();
}
