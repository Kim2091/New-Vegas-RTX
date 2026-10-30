#pragma once
#include "shared/common/remix_api.hpp"

// Shared plumbing for the geometry the wrapper submits through the Remix C
// API (the api_*.cpp paths).
//
// Cost model (every call crosses the 32->64-bit bridge as IPC):
//   * CreateTexture / CreateMaterial / CreateMesh: heavy, once per unique
//     object, throttled by the per-frame build budget.
//   * DrawInstance: one async message per visible object per frame, issued at
//     the moment the game would have drawn it; the game's own draw is then
//     skipped, which saves more traffic than the DrawInstance costs.
//   * Reading game data: vertex / index buffers and texture mips are read
//     through READONLY locks, which the bridge client answers from its CPU
//     shadow without IPC (bridge/src/client/lockable_buffer.h, d3d9_surface.cpp).
//
// Everything here runs on the game's render thread, like every D3D9 call.

namespace comp::game::api
{
	// ---- Hashing -------------------------------------------------------------

	struct Hash64
	{
		uint64_t h = 0xcbf29ce484222325ull;

		void mix(uint64_t w)
		{
			h ^= w + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
			h *= 0xff51afd7ed558ccdull;
			h ^= h >> 29;
		}

		void add(const void* data, size_t n)
		{
			const auto* p = static_cast<const unsigned char*>(data);
			size_t i = 0;
			for (; i + 8 <= n; i += 8)
			{
				uint64_t w;
				std::memcpy(&w, p + i, sizeof(w));
				mix(w);
			}
			uint64_t tail = 0;
			for (size_t k = 0; i < n; i++, k++) tail |= static_cast<uint64_t>(p[i]) << (8 * k);
			mix(tail ^ (static_cast<uint64_t>(n) << 56));
		}

		template <typename T> void pod(const T& v) { add(&v, sizeof(T)); }
		void ptr(const void* p) { mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p))); }

		// Never 0: the runtime rejects a zero mesh / texture / material hash.
		uint64_t get() const { return h ? h : 0x1ull; }
	};

	// ---- Draw description -----------------------------------------------------

	struct DrawDesc
	{
		D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
		bool indexed = false;
		INT  base = 0;          // BaseVertexIndex (indexed) / StartVertex (non-indexed)
		UINT min_vtx = 0;       // indexed only: MinVertexIndex
		UINT num_vtx = 0;       // indexed: NumVertices; non-indexed: vertices consumed
		UINT start_index = 0;   // indexed only
		UINT prim_count = 0;
	};

	// ---- Vertex attributes (stream 0 of the bound declaration) -----------------

	struct Attr
	{
		int off = -1;
		int type = -1;
		bool ok() const { return off >= 0; }
	};

	// First stream-0 element with this usage / usage index, or an invalid Attr.
	Attr find_attr(BYTE usage, BYTE usage_index);
	UINT decl_type_size(int type);
	bool attr_fits(const Attr& a, UINT stride);
	// One component as the vertex shader sees it, (0, 0, 0, 1)-filled.
	float read_comp(const uint8_t* p, int type, int comp);
	// Hash of the bound declaration's elements (layout identity).
	uint64_t decl_hash();

	// ---- Geometry --------------------------------------------------------------

	// Cheap per-frame identity of the draw's geometry: buffer pointers, the
	// vertex / index window, the decl layout and a sampled hash of the window's
	// bytes (16 vertices, 16 indices), so a recycled buffer address holding
	// different data never matches. Two READONLY locks, no IPC.
	bool geo_key(IDirect3DDevice9* dev, const DrawDesc& d, uint64_t& out, const char*& reject);

	// Hash of every byte of the draw's vertex window and index range (the
	// window read_geometry copies). Not per frame: for a caller that saw a
	// buffer's write generation move and must know whether the bytes did.
	// Two READONLY locks, no IPC.
	bool content_key(IDirect3DDevice9* dev, const DrawDesc& d, uint64_t& out, const char*& reject);

	// Full copy of the draw's vertex window plus a triangle list whose indices
	// are relative to the window's first vertex. Strips and fans are unrolled;
	// degenerate triangles are dropped.
	struct Geometry
	{
		std::vector<uint8_t> verts;
		UINT stride = 0;
		UINT count = 0;
		std::vector<uint32_t> tris;
	};
	bool read_geometry(IDirect3DDevice9* dev, const DrawDesc& d, Geometry& g, const char*& reject);

	// Copies `bytes` bytes at `offset` of a buffer the caller names directly
	// (not the bound one): one READONLY lock, answered from the bridge
	// client's CPU shadow, no IPC. The same rules as the draw path: not
	// DYNAMIC, inside the buffer. Used by the scene-graph statics, which read
	// a geometry's packed buffers between frames.
	bool copy_vertex_bytes(IDirect3DVertexBuffer9* vb, UINT offset, UINT bytes, std::vector<uint8_t>& out,
		const char*& reject);
	// Same for `count` indices from index `first` of an index buffer (16 or
	// 32 bit), widened to u32.
	bool copy_index_bytes(IDirect3DIndexBuffer9* ib, UINT first, UINT count, std::vector<uint32_t>& out,
		const char*& reject);

	// ---- Transforms ------------------------------------------------------------

	// The NiDX9Renderer WORLD matrix (D3D row-major, row-vector convention) of
	// the draw being issued. FNV renders camera-relative, so this maps object
	// space to camera-relative world, which is what the Remix camera expects.
	const float* world_matrix();
	// Remix 3x4 row-major: row r = D3D column r.
	remixapi_Transform to_remix(const float* m16);

	// DrawInstance.doubleSided for a draw, from the device's current
	// D3DRS_CULLMODE. The runtime builds every API mesh with clockwise front
	// faces (rtx_remix_api.cpp CreateMesh: VK_FRONT_FACE_CLOCKWISE) and culls
	// back faces unless doubleSided, the D3D9 route's equivalent of
	// D3DCULL_CCW. The wrapper copies the game's index order unchanged, so
	// only a D3DCULL_CCW draw may go out single-sided; D3DCULL_NONE (and
	// D3DCULL_CW, whose front faces are counter-clockwise) go double-sided.
	// GetRenderState is answered from the bridge client's state shadow.
	bool double_sided_for_cull(IDirect3DDevice9* dev);

	// ---- Runtime --------------------------------------------------------------

	// The Remix API function table, or nullptr when the API isn't initialised
	// or lacks an entry point these paths need.
	remixapi_Interface* bridge();
	UINT frame();

	// Per-frame budget of new API objects. A draw that cannot take a slot is
	// not drawn this frame (or keeps its current copy) and asks again at its
	// next draw. `key` names what is being built (a material key, a mesh
	// key), so the budget is a fair queue: whoever was refused waits, and at
	// each frame start the oldest waiters (refused first, still asking) get
	// the frame's slots reserved, so a draw the game issues late in the frame
	// (emissive signs, unlit FX) can't lose to earlier draws every frame.
	// `verts` > 0 is a mesh build of that many vertices: one of at most
	// kSmallMeshVerts takes a slot of its own small-mesh pool
	// (kSmallBuildsPerFrame, no texture uploads) instead of MaxBuildsPerFrame.
	// Material builds (texture uploads) pass 0 and also wait while this
	// frame's upload soft budget is spent.
	constexpr uint32_t kSmallMeshVerts = 512;
	constexpr int kSmallBuildsPerFrame = 64;
	bool take_build_budget(uint64_t key, uint32_t verts = 0);

	// The build budget part of the traced->D3D9 status line: grants per frame
	// (small-mesh pool and reserved-for-the-oldest included), refusals, the
	// queue now, and builds that waited more than kLongBudgetWait frames
	// (granted in this window, and still waiting now); both should be ~0.
	// Resets the window's counters.
	constexpr UINT kLongBudgetWait = 30;
	std::string budget_status(double frames);

	// The loaded bridge client (d3d9_remix.dll) carries `marker`, the text of
	// a log message a bridge feature writes, in its read-only data. A path
	// uses it to tell a client with that feature from one without. Searches
	// the image on every call; callers cache the answer.
	bool bridge_client_has(const char* marker);

	// The draw being made keeps its D3D9 route: a first-person draw (the
	// viewmodel projection) while the Pip-Boy is not closed (the engine
	// monitor's state: opening, open or closing), which covers the Pip-Boy's
	// model, screen and arm, or any draw at the main menu or while loading. The
	// world behind the Pip-Boy (the menu-background capture and the closing
	// animation's world) renders with the world projection and keeps its API
	// routes, so opening or closing the Pip-Boy changes nothing in the world's
	// instances. Other menus (console, dialogue, barter) draw the real world.
	// False without the plugin: the API paths keep every world draw.
	bool pipboy_draw();

	// The draw being made is the Pip-Boy's own: a first-person draw while the
	// Pip-Boy is not closed (pipboy_draw without the main menu and loading).
	bool pipboy_own_draw();

	// Every draw, before its route: the draw carries the protocol's
	// per-draw-path bit (remix_protocol::kPerDrawPath) exactly when it is the
	// Pip-Boy's own (pipboy_own_draw), so the Pip-Boy's D3D9 draws (its model
	// and screen) take the runtime's per-draw path instead of D3D9 v2's scene,
	// decided from the same engine-monitor facts as their route. The sky and
	// the D3D9-only lists' draws stay with D3D9 v2 through the Pip-Boy.
	void scope_per_draw_path();

	// ---- Stats / logging -------------------------------------------------------

	struct FeatureStats
	{
		const char* name = "";
		uint32_t live = 0;             // cached meshes
		uint64_t created = 0;
		uint64_t refreshed = 0;        // rebuilds that kept the mesh (geometry refreshed in place)
		uint64_t evicted = 0;
		uint64_t failed = 0;
		uint32_t draws = 0;            // DrawInstance calls this frame
		uint32_t draws_last = 0;
		uint32_t fallbacks = 0;        // draws the feature recognised but did not draw this frame (refused / over budget)
		uint32_t fallbacks_last = 0;
		uint32_t skipped = 0;          // game draws dropped (dedup / extra passes)
		uint32_t skipped_last = 0;
		// Terrain ([RemixApi] TerrainHeightMaps), recounted each frame: layers
		// with a height map across the live quads, and quads still waiting for
		// a height lookup (rebuilt when it finishes).
		uint32_t height_layers = 0;
		uint32_t height_pending = 0;
		// Land and far LOD, recounted each frame: live entries the runtime
		// hides because a texture list names their albedo (MeshEntry::list_hidden).
		uint32_t list_hidden = 0;
		// [RemixApi] RetainedInstances (api_retained.cpp).
		uint32_t retained_live = 0;              // entries with a retained instance
		uint32_t retained_updates = 0;           // transform / visibility / description changes this frame
		uint32_t retained_updates_last = 0;
		uint32_t retained_fallbacks = 0;         // first draws of an entry that took DrawInstance instead
		uint32_t retained_fallbacks_last = 0;
		// [RemixApi] RetainedKeepCulled for this feature this frame (set each
		// frame by api_submit::end_frame_retained: false while the feature is
		// switched off, or for statics while a frame-wide fallback sent every
		// static to D3D9). False hides an entry in any frame that does not
		// draw it.
		bool keep_culled = true;
		// The per-feature RetainedKeepCulledCap was hit (logged once).
		bool logged_cap = false;
		bool logged_ok = false;
		bool logged_fail = false;
		const char* last_reject = "";

		void end_frame()
		{
			draws_last = draws; draws = 0;
			fallbacks_last = fallbacks; fallbacks = 0;
			skipped_last = skipped; skipped = 0;
			retained_updates_last = retained_updates; retained_updates = 0;
			retained_fallbacks_last = retained_fallbacks; retained_fallbacks = 0;
		}
	};

	struct TextureStats
	{
		uint32_t live = 0;
		uint64_t uploads = 0;
		uint64_t upload_bytes = 0;
		uint64_t evicted = 0;
		uint64_t failed = 0;
		uint64_t reused = 0;           // content-hash hits across different pointers
		uint64_t normal_no_gloss = 0;  // normal maps uploaded as BC1: no gloss in alpha (api_textures Flavor::NormalMap)
		// [RemixApi] ImportD3D9Textures: textures handed over by reference, the
		// bytes an upload of them would have sent, and imports refused (those
		// took the upload path).
		uint64_t imports = 0;
		uint64_t import_bytes = 0;
		uint64_t import_fallbacks = 0;
		const char* last_import_reject = "";
	};

	FeatureStats& terrain_stats();
	FeatureStats& far_lod_stats();
	FeatureStats& water_stats();
	FeatureStats& statics_stats();
	FeatureStats& skinned_stats();
	FeatureStats& foliage_stats();
	FeatureStats& unlit_stats();
	FeatureStats& particle_stats();
	TextureStats& texture_stats();

	// One-shot "first success" / "first failure" line per feature, and the
	// reject reason recorded for the periodic status line.
	void note_success(FeatureStats& s, const std::string& detail);
	void note_failure(FeatureStats& s, const char* reason, const std::string& detail = {});

	// Called once per frame (Present): resets the build budget, rolls the
	// per-frame counters and writes the periodic status line.
	void end_frame();

	// "0x%016llX": how materials name API-uploaded textures.
	std::wstring texture_path(uint64_t hash);

	// Common material defaults. Opaque: no draw-call alpha state, no blending,
	// alpha test ALWAYS (0 would be NEVER), metallic 0, DLSS control-mask
	// defaults from remix.h.
	void init_opaque_ext(remixapi_MaterialInfoOpaqueEXT& ext, float roughness);
	void init_material_info(remixapi_MaterialInfo& info, uint64_t hash, void* pnext);
}
