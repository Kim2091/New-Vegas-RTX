#include "std_include.hpp"
#include "api_mesh.hpp"
#include "stable_origin.hpp"
#include "scene_graph.hpp"
#include "nvse_link.hpp"

#include "shared/common/config.hpp"

// Retained instances ([RemixApi] RetainedInstances). See api_mesh.hpp.
//
// The entry points are not in remixapi_Interface: they are named exports of
// the bridge client (d3d9_remix.dll), resolved here with GetProcAddress. A
// client that lacks them (an older bridge) leaves the feature off and every
// draw keeps its DrawInstance.
//
// Transforms go to the runtime absolute (camera-relative WORLD + camera
// world position, in double), and DrawRetainedInstances passes the stable
// origin O, which the runtime subtracts at replay. When O re-snaps, the new O
// is passed and nothing is resent. Both are kept in FNV units here and are
// multiplied by the world unit scale where they are sent (to_render, s * O).
//
// When: the frame's updates (hides, moves) are flushed at Present in one
// batch; the replay itself is issued at the NEXT frame's first BeginScene
// (RetainedReplayAtFrameStart), after stable_origin::begin_frame, so it
// carries that frame's O (the same O its D3D9 draws are shifted by) and
// reaches the runtime before the HUD draw that triggers the frame's trace.
// The bridge is a FIFO, so the flushed updates are applied first.

namespace comp::game::api::retained
{
	namespace
	{
		struct Exports
		{
			PFN_remixapi_CreateRetainedInstance   create = nullptr;
			PFN_remixapi_UpdateRetainedInstance   update = nullptr;
			PFN_remixapi_UpdateRetainedInstances  update_many = nullptr;
			PFN_remixapi_DestroyRetainedInstances destroy = nullptr;
			PFN_remixapi_DrawRetainedInstances    draw = nullptr;
			// Optional (scene_statics): one message for a batch of creates.
			PFN_remixapi_CreateRetainedInstances  create_many = nullptr;
		};

		bool    g_resolved = false;
		bool    g_available = false;
		Exports g_fn;

		// Entries whose retained instance is visible; end_frame hides the
		// ones no draw claimed this frame.
		std::unordered_set<MeshEntry*> g_visible;
		std::vector<remixapi_RetainedInstanceUpdate> g_pending;
		uint32_t g_live = 0;
		bool g_logged_first = false;
		// The table must be replayed at least once more: something was live
		// at the last replay (replayed once more after the last instance
		// goes, so the runtime drops it), or updates went out since.
		bool g_replay_owed = false;
		// Frame number of the last DrawRetainedInstances: one per frame.
		UINT g_replayed_frame = 0xFFFFFFFFu;
		// Frame number whose start (first BeginScene, origin active) was seen
		// with RetainedReplayAtFrameStart: that frame does not replay at
		// Present, whether or not its start had anything to replay.
		UINT g_start_frame = 0xFFFFFFFFu;
		// Where the replays happened, for the status line.
		uint32_t g_replays_at_start = 0, g_replays_at_present = 0;
		// Instances owned by scene_statics (create_external): counted so the
		// table keeps being replayed, never hidden here.
		uint32_t g_external_live = 0;

		// Updates per UpdateRetainedInstances message: bounds one bridge
		// message's size when a whole cell changes visibility at once.
		constexpr size_t kUpdateChunk = 2048;

		// Translation in world units; the linear part relative to its size.
		// FNV statics don't move: anything under these is float noise from
		// the engine's camera-relative subtraction, never a real move.
		constexpr float kMoveTolerance = 0.05f;
		constexpr float kLinearTolerance = 1.0e-4f;

		// ---- RetainedKeepCulled: residency, view, supersession ----------------------

		// FNV exterior cell edge, world units.
		constexpr double kCellSize = 4096.0;
		// A centre this close outside the loaded rectangle still counts as in
		// it (objects near a border whose bound centre crosses it).
		constexpr double kCellMargin = 64.0;
		// On-screen guard band (3 degrees) and the widest half-angle.
		constexpr float kViewMarginRad = 3.0f * 3.14159265358979f / 180.0f;
		constexpr float kMaxHalfAngle = 1.55f;

		// What the loaded world was at the last end_frame, from the engine
		// monitor's facts. gen moves when it changes discontinuously (a cut:
		// load, door, fast travel, teleport; the grid centre moving more than
		// a cell at once): every entry not claimed since is hidden.
		struct ResidencyState
		{
			bool valid = false;        // the plugin's cell facts hold this frame
			bool seen = false;         // any frame read so far (for change detection)
			bool interior = false;
			bool has_center = false;
			int32_t cx = 0, cy = 0;
			uint32_t dim = 0;
			uint32_t gen = 1;
			bool cut_known = false;
			uint32_t cut_serial = 0;
		};
		ResidencyState g_res;

		// This frame's world camera (note_world_view).
		struct ViewState
		{
			UINT frame = 0xFFFFFFFFu;
			float n[5][3] = {};        // inward plane normals: near, bottom, top, left, right
		};
		ViewState g_view;

		// Geometry key (MeshEntry::geo) -> the entry that last claimed it. A
		// different entry claiming the same geometry (another layer set of a
		// land quad, another water look) supersedes the old one, which is
		// hidden.
		std::unordered_map<uint64_t, MeshEntry*> g_geo_owner;

		// Counts since the last status line (kept = entries left visible
		// while not drawn, last end_frame).
		uint32_t g_kept_last = 0;
		uint32_t g_hid_drawn = 0, g_hid_cell = 0, g_hid_lod = 0, g_hid_gen = 0, g_hid_superseded = 0,
			g_hid_other = 0, g_gen_bumps = 0;
		// Foliage entries hidden by the view rule (in view, not drawn), and
		// leaves hidden because their NiBillboardNode was app-culled
		// (SpeedTree LOD).
		uint32_t g_hid_foliage = 0, g_hid_leaf_culled = 0;

		// ---- RetainedLodRule = engine ------------------------------------------------

		// NiGeometry -> the LOD entry that last claimed it. Another entry
		// claiming the same geometry means the engine drew that block under a
		// new key (re-packed buffer, new texture, new look): the old entry is
		// the same engine object and leaves the scene at once.
		std::unordered_map<uintptr_t, MeshEntry*> g_engine_owner;
		// A Foliage entry was ever linked (they are not in g_engine_owner).
		bool g_foliage_linked = false;
		constexpr int kChainHops = 64;
		bool g_lod_hooks_asked = false;
		// Hides by engine cause since the last status line, and the last
		// end_frame's LOD entries left visible while not drawn: linked (the
		// engine rule decided), unlinked (no NiGeometry: the view rule
		// decided), and linked ones the view rule would have hidden (in view,
		// not drawn for RetainedLodStaleFrames: culled or occluded by the
		// engine, kept).
		uint32_t g_hid_dead = 0, g_hid_rekeyed = 0, g_hid_appculled = 0, g_hid_detached = 0;
		uint32_t g_lod_linked_last = 0, g_lod_unlinked_last = 0, g_lod_inview_last = 0;

		bool lod_engine_rule()
		{
			return shared::common::config::get().remix_api.retained_lod_rule == 1;
		}

		// Walks geo's parent chain to its top (the node with no parent),
		// noting any app-culled node on the way. False when a link is
		// unreadable or the chain is longer than kChainHops.
		bool chain_top(uintptr_t geo, uintptr_t& top, bool& app_culled)
		{
			app_culled = false;
			uintptr_t node = geo;
			for (int hop = 0; hop < kChainHops && node; hop++)
			{
				uintptr_t parent = 0;
				uint32_t flags = 0;
				if (!scene_graph::read_link(node, parent, flags)) return false;
				if (flags & 1u) app_culled = true;
				if (!parent)
				{
					top = node;
					return true;
				}
				node = parent;
			}
			return false;
		}

		void unlink_engine(MeshEntry& e)
		{
			if (!e.engine_geo) return;
			if (const auto it = g_engine_owner.find(e.engine_geo); it != g_engine_owner.end() && it->second == &e)
				g_engine_owner.erase(it);
			e.engine_geo = 0;
			e.engine_top = 0;
			e.engine_parent = 0;
		}

		void update_residency()
		{
			const FnvRemixFacts& f = nvse_link::facts();
			g_res.valid = nvse_link::has(FNVR_F_CELLS);
			bool bump = false;
			if (g_res.valid)
			{
				const bool interior = f.interior != 0;
				const bool has_center = !interior && f.grid_dim > 0;
				if (g_res.seen)
				{
					if (interior != g_res.interior) bump = true;
					else if (!interior && has_center && g_res.has_center &&
						(std::abs(f.grid_cx - g_res.cx) > 1 || std::abs(f.grid_cy - g_res.cy) > 1))
						bump = true;
				}
				g_res.seen = true;
				g_res.interior = interior;
				g_res.has_center = has_center;
				g_res.cx = f.grid_cx;
				g_res.cy = f.grid_cy;
				g_res.dim = f.grid_dim;
				// A cut (load, door, fast travel, teleport); the Pip-Boy's
				// menu camera never makes one.
				if (g_res.cut_known && f.cut_serial != g_res.cut_serial) bump = true;
				g_res.cut_known = true;
				g_res.cut_serial = f.cut_serial;
			}
			if (bump)
			{
				g_res.gen++;
				g_gen_bumps++;
			}
		}

		// Still in the loaded world: claimed in this residency generation and,
		// in an exterior, with its world centre inside the loaded cell grid.
		// No information (main menu, unreadable grid, no bound) = resident.
		bool resident(const MeshEntry& e)
		{
			if (e.res_gen != g_res.gen) return false;
			if (!g_res.valid || g_res.interior || !g_res.has_center || !e.has_world) return true;
			const int32_t half = static_cast<int32_t>(g_res.dim / 2);
			const double x0 = static_cast<double>(g_res.cx - half) * kCellSize - kCellMargin;
			const double y0 = static_cast<double>(g_res.cy - half) * kCellSize - kCellMargin;
			const double x1 = static_cast<double>(g_res.cx - half + static_cast<int32_t>(g_res.dim)) * kCellSize + kCellMargin;
			const double y1 = static_cast<double>(g_res.cy - half + static_cast<int32_t>(g_res.dim)) * kCellSize + kCellMargin;
			return e.world_c[0] >= x0 && e.world_c[0] <= x1 && e.world_c[1] >= y0 && e.world_c[1] <= y1;
		}

		// The entry's world bound at its last claim (absolute; its object
		// bound through the absolute transform), and the residency generation.
		void stamp(MeshEntry& e, const remixapi_Transform& t)
		{
			const auto& m = t.matrix;
			const float c[3] = { e.has_bound ? e.bound_c[0] : 0.0f, e.has_bound ? e.bound_c[1] : 0.0f,
				e.has_bound ? e.bound_c[2] : 0.0f };
			float scale = 0.0f;
			for (int r = 0; r < 3; r++)
			{
				e.world_c[r] = static_cast<double>(m[r][3]) + m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2];
				// Largest column length: the most the linear part stretches.
				const float col = std::sqrt(m[0][r] * m[0][r] + m[1][r] * m[1][r] + m[2][r] * m[2][r]);
				scale = (std::max)(scale, col);
			}
			e.world_r = e.has_bound ? e.bound_r * scale : 0.0f;
			e.has_world = std::isfinite(e.world_c[0]) && std::isfinite(e.world_c[1]) && std::isfinite(e.world_c[2]);
			e.res_gen = g_res.gen;
		}

		void queue_hide(MeshEntry& e)
		{
			remixapi_RetainedInstanceUpdate u = {};
			u.handle = e.retained;
			u.flags = REMIXAPI_RETAINED_UPDATE_VISIBILITY;
			u.visible = 0;
			g_pending.push_back(u);
			e.retained_visible = false;
			if (e.retained_stats) e.retained_stats->retained_updates++;
		}

		// Keep-culled needs the engine monitor's cell facts: without them
		// nothing would ever leave the loaded world.
		bool keep_mode()
		{
			return shared::common::config::get().remix_api.retained_keep_culled && nvse_link::has(FNVR_F_CELLS);
		}

		// e claims its geometry (after stamp). A different visible entry that
		// held the same geometry AT THE SAME PLACE and wasn't claimed this
		// frame is superseded and hidden. Entries sharing a buffer at other
		// places (water planes of one quad VB at different anchors) are left
		// alone: both are drawn, or both are kept while culled.
		void own_geometry(MeshEntry& e, UINT now)
		{
			if (!e.geo || !e.retained || !keep_mode()) return;
			auto& owner = g_geo_owner[e.geo];
			if (owner && owner != &e && owner->retained && owner->retained_visible && owner->retained_frame != now &&
				owner->has_world && e.has_world)
			{
				constexpr double kSamePlace = 1.0;
				bool same = true;
				for (int k = 0; k < 3; k++)
					same = same && std::fabs(owner->world_c[k] - e.world_c[k]) <= kSamePlace;
				if (same)
				{
					queue_hide(*owner);
					g_visible.erase(owner);
					g_hid_superseded++;
				}
			}
			owner = &e;
		}

		// RetainedLodRule = engine: a LOD entry claimed by a draw learns the
		// NiGeometry being rendered (the dispatcher is inside that geometry's
		// pass, so it is alive) and the top of its parent chain. A different
		// entry that held the same geometry and wasn't drawn this frame is the
		// same block under an old key: it is hidden now (rekeyed).
		void link_engine(MeshEntry& e, UINT now)
		{
			// Foliage links whatever RetainedLodRule says (its view rule
			// applies on top, see end_frame).
			const bool foliage = e.residency == Residency::Foliage || e.residency == Residency::FoliageBatch;
			const bool policy = (e.residency == Residency::LodOnScreen && lod_engine_rule()) || foliage;
			if (!policy || !keep_mode() || !scene_graph::hooks_installed())
				return;
			const uintptr_t geo = scene_graph::current_geometry();
			if (!geo || scene_graph::death_pending(geo)) return;
			if (e.engine_geo != geo)
			{
				unlink_engine(e);
				uintptr_t top = 0;
				bool culled = false;
				e.engine_geo = geo;
				e.engine_top = chain_top(geo, top, culled) ? top : 0;
				// Foliage: remember a tree / billboard parent (a SpeedTree leaf
				// shape's NiBillboardNode), whose APP_CULLED end_frame checks.
				e.engine_parent = 0;
				uintptr_t parent = 0;
				uint32_t flags = 0;
				if (foliage && scene_graph::read_link(geo, parent, flags) && parent)
					if (const auto* ci = scene_graph::class_of(parent); ci && ci->subtree_tree)
						e.engine_parent = parent;
			}
			// Foliage: one NiGeometry may draw several leaf windows (each its
			// own entry), so there is no single owner to supersede; its own
			// path retires replaced batches (api_foliage.cpp).
			if (foliage)
			{
				g_foliage_linked = true;
				return;
			}
			MeshEntry*& owner = g_engine_owner[geo];
			if (owner && owner != &e && owner->retained_frame != now)
			{
				if (owner->retained && owner->retained_visible)
				{
					queue_hide(*owner);
					g_visible.erase(owner);
					g_hid_rekeyed++;
				}
				owner->engine_geo = 0;
				owner->engine_top = 0;
			}
			owner = &e;
		}

		// RetainedLodRule = engine, at end_frame for a linked LOD entry the
		// game didn't draw: whether the engine took the block out of the
		// world: its NiGeometry died, or an app-culled node or a detach
		// sits on its chain (the chain now ends somewhere other than where it
		// ended while the block was drawn). Culling and occlusion change none
		// of that: those blocks stay. An unreadable chain holds the verdict.
		bool engine_removed(MeshEntry& e)
		{
			const uintptr_t geo = e.engine_geo;
			if (scene_graph::death_pending(geo) || !scene_graph::begin_resolve(geo))
			{
				unlink_engine(e);
				g_hid_dead++;
				return true;
			}
			uintptr_t top = 0;
			bool culled = false;
			const bool ok = chain_top(geo, top, culled);
			scene_graph::end_resolve();
			if (!ok) return false;
			if (culled)
			{
				g_hid_appculled++;
				return true;
			}
			if (e.engine_top && top != e.engine_top)
			{
				g_hid_detached++;
				return true;
			}
			return false;
		}

		bool resolve()
		{
			if (g_resolved) return g_available;
			g_resolved = true;

			HMODULE m = shared::globals::d3d9_chain_module;
			if (m)
			{
				g_fn.create = reinterpret_cast<PFN_remixapi_CreateRetainedInstance>(
					GetProcAddress(m, "remixapi_CreateRetainedInstance"));
				g_fn.update = reinterpret_cast<PFN_remixapi_UpdateRetainedInstance>(
					GetProcAddress(m, "remixapi_UpdateRetainedInstance"));
				g_fn.update_many = reinterpret_cast<PFN_remixapi_UpdateRetainedInstances>(
					GetProcAddress(m, "remixapi_UpdateRetainedInstances"));
				g_fn.destroy = reinterpret_cast<PFN_remixapi_DestroyRetainedInstances>(
					GetProcAddress(m, "remixapi_DestroyRetainedInstances"));
				g_fn.draw = reinterpret_cast<PFN_remixapi_DrawRetainedInstances>(
					GetProcAddress(m, "remixapi_DrawRetainedInstances"));
				g_fn.create_many = reinterpret_cast<PFN_remixapi_CreateRetainedInstances>(
					GetProcAddress(m, "remixapi_CreateRetainedInstances"));
			}
			g_available = g_fn.create && g_fn.update && g_fn.update_many && g_fn.destroy && g_fn.draw;

			if (!g_available)
				shared::common::log("RemixApi",
					"RetainedInstances: the Remix bridge does not export the retained-instance calls "
					"(older bridge?) -- API draws keep DrawInstance",
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			else if (!stable_origin::enabled())
				shared::common::log("RemixApi",
					"RetainedInstances needs StableOrigin=1 -- API draws keep DrawInstance",
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return g_available;
		}

		// The draw's WORLD (camera-relative) made absolute with this frame's
		// camera, in double; the runtime subtracts O at replay.
		remixapi_Transform absolute_transform(const float* w)
		{
			remixapi_Transform t = to_remix(w);
			const double* cam = stable_origin::camera();
			for (int r = 0; r < 3; r++)
				t.matrix[r][3] = static_cast<float>(static_cast<double>(w[12 + r]) + cam[r]);
			return t;
		}

		bool same_transform(const remixapi_Transform& a, const remixapi_Transform& b)
		{
			for (int r = 0; r < 3; r++)
			{
				for (int c = 0; c < 3; c++)
				{
					const float tol = kLinearTolerance * (std::max)(1.0f, std::fabs(a.matrix[r][c]));
					if (!(std::fabs(a.matrix[r][c] - b.matrix[r][c]) <= tol)) return false;
				}
				if (!(std::fabs(a.matrix[r][3] - b.matrix[r][3]) <= kMoveTolerance)) return false;
			}
			return true;
		}

		// The runtime's description of an instance at absolute transform t
		// (FNV units).
		remixapi_InstanceInfo instance_info(const MeshEntry& e, const remixapi_Transform& t,
			remixapi_InstanceCategoryFlags categories, bool double_sided, const void* pnext = nullptr)
		{
			remixapi_InstanceInfo ii = {};
			ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
			ii.pNext = const_cast<void*>(pnext);
			ii.categoryFlags = categories;
			ii.mesh = e.mesh;
			ii.transform = stable_origin::to_render(t);
			ii.doubleSided = double_sided ? 1 : 0;
			return ii;
		}

		// One DrawRetainedInstances(O) per frame number, with the origin of
		// the frame the caller is in (stable_origin::begin_frame has run).
		bool replay(UINT f)
		{
			if (g_replayed_frame == f) return false;
			const bool any = g_live != 0 || g_external_live != 0;
			if (!any && !g_replay_owed) return false;
			g_fn.draw(stable_origin::origin_render());
			g_replayed_frame = f;
			g_replay_owed = any;
			return true;
		}

		void forget(MeshEntry& e)
		{
			if (e.geo)
				if (const auto it = g_geo_owner.find(e.geo); it != g_geo_owner.end() && it->second == &e)
					g_geo_owner.erase(it);
			unlink_engine(e);
			g_visible.erase(&e);
			if (e.retained_stats && e.retained_stats->retained_live) e.retained_stats->retained_live--;
			if (g_live) g_live--;
			e.retained = 0;
			e.retained_visible = false;
			e.retained_stats = nullptr;
		}
	}

	bool enabled()
	{
		return shared::common::config::get().remix_api.retained_instances;
	}

	bool available()
	{
		return enabled() && resolve() && stable_origin::active();
	}

	remixapi_Transform absolute(const float* w)
	{
		return absolute_transform(w);
	}

	bool same_placement(const remixapi_Transform& a, const remixapi_Transform& b)
	{
		return same_transform(a, b);
	}

	bool draw(MeshEntry& e, const float* w, remixapi_InstanceCategoryFlags categories,
		bool double_sided, FeatureStats& stats)
	{
		if (!enabled()) return false;
		if (!resolve() || !stable_origin::active() || !e.mesh)
		{
			stats.retained_fallbacks++;
			return false;
		}
		// A world draw at draw time: this frame's camera for on_screen().
		note_world_view();

		// The absolute transform needs the camera the draw is relative to.
		// While the engine's camera global disagrees with this frame's camera
		// (the Pip-Boy's camera flips, a jump not yet confirmed) it can't be
		// trusted: an existing instance keeps its last transform, and a new
		// one is not created yet.
		const bool camera_ok = stable_origin::camera_matches_now();
		if (!e.retained && !camera_ok)
		{
			stats.retained_fallbacks++;
			return false;
		}
		return draw_absolute(e, camera_ok ? absolute_transform(w) : e.retained_xform,
			categories, double_sided, stats);
	}

	bool draw_absolute(MeshEntry& e, const remixapi_Transform& abs, remixapi_InstanceCategoryFlags categories,
		bool double_sided, FeatureStats& stats, const void* instance_pnext)
	{
		if (!enabled()) return false;
		if (!resolve() || !stable_origin::active() || !e.mesh)
		{
			stats.retained_fallbacks++;
			return false;
		}
		const UINT now = frame();

		if (!e.retained)
		{
			const remixapi_InstanceInfo ii = instance_info(e, abs, categories, double_sided, instance_pnext);
			uint64_t handle = 0;
			if (g_fn.create(&ii, &handle) != REMIXAPI_ERROR_CODE_SUCCESS || !handle)
			{
				stats.retained_fallbacks++;
				return false;
			}
			e.retained = handle;
			e.retained_xform = abs;
			e.retained_cats = categories;
			e.retained_double_sided = double_sided;
			e.retained_visible = true;       // the runtime creates it visible
			e.retained_frame = now;
			e.retained_stats = &stats;
			g_visible.insert(&e);
			stats.retained_live++;
			g_live++;
			stamp(e, abs);
			own_geometry(e, now);
			link_engine(e, now);
			if (!g_logged_first)
			{
				g_logged_first = true;
				shared::common::log("RemixApi", std::format(
					"RetainedInstances: first retained instance created ({}), origin ({:.0f}, {:.0f}, {:.0f})",
					stats.name, stable_origin::origin()[0], stable_origin::origin()[1], stable_origin::origin()[2]),
					shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			}
			return true;
		}

		remixapi_RetainedInstanceUpdate u = {};
		u.handle = e.retained;
		if (e.retained_refresh || categories != e.retained_cats || double_sided != e.retained_double_sided)
		{
			// A different description (a pass with another cull mode or
			// category, or new extension structs such as instance transforms):
			// replace it whole. Keeps visibility.
			const remixapi_InstanceInfo ii = instance_info(e, abs, categories, double_sided, instance_pnext);
			g_fn.update(e.retained, &ii);
			e.retained_xform = abs;
			e.retained_cats = categories;
			e.retained_double_sided = double_sided;
			e.retained_refresh = false;
			stats.retained_updates++;
		}
		else if (!same_transform(abs, e.retained_xform))
		{
			u.flags |= REMIXAPI_RETAINED_UPDATE_TRANSFORM;
			u.transform = abs;
			e.retained_xform = abs;
		}
		// Otherwise the cached value stands: float noise sends nothing.

		if (!e.retained_visible)
		{
			u.flags |= REMIXAPI_RETAINED_UPDATE_VISIBILITY;
			u.visible = 1;
			e.retained_visible = true;
			g_visible.insert(&e);
		}
		if (u.flags)
		{
			g_pending.push_back(u);
			stats.retained_updates++;
		}
		e.retained_frame = now;
		stamp(e, e.retained_xform);
		own_geometry(e, now);
		link_engine(e, now);
		return true;
	}

	void destroy(MeshEntry& e)
	{
		if (!e.retained) return;
		if (g_fn.destroy) g_fn.destroy(&e.retained, 1);
		forget(e);
	}

	uint64_t retire(MeshEntry& e)
	{
		const uint64_t handle = e.retained;
		if (!handle) return 0;
		if (e.retained_visible) queue_hide(e);
		forget(e);
		return handle;
	}

	void destroy_handle(uint64_t handle)
	{
		if (handle && g_fn.destroy) g_fn.destroy(&handle, 1);
	}

	void destroy_many(const std::vector<MeshEntry*>& entries)
	{
		if (entries.empty() || !g_fn.destroy) return;
		std::vector<uint64_t> handles;
		handles.reserve(entries.size());
		for (const MeshEntry* e : entries)
			if (e->retained) handles.push_back(e->retained);
		if (!handles.empty())
			g_fn.destroy(handles.data(), static_cast<uint32_t>(handles.size()));
		for (MeshEntry* e : entries)
			if (e->retained) forget(*e);
	}

	void end_frame()
	{
		if (!enabled() || !g_available) return;
		const UINT now = frame();
		const auto& cfg = shared::common::config::get().remix_api;
		const bool keep = keep_mode();
		if (keep) update_residency();
		const UINT lod_stale = static_cast<UINT>(cfg.retained_lod_stale_frames);
		const bool view_now = g_view.frame == now;
		const bool engine = keep && lod_engine_rule();
		// The engine rule needs the NiGeometry of each LOD draw: the
		// render-pass dispatcher and ~NiGeometry hooks (between frames, here).
		if (engine && !g_lod_hooks_asked)
		{
			g_lod_hooks_asked = true;
			const bool ok = scene_graph::install_hooks();
			shared::common::log("RemixApi", ok
				? "RetainedLodRule=engine: far LOD / distant water follow their NiGeometry (hidden when it dies, is "
				  "drawn under another entry, or is app-culled / detached; kept while culled or occluded)"
				: "RetainedLodRule=engine: the scene-graph hooks are not available -- far LOD / distant water keep the "
				  "view rule (in view and not drawn for RetainedLodStaleFrames)",
				ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		}
		uint32_t lod_linked = 0, lod_unlinked = 0, lod_inview = 0;

		// What the game didn't draw this frame. RetainedKeepCulled off: all
		// of it is hidden (a far-LOD block the near cells replaced, an
		// unloaded cell, anything off-screen). On: culling is not visibility
		// and the entry stays in the scene unless its
		// residency policy says the engine removed it.
		uint32_t kept = 0;
		for (auto it = g_visible.begin(); it != g_visible.end();)
		{
			MeshEntry* e = *it;
			if (e->retained_frame == now)
			{
				// Drawn this frame: in the world as it is now (a generation
				// that moved at this end_frame included).
				e->res_gen = g_res.gen;
				++it;
				continue;
			}
			bool hide = true;
			if (keep && e->retained_stats && e->retained_stats->keep_culled)
			{
				switch (e->residency)
				{
				case Residency::Drawn:
					g_hid_drawn++;
					break;
				case Residency::Cell:
					hide = !resident(*e);
					if (hide) (e->res_gen != g_res.gen ? g_hid_gen : g_hid_cell)++;
					break;
				case Residency::LodOnScreen:
				{
					if (e->res_gen != g_res.gen) { g_hid_gen++; break; }
					// The view rule's verdict, taken only while the block is
					// in view and the world is being drawn; off screen it is
					// held.
					const bool stale_in_view = view_now && now - e->retained_frame >= lod_stale && on_screen(*e);
					if (engine && e->engine_geo)
					{
						// RetainedLodRule = engine: only what the engine did
						// to the block's NiGeometry.
						hide = engine_removed(*e);
						if (!hide)
						{
							lod_linked++;
							if (stale_in_view) lod_inview++;
						}
						break;
					}
					if (engine) lod_unlinked++;
					hide = stale_in_view;
					if (hide) g_hid_lod++;
					break;
				}
				case Residency::FoliageBatch:
				{
					// Grass batches: generation, cell grid and the engine rule
					// only; culling never hides (api_foliage.cpp retires a
					// batch whose instance set changed or that was destroyed).
					if (e->res_gen != g_res.gen) { g_hid_gen++; break; }
					if (!resident(*e)) { g_hid_cell++; break; }
					if (e->engine_geo && ((now + static_cast<UINT>(reinterpret_cast<uintptr_t>(e) >> 4)) & 15u) == 0 &&
						engine_removed(*e))
						break;   // counted by engine_removed
					hide = false;
					break;
				}
				case Residency::Foliage:
				{
					if (e->res_gen != g_res.gen) { g_hid_gen++; break; }
					if (!resident(*e)) { g_hid_cell++; break; }
					// A leaf whose NiBillboardNode is app-culled (SpeedTree LOD
					// hid the tree's leaves): gone at once, checked every frame
					// (one guarded read). A geometry that died is left to
					// on_engine_deaths.
					if (e->engine_parent && e->engine_geo && !scene_graph::death_pending(e->engine_geo))
					{
						uintptr_t pp = 0;
						uint32_t pflags = 0;
						if (scene_graph::read_link(e->engine_parent, pp, pflags) && (pflags & 1u))
						{
							g_hid_leaf_culled++;
							break;
						}
					}
					// The chain walk for a linked entry, one frame in 16 per
					// entry (staggered): there can be hundreds of kept
					// entries, and a death is seen at once anyway
					// (on_engine_deaths); app-cull / detach may lag.
					if (e->engine_geo && ((now + static_cast<UINT>(reinterpret_cast<uintptr_t>(e) >> 4)) & 15u) == 0 &&
						engine_removed(*e))
						break;   // counted by engine_removed
					// In view and not drawn: the game's image has none of it.
					hide = view_now && now - e->retained_frame >= lod_stale && on_screen(*e);
					if (hide) g_hid_foliage++;
					break;
				}
				}
			}
			else g_hid_other++;
			if (!hide) { kept++; ++it; continue; }
			queue_hide(*e);
			it = g_visible.erase(it);
		}
		g_kept_last = kept;
		g_lod_linked_last = lod_linked;
		g_lod_unlinked_last = lod_unlinked;
		g_lod_inview_last = lod_inview;

		for (remixapi_RetainedInstanceUpdate& u : g_pending)
			if (u.flags & REMIXAPI_RETAINED_UPDATE_TRANSFORM) u.transform = stable_origin::to_render(u.transform);
		for (size_t i = 0; i < g_pending.size(); i += kUpdateChunk)
		{
			const size_t n = (std::min)(kUpdateChunk, g_pending.size() - i);
			g_fn.update_many(g_pending.data() + i, static_cast<uint32_t>(n));
		}
		if (!g_pending.empty()) g_replay_owed = true;
		g_pending.clear();

		// The runtime replays the table only when asked; ask every frame
		// while anything is registered (and once more after the last goes).
		// RetainedReplayAtFrameStart: the frame's replay already went out at
		// its first BeginScene (replay_for_frame); the updates just flushed
		// are applied by the next frame's. With the switch off, or for a
		// frame that never began a scene, the replay goes out here. A frame
		// that began with nothing to replay and registered its first
		// instances during its draws does not replay late here: that replay
		// would land after the trace, and the next frame's start has it.
		if (!shared::common::config::get().remix_api.retained_replay_at_frame_start || g_start_frame != now)
		{
			if (replay(now)) g_replays_at_present++;
		}
	}

	void replay_for_frame(UINT frame_number)
	{
		if (!shared::common::config::get().remix_api.retained_replay_at_frame_start) return;
		if (!enabled() || !g_available) return;
		// Once per frame: later BeginScenes of the frame return here.
		if (g_start_frame == frame_number) return;
		// Only with this frame's origin; with the origin inactive the Present
		// fallback replays.
		if (!stable_origin::active()) return;
		g_start_frame = frame_number;
		if (replay(frame_number)) g_replays_at_start++;
	}

	uint32_t live_count()
	{
		return g_live + g_external_live;
	}

	std::string status()
	{
		if (!enabled()) return {};
		const auto& cfg = shared::common::config::get().remix_api;
		std::string s = std::format("retained: live={} external={} visible={} replays start/present={}/{}{}",
			g_live, g_external_live, g_visible.size(), g_replays_at_start, g_replays_at_present,
			cfg.retained_replay_at_frame_start ? "" : " [replay at Present]");
		if (cfg.retained_keep_culled)
		{
			s += std::format(" | keepCulled: kept={} hidden cell={} lod={} foliage={} leafCulled={} gen={} superseded={} "
				"drawn={} other={} genBumps={} gen={} grid={}",
				g_kept_last, g_hid_cell, g_hid_lod, g_hid_foliage, g_hid_leaf_culled, g_hid_gen, g_hid_superseded, g_hid_drawn, g_hid_other,
				g_gen_bumps, g_res.gen,
				!g_res.valid ? std::string("none") : g_res.interior ? std::string("interior")
					: g_res.has_center ? std::format("({},{}) dim {}", g_res.cx, g_res.cy, g_res.dim) : std::string("?"));
			// lodRule: the engine rule's hides by cause, and the LOD entries
			// kept while not drawn at the last end_frame: linked (engine rule),
			// unlinked (no NiGeometry, view rule; lod= counts its hides), and
			// linked ones in view and not drawn (the engine culled / occluded
			// them; the view rule would have hidden them).
			if (cfg.retained_lod_rule == 1)
				s += std::format(" | lodRule=engine hidden[dead={} rekeyed={} appCulled={} detached={}] "
					"kept[linked={} unlinked={} inViewNotDrawn={}] owners={}",
					g_hid_dead, g_hid_rekeyed, g_hid_appculled, g_hid_detached, g_lod_linked_last,
					g_lod_unlinked_last, g_lod_inview_last, g_engine_owner.size());
			else
				s += " | lodRule=view";
		}
		else
			s += " | keepCulled off (hidden when not drawn)";
		g_replays_at_start = g_replays_at_present = 0;
		g_hid_drawn = g_hid_cell = g_hid_lod = g_hid_gen = g_hid_superseded = g_hid_other = g_gen_bumps = 0;
		g_hid_foliage = g_hid_leaf_culled = 0;
		g_hid_dead = g_hid_rekeyed = g_hid_appculled = g_hid_detached = 0;
		return s;
	}

	bool keep_culled()
	{
		return enabled() && keep_mode();
	}

	UINT stale_after(const MeshEntry& e, UINT evict_frames, bool kept)
	{
		if (!kept || !e.retained || !e.retained_visible) return evict_frames;
		const UINT ttl = static_cast<UINT>(shared::common::config::get().remix_api.retained_keep_culled_frames);
		return (std::max)(evict_frames, ttl);
	}

	bool keep_in_effect(size_t count, FeatureStats& stats)
	{
		if (!keep_culled() || !stats.keep_culled) return false;
		const auto cap = static_cast<size_t>(shared::common::config::get().remix_api.retained_keep_culled_cap);
		if (count <= cap) return true;
		if (!stats.logged_cap)
		{
			stats.logged_cap = true;
			shared::common::log("RemixApi", std::format(
				"RetainedKeepCulled: {} holds {} entries, over RetainedKeepCulledCap {} -- EvictFrames applies to all "
				"of them while it stays over (logged once)", stats.name, count, cap),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}
		return false;
	}

	void hide(MeshEntry& e)
	{
		if (!e.retained || !e.retained_visible) return;
		queue_hide(e);
		g_visible.erase(&e);
	}

	void note_world_view()
	{
		const UINT now = frame();
		if (g_view.frame == now) return;
		if (!game::rendering_to_backbuffer || game::is_2d() || game::uses_viewmodel_projection()) return;
		const float* v = game::get_renderer_matrix(RENDERER_VIEW_OFF);
		const float* p = game::get_renderer_matrix(RENDERER_PROJ_OFF);
		if (!v || !p || !(p[0] > 1e-4f) || !(p[5] > 1e-4f)) return;
		// D3D row-vector VIEW: camera axes are its columns (right, up,
		// forward), in world space; FNV's VIEW has no translation (the
		// camera is stable_origin::camera()).
		const float R[3] = { v[0], v[4], v[8] };
		const float U[3] = { v[1], v[5], v[9] };
		const float F[3] = { v[2], v[6], v[10] };
		const float hh = (std::min)(std::atan(1.0f / p[0]) + kViewMarginRad, kMaxHalfAngle);
		const float hv = (std::min)(std::atan(1.0f / p[5]) + kViewMarginRad, kMaxHalfAngle);
		const float sv = std::sin(hv), cv = std::cos(hv), sh = std::sin(hh), ch = std::cos(hh);
		for (int k = 0; k < 3; k++)
		{
			g_view.n[0][k] = F[k];
			g_view.n[1][k] = F[k] * sv - U[k] * cv;
			g_view.n[2][k] = F[k] * sv + U[k] * cv;
			g_view.n[3][k] = F[k] * sh - R[k] * ch;
			g_view.n[4][k] = F[k] * sh + R[k] * ch;
		}
		g_view.frame = now;
	}

	bool on_screen(const MeshEntry& e)
	{
		if (g_view.frame != frame() || !stable_origin::active()) return false;
		if (!e.has_world) return true;
		return on_screen(e.world_c, e.world_r);
	}

	bool on_screen(const double (&c)[3], float r)
	{
		if (g_view.frame != frame() || !stable_origin::active()) return false;
		const double* cam = stable_origin::camera();
		const double d[3] = { c[0] - cam[0], c[1] - cam[1], c[2] - cam[2] };
		for (int p = 0; p < 5; p++)
		{
			const double dist = g_view.n[p][0] * d[0] + g_view.n[p][1] * d[1] + g_view.n[p][2] * d[2];
			if (dist < -static_cast<double>(r)) return false;
		}
		return true;
	}

	bool create_external(const remixapi_InstanceInfo* in, uint32_t count, uint64_t* out_handles)
	{
		if (!count) return true;
		if (!available()) return false;
		static std::vector<remixapi_InstanceInfo> s_infos;
		s_infos.assign(in, in + count);
		for (remixapi_InstanceInfo& ii : s_infos) ii.transform = stable_origin::to_render(ii.transform);
		const remixapi_InstanceInfo* infos = s_infos.data();
		if (g_fn.create_many)
		{
			if (g_fn.create_many(infos, count, out_handles) == REMIXAPI_ERROR_CODE_SUCCESS) return true;
			for (uint32_t i = 0; i < count; i++) out_handles[i] = 0;
			return false;
		}
		bool any = false;
		for (uint32_t i = 0; i < count; i++)
		{
			out_handles[i] = 0;
			if (g_fn.create(&infos[i], &out_handles[i]) != REMIXAPI_ERROR_CODE_SUCCESS) out_handles[i] = 0;
			any = any || out_handles[i] != 0;
		}
		return any;
	}

	void update_external(const remixapi_RetainedInstanceUpdate& u)
	{
		g_pending.push_back(u);
	}

	void destroy_external(const uint64_t* handles, uint32_t count)
	{
		if (count && g_fn.destroy) g_fn.destroy(handles, count);
	}

	void set_external_live(uint32_t count)
	{
		g_external_live = count;
	}

	void on_engine_deaths(const std::vector<uintptr_t>& dead)
	{
		// (Foliage entries are linked without an owner-map entry.)
		if (dead.empty() || (g_engine_owner.empty() && !g_foliage_linked)) return;
		const std::unordered_set<uintptr_t> gone(dead.begin(), dead.end());
		for (const uintptr_t geo : dead) g_engine_owner.erase(geo);
		// Every visible entry linked to one of them (the owner, and any other
		// entry drawn for it the same frame): the block is gone for good.
		for (auto it = g_visible.begin(); it != g_visible.end();)
		{
			MeshEntry* e = *it;
			if (!e->engine_geo || !gone.count(e->engine_geo)) { ++it; continue; }
			e->engine_geo = 0;
			e->engine_top = 0;
			e->engine_parent = 0;
			queue_hide(*e);
			g_hid_dead++;
			it = g_visible.erase(it);
		}
	}

	void on_all_destroyed()
	{
		if (g_live)
			shared::common::log("RemixApi", std::format(
				"RetainedInstances: {} retained instance(s) not destroyed by their feature -- forgotten", g_live),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		g_visible.clear();
		g_pending.clear();
		g_live = 0;
		g_external_live = 0;
		g_replay_owed = false;
		g_geo_owner.clear();
		g_engine_owner.clear();
	}
}
