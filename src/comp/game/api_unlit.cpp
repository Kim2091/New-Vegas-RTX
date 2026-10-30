#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "api_identity.hpp"
#include "api_rtxconf.hpp"
#include "api_material.hpp"
#include "api_camera.hpp"
#include "scene_graph.hpp"
#include "nvse_link.hpp"
#include "vs_expand.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/config.hpp"

/*
 * Unlit NoLighting FX through the Remix API.
 *
 * What is taken
 * -------------
 * The NOLIGHT TexCoordTranform family (VS classifier NoLightUv), from the
 * renderer's expander route. The PS is
 * out = tex(TexCoordTranform * uv) * vertex colour * MaterialColor, unlit.
 * Anything refused below is not drawn (counted per reason), except the
 * Pip-Boy's own draws: its screen FX keep the D3D9 route.
 *   * scene   not the Pip-Boy's own first-person draws while it is not
 *             closed (api::pipboy_draw); other first-person draws carry
 *             VIEW_MODEL.
 *   * layout  stream 0; POSITION float3/4, TEXCOORD0; NORMAL float3 and
 *             COLOR optional (no normal: face normals are computed).
 *   * buffers neither DYNAMIC, streamed into nor refilled after a mesh was
 *             built from them (api_identity). A draw from a DYNAMIC VB (a
 *             particle system: fire, embers) is offered to the particle path
 *             (api_particles.cpp) first and reaches this one only when that
 *             refused it.
 *   * state   colour writes on; blend factors / op and alpha function the
 *             instance can carry (material::draw_state_blend).
 *   * texture lists become instance categories (as scene statics). An
 *             albedo in a D3D9-only list (rtx.lightConverter: the effect
 *             light of a bulb or neon tube) never gets here: the renderer
 *             keeps it on the D3D9 expander route (rtxconf::d3d9_only).
 *   * cards   an alpha-over draw that is not a decal (smoke, dust, steam:
 *             NoLighting cards with NiAlphaProperty SRCALPHA /
 *             INVSRCALPHA) carries PARTICLE, so it stays translucent at
 *             every graphics preset (material::translucent_card). The
 *             game's fog, mist and haze cards (unlit, alpha-over,
 *             UV-animated) never get here: the engine monitor hides them in
 *             the game ([GameFog] in fnv_remix_nvse.ini).
 *
 * Mesh, material
 * --------------
 * One mesh per draw geometry (geo_key + the buffers' write generations) and
 * tint, built from the bridge client's CPU shadow. The UV transform is baked
 * into the texcoords (the D3D9 route's stage-0 texture matrix); MaterialColor
 * up to 1 is folded into the vertex colours (rgb and alpha), the part above
 * 1 rides on the material as MaterialInfoOpaqueAlbedoTintEXT (bridge client
 * permitting). When the UV transform changes, the mesh is refreshed in place
 * under the same hash (MeshInfoRefreshGeometryEXT: same topology, the runtime
 * refits and keeps the history), so animated UVs are re-sent every frame.
 *
 * Tint variants. Another (quantised) MaterialColor is another mesh (and,
 * with glow, another material): a variant of the geometry. Each geometry keeps
 * up to kMaxVariants of them, least recently drawn evicted first (never one
 * drawn this frame: then the cap is soft). Blinking bulbs and chasing signs
 * step through a few MaterialColor states, so after one cycle every step is
 * a cached variant and nothing is built. A variant not built yet (budget,
 * retry) is drawn this frame with the nearest variant the geometry has
 * (closest tint, same texture first); only a geometry with no variant at all
 * waits for its first build. Material variants of textures the geometry
 * already drew take the small-mesh pool (nothing to upload). A geometry that
 * keeps evicting variants it drew recently (a continuous tint) has its
 * MaterialColor quantised more coarsely: 1/64, then 1/16, 1/8, 1/4. A
 * geometry whose set of drawn variants changes from frame to frame is
 * tint-animated: DrawInstance per frame, no retained instances, for
 * kAnimatedHold frames after its last change.
 * Material: api_material's builder with the draw-call alpha state
 * (useDrawCallAlphaState: the instance's InstanceInfoBlendEXT is the draw's
 * own blend / test / write mask, stage 0 = TEXTURE x vertex colour), and,
 * unless the draw blends additively, which the runtime already turns into
 * emission, the texture as its own glow at UnlitEmissiveIntensity, tinted
 * by MaterialColor (the tint needs ApiGlowTint; a tinted draw without it gets
 * no glow rather than a white one).
 *
 * Per frame
 * ---------
 * A draw whose geometry kept its transform, UVs and description for
 * kRetainAfterFrames frames is a retained instance (hidden the first frame
 * the game doesn't draw it); until then, and always while animated or
 * first-person, a DrawInstance at the draw's WORLD. A retained instance that
 * was not in this frame's replay (created or shown just now) is also drawn
 * with this frame's DrawInstance. A second draw of one geometry at the same
 * transform in one frame is folded; at another transform it is a
 * DrawInstance.
 *
 * Log ([RemixApi] UnlitDebugLog): one "unlit draw" line per distinct draw
 * (shaders, texture, UV transform, vertex colour, MaterialColor, blend, and
 * where it went), at most kMaxLogLines a session. Always, once per animated
 * geometry (tint or UV; signs, bulbs), at most kMaxSignLines a session: an
 * "unlit animated" line with its route, glow source, emissive intensity and
 * the animation detected.
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;
	using shared::common::PsSlotMap;
	using shared::common::PsSlotRole;

	namespace
	{
		constexpr uint64_t kUnlitDomain = 0x554E4C4954303031ull;   // "UNLIT001"
		constexpr UINT kRetryFrames = 120;
		constexpr UINT kRetainAfterFrames = 30;
		constexpr uint32_t kMaxLogLines = 256;
		constexpr uint32_t kMaxSignLines = 128;
		// Tint variants kept per geometry (LRU; soft while all were drawn this
		// frame).
		constexpr uint32_t kMaxVariants = 16;
		// Evicting kThrashEvictions variants that were drawn within the last
		// kThrashRecent frames, inside kThrashWindow frames, coarsens the
		// geometry's MaterialColor quantisation one step.
		constexpr UINT kThrashRecent = 120;
		constexpr uint32_t kThrashEvictions = 8;
		constexpr UINT kThrashWindow = 600;
		// MaterialColor steps per unit, finest first.
		constexpr float kQuantSteps[] = { 64.0f, 16.0f, 8.0f, 4.0f };
		constexpr uint32_t kQuantLevels = static_cast<uint32_t>(std::size(kQuantSteps));
		// Frames a tint-animated geometry stays per frame after its last change.
		constexpr UINT kAnimatedHold = 300;
		// A geometry undrawn for longer than this is counted on the status line.
		constexpr UINT kLongUndrawn = 30;

		enum class Reason : uint8_t
		{
			Inactive, PipBoy, Shader, Layout, Dynamic, Rewritten, ColorWrite, Blend, RtxConf,
			Texture, Budget, Build, Retry, Submit, Count
		};
		constexpr const char* kReasonNames[] = {
			"inactive", "pipboy", "shader", "layout", "dynamic", "rewritten", "colorwrite", "blend",
			"rtxconf", "texture", "budget", "build", "retry", "submit",
		};
		static_assert(std::size(kReasonNames) == static_cast<size_t>(Reason::Count));

		struct Counters
		{
			uint64_t retained = 0, per_frame = 0, folded = 0, first_frame = 0, refreshed = 0;
			uint64_t meshes_built = 0, materials_built = 0, emissive = 0, tinted = 0, hdr_tint = 0, viewmodel = 0;
			uint64_t decals = 0;              // draws tagged DECAL_* by material::decal_of
			uint64_t cards = 0;               // alpha-over draws tagged PARTICLE (material::translucent_card)
			// Tint variants: built for a geometry that already had one, drawn
			// again after another variant (an animation step served from the
			// cache), drawn in place of one not built yet, evicted, geometries
			// quantised more coarsely.
			uint64_t variants_built = 0, variants_reused = 0, nearest = 0, variants_evicted = 0, coarsened = 0;
			uint64_t drawn_late = 0;          // geometries drawn after more than kLongUndrawn undrawn frames
			UINT max_undrawn = 0;
			uint64_t fallback[static_cast<size_t>(Reason::Count)] = {};
		};
		Counters g_c;           // since the last status line
		UINT g_window_start = 0;

		struct Mat
		{
			remixapi_MaterialHandle handle = nullptr;
			std::vector<uint64_t> textures;
			uint32_t refs = 0;
			UINT last_used = 0;
			bool no_vertex_color = false;
			uint32_t categories = 0;
			bool alpha_dropped = false;
			bool declined = false;
			Reason why = Reason::Build;
			UINT retry_frame = 0;
			uint64_t albedo_d3d9 = 0;         // the [Decal] line
		};
		std::unordered_map<uint64_t, Mat> g_mats;

		struct Entry
		{
			MeshEntry e;                      // owns e.mesh; e.material stays null (shared)
			uint64_t hash = 0;                // the mesh's API hash (refreshes keep it)
			uint64_t material_key = 0;
			uint64_t geometry = 0;            // the Geo it is a variant of
			uint64_t diffuse = 0;             // the albedo's address (nearest variant: same texture first)
			bool vertex_color = false;        // built with vertex colours
			float uv[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };   // the UV transform the mesh holds
			float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };             // MaterialColor folded into its colours (in the key)
			UINT verts = 0, tris = 0;
			bool animated = false;            // refreshed since built: DrawInstance every frame
			uint64_t blend_sig = 0;           // the instance description its retained instance was made with
			UINT stable_since = 0;            // first frame since the mesh was built / refreshed
			remixapi_Transform place = {};    // absolute transform of its last draw, and since when it holds
			UINT place_since = 0;
			UINT last_frame = 0;
		};
		std::unordered_map<uint64_t, Entry> g_entries;   // (geometry, material, tint) -> mesh

		// A draw geometry and its tint variants.
		struct Geo
		{
			std::vector<uint64_t> variants;   // entry keys, least recently drawn first
			uint32_t quant = 0;               // index into kQuantSteps
			UINT thrash_start = 0;
			uint32_t thrash = 0;              // recently drawn variants evicted since thrash_start
			UINT last_frame = 0;              // last asked for
			uint64_t last_sig = 0;            // the tint / material it drew last
			// The tints drawn per frame (sum of their signatures): a set that
			// changes between consecutive frames is an animation.
			UINT set_frame = 0, prev_set_frame = 0;
			uint64_t set_sum = 0, prev_set_sum = 0;
			UINT tint_animated_until = 0;
			uint32_t tint_changes = 0;
			bool uv_animated = false;
			bool undrawn = false;             // asked for but not drawn since undrawn_since
			UINT undrawn_since = 0;
			bool sign_logged = false;
		};
		std::unordered_map<uint64_t, Geo> g_geos;
		std::unordered_map<uint64_t, UINT> g_retry_at;   // entry key -> first frame to retry a failed build

		// UnlitDebugLog.
		std::unordered_set<uint64_t> g_logged;
		uint32_t g_log_lines = 0;
		bool g_logged_first = false;
		uint32_t g_sign_lines = 0;

		struct Layout
		{
			Attr pos, nrm, uv, col;
			UINT stride = 0;
		};

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

		// MaterialColor quantised (1/steps; 1/64 unless the geometry's tint
		// proved continuous): a value that only wobbles in the last bits is
		// not a new tint.
		float quantise(float v, float steps)
		{
			if (!std::isfinite(v)) return 1.0f;
			return std::round(std::clamp(v, 0.0f, 16.0f) * steps) / steps;
		}

		bool same6(const float* a, const float* b)
		{
			for (int i = 0; i < 6; i++)
				if (std::fabs(a[i] - b[i]) > 1e-6f) return false;
			return true;
		}

		// A D3DCOLOR's bytes (B, G, R, A: the runtime's B8G8R8A8) times the
		// folded tint (each channel <= 1).
		uint32_t modulate(uint32_t bgra, const float (&t)[4])
		{
			uint8_t c[4];
			std::memcpy(c, &bgra, 4);
			c[0] = static_cast<uint8_t>(std::lround(c[0] * (std::min)(t[2], 1.0f)));
			c[1] = static_cast<uint8_t>(std::lround(c[1] * (std::min)(t[1], 1.0f)));
			c[2] = static_cast<uint8_t>(std::lround(c[2] * (std::min)(t[0], 1.0f)));
			c[3] = static_cast<uint8_t>(std::lround(c[3] * (std::min)(t[3], 1.0f)));
			uint32_t out;
			std::memcpy(&out, c, 4);
			return out;
		}

		bool get_layout(Layout& L)
		{
			const auto& ffp = shared::common::ffp_state::get();
			if (ffp.cur_decl_multi_stream() || ffp.cur_decl_is_skinned() || ffp.cur_decl_has_pos_t()) return false;
			L.stride = ffp.stream_stride(0);
			L.pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			L.nrm = find_attr(D3DDECLUSAGE_NORMAL, 0);
			L.uv = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			L.col = find_attr(D3DDECLUSAGE_COLOR, 0);
			if (!L.stride || !L.pos.ok() || (L.pos.type != D3DDECLTYPE_FLOAT3 && L.pos.type != D3DDECLTYPE_FLOAT4) ||
				!attr_fits(L.pos, L.stride))
				return false;
			if (!L.uv.ok() || !attr_fits(L.uv, L.stride)) return false;
			if (L.nrm.ok() && (L.nrm.type != D3DDECLTYPE_FLOAT3 || !attr_fits(L.nrm, L.stride))) L.nrm = {};
			if (L.col.ok() && !attr_fits(L.col, L.stride)) L.col = {};
			return true;
		}

		// The mesh (or, `refresh`, its vertices again under the same hash).
		bool build_mesh(IDirect3DDevice9* dev, const DrawDesc& d, const Layout& L, uint64_t key, bool vertex_color,
			const float (&uv)[6], const float (&tint)[4], const Mat& mat, Entry& en, bool refresh, const char*& reject)
		{
			PROFILE_ZONE_N("api::unlit::build_mesh");
			Geometry g;
			if (!read_geometry(dev, d, g, reject)) return false;
			if (refresh && (g.count != en.verts || g.tris.size() / 3 != en.tris))
			{
				reject = "refresh changed the vertex / triangle count";
				return false;
			}
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
				// The D3D9 route's stage-0 texture matrix, baked.
				const float u = read_comp(p + L.uv.off, L.uv.type, 0);
				const float v = read_comp(p + L.uv.off, L.uv.type, 1);
				o.texcoord[0] = uv[0] * u + uv[1] * v + uv[2];
				o.texcoord[1] = uv[3] * u + uv[4] * v + uv[5];
				if (vertex_color)
				{
					uint32_t c = 0xFFFFFFFFu;
					if (L.col.ok())
					{
						if (L.col.type == D3DDECLTYPE_D3DCOLOR) std::memcpy(&c, p + L.col.off, sizeof(c));
						else c = D3DCOLOR_ARGB(unorm8(read_comp(p + L.col.off, L.col.type, 3)),
							unorm8(read_comp(p + L.col.off, L.col.type, 0)), unorm8(read_comp(p + L.col.off, L.col.type, 1)),
							unorm8(read_comp(p + L.col.off, L.col.type, 2)));
					}
					o.color = modulate(c, tint);
				}
				else o.color = 0xFFFFFFFFu;
			}
			if (!L.nrm.ok())
			{
				// No vertex normal: area-weighted face normals, as the runtime
				// synthesises them for a D3D9 draw.
				for (size_t t = 0; t + 2 < g.tris.size(); t += 3)
				{
					const float* a = verts[g.tris[t]].position;
					const float* b = verts[g.tris[t + 1]].position;
					const float* c = verts[g.tris[t + 2]].position;
					const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
					const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
					const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
					for (int k = 0; k < 3; k++)
						for (const uint32_t vi : { g.tris[t], g.tris[t + 1], g.tris[t + 2] }) verts[vi].normal[k] += n[k];
				}
				for (auto& o : verts)
				{
					const float len = std::sqrt(o.normal[0] * o.normal[0] + o.normal[1] * o.normal[1] + o.normal[2] * o.normal[2]);
					if (std::isfinite(len) && len > 1e-12f) { for (float& c : o.normal) c /= len; }
					else { o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f; }
				}
			}

			remixapi_MeshInfoSurfaceTriangles surf = {};
			surf.vertices_values = verts.data();
			surf.vertices_count = static_cast<uint64_t>(verts.size());
			surf.indices_values = g.tris.data();
			surf.indices_count = static_cast<uint64_t>(g.tris.size());
			surf.skinning_hasvalue = 0;
			surf.material = mat.handle;
			remixapi_MeshInfoRefreshGeometryEXT rg = {};
			rg.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT;
			remixapi_MeshInfo mi = {};
			mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mi.pNext = refresh ? &rg : nullptr;
			mi.hash = refresh ? en.hash : next_object_hash(key, kUnlitDomain);
			mi.surfaces_values = &surf;
			mi.surfaces_count = 1;
			// A refresh keeps the key the mesh was created with.
			remixapi_AssetKeyEXT ak = {};
			if (!refresh) asset_keys::chain(mi, ak, asset_keys::of(verts, g.tris));
			auto* b = bridge();
			remixapi_MeshHandle h = nullptr;
			if (!b || b->CreateMesh(&mi, &h) != REMIXAPI_ERROR_CODE_SUCCESS || !h)
			{
				reject = refresh ? "CreateMesh (refresh) failed" : "CreateMesh failed";
				return false;
			}
			if (!refresh) set_bound(en.e, verts);
			en.e.mesh = h;
			en.hash = mi.hash;
			en.verts = g.count;
			en.tris = static_cast<UINT>(g.tris.size() / 3);
			std::memcpy(en.uv, uv, sizeof(en.uv));
			std::memcpy(en.tint, tint, sizeof(en.tint));
			return true;
		}

		void release_material(uint64_t key)
		{
			const auto it = g_mats.find(key);
			if (it == g_mats.end() || !it->second.refs) return;
			it->second.refs--;
			it->second.last_used = frame();
		}

		// Destroys the entry's API objects (retained instance first, then the
		// mesh) and drops its material reference.
		void destroy_entry_objects(Entry& en)
		{
			if (en.e.retained) retained::destroy(en.e);
			if (en.e.mesh) if (auto* b = bridge()) b->DestroyMesh(en.e.mesh);
			en.e.mesh = nullptr;
			if (en.material_key) release_material(en.material_key);
			en.material_key = 0;
		}

		// The variant was drawn: most recently drawn last.
		void touch_variant(Geo& gs, uint64_t key)
		{
			const auto it = std::find(gs.variants.begin(), gs.variants.end(), key);
			if (it != gs.variants.end()) gs.variants.erase(it);
			gs.variants.push_back(key);
		}

		// Room for one more variant of the geometry: the least recently drawn
		// one goes, unless every variant was drawn this frame (several
		// placements, each its own tint: the cap is soft). Evicting variants
		// drawn just before (a continuous tint never repeats) coarsens the
		// geometry's quantisation.
		void make_room(Geo& gs, UINT now)
		{
			std::erase_if(gs.variants, [](uint64_t k) {
				const auto it = g_entries.find(k);
				return it == g_entries.end() || !it->second.e.mesh;
			});
			while (gs.variants.size() >= kMaxVariants)
			{
				const auto victim = std::find_if(gs.variants.begin(), gs.variants.end(), [now](uint64_t k) {
					return g_entries.find(k)->second.last_frame != now;
				});
				if (victim == gs.variants.end()) return;
				const auto eit = g_entries.find(*victim);
				const bool recent = now - eit->second.last_frame <= kThrashRecent;
				destroy_entry_objects(eit->second);
				g_entries.erase(eit);
				gs.variants.erase(victim);
				g_c.variants_evicted++;
				unlit_stats().evicted++;
				if (!recent) continue;
				if (now - gs.thrash_start > kThrashWindow)
				{
					gs.thrash_start = now;
					gs.thrash = 0;
				}
				if (++gs.thrash >= kThrashEvictions && gs.quant + 1 < kQuantLevels)
				{
					gs.quant++;
					gs.thrash = 0;
					gs.thrash_start = now;
					g_c.coarsened++;
				}
			}
		}

		// The built variant closest to this tint: same texture first, then the
		// smallest tint difference. Its material must still be live.
		Entry* nearest_variant(const Geo& gs, const float (&tint)[4], uint64_t diffuse, uint64_t& key_out)
		{
			Entry* best = nullptr;
			float best_dist = 0.0f;
			for (const uint64_t k : gs.variants)
			{
				const auto it = g_entries.find(k);
				if (it == g_entries.end() || !it->second.e.mesh) continue;
				const auto mit = g_mats.find(it->second.material_key);
				if (mit == g_mats.end() || mit->second.declined || !mit->second.handle) continue;
				const Entry& en = it->second;
				float dist = en.diffuse == diffuse ? 0.0f : 100.0f;
				for (int c = 0; c < 4; c++) dist += std::fabs(en.tint[c] - tint[c]);
				if (!best || dist < best_dist)
				{
					best = &it->second;
					best_dist = dist;
					key_out = k;
				}
			}
			return best;
		}

		// One draw of the geometry with this tint signature. The first draw of
		// a frame closes the frame before: a set of tints that changed between
		// two consecutive frames marks the geometry tint-animated.
		void note_tint(Geo& gs, uint64_t sig, UINT now)
		{
			if (gs.set_frame != now)
			{
				if (gs.prev_set_frame && gs.prev_set_frame + 1 == gs.set_frame && gs.prev_set_sum != gs.set_sum)
				{
					gs.tint_animated_until = gs.set_frame + kAnimatedHold;
					gs.tint_changes++;
				}
				gs.prev_set_frame = gs.set_frame;
				gs.prev_set_sum = gs.set_sum;
				gs.set_frame = now;
				gs.set_sum = 0;
			}
			gs.set_sum += sig;
		}

		uint64_t blend_signature(const remixapi_InstanceInfoBlendEXT& b)
		{
			Hash64 h;
			h.mix(static_cast<uint64_t>(b.alphaTestEnabled) | (static_cast<uint64_t>(b.alphaTestReferenceValue) << 8) |
				(static_cast<uint64_t>(b.alphaTestCompareOp) << 16) | (static_cast<uint64_t>(b.alphaBlendEnabled) << 24) |
				(static_cast<uint64_t>(b.writeMask) << 32) | (static_cast<uint64_t>(b.isVertexColorBakedLighting) << 40));
			h.mix(static_cast<uint64_t>(b.srcColorBlendFactor) | (static_cast<uint64_t>(b.dstColorBlendFactor) << 8) |
				(static_cast<uint64_t>(b.colorBlendOp) << 16) | (static_cast<uint64_t>(b.srcAlphaBlendFactor) << 24) |
				(static_cast<uint64_t>(b.dstAlphaBlendFactor) << 32) | (static_cast<uint64_t>(b.alphaBlendOp) << 40));
			h.mix(static_cast<uint64_t>(b.textureColorArg1Source) | (static_cast<uint64_t>(b.textureColorArg2Source) << 8) |
				(static_cast<uint64_t>(b.textureColorOperation) << 16) | (static_cast<uint64_t>(b.textureAlphaArg1Source) << 24) |
				(static_cast<uint64_t>(b.textureAlphaArg2Source) << 32) | (static_cast<uint64_t>(b.textureAlphaOperation) << 40));
			h.mix(static_cast<uint64_t>(b.tFactor) | (static_cast<uint64_t>(b.isTextureFactorBlend) << 32));
			return h.get();
		}

		// This frame's submission of the entry at the current WORLD. A
		// retained instance that was not in this frame's replay (created or
		// shown just now) is also drawn with this frame's DrawInstance.
		bool submit(Entry& en, remixapi_InstanceCategoryFlags cats, bool double_sided,
			const remixapi_InstanceInfoBlendEXT& blend, bool per_frame, FeatureStats& st)
		{
			auto* b = bridge();
			const float* w = world_matrix();
			if (!b || !w || !en.e.mesh) return false;
			const remixapi_Transform t = frame_transform(w);
			const UINT now = frame();
			if (en.e.drawn_frame == now && std::memcmp(&en.e.drawn_xform, &t, sizeof(t)) == 0)
			{
				g_c.folded++;
				st.skipped++;
				return true;
			}
			const uint64_t sig = blend_signature(blend) ^ (static_cast<uint64_t>(cats) << 1) ^ (double_sided ? 1ull : 0ull);
			const bool first_today = en.e.drawn_frame != now;
			// The absolute transform bakes the camera in: only while the engine's
			// camera global agrees with this frame's camera (not mid Pip-Boy flip).
			const bool camera_ok = stable_origin::camera_matches_now();
			remixapi_Transform abs = {};
			if (camera_ok)
			{
				// Where it is, and since when: only a draw that stayed put for
				// kRetainAfterFrames becomes a retained instance.
				abs = retained::absolute(w);
				if (first_today && !retained::same_placement(abs, en.place))
				{
					en.place = abs;
					en.place_since = now;
				}
			}
			const bool can_retain = first_today && retained::enabled() && retained::available();
			// This frame's DrawInstance at the draw's WORLD.
			auto draw_now = [&]() {
				remixapi_InstanceInfo ii = {};
				ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
				ii.pNext = const_cast<remixapi_InstanceInfoBlendEXT*>(&blend);
				ii.categoryFlags = cats;
				ii.mesh = en.e.mesh;
				ii.transform = t;
				ii.doubleSided = double_sided ? 1 : 0;
				return b->DrawInstance(&ii) == REMIXAPI_ERROR_CODE_SUCCESS;
			};
			auto claim = [&](const remixapi_Transform& at) {
				const bool live = en.e.retained && en.e.retained_visible;   // in this frame's replay
				if (!retained::draw_absolute(en.e, at, cats, double_sided, st, &blend)) return false;
				en.blend_sig = sig;
				en.e.drawn_frame = now;
				en.e.drawn_xform = t;
				en.e.last_frame = now;
				// Not in this frame's replay (the next frame's has it): drawn now.
				if (!live && draw_now()) g_c.first_frame++;
				g_c.retained++;
				st.draws++;
				return true;
			};
			if (can_retain && en.e.retained)
			{
				// Its retained instance, while it still describes this draw (with
				// the camera untrustworthy its last transform stands).
				if (!per_frame && en.blend_sig == sig && (!camera_ok || retained::same_placement(abs, en.e.retained_xform)) &&
					claim(camera_ok ? abs : en.e.retained_xform))
					return true;
				// Animated, moved or described otherwise: per frame until it
				// settles again.
				retained::destroy(en.e);
				if (camera_ok) { en.place = abs; en.place_since = now; }
			}
			else if (can_retain && !per_frame && camera_ok && nvse_link::facts().settled &&
				now - en.place_since >= kRetainAfterFrames && claim(abs))
				return true;
			if (!draw_now()) return false;
			en.e.drawn_frame = now;
			en.e.drawn_xform = t;
			en.e.last_frame = now;
			g_c.per_frame++;
			st.draws++;
			return true;
		}

		std::string describe_texture(IDirect3DBaseTexture9* base)
		{
			if (!base) return "none";
			if (base->GetType() != D3DRTYPE_TEXTURE) return std::format("type {}", static_cast<int>(base->GetType()));
			IDirect3DTexture9* t2d = nullptr;
			if (FAILED(base->QueryInterface(IID_IDirect3DTexture9, reinterpret_cast<void**>(&t2d))) || !t2d) return "?";
			D3DSURFACE_DESC sd = {};
			const bool ok = SUCCEEDED(t2d->GetLevelDesc(0, &sd));
			t2d->Release();
			if (!ok) return "?";
			std::string s = std::format("fmt={} {}x{} pool={}", static_cast<int>(sd.Format), sd.Width, sd.Height,
				static_cast<int>(sd.Pool));
			uint64_t h = 0;
			if (rtxconf::d3d9_image_hash(base, h))
			{
				const char* list = rtxconf::listed(h);
				s += std::format(" d3d9Hash=0x{:016X}{}", h, list ? std::format(" [{}]", list) : std::string());
			}
			return s;
		}

		struct Decision
		{
			bool taken = false;
			Reason why = Reason::Count;
			const char* detail = nullptr;
			bool retained = false, emissive = false, refreshed = false, viewmodel = false;
			bool nearest = false;             // drawn with the nearest built variant (this tint's not built yet)
			uint64_t geometry = 0;            // 0: refused before the geometry was known
			const char* glow = "none";        // where its emission comes from ("unlit animated" line)
		};

		// UnlitDebugLog: the first sight of each distinct (shaders, texture,
		// UV transform kind, blend, layout, decision).
		void log_draw(IDirect3DDevice9* dev, const shared::common::VsDesc& vs, const PsSlotMap* psmap, const DrawDesc& d,
			bool xform, const float (&uv)[6], const float (&mc)[4], bool has_mc, const Decision& dec)
		{
			if (g_log_lines >= kMaxLogLines) return;
			const auto& ffp = shared::common::ffp_state::get();
			IDirect3DBaseTexture9* tex = psmap ? ffp.cur_texture(psmap->slot(PsSlotRole::Diffuse)) : nullptr;
			const DWORD blend = rs(dev, D3DRS_ALPHABLENDENABLE, FALSE), src = rs(dev, D3DRS_SRCBLEND, D3DBLEND_ONE),
				dst = rs(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO), op = rs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
			const DWORD atest = rs(dev, D3DRS_ALPHATESTENABLE, FALSE), afunc = rs(dev, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS),
				aref = rs(dev, D3DRS_ALPHAREF, 0), zw = rs(dev, D3DRS_ZWRITEENABLE, TRUE), cull = rs(dev, D3DRS_CULLMODE, D3DCULL_CCW),
				cw = rs(dev, D3DRS_COLORWRITEENABLE, 0xF);
			Hash64 k;
			k.mix(psmap ? psmap->ps_hash : 0u);
			k.mix(vs.vs_hash);
			k.ptr(tex);
			k.mix(static_cast<uint64_t>(xform) | (static_cast<uint64_t>(blend) << 1) | (static_cast<uint64_t>(src) << 8) |
				(static_cast<uint64_t>(dst) << 16) | (static_cast<uint64_t>(atest) << 24));
			k.mix(decl_hash());
			k.mix(static_cast<uint64_t>(dec.why) | (static_cast<uint64_t>(dec.taken) << 8) | (static_cast<uint64_t>(dec.viewmodel) << 9));
			if (!g_logged.insert(k.get()).second) return;
			g_log_lines++;

			// Vertex colours: the first one and whether all are the same.
			std::string vcol = "none";
			const Attr col = find_attr(D3DDECLUSAGE_COLOR, 0);
			if (col.ok())
			{
				Geometry g;
				const char* rej = nullptr;
				if (read_geometry(dev, d, g, rej) && g.count && attr_fits(col, g.stride))
				{
					auto read = [&](UINT i) {
						const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride + col.off;
						return D3DCOLOR_ARGB(unorm8(read_comp(p, col.type, 3)), unorm8(read_comp(p, col.type, 0)),
							unorm8(read_comp(p, col.type, 1)), unorm8(read_comp(p, col.type, 2)));
					};
					const uint32_t first = read(0);
					bool uniform = true;
					for (UINT i = 1; i < g.count && uniform; i++) uniform = read(i) == first;
					vcol = std::format("0x{:08X}{}", first, uniform ? " (uniform)" : " (varies)");
				}
				else vcol = std::format("unreadable ({})", rej ? rej : "?");
			}
			const uintptr_t geo = scene_graph::current_geometry();
			const scene_graph::ClassInfo* ci = geo ? scene_graph::class_of(geo) : nullptr;
			std::string where = dec.taken
				? std::format("API ({}{}{}{})", dec.retained ? "retained" : "DrawInstance", dec.emissive ? ", emissive" : "",
					dec.refreshed ? ", refreshed" : "", dec.viewmodel ? ", view model" : "")
				: std::format("{} ({}{}{})", dec.why == Reason::PipBoy ? "D3D9" : "not drawn",
					kReasonNames[static_cast<size_t>(dec.why)], dec.detail ? ": " : "",
					dec.detail ? dec.detail : "");
			shared::common::log("RemixApi", std::format(
				"unlit draw #{}: PS 0x{:08X} VS 0x{:08X} geo 0x{:X} {} | tex {} | uv {} | vcol {} | MaterialColor {} | "
				"blend {} src {} dst {} op {} atest {} func {} ref {} zwrite {} cull {} colorWrite 0x{:X} | {} {}v {}p{} | "
				"viewmodel {} noLighting {} -> {}",
				g_log_lines, psmap ? psmap->ps_hash : 0u, vs.vs_hash, geo, ci ? ci->name : std::string("-"),
				describe_texture(tex),
				xform ? std::format("transform ({:.4f} {:.4f} {:.4f} / {:.4f} {:.4f} {:.4f})", uv[0], uv[1], uv[2], uv[3], uv[4], uv[5])
					: std::string("identity"),
				vcol, has_mc ? std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", mc[0], mc[1], mc[2], mc[3]) : std::string("none"),
				blend, src, dst, op, atest, afunc, aref, zw, cull, cw,
				d.indexed ? "indexed" : "non-indexed", d.num_vtx, d.prim_count, d.type == D3DPT_TRIANGLESTRIP ? " strip" : "",
				game::uses_viewmodel_projection() ? 1 : 0, game::is_no_lighting() ? 1 : 0, where),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		// Once per animated geometry (tint or UV transform: signs, chasing
		// bulbs): route, glow source, emissive intensity, animation.
		void log_sign(const Geo& gs, IDirect3DBaseTexture9* tex, const Decision& dec, const float (&mc)[4], bool has_mc)
		{
			if (g_sign_lines >= kMaxSignLines) return;
			g_sign_lines++;
			const auto& cfg = shared::common::config::get().remix_api;
			const uintptr_t geo = scene_graph::current_geometry();
			const scene_graph::ClassInfo* ci = geo ? scene_graph::class_of(geo) : nullptr;
			std::string anim;
			if (gs.tint_changes)
				anim = std::format("MaterialColor ({} variants, quantised 1/{:.0f})", gs.variants.size(), kQuantSteps[gs.quant]);
			if (gs.uv_animated) anim += std::format("{}UV transform (refreshed in place)", anim.empty() ? "" : " + ");
			FnvRemixOwner o = {};
			const std::string owner = geo && nvse_link::owner_of(geo, o)
				? std::format("ref {:08X} base type 0x{:02X}", o.ref_id, o.base_type)
				: std::string("none (not under a loaded reference)");
			shared::common::log("RemixApi", std::format(
				"unlit animated #{}: geo 0x{:X} {} | tex {} | route API unlit ({}{}{}) | glow {} | emissive intensity {:.2f} | "
				"MaterialColor {} | animation {} | owner {}",
				g_sign_lines, geo, ci ? ci->name : std::string("-"), describe_texture(tex),
				dec.retained ? "retained" : "DrawInstance", dec.nearest ? ", nearest variant" : "",
				dec.viewmodel ? ", view model" : "", dec.glow, dec.emissive ? cfg.unlit_emissive_intensity : 0.0f,
				has_mc ? std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", mc[0], mc[1], mc[2], mc[3]) : std::string("none"),
				anim.empty() ? std::string("-") : anim, owner),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		Decision evaluate(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d, const float (&uv)[6], bool xform,
			const float (&raw_mc)[4], bool has_mc)
		{
			PROFILE_ZONE_N("api::unlit::evaluate");
			Decision dec;
			auto refuse = [&](Reason r, const char* detail = nullptr) { dec.why = r; dec.detail = detail; return dec; };
			const auto& cfg = shared::common::config::get().remix_api;
			auto& st = unlit_stats();
			const auto& ffp = shared::common::ffp_state::get();

			if (!bridge() || !world_matrix()) return refuse(Reason::Inactive);
			if (pipboy_draw()) return refuse(Reason::PipBoy);
			dec.viewmodel = game::uses_viewmodel_projection();
			if (!psmap || !psmap->has(PsSlotRole::Diffuse)) return refuse(Reason::Shader);

			Layout L;
			if (!get_layout(L)) return refuse(Reason::Layout, "declaration");
			if (d.type != D3DPT_TRIANGLELIST && d.type != D3DPT_TRIANGLESTRIP && d.type != D3DPT_TRIANGLEFAN)
				return refuse(Reason::Layout, "not triangles");
			if (!(rs(dev, D3DRS_COLORWRITEENABLE, 0xF) & 0xFu)) return refuse(Reason::ColorWrite);

			// Buffers.
			IDirect3DVertexBuffer9* vb = ffp.stream_vb(0);
			IDirect3DIndexBuffer9* ib = nullptr;
			if (d.indexed && (FAILED(dev->GetIndices(&ib)) || !ib)) return refuse(Reason::Layout, "no index buffer");
			const bool tracked = identity::track_buffers(vb, ib);
			const identity::BufferInfo vbi = identity::buffer(vb);
			const identity::BufferInfo ibi = identity::buffer(ib);
			if (ib) ib->Release();   // compared by address only from here on
			if (!tracked) return refuse(Reason::Inactive, "buffer tracking");
			if (vbi.dynamic || ibi.dynamic || vbi.streamed || ibi.streamed) return refuse(Reason::Dynamic);
			if (vbi.rewritten || ibi.rewritten) return refuse(Reason::Rewritten);
			const char* reject = nullptr;
			uint64_t geo = 0;
			if (!geo_key(dev, d, geo, reject))
				return refuse(reject && std::strstr(reject, "dynamic") ? Reason::Dynamic : Reason::Layout, reject);

			// The geometry (the buffers' generations name the contents) and its
			// tint variants.
			Hash64 gh;
			gh.mix(geo);
			gh.mix(vbi.gen);
			gh.mix(ibi.gen);
			const uint64_t geometry = gh.get();
			dec.geometry = geometry;
			const UINT now = frame();
			Geo& gs = g_geos[geometry];
			gs.last_frame = now;
			// MaterialColor at the geometry's quantisation (coarser once its
			// tint proved continuous); the part up to 1 is folded into the
			// vertex colours.
			float mc[4];
			for (int k = 0; k < 4; k++) mc[k] = quantise(raw_mc[k], kQuantSteps[gs.quant]);
			const float tint[4] = { (std::min)(mc[0], 1.0f), (std::min)(mc[1], 1.0f), (std::min)(mc[2], 1.0f), (std::min)(mc[3], 1.0f) };

			// Material description.
			material::Desc md;
			md.diffuse = ffp.cur_texture(psmap->slot(PsSlotRole::Diffuse));
			if (!md.diffuse) return refuse(Reason::Texture, "no diffuse bound");
			md.blend = true;            // the instance carries the draw's own state
			// The albedo's own sampler stage (not stage 0): addressing and filter;
			// the geometry's own Texture Clamp Mode overrides the addressing
			// (exact shade property class; else the stage's stays).
			material::sampler_from_stage(dev, psmap->slot(PsSlotRole::Diffuse), md);
			material::clamp_from_geometry(scene_graph::current_geometry(), md);
			md.path = "unlit";
			const bool tinted = mc[0] != 1.0f || mc[1] != 1.0f || mc[2] != 1.0f;
			const bool hdr = mc[0] > 1.0f || mc[1] > 1.0f || mc[2] > 1.0f;
			if (hdr && material::albedo_tint_supported())
				for (int k = 0; k < 3; k++) md.albedo_tint[k] = (std::max)(mc[k], 1.0f);
			const bool additive = material::draw_is_additive(dev);
			if (cfg.unlit_emissive_intensity > 0.0f && !additive && (!tinted || material::glow_tint_ready()))
			{
				md.glow = md.diffuse;
				md.emissive_intensity = cfg.unlit_emissive_intensity;
				if (tinted) for (int k = 0; k < 3; k++) md.glow_tint[k] = mc[k];
				dec.emissive = true;
				dec.glow = tinted ? "texture x MaterialColor (its own glow)" : "texture (its own glow)";
			}
			else if (additive)
				dec.glow = "additive blend: the runtime's emissive-blend override (rtx.emissiveBlendOverrideEmissiveIntensity)";
			else if (cfg.unlit_emissive_intensity <= 0.0f)
				dec.glow = "NONE: [RemixApi] UnlitEmissiveIntensity=0 (Host Settings: Unlit FX emissive intensity)";
			else
				dec.glow = "NONE: tinted and the glow tint is not ready (ApiGlowTint off or not pushed yet)";
			const uint64_t mkey = material::key(md);
			const uint64_t diffuse_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(md.diffuse));
			// The tint (and material) this draw asks for: animation detection.
			Hash64 sh;
			sh.mix(mkey);
			sh.add(tint, sizeof(tint));
			const uint64_t sig = sh.get();

			// Draws the entry: this tint's variant, or the nearest built one.
			auto finish = [&](Entry& en, const Mat& m, uint64_t ekey, const remixapi_InstanceInfoBlendEXT& blend,
				bool fresh) -> Decision {
				en.last_frame = now;
				en.e.last_frame = now;
				touch_variant(gs, ekey);
				note_tint(gs, sig, now);
				// Another cached step of a tint animation: nothing built.
				if (!fresh && !dec.nearest && gs.last_sig && sig != gs.last_sig && now < gs.tint_animated_until)
					g_c.variants_reused++;
				gs.last_sig = sig;

				// The UV transform changed since the mesh was sent: the same mesh
				// with this frame's texcoords (its own tint and material).
				if (!fresh && !same6(en.uv, uv))
				{
					if (en.e.retained) retained::destroy(en.e);   // animated: per frame from now on
					float own_tint[4];
					std::memcpy(own_tint, en.tint, sizeof(own_tint));
					const char* rj = nullptr;
					if (!build_mesh(dev, d, L, ekey, en.vertex_color, uv, own_tint, m, en, true, rj))
					{
						destroy_entry_objects(en);
						g_retry_at[ekey] = now + kRetryFrames;
						note_failure(st, rj, std::format("PS 0x{:08X} refresh", psmap->ps_hash));
						return refuse(Reason::Build, rj);
					}
					en.animated = true;
					en.stable_since = now;
					dec.refreshed = true;
					g_c.refreshed++;
					gs.uv_animated = true;
				}

				remixapi_InstanceCategoryFlags cats = m.categories;
				if (dec.viewmodel) cats |= api_camera::view_model_categories(dev);
				else
				{
					// Decal: the shader's decal flags (material::decal_of);
					// DECAL_DYNAMIC while it settles after a move. Logged once
					// per geometry when blended or tagged.
					const uintptr_t engine_geo = scene_graph::current_geometry();
					const bool moving = en.place_since > en.stable_since && now - en.place_since < kRetainAfterFrames;
					const material::Decal decal = material::decal_of(engine_geo, dev, m.categories, moving);
					material::log_decal("unlit", engine_geo ? engine_geo : static_cast<uintptr_t>(geometry), m.albedo_d3d9, decal);
					cats |= decal.categories;
					if (decal.categories) g_c.decals++;
				}
				// Smoke, dust and steam cards (alpha-over blend): PARTICLE, so they
				// stay translucent whatever the graphics preset.
				if (!material::is_decal(cats) && material::translucent_card(blend))
				{
					cats |= REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP;
					g_c.cards++;
				}
				// Tint-animated: its variants take turns, per frame (a retained
				// instance per step would be made and hidden over and over).
				const bool per_frame = en.animated || dec.viewmodel || now - en.stable_since < kRetainAfterFrames ||
					now < gs.tint_animated_until;
				const uint64_t retained_before = g_c.retained;
				if (!submit(en, cats, double_sided_for_cull(dev), blend, per_frame, st))
					return refuse(Reason::Submit);
				dec.taken = true;
				dec.retained = g_c.retained != retained_before;
				if (dec.emissive) g_c.emissive++;
				if (tinted) g_c.tinted++;
				if (hdr && md.albedo_tint[0] * md.albedo_tint[1] * md.albedo_tint[2] != 1.0f) g_c.hdr_tint++;
				if (dec.viewmodel) g_c.viewmodel++;
				if (!gs.sign_logged && (gs.uv_animated || gs.tint_changes))
				{
					gs.sign_logged = true;
					log_sign(gs, md.diffuse, dec, raw_mc, has_mc);
				}
				return dec;
			};
			// This tint's variant can't be drawn this frame (budget, retry,
			// failed build): the nearest built variant of the geometry is.
			auto nearest_or = [&](Reason why, const char* detail) -> Decision {
				uint64_t nkey = 0;
				Entry* ne = nearest_variant(gs, tint, diffuse_id, nkey);
				if (!ne) return refuse(why, detail);
				const Mat& fm = g_mats.find(ne->material_key)->second;
				remixapi_InstanceInfoBlendEXT fb;
				const char* rj = nullptr;
				if (!material::draw_state_blend(dev, ne->vertex_color, fm.alpha_dropped, fb, rj)) return refuse(why, detail);
				dec.nearest = true;
				g_c.nearest++;
				return finish(*ne, fm, nkey, fb, false);
			};

			auto mit = g_mats.find(mkey);
			if (mit != g_mats.end() && mit->second.declined && !mit->second.refs && now >= mit->second.retry_frame)
			{
				g_mats.erase(mit);
				mit = g_mats.end();
			}
			if (mit == g_mats.end())
			{
				// First-person parts are never throttled. A tint variant of a
				// texture this geometry already drew uploads nothing: it takes
				// the small-mesh pool, as a one-vertex build would.
				bool known_texture = false;
				for (const uint64_t k : gs.variants)
				{
					const auto it = g_entries.find(k);
					if (it != g_entries.end() && it->second.e.mesh && it->second.diffuse == diffuse_id)
					{
						known_texture = true;
						break;
					}
				}
				if (!dec.viewmodel && !take_build_budget(mkey, known_texture ? 1u : 0u)) return nearest_or(Reason::Budget, nullptr);
				material::Built bt;
				material::build(md, mkey, kUnlitDomain ^ 0x4D4154ull, bt);
				Mat m;
				m.last_used = now;
				if (bt.why != material::Decline::None)
				{
					for (const uint64_t t : bt.textures) textures::release(t);
					if (bt.handle) if (auto* b = bridge()) b->DestroyMaterial(bt.handle);
					m.declined = true;
					m.why = bt.why == material::Decline::RtxConf ? Reason::RtxConf
						: bt.why == material::Decline::Texture ? Reason::Texture : Reason::Build;
					m.retry_frame = now + kRetryFrames;
				}
				else
				{
					m.handle = bt.handle;
					m.textures = std::move(bt.textures);
					m.no_vertex_color = bt.no_vertex_color;
					m.categories = bt.categories;
					m.alpha_dropped = bt.alpha_dropped;
					m.albedo_d3d9 = bt.albedo_d3d9;
					g_c.materials_built++;
				}
				mit = g_mats.emplace(mkey, std::move(m)).first;
			}
			Mat& mat = mit->second;
			mat.last_used = now;
			if (mat.declined) return nearest_or(mat.why, nullptr);

			// The instance's alpha state: the draw's own.
			const bool vertex_color = !mat.no_vertex_color && (L.col.ok() || tinted || mc[3] != 1.0f);
			remixapi_InstanceInfoBlendEXT blend;
			if (!material::draw_state_blend(dev, vertex_color, mat.alpha_dropped, blend, reject))
				return refuse(Reason::Blend, reject);

			// The variant: this geometry with this material and tint, with or
			// without vertex colour. Two placements of one mesh with different
			// tints are two variants.
			Hash64 kh;
			kh.mix(geometry);
			kh.mix(mkey);
			kh.mix(vertex_color ? 1u : 0u);
			// The tint folded into the vertex colours is part of the mesh too.
			kh.add(tint, sizeof(tint));
			const uint64_t key = kh.get();
			if (const auto eit = g_entries.find(key); eit != g_entries.end() && eit->second.e.mesh)
				return finish(eit->second, mat, key, blend, false);

			// A new variant: built now (meshes up to kSmallMeshVerts, such as
			// bulbs and signs, take the small-mesh pool), else the nearest one
			// draws.
			if (const auto rt = g_retry_at.find(key); rt != g_retry_at.end() && now < rt->second)
				return nearest_or(Reason::Retry, nullptr);
			if (!dec.viewmodel && !take_build_budget(key, d.num_vtx)) return nearest_or(Reason::Budget, nullptr);
			make_room(gs, now);
			const bool had_variant = !gs.variants.empty();
			Entry& en = g_entries[key];
			destroy_entry_objects(en);
			if (!build_mesh(dev, d, L, key, vertex_color, uv, tint, mat, en, false, reject))
			{
				destroy_entry_objects(en);
				g_retry_at[key] = now + kRetryFrames;
				note_failure(st, reject, std::format("PS 0x{:08X}, {} verts, {} prims", psmap->ps_hash, d.num_vtx, d.prim_count));
				return nearest_or(Reason::Build, reject);
			}
			en.material_key = mkey;
			en.geometry = geometry;
			en.diffuse = diffuse_id;
			en.vertex_color = vertex_color;
			en.animated = false;
			en.stable_since = now;
			mat.refs++;
			g_c.meshes_built++;
			if (had_variant) g_c.variants_built++;
			st.created++;
			st.live = static_cast<uint32_t>(g_entries.size());
			if (!g_logged_first)
			{
				g_logged_first = true;
				shared::common::log("RemixApi", std::format("unlit: first API mesh -- PS 0x{:08X}, {} verts, {} tris, "
					"uv {}, emissive {}", psmap->ps_hash, en.verts, en.tris, xform ? "transformed" : "identity",
					dec.emissive ? 1 : 0), shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			}
			return finish(en, mat, key, blend, true);
		}
	}

	// ---- Entry points ----------------------------------------------------------------

	bool try_unlit(IDirect3DDevice9* dev, const shared::common::VsDesc& vs, const PsSlotMap* psmap, const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_unlit");
		const auto& cfg = shared::common::config::get().remix_api;
		const auto& ffp = shared::common::ffp_state::get();

		// The D3D9 route's stage-0 texture matrix (vs_expand::nolight_uv_matrix):
		// u' = _11 u + _21 v + _41, v' = _12 u + _22 v + _42.
		float uv[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		D3DMATRIX m;
		const bool xform = game::vs_expand::nolight_uv_matrix(vs, m);
		if (xform)
		{
			uv[0] = m._11; uv[1] = m._21; uv[2] = m._41;
			uv[3] = m._12; uv[4] = m._22; uv[5] = m._42;
		}
		// MaterialColor (out = tex * COLOR * MaterialColor); evaluate quantises
		// it at the geometry's step.
		float mc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		const bool has_mc = psmap && psmap->material_color_reg != PsSlotMap::kNoReg;
		if (has_mc)
		{
			const float* c = ffp.ps_const_data() + psmap->material_color_reg * 4;
			for (int k = 0; k < 4; k++) mc[k] = c[k];
		}

		const Decision dec = evaluate(dev, psmap, d, uv, xform, mc, has_mc);
		if (!dec.taken)
		{
			g_c.fallback[static_cast<size_t>(dec.why)]++;
			auto& st = unlit_stats();
			st.fallbacks++;
			st.last_reject = kReasonNames[static_cast<size_t>(dec.why)];
		}
		// How long a known geometry waits undrawn (budget, retry, failed build
		// or submit, with no variant to stand in): the status line's undrawn>30f.
		if (dec.geometry)
		{
			if (const auto git = g_geos.find(dec.geometry); git != g_geos.end())
			{
				Geo& gs = git->second;
				const UINT now = frame();
				if (dec.taken)
				{
					if (gs.undrawn)
					{
						const UINT waited = now - gs.undrawn_since;
						if (waited > kLongUndrawn) g_c.drawn_late++;
						g_c.max_undrawn = (std::max)(g_c.max_undrawn, waited);
						gs.undrawn = false;
					}
				}
				else if (!gs.undrawn && (dec.why == Reason::Budget || dec.why == Reason::Retry || dec.why == Reason::Build ||
					dec.why == Reason::Submit))
				{
					gs.undrawn = true;
					gs.undrawn_since = now;
				}
			}
		}
		if (cfg.unlit_debug_log) log_draw(dev, vs, psmap, d, xform, uv, mc, has_mc, dec);
		return dec.taken;
	}

	std::string unlit_status()
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
		// Built variants per geometry, geometries animated / coarsened now, and
		// geometries still waiting undrawn past kLongUndrawn.
		size_t max_variants = 0, animated = 0, coarse = 0, waiting = 0;
		for (const auto& [key, gs] : g_geos)
		{
			size_t live = 0;
			for (const uint64_t k : gs.variants)
				if (const auto it = g_entries.find(k); it != g_entries.end() && it->second.e.mesh) live++;
			max_variants = (std::max)(max_variants, live);
			if (now < gs.tint_animated_until || gs.uv_animated) animated++;
			if (gs.quant) coarse++;
			if (gs.undrawn && now - gs.last_frame <= 2 && now - gs.undrawn_since > kLongUndrawn) waiting++;
		}
		const uint64_t sent = g_c.retained + g_c.per_frame;
		std::string s = std::format(
			"unlit: per frame sent={:.1f} (retained {:.1f}, DrawInstance {:.1f}, folded {:.1f}, viewModel {:.1f}) "
			"refreshed={:.1f} emissive={:.1f} tinted={:.1f} hdrTint={:.1f} decalTagged={:.1f} translucentCards={:.1f} | refused={:.1f} [{}] | meshes={} (+{}) "
			"materials={} (+{}) firstFrameDrawInstance={} | variants: built +{} reused={:.1f}/frame nearest={:.1f}/frame "
			"evicted +{} coarsened +{} (coarser now {}) maxPerGeometry={} animated={} | undrawn>{}f: {} drawn late, {} still "
			"waiting (both should be ~0) maxUndrawn={}f | logLines={}",
			sent / frames, g_c.retained / frames, g_c.per_frame / frames,
			g_c.folded / frames, g_c.viewmodel / frames, g_c.refreshed / frames, g_c.emissive / frames,
			g_c.tinted / frames, g_c.hdr_tint / frames, g_c.decals / frames, g_c.cards / frames, refused / frames, reasons.empty() ? std::string("-") : reasons,
			g_entries.size(), g_c.meshes_built, g_mats.size(), g_c.materials_built, g_c.first_frame,
			g_c.variants_built, g_c.variants_reused / frames, g_c.nearest / frames, g_c.variants_evicted, g_c.coarsened, coarse,
			max_variants, animated, kLongUndrawn, g_c.drawn_late, waiting, g_c.max_undrawn, g_log_lines);
		g_c = {};
		return s;
	}

	void unlit_on_present()
	{
		const UINT now = frame();
		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.evict_frames);
		for (auto it = g_entries.begin(); it != g_entries.end();)
		{
			Entry& en = it->second;
			if (now - en.last_frame > keep)
			{
				destroy_entry_objects(en);
				unlit_stats().evicted++;
				it = g_entries.erase(it);
			}
			else ++it;
		}
		unlit_stats().live = static_cast<uint32_t>(g_entries.size());
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
			// A geometry's variants were last drawn no later than it: gone too.
			std::erase_if(g_geos, [now, keep](const auto& g) { return now - g.second.last_frame > keep; });
		}
	}

	void unlit_drop_all()
	{
		for (auto& [key, en] : g_entries) destroy_entry_objects(en);
		g_entries.clear();
		if (auto* b = bridge())
			for (auto& [key, m] : g_mats)
				if (m.handle) b->DestroyMaterial(m.handle);
		for (auto& [key, m] : g_mats)
			for (const uint64_t t : m.textures) textures::release(t);
		g_mats.clear();
		g_retry_at.clear();
		g_geos.clear();
		unlit_stats().live = 0;
	}

	void unlit_drop_retained()
	{
		for (auto& [key, en] : g_entries)
			if (en.e.retained) retained::destroy(en.e);
	}
}
