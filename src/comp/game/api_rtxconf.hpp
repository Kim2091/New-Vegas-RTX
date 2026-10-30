#pragma once

// The runtime options an API draw has to agree with on what the D3D9 route
// would have got, read from the game folder's rtx.conf, user.conf and
// dxvk.conf once, at first use.
//
// Texture lists. The runtime tags a D3D9 draw with instance categories by its
// albedo's image hash: rtx.ignoreTextures, hideInstanceTextures,
// decalTextures, uiTextures, ... An API material names an uploaded / imported
// texture whose hash the wrapper chose, so none of those lists would ever
// match it, and an ignored or hidden object would reappear in the path-traced
// image. lookup() answers with the D3D9 hash, computed here exactly as the
// runtime does (XXH3-64 of mip 0's bytes, D3D9CommonTexture::SetupForRtx;
// same xxHash release as the runtime's), with the instance categories the
// API draw carries instead. Entries written "-0x..." are removals (the
// runtime's option layers subtract them) and are honoured as such.
//
// Scalars: the protocol decode constants the FFP route gets from the runtime
// (rtx.fnvProtocol.*, rtx.legacyMaterial.roughnessConstant, vertex-colour
// options), so the API material can be built with the same values. Defaults
// are the runtime's own.
//
// Only what the files say at load: edits made later through the Remix UI are
// not seen until the game restarts.

namespace comp::game::api::rtxconf
{
	struct Scalars
	{
		bool  protocol_enabled = true;          // rtx.fnvProtocol.enable
		float glow_intensity = 1.0f;            // rtx.fnvProtocol.glowEmissiveIntensity
		int   normal_alpha_mode = 1;            // rtx.fnvProtocol.normalAlphaMode
		float height_displace_in = 0.05f;       // rtx.fnvProtocol.heightDisplaceIn
		float legacy_roughness = 0.7f;          // rtx.legacyMaterial.roughnessConstant
		bool  vertex_color_baked = true;        // rtx.vertexColorIsBakedLighting
		bool  ignore_all_vertex_color = false;  // rtx.ignoreAllVertexColorBakedLighting
	};

	const Scalars& scalars();

	// What the runtime's texture lists say about one D3D9 image hash: the
	// instance categories an API draw of it carries (every list naming it,
	// ORed, as the runtime does: ignoreTextures -> IGNORE, skyBoxTextures ->
	// SKY, particleTextures -> PARTICLE, terrainTextures -> TERRAIN, the decal
	// lists -> DECAL_*, ignoreAlphaOnTextures -> IGNORE_ALPHA_CHANNEL with
	// drop_alpha: the material must also lose its alpha test / blend, ...),
	// and a list the runtime acts on only for D3D9 draws (uiTextures,
	// lightConverter, lightmapTextures, raytracedRenderTargetTextures) when one
	// names it. rtx.ignoreBakedLightingTextures is answered by
	// ignores_baked_lighting().
	struct Listing
	{
		const char* first = nullptr;        // a list naming it, for the logs
		const char* d3d9_only = nullptr;    // a list naming it that only D3D9 draws can carry, or null
		uint32_t categories = 0;
		bool drop_alpha = false;
	};
	const Listing* lookup(uint64_t d3d9_hash);   // nullptr: in no list
	const char* listed(uint64_t d3d9_hash);      // lookup()->first, or nullptr

	// The list that makes the runtime hide every API instance whose albedo
	// has this D3D9 image hash, or nullptr. The runtime matches an imported
	// texture by its D3D9 hash (fork_hooks::externalDrawTextureCategories):
	// rtx.ignoreTextures makes the instance Ignore + Hidden,
	// rtx.hideInstanceTextures makes it Hidden.
	const char* hiding_list(uint64_t d3d9_hash);
	bool ignores_baked_lighting(uint64_t d3d9_hash);

	// True when any category list has an entry (else there is nothing to check).
	bool has_lists();

	// The runtime list that makes a draw with this albedo a D3D9-only draw,
	// or nullptr. The runtime reads these lists only on D3D9 draws, keyed by
	// what it hashes there, so an API draw can't carry them:
	//   * rtx.lightConverter: an effect light at the draw's geometry centroid
	//     (RtxOptions::shouldConvertToLight on the D3D9 draw's material hash;
	//     an API material has the wrapper's hash): bulbs, neon, flames;
	//   * rtx.uiTextures: the draw is screen UI;
	//   * rtx.lightmapTextures: the texture is skipped when the runtime picks
	//     the draw's albedo stage;
	//   * rtx.raytracedRenderTargetTextures: keyed by a render target's
	//     descriptor hash, which the wrapper can't reproduce; with any entry
	//     in that list, every albedo that is a render target answers it (the
	//     API can't upload a render target either).
	// Such a draw keeps its D3D9 route (docs/fnv-api-only.md: the D3D9
	// exceptions). Image hashes are cached per texture (address + creation
	// generation + size / format); nullptr at once while no list names
	// anything. Logs the first sight of each (list, hash), at most 64 lines.
	const char* d3d9_only(IDirect3DBaseTexture9* tex);

	// XXH3-64 of mip 0 exactly as the runtime hashes a D3D9 texture's CPU
	// copy. False for layouts this can't reproduce byte for byte (anything but
	// DXT1-5 / A8R8G8B8 / X8R8G8B8 2D textures) or a failed read.
	bool d3d9_image_hash(IDirect3DBaseTexture9* tex, uint64_t& out);
}
