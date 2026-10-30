#pragma once
#include "api_common.hpp"
#include "api_textures.hpp"
#include "stable_origin.hpp"

// Internal to the api_* translation units: one cached API mesh (mesh +
// material + the texture references the material holds), and the per-frame
// DrawInstance / eviction logic every feature shares.

namespace comp::game::api
{
	// What may hide a retained entry the game didn't draw this frame
	// ([RemixApi] RetainedKeepCulled; off, every policy acts as Drawn).
	enum class Residency : uint8_t
	{
		// Hidden the first frame it isn't drawn.
		Drawn,
		// Kept while culled; hidden when its world centre leaves the loaded
		// cell grid, the interior changes or the camera jumps (land, near
		// water, statics).
		Cell,
		// Kept while culled; hidden when the engine swapped it out
		// (RetainedLodRule): engine = its NiGeometry died, was drawn again
		// under another entry, or was app-culled / detached; view = it has
		// been in view and not drawn for RetainedLodStaleFrames, off screen
		// the last verdict held. Also hidden on an interior change or camera
		// jump.
		LodOnScreen,
		// SpeedTree leaves and the instanced distant objects / trees
		// (api_foliage.cpp). Kept while culled (off screen: shadows,
		// reflections, GI behind the camera); hidden when the residency
		// generation moves, its world centre leaves the loaded cell grid
		// (Cell), its NiGeometry dies / is drawn under another entry / is
		// app-culled or detached (when a draw linked one: the engine rule,
		// whatever RetainedLodRule says), or it has been in view and not
		// drawn for RetainedLodStaleFrames (the view rule, always: what the
		// engine stops drawing in view, such as a tree swapped for its
		// distant billboard, is gone from the game's image). A linked leaf
		// also leaves at once when its NiBillboardNode (the leaf shape's
		// parent) is app-culled: SpeedTree LOD hides a tree's leaves that way
		// (0x66BCE0 -> 0x450F90).
		Foliage,
		// Grass batches (api_foliage.cpp): one entry per batch NiGeometry.
		// As Foliage (generation, cell grid, the engine rule on its
		// NiGeometry) but never the view rule: culling never hides a batch. The feature itself hides / destroys it when the
		// batch's instance set changes or the batch is destroyed.
		FoliageBatch,
	};

	struct MeshEntry
	{
		remixapi_MeshHandle mesh = nullptr;
		remixapi_MaterialHandle material = nullptr;
		std::vector<uint64_t> textures;     // references held on api::textures

		uint64_t geo = 0;                   // geometry identity the entry was built from
		UINT last_frame = 0;                // last frame the game drew it
		UINT built_frame = 0;
		UINT drawn_frame = 0xFFFFFFFFu;     // last frame DrawInstance was issued
		remixapi_Transform drawn_xform = {};

		// Far LOD: the sink constants the mesh was built with, each vertex's
		// sink inputs (x, y, z, w, TEXCOORD1.x) and the Z it was built with,
		// so a constant change that moves no vertex costs no rebuild.
		float consts[16] = {};
		std::vector<float> lod_src;
		std::vector<float> lod_z;
		// Far LOD, for the whole-block test of a constant change: the bounds of
		// x, y, the morphed Z and w over the vertices, how many sank at the
		// build (kSankNone, kSankAll, kSankSome) and the GeomorphParams x / y
		// it was built with.
		float lod_lo[4] = {}, lod_hi[4] = {};
		// The hash the mesh was created with (a refresh names the mesh by it).
		uint64_t mesh_hash = 0;
		// Far LOD: the triangles the mesh was built with. A rebuild that keeps
		// them refreshes the mesh in place (refresh_mesh).
		std::vector<uint32_t> lod_tris;
		bool lod_flat_normals = false;      // the detail normal map set every vertex normal to +Z
		uint8_t lod_sank = 2;
		float lod_g0 = 0.0f, lod_g1 = 0.0f;

		// [RemixApi] RetainedInstances: the entry's retained instance and what
		// the runtime last got for it (transform absolute, as the runtime
		// takes it; it subtracts the scene origin at replay).
		uint64_t retained = 0;
		remixapi_Transform retained_xform = {};
		remixapi_InstanceCategoryFlags retained_cats = 0;
		bool retained_double_sided = false;
		bool retained_refresh = false;      // its extension structs changed: the next claim replaces it whole
		bool retained_visible = false;
		UINT retained_frame = 0xFFFFFFFFu;  // last frame a draw claimed it
		FeatureStats* retained_stats = nullptr;

		// RetainedKeepCulled. The mesh's object-space bound (set at build,
		// set_bound), its world centre / radius at the last claim, and the
		// residency generation it was last claimed in.
		Residency residency = Residency::Drawn;
		bool has_bound = false;
		float bound_c[3] = {};
		float bound_r = 0.0f;
		bool has_world = false;
		double world_c[3] = {};
		float world_r = 0.0f;
		uint32_t res_gen = 0;

		// RetainedLodRule = engine (LodOnScreen): the NiGeometry the engine
		// last drew this entry for (render-pass dispatcher; 0 = unknown), and
		// the top of its parent chain when it was linked. A later walk that
		// ends at another node means the block was detached.
		uintptr_t engine_geo = 0;
		uintptr_t engine_top = 0;
		// Foliage: the geometry's parent when it is a tree / billboard node
		// (SpeedTree leaves: the NiBillboardNode), whose APP_CULLED flag is
		// checked every frame the entry isn't drawn. 0 = none.
		uintptr_t engine_parent = 0;

		// Terrain ([RemixApi] TerrainHeightMaps): layers built with a height
		// map, whether a layer's height lookup was still loading at build,
		// and whether heights were wanted at build (a change rebuilds).
		uint8_t height_layers = 0;
		bool height_pending = false;
		bool height_on = false;

		// Land and far LOD: the albedo's D3D9 image hash is in a list that
		// makes the runtime hide the instance (rtxconf::hiding_list).
		bool list_hidden = false;
	};

	// The object-space bounding sphere (AABB centre, half diagonal) of a
	// built mesh's vertices.
	inline void set_bound(MeshEntry& e, const std::vector<remixapi_HardcodedVertex>& verts)
	{
		if (verts.empty()) { e.has_bound = false; return; }
		float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const auto& v : verts)
			for (int k = 0; k < 3; k++)
			{
				lo[k] = (std::min)(lo[k], v.position[k]);
				hi[k] = (std::max)(hi[k], v.position[k]);
			}
		float r2 = 0.0f;
		for (int k = 0; k < 3; k++)
		{
			e.bound_c[k] = 0.5f * (lo[k] + hi[k]);
			const float h = 0.5f * (hi[k] - lo[k]);
			r2 += h * h;
		}
		e.bound_r = std::sqrt(r2);
		e.has_bound = std::isfinite(e.bound_r);
	}

	inline void copy_bound(MeshEntry& to, const MeshEntry& from)
	{
		to.has_bound = from.has_bound;
		std::memcpy(to.bound_c, from.bound_c, sizeof(to.bound_c));
		to.bound_r = from.bound_r;
	}

	// Retained instances ([RemixApi] RetainedInstances, needs StableOrigin).
	// Each entry is registered once and replayed by the runtime every frame;
	// per frame only changes cross the bridge: a transform that really moved,
	// or visibility (RetainedKeepCulled off: an entry the game didn't draw
	// this frame is hidden; on: only what its Residency policy says the engine
	// removed; see Residency). One batched
	// UpdateRetainedInstances ends the frame; one DrawRetainedInstances(O)
	// starts the next (RetainedReplayAtFrameStart; else it ends the frame too).
	namespace retained
	{
		// Config switch. Off: none of the below is ever called with effect.
		bool enabled();

		// Claims the entry's retained instance for this frame's first draw of
		// it at WORLD w, creating it on first use and queueing only real
		// changes. False: the caller issues the frame's DrawInstance instead
		// (feature off, exports missing, no stable origin this frame, or the
		// camera global mid Pip-Boy flip), and the entry stays hidden.
		bool draw(MeshEntry& e, const float* w, remixapi_InstanceCategoryFlags categories,
			bool double_sided, FeatureStats& stats);

		// draw() with the absolute transform already computed (absolute()
		// below), for a caller that decides placements after the draw itself
		// (api_statics.cpp). `instance_pnext` is chained on the InstanceInfo
		// of a create / full update (e.g. an InstanceInfoBlendEXT); it must
		// describe the instance the same way on every call.
		bool draw_absolute(MeshEntry& e, const remixapi_Transform& abs, remixapi_InstanceCategoryFlags categories,
			bool double_sided, FeatureStats& stats, const void* instance_pnext = nullptr);

		// True when a retained instance can be created or claimed this frame:
		// feature on, bridge exports present, stable origin active.
		bool available();

		// The draw's WORLD (camera-relative) as the absolute transform the
		// runtime takes: + this frame's camera, in double.
		remixapi_Transform absolute(const float* w);

		// Same comparison draw() uses to tell a real move from float noise.
		bool same_placement(const remixapi_Transform& a, const remixapi_Transform& b);

		// Destroys the entry's retained instance now (before its mesh).
		void destroy(MeshEntry& e);

		// Destroys the retained instances of many entries in one call.
		void destroy_many(const std::vector<MeshEntry*>& entries);

		// Detaches the entry's retained instance and queues its hide, which
		// the next replay applies (the replay that first draws a replacement
		// created this frame). Returns its handle for destroy_handle once that
		// replay has run; 0 when the entry had none.
		uint64_t retire(MeshEntry& e);
		void destroy_handle(uint64_t handle);

		// Once per frame, before Present: hides entries not drawn this frame,
		// flushes the queued updates (one batched call), and replays the table
		// at the origin when RetainedReplayAtFrameStart is off or this frame
		// never reached replay_for_frame.
		void end_frame();

		// [RemixApi] RetainedReplayAtFrameStart: the frame's one
		// DrawRetainedInstances(O), from the frame's first BeginScene, after
		// stable_origin::begin_frame (so O is this frame's) and before any
		// HUD draw can trigger the runtime's injection. Updates flushed at
		// the previous Present are ahead of it in the bridge's FIFO. Later
		// calls in the same frame return at once.
		void replay_for_frame(UINT frame_number);

		// Counts for the periodic status line (replays since the last call).
		std::string status();

		// Live retained instances, the wrapper's own and external ones.
		uint32_t live_count();

		// ---- RetainedKeepCulled ----

		// Feature switch: RetainedInstances and RetainedKeepCulled both on.
		bool keep_culled();

		// Frames the entry may go undrawn before its feature destroys it:
		// with keep-culled in effect (and the feature under its cap) a
		// visible retained entry gets the long RetainedKeepCulledFrames TTL;
		// anything else (hidden, no retained instance, keep-culled off)
		// keeps `evict_frames`.
		UINT stale_after(const MeshEntry& e, UINT evict_frames, bool kept);

		// Whether keep-culled eviction applies to a feature holding `count`
		// entries: false above RetainedKeepCulledCap (logged once per feature).
		bool keep_in_effect(size_t count, FeatureStats& stats);

		// Hides the entry's retained instance now (queued with the frame's
		// updates). It shows again on its next claim (draw / draw_absolute).
		void hide(MeshEntry& e);

		// Latches this frame's world camera (NiDX9Renderer VIEW / PROJ) for
		// the on-screen test, from a world draw to the backbuffer (not 2D,
		// not the viewmodel projection). Cheap after the first call a frame.
		void note_world_view();

		// The entry's world bound (last claim) is inside this frame's view
		// cone plus a 3-degree guard band. False when no world
		// view was latched this frame (menus, loading: verdicts are held).
		bool on_screen(const MeshEntry& e);
		// The same test for a world sphere (absolute centre, radius) the
		// caller keeps itself (scene_statics' switch / LOD leaves).
		bool on_screen(const double (&c)[3], float r);

		// ~NiGeometry since the last call (scene_statics drains the dead set
		// at Present, before end_frame): LOD entries linked to a dead
		// geometry are hidden and unlinked (RetainedLodRule = engine).
		void on_engine_deaths(const std::vector<uintptr_t>& dead);

		// After every feature destroyed its entries' retained instances
		// (RetainedInstances or StableOrigin switched off, or a rebuild of
		// everything): drops the per-frame bookkeeping. Entries left alive
		// draw with DrawInstance, or get a new instance on their next draw.
		void on_all_destroyed();

		// ---- Instances owned elsewhere (scene_statics.cpp) ----
		// Registered, moved, hidden and destroyed by their owner only:
		// end_frame never hides them, it only keeps replaying the table while
		// any of them is live (set_external_live). Transforms are absolute,
		// in FNV units; the world unit scale is applied when they are sent.

		// Creates `count` instances (CreateRetainedInstances when the bridge
		// exports it, else one CreateRetainedInstance each). A failed one gets
		// handle 0. False when the retained path isn't available.
		bool create_external(const remixapi_InstanceInfo* infos, uint32_t count, uint64_t* out_handles);
		// Queued; flushed with the frame's own updates in end_frame.
		void update_external(const remixapi_RetainedInstanceUpdate& u);
		void destroy_external(const uint64_t* handles, uint32_t count);
		void set_external_live(uint32_t count);
	}

	// Destroys the API objects and drops the texture references. Mesh first:
	// it names the material.
	inline void destroy_entry(MeshEntry& e)
	{
		if (e.retained) retained::destroy(e);
		if (auto* b = bridge())
		{
			if (e.mesh) b->DestroyMesh(e.mesh);
			if (e.material) b->DestroyMaterial(e.material);
		}
		for (const uint64_t t : e.textures) textures::release(t);
		e.mesh = nullptr;
		e.material = nullptr;
		e.textures.clear();
	}

	// The draw's transform in the frame Remix renders in: camera-relative
	// (NiDX9Renderer WORLD), plus the stable render origin's D while that is
	// on, times the world unit scale.
	inline remixapi_Transform frame_transform(const float* w)
	{
		remixapi_Transform t = to_remix(w);
		const double s = stable_origin::scale();
		const double* d = stable_origin::active() ? stable_origin::shift() : nullptr;
		for (int r = 0; r < 3; r++)
		{
			for (int c = 0; c < 3; c++) t.matrix[r][c] = static_cast<float>(static_cast<double>(t.matrix[r][c]) * s);
			t.matrix[r][3] = static_cast<float>((static_cast<double>(t.matrix[r][3]) + (d ? d[r] : 0.0)) * s);
		}
		return t;
	}

	// Issues this frame's DrawInstance at the current NiDX9Renderer WORLD.
	// A second draw of the same entry at the same transform within one frame
	// (an additive light pass over the same geometry) is folded into the
	// first. Returns false when there is no WORLD to draw with.
	inline bool draw_entry(MeshEntry& e, remixapi_InstanceCategoryFlags categories, bool double_sided,
		FeatureStats& stats)
	{
		auto* b = bridge();
		const float* w = world_matrix();
		if (!b || !w || !e.mesh) return false;

		const remixapi_Transform t = frame_transform(w);
		const UINT now = frame();
		e.last_frame = now;
		if (e.drawn_frame == now && std::memcmp(&e.drawn_xform, &t, sizeof(t)) == 0)
		{
			stats.skipped++;
			return true;
		}

		// The frame's first draw of the entry is its retained instance; a
		// further draw at another transform takes a DrawInstance below.
		if (e.drawn_frame != now && retained::enabled() &&
			retained::draw(e, w, categories, double_sided, stats))
		{
			e.drawn_frame = now;
			e.drawn_xform = t;
			return true;
		}

		remixapi_InstanceInfo ii = {};
		ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		ii.pNext = nullptr;
		ii.categoryFlags = categories;
		ii.mesh = e.mesh;
		ii.transform = t;
		ii.doubleSided = double_sided ? 1 : 0;
		b->DrawInstance(&ii);

		e.drawn_frame = now;
		e.drawn_xform = t;
		stats.draws++;
		return true;
	}

	// Whether evict_stale / destroy_stale_retained take the entry this frame:
	// undrawn for longer than retained::stale_after allows (EvictFrames, or
	// RetainedKeepCulledFrames for a visible entry kept while culled).
	inline bool is_stale(const MeshEntry& e, UINT now, UINT keep_frames, bool kept)
	{
		return now - e.last_frame > retained::stale_after(e, keep_frames, kept);
	}

	// Destroys, in one batched call, the retained instances of the entries
	// evict_stale (or an equivalent sweep) is about to destroy.
	template <typename Map>
	void destroy_stale_retained(Map& entries, UINT keep_frames, bool kept = false)
	{
		if (!retained::enabled()) return;
		const UINT now = frame();
		std::vector<MeshEntry*> doomed;
		for (auto& [key, e] : entries)
			if (e.retained && is_stale(e, now, keep_frames, kept)) doomed.push_back(&e);
		retained::destroy_many(doomed);
	}

	// Destroys the retained instances of every entry (one batched call); the
	// entries stay and draw with DrawInstance, or get a new instance later.
	template <typename Map>
	void drop_retained(Map& entries)
	{
		std::vector<MeshEntry*> v;
		for (auto& [key, e] : entries)
			if (e.retained) v.push_back(&e);
		retained::destroy_many(v);
	}

	// Destroys every entry (retained instances first, in one call).
	template <typename Map>
	void destroy_all(Map& entries, FeatureStats& stats)
	{
		drop_retained(entries);
		for (auto& [key, e] : entries)
		{
			destroy_entry(e);
			stats.evicted++;
		}
		entries.clear();
		stats.live = 0;
	}

	// Destroys entries the game hasn't drawn for too long (is_stale).
	template <typename Map>
	void evict_stale(Map& entries, UINT keep_frames, FeatureStats& stats)
	{
		// RetainedKeepCulled: a visible (kept-while-culled) entry lives for
		// the long TTL; hidden ones (left the grid, swapped-out LOD) keep
		// EvictFrames. Decided once, before either loop, so both agree.
		const bool keep = retained::keep_in_effect(entries.size(), stats);
		destroy_stale_retained(entries, keep_frames, keep);
		const UINT now = frame();
		for (auto it = entries.begin(); it != entries.end();)
		{
			if (is_stale(it->second, now, keep_frames, keep))
			{
				destroy_entry(it->second);
				stats.evicted++;
				it = entries.erase(it);
			}
			else ++it;
		}
		stats.live = static_cast<uint32_t>(entries.size());
	}

	// Asset keys (remixapi_AssetKeyEXT): the names mods and captures give an
	// API mesh, while its handle (next_object_hash) stays per session.
	// The runtime's documentation/RemixModPackaging.md has the scheme.
	namespace asset_keys
	{
		// The runtime's default geometry asset hash rule
		// (rtx.geometryAssetHashRuleString = positions,indices,geometrydescriptor)
		// applied to a triangle list: `stride` floats from one position to the
		// next. 0 for an empty mesh.
		uint64_t of(const float* positions, size_t stride, size_t vertex_count, const uint32_t* indices,
			size_t index_count);
		uint64_t of(const std::vector<remixapi_HardcodedVertex>& verts, const std::vector<uint32_t>& indices);

		// Whether the bridge client forwards the struct (checked once, logged).
		bool forwarded();

		// Chains `ext` holding `key` onto `mi`. No-op for key 0 or a bridge
		// client that does not forward the struct. `ext` must outlive the call.
		void chain(remixapi_MeshInfo& mi, remixapi_AssetKeyEXT& ext, uint64_t key);
	}

	// Builds a single-surface mesh. `asset_key` (0 = none) goes out as its
	// remixapi_AssetKeyEXT.
	inline remixapi_MeshHandle create_mesh(uint64_t hash, const std::vector<remixapi_HardcodedVertex>& verts,
		const std::vector<uint32_t>& indices, remixapi_MaterialHandle material, uint64_t asset_key)
	{
		auto* b = bridge();
		if (!b || verts.empty() || indices.empty()) return nullptr;

		remixapi_MeshInfoSurfaceTriangles surf = {};
		surf.vertices_values = verts.data();
		surf.vertices_count = static_cast<uint64_t>(verts.size());
		surf.indices_values = indices.data();
		surf.indices_count = static_cast<uint64_t>(indices.size());
		surf.skinning_hasvalue = 0;
		surf.skinning_value = {};
		surf.material = material;

		remixapi_MeshInfo mi = {};
		mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
		mi.pNext = nullptr;
		mi.hash = hash;
		mi.surfaces_values = &surf;
		mi.surfaces_count = 1;
		remixapi_AssetKeyEXT key = {};
		asset_keys::chain(mi, key, asset_key);

		remixapi_MeshHandle h = nullptr;
		if (b->CreateMesh(&mi, &h) != REMIXAPI_ERROR_CODE_SUCCESS) return nullptr;
		return h;
	}

	// Sends new vertex positions for a live mesh with the same triangles
	// (MeshInfoRefreshGeometryEXT): the handle, its instances and their motion
	// history stay, and the runtime refits the BLAS.
	inline bool refresh_mesh(uint64_t hash, remixapi_MeshHandle mesh, const std::vector<remixapi_HardcodedVertex>& verts,
		const std::vector<uint32_t>& indices, remixapi_MaterialHandle material)
	{
		auto* b = bridge();
		if (!b || !hash || !mesh || verts.empty() || indices.empty()) return false;

		remixapi_MeshInfoSurfaceTriangles surf = {};
		surf.vertices_values = verts.data();
		surf.vertices_count = static_cast<uint64_t>(verts.size());
		surf.indices_values = indices.data();
		surf.indices_count = static_cast<uint64_t>(indices.size());
		surf.skinning_hasvalue = 0;
		surf.skinning_value = {};
		surf.material = material;

		remixapi_MeshInfoRefreshGeometryEXT rg = {};
		rg.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT;
		remixapi_MeshInfo mi = {};
		mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
		mi.pNext = &rg;
		mi.hash = hash;
		mi.surfaces_values = &surf;
		mi.surfaces_count = 1;

		remixapi_MeshHandle h = nullptr;
		return b->CreateMesh(&mi, &h) == REMIXAPI_ERROR_CODE_SUCCESS && h == mesh;
	}

	inline remixapi_MaterialHandle create_material(const remixapi_MaterialInfo& info)
	{
		auto* b = bridge();
		if (!b) return nullptr;
		remixapi_MaterialHandle h = nullptr;
		if (b->CreateMaterial(&info, &h) != REMIXAPI_ERROR_CODE_SUCCESS) return nullptr;
		return h;
	}

	// Per-object API handles: the cache key plus a process-wide build counter,
	// so a rebuilt entry never reuses the hash of one destroyed moments ago.
	// Mods and captures never see them: they name a mesh by its asset key.
	inline uint64_t next_object_hash(uint64_t key, uint64_t domain)
	{
		static uint64_t s_counter = 0;
		Hash64 h;
		h.mix(domain);
		h.mix(key);
		h.mix(++s_counter);
		return h.get();
	}
}
