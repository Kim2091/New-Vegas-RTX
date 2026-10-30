#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "api_rtxconf.hpp"
#include "api_material.hpp"
#include "api_camera.hpp"
#include "scene_graph.hpp"
#include "vs_expand.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/config.hpp"

/*
 * Flipbook particles through the Remix API. A refused draw is not drawn
 * (counted per reason); only the Pip-Boy's own draws keep the renderer's D3D9
 * route.
 *
 * What is taken
 * -------------
 * NOLIGHT017/019 particle draws (VS classifier ParticleSubTex): the
 * renderer's FFP_EXPAND_PARTICLE route, before it binds the expanded copy.
 * The engine already builds the quads on the CPU into a dynamic VB; the
 * vertex expander's own replay (vs_expand::describe_for_api) gives this
 * frame's quads with the flipbook cell in TEXCOORD0 and MaterialColor folded
 * into COLOR (clamped to 1), which is what the D3D9 copy draws.
 * Also the NoLightUv (NOLIGHT TexCoordTranform) draws from a DYNAMIC vertex
 * buffer: the effect particle systems (fire, embers) whose PS is
 * out = tex * COLOR * MaterialColor and which the unlit path's built-once
 * meshes can't follow. They take the same replay with TexCoordTranform baked
 * into TEXCOORD0 and MaterialColor (tint, and the alpha the engine fades the
 * whole system with) folded into COLOR, or into the vertex colour itself when
 * the declaration has none. Their texture is also their own glow at
 * UnlitEmissiveIntensity (tinted by MaterialColor, ApiGlowTint permitting)
 * unless the draw blends additively, as the unlit path does for the family.
 *   * the system's identity: its NiParticleSystem from the render-pass
 *     dispatcher, else the vertex buffer it streams into;
 *   * quads: 4 vertices and two triangles per particle, every quad with one
 *     index pattern; the quads may come in any order (a depth-sorted system
 *     reorders them through the index buffer); anything else is refused as
 *     topology, logged with the first indices;
 *   * not the Pip-Boy's own first-person draws while it is not closed
 *     (api::pipboy_draw); other first-person draws carry VIEW_MODEL;
 *   * blend factors / op and alpha function the instance can carry;
 *   * the texture in no rtx.conf list the API can't express.
 *
 * One capacity mesh per system (and per draw of it within a frame): room for
 * a multiple of kQuadStep quads, indices fixed (the system's pattern repeated);
 * every frame its vertices are re-sent under the same hash
 * (MeshInfoRefreshGeometryEXT: same topology, the runtime refits the BLAS and
 * keeps motion history): live quads first, the unused ones collapsed onto
 * one point with zero alpha. More live quads than room: a new, larger mesh.
 * Drawn with one DrawInstance (PARTICLE, no opacity micromaps, double sided,
 * the draw's own InstanceInfoBlendEXT: the runtime handles additive and
 * multiplicative blends as it does for the D3D9 draw). The part of
 * MaterialColor above 1 rides on the material (AlbedoTintEXT, bridge client
 * permitting). A system whose material keeps changing (more than
 * kChurnBuilds times in kChurnWindow frames) keeps its current material for
 * kChurnHold frames. Lifetime: ~NiGeometry (scene-graph dead set) or
 * EvictFrames undrawn.
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;
	using shared::common::PsSlotMap;
	using shared::common::PsSlotRole;

	namespace
	{
		constexpr uint64_t kParticleDomain = 0x5041525449434C31ull;   // "PARTICL1"
		constexpr UINT kQuadStep = 16;
		constexpr UINT kMaxQuads = 16384;
		constexpr UINT kRetryFrames = 120;
		constexpr uint32_t kChurnBuilds = 6;     // material changes within kChurnWindow before a hold
		constexpr UINT kChurnWindow = 120;
		constexpr UINT kChurnHold = 1800;

		enum class Reason : uint8_t
		{
			Inactive, PipBoy, Shader, Unlinked, Expand, Layout, Topology, TooMany, ColorWrite, Blend,
			RtxConf, Texture, Budget, Build, Retry, Submit, Count
		};
		constexpr const char* kReasonNames[] = {
			"inactive", "pipboy", "shader", "unlinked", "expand", "layout", "topology", "tooMany",
			"colorwrite", "blend", "rtxconf", "texture", "budget", "build", "retry", "submit",
		};
		static_assert(std::size(kReasonNames) == static_cast<size_t>(Reason::Count));

		struct Counters
		{
			uint64_t sent = 0, refreshed = 0, grown = 0, quads = 0, empty = 0, meshes_built = 0, materials_built = 0;
			uint64_t hdr_tint = 0, viewmodel = 0, deaths = 0, nolight = 0;
			uint64_t fallback[static_cast<size_t>(Reason::Count)] = {};
		};
		Counters g_c;
		UINT g_window_start = 0;
		std::unordered_set<uint64_t> g_logged_first;      // (PS, VS) whose first DrawInstance was logged
		std::unordered_set<uint64_t> g_logged_refusals;   // (reason, PS, VS) logged once

		struct Mat
		{
			remixapi_MaterialHandle handle = nullptr;
			std::vector<uint64_t> textures;
			uint32_t refs = 0;
			UINT last_used = 0;
			uint32_t categories = 0;
			bool no_vertex_color = false;
			bool alpha_dropped = false;
			bool declined = false;
			Reason why = Reason::Build;
			UINT retry_frame = 0;
		};
		std::unordered_map<uint64_t, Mat> g_mats;

		struct System
		{
			uintptr_t geo = 0;                // the NiParticleSystem (dispatcher geometry)
			remixapi_MeshHandle mesh = nullptr;
			uint64_t hash = 0;
			uint64_t material_key = 0;
			UINT capacity = 0;                // quads
			uint8_t pattern[6] = {};          // index pattern of one quad (offsets 0..3)
			UINT last_frame = 0;
			UINT churn_start = 0;
			uint32_t churn_builds = 0;
			UINT hold_until = 0;
		};
		std::unordered_map<uint64_t, System> g_systems;   // (geometry, draw ordinal this frame) -> system
		std::unordered_map<uint64_t, UINT> g_retry_at;

		// Draws of one geometry this frame (a system drawn twice gets a second mesh).
		std::unordered_map<uintptr_t, uint32_t> g_ordinal;
		UINT g_ordinal_frame = 0xFFFFFFFFu;

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

		void release_material(uint64_t key)
		{
			const auto it = g_mats.find(key);
			if (it == g_mats.end() || !it->second.refs) return;
			it->second.refs--;
			it->second.last_used = frame();
		}

		void destroy_system(System& sys)
		{
			if (sys.mesh) if (auto* b = bridge()) b->DestroyMesh(sys.mesh);
			sys.mesh = nullptr;
			sys.capacity = 0;
			if (sys.material_key) release_material(sys.material_key);
			sys.material_key = 0;
		}

		// The draw's index stream relative to the window's first vertex.
		bool read_indices(IDirect3DDevice9* dev, const DrawDesc& d, std::vector<uint32_t>& out, const char*& reject)
		{
			if (!d.indexed)
			{
				if (d.type != D3DPT_TRIANGLELIST) { reject = "non-indexed, not a list"; return false; }
				out.resize(static_cast<size_t>(d.prim_count) * 3);
				for (size_t i = 0; i < out.size(); i++) out[i] = static_cast<uint32_t>(i);
				return true;
			}
			if (d.type != D3DPT_TRIANGLELIST) { reject = "indexed, not a list"; return false; }
			IDirect3DIndexBuffer9* ib = nullptr;
			if (FAILED(dev->GetIndices(&ib)) || !ib) { reject = "no index buffer"; return false; }
			D3DINDEXBUFFER_DESC desc = {};
			const bool ok_desc = SUCCEEDED(ib->GetDesc(&desc));
			const UINT isz = desc.Format == D3DFMT_INDEX32 ? 4u : 2u;
			const UINT count = d.prim_count * 3;
			bool ok = ok_desc && (static_cast<uint64_t>(d.start_index) + count) * isz <= desc.Size;
			if (!ok) reject = "index range overruns the buffer";
			void* p = nullptr;
			// A READONLY lock of a dynamic IB may not return the data through the
			// bridge: refuse it rather than read garbage.
			if (ok && (desc.Usage & D3DUSAGE_DYNAMIC)) { ok = false; reject = "dynamic index buffer"; }
			if (ok && (FAILED(ib->Lock(d.start_index * isz, count * isz, &p, D3DLOCK_READONLY)) || !p))
			{
				ok = false;
				reject = "IB lock failed";
			}
			if (ok)
			{
				out.resize(count);
				for (UINT i = 0; i < count; i++)
				{
					uint32_t v = 0;
					if (isz == 4) std::memcpy(&v, static_cast<const uint8_t*>(p) + i * 4u, 4);
					else { uint16_t h; std::memcpy(&h, static_cast<const uint8_t*>(p) + i * 2u, 2); v = h; }
					if (v < d.min_vtx || v - d.min_vtx >= d.num_vtx) { ok = false; reject = "index outside the window"; break; }
					out[i] = v - d.min_vtx;
				}
				ib->Unlock();
			}
			ib->Release();
			return ok;
		}

		// Quads: 4 vertices per particle (vertices 4q..4q+3), two triangles per
		// quad, every quad with the same index pattern (offsets from 4q, in the
		// order its triangles come). The order the quads are drawn in is free:
		// a system the engine depth-sorts draws its quads back to front through
		// the index buffer, and the capacity mesh (traced, not blended in draw
		// order) lists them in vertex order.
		bool quad_pattern(const std::vector<uint32_t>& idx, UINT verts, uint8_t (&pattern)[6], const char*& reject)
		{
			const UINT quads = verts / 4;
			if (!quads || verts % 4 || idx.size() % 6 || idx.size() / 6 != quads) { reject = "not 4 vertices / 6 indices per quad"; return false; }
			std::vector<uint8_t> tris(quads, 0);
			std::vector<uint8_t> pat(static_cast<size_t>(quads) * 6, 0);
			for (size_t t = 0; t + 2 < idx.size(); t += 3)
			{
				const uint32_t q = idx[t] / 4;
				if (q >= quads || idx[t + 1] / 4 != q || idx[t + 2] / 4 != q) { reject = "a triangle spans two quads"; return false; }
				uint8_t& n = tris[q];
				if (n >= 2) { reject = "a quad with more than two triangles"; return false; }
				for (size_t k = 0; k < 3; k++)
					pat[static_cast<size_t>(q) * 6 + n * 3u + k] = static_cast<uint8_t>(idx[t + k] - 4u * q);
				n++;
			}
			// Two triangles for every quad (the counts match, none has more).
			std::memcpy(pattern, pat.data(), sizeof(pattern));
			for (size_t q = 1; q < quads; q++)
				if (std::memcmp(pat.data() + q * 6, pattern, sizeof(pattern)) != 0) { reject = "quads differ in index pattern"; return false; }
			return true;
		}

		// A topology refusal with the start of the draw's index stream, for
		// the once-per-(reason, shaders) log line.
		std::string g_topology_detail;
		const char* topology_detail(const char* reject, const std::vector<uint32_t>& idx)
		{
			g_topology_detail = reject ? reject : "?";
			g_topology_detail += "; first indices";
			for (size_t i = 0; i < idx.size() && i < 12; i++) g_topology_detail += std::format(" {}", idx[i]);
			return g_topology_detail.c_str();
		}

		struct Decision
		{
			bool taken = false;
			Reason why = Reason::Count;
			const char* detail = nullptr;
		};

		Decision evaluate(IDirect3DDevice9* dev, const shared::common::VsDesc& vs, const PsSlotMap* psmap, const DrawDesc& d)
		{
			PROFILE_ZONE_N("api::particles::evaluate");
			Decision dec;
			auto refuse = [&](Reason r, const char* detail = nullptr) { dec.why = r; dec.detail = detail; return dec; };
			const auto& cfg = shared::common::config::get().remix_api;
			auto& st = particle_stats();
			const auto& ffp = shared::common::ffp_state::get();

			auto* b = bridge();
			const float* w = world_matrix();
			if (!b || !w) return refuse(Reason::Inactive);
			if (pipboy_draw()) return refuse(Reason::PipBoy);
			const bool vm = game::uses_viewmodel_projection();
			if (!psmap || !psmap->has(PsSlotRole::Diffuse)) return refuse(Reason::Shader);
			if (!(rs(dev, D3DRS_COLORWRITEENABLE, 0xF) & 0xFu)) return refuse(Reason::ColorWrite);

			// The system's identity: its NiGeometry from the render-pass
			// dispatcher, else the vertex buffer it streams into.
			uintptr_t geo = scene_graph::current_geometry();
			if (!geo) geo = reinterpret_cast<uintptr_t>(ffp.stream_vb(0));
			if (!geo) return refuse(Reason::Unlinked);

			// This frame's quads, as the D3D9 copy would draw them.
			const INT base = d.base;
			const UINT skip = d.indexed ? d.min_vtx : 0u;
			const UINT count = d.num_vtx;
			if (!count) return refuse(Reason::Expand, "empty window");
			vs_expand::ApiWindow win;
			std::vector<uint8_t> verts;
			const char* reject = nullptr;
			if (vs_expand::describe_for_api(vs, psmap, base, skip, count, win, &verts, reject) != vs_expand::Bind::Bound)
				return refuse(Reason::Expand, reject);
			const UINT stride = win.stride;
			const Attr pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			const Attr uv = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			const Attr col = find_attr(D3DDECLUSAGE_COLOR, 0);
			if (!pos.ok() || !attr_fits(pos, stride) || !uv.ok() || !attr_fits(uv, stride) || verts.size() < static_cast<size_t>(count) * stride)
				return refuse(Reason::Layout);

			std::vector<uint32_t> idx;
			if (!read_indices(dev, d, idx, reject)) return refuse(Reason::Topology, reject);
			uint8_t pattern[6];
			if (!quad_pattern(idx, count, pattern, reject)) return refuse(Reason::Topology, topology_detail(reject, idx));
			const UINT live = count / 4;
			if (live > kMaxQuads) return refuse(Reason::TooMany);

			// Material: the atlas, the draw's own alpha state, MaterialColor
			// above 1 as the albedo tint.
			material::Desc md;
			md.diffuse = ffp.cur_texture(psmap->slot(PsSlotRole::Diffuse));
			if (!md.diffuse) return refuse(Reason::Texture, "no diffuse bound");
			md.blend = true;
			// The albedo's own sampler stage (not stage 0): addressing and filter.
			material::sampler_from_stage(dev, psmap->slot(PsSlotRole::Diffuse), md);
			md.path = "particles";
			bool hdr = false;
			// MaterialColor, quantised (1/64): the glow tint below.
			float mc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			// MaterialColor as the vertex colour of a declaration without COLOR
			// (with one, the expansion already folded it in).
			uint32_t mc_color = 0xFFFFFFFFu;
			if (psmap->material_color_reg != PsSlotMap::kNoReg)
			{
				const float* c = ffp.ps_const_data() + psmap->material_color_reg * 4;
				for (int k = 0; k < 4; k++)
					mc[k] = std::isfinite(c[k]) ? std::round(std::clamp(c[k], 0.0f, 16.0f) * 64.0f) / 64.0f : 1.0f;
				mc_color = D3DCOLOR_ARGB(unorm8(mc[3]), unorm8(mc[0]), unorm8(mc[1]), unorm8(mc[2]));
				if (material::albedo_tint_supported())
					for (int k = 0; k < 3; k++)
						if (mc[k] > 1.0f)
						{
							md.albedo_tint[k] = mc[k];
							hdr = true;
						}
			}
			// NoLightUv systems (fire, embers): the texture as its own glow, as
			// the unlit path gives the family's static draws, unless the draw
			// blends additively, which the runtime already turns into emission.
			if (vs.family == shared::common::VsFamily::NoLightUv && cfg.unlit_emissive_intensity > 0.0f &&
				!material::draw_is_additive(dev))
			{
				const bool tinted = mc[0] != 1.0f || mc[1] != 1.0f || mc[2] != 1.0f;
				if (!tinted || material::glow_tint_ready())
				{
					md.glow = md.diffuse;
					md.emissive_intensity = cfg.unlit_emissive_intensity;
					if (tinted) for (int k = 0; k < 3; k++) md.glow_tint[k] = mc[k];
				}
			}
			const UINT now = frame();

			// The system (and this draw's ordinal of it this frame).
			if (g_ordinal_frame != now) { g_ordinal.clear(); g_ordinal_frame = now; }
			const uint32_t ordinal = g_ordinal[geo]++;
			Hash64 kh;
			kh.mix(static_cast<uint64_t>(geo));
			kh.mix(ordinal);
			const uint64_t key = kh.get();
			System& sys = g_systems[key];
			sys.geo = geo;

			// Held (its material kept changing, e.g. an animated MaterialColor):
			// it keeps the material it has until the hold ends.
			uint64_t mkey = material::key(md);
			if (now < sys.hold_until && sys.mesh && g_mats.contains(sys.material_key)) mkey = sys.material_key;
			auto mit = g_mats.find(mkey);
			if (mit != g_mats.end() && mit->second.declined && !mit->second.refs && now >= mit->second.retry_frame)
			{
				g_mats.erase(mit);
				mit = g_mats.end();
			}
			if (mit == g_mats.end())
			{
				if (!vm && !take_build_budget(mkey)) return refuse(Reason::Budget);
				material::Built bt;
				material::build(md, mkey, kParticleDomain ^ 0x4D4154ull, bt);
				Mat m;
				m.last_used = now;
				if (bt.why != material::Decline::None)
				{
					for (const uint64_t t : bt.textures) textures::release(t);
					if (bt.handle) b->DestroyMaterial(bt.handle);
					m.declined = true;
					m.why = bt.why == material::Decline::RtxConf ? Reason::RtxConf
						: bt.why == material::Decline::Texture ? Reason::Texture : Reason::Build;
					m.retry_frame = now + kRetryFrames;
				}
				else
				{
					m.handle = bt.handle;
					m.textures = std::move(bt.textures);
					m.categories = bt.categories;
					m.no_vertex_color = bt.no_vertex_color;
					m.alpha_dropped = bt.alpha_dropped;
					g_c.materials_built++;
				}
				mit = g_mats.emplace(mkey, std::move(m)).first;
			}
			Mat& mat = mit->second;
			mat.last_used = now;
			if (mat.declined) return refuse(mat.why);

			const bool has_col = col.ok() && attr_fits(col, stride);
			const bool vertex_color = !mat.no_vertex_color && (has_col || mc_color != 0xFFFFFFFFu);
			remixapi_InstanceInfoBlendEXT blend;
			if (!material::draw_state_blend(dev, vertex_color, mat.alpha_dropped, blend, reject))
				return refuse(Reason::Blend, reject);

			// A new mesh: none yet, another material (then counted toward the
			// churn hold), more live quads than room, another quad pattern.
			const bool material_changed = sys.mesh && sys.material_key != mkey;
			const bool pattern_changed = sys.mesh && std::memcmp(sys.pattern, pattern, sizeof(pattern)) != 0;
			const bool grow = sys.mesh && live > sys.capacity;
			if (material_changed)
			{
				if (now - sys.churn_start > kChurnWindow) { sys.churn_start = now; sys.churn_builds = 0; }
				// Too many: this material stays for kChurnHold frames.
				if (++sys.churn_builds > kChurnBuilds) sys.hold_until = now + kChurnHold;
			}
			const bool create = !sys.mesh || material_changed || pattern_changed || grow;
			if (create)
			{
				if (const auto rt = g_retry_at.find(key); rt != g_retry_at.end() && now < rt->second) return refuse(Reason::Retry);
				if (!vm && !take_build_budget(key, live * 4u)) return refuse(Reason::Budget);
				const UINT old_capacity = sys.capacity;
				destroy_system(sys);
				UINT cap = (std::max)(live, (std::max)(kQuadStep, old_capacity));
				cap = (cap + kQuadStep - 1) / kQuadStep * kQuadStep;
				if (grow) cap = (std::min)((std::max)(cap, old_capacity * 2), kMaxQuads);
				sys.capacity = cap;
				std::memcpy(sys.pattern, pattern, sizeof(pattern));
				sys.hash = next_object_hash(key, kParticleDomain);
				if (grow) g_c.grown++;
			}

			// This frame's vertices: the live quads, then the unused ones on one
			// point with zero alpha (zero area: nothing to hit).
			std::vector<remixapi_HardcodedVertex> hv(static_cast<size_t>(sys.capacity) * 4);
			for (UINT i = 0; i < count; i++)
			{
				const uint8_t* p = verts.data() + static_cast<size_t>(i) * stride;
				auto& o = hv[i];
				o = {};
				for (int k = 0; k < 3; k++)
				{
					o.position[k] = read_comp(p + pos.off, pos.type, k);
					if (!std::isfinite(o.position[k])) return refuse(Reason::Expand, "non-finite POSITION");
				}
				o.texcoord[0] = read_comp(p + uv.off, uv.type, 0);
				o.texcoord[1] = read_comp(p + uv.off, uv.type, 1);
				if (vertex_color && has_col)
				{
					// D3DCOLOR's bytes (B, G, R, A) are the runtime's B8G8R8A8.
					if (col.type == D3DDECLTYPE_D3DCOLOR) std::memcpy(&o.color, p + col.off, sizeof(o.color));
					else o.color = D3DCOLOR_ARGB(unorm8(read_comp(p + col.off, col.type, 3)),
						unorm8(read_comp(p + col.off, col.type, 0)), unorm8(read_comp(p + col.off, col.type, 1)),
						unorm8(read_comp(p + col.off, col.type, 2)));
				}
				else o.color = vertex_color ? mc_color : 0xFFFFFFFFu;
			}
			// Face normals per quad (the quads are flat cards).
			for (UINT q = 0; q < live; q++)
			{
				const float* a = hv[q * 4 + pattern[0]].position;
				const float* bb = hv[q * 4 + pattern[1]].position;
				const float* c = hv[q * 4 + pattern[2]].position;
				const float e1[3] = { bb[0] - a[0], bb[1] - a[1], bb[2] - a[2] };
				const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
				float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (std::isfinite(len) && len > 1e-12f) { for (float& x : n) x /= len; }
				else { n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f; }
				for (UINT v = 0; v < 4; v++) std::memcpy(hv[q * 4 + v].normal, n, sizeof(n));
			}
			{
				remixapi_HardcodedVertex dead = count ? hv[0] : remixapi_HardcodedVertex{};
				dead.color &= 0x00FFFFFFu;   // alpha 0
				if (!count) dead.normal[2] = 1.0f;
				for (size_t i = count; i < hv.size(); i++) hv[i] = dead;
			}
			std::vector<uint32_t> indices(static_cast<size_t>(sys.capacity) * 6);
			for (UINT q = 0; q < sys.capacity; q++)
				for (int k = 0; k < 6; k++) indices[static_cast<size_t>(q) * 6 + k] = q * 4u + sys.pattern[k];

			remixapi_MeshInfoSurfaceTriangles surf = {};
			surf.vertices_values = hv.data();
			surf.vertices_count = static_cast<uint64_t>(hv.size());
			surf.indices_values = indices.data();
			surf.indices_count = static_cast<uint64_t>(indices.size());
			surf.skinning_hasvalue = 0;
			surf.material = mat.handle;
			remixapi_MeshInfoRefreshGeometryEXT rg = {};
			rg.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT;
			remixapi_MeshInfo mi = {};
			mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mi.pNext = create ? nullptr : &rg;
			mi.hash = sys.hash;
			mi.surfaces_values = &surf;
			mi.surfaces_count = 1;
			remixapi_MeshHandle h = nullptr;
			if (b->CreateMesh(&mi, &h) != REMIXAPI_ERROR_CODE_SUCCESS || !h)
			{
				destroy_system(sys);
				g_retry_at[key] = now + kRetryFrames;
				note_failure(st, create ? "CreateMesh failed" : "CreateMesh (refresh) failed",
					std::format("PS 0x{:08X}, {} quads (capacity {})", psmap->ps_hash, live, sys.capacity));
				return refuse(Reason::Build);
			}
			if (create)
			{
				sys.mesh = h;
				sys.material_key = mkey;
				mat.refs++;
				g_c.meshes_built++;
				st.created++;
			}
			else g_c.refreshed++;
			sys.last_frame = now;

			remixapi_InstanceInfo ii = {};
			ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
			ii.pNext = &blend;
			ii.categoryFlags = mat.categories | REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE |
				REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP;
			if (vm) ii.categoryFlags |= api_camera::view_model_categories(dev);
			ii.mesh = sys.mesh;
			ii.transform = frame_transform(w);
			ii.doubleSided = 1;   // camera-facing cards
			if (b->DrawInstance(&ii) != REMIXAPI_ERROR_CODE_SUCCESS) return refuse(Reason::Submit);
			st.draws++;
			st.live = static_cast<uint32_t>(g_systems.size());
			g_c.sent++;
			g_c.quads += live;
			if (hdr) g_c.hdr_tint++;
			if (vm) g_c.viewmodel++;
			const bool nolight = vs.family == shared::common::VsFamily::NoLightUv;
			if (nolight) g_c.nolight++;
			// The first DrawInstance of each (PS, VS), with the alpha state the
			// instance carries (VkBlendFactor / VkCompareOp numbering) and the
			// first quad's vertex colour: what to compare against the D3D9 draw.
			if (g_logged_first.size() < 64 &&
				g_logged_first.insert((static_cast<uint64_t>(psmap->ps_hash) << 32) | vs.vs_hash).second)
			{
				shared::common::log("RemixApi", std::format("particles: first {}DrawInstance -- PS 0x{:08X}{} VS 0x{:08X}, {} quads "
					"(capacity {}), system 0x{:X}, MaterialColor ({:.3f}, {:.3f}, {:.3f}, {:.3f}), glow {} | blend {} src {} dst {} "
					"op {} atest {} op {} ref {} | vcol[0] 0x{:08X}{}",
					nolight ? "NoLightUv " : "", psmap->ps_hash, psmap->premultiplied_alpha ? " (premultiplied)" : "",
					vs.vs_hash, live, sys.capacity, geo, mc[0], mc[1], mc[2], mc[3], md.glow ? 1 : 0,
					blend.alphaBlendEnabled ? 1 : 0, blend.srcColorBlendFactor, blend.dstColorBlendFactor, blend.colorBlendOp,
					blend.alphaTestEnabled ? 1 : 0, blend.alphaTestCompareOp, static_cast<unsigned>(blend.alphaTestReferenceValue),
					count ? hv[0].color : 0u, vertex_color ? "" : " (no vertex colour)"),
					shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			}
			dec.taken = true;
			return dec;
		}
	}

	// ---- Entry points ----------------------------------------------------------------

	bool try_particles(IDirect3DDevice9* dev, const shared::common::VsDesc& vs, const PsSlotMap* psmap, const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_particles");
		// NoLightUv: only a draw from a DYNAMIC vertex buffer (a particle
		// system the engine streams its quads into); the unlit path takes the
		// family's other draws. Passed over without counting.
		if (vs.family == shared::common::VsFamily::NoLightUv)
		{
			IDirect3DVertexBuffer9* vb = shared::common::ffp_state::get().stream_vb(0);
			D3DVERTEXBUFFER_DESC vd = {};
			if (!vb || FAILED(vb->GetDesc(&vd)) || !(vd.Usage & D3DUSAGE_DYNAMIC)) return false;
		}
		const Decision dec = evaluate(dev, vs, psmap, d);
		if (dec.taken)
		{
			vs_expand::count_api_draw(vs, false);
			return true;
		}
		g_c.fallback[static_cast<size_t>(dec.why)]++;
		auto& st = particle_stats();
		st.fallbacks++;
		st.last_reject = kReasonNames[static_cast<size_t>(dec.why)];
		// Each (reason, PS, VS) once: why a particle family is not drawn.
		if (g_logged_refusals.size() < 128)
		{
			Hash64 h;
			h.mix(static_cast<uint64_t>(dec.why));
			h.mix(psmap ? psmap->ps_hash : 0u);
			h.mix(vs.vs_hash);
			if (g_logged_refusals.insert(h.get()).second)
				shared::common::log("RemixApi", std::format("particles: PS 0x{:08X} VS 0x{:08X} {}v {}p -> {} ({}{}{})",
					psmap ? psmap->ps_hash : 0u, vs.vs_hash, d.num_vtx, d.prim_count,
					dec.why == Reason::PipBoy ? "D3D9" : "not drawn", kReasonNames[static_cast<size_t>(dec.why)],
					dec.detail ? ": " : "", dec.detail ? dec.detail : ""), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
		return false;
	}

	std::string particles_status()
	{
		const UINT now = frame();
		const double frames = static_cast<double>((std::max)(1u, now - g_window_start));
		g_window_start = now;
		uint64_t refused = 0;
		std::string reasons;
		for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
		{
			refused += g_c.fallback[i];
			if (g_c.fallback[i])
				reasons += std::format("{}{}={:.1f}", reasons.empty() ? "" : " ", kReasonNames[i], g_c.fallback[i] / frames);
		}
		std::string s = std::format(
			"particles: per frame sent={:.1f} (quads {:.1f}, viewModel {:.1f}, hdrTint {:.1f}, noLightUv {:.1f}) refreshed={:.1f} | "
			"refused={:.1f} [{}] | systems={} materials={} | since last line: built +{} grown +{} mats +{} deaths {}",
			g_c.sent / frames, g_c.quads / frames,
			g_c.viewmodel / frames, g_c.hdr_tint / frames, g_c.nolight / frames, g_c.refreshed / frames, refused / frames,
			reasons.empty() ? std::string("-") : reasons, g_systems.size(), g_mats.size(), g_c.meshes_built, g_c.grown,
			g_c.materials_built, g_c.deaths);
		g_c = {};
		return s;
	}

	void particles_on_present()
	{
		const UINT now = frame();
		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.evict_frames);
		for (auto it = g_systems.begin(); it != g_systems.end();)
		{
			if (now - it->second.last_frame > keep && now >= it->second.hold_until)
			{
				destroy_system(it->second);
				particle_stats().evicted++;
				it = g_systems.erase(it);
			}
			else ++it;
		}
		particle_stats().live = static_cast<uint32_t>(g_systems.size());
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
		}
	}

	void particles_on_deaths(const std::vector<uintptr_t>& dead)
	{
		if (g_systems.empty() || dead.empty()) return;
		const std::unordered_set<uintptr_t> set(dead.begin(), dead.end());
		for (auto it = g_systems.begin(); it != g_systems.end();)
		{
			if (set.count(it->second.geo))
			{
				destroy_system(it->second);
				g_c.deaths++;
				it = g_systems.erase(it);
			}
			else ++it;
		}
		particle_stats().live = static_cast<uint32_t>(g_systems.size());
	}

	void particles_drop_all()
	{
		for (auto& [key, sys] : g_systems) destroy_system(sys);
		g_systems.clear();
		if (auto* b = bridge())
			for (auto& [key, m] : g_mats)
				if (m.handle) b->DestroyMaterial(m.handle);
		for (auto& [key, m] : g_mats)
			for (const uint64_t t : m.textures) textures::release(t);
		g_mats.clear();
		g_retry_at.clear();
		particle_stats().live = 0;
	}
}
