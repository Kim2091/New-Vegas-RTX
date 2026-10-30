#pragma once

// Plain D3DPOOL_DEFAULT textures and the Remix API paths.
//
// The engine loads its textures with D3DXCreate{,Cube,Volume}TextureFrom
// FileInMemory (FalloutNV.exe 1.4.0.525, NiDX9_CreateD3DTextureFromFileOrEntry
// 0xE68A80), which creates them MANAGED. A plugin can route those calls to the
// *Ex variants with D3DPOOL_DEFAULT (NVTF's [DirectX]
// bUseDefaultPoolForTextures does). D3DX then fills each DEFAULT texture from
// a SYSTEMMEM copy with the device's UpdateTexture.
//
// The API paths need each texture's content hash (api_textures.cpp). For a
// MANAGED texture it is read with LockRect from the bridge client's CPU copy
// of the texture, which the client keeps for as long as the texture lives, in
// the game's 32-bit address space. A DEFAULT texture has no such copy, so a
// LockRect of one returns garbage, and the copy it allocates is never freed.
//
// [RemixApi] KeepDefaultPoolTextures=1 keeps a plain DEFAULT 2D texture in
// D3DPOOL_DEFAULT when the bridge and runtime can import D3D9 textures and
// the import takes its format:
//   * after_update_texture() hashes the SYSTEMMEM source of each UpdateTexture
//     into such a texture (api::textures::capture_default), while the source
//     still has its CPU copy, and the API paths import the texture by
//     reference under that hash;
//   * an UpdateSurface into one drops its hashes (after_update_surface()), so
//     content the wrapper has not seen is never imported under an old hash.
// Plain DEFAULT cube and volume textures stay DEFAULT too: no API path reads
// them. FaceGen face maps are created MANAGED: the skin tint reads every
// level of them (api_material.cpp facegen_layer). So are hair LayerMaps
// ("*_hl.dds", loaded by the same file load): the hair material reads their
// alpha (hair_layer_textures). With [RemixApi] ShaderBakeHair, so is every
// texture under textures\characters\hair\: the hair bake reads the BaseMap
// too (api_hair_bake.cpp). They are told apart by the path the engine's
// D3DX file load (texture_from_file, hooked by install_hook()) is loading on
// that thread, under the engine's FaceGen folder,
// textures\characters\facemods\ (the folder the engine's FaceGen export
// writes, so mods follow it), or by the engine's in-memory face map build
// from EGT files (bLoadFaceGenHeadEGTFiles), which creates its texture inside
// the hooked build (facegen_image_texture). Any other plain DEFAULT 2D
// texture is created MANAGED, as below.
//
// KeepDefaultPoolTextures=0, and the cases above: a D3DPOOL_DEFAULT request
// with no usage flag but D3DUSAGE_AUTOGENMIPMAP, no shared handle and a
// non-Ex device is created MANAGED, so the API paths can read its CPU copy.
// Render targets, depth textures and dynamic textures keep D3DPOOL_DEFAULT.
// The runtime refuses UpdateTexture into a MANAGED texture, so the proxy's
// UpdateTexture and UpdateSurface copy a SYSTEMMEM -> MANAGED pair level by
// level with locks (update_texture() / update_surface()).
//
// Whether the import works is asked once, at the first Present (on_present()).
// Textures created before that are created MANAGED. So are new ones once the
// API paths have met many DEFAULT textures and no capture ever worked
// (api::textures::capture_failed()): those are filled some other way.

namespace comp::game::tex_pool
{
	// The pool to create a texture in, for a request of `pool`. `ex_device`
	// is true on a device made with CreateDeviceEx, where D3DPOOL_MANAGED is
	// not valid.
	D3DPOOL texture_pool(D3DRESOURCETYPE type, D3DPOOL pool, DWORD usage, D3DFORMAT format,
		const HANDLE* shared_handle, bool ex_device);

	// A D3DPOOL_DEFAULT request with no usage flag but D3DUSAGE_AUTOGENMIPMAP,
	// no shared handle and a non-Ex device.
	bool plain_default(D3DPOOL pool, DWORD usage, const HANDLE* shared_handle, bool ex_device);

	// Counts a plain DEFAULT request after its creation: `created` is the pool
	// it was created in. `bytes` is the size of all its levels.
	void note_created(D3DPOOL created, uint64_t bytes);

	// The size of all levels of a texture (faces included), 0 for a format
	// with no fixed layout. `levels` 0 is the full chain.
	uint64_t chain_bytes(D3DRESOURCETYPE type, UINT width, UINT height, UINT depth, UINT levels, D3DFORMAT format);

	// Creates a texture through `create(D3DPOOL)` in the pool texture_pool()
	// picks, retrying in the requested pool when a MANAGED create fails.
	template <class Create>
	HRESULT create(D3DRESOURCETYPE type, UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
		D3DFORMAT format, D3DPOOL requested, const HANDLE* shared_handle, bool ex_device, Create&& make)
	{
		const D3DPOOL pool = texture_pool(type, requested, usage, format, shared_handle, ex_device);
		HRESULT hr = make(pool);
		D3DPOOL created = pool;
		if (FAILED(hr) && pool != requested)
		{
			hr = make(requested);
			created = requested;
		}
		if (SUCCEEDED(hr) && plain_default(requested, usage, shared_handle, ex_device))
			note_created(created, chain_bytes(type, width, height, depth, levels, format));
		return hr;
	}

	// Textures created MANAGED by texture_pool() since startup.
	uint64_t promoted_count();

	// Copies `src` (SYSTEMMEM) into `dst` (MANAGED) the way UpdateTexture
	// copies into a D3DPOOL_DEFAULT texture. Returns false when the pair is
	// anything else, and the caller forwards the call; `hr` is the result
	// otherwise.
	bool update_texture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst, HRESULT& hr);

	// The same for UpdateSurface.
	bool update_surface(IDirect3DSurface9* src, const RECT* src_rect, IDirect3DSurface9* dst, const POINT* dst_point, HRESULT& hr);

	// The proxy forwarded UpdateTexture / UpdateSurface with result `hr`.
	void after_update_texture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst, HRESULT hr);
	void after_update_surface(IDirect3DSurface9* dst);

	// Hooks the engine's D3DX texture file load (fnv_exe texture_from_file), so
	// FaceGen face maps are told apart when they are created. Window thread,
	// with the game patches.
	void install_hook();

	// Every Present: asks once whether the import works.
	void on_present();

	// What texture_pool() decided since startup ([Memory] status line).
	struct Stats
	{
		uint64_t kept = 0, kept_bytes = 0;            // plain DEFAULT requests created DEFAULT
		uint64_t promoted = 0, promoted_bytes = 0;    // plain DEFAULT requests created MANAGED
		// Why the promoted ones were (KeepDefaultPoolTextures=1).
		uint64_t by_switch = 0, by_format = 0, by_autogen = 0, by_import = 0, by_facegen = 0;
	};
	Stats stats();

	// The bridge client's CPU copies (BridgeClient_GetMemoryStats). False
	// with a bridge that does not export it. Allocates nothing.
	struct BridgeMemory
	{
		uint64_t surface_bytes = 0, surfaces = 0;
		uint64_t buffer_bytes = 0, buffers = 0;
	};
	bool bridge_memory(BridgeMemory& out);

	// Writes the once-per-session line about promoted textures when there
	// are any (called with the periodic status line).
	void log_once();
}
