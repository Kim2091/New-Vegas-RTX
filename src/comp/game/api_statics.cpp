#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "api_identity.hpp"
#include "api_rtxconf.hpp"
#include "api_material.hpp"
#include "api_camera.hpp"
#include "api_hair_bake.hpp"
#include "scene_graph.hpp"
#include "scene_statics.hpp"
#include "stable_origin.hpp"
#include "nvse_link.hpp"
#include "tex_pack.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/config.hpp"

/*
 * Draw capture: the Remix-API path of every traced world draw that no
 * dedicated path takes (docs/fnv-api-only.md).
 *
 * The renderer offers it every traced world draw after land, far LOD,
 * water, the expander families and the scene-graph statics' recording:
 * FFP_WORLD (what the scene graph refused, has not claimed yet or never
 * sees: actor rigid parts, effects, blended or dynamic objects, the
 * first-person weapon), FFP_BI (unskinned BLENDINDICES, ignored as the FFP
 * route ignores them), land-shaped placed objects, non-indexed world draws
 * and world draws without a NORMAL (their normals are computed here). What
 * it refuses is not drawn; the renderer counts it per reason. It refuses "pipboy" for the Pip-Boy's
 * own first-person draws while it is not closed and for every draw at the
 * main menu or while loading (api::pipboy_draw), and the renderer keeps
 * their D3D9 route; the world behind the Pip-Boy stays here.
 *
 * Per draw
 * --------
 *   * object   the dispatcher's NiGeometry and the draw's index range (a
 *              segmented LOD block is drawn as one range per run of shown
 *              cells), else the draw's buffers, window and WORLD
 *              translation. The first pass of an object in a frame is its
 *              traced instance; later passes of it (the engine's light /
 *              multiply passes, repeats) are dropped, as the runtime merges
 *              them into the first draw's instance on the D3D9 route.
 *   * lighting a lit shader (Toggles) blending additively (dst ONE) or as a
 *              multiply is a raster lighting pass: dropped, and the object
 *              stays unclaimed for its base pass.
 *   * depth    colour writes off: a depth / stencil pass, dropped.
 *   * mesh     built once from the draw's vertex window (bridge CPU shadow,
 *              READONLY locks), keyed by geo_key (buffers, window, layout,
 *              sampled contents), the hash of every byte of the window
 *              (content_key) and the material. The full hash is taken only
 *              when a buffer's write generation moved since the slot's last
 *              one: a buffer the engine refills with the same bytes keeps its
 *              mesh. A slot (buffers + window + layout + material) whose
 *              bytes change gets a new mesh; the old one is destroyed
 *              once no retained placement draws it. Capture never marks a
 *              buffer built (identity::note_built): nothing here is kept
 *              across a write without its bytes being compared.
 *   * material api_material from the PS's slots and constants (albedo,
 *              normal + gloss, glow with grey tints as an intensity and
 *              coloured ones while the runtime tints API emissive textures,
 *              height from the PS or the texture pack), with the rtx.conf
 *              lists as instance categories. Alpha: a GREATER / GREATEREQUAL
 *              test is the material's cutout, with or without plain alpha
 *              blending (material::blend_test_as_cutout); plain alpha
 *              blending without a test rides on the instance as the FFP
 *              route's stage 0 gives it (material::alpha_blend); any other
 *              blend or test is the draw's own state
 *              (material::draw_state_blend, useDrawCallAlphaState).
 *   * instance a world draw of a NiGeometry whose mesh and placement stayed
 *              the same for kStableDraws drawn frames, owned by a placed
 *              non-actor reference (the engine monitor's owner map,
 *              scene_statics::capture_may_retain), with no draw-state alpha:
 *              a retained instance (retained::draw_absolute,
 *              Residency::Cell), kept while culled under the keep-culled TTL
 *              (RetainedKeepCulledFrames) like the scene statics. It leaves
 *              when its NiGeometry dies, the scene graph claims it, or its
 *              cell leaves the grid / the interior changes / a cut
 *              (residency); it is hidden while its owner check (or, for an
 *              unowned one, its parent chain) says app-culled. A change of
 *              mesh or placement sends it back to per frame (retained again
 *              once stable). Everything else (moving or changing objects,
 *              draws without a NiGeometry, actors' rigid parts, draw-state
 *              blends) is one DrawInstance per frame at the draw's WORLD
 *              (frame_transform), doubleSided from the cull mode; a
 *              first-person draw carries VIEW_MODEL and
 *              the frame's view-model camera ([RemixApi] ViewModelCamera,
 *              api_camera).
 * Builds take the build budget (take_build_budget: a fair queue, oldest
 * waiter first; meshes of up to kSmallMeshVerts vertices from their own
 * pool; first-person parts never wait); a draw over budget is not drawn that
 * frame and is built on a later one. A draw whose albedo is in a D3D9-only
 * list (rtx.lightConverter, ...) never gets here: the renderer keeps it on
 * D3D9 (rtxconf::d3d9_only).
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;
	using shared::common::PsSlotMap;
	using shared::common::PsSlotRole;

	namespace
	{
		constexpr uint64_t kCaptureDomain = 0x4341505455524531ull;   // "CAPTURE1"
		constexpr UINT kRetryFrames = 120;
		constexpr size_t kMaxRefusalLogs = 256;
		// Drawn frames at the same mesh and placement before a world draw
		// becomes a retained instance: a placement that pauses (a swinging
		// lamp at its turning point, clutter coming to rest) stays per frame.
		constexpr UINT kStableDraws = 60;
		// A retained placement not drawn this frame has its owner (or parent
		// chain) checked for APP_CULLED one frame in this many (staggered).
		constexpr UINT kAppCullEvery = 16;
		constexpr int kMaxChainHops = 64;

		enum class Reason : uint8_t
		{
			PipBoy, Inactive, Shader, VertexShader, Layout, Dynamic, Blend, RtxConf, Texture, Budget, Build, Retry,
			Submit, Count
		};
		constexpr const char* kReasonNames[] = {
			"pipboy", "inactive", "shader", "vs", "layout", "dynamic", "blend", "rtxconf", "texture", "budget", "build",
			"retry", "submit",
		};
		static_assert(std::size(kReasonNames) == static_cast<size_t>(Reason::Count));

		struct Counters
		{
			uint64_t sent = 0, viewmodel = 0, camera = 0, blended = 0, draw_state = 0;
			uint64_t extra = 0, lighting = 0, depth_only = 0;
			uint64_t decals = 0;    // world draws tagged DECAL_* by material::decal_of
			uint64_t meshes_built = 0, materials_built = 0, replaced = 0, computed_normals = 0;
			uint64_t refused[static_cast<size_t>(Reason::Count)] = {};
			// World draws (not first-person) that went out: through their
			// retained instance, or per frame: warming up (same mesh and
			// placement, not kStableDraws yet), changing (mesh or placement
			// moved since its last draw), no NiGeometry, draw-state alpha, not
			// owned by a placed non-actor reference, no trustworthy camera, the
			// retained path refused, a later index range of a geometry drawn
			// in several this frame.
			uint64_t retained = 0;
			uint64_t pf_warming = 0, pf_changing = 0, pf_no_geo = 0, pf_draw_state = 0, pf_not_placed = 0, pf_camera = 0,
				pf_failed = 0, pf_ranges = 0;
			// Retained placements: made retained, sent back to per frame (moved
			// / new mesh), handed to the scene graph, dead, hidden for an
			// app-culled chain, evicted (keep-culled TTL / EvictFrames).
			uint64_t promoted = 0, demoted_moved = 0, demoted_mesh = 0, to_scene = 0, dead = 0, app_culled = 0, evicted = 0;
			// Slots whose buffer generation moved: bytes hashed, and how many
			// of those held the same bytes (no new mesh).
			uint64_t content_checks = 0, content_same = 0;
		};
		Counters g_c;           // since the last status line
		UINT g_window_start = 0;
		bool g_logged_first = false;
		std::unordered_set<uint64_t> g_logged_refusals;   // (reason, PS) logged once

		struct Mat
		{
			remixapi_MaterialHandle handle = nullptr;
			std::vector<uint64_t> textures;
			uint32_t refs = 0;
			UINT last_used = 0;
			bool no_vertex_color = false;
			bool alpha_dropped = false;
			uint32_t categories = 0;
			bool declined = false;
			Reason why = Reason::Build;
			UINT retry_frame = 0;
			// For the [Addressing] mesh line.
			uint64_t albedo_d3d9 = 0;
			uint8_t wrap_u = 1, wrap_v = 1;
		};
		std::unordered_map<uint64_t, Mat> g_mats;

		struct Mesh
		{
			remixapi_MeshHandle mesh = nullptr;
			uint64_t material_key = 0;
			bool vertex_color = false;
			UINT last_frame = 0;
			uint32_t places = 0;              // retained placements drawing it: not destroyed while any
			bool has_bound = false;           // object-space bound (Residency::Cell)
			float bound_c[3] = {};
			float bound_r = 0.0f;
			material::MeshFacts facts;        // winding, vertex alpha, reversed copy ([DecalRoute])
		};
		std::unordered_map<uint64_t, Mesh> g_meshes;
		std::unordered_map<uint64_t, uint64_t> g_slots;   // slot -> the mesh key it holds now
		std::unordered_map<uint64_t, UINT> g_retry_at;

		// Slot -> the buffers' write generations its bytes were last hashed
		// at, and that hash (content_key).
		struct SlotBytes
		{
			bool known = false;
			uint32_t vb_gen = 0, ib_gen = 0;
			uint64_t content = 0;
		};
		std::unordered_map<uint64_t, SlotBytes> g_slot_bytes;

		// A world draw's NiGeometry: its last mesh and placement, how long
		// they held, and its retained instance once they held kStableDraws.
		struct Place
		{
			MeshEntry e;                      // e.mesh borrowed from g_meshes[retained_mesh]; never destroy_entry'd
			uint64_t mesh_key = 0;            // the mesh of its last draw
			uint64_t retained_mesh = 0;       // the mesh its retained instance draws (holds Mesh::places); 0 = none
			remixapi_Transform abs = {};      // absolute placement at the start of the streak
			UINT stable = 0;                  // drawn frames at mesh_key / abs (0: none yet)
		};
		std::unordered_map<uintptr_t, Place> g_places;

		// This frame's objects whose traced instance went out, and the
		// NiGeometries they belong to.
		std::unordered_set<uint64_t> g_objects;
		std::unordered_set<uintptr_t> g_object_geos;
		UINT g_objects_frame = 0xFFFFFFFFu;

		DWORD rs(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD fallback)
		{
			DWORD v = fallback;
			return SUCCEEDED(dev->GetRenderState(s, &v)) ? v : fallback;
		}

		uint8_t unorm8(float v)
		{
			if (!(v > 0.0f)) return 0;
			return v >= 1.0f ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f);
		}

		struct Layout
		{
			Attr pos, nrm, uv, col;
		};

		// Stream 0 only; POSITION float3 / float4 and TEXCOORD0 float2 / half2;
		// NORMAL float3 and COLOR when present (no NORMAL: computed). No
		// BLENDWEIGHT (skinned) or POSITIONT; an unskinned BLENDINDICES is
		// ignored, as the FFP route ignores it.
		bool capture_layout(Layout& l)
		{
			const auto& ffp = shared::common::ffp_state::get();
			if (!ffp.last_decl() || ffp.cur_decl_multi_stream() || ffp.cur_decl_is_skinned() || ffp.cur_decl_has_pos_t())
				return false;
			for (UINT i = 0; i < ffp.cur_decl_num_elements(); i++)
				if (ffp.cur_decl_elements()[i].Usage == D3DDECLUSAGE_BLENDWEIGHT) return false;
			l.pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			l.nrm = find_attr(D3DDECLUSAGE_NORMAL, 0);
			l.uv = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			l.col = find_attr(D3DDECLUSAGE_COLOR, 0);
			const UINT stride = ffp.stream_stride(0);
			if (!l.pos.ok() || (l.pos.type != D3DDECLTYPE_FLOAT3 && l.pos.type != D3DDECLTYPE_FLOAT4) ||
				!attr_fits(l.pos, stride))
				return false;
			if (!l.uv.ok() || (l.uv.type != D3DDECLTYPE_FLOAT2 && l.uv.type != D3DDECLTYPE_FLOAT16_2) ||
				!attr_fits(l.uv, stride))
				return false;
			if (l.nrm.ok() && (l.nrm.type != D3DDECLTYPE_FLOAT3 || !attr_fits(l.nrm, stride))) l.nrm = {};
			if (l.col.ok() && !attr_fits(l.col, stride)) l.col = {};
			return true;
		}

		// Keeps only the vertices the triangles use, in their order. A run of
		// a segmented LOD block is drawn with the whole block's vertex window.
		void compact_vertices(Geometry& g)
		{
			constexpr uint32_t kUnused = 0xFFFFFFFFu;
			std::vector<uint32_t> remap(g.count, kUnused);
			uint32_t used = 0;
			for (const uint32_t v : g.tris)
				if (remap[v] == kUnused) remap[v] = used++;
			if (used == g.count) return;
			std::vector<uint8_t> verts(static_cast<size_t>(used) * g.stride);
			for (uint32_t v = 0; v < g.count; v++)
				if (remap[v] != kUnused)
					std::memcpy(verts.data() + static_cast<size_t>(remap[v]) * g.stride,
						g.verts.data() + static_cast<size_t>(v) * g.stride, g.stride);
			for (uint32_t& v : g.tris) v = remap[v];
			g.verts = std::move(verts);
			g.count = used;
		}

		// reversed_copy: a two-sided decal (material::append_reversed).
		// hair: the hair PS's tint (material::hair_tint), null for any other PS.
		bool build_mesh(IDirect3DDevice9* dev, const DrawDesc& d, const Layout& L, uint64_t key, const Mat& mat,
			bool reversed_copy, const float (*hair)[3], Mesh& m, const char*& reject)
		{
			PROFILE_ZONE_N("api::capture::build_mesh");
			Geometry g;
			if (!read_geometry(dev, d, g, reject)) return false;
			compact_vertices(g);
			std::vector<remixapi_HardcodedVertex> verts(g.count);
			for (UINT i = 0; i < g.count; i++)
			{
				const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride;
				auto& o = verts[i];
				o = {};
				for (int k = 0; k < 3; k++) o.position[k] = read_comp(p + L.pos.off, L.pos.type, k);
				if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2]))
				{
					reject = "non-finite POSITION";
					return false;
				}
				if (L.nrm.ok())
				{
					float n[3];
					for (int k = 0; k < 3; k++) n[k] = read_comp(p + L.nrm.off, L.nrm.type, k);
					const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
					if (std::isfinite(len) && len > 1e-6f) { o.normal[0] = n[0] / len; o.normal[1] = n[1] / len; o.normal[2] = n[2] / len; }
					else { o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f; }
				}
				o.texcoord[0] = read_comp(p + L.uv.off, L.uv.type, 0);
				o.texcoord[1] = read_comp(p + L.uv.off, L.uv.type, 1);
				if (hair)
				{
					// The hair PS's factor (material::hair_color).
					const float green = L.col.type == D3DDECLTYPE_D3DCOLOR ? p[L.col.off + 1] / 255.0f
						: read_comp(p + L.col.off, L.col.type, 1);
					float scale[3];
					material::hair_scale(*hair, scale);
					o.color = material::hair_color(green, *hair, scale);
				}
				else if (m.vertex_color)
				{
					// D3DCOLOR's bytes (B, G, R, A) are the runtime's B8G8R8A8.
					if (L.col.type == D3DDECLTYPE_D3DCOLOR) std::memcpy(&o.color, p + L.col.off, sizeof(o.color));
					else o.color = D3DCOLOR_ARGB(unorm8(read_comp(p + L.col.off, L.col.type, 3)),
						unorm8(read_comp(p + L.col.off, L.col.type, 0)), unorm8(read_comp(p + L.col.off, L.col.type, 1)),
						unorm8(read_comp(p + L.col.off, L.col.type, 2)));
				}
				else o.color = 0xFFFFFFFFu;
			}
			if (!L.nrm.ok())
			{
				// No NORMAL: the face normals summed per vertex (area-weighted),
				// as the runtime synthesises geometric normals for a
				// vertex-captured draw.
				std::vector<float> acc(static_cast<size_t>(g.count) * 3, 0.0f);
				for (size_t t = 0; t + 2 < g.tris.size(); t += 3)
				{
					const float* a = verts[g.tris[t]].position;
					const float* b = verts[g.tris[t + 1]].position;
					const float* c = verts[g.tris[t + 2]].position;
					const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
					const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
					const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
					for (size_t v = 0; v < 3; v++)
						for (int k = 0; k < 3; k++) acc[static_cast<size_t>(g.tris[t + v]) * 3 + k] += n[k];
				}
				for (UINT i = 0; i < g.count; i++)
				{
					const float* n = acc.data() + static_cast<size_t>(i) * 3;
					const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
					auto& o = verts[i];
					if (std::isfinite(len) && len > 1e-12f) { o.normal[0] = n[0] / len; o.normal[1] = n[1] / len; o.normal[2] = n[2] / len; }
					else { o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f; }
				}
				g_c.computed_normals++;
			}
			// The asset key names the geometry before the reversed copy.
			const uint64_t asset_key = asset_keys::of(verts, g.tris);
			m.facts = material::mesh_facts(verts, g.tris, m.vertex_color);
			if (reversed_copy)
			{
				material::append_reversed(g.tris);
				m.facts.reversed_copy = true;
			}
			m.mesh = create_mesh(next_object_hash(key, kCaptureDomain), verts, g.tris, mat.handle, asset_key);
			if (!m.mesh) { reject = "CreateMesh failed"; return false; }
			material::log_mesh_addressing("capture", static_cast<uintptr_t>(key), mat.albedo_d3d9, mat.wrap_u, mat.wrap_v, verts);
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
				m.bound_c[k] = 0.5f * (lo[k] + hi[k]);
				const float h = 0.5f * (hi[k] - lo[k]);
				r2 += h * h;
			}
			m.bound_r = std::sqrt(r2);
			m.has_bound = std::isfinite(m.bound_r);
			return true;
		}

		void release_material(uint64_t key)
		{
			const auto it = g_mats.find(key);
			if (it == g_mats.end() || !it->second.refs) return;
			it->second.refs--;
			it->second.last_used = frame();
		}

		void destroy_mesh(Mesh& m)
		{
			if (m.mesh) if (auto* b = bridge()) b->DestroyMesh(m.mesh);
			m.mesh = nullptr;
			release_material(m.material_key);
		}

		// The placement's retained instance goes (one call; sweeps batch
		// theirs with retained::destroy_many first), and its hold on the mesh.
		void release_place(Place& p)
		{
			if (p.e.retained) retained::destroy(p.e);
			if (p.retained_mesh)
			{
				if (const auto it = g_meshes.find(p.retained_mesh); it != g_meshes.end() && it->second.places)
					it->second.places--;
				p.retained_mesh = 0;
			}
			p.e.mesh = nullptr;
		}

		// An APP_CULLED node on the geometry's chain (the engine hid it: a
		// switched-off light's glow, a controller's visibility): the owner
		// map's check, or the parent chain walked here for a geometry no
		// reference owns.
		bool app_culled(uintptr_t geo)
		{
			if (scene_graph::death_pending(geo) || !scene_graph::begin_resolve(geo)) return false;
			bool culled = false;
			FnvRemixOwner o;
			if (nvse_link::owner_of(geo, o))
				culled = (o.flags & FNVR_OW_APP_CULLED) != 0;
			else
			{
				uintptr_t node = geo;
				for (int hop = 0; hop < kMaxChainHops && node; hop++)
				{
					uintptr_t parent = 0;
					uint32_t flags = 0;
					if (!scene_graph::read_link(node, parent, flags)) break;
					if (flags & 1u) { culled = true; break; }
					node = parent;
				}
			}
			scene_graph::end_resolve();
			return culled;
		}

		// A world draw of NiGeometry `geo` with mesh `key`: its retained
		// instance claimed for this frame (true), or it goes out per frame
		// (false, counted by why).
		bool draw_retained(uintptr_t geo, uint64_t key, Mesh& m, const float* w, remixapi_InstanceCategoryFlags cats,
			bool double_sided, const void* ext)
		{
			auto& st = statics_stats();
			const UINT now = frame();
			Place& p = g_places[geo];
			p.e.last_frame = now;
			if (!retained::available())
			{
				g_c.pf_failed++;
				return false;
			}
			// The absolute placement needs the camera the WORLD is relative to;
			// while the engine's camera global disagrees (the Pip-Boy's camera
			// flips, a jump not yet confirmed) a retained instance keeps its
			// last placement and nothing new is decided.
			if (!stable_origin::camera_matches_now())
			{
				if (p.e.retained && p.retained_mesh == key &&
					retained::draw_absolute(p.e, p.e.retained_xform, cats, double_sided, st, ext))
				{
					g_c.retained++;
					return true;
				}
				if (p.e.retained || p.retained_mesh)
				{
					g_c.demoted_mesh++;
					release_place(p);
					p.stable = 0;
				}
				g_c.pf_camera++;
				return false;
			}
			const remixapi_Transform abs = retained::absolute(w);
			if (!p.stable || p.mesh_key != key || !retained::same_placement(abs, p.abs))
			{
				if (p.e.retained || p.retained_mesh)
				{
					if (p.mesh_key != key) g_c.demoted_mesh++;
					else g_c.demoted_moved++;
					release_place(p);
				}
				if (p.stable) g_c.pf_changing++;
				else g_c.pf_warming++;
				p.mesh_key = key;
				p.abs = abs;
				p.stable = 1;
				return false;
			}
			if (!p.e.retained)
			{
				if (p.stable < kStableDraws)
				{
					p.stable++;
					g_c.pf_warming++;
					return false;
				}
				// Owned by a placed non-actor reference (the owner map).
				if (!scene_statics::capture_may_retain(geo))
				{
					g_c.pf_not_placed++;
					return false;
				}
				p.e.mesh = m.mesh;
				p.e.geo = 0;                  // no supersession by geometry key: one placement per NiGeometry
				p.e.residency = Residency::Cell;
				p.e.has_bound = m.has_bound;
				std::memcpy(p.e.bound_c, m.bound_c, sizeof(p.e.bound_c));
				p.e.bound_r = m.bound_r;
			}
			if (!retained::draw_absolute(p.e, abs, cats, double_sided, st, ext))
			{
				g_c.pf_failed++;
				return false;
			}
			if (!p.retained_mesh)
			{
				p.retained_mesh = key;
				m.places++;
				g_c.promoted++;
			}
			g_c.retained++;
			return true;
		}

		// Retained placements whose NiGeometry the scene graph claimed, and
		// placements undrawn past their TTL (retained::stale_after: the
		// keep-culled TTL for a visible retained instance, EvictFrames
		// otherwise); a retained one not drawn this frame whose chain is
		// app-culled is hidden (shown again at its next draw).
		void sweep_places(UINT now, UINT keep)
		{
			auto& st = statics_stats();
			size_t holding = 0;
			for (const auto& [geo, p] : g_places) holding += p.e.retained ? 1u : 0u;
			const bool kept = retained::keep_in_effect(holding, st);
			static std::vector<MeshEntry*> doomed;
			static std::vector<uintptr_t> gone;
			doomed.clear();
			gone.clear();
			for (auto& [geo, p] : g_places)
			{
				const bool scene = (p.e.retained || p.retained_mesh) && scene_statics::claimed(geo);
				if (scene || is_stale(p.e, now, keep, kept))
				{
					if (p.e.retained || p.retained_mesh)
					{
						if (scene) g_c.to_scene++;
						else g_c.evicted++;
					}
					if (p.e.retained) doomed.push_back(&p.e);
					gone.push_back(geo);
					continue;
				}
				if (p.e.retained && p.e.retained_visible && p.e.retained_frame != now &&
					((now + static_cast<UINT>(geo >> 4)) % kAppCullEvery) == 0 && app_culled(geo))
				{
					retained::hide(p.e);
					g_c.app_culled++;
				}
			}
			retained::destroy_many(doomed);
			for (const uintptr_t geo : gone)
			{
				const auto it = g_places.find(geo);
				if (it == g_places.end()) continue;
				release_place(it->second);
				g_places.erase(it);
			}
		}
	}

	// ---- Entry points ----------------------------------------------------------------

	Capture try_static(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d, const char*& why)
	{
		PROFILE_ZONE_N("api::try_static");
		auto& st = statics_stats();
		const auto& ffp = shared::common::ffp_state::get();
		const UINT now = frame();
		const bool vm = game::uses_viewmodel_projection();
		why = nullptr;

		const char* detail = nullptr;
		auto refuse = [&](Reason r) {
			g_c.refused[static_cast<size_t>(r)]++;
			st.fallbacks++;
			st.last_reject = kReasonNames[static_cast<size_t>(r)];
			why = kReasonNames[static_cast<size_t>(r)];
			// Each (reason, PS) once: what is not drawn, and why.
			if (r != Reason::PipBoy && g_logged_refusals.size() < kMaxRefusalLogs)
			{
				Hash64 h;
				h.mix(static_cast<uint64_t>(r));
				h.mix(psmap ? psmap->ps_hash : 0u);
				if (g_logged_refusals.insert(h.get()).second)
					shared::common::log("RemixApi", std::format("capture: PS 0x{:08X} {} {}v {}p{} -> not drawn ({}{}{})",
						psmap ? psmap->ps_hash : 0u, d.indexed ? "indexed" : "non-indexed", d.num_vtx, d.prim_count,
						vm ? " first-person" : "", kReasonNames[static_cast<size_t>(r)], detail ? ": " : "",
						detail ? detail : ""), shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
			}
			return Capture::Refused;
		};
		auto dropped = [&](uint64_t& counter) {
			counter++;
			st.skipped++;
			return Capture::Skipped;
		};

		// The Pip-Boy's own draws keep D3D9 (its screen is a render target the
		// Pip-Boy features drive through the D3D9 draws).
		if (pipboy_draw()) return refuse(Reason::PipBoy);
		auto* b = bridge();
		const float* w = world_matrix();
		if (!b || !w) return refuse(Reason::Inactive);
		if (!psmap || !psmap->has(PsSlotRole::Diffuse)) return refuse(Reason::Shader);
		// A vertex-expander family the expander declined (its declaration
		// lacks what the expansion reads): its positions need the expansion.
		if (const auto* vsd = shared::common::g_vs_classifier.classify(ffp.last_vs());
			vsd && vsd->family != shared::common::VsFamily::None)
			return refuse(Reason::VertexShader);

		// Colour writes off: a depth / stencil pass, nothing to trace.
		if (!(rs(dev, D3DRS_COLORWRITEENABLE, 0xF) & 0xFu)) return dropped(g_c.depth_only);

		// A lit shader's additive / multiply pass is raster lighting: the
		// path tracer replaces what it adds.
		const bool lit = psmap->toggles_reg != PsSlotMap::kNoReg;
		const bool blended = rs(dev, D3DRS_ALPHABLENDENABLE, FALSE) != 0;
		const DWORD src = rs(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
		const DWORD dst = rs(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
		const bool multiply = (src == D3DBLEND_DESTCOLOR && dst == D3DBLEND_ZERO) ||
			(src == D3DBLEND_ZERO && dst == D3DBLEND_SRCCOLOR);
		if (blended && lit && (dst == D3DBLEND_ONE || multiply)) return dropped(g_c.lighting);

		// The object: a later pass of one drawn this frame is dropped. A
		// NiGeometry drawn in several index ranges in one pass (a segmented
		// LOD block, whose triangles the engine draws as runs of the cells it
		// shows) is one object per range.
		if (g_objects_frame != now) { g_objects.clear(); g_object_geos.clear(); g_objects_frame = now; }
		const uintptr_t engine_geo = scene_graph::current_geometry();
		uint64_t object = 0;
		if (engine_geo)
		{
			Hash64 h;
			h.mix(static_cast<uint64_t>(engine_geo));
			h.mix(static_cast<uint64_t>(static_cast<uint32_t>(d.base)) | (static_cast<uint64_t>(d.start_index) << 32));
			h.mix(static_cast<uint64_t>(d.prim_count));
			object = h.get();
		}
		else
		{
			Hash64 h;
			h.ptr(ffp.stream_vb(0));
			h.mix(static_cast<uint64_t>(static_cast<uint32_t>(d.base)) | (static_cast<uint64_t>(d.start_index) << 32));
			h.mix(static_cast<uint64_t>(d.num_vtx) | (static_cast<uint64_t>(d.prim_count) << 32));
			h.add(w + 12, 3 * sizeof(float));
			object = h.get();
		}
		if (g_objects.contains(object)) return dropped(g_c.extra);

		// Layout and buffers.
		Layout L;
		if (!capture_layout(L)) { detail = "declaration"; return refuse(Reason::Layout); }
		if (d.type != D3DPT_TRIANGLELIST && d.type != D3DPT_TRIANGLESTRIP && d.type != D3DPT_TRIANGLEFAN)
		{
			detail = "not triangles";
			return refuse(Reason::Layout);
		}
		IDirect3DVertexBuffer9* vb = ffp.stream_vb(0);
		IDirect3DIndexBuffer9* ib = nullptr;
		if (d.indexed && (FAILED(dev->GetIndices(&ib)) || !ib)) { detail = "no index buffer"; return refuse(Reason::Layout); }
		const bool tracked = identity::track_buffers(vb, ib);
		const identity::BufferInfo vbi = identity::buffer(vb);
		const identity::BufferInfo ibi = identity::buffer(ib);
		if (ib) ib->Release();   // compared by address only from here on
		if (!tracked) { detail = "buffer tracking"; return refuse(Reason::Inactive); }
		if (vbi.dynamic || ibi.dynamic) return refuse(Reason::Dynamic);
		const char* reject = nullptr;
		uint64_t geo = 0;
		if (!geo_key(dev, d, geo, reject))
		{
			detail = reject;
			return refuse(reject && std::strstr(reject, "dynamic") ? Reason::Dynamic : Reason::Layout);
		}

		// Material: what the protocol would carry for the D3D9 draw.
		const auto& cfg = shared::common::config::get().remix_api;
		const auto& rc = rtxconf::scalars();
		const float* pc = ffp.ps_const_data();
		material::Desc md;
		md.diffuse = ffp.cur_texture(psmap->slot(PsSlotRole::Diffuse));
		if (!md.diffuse) { detail = "no diffuse bound"; return refuse(Reason::Texture); }
		md.plain = cfg.statics_debug_plain_material;
		if (rc.protocol_enabled)
		{
			md.normal_role = psmap->has(PsSlotRole::Normal);
			if (md.normal_role)
			{
				md.normal = ffp.cur_texture(psmap->slot(PsSlotRole::Normal));
				// Gloss in the normal's alpha: the lit shaders with one BaseMap,
				// when the runtime reads it so (normalAlphaMode 1).
				md.gloss_alpha = lit && psmap->base_map_count <= 1 && rc.normal_alpha_mode == 1;
				if (md.gloss_alpha)
				{
					const float g = pc[psmap->toggles_reg * 4 + 2];
					if (std::isfinite(g) && g > 0.0f) md.gloss = (std::min)(g, 4096.0f);
				}
			}
			if (psmap->has(PsSlotRole::Glow))
			{
				md.glow = ffp.cur_texture(psmap->slot(PsSlotRole::Glow));
				if (md.glow && psmap->emittance_reg != PsSlotMap::kNoReg)
				{
					const float* e = pc + psmap->emittance_reg * 4;
					const uint8_t c[3] = { unorm8(e[0]), unorm8(e[1]), unorm8(e[2]) };
					if (c[0] < 254 || c[1] < 254 || c[2] < 254)
					{
						const uint8_t hi = (std::max)({ c[0], c[1], c[2] });
						const uint8_t lo = (std::min)({ c[0], c[1], c[2] });
						if (hi - lo <= 1)
						{
							// Grey: an intensity (black: no glow), as the scene statics.
							if (hi == 0) md.glow = nullptr;
							else md.glow_scale = (static_cast<float>(c[0]) + c[1] + c[2]) / (3.0f * 255.0f);
						}
						else if (material::glow_tint_ready())
						{
							for (int k = 0; k < 3; k++) md.glow_tint[k] = static_cast<float>(c[k]) / 255.0f;
						}
						else md.glow = nullptr;   // the runtime can't tint it yet: no glow rather than a white one
					}
				}
			}
			md.height = psmap->has(PsSlotRole::Height) ? ffp.cur_texture(psmap->slot(PsSlotRole::Height))
				: tex_pack::height_for(md.diffuse);
		}
		// The albedo's own sampler stage (not stage 0): addressing and filter;
		// the geometry's own Texture Clamp Mode overrides the addressing
		// (exact shade property class; else the stage's stays).
		material::sampler_from_stage(dev, psmap->slot(PsSlotRole::Diffuse), md);
		material::clamp_from_geometry(engine_geo, md);
		// Hair: a light HairTint brightens the texture up to 2x, past what a
		// vertex colour holds; the material carries max(1, 2 HairTint) times
		// [Hair] Brightness and the vertex colours the rest (material::hair_color).
		float hair_tint[3] = {};
		const bool hair = L.col.ok() && material::hair_tint(psmap, hair_tint);
		if (hair) material::hair_material_tint(hair_tint, md.albedo_tint);
		if (hair && psmap->hair_layer_slot != PsSlotMap::kNoSlot)
		{
			md.hair_layer = ffp.cur_texture(psmap->hair_layer_slot);
			// [RemixApi] ShaderBakeHair: the albedo baked from the PS, once ready.
			md.hair_bake = hair_bake::albedo(psmap, ffp.last_ps(), md.diffuse, md.hair_layer);
			if (md.hair_bake) md.hair_layer = nullptr;
		}
		md.path = "capture";

		// Alpha state. A GREATER / GREATEREQUAL test is the material's cutout,
		// also under plain alpha blending (material::blend_test_as_cutout);
		// plain alpha blending without a test rides on the instance as the
		// FFP route's stage 0 gives it, and any other blend or test is the
		// draw's own state on the instance.
		const bool plain_blend = blended && src == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA &&
			rs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD) == D3DBLENDOP_ADD;
		const DWORD afunc = rs(dev, D3DRS_ALPHATESTENABLE, FALSE) ? rs(dev, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS)
			: static_cast<DWORD>(D3DCMP_ALWAYS);
		const bool plain_test = afunc == D3DCMP_ALWAYS || afunc == D3DCMP_GREATER || afunc == D3DCMP_GREATEREQUAL;
		const bool draw_state = (blended && !plain_blend) || !plain_test;
		md.blend = blended || draw_state;
		if (!draw_state && afunc != D3DCMP_ALWAYS)
		{
			md.alpha_func = static_cast<uint8_t>(afunc);
			md.alpha_ref = static_cast<uint8_t>(rs(dev, D3DRS_ALPHAREF, 0) & 0xFF);
		}
		material::blend_test_as_cutout(md);

		const uint64_t mkey = material::key(md);
		auto mit = g_mats.find(mkey);
		if (mit != g_mats.end() && mit->second.declined && !mit->second.refs && now >= mit->second.retry_frame)
		{
			g_mats.erase(mit);
			mit = g_mats.end();
		}
		if (mit == g_mats.end())
		{
			// First-person parts are never throttled: the weapon must not wait.
			if (!vm && !take_build_budget(mkey)) return refuse(Reason::Budget);
			material::Built bt;
			material::build(md, mkey, kCaptureDomain ^ 0x4D4154ull, bt);
			Mat m;
			m.last_used = now;
			if (bt.why != material::Decline::None)
			{
				for (const uint64_t t : bt.textures) textures::release(t);
				if (bt.handle) b->DestroyMaterial(bt.handle);
				m.declined = true;
				m.why = bt.why == material::Decline::RtxConf ? Reason::RtxConf
					: bt.why == material::Decline::Texture ? Reason::Texture : Reason::Build;
				// A D3D9-only list is for good; a texture not loaded yet
				// or a failed build is tried again.
				m.retry_frame = m.why == Reason::RtxConf ? 0xFFFFFFFFu : now + kRetryFrames;
				if (bt.list) detail = bt.list;
			}
			else
			{
				m.handle = bt.handle;
				m.textures = std::move(bt.textures);
				m.no_vertex_color = bt.no_vertex_color;
				m.alpha_dropped = bt.alpha_dropped;
				m.categories = bt.categories;
				m.albedo_d3d9 = bt.albedo_d3d9;
				m.wrap_u = md.wrap_u;
				m.wrap_v = md.wrap_v;
				g_c.materials_built++;
			}
			mit = g_mats.emplace(mkey, std::move(m)).first;
		}
		Mat& mat = mit->second;
		mat.last_used = now;
		if (mat.declined) return refuse(mat.why);

		// Decal: the shader's decal flags (material::decal_of; the engine
		// monitor sets them on named overlays); one whose placement changed
		// since its last draw is DECAL_DYNAMIC. Logged once per geometry when
		// blended or tagged. The first-person weapon has only its named decal
		// layers (material::named_decal_layer), blended, as DECAL_DYNAMIC.
		// Decided before the mesh: a two-sided decal's mesh carries a reversed
		// copy of its triangles.
		const bool double_sided = double_sided_for_cull(dev);
		material::Decal decal;
		if (!vm)
		{
			bool moving = false;
			if (engine_geo)
				if (const auto pit = g_places.find(engine_geo); pit != g_places.end() && pit->second.stable)
					moving = !retained::same_placement(retained::absolute(w), pit->second.abs);
			decal = material::decal_of(engine_geo, dev, mat.categories, moving);
			material::log_decal("capture", engine_geo ? engine_geo : static_cast<uintptr_t>(object), mat.albedo_d3d9, decal);
		}
		else if (md.blend && material::named_decal_layer(engine_geo))
			decal.categories = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC;
		const bool decal_tagged = (!vm || decal.categories) && material::is_decal(mat.categories | decal.categories);
		const bool reversed_copy = decal_tagged && double_sided;

		// The mesh: this slot's contents with this material. Hair: the PS's
		// tint baked into the vertex colours (material::hair_tint).
		if (hair)
		{
			// The diffuse's file name, for the [Hair] line (the engine renders
			// the geometry now: it is alive).
			char tex[128] = {};
			scene_graph::GeoRead gr;
			if (engine_geo && scene_graph::read_geo(engine_geo, gr))
				if (const uintptr_t shade = gr.props[scene_graph::fnv::kPropShade])
					if (const uintptr_t t = scene_graph::ppl_texture(shade, 0)) scene_graph::texture_name(t, tex, sizeof(tex));
			material::log_hair("capture", psmap->ps_hash, tex, hair_tint);
		}
		const bool vertex_color = hair || (L.col.ok() && !mat.no_vertex_color);
		Hash64 sh;
		sh.ptr(vb);
		sh.ptr(ib);
		sh.mix(ffp.stream_offset(0));
		sh.mix(ffp.stream_stride(0));
		sh.mix(static_cast<uint64_t>(static_cast<uint32_t>(d.base)) | (static_cast<uint64_t>(d.min_vtx) << 32));
		sh.mix(static_cast<uint64_t>(d.num_vtx) | (static_cast<uint64_t>(d.start_index) << 32));
		sh.mix(static_cast<uint64_t>(d.type) | (static_cast<uint64_t>(d.prim_count) << 8) | (d.indexed ? 1ull << 40 : 0ull));
		sh.mix(decl_hash());
		sh.mix(mkey);
		const uint64_t slot = sh.get();
		// The slot's bytes: hashed again only when a buffer was written since
		// (a refill with the same bytes keeps the mesh).
		SlotBytes& sb = g_slot_bytes[slot];
		if (!sb.known || sb.vb_gen != vbi.gen || sb.ib_gen != ibi.gen)
		{
			uint64_t content = 0;
			if (!content_key(dev, d, content, reject))
			{
				detail = reject;
				return refuse(reject && std::strstr(reject, "dynamic") ? Reason::Dynamic : Reason::Layout);
			}
			g_c.content_checks++;
			if (sb.known && sb.content == content) g_c.content_same++;
			sb.known = true;
			sb.vb_gen = vbi.gen;
			sb.ib_gen = ibi.gen;
			sb.content = content;
		}
		Hash64 kh;
		kh.mix(slot);
		kh.mix(geo);
		kh.mix(sb.content);
		kh.mix(vertex_color ? 1u : 0u);
		kh.mix(reversed_copy ? 0x5245564552534544ull : 0ull);   // "REVERSED"
		if (hair)
			for (const float t : hair_tint) kh.mix(static_cast<uint64_t>(t * 4096.0f) | 0x48414952ull << 32);
		const uint64_t key = kh.get();

		auto mesh_it = g_meshes.find(key);
		if (mesh_it == g_meshes.end())
		{
			if (const auto rt = g_retry_at.find(key); rt != g_retry_at.end() && now < rt->second) return refuse(Reason::Retry);
			if (!vm && !take_build_budget(key, d.num_vtx)) return refuse(Reason::Budget);
			Mesh m;
			m.material_key = mkey;
			m.vertex_color = vertex_color;
			if (!build_mesh(dev, d, L, key, mat, reversed_copy, hair ? &hair_tint : nullptr, m, reject))
			{
				g_retry_at[key] = now + kRetryFrames;
				note_failure(st, reject, std::format("PS 0x{:08X}, {} verts, {} prims", psmap->ps_hash, d.num_vtx, d.prim_count));
				detail = reject;
				return refuse(Reason::Build);
			}
			mat.refs++;
			g_c.meshes_built++;
			st.created++;
			// The slot's previous contents: their mesh goes, unless a draw
			// already used it this frame or a retained placement still draws
			// it (then eviction takes it later).
			uint64_t& held = g_slots[slot];
			if (held && held != key)
			{
				if (const auto old = g_meshes.find(held);
					old != g_meshes.end() && old->second.last_frame != now && !old->second.places)
				{
					destroy_mesh(old->second);
					g_meshes.erase(old);
					g_c.replaced++;
					st.evicted++;
				}
			}
			held = key;
			mesh_it = g_meshes.emplace(key, m).first;
			st.live = static_cast<uint32_t>(g_meshes.size());
		}
		Mesh& m = mesh_it->second;
		m.last_frame = now;

		// The instance's alpha state.
		remixapi_InstanceInfoBlendEXT blend_ext;
		bool use_ext = false;
		if (draw_state && !mat.alpha_dropped)
		{
			if (!material::draw_state_blend(dev, m.vertex_color, false, blend_ext, reject))
			{
				detail = reject;
				return refuse(Reason::Blend);
			}
			use_ext = true;
			g_c.draw_state++;
		}
		else if (md.blend && !mat.alpha_dropped)
		{
			blend_ext = material::alpha_blend(md.alpha_func, md.alpha_ref, m.vertex_color);
			use_ext = true;
			g_c.blended++;
		}
		else if (m.vertex_color)
		{
			blend_ext = material::vertex_color_blend();
			use_ext = true;
		}
		// The vertex alpha joins the opacity only with the shader's
		// Vertex_Alpha flag, as in the engine.
		const bool vertex_alpha_dropped = use_ext && material::shader_alpha(decal, blend_ext);

		remixapi_InstanceCategoryFlags cats = mat.categories | decal.categories;
		if (decal.categories) g_c.decals++;
		// What the decal goes out as ([DecalRoute], once per object and mesh).
		auto log_route = [&](const char* instance) {
			if (!decal_tagged) return;
			material::DecalRoute dr;
			dr.path = "capture";
			dr.object = engine_geo ? engine_geo : static_cast<uintptr_t>(object);
			dr.albedo_d3d9 = mat.albedo_d3d9;
			dr.mesh_key = key;
			dr.categories = cats;
			dr.instance = instance;
			dr.draw_alpha = md.blend && !mat.alpha_dropped;
			dr.alpha_func = md.alpha_func;   // the material's test (none when its alpha is dropped)
			if (mat.alpha_dropped) dr.alpha_func = 0;
			dr.alpha_ref = md.alpha_ref;
			dr.ext = use_ext ? &blend_ext : nullptr;
			dr.vertex_alpha_dropped = vertex_alpha_dropped;
			dr.double_sided = double_sided;
			dr.mesh = m.facts;
			material::log_decal_route(dr);
		};
		if (vm)
		{
			const remixapi_InstanceCategoryFlags vmc = api_camera::view_model_categories(dev);
			if (vmc) g_c.camera++;
			cats |= vmc;
			g_c.viewmodel++;
		}

		// A stable world draw of a placed NiGeometry: its retained instance,
		// kept while culled.
		if (!vm)
		{
			bool kept = false;
			if (!engine_geo) g_c.pf_no_geo++;
			// One placement per NiGeometry: its first range of the frame.
			else if (g_object_geos.contains(engine_geo)) g_c.pf_ranges++;
			else if (draw_state)
			{
				g_c.pf_draw_state++;
				// Its alpha state is the draw's own now: no retained copy beside it.
				if (const auto pit = g_places.find(engine_geo);
					pit != g_places.end() && (pit->second.e.retained || pit->second.retained_mesh))
				{
					g_c.demoted_mesh++;
					release_place(pit->second);
					pit->second.stable = 0;
				}
			}
			else kept = draw_retained(engine_geo, key, m, w, cats, double_sided, use_ext ? &blend_ext : nullptr);
			if (kept)
			{
				log_route("retained");
				g_objects.insert(object);
				g_object_geos.insert(engine_geo);
				return Capture::Taken;
			}
		}

		remixapi_InstanceInfo ii = {};
		ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		ii.pNext = use_ext ? &blend_ext : nullptr;
		ii.categoryFlags = cats;
		ii.mesh = m.mesh;
		ii.transform = frame_transform(w);
		ii.doubleSided = double_sided ? 1 : 0;
		if (b->DrawInstance(&ii) != REMIXAPI_ERROR_CODE_SUCCESS) return refuse(Reason::Submit);
		log_route("per frame");
		g_objects.insert(object);
		if (engine_geo) g_object_geos.insert(engine_geo);
		st.draws++;
		g_c.sent++;
		if (!g_logged_first)
		{
			g_logged_first = true;
			shared::common::log("RemixApi", std::format("capture: first draw through the API -- PS 0x{:08X}, {} verts, "
				"{} prims{}", psmap->ps_hash, d.num_vtx, d.prim_count, vm ? ", first-person" : ""),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		return Capture::Taken;
	}

	std::string statics_status()
	{
		const UINT now = frame();
		const double frames = static_cast<double>((std::max)(1u, now - g_window_start));
		g_window_start = now;
		uint64_t refused = 0;
		std::string reasons;
		for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
		{
			refused += g_c.refused[i];
			if (g_c.refused[i])
				reasons += std::format("{}{}={:.1f}", reasons.empty() ? "" : " ", kReasonNames[i], g_c.refused[i] / frames);
		}
		// Retained placements now: live, and how many of them the engine did
		// not draw this frame (kept while culled).
		uint32_t live = 0, culled = 0;
		for (const auto& [geo, p] : g_places)
			if (p.e.retained)
			{
				live++;
				if (p.e.retained_frame != now) culled++;
			}
		const uint64_t per_frame = g_c.pf_warming + g_c.pf_changing + g_c.pf_no_geo + g_c.pf_draw_state +
			g_c.pf_not_placed + g_c.pf_camera + g_c.pf_failed + g_c.pf_ranges;
		std::string s = std::format(
			"capture: world draws per frame: retained={:.1f} perFrame={:.1f} [warming {:.1f} changing {:.1f} noGeometry {:.1f} "
			"drawState {:.1f} notPlaced {:.1f} camera {:.1f} failed {:.1f} moreRanges {:.1f}] | retained placements={} (keptCulled {}, "
			"promoted +{}, demoted moved +{} mesh +{}, toScene +{}, dead +{}, appCulled +{}, evicted +{}) "
			"| DrawInstance per frame={:.1f} (first-person {:.1f}, VIEW_MODEL camera {:.1f}, alpha-blended {:.1f}, draw state {:.1f}) "
			"| decal-tagged={:.1f} "
			"| passes dropped: extra={:.1f} lighting={:.1f} depthOnly={:.1f} | refused={:.1f} [{}] | meshes={} "
			"(+{}, replaced {}, bytes hashed {} same {}, normals computed {}) materials={} (+{})",
			g_c.retained / frames, per_frame / frames, g_c.pf_warming / frames, g_c.pf_changing / frames,
			g_c.pf_no_geo / frames, g_c.pf_draw_state / frames, g_c.pf_not_placed / frames, g_c.pf_camera / frames,
			g_c.pf_failed / frames, g_c.pf_ranges / frames, live, culled, g_c.promoted, g_c.demoted_moved, g_c.demoted_mesh, g_c.to_scene,
			g_c.dead, g_c.app_culled, g_c.evicted,
			g_c.sent / frames, g_c.viewmodel / frames, g_c.camera / frames, g_c.blended / frames, g_c.draw_state / frames,
			g_c.decals / frames,
			g_c.extra / frames, g_c.lighting / frames, g_c.depth_only / frames, refused / frames,
			reasons.empty() ? std::string("-") : reasons, g_meshes.size(), g_c.meshes_built, g_c.replaced,
			g_c.content_checks, g_c.content_same, g_c.computed_normals, g_mats.size(), g_c.materials_built);
		g_c = {};
		return s;
	}

	// Called from api_submit.cpp.
	void statics_on_present()
	{
		const UINT now = frame();
		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.evict_frames);
		auto& st = statics_stats();
		sweep_places(now, keep);
		for (auto it = g_meshes.begin(); it != g_meshes.end();)
		{
			if (!it->second.places && now - it->second.last_frame > keep)
			{
				destroy_mesh(it->second);
				st.evicted++;
				it = g_meshes.erase(it);
			}
			else ++it;
		}
		st.live = static_cast<uint32_t>(g_meshes.size());
		if ((now & 63u) == 0)
		{
			auto* b = bridge();
			for (auto it = g_mats.begin(); it != g_mats.end();)
			{
				if (it->second.refs || now - it->second.last_used <= keep) { ++it; continue; }
				if (b && it->second.handle) b->DestroyMaterial(it->second.handle);
				for (const uint64_t t : it->second.textures) textures::release(t);
				it = g_mats.erase(it);
			}
			std::erase_if(g_retry_at, [now](const auto& r) { return now >= r.second; });
			std::erase_if(g_slots, [](const auto& s) { return !g_meshes.contains(s.second); });
			std::erase_if(g_slot_bytes, [](const auto& s) { return !g_slots.contains(s.first); });
		}
	}

	void statics_on_deaths(const std::vector<uintptr_t>& dead)
	{
		if (g_places.empty()) return;
		for (const uintptr_t geo : dead)
		{
			const auto it = g_places.find(geo);
			if (it == g_places.end()) continue;
			if (it->second.e.retained || it->second.retained_mesh) g_c.dead++;
			release_place(it->second);
			g_places.erase(it);
		}
	}

	// Every retained placement's instance goes (RetainedInstances off, a
	// rebuild); the placements stay and are retained again once stable.
	void statics_drop_retained()
	{
		std::vector<MeshEntry*> v;
		for (auto& [geo, p] : g_places)
			if (p.e.retained) v.push_back(&p.e);
		retained::destroy_many(v);
		for (auto& [geo, p] : g_places)
		{
			release_place(p);
			p.stable = 0;
		}
	}

	void statics_on_reset()
	{
		g_retry_at.clear();
		g_objects.clear();
		g_object_geos.clear();
	}

	void statics_drop_all()
	{
		auto& st = statics_stats();
		statics_drop_retained();
		g_places.clear();
		g_slot_bytes.clear();
		for (auto& [key, m] : g_meshes)
		{
			destroy_mesh(m);
			st.evicted++;
		}
		g_meshes.clear();
		g_slots.clear();
		if (auto* b = bridge())
			for (auto& [key, m] : g_mats)
				if (m.handle) b->DestroyMaterial(m.handle);
		for (auto& [key, m] : g_mats)
			for (const uint64_t t : m.textures) textures::release(t);
		g_mats.clear();
		g_retry_at.clear();
		g_objects.clear();
		g_object_geos.clear();
		st.live = 0;
	}
}
