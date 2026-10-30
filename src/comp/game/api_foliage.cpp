#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "api_identity.hpp"
#include "api_rtxconf.hpp"
#include "api_material.hpp"
#include "scene_graph.hpp"
#include "stable_origin.hpp"
#include "nvse_link.hpp"
#include "vs_expand.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/config.hpp"

/*
 * Grass batches, SpeedTree leaves and the instanced distant objects / trees
 * through the Remix API.
 *
 * What is taken
 * -------------
 * Draws the renderer sends down the vertex-expander route as Grass
 * (GRASS20xx), Leaf (STLEAF), DistLod (DISTLOD2002/2003) or DistTree
 * (DISTTREE). vs_expand already turns each into a static copy (the instance
 * positions baked in, wind dropped: expand_instanced drops the wind term and
 * expand_leaves applies no STLEAF rock / rustle; leaves as world-up cards spun
 * by a per-leaf hash), keyed by the draw's buffer, window, sampled contents
 * and every constant the expansion reads. That copy is built once more here as an API mesh. DistLod
 * and DistTree take the leaves' slot-keyed path.
 * Grass, DistLod and DistTree: the mesh is instead one
 * copy of the batch's model, in its own space, GPU-instanced
 * (InstanceInfoGpuInstancingEXT) at the instances' transforms, which the
 * expansion's arithmetic gives (vs_expand::describe_instances). Its asset key
 * is then the model's, the same in every batch and session, so a mesh
 * replacement of the model applies to every instance. A batch whose copies
 * are not all the same model (make_template), or a bridge client that
 * doesn't forward the struct, is baked as above.
 * Refused (counted per reason; the draw is not drawn):
 *   * unlinked         grass whose draw did not come from the render-pass
 *                      dispatcher with a grass batch NiGeometry (below);
 *   * dynamic / rewritten  a DYNAMIC source VB, or one refilled after a mesh
 *                      was built from it (api_identity);
 *   * the statics' material rules (shader, alpha func, blend, texture,
 *     rtx.conf lists the API can't express).
 * Not refused, the batch's / slot's current copy draws instead:
 *   * churn            one batch (grass) / one draw slot (leaves) that needed
 *                      more than kChurnBuilds meshes within kChurnWindow
 *                      frames builds nothing new for kChurnHold frames;
 *   * budget           the per-frame build budget (MaxBuildsPerFrame, and
 *                      kGrassBuildsPerFrame for grass) is spent this frame;
 *   * a retry pending after a failed build.
 * (With no copy yet, those draws are not drawn this frame.)
 *
 * Grass identity: one batch = one NiGeometry
 * ------------------------------------------
 * Every batch of one grass type shares ONE NiGeometryData and so one vertex
 * buffer (ctor 0xBD8AE0 passes template+0x08); batches differ only in their
 * instance data (batch+0x14 world positions, count at +0x12), uploaded as VS
 * constants per draw. A VB window is therefore NOT a batch: two batches with
 * the same instance count draw the same window. Each batch has its own
 * NiGeometry (batch+0, vtable TallGrassTriStrips 0x10BCC90 / TriShape
 * 0x10BCDC8), which the render-pass dispatcher (0xB994F0, scene_graph) hands
 * over as the pass's geometry. So a grass part is (that NiGeometry, first
 * index), and its mesh is keyed by the part plus the expansion's key (VS, the
 * constants = the batch's current instance set) plus the buffer generations
 * and the material. A draw that doesn't come with a grass batch
 * NiGeometry is refused (unlinked): no identity is guessed.
 *
 * A part holds one mesh at a time. The same batch drawn with a new instance
 * set (a new expansion key) is a new generation: the new mesh is built (within
 * the build budget; over it the batch draws its current copy this frame) and
 * the part's old mesh and instance are destroyed. Supersession never crosses
 * batches.
 * The grass batch hooks (scene_graph: append 0xBD8550, remove 0xBD85B0,
 * destroy 0xBD8F90) report changes the draws can't see: a batch whose
 * instance set changed since its last claim is hidden at the end of the frame
 * (it may be off screen: its old copy must not stay in the scene), and shown
 * again by its next draw, rebuilt if the constants moved; a destroyed batch
 * is dropped outright, as is one whose NiGeometry died (~NiGeometry 0xA80580).
 *
 * Leaves
 * ------
 * One placement per leaf NiTriShape (cloned per tree for all but a model's
 * first tree) when the dispatcher brought the draw, else per absolute WORLD.
 * A leaf mesh is shared by the trees of one model; its placements are not.
 * The leaf shape's parent is the tree's NiBillboardNode (BSTreeNode child 2),
 * which SpeedTree LOD app-culls to hide the leaves (0x66BCE0 -> 0x450F90):
 * that hides the placement at once (api_retained.cpp, Residency::Foliage).
 *
 * Mesh, material, placement
 * -------------------------
 * Material: the statics builder (api_material: ImportD3D9Textures, normal /
 * gloss as the protocol would send them, rtx.conf lists as instance
 * categories), with the cutout the grass PS computes: grass = ALPHAFUNC
 * GREATER at the PS's AlphaTestRef (its binary alpha), leaves and the rest =
 * the draw's own alpha test. A blended draw carries its alpha state on the
 * instance (as scene statics). Vertex colour modulates the albedo
 * (InstanceInfoBlendEXT), as stage 0 does; alpha = texture (fnv_engage's
 * SELECTARG1). Always double-sided (the route draws the cards with CULL_NONE).
 *
 * Suppression
 * -----------
 * The frame a placement's retained instance is created (or shown again after
 * a hide) it is also drawn with that frame's DrawInstance: the runtime only
 * draws what the replay at the NEXT frame's start carries. While the
 * retained path is unavailable, the camera global disagrees with this
 * frame's camera or the camera is settling after a jump, every draw goes out
 * as its own DrawInstance (transient) and kept placements are hidden that
 * frame unless drawn.
 *
 * Lifetime (RetainedKeepCulled; api_retained.cpp)
 * -----------------------------------------------
 * Not "drawn this frame". Grass (Residency::FoliageBatch): kept while culled
 * (culling never hides a batch) and gone when the residency generation
 * moves (interior change, camera jump), its world centre leaves the loaded
 * cell grid, its NiGeometry dies / is app-culled / detached, the batch is
 * destroyed or its instance set changes (above), or the game draws it fully
 * faded out (AllCulled). Leaves (Residency::Foliage): the same cell / engine
 * rules, the NiBillboardNode's APP_CULLED, and in view and not drawn for
 * RetainedLodStaleFrames (the tree swapped to its distant billboard). Also
 * hidden at once: a batch / leaf slot whose draw was refused, and every
 * placement not drawn on a transient frame (retained path unavailable, camera
 * settling: hidden unless drawn that frame). Meshes with no
 * placement left die after EvictFrames.
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;
	using shared::common::PsSlotMap;
	using shared::common::PsSlotRole;
	using shared::common::VsFamily;
	namespace sg = comp::game::scene_graph;

	namespace
	{
		constexpr uint64_t kDomain = 0x464F4C4941474531ull;   // "FOLIAGE1"
		constexpr UINT kRetryFrames = 120;
		// A batch (grass) / slot (leaves) that needs more than kChurnBuilds
		// meshes within kChurnWindow frames builds none for kChurnHold frames.
		constexpr uint32_t kChurnBuilds = 6;
		constexpr UINT kChurnWindow = 300;
		constexpr UINT kChurnHold = 1800;
		// Grass meshes built per frame (first builds and new generations),
		// on top of MaxBuildsPerFrame: a grass-grid move regenerates many
		// batches at once; the rest draw their current copy until a later frame.
		constexpr uint32_t kGrassBuildsPerFrame = 6;

		enum class Reason : uint8_t
		{
			Inactive, Viewmodel, Unlinked, Expand, Dynamic, Rewritten, Churn, Shader, Layout, Blend, AlphaFunc,
			Texture, RtxConf, Budget, Build, Retry, Submit, Count
		};
		constexpr const char* kReasonNames[] = {
			"inactive", "viewmodel", "unlinked", "expand", "dynamic", "rewritten", "churn", "shader", "layout",
			"blend", "alphafunc", "texture", "rtxconf", "budget", "build", "retry", "submit",
		};
		static_assert(std::size(kReasonNames) == static_cast<size_t>(Reason::Count));

		struct Counters
		{
			// Per frame (rolled into *_last at Present).
			uint32_t suppressed_grass = 0, suppressed_leaf = 0;   // draws taken (D3D9 draw skipped)
			uint32_t first = 0;          // claimed, not live yet: drawn with this frame's DrawInstance
			uint32_t transient = 0;      // drawn with a DrawInstance only (retained path unavailable, camera settling)
			uint32_t stale = 0;          // a held / over-budget batch or slot drew its current copy
			uint32_t folded = 0;         // a second draw of a placement already claimed this frame
			uint32_t culled = 0;         // all-faded draws dropped
			uint32_t placed = 0;         // placements created
			uint32_t built = 0, materials_built = 0;
			uint32_t grass_linked = 0;   // grass draws with a batch NiGeometry from the dispatcher
			uint32_t grass_unlinked = 0; // grass draws without one (not drawn)
			uint32_t grass_builds = 0;   // grass meshes built (kGrassBuildsPerFrame)
			uint32_t fallback[static_cast<size_t>(Reason::Count)] = {};
			// Since the last status line.
			uint32_t culled_hidden = 0, superseded = 0, fallback_hidden = 0;
			uint32_t rebuilds = 0;       // grass: a batch's new generation built (its old mesh destroyed)
			uint32_t budget_holds = 0;   // a build over budget: the batch / slot drew its current copy (or nothing)
			uint32_t gen_changes = 0;    // grass: batches the hooks reported changed
			uint32_t gen_hidden = 0;     // grass: instances hidden because their batch changed since the claim
			uint32_t batches_destroyed = 0, batch_deaths = 0;   // grass parts dropped: destroy hook / ~NiGeometry
			uint32_t leaf_moved = 0;     // a linked leaf placement drawn at a new WORLD
			uint32_t leaf_deaths = 0;    // leaf placements dropped: ~NiGeometry
			uint64_t churn_holds = 0, placements_evicted = 0;
			uint32_t instanced = 0, baked = 0;   // instanced-family meshes built each way
			uint32_t instance_updates = 0;       // instanced meshes that took a new instance set in place
			const char* baked_why = nullptr;     // the last reason one was baked
		};
		Counters g_count;
		Counters g_last;   // the per-frame part of the previous frame

		struct Material
		{
			remixapi_MaterialHandle handle = nullptr;
			std::vector<uint64_t> textures;
			uint32_t refs = 0;
			UINT last_used = 0;
			bool declined = false;
			Reason why = Reason::Build;
			UINT retry_frame = 0xFFFFFFFFu;
			bool no_vertex_color = false;
			uint32_t categories = 0;       // rtx.conf lists as instance categories
			bool alpha_dropped = false;    // rtx.ignoreAlphaOnTextures: no alpha state at all
		};
		std::unordered_map<uint64_t, Material> g_materials;

		struct Mesh;

		struct Placement
		{
			MeshEntry e;                       // e.mesh borrowed from the Mesh
			remixapi_Transform abs = {};
			uintptr_t geo = 0;                 // the NiGeometry it was drawn for (0 = unlinked leaf)
			UINT last_frame = 0;
			UINT claimed_frame = 0xFFFFFFFFu;
			UINT shown_frame = 0xFFFFFFFFu;    // frame its instance was created / last shown
			Mesh* owner = nullptr;
		};

		struct Mesh
		{
			MeshEntry e;                       // owns e.mesh; e.material stays null (shared)
			uint64_t key = 0, slot = 0, material_key = 0;
			uint64_t part = 0;                 // grass: the batch part it belongs to
			bool leaf = false;
			bool vertex_color = false;
			bool blend = false;
			uint8_t alpha_func = 0, alpha_ref = 0;
			uint32_t categories = 0;
			UINT verts = 0, tris = 0;
			// Instanced: the mesh is one copy of the model and these are its
			// instances (model space, InstanceInfoGpuInstancingEXT). Empty:
			// the instances are baked into the mesh.
			std::vector<remixapi_Transform> instances;
			uint64_t model_key = 0;            // instanced: the model's asset key
			std::vector<std::unique_ptr<Placement>> placements;
		};
		std::unordered_map<uint64_t, Mesh> g_meshes;

		// Leaves: a draw slot (vs_expand's VB window identity), the mesh it
		// drew last and its build history (churn).
		struct Slot
		{
			uint64_t mesh_key = 0;
			UINT last_frame = 0;
			UINT window_start = 0;
			uint32_t builds = 0;
			UINT hold_until = 0;
		};
		std::unordered_map<uint64_t, Slot> g_slots;

		// Grass: one batch part (its NiGeometry, VS, PS, first index) and
		// the one mesh it holds, its current generation.
		struct Part
		{
			uintptr_t geo = 0;
			uint64_t mesh_key = 0;             // 0 = none built (or dropped)
			uint64_t engine_gen = 0;           // scene_graph::grass_generation at the last claim
			uint32_t generations = 0;          // meshes built for it
			UINT last_frame = 0;
			UINT window_start = 0;
			uint32_t builds = 0;
			UINT hold_until = 0;
		};
		std::unordered_map<uint64_t, Part> g_parts;

		std::unordered_map<uint64_t, UINT> g_retry_at;
		uint32_t g_live_placements = 0;

		UINT g_blocked_frame = 0xFFFFFFFFu;
		bool g_logged_settling = false, g_logged_unlinked = false, g_logged_grass_hooks = false;
		bool g_grass_hooks_asked = false;
		std::unordered_set<uint32_t> g_logged_ps;
		std::vector<uintptr_t> g_scratch_changed, g_scratch_destroyed;

		// Whether the bridge client forwards InstanceInfoGpuInstancingEXT
		// (checked once, logged). Without it the runtime would get the model
		// copy with no instances: one copy at the batch's origin.
		bool instancing_forwarded()
		{
			static int s_forwarded = -1;
			if (s_forwarded < 0)
			{
				const bool ok = bridge_client_has("Forwarding InstanceInfoGpuInstancingEXT");
				s_forwarded = ok ? 1 : 0;
				shared::common::log("RemixApi", ok
					? "foliage: the bridge client forwards InstanceInfoGpuInstancingEXT -- grass and distant batches "
					  "are one model copy instanced at each instance"
					: "foliage: the bridge client does NOT forward InstanceInfoGpuInstancingEXT (older bridge) -- grass "
					  "and distant batches keep their instances baked in, so their models can't be replaced",
					ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
			}
			return s_forwarded == 1;
		}

		Foliage fallback(Reason r, FeatureStats& st)
		{
			g_count.fallback[static_cast<size_t>(r)]++;
			st.fallbacks++;
			st.last_reject = kReasonNames[static_cast<size_t>(r)];
			return Foliage::Declined;
		}

		DWORD render_state(IDirect3DDevice9* dev, D3DRENDERSTATETYPE rs, DWORD fallback_value)
		{
			DWORD v = fallback_value;
			if (FAILED(dev->GetRenderState(rs, &v))) return fallback_value;
			return v;
		}

		uint8_t unorm8(float v)
		{
			if (!(v > 0.0f)) return 0;
			return v >= 1.0f ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f);
		}

		// The placement of a leaf mesh for this draw: the one drawn for the
		// same leaf NiTriShape when the dispatcher linked it, else the one at
		// the same absolute WORLD (unlinked placements only).
		Placement* find_placement(Mesh& m, const remixapi_Transform& t, uintptr_t geo)
		{
			for (auto& p : m.placements)
			{
				if (geo ? p->geo == geo : (!p->geo && retained::same_placement(t, p->abs))) return p.get();
			}
			return nullptr;
		}

		// Hides p unless a draw claimed it this frame. True when it was hidden.
		bool hide_unclaimed(Placement& p)
		{
			if (p.claimed_frame == frame() || !p.e.retained || !p.e.retained_visible) return false;
			retained::hide(p.e);
			return true;
		}

		// Leaves: the mesh this slot drew last, at this placement (not mesh
		// `except`): hidden, because this frame's draw of the slot is not it
		// (it was refused, was faded out, or is a new mesh).
		bool hide_slot_at(uint64_t slot, const remixapi_Transform& t, uintptr_t geo, uint64_t except = 0)
		{
			const auto s = g_slots.find(slot);
			if (s == g_slots.end() || !s->second.mesh_key || s->second.mesh_key == except) return false;
			const auto m = g_meshes.find(s->second.mesh_key);
			if (m == g_meshes.end()) return false;
			Placement* p = find_placement(m->second, t, geo);
			return p && hide_unclaimed(*p);
		}

		// Grass: this batch part's instance, unless claimed this frame (the
		// batch was refused, was faded out, or changed). Only this batch.
		uint32_t hide_part(const Part& part)
		{
			if (!part.mesh_key) return 0;
			const auto m = g_meshes.find(part.mesh_key);
			if (m == g_meshes.end()) return 0;
			uint32_t n = 0;
			for (auto& p : m->second.placements) n += hide_unclaimed(*p) ? 1u : 0u;
			return n;
		}

		void block_frame() { g_blocked_frame = frame(); }

		void release_material(uint64_t key)
		{
			const auto it = g_materials.find(key);
			if (it == g_materials.end() || it->second.refs == 0) return;
			it->second.refs--;
			it->second.last_used = frame();
		}

		void destroy_placements(Mesh& m)
		{
			std::vector<MeshEntry*> doomed;
			for (auto& p : m.placements)
				if (p->e.retained) doomed.push_back(&p->e);
			retained::destroy_many(doomed);
			const auto n = static_cast<uint32_t>(m.placements.size());
			g_live_placements = g_live_placements >= n ? g_live_placements - n : 0;
			m.placements.clear();
		}

		void destroy_mesh(Mesh& m)
		{
			destroy_placements(m);
			m.e.material = nullptr;   // shared: released by key
			destroy_entry(m.e);
			release_material(m.material_key);
		}

		// Destroys one mesh by key (its placements, its retained instances).
		bool destroy_mesh_key(uint64_t key)
		{
			const auto it = g_meshes.find(key);
			if (it == g_meshes.end()) return false;
			destroy_mesh(it->second);
			foliage_stats().evicted++;
			g_meshes.erase(it);
			return true;
		}

		// Grass: drops every part of the given batch geometries with its mesh.
		uint32_t drop_parts(const std::unordered_set<uintptr_t>& geos)
		{
			uint32_t n = 0;
			for (auto it = g_parts.begin(); it != g_parts.end();)
			{
				if (!geos.count(it->second.geo)) { ++it; continue; }
				if (it->second.mesh_key) destroy_mesh_key(it->second.mesh_key);
				it = g_parts.erase(it);
				n++;
			}
			return n;
		}

		void build_material(const material::Desc& md, uint64_t key, Material& m)
		{
			PROFILE_ZONE_N("api::foliage::build_material");
			material::Built b;
			material::build(md, key, kDomain ^ 0x4D4154ull, b);
			m.handle = b.handle;
			m.textures = std::move(b.textures);
			m.no_vertex_color = b.no_vertex_color;
			m.categories = b.categories;
			m.alpha_dropped = b.alpha_dropped;
			switch (b.why)
			{
			case material::Decline::None:    g_count.materials_built++; return;
			case material::Decline::RtxConf: m.declined = true; m.why = Reason::RtxConf; return;
			case material::Decline::Texture: m.declined = true; m.why = Reason::Texture; return;
			case material::Decline::Build:   m.declined = true; m.why = Reason::Build; return;
			}
		}

		// An instanced window (Grass, DistLod, DistTree) as one copy of its
		// model and the instances' transforms. The vertex buffer holds one
		// copy of the model per instance, told apart by TEXCOORD1 only: the
		// template is the copy the first triangle belongs to, in the source
		// vertices' own (model) space. Every other drawn copy must be the same
		// model: as many vertices, byte for byte the same apart from
		// TEXCOORD1, and the same triangles (either winding: a strip's
		// parity can flip between copies). False with `why` otherwise; the
		// batch is then baked.
		template <typename Convert>
		bool make_template(const shared::common::VsDesc& vs, const PsSlotMap* psmap, const DrawDesc& d,
			const Geometry& g, Convert& convert, std::vector<remixapi_HardcodedVertex>& tverts,
			std::vector<uint32_t>& ttris, std::vector<remixapi_Transform>& xforms, const char*& why)
		{
			PROFILE_ZONE_N("api::foliage::make_template");
			static_assert(sizeof(vs_expand::ApiInstances::Xform) == sizeof(remixapi_Transform));
			vs_expand::ApiInstances in;
			const char* r = nullptr;
			if (!vs_expand::describe_instances(vs, psmap, d.base, d.indexed ? d.min_vtx : 0u, d.num_vtx, in, r))
			{
				why = r ? r : "instances";
				return false;
			}
			if (in.vertex_instance.size() != g.count || in.tc1_off < 0 || in.tc1_size <= 0 ||
				static_cast<UINT>(in.tc1_off + in.tc1_size) > g.stride)
			{
				why = "vertex window mismatch";
				return false;
			}

			// Each instance's copy: its vertices in window order.
			const size_t n = in.xforms.size();
			std::vector<std::vector<uint32_t>> copy(n);
			std::vector<uint32_t> local(g.count, 0);
			for (UINT v = 0; v < g.count; v++)
			{
				const int s = in.vertex_instance[v];
				if (s < 0) continue;
				local[v] = static_cast<uint32_t>(copy[s].size());
				copy[s].push_back(v);
			}

			// Each instance's triangles, by local index, sorted within the
			// triangle (winding-blind) and then as a list.
			using Tri = std::array<uint32_t, 3>;
			std::vector<std::vector<Tri>> tris(n);
			int t = -1;
			for (size_t i = 0; i + 2 < g.tris.size(); i += 3)
			{
				const uint32_t a = g.tris[i], b = g.tris[i + 1], c = g.tris[i + 2];
				const int sa = in.vertex_instance[a], sb = in.vertex_instance[b], sc = in.vertex_instance[c];
				if (sa < 0 && sb < 0 && sc < 0) continue;   // an unaddressable copy: collapsed by the expansion
				if (sa < 0 || sa != sb || sa != sc) { why = "a triangle spans instances"; return false; }
				if (t < 0) t = sa;
				if (sa == t)
				{
					ttris.push_back(local[a]);
					ttris.push_back(local[b]);
					ttris.push_back(local[c]);
				}
				Tri k = { local[a], local[b], local[c] };
				std::sort(k.begin(), k.end());
				tris[sa].push_back(k);
			}
			if (t < 0) { why = "no triangles"; return false; }
			for (auto& l : tris) std::sort(l.begin(), l.end());

			// The window can hold copies the draw's indices don't reach (the
			// engine passes the whole buffer's vertex count); those instances
			// are not drawn.
			const auto& ref = copy[t];
			const UINT lo = static_cast<UINT>(in.tc1_off), hi = lo + static_cast<UINT>(in.tc1_size);
			xforms.clear();
			for (size_t s = 0; s < n; s++)
			{
				if (tris[s].empty()) continue;
				remixapi_Transform xf;
				std::memcpy(&xf, &in.xforms[s], sizeof(xf));
				xforms.push_back(xf);
				if (static_cast<int>(s) == t) continue;
				if (copy[s].size() != ref.size()) { why = "instances differ (vertex count)"; return false; }
				if (tris[s] != tris[t])
				{
					static bool s_logged = false;
					if (!std::exchange(s_logged, true))
						shared::common::log("RemixApi", std::format("foliage: first batch whose copies differ: {} vertices, "
							"{} copies, template copy {} ({} vertices, {} triangles) vs copy {} ({} vertices, {} triangles)",
							g.count, n, t, ref.size(), tris[t].size(), s, copy[s].size(), tris[s].size()),
							shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
					why = "instances differ (triangles)";
					return false;
				}
				for (size_t k = 0; k < ref.size(); k++)
				{
					const uint8_t* x = g.verts.data() + static_cast<size_t>(ref[k]) * g.stride;
					const uint8_t* y = g.verts.data() + static_cast<size_t>(copy[s][k]) * g.stride;
					if (std::memcmp(x, y, lo) != 0 || std::memcmp(x + hi, y + hi, g.stride - hi) != 0)
					{
						why = "instances differ (vertices)";
						return false;
					}
				}
			}

			tverts.resize(ref.size());
			for (size_t k = 0; k < ref.size(); k++)
			{
				if (!convert(g.verts.data() + static_cast<size_t>(ref[k]) * g.stride, tverts[k]))
				{
					why = "non-finite POSITION";
					return false;
				}
			}
			return true;
		}

		// The API mesh of the draw's expanded window. The indices come from
		// the game's index buffer (read_geometry), the vertices from the
		// expansion (positions replayed, everything else as the VB has it).
		// `prev`: the batch's current mesh. When it is the same instanced
		// model, it takes the new instances instead and `reused` is set (m
		// then holds no mesh).
		bool build_mesh(IDirect3DDevice9* dev, const shared::common::VsDesc& vs, const PsSlotMap* psmap,
			const DrawDesc& d, const vs_expand::ApiWindow& win, const Material& mat, Mesh& m, Mesh* prev, bool& reused,
			const char*& reject)
		{
			reused = false;
			PROFILE_ZONE_N("api::foliage::build_mesh");
			std::vector<uint8_t> xv;
			vs_expand::ApiWindow again;
			const INT base = d.base;
			const UINT skip = d.indexed ? d.min_vtx : 0u;
			if (vs_expand::describe_for_api(vs, psmap, base, skip, d.num_vtx, again, &xv, reject) != vs_expand::Bind::Bound)
				return false;
			if (again.key != win.key) { reject = "expansion changed between reads"; return false; }

			Geometry g;
			if (!read_geometry(dev, d, g, reject)) return false;
			if (g.count != d.num_vtx || g.stride != win.stride ||
				xv.size() != static_cast<size_t>(g.count) * g.stride)
			{
				reject = "vertex window mismatch";
				return false;
			}

			const Attr nrm = find_attr(D3DDECLUSAGE_NORMAL, 0);
			const Attr uv = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			const Attr col = find_attr(D3DDECLUSAGE_COLOR, 0);
			const Attr pos{ win.pos_off, win.pos_type };
			if (!attr_fits(pos, g.stride) || !attr_fits(uv, g.stride)) { reject = "POSITION / TEXCOORD0 unreadable"; return false; }
			const bool has_nrm = attr_fits(nrm, g.stride);
			m.vertex_color = attr_fits(col, g.stride) && !mat.no_vertex_color;

			auto convert = [&](const uint8_t* p, remixapi_HardcodedVertex& o) {
				o = {};
				for (int k = 0; k < 3; k++) o.position[k] = read_comp(p + pos.off, pos.type, k);
				if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2]))
					return false;
				float n[3] = { 0.0f, 0.0f, 1.0f };
				if (has_nrm)
					for (int k = 0; k < 3; k++) n[k] = read_comp(p + nrm.off, nrm.type, k);
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (std::isfinite(len) && len > 1e-6f) { o.normal[0] = n[0] / len; o.normal[1] = n[1] / len; o.normal[2] = n[2] / len; }
				else { o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f; }
				o.texcoord[0] = read_comp(p + uv.off, uv.type, 0);
				o.texcoord[1] = read_comp(p + uv.off, uv.type, 1);
				if (m.vertex_color)
				{
					if (col.type == D3DDECLTYPE_D3DCOLOR) std::memcpy(&o.color, p + col.off, sizeof(o.color));
					else o.color = D3DCOLOR_ARGB(unorm8(read_comp(p + col.off, col.type, 3)),
						unorm8(read_comp(p + col.off, col.type, 0)), unorm8(read_comp(p + col.off, col.type, 1)),
						unorm8(read_comp(p + col.off, col.type, 2)));
				}
				else o.color = 0xFFFFFFFFu;
				return true;
			};

			// The expanded copy: the batch's bound either way, and its mesh
			// when the batch is baked.
			std::vector<remixapi_HardcodedVertex> verts(g.count);
			for (UINT i = 0; i < g.count; i++)
			{
				if (!convert(xv.data() + static_cast<size_t>(i) * g.stride, verts[i]))
				{
					reject = "non-finite POSITION";
					return false;
				}
			}
			set_bound(m.e, verts);   // copied to each placement (residency)

			const bool instanced_family = vs.family == VsFamily::Grass || vs.family == VsFamily::DistLod ||
				vs.family == VsFamily::DistTree;
			if (instanced_family)
			{
				const char* why = nullptr;
				std::vector<remixapi_HardcodedVertex> tverts;
				std::vector<uint32_t> ttris;
				if (!instancing_forwarded()) why = "bridge";
				else if (make_template(vs, psmap, d, g, convert, tverts, ttris, m.instances, why))
				{
					m.model_key = asset_keys::of(tverts, ttris);
					if (prev && prev->e.mesh && prev->model_key == m.model_key && prev->material_key == m.material_key &&
						!prev->instances.empty())
					{
						// The same model at a new instance set: the batch's mesh
						// and its retained instance stay; only the transforms
						// change.
						prev->instances = std::move(m.instances);
						copy_bound(prev->e, m.e);
						for (auto& p : prev->placements)
						{
							copy_bound(p->e, prev->e);
							p->e.retained_refresh = true;
						}
						reused = true;
						g_count.instance_updates++;
						return true;
					}
					m.e.mesh = create_mesh(next_object_hash(m.key, kDomain), tverts, ttris, mat.handle, m.model_key);
					if (!m.e.mesh) { reject = "CreateMesh failed"; m.instances.clear(); return false; }
					m.verts = static_cast<UINT>(tverts.size());
					m.tris = static_cast<UINT>(ttris.size() / 3);
					g_count.instanced++;
					return true;
				}
				m.instances.clear();
				g_count.baked++;
				g_count.baked_why = why;
			}

			m.e.mesh = create_mesh(next_object_hash(m.key, kDomain), verts, g.tris, mat.handle,
				asset_keys::of(verts, g.tris));
			if (!m.e.mesh) { reject = "CreateMesh failed"; return false; }
			m.verts = g.count;
			m.tris = static_cast<UINT>(g.tris.size() / 3);
			return true;
		}

		// Churn: a batch (grass) or slot (leaves) that keeps needing new
		// meshes. True = over the limit: it builds nothing new for a while
		// and keeps drawing its current copy.
		template <typename T>
		bool churned(T& s, UINT now)
		{
			if (s.builds && now - s.window_start <= kChurnWindow) s.builds++;
			else { s.builds = 1; s.window_start = now; }
			if (s.builds <= kChurnBuilds) return false;
			s.hold_until = now + kChurnHold;
			s.builds = 0;
			g_count.churn_holds++;
			return true;
		}

		void destroy_materials()
		{
			for (auto& [key, m] : g_materials)
			{
				if (m.handle) { if (auto* b = bridge()) b->DestroyMaterial(m.handle); }
				for (const uint64_t t : m.textures) textures::release(t);
			}
			g_materials.clear();
		}
	}

	// ---- Entry points ----------------------------------------------------------------

	Foliage try_foliage(IDirect3DDevice9* dev, const shared::common::VsDesc& vs, const PsSlotMap* psmap,
		const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_foliage");
		// Grass batches are keyed by their NiGeometry; SpeedTree leaves and the
		// instanced distant objects / trees by their draw slot (the "leaf" path).
		const bool leaf = vs.family == VsFamily::Leaf || vs.family == VsFamily::DistLod ||
			vs.family == VsFamily::DistTree;
		if (vs.family != VsFamily::Grass && !leaf) return Foliage::Declined;

		auto& st = foliage_stats();
		auto* b = bridge();
		if (!b) return fallback(Reason::Inactive, st);

		// Retained placements need the retained path, a camera global that
		// agrees with this frame's camera, and the engine monitor's word that
		// the last cut (a load, a door, fast travel) settled: the absolute
		// transform bakes the camera in. Otherwise this frame's draws go out as
		// a DrawInstance each at their WORLD (transient), and kept placements
		// are hidden this frame unless drawn.
		bool transient = !retained::available() || !stable_origin::camera_matches_now();
		if (!transient)
		{
			const double* cam = stable_origin::camera();
			const bool zero = cam[0] == 0.0 && cam[1] == 0.0 && cam[2] == 0.0;
			if (zero || !nvse_link::facts().settled)
			{
				if (!g_logged_settling)
				{
					g_logged_settling = true;
					shared::common::log("RemixApi", std::format(
						"foliage: waiting for the cut to settle (engine monitor: loads done, references loaded{}) -- grass "
						"and leaves draw per frame meanwhile", zero ? "; camera reads (0, 0, 0)" : ""),
						shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				}
				transient = true;
			}
		}
		if (transient) block_frame();
		if (game::uses_viewmodel_projection()) return fallback(Reason::Viewmodel, st);

		const float* w = world_matrix();
		if (!w) return fallback(Reason::Inactive, st);
		retained::note_world_view();
		const remixapi_Transform abs = retained::absolute(w);
		const UINT now = frame();

		// ---- Identity: the NiGeometry the dispatcher is rendering ----
		uintptr_t geo = 0;
		if (sg::hooks_installed())
		{
			const uintptr_t g = sg::current_geometry();
			if (g && !sg::death_pending(g)) geo = g;
		}
		uint64_t part_key = 0;
		Part* part = nullptr;
		if (!leaf)
		{
			// Grass: the batch's own NiGeometry, or nothing. A VB window is
			// shared by every batch of the type, so it is no identity.
			if (!geo || !sg::is_grass_geometry(geo))
			{
				g_count.grass_unlinked++;
				if (!g_logged_unlinked)
				{
					g_logged_unlinked = true;
					shared::common::log("RemixApi", std::format(
						"foliage: a grass draw came without a grass batch NiGeometry from the render-pass dispatcher "
						"({}) -- such draws are not drawn (unlinked= on the status line); no identity is guessed",
						!sg::hooks_installed() ? "the scene-graph hooks are not installed"
							: geo ? std::format("pass geometry 0x{:08X} is not TallGrassTriStrips / TriShape", geo)
							: std::string("no pass in flight")),
						shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
				return fallback(Reason::Unlinked, st);
			}
			g_count.grass_linked++;
			// The part: the batch and where its draw starts. Not the shaders:
			// a VS / PS variant switch (lighting) is a new generation of the
			// same batch (the expansion key and the material are in the mesh
			// key), so the old copy is replaced, never left beside the new.
			Hash64 ph;
			ph.mix(geo);
			ph.mix(d.indexed ? d.start_index : static_cast<UINT>(d.base));
			part_key = ph.get();
			part = &g_parts[part_key];
			part->geo = geo;
			part->last_frame = now;
		}

		// ---- The expansion ----
		vs_expand::ApiWindow win;
		const char* reject = nullptr;
		const auto bound = vs_expand::describe_for_api(vs, psmap, d.base, d.indexed ? d.min_vtx : 0u, d.num_vtx,
			win, nullptr, reject);
		if (bound == vs_expand::Bind::Declined)
		{
			st.last_reject = reject ? reject : "expand";
			g_count.fallback[static_cast<size_t>(Reason::Expand)]++;
			st.fallbacks++;
			if (part) g_count.fallback_hidden += hide_part(*part);
			return Foliage::Declined;
		}
		if (bound == vs_expand::Bind::AllCulled)
		{
			// The batch is fully faded out: the game's image has none of it,
			// so neither may the scene (it comes back with the next draw that
			// shows any of it).
			if (part) g_count.culled_hidden += hide_part(*part);
			else if (hide_slot_at(win.slot, abs, geo)) g_count.culled_hidden++;
			g_count.culled++;
			vs_expand::count_api_draw(vs, true);
			return Foliage::Culled;
		}
		auto refuse = [&](Reason r) {
			// This draw is not drawn: what this batch / slot drew last at this
			// placement must not stay in the scene either.
			if (part) g_count.fallback_hidden += hide_part(*part);
			else if (hide_slot_at(win.slot, abs, geo)) g_count.fallback_hidden++;
			return fallback(r, st);
		};
		if (win.dynamic) return refuse(Reason::Dynamic);

		// ---- Buffers ----
		const auto& ffp = shared::common::ffp_state::get();
		IDirect3DVertexBuffer9* vb = win.src;
		IDirect3DIndexBuffer9* ib = nullptr;
		if (d.indexed && (FAILED(dev->GetIndices(&ib)) || !ib)) return refuse(Reason::Layout);
		const bool tracked = identity::track_buffers(vb, ib);
		const identity::BufferInfo vbi = identity::buffer(vb);
		const identity::BufferInfo ibi = identity::buffer(ib);
		if (ib) ib->Release();   // only compared by address from here on
		if (!tracked) return refuse(Reason::Inactive);
		if (vbi.rewritten || ibi.rewritten) return refuse(Reason::Rewritten);

		Slot* slot = nullptr;
		if (leaf)
		{
			slot = &g_slots[win.slot];
			slot->last_frame = now;
		}

		// ---- The draw: mesh m at this draw's placement ----
		auto claim = [&](Mesh& m) -> Foliage {
			m.e.last_frame = now;
			remixapi_InstanceInfoBlendEXT blend;
			const void* pnext = nullptr;
			if (m.blend)
			{
				blend = material::alpha_blend(m.alpha_func, m.alpha_ref, m.vertex_color);
				pnext = &blend;
			}
			else if (m.vertex_color)
			{
				blend = material::vertex_color_blend();
				pnext = &blend;
			}
			remixapi_InstanceInfoGpuInstancingEXT instancing = {};
			if (!m.instances.empty())
			{
				instancing.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
				instancing.pNext = nullptr;
				instancing.instanceTransforms_values = m.instances.data();
				instancing.instanceTransforms_count = static_cast<uint32_t>(m.instances.size());
				if (pnext) blend.pNext = &instancing;
				else pnext = &instancing;
			}
			// This frame's DrawInstance of the mesh at the draw's WORLD.
			auto draw_now = [&]() {
				remixapi_InstanceInfo ii = {};
				ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
				ii.pNext = const_cast<void*>(pnext);
				ii.categoryFlags = m.categories;
				ii.mesh = m.e.mesh;
				ii.transform = frame_transform(w);
				ii.doubleSided = 1;   // the cards are seen from both sides
				return b->DrawInstance(&ii) == REMIXAPI_ERROR_CODE_SUCCESS;
			};
			auto taken = [&]() {
				(leaf ? g_count.suppressed_leaf : g_count.suppressed_grass)++;
				vs_expand::count_api_draw(vs, false);
				return Foliage::Taken;
			};
			if (transient)
			{
				if (!draw_now()) return refuse(Reason::Submit);
				g_count.transient++;
				st.draws++;
				return taken();
			}

			// Grass: the batch's one instance (its WORLD is the grass root's).
			// Leaves: per leaf NiTriShape (per tree), or per WORLD unlinked.
			Placement* p = leaf ? find_placement(m, abs, geo) : (m.placements.empty() ? nullptr : m.placements.front().get());
			if (p && p->claimed_frame == now)
			{
				// Another draw of a placement already claimed this frame (a
				// second pass): already drawn.
				g_count.folded++;
				st.skipped++;
				return taken();
			}
			if (!p)
			{
				auto np = std::make_unique<Placement>();
				np->e.mesh = m.e.mesh;
				np->e.residency = leaf ? Residency::Foliage : Residency::FoliageBatch;
				copy_bound(np->e, m.e);
				np->owner = &m;
				np->abs = abs;
				np->geo = geo;
				p = np.get();
				m.placements.push_back(std::move(np));
				g_live_placements++;
				g_count.placed++;
			}
			else if (!retained::same_placement(abs, p->abs))
			{
				// Same placement at a new WORLD (a linked leaf shape whose tree
				// node moved, or the grass root): the retained instance moves.
				if (leaf) g_count.leaf_moved++;
				p->abs = abs;
			}

			// Live = visible in the table the replay at this frame's start drew.
			const bool was_visible = p->e.retained && p->e.retained_visible;
			const bool live = was_visible && p->shown_frame != now;
			if (!retained::draw_absolute(p->e, abs, m.categories, true, st, pnext)) return refuse(Reason::Submit);
			if (!was_visible) p->shown_frame = now;
			p->claimed_frame = now;
			p->last_frame = now;
			p->e.last_frame = now;
			// The instance set this claim shows (a change reported after it
			// hides the batch at the end of the frame).
			if (part) part->engine_gen = sg::grass_generation(geo);
			st.draws++;
			if (!live)
			{
				// Created or shown this frame: the next frame's replay has it,
				// this frame's DrawInstance draws it now.
				g_count.first++;
				if (!draw_now()) return refuse(Reason::Submit);
			}
			return taken();
		};
		// The batch / slot is held (its copy keeps changing) or can't build
		// now: the copy it drew last stands in for this draw.
		auto stale = [&](Reason r) -> Foliage {
			const uint64_t k = part ? part->mesh_key : slot->mesh_key;
			const auto it = k ? g_meshes.find(k) : g_meshes.end();
			if (it == g_meshes.end() || !it->second.e.mesh) return refuse(r);
			g_count.stale++;
			return claim(it->second);
		};
		if (now < (part ? part->hold_until : slot->hold_until)) return stale(Reason::Churn);

		// ---- Material ----
		if (!psmap || !psmap->any_classified || !psmap->has(PsSlotRole::Diffuse) || psmap->has_lod_sampler ||
			psmap->has_water_sampler || psmap->is_water_surface)
			return refuse(Reason::Shader);
		const auto& rc = rtxconf::scalars();
		const float* pc = ffp.ps_const_data();

		material::Desc md;
		md.diffuse = ffp.cur_texture(psmap->slot(PsSlotRole::Diffuse));
		if (!md.diffuse) return refuse(Reason::Texture);
		if (rc.protocol_enabled)
		{
			md.normal_role = psmap->has(PsSlotRole::Normal);
			if (md.normal_role)
			{
				// As ps_protocol_payload: the normal alpha is a spec mask (gloss)
				// only for the lit shaders with Toggles and a single BaseMap, and
				// only when the runtime reads it so (normalAlphaMode 1).
				md.gloss_alpha = psmap->toggles_reg != PsSlotMap::kNoReg && psmap->base_map_count <= 1 &&
					rc.normal_alpha_mode == 1;
				md.normal = ffp.cur_texture(psmap->slot(PsSlotRole::Normal));
				if (md.gloss_alpha)
				{
					const float gl = pc[psmap->toggles_reg * 4 + 2];
					if (std::isfinite(gl) && gl > 0.0f) md.gloss = (std::min)(gl, 4096.0f);
				}
			}
			if (psmap->has(PsSlotRole::Glow))
			{
				md.glow = ffp.cur_texture(psmap->slot(PsSlotRole::Glow));
				if (md.glow && psmap->emittance_reg != PsSlotMap::kNoReg)
				{
					const float* e = pc + psmap->emittance_reg * 4;
					if (unorm8(e[0]) < 254 || unorm8(e[1]) < 254 || unorm8(e[2]) < 254)
					{
						// A coloured tint rides on the material only while the
						// runtime tints API emissive textures (glow_tint_ready);
						// otherwise no glow rather than a white one.
						if (!material::glow_tint_ready()) md.glow = nullptr;
						else for (int k = 0; k < 3; k++) md.glow_tint[k] = static_cast<float>(unorm8(e[k])) / 255.0f;
					}
				}
			}
			// The expander route applies the protocol without a pack height map.
			md.height = psmap->has(PsSlotRole::Height) ? ffp.cur_texture(psmap->slot(PsSlotRole::Height)) : nullptr;
		}

		// Cutout. Grass: GREATER at the PS's own AlphaTestRef (its PS emits a
		// binary alpha); otherwise the draw's alpha test.
		if (!leaf && psmap->alpha_test_ref_reg != PsSlotMap::kNoReg)
		{
			const float ref = pc[psmap->alpha_test_ref_reg * 4];
			const float clamped = ref < 0.0f ? 0.0f : (ref > 1.0f ? 1.0f : ref);
			md.alpha_func = D3DCMP_GREATER;
			md.alpha_ref = static_cast<uint8_t>(clamped * 255.0f + 0.5f);
		}
		else if (render_state(dev, D3DRS_ALPHATESTENABLE, FALSE))
		{
			const DWORD func = render_state(dev, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
			if (func == D3DCMP_GREATER || func == D3DCMP_GREATEREQUAL)
			{
				md.alpha_func = static_cast<uint8_t>(func);
				md.alpha_ref = static_cast<uint8_t>(render_state(dev, D3DRS_ALPHAREF, 0) & 0xFF);
			}
			else if (func != D3DCMP_ALWAYS) return refuse(Reason::AlphaFunc);
		}
		if (render_state(dev, D3DRS_ALPHABLENDENABLE, FALSE))
		{
			// Plain alpha blending only: carried on the instance, as scene
			// statics carry a blended draw's state.
			if (render_state(dev, D3DRS_SRCBLEND, D3DBLEND_ONE) != D3DBLEND_SRCALPHA ||
				render_state(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO) != D3DBLEND_INVSRCALPHA)
				return refuse(Reason::Blend);
			md.blend = true;
		}
		// The albedo's own sampler stage (not stage 0): addressing and filter;
		// the geometry's own Texture Clamp Mode overrides the addressing
		// (exact shade property class; else the stage's stays).
		material::sampler_from_stage(dev, psmap->slot(PsSlotRole::Diffuse), md);
		material::clamp_from_geometry(geo, md);
		md.path = "foliage";
		const uint64_t mkey = material::key(md);

		auto mit = g_materials.find(mkey);
		if (mit != g_materials.end() && mit->second.declined && mit->second.refs == 0 && now >= mit->second.retry_frame)
		{
			g_materials.erase(mit);
			mit = g_materials.end();
		}
		if (mit == g_materials.end())
		{
			if (!take_build_budget(mkey))
			{
				if (!leaf) g_count.budget_holds++;
				return stale(Reason::Budget);
			}
			Material m;
			build_material(md, mkey, m);
			if (m.declined)
			{
				for (const uint64_t t : m.textures) textures::release(t);
				m.textures.clear();
				if (m.handle) { b->DestroyMaterial(m.handle); m.handle = nullptr; }
				if (m.why == Reason::Texture || m.why == Reason::Build) m.retry_frame = now + kRetryFrames;
			}
			m.last_used = now;
			mit = g_materials.emplace(mkey, std::move(m)).first;
		}
		Material& mat = mit->second;
		mat.last_used = now;
		if (mat.declined)
			return (mat.why == Reason::Texture || mat.why == Reason::Build) ? stale(mat.why) : refuse(mat.why);

		// ---- Mesh ----
		// Grass: the batch part + the expansion key (its current instance
		// set); leaves: the expansion key (slot + contents + constants).
		Hash64 kh;
		if (part) kh.mix(part_key);
		kh.mix(win.key);
		kh.mix(vbi.gen);
		kh.mix(ibi.gen);
		kh.mix(d.indexed ? d.start_index : 0u);
		kh.mix(static_cast<uint64_t>(d.type) | (static_cast<uint64_t>(d.prim_count) << 8) | (d.indexed ? 1ull << 40 : 0ull));
		kh.mix(mkey);
		const uint64_t key = kh.get();

		auto mesh_it = g_meshes.find(key);
		if (mesh_it == g_meshes.end())
		{
			const auto rt = g_retry_at.find(key);
			if (rt != g_retry_at.end() && now < rt->second) return stale(Reason::Retry);
			if (part ? churned(*part, now) : churned(*slot, now)) return stale(Reason::Churn);
			if (part && g_count.grass_builds >= kGrassBuildsPerFrame)
			{
				g_count.budget_holds++;
				return stale(Reason::Budget);
			}
			if (!take_build_budget(key))
			{
				if (part) g_count.budget_holds++;
				return stale(Reason::Budget);
			}

			Mesh m;
			m.key = key;
			m.slot = win.slot;
			m.part = part_key;
			m.material_key = mkey;
			m.leaf = leaf;
			m.categories = mat.categories;
			m.blend = md.blend && !mat.alpha_dropped;
			m.alpha_func = md.alpha_func;
			m.alpha_ref = md.alpha_ref;
			Mesh* prev = nullptr;
			if (part && part->mesh_key)
			{
				const auto pit = g_meshes.find(part->mesh_key);
				if (pit != g_meshes.end()) prev = &pit->second;
			}
			bool reused = false;
			if (!build_mesh(dev, vs, psmap, d, win, mat, m, prev, reused, reject))
			{
				destroy_entry(m.e);
				g_retry_at[key] = now + kRetryFrames;
				note_failure(st, reject, std::format("{} VS 0x{:08X} PS 0x{:08X}, {} verts, {} prims",
					leaf ? "leaves" : "grass", vs.vs_hash, psmap->ps_hash, d.num_vtx, d.prim_count));
				return stale(Reason::Build);
			}
			identity::note_built(vb);
			identity::note_built(ib);   // compared by address only
			if (reused)
			{
				// The batch's mesh, filed under this instance set's key.
				auto node = g_meshes.extract(part->mesh_key);
				node.key() = key;
				node.mapped().key = key;
				mesh_it = g_meshes.insert(std::move(node)).position;
				part->mesh_key = key;
				part->generations++;
				g_count.grass_builds++;
				return claim(mesh_it->second);
			}
			mat.refs++;
			m.e.built_frame = now;
			mesh_it = g_meshes.emplace(key, std::move(m)).first;
			st.created++;
			g_count.built++;
			note_success(st, std::format("{} VS 0x{:08X} PS 0x{:08X}, {} verts, {} tris{}{}{}", leaf ? "leaves" : "grass",
				vs.vs_hash, psmap->ps_hash, mesh_it->second.verts, mesh_it->second.tris,
				md.alpha_func ? std::format(" cutout(func {} ref {})", md.alpha_func, md.alpha_ref) : std::string(),
				mesh_it->second.vertex_color ? " + vertex colour" : "", md.normal ? " + normal" : ""));
			if (part)
			{
				// The same batch at a new instance set: its old mesh and
				// instance go (only this batch's).
				g_count.grass_builds++;
				if (part->mesh_key && part->mesh_key != key && destroy_mesh_key(part->mesh_key))
				{
					g_count.rebuilds++;
					g_count.superseded++;
				}
				part->mesh_key = key;
				part->generations++;
			}
			// Leaves: the slot's previous mesh at this placement is replaced
			// by this one (instance constants or buffer contents changed).
			else if (hide_slot_at(win.slot, abs, geo, key)) g_count.superseded++;
			st.live = static_cast<uint32_t>(g_meshes.size());
		}
		Mesh& m = mesh_it->second;
		if (slot) slot->mesh_key = key;
		if (part) part->mesh_key = key;

		if (g_logged_ps.size() < 256 && g_logged_ps.insert(psmap->ps_hash ^ (leaf ? 0x80000000u : 0u)).second)
			shared::common::log("RemixApi", std::format("foliage: {} PS 0x{:08X} (VS 0x{:08X}) now drawn through the API",
				leaf ? "leaves" : "grass", psmap->ps_hash, vs.vs_hash), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		return claim(m);
	}

	bool foliage_frame_blocked()
	{
		return g_blocked_frame == frame();
	}

	void foliage_end_frame()
	{
		PROFILE_ZONE_N("api::foliage_end_frame");
		if (!sg::grass_hooks_installed()) return;
		sg::take_grass_events(g_scratch_changed, g_scratch_destroyed);
		if (g_parts.empty()) return;
		// Destroyed batches: their parts, meshes and instances go now.
		if (!g_scratch_destroyed.empty())
		{
			const std::unordered_set<uintptr_t> gone(g_scratch_destroyed.begin(), g_scratch_destroyed.end());
			g_count.batches_destroyed += drop_parts(gone);
		}
		// Changed batches: an instance claimed at an older instance set is
		// hidden (it may be off screen); the batch's next draw shows it
		// again, rebuilt when its constants moved.
		if (!g_scratch_changed.empty())
		{
			g_count.gen_changes += static_cast<uint32_t>(g_scratch_changed.size());
			const std::unordered_set<uintptr_t> changed(g_scratch_changed.begin(), g_scratch_changed.end());
			for (auto& [pk, part] : g_parts)
			{
				if (!part.mesh_key || !changed.count(part.geo)) continue;
				if (sg::grass_generation(part.geo) == part.engine_gen) continue;
				const auto m = g_meshes.find(part.mesh_key);
				if (m == g_meshes.end()) continue;
				for (auto& p : m->second.placements)
				{
					if (!p->e.retained || !p->e.retained_visible) continue;
					retained::hide(p->e);
					g_count.gen_hidden++;
				}
			}
		}
	}

	void foliage_on_deaths(const std::vector<uintptr_t>& dead)
	{
		if (dead.empty() || g_meshes.empty()) return;
		const std::unordered_set<uintptr_t> gone(dead.begin(), dead.end());
		// Grass: the batch's NiGeometry died (its address may be a new
		// object's from now on).
		if (!g_parts.empty()) g_count.batch_deaths += drop_parts(gone);
		// Leaves: placements drawn for a dead leaf shape.
		std::vector<MeshEntry*> doomed;
		for (auto& [key, m] : g_meshes)
			for (auto& p : m.placements)
				if (p->geo && gone.count(p->geo) && p->e.retained) doomed.push_back(&p->e);
		retained::destroy_many(doomed);
		for (auto& [key, m] : g_meshes)
		{
			for (auto it = m.placements.begin(); it != m.placements.end();)
			{
				if ((*it)->geo && gone.count((*it)->geo))
				{
					it = m.placements.erase(it);
					if (g_live_placements) g_live_placements--;
					g_count.leaf_deaths++;
				}
				else ++it;
			}
		}
	}

	void foliage_on_present()
	{
		auto& st = foliage_stats();
		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.evict_frames);
		const UINT now = frame();

		// Hooks go in here, between frames. Grass needs the render-pass
		// dispatcher for its identity (whatever RetainedKeepCulled says) and
		// the grass batch hooks for its instance-set generations; leaves
		// link to their NiGeometry under RetainedKeepCulled.
		if (!sg::hooks_installed())
		{
			static bool s_asked = false;
			if (!s_asked)
			{
				s_asked = true;
				const bool ok = sg::install_hooks();
				shared::common::log("RemixApi", ok
					? "foliage: grass batches are identified by their NiGeometry and leaf placements follow theirs "
					  "(render-pass dispatcher, ~NiGeometry)"
					: "foliage: the scene-graph hooks are not available -- grass is not drawn (unlinked); leaf "
					  "placements follow the cell grid and the in-view rule only",
					ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
			}
		}
		if (!g_grass_hooks_asked)
		{
			g_grass_hooks_asked = true;
			const bool ok = sg::install_grass_hooks();
			if (!g_logged_grass_hooks)
			{
				g_logged_grass_hooks = true;
				shared::common::log("RemixApi", ok
					? "foliage: grass batch hooks in -- a batch whose instance set changes leaves the scene until its "
					  "next draw, a destroyed batch is dropped at once"
					: "foliage: the grass batch hooks are not available -- a batch changed while off screen keeps its "
					  "old copy until drawn; destroyed batches go with their NiGeometry",
					ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
			}
		}

		if (!g_meshes.empty())
		{
			const bool kept = retained::keep_in_effect(g_live_placements, st);
			auto stale = [&](const Placement& p) {
				return now - p.last_frame > retained::stale_after(p.e, keep, kept);
			};
			std::vector<MeshEntry*> doomed;
			for (auto& [key, m] : g_meshes)
				for (auto& p : m.placements)
					if (p->e.retained && stale(*p)) doomed.push_back(&p->e);
			retained::destroy_many(doomed);
			for (auto& [key, m] : g_meshes)
			{
				for (auto it = m.placements.begin(); it != m.placements.end();)
				{
					if (stale(**it))
					{
						it = m.placements.erase(it);
						if (g_live_placements) g_live_placements--;
						g_count.placements_evicted++;
					}
					else ++it;
				}
			}
			for (auto it = g_meshes.begin(); it != g_meshes.end();)
			{
				if (it->second.placements.empty() && now - it->second.e.last_frame > keep)
				{
					destroy_mesh(it->second);
					st.evicted++;
					it = g_meshes.erase(it);
				}
				else ++it;
			}
		}
		st.live = static_cast<uint32_t>(g_meshes.size());

		for (auto it = g_materials.begin(); it != g_materials.end();)
		{
			Material& m = it->second;
			if (m.refs == 0 && now - m.last_used > keep)
			{
				if (m.handle) { if (auto* b = bridge()) b->DestroyMaterial(m.handle); }
				for (const uint64_t t : m.textures) textures::release(t);
				it = g_materials.erase(it);
			}
			else ++it;
		}
		for (auto it = g_slots.begin(); it != g_slots.end();)
		{
			if (now - it->second.last_frame > keep && now >= it->second.hold_until) it = g_slots.erase(it);
			else ++it;
		}
		for (auto it = g_parts.begin(); it != g_parts.end();)
		{
			// A part whose mesh was evicted and that hasn't been drawn for
			// EvictFrames (its geometry may be long gone).
			const bool has_mesh = it->second.mesh_key && g_meshes.count(it->second.mesh_key);
			if (!has_mesh && now - it->second.last_frame > keep && now >= it->second.hold_until) it = g_parts.erase(it);
			else ++it;
		}
		for (auto it = g_retry_at.begin(); it != g_retry_at.end();)
		{
			if (now >= it->second) it = g_retry_at.erase(it);
			else ++it;
		}

		// Roll the per-frame counters.
		auto& c = g_count;
		auto& l = g_last;
		l.suppressed_grass = std::exchange(c.suppressed_grass, 0u);
		l.suppressed_leaf = std::exchange(c.suppressed_leaf, 0u);
		l.first = std::exchange(c.first, 0u);
		l.transient = std::exchange(c.transient, 0u);
		l.stale = std::exchange(c.stale, 0u);
		l.folded = std::exchange(c.folded, 0u);
		l.culled = std::exchange(c.culled, 0u);
		l.placed = std::exchange(c.placed, 0u);
		l.built = std::exchange(c.built, 0u);
		l.materials_built = std::exchange(c.materials_built, 0u);
		l.grass_linked = std::exchange(c.grass_linked, 0u);
		l.grass_unlinked = std::exchange(c.grass_unlinked, 0u);
		l.grass_builds = std::exchange(c.grass_builds, 0u);
		std::memcpy(l.fallback, c.fallback, sizeof(c.fallback));
		std::memset(c.fallback, 0, sizeof(c.fallback));
	}

	void foliage_on_reset()
	{
		// API objects live in the runtime and survive a device reset.
		g_retry_at.clear();
	}

	void foliage_drop_family(bool leaf)
	{
		auto& st = foliage_stats();
		std::vector<MeshEntry*> doomed;
		for (auto& [key, m] : g_meshes)
			if (m.leaf == leaf)
				for (auto& p : m.placements)
					if (p->e.retained) doomed.push_back(&p->e);
		retained::destroy_many(doomed);
		for (auto it = g_meshes.begin(); it != g_meshes.end();)
		{
			if (it->second.leaf != leaf) { ++it; continue; }
			destroy_mesh(it->second);
			st.evicted++;
			it = g_meshes.erase(it);
		}
		if (leaf) g_slots.clear();
		else g_parts.clear();
		st.live = static_cast<uint32_t>(g_meshes.size());
	}

	void foliage_drop_all()
	{
		foliage_drop_family(false);
		foliage_drop_family(true);
		destroy_materials();
		g_retry_at.clear();
		g_live_placements = 0;
	}

	std::string foliage_status()
	{
		const auto& l = g_last;
		const auto& s = foliage_stats();
		std::string reasons;
		uint32_t total = 0;
		for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
		{
			if (!l.fallback[i]) continue;
			total += l.fallback[i];
			reasons += std::format("{}{}={}", reasons.empty() ? "" : " ", kReasonNames[i], l.fallback[i]);
		}
		uint32_t grass_meshes = 0, leaf_meshes = 0, grass_inst = 0, leaf_inst = 0, visible = 0, linked = 0;
		for (const auto& [key, m] : g_meshes)
		{
			(m.leaf ? leaf_meshes : grass_meshes)++;
			(m.leaf ? leaf_inst : grass_inst) += static_cast<uint32_t>(m.placements.size());
			for (const auto& p : m.placements)
			{
				visible += (p->e.retained && p->e.retained_visible) ? 1u : 0u;
				linked += p->e.engine_geo ? 1u : 0u;
			}
		}
		const UINT now = frame();
		uint32_t held = 0;
		for (const auto& [k, sl] : g_slots) held += now < sl.hold_until ? 1u : 0u;
		uint64_t gens = 0;
		uint32_t batches_held = 0, multi_gen = 0;
		for (const auto& [k, p] : g_parts)
		{
			gens += p.generations;
			batches_held += now < p.hold_until ? 1u : 0u;
			multi_gen += p.generations > 1 ? 1u : 0u;
		}
		const auto hs = sg::grass_hook_stats();
		auto& c = g_count;
		const uint32_t baked = std::exchange(c.baked, 0u);
		return std::format(
			"foliage: meshes={} (grass {} leaves {}) instances={} (grass {} leaves {}) visible={} "
			"linked={} materials={} | last frame: taken={} (grass {} leaves {}) firstFrame={} transient={} stale={} "
			"folded={} culled={} built={}+{}mat (grass {}) placed={} grassDraws linked={} unlinked={} refused={}{} | "
			"grass batches={} "
			"generations={} (batches past their first {}) held={} | since last line: rebuilds={} budgetHolds={} "
			"genChanges={} genHidden={} destroyed={} deaths={} culledHidden={} superseded={} refusedHidden={} "
			"leafMoved={} leafDeaths={} instanced={} (updated in place {}) baked={}{} | slotsHeld={} churnHolds={} evicted={} placementsEvicted={} "
			"failed={} retainedUpdates={} retainedFallback={} | hooks dispatcher={} batch={} (appends {} removes {} "
			"destroys {})",
			g_meshes.size(), grass_meshes, leaf_meshes, g_live_placements, grass_inst, leaf_inst, visible, linked,
			g_materials.size(),
			l.suppressed_grass + l.suppressed_leaf, l.suppressed_grass, l.suppressed_leaf, l.first, l.transient, l.stale,
			l.folded, l.culled,
			l.built, l.materials_built, l.grass_builds, l.placed, l.grass_linked, l.grass_unlinked, total,
			reasons.empty() ? std::string() : std::format(" ({})", reasons),
			g_parts.size(), gens, multi_gen, batches_held,
			std::exchange(c.rebuilds, 0u), std::exchange(c.budget_holds, 0u), std::exchange(c.gen_changes, 0u),
			std::exchange(c.gen_hidden, 0u), std::exchange(c.batches_destroyed, 0u), std::exchange(c.batch_deaths, 0u),
			std::exchange(c.culled_hidden, 0u), std::exchange(c.superseded, 0u), std::exchange(c.fallback_hidden, 0u),
			std::exchange(c.leaf_moved, 0u), std::exchange(c.leaf_deaths, 0u),
			std::exchange(c.instanced, 0u), std::exchange(c.instance_updates, 0u), baked,
			baked ? std::format(" (last: {})", c.baked_why ? c.baked_why : "?") : std::string(),
			held, c.churn_holds, s.evicted, c.placements_evicted, s.failed, s.retained_updates_last,
			s.retained_fallbacks_last,
			sg::hooks_installed() ? "on" : "off", sg::grass_hooks_installed() ? "on" : "off",
			hs.appends, hs.removes, hs.destroys);
	}
}
