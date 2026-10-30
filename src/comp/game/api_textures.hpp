#pragma once
#include "api_common.hpp"

// Remix-API texture cache: game textures (and wrapper-generated ones) uploaded
// once through remixapi_CreateTexture and named by materials as
// "0x%016llX" of the hash passed at creation.
//
// Game textures are read from the bridge client's CPU shadow: a READONLY
// LockRect on a MANAGED / SYSTEMMEM texture returns the bytes the game wrote
// when it filled the texture, without any IPC. D3DPOOL_DEFAULT textures and
// render targets have no meaningful shadow and are never locked (a lock would
// return garbage and leave the client a shadow it never frees). Render
// targets are refused; DEFAULT textures are covered below.
//
// Identity:
//   * The upload is keyed by a content hash (desc + converted level data +
//     format flavour + salt), so the same image bound through two different
//     texture objects is uploaded once, and the hash can never coincide with
//     a D3D9-captured texture's XXH3 image hash.
//   * A pointer -> hash map makes repeat lookups cheap. Each pointer entry
//     remembers the texture's desc and a sampled hash of its top level, and
//     is re-validated on every acquire, so a texture freed by the game whose
//     address is recycled for a different image is re-uploaded, not aliased.
//
// [RemixApi] ImportD3D9Textures=1: a MANAGED DXT1-5 / A8R8G8B8 / X8R8G8B8
// game texture is handed to Remix by reference instead (the bridge's
// remixapi_ImportD3D9Texture: Remix samples the texture the game created on
// the bridge server's device). Only the uploaded top level is read, for the
// hash, which is the one the upload would get; nothing crosses the bridge but
// a handle, and the upload budget doesn't apply. An import is cached, shared
// and evicted exactly like an upload. Whatever the import refuses uploads.
// An imported DXT1 colour texture keeps its 1-bit alpha; an uploaded one goes
// as BC1_RGB, the only BC1 format of the API, which reads alpha as 1.
// Static content only: an imported texture the game rewrites in place would
// show the new pixels to every material using the import.
//
// D3DPOOL_DEFAULT textures ([RemixApi] KeepDefaultPoolTextures, tex_pool.hpp)
// have no CPU copy and are never locked. capture_default() hashes each one
// from the SYSTEMMEM source of the UpdateTexture that fills it, for every
// flavour at once (the hash an import of a MANAGED copy would get), plus the
// runtime's XXH3 hash of its top level (default_d3d9_hash, for the rtx.conf
// texture lists). acquire() imports it under the captured hash, whatever
// ImportD3D9Textures says: an upload would need the pixels. A DEFAULT texture
// with no capture (filled some other way) is refused.
//
// Lifetime: acquire / acquire_generated add one reference, release drops it.
// An upload with no references is kept for TextureEvictFrames frames (cells
// reload often; re-uploading is the expensive part) and then destroyed.

namespace comp::game::api::textures
{
	enum class Flavor : uint8_t
	{
		// Values used as stored. Terrain layers: the runtime blends layers in
		// the textures' own encoding and linearises once, so FNV's gamma-space
		// blend needs UNORM uploads (and normal maps are always UNORM).
		Unorm = 0,
		// Colour textures sampled as sRGB (plain albedo, e.g. far-LOD land).
		Srgb = 1,
		// Tangent-space normal maps whose alpha may be gloss (NVR / texture-pack
		// convention: roughness = 1 - n.a). UNORM like Unorm, but the upload
		// FORMAT tells the runtime whether the alpha is meaningful: a normal map
		// without real alpha (DXT1 / X8R8G8B8 / R5G6B5 ... sources, or an alpha
		// channel that is 255 everywhere in the uploaded top level) is uploaded
		// as BC1 (DXT3/DXT5 are transcoded losslessly, uncompressed ones are
		// BC1-encoded), and the runtime reads "BC1 = no gloss, use the material's
		// roughness constant". Alpha-carrying ones stay BC3 / B8G8R8A8.
		// [RemixApi] TerrainNormalGloss=0 uploads every normal map as BC1.
		// Terrain layers: the D3D9 route never reads their normal alpha.
		NormalMap = 2,
		// A normal map whose alpha must NOT be read as gloss, whatever it
		// holds: always BC1 (or imported with alpha forced to 1). Statics
		// whose shader has no Toggles constant get no normal-alpha flag on
		// the protocol route, so their API copy must not get gloss either.
		NormalMapNoGloss = 3,
		// A protocol normal map (lit shader with Toggles): gloss whenever the
		// source FORMAT has an alpha channel, 255 everywhere included, as the
		// D3D9 route decides (FnvMaterialProtocol::d3d9FormatHasGlossAlpha).
		// Only alpha-less sources go BC1. NormalMap's constant-255 test would
		// give the API copy the gloss-exponent roughness where the D3D9 route
		// gives full gloss.
		NormalMapFormatAlpha = 4,
	};

	// Uploads (or reuses) `tex`. Returns the API hash, or 0 with `reject` set.
	uint64_t acquire(IDirect3DBaseTexture9* tex, Flavor flavor, const char*& reject);

	// Uploads (or reuses) a wrapper-generated image: `mips` levels of
	// tightly-packed 4-byte texels, largest first, in `format`
	// (R8G8B8A8_UNORM / B8G8R8A8_UNORM). Returns the API hash, or 0.
	uint64_t acquire_generated(uint32_t width, uint32_t height, uint32_t mips,
		const void* data, size_t bytes, remixapi_Format format, const char*& reject);

	// Uploads (or reuses) a wrapper-generated image as BC1 (no alpha, so a
	// normal map's alpha is never read as gloss): `mips` levels of
	// tightly-packed B8G8R8A8 texels, largest first. Returns the API hash, or 0.
	uint64_t acquire_generated_bc1(uint32_t width, uint32_t height, uint32_t mips, const std::vector<uint8_t>& bgra,
		const char*& reject);

	// Every level of a MANAGED / SYSTEMMEM 2D texture as tightly-packed
	// B8G8R8A8 texels, largest first (DXT1-5 decoded, the other formats
	// acquire takes expanded), from the bridge client's CPU shadow. False,
	// with `reject`, when it can't be read.
	bool read_bgra8(IDirect3DBaseTexture9* tex, uint32_t& width, uint32_t& height, uint32_t& levels,
		std::vector<uint8_t>& out, const char*& reject);

	// The bridge client forwards remixapi_TextureInfoHostImportEXT (checked
	// once, by its log text in the client image), so float textures can be
	// uploaded. An older client would drop the format and upload garbage.
	bool half_float_supported();

	// Uploads (or reuses) a wrapper-generated R16G16B16A16_SFLOAT image:
	// `mips` levels of tightly-packed 8-byte texels (IEEE half R, G, B, A),
	// largest first. For values outside 0..1 (signed offsets, factors above
	// 1). Returns the API hash, or 0 (also without half_float_supported()).
	uint64_t acquire_generated_half(uint32_t width, uint32_t height, uint32_t mips,
		const uint16_t* data, size_t bytes, const char*& reject);

	// False when no upload has that hash (evicted).
	bool add_ref(uint64_t hash);
	void release(uint64_t hash);

	// True once this frame's uploads passed the soft byte budget; new object
	// builds should wait for the next frame (see kFrameSoftBudget).
	bool upload_budget_spent();

	// Destroys every texture no material holds now, instead of keeping it
	// TextureEvictFrames for reuse: the next acquire of that image builds it
	// again, through whichever path (upload / import) is configured then.
	void purge_unreferenced();

	// Once per frame: resets the upload byte budget. Every 60 frames, evicts
	// uploads unreferenced for TextureEvictFrames frames and prunes stale
	// pointer entries.
	void on_present();

	// Formats remixapi_ImportD3D9Texture takes (DXT1-5, A8R8G8B8, X8R8G8B8).
	bool importable(D3DFORMAT format);

	// Asks the bridge once whether the runtime imports D3D9 textures, whatever
	// ImportD3D9Textures says (render thread; one round trip).
	void resolve_import();

	// True once resolve_import() found the import working. Any thread.
	bool import_available();

	// True once the API paths met many D3DPOOL_DEFAULT textures and no
	// capture_default() ever recorded one: the textures are filled some other
	// way, and new ones have to be created MANAGED. Any thread.
	bool capture_failed();

	// UpdateTexture filled `dst`, a D3DPOOL_DEFAULT texture, from `src`: hashes
	// the source while it still has its CPU copy. Any thread.
	void capture_default(IDirect3DTexture9* src, IDirect3DTexture9* dst);

	// Drops what capture_default() recorded for this address (a new texture
	// there, or content written another way). Any thread.
	void forget_default(const void* tex);

	// The runtime's XXH3 image hash of a captured D3DPOOL_DEFAULT texture's
	// top level, as rtxconf::d3d9_image_hash computes it for a MANAGED one.
	bool default_d3d9_hash(IDirect3DTexture9* tex, uint64_t& out);

	// [Memory] status line.
	struct DefaultStats
	{
		uint64_t captured = 0;       // capture_default() calls that recorded a texture
		uint64_t entries = 0;        // recorded textures now (live or not yet recycled)
		uint64_t imports = 0;        // imports of DEFAULT textures
		uint64_t uncaptured = 0;     // acquires refused: no capture
		uint64_t refused = 0;        // acquires refused: the import failed or is unavailable
	};
	DefaultStats default_stats();
}
