#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "api_rtxconf.hpp"
#include "tex_pack.hpp"
#include "scene_graph.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/config.hpp"

/*
 * FNV land and far land LOD through the Remix API.
 *
 * Near land (SLS2092-2147 PS family, SLS2100/2101 VS)
 * ---------------------------------------------------
 * The PS blends up to 7 BaseMap layers (s0..s6) and their NormalMaps
 * (s7..s13) with per-vertex weights, then multiplies a per-vertex tint:
 *
 *   w[0..3] = TEXCOORD1.xyzw   w[4..6] = TEXCOORD2.xyz      (unnormalised)
 *   albedo  = sum_i w[i] * BaseMap[i](TEXCOORD0)
 *   normal  = normalize(sum_i w[i] * (2 * NormalMap[i](TEXCOORD0).rgb - 1))
 *   out     = lighting * albedo * COLOR0.rgb
 *
 * The runtime's TerrainLayersEXT material reproduces this per ray hit, with
 * the weights and the tint read from textures at weightUV = uv * scale +
 * offset. Those textures are baked here, once per land quad, by rasterising
 * the quad's triangles in TEXCOORD0 space: every texel is the per-triangle
 * linear interpolation of the vertex attributes, as the GPU interpolates
 * them for the game's own draw.
 *
 * Weight texture resolution (TerrainWeightRes, default 65 x 65). An FNV land
 * quadrant is a regular 16 x 16-quad grid with TEXCOORD0 affine in the grid
 * position. The texel grid is laid out so texel centres 0 and res-1 fall
 * exactly on the UV bounds; with res = 16k + 1 every vertex lands on a texel
 * centre and every quad edge on a texel row/column. Bilinear sampling of the
 * baked texture then reproduces the piecewise-linear vertex interpolation
 * exactly along every edge and every texel row/column, and differs only
 * inside a texel cell that the quad diagonal crosses, by at most the
 * cell's bilinear twist, i.e. 1/k^2 of the per-quad twist. k = 4 (65 texels)
 * makes that 1/16 of the per-quad value, well below 8-bit weight precision
 * for real content, at 17 KB per RGBA8 texture. Clamp sampling at the
 * borders is exact because the border texels are the edge vertices.
 *
 * Height maps ([RemixApi] TerrainHeightMaps). Each layer's texture-pack
 * height map ("<BaseMap>_p.dds", tex_pack::height_for) rides in the
 * extension's heightTextures[i]; the depth is OpaqueEXT.displaceIn =
 * rtx.fnvProtocol.heightDisplaceIn, as for statics. The runtime marches the
 * weight-blended height field (a layer without a map counts as flat).
 * A height map is loaded in the background: a quad built while any of its
 * layer lookups was still pending is rebuilt once none is (checked every
 * kHeightRecheckFrames while drawn), and every quad is rebuilt when drawn
 * after TerrainHeightMaps changed. A rebuild is a whole new entry (weights
 * re-baked, new mesh + material), swapped in the way far LOD rebuilds are.
 *
 * Far land LOD (SLS2002/2005 VS, 0x36E87D02 PS)
 * ---------------------------------------------
 * Positions run through lod_sink's replica of the VS geomorph + loaded-area
 * sink; the PS samples its own block texture s0 at t0 * 0.9921875 +
 * 0.00390625 (a half-texel inset), which is baked into the mesh UVs. The
 * decl has no normal: vertex normals are accumulated from the triangles and
 * oriented up (+Z object space: the block is a height field).
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;
	using shared::common::PsSlotMap;
	using shared::common::PsSlotRole;

	namespace
	{

		constexpr uint64_t kTerrainDomain = 0x5445525241494E31ull;   // "TERRAIN1"
		constexpr uint64_t kFarLodDomain  = 0x4641524C4F443031ull;   // "FARLOD01"
		constexpr UINT kRetryFrames = 120;
		constexpr UINT kHeightRecheckFrames = 30;   // pending height lookups, per drawn quad
		constexpr int kMaxLayers = 7;        // FNV: BaseMap[1..7]
		constexpr int kNormalBase = 7;       // NormalMap[N] starts at s7

		std::unordered_map<uint64_t, MeshEntry> g_terrain;
		std::unordered_map<uint64_t, MeshEntry> g_far_lod;
		std::unordered_map<uint64_t, UINT> g_retry_at;   // failed key -> first frame to retry

		// Land geometry drawn this frame -> the entry key that drew it.
		std::unordered_map<uint64_t, uint64_t> g_land_drawn;
		UINT g_land_drawn_frame = 0xFFFFFFFFu;

		bool g_logged_extra_pass = false;

		// D3D9 hashes of land / far-LOD albedos a hiding list names, logged.
		std::unordered_set<uint64_t> g_hiding_logged;
		constexpr size_t kHidingLogMax = 64;

		// The runtime hides an API instance whose albedo's D3D9 image hash is
		// in rtx.ignoreTextures or rtx.hideInstanceTextures (rtxconf::
		// hiding_list). A land quad or far-LOD block hidden that way is a hole
		// in the path-traced world: the entry is flagged, and each texture is
		// logged once with where the draw is.
		void note_hiding_list(MeshEntry& e, IDirect3DBaseTexture9* albedo, const char* what)
		{
			e.list_hidden = false;
			uint64_t h = 0;
			if (!rtxconf::has_lists() || !rtxconf::d3d9_image_hash(albedo, h)) return;
			const char* list = rtxconf::hiding_list(h);
			if (!list) return;
			e.list_hidden = true;
			if (g_hiding_logged.size() >= kHidingLogMax || !g_hiding_logged.insert(h).second) return;
			double pos[3] = {};
			if (const float* w = world_matrix())
			{
				const double* cam = stable_origin::active() ? stable_origin::camera() : nullptr;
				for (int k = 0; k < 3; k++) pos[k] = static_cast<double>(w[12 + k]) + (cam ? cam[k] : 0.0);
			}
			shared::common::log("RemixApi", std::format(
				"{}: albedo 0x{:016X} is in {} -- the runtime hides every API instance with that texture, so the {} at "
				"({:.0f}, {:.0f}, {:.0f}) is missing from the path-traced image. Remove the hash from {} in rtx.conf.",
				what, h, list, what, pos[0], pos[1], pos[2], list),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		bool retry_blocked(uint64_t key)
		{
			const auto it = g_retry_at.find(key);
			return it != g_retry_at.end() && frame() < it->second;
		}

		void block_retry(uint64_t key)
		{
			g_retry_at[key] = frame() + kRetryFrames;
		}

		bool is_pos_type(int t) { return t == D3DDECLTYPE_FLOAT3 || t == D3DDECLTYPE_FLOAT4; }

		// ---- Land detection --------------------------------------------------------

		struct LandLayout
		{
			Attr pos, nrm, uv, col, tc1, tc2;
		};

		// The PS side: BaseMap[N] at s0, and either NormalMap[N] at s7 or no
		// NormalMap at all. A single-layer shader must carry the s7 NormalMap:
		// ordinary objects put theirs at s1, so s7 is what marks it as land.
		bool g_logged_no_layout = false;
		bool g_logged_lit_block = false;

		bool land_shader(const PsSlotMap* m, int& layers, bool& normals)
		{
			if (!m || !m->any_classified) return false;
			if (m->slot(PsSlotRole::Diffuse) != 0) return false;
			layers = m->base_map_count;
			if (layers < 1 || layers > kMaxLayers) return false;
			normals = m->normal_map_count > 0;
			if (normals && (m->normal_map_reg != kNormalBase || m->normal_map_count != m->base_map_count)) return false;
			if (layers == 1 && !normals) return false;
			// A HeightMap sampler of the shader's own would be one map for the
			// whole blend at a stage outside the BaseMap / NormalMap layout; no
			// vanilla land PS declares one, so such a shader is not this family.
			// Layer heights come from the texture packs instead (layer_height).
			if (m->has(PsSlotRole::Glow) || m->has(PsSlotRole::Height) || m->has(PsSlotRole::Decal)) return false;
			return true;
		}

		// [RemixApi] TerrainHeightMaps with a non-zero POM depth.
		bool heights_wanted()
		{
			return shared::common::config::get().remix_api.terrain_height_maps &&
				rtxconf::scalars().height_displace_in > 0.0f;
		}

		// Layer i's texture-pack height map, or nullptr; `pending` = the
		// lookup may still find one (queued / loading).
		IDirect3DTexture9* layer_height(int i, bool& pending)
		{
			const auto& ffp = shared::common::ffp_state::get();
			return tex_pack::height_for(ffp.cur_texture(static_cast<UINT>(i)), &pending);
		}

		bool any_height_pending(int layers)
		{
			for (int i = 0; i < layers; i++)
			{
				bool pending = false;
				layer_height(i, pending);
				if (pending) return true;
			}
			return false;
		}

		// The declaration side: SLS2100/2101's layout.
		bool land_decl(LandLayout& l)
		{
			const auto& ffp = shared::common::ffp_state::get();
			if (ffp.cur_decl_multi_stream() || ffp.cur_decl_is_skinned()) return false;
			l.pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			l.nrm = find_attr(D3DDECLUSAGE_NORMAL, 0);
			l.uv  = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			l.col = find_attr(D3DDECLUSAGE_COLOR, 0);
			l.tc1 = find_attr(D3DDECLUSAGE_TEXCOORD, 1);
			l.tc2 = find_attr(D3DDECLUSAGE_TEXCOORD, 2);
			if (!l.pos.ok() || !is_pos_type(l.pos.type)) return false;
			if (!l.nrm.ok() || !l.uv.ok() || !l.col.ok() || !l.tc1.ok() || !l.tc2.ok()) return false;
			const UINT stride = ffp.stream_stride(0);
			return attr_fits(l.pos, stride) && attr_fits(l.nrm, stride) && attr_fits(l.uv, stride) &&
				attr_fits(l.col, stride) && attr_fits(l.tc1, stride) && attr_fits(l.tc2, stride);
		}

		// ---- Weight / tint bake ----------------------------------------------------

		constexpr int kChannels = 11;   // 8 layer weights + tint rgb

		struct WeightBake
		{
			int res = 0;
			std::vector<uint8_t> w0, w1, tint;   // RGBA8
			bool has_w1 = false;
			bool has_tint = false;
			remixapi_Float2D scale = {}, offset = {};
		};

		uint8_t unorm8(float v)
		{
			if (!(v > 0.0f)) return 0;
			if (v >= 1.0f) return 255;
			return static_cast<uint8_t>(v * 255.0f + 0.5f);
		}

		bool bake_weights(const Geometry& g, const LandLayout& L, int layers, int res,
			WeightBake& out, const char*& reject)
		{
			PROFILE_ZONE_N("api::terrain::bake_weights");
			const UINT n = g.count;
			// The UV bounds come from the vertices the triangles use: a
			// quadrant's draw may declare a vertex window spanning its whole
			// cell, and the texture should cover only the quadrant.
			std::vector<uint8_t> used(n, 0);
			for (const uint32_t t : g.tris) used[t] = 1;

			std::vector<float> u(n), v(n), ch(static_cast<size_t>(n) * kChannels, 0.0f);
			float umin = FLT_MAX, umax = -FLT_MAX, vmin = FLT_MAX, vmax = -FLT_MAX;
			for (UINT i = 0; i < n; i++)
			{
				if (!used[i]) continue;
				const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride;
				u[i] = read_comp(p + L.uv.off, L.uv.type, 0);
				v[i] = read_comp(p + L.uv.off, L.uv.type, 1);
				if (!std::isfinite(u[i]) || !std::isfinite(v[i])) { reject = "non-finite TEXCOORD0"; return false; }
				umin = (std::min)(umin, u[i]); umax = (std::max)(umax, u[i]);
				vmin = (std::min)(vmin, v[i]); vmax = (std::max)(vmax, v[i]);

				float* c = &ch[static_cast<size_t>(i) * kChannels];
				for (int k = 0; k < layers; k++)
				{
					c[k] = (k < 4) ? read_comp(p + L.tc1.off, L.tc1.type, k)
					               : read_comp(p + L.tc2.off, L.tc2.type, k - 4);
				}
				c[8]  = read_comp(p + L.col.off, L.col.type, 0);
				c[9]  = read_comp(p + L.col.off, L.col.type, 1);
				c[10] = read_comp(p + L.col.off, L.col.type, 2);
			}
			const float du = umax - umin, dv = vmax - vmin;
			if (!(du > 1e-6f) || !(dv > 1e-6f)) { reject = "degenerate TEXCOORD0 range"; return false; }

			const float sx = static_cast<float>(res - 1) / du;
			const float sy = static_cast<float>(res - 1) / dv;
			const size_t texels = static_cast<size_t>(res) * res;
			std::vector<float> val(texels * kChannels, 0.0f);
			std::vector<float> best(texels, -1.0f);   // min barycentric of the writer; < 0 = empty
			int overlaps = 0;

			for (size_t t = 0; t + 2 < g.tris.size(); t += 3)
			{
				const uint32_t ia = g.tris[t], ib = g.tris[t + 1], ic = g.tris[t + 2];
				const float X0 = (u[ia] - umin) * sx, Y0 = (v[ia] - vmin) * sy;
				const float X1 = (u[ib] - umin) * sx, Y1 = (v[ib] - vmin) * sy;
				const float X2 = (u[ic] - umin) * sx, Y2 = (v[ic] - vmin) * sy;
				const float area = (X1 - X0) * (Y2 - Y0) - (X2 - X0) * (Y1 - Y0);
				if (std::fabs(area) < 1e-9f) continue;
				const float inv = 1.0f / area;

				const int x0 = (std::max)(0, static_cast<int>(std::ceil((std::min)({ X0, X1, X2 }) - 1e-3f)));
				const int x1 = (std::min)(res - 1, static_cast<int>(std::floor((std::max)({ X0, X1, X2 }) + 1e-3f)));
				const int y0 = (std::max)(0, static_cast<int>(std::ceil((std::min)({ Y0, Y1, Y2 }) - 1e-3f)));
				const int y1 = (std::min)(res - 1, static_cast<int>(std::floor((std::max)({ Y0, Y1, Y2 }) + 1e-3f)));

				const float* ca = &ch[static_cast<size_t>(ia) * kChannels];
				const float* cb = &ch[static_cast<size_t>(ib) * kChannels];
				const float* cc = &ch[static_cast<size_t>(ic) * kChannels];

				for (int y = y0; y <= y1; y++)
				{
					const float py = static_cast<float>(y);
					for (int x = x0; x <= x1; x++)
					{
						const float px = static_cast<float>(x);
						float l0 = ((X1 - px) * (Y2 - py) - (X2 - px) * (Y1 - py)) * inv;
						float l1 = ((X2 - px) * (Y0 - py) - (X0 - px) * (Y2 - py)) * inv;
						float l2 = 1.0f - l0 - l1;
						const float m = (std::min)({ l0, l1, l2 });
						if (m < -1e-3f) continue;

						const size_t ti = static_cast<size_t>(y) * res + x;
						if (best[ti] >= 0.0f)
						{
							// Texels on a shared edge are written by both
							// triangles; only a texel INSIDE two triangles
							// means the UV mapping folds over itself.
							if (m > 0.02f && best[ti] > 0.02f) overlaps++;
							if (m <= best[ti]) continue;
						}
						l0 = (std::max)(l0, 0.0f); l1 = (std::max)(l1, 0.0f); l2 = (std::max)(l2, 0.0f);
						const float s = 1.0f / (l0 + l1 + l2);
						l0 *= s; l1 *= s; l2 *= s;
						float* dst = &val[ti * kChannels];
						for (int k = 0; k < kChannels; k++) dst[k] = l0 * ca[k] + l1 * cb[k] + l2 * cc[k];
						best[ti] = m;
					}
				}
			}

			if (overlaps > res) { reject = "TEXCOORD0 is not a one-to-one map of the quad"; return false; }

			// Texels outside every triangle (a non-rectangular UV footprint)
			// are never the nearest sample of a surface point, but bilinear
			// filtering at the footprint's edge still reads them: fill them
			// from their filled neighbours so the edge isn't darkened.
			size_t filled = 0;
			for (float b : best) if (b >= 0.0f) filled++;
			if (filled == 0) { reject = "no texel covered by the quad"; return false; }
			for (int pass = 0; pass < res && filled < texels; pass++)
			{
				std::vector<float> next = best;
				for (int y = 0; y < res; y++)
				{
					for (int x = 0; x < res; x++)
					{
						const size_t ti = static_cast<size_t>(y) * res + x;
						if (best[ti] >= 0.0f) continue;
						float acc[kChannels] = {};
						int cnt = 0;
						const int nx[4] = { x - 1, x + 1, x, x };
						const int ny[4] = { y, y, y - 1, y + 1 };
						for (int k = 0; k < 4; k++)
						{
							if (nx[k] < 0 || ny[k] < 0 || nx[k] >= res || ny[k] >= res) continue;
							const size_t ni = static_cast<size_t>(ny[k]) * res + nx[k];
							if (best[ni] < 0.0f) continue;
							for (int c = 0; c < kChannels; c++) acc[c] += val[ni * kChannels + c];
							cnt++;
						}
						if (!cnt) continue;
						for (int c = 0; c < kChannels; c++) val[ti * kChannels + c] = acc[c] / static_cast<float>(cnt);
						next[ti] = 0.0f;
						filled++;
					}
				}
				best.swap(next);
			}

			out.res = res;
			out.w0.assign(texels * 4, 0);
			out.w1.assign(texels * 4, 0);
			out.tint.assign(texels * 4, 255);
			out.has_w1 = layers > 4;
			out.has_tint = false;
			for (size_t ti = 0; ti < texels; ti++)
			{
				const float* s = &val[ti * kChannels];
				for (int k = 0; k < 4; k++)
				{
					out.w0[ti * 4 + k] = unorm8(s[k]);
					out.w1[ti * 4 + k] = unorm8(s[4 + k]);
				}
				for (int k = 0; k < 3; k++)
				{
					const uint8_t c = unorm8(s[8 + k]);
					out.tint[ti * 4 + k] = c;
					if (c < 254) out.has_tint = true;
				}
			}

			// weightUV = uv * scale + offset puts texel centre 0 on the UV
			// minimum and texel centre res-1 on the maximum.
			const float r = static_cast<float>(res);
			out.scale  = { static_cast<float>(res - 1) / (r * du), static_cast<float>(res - 1) / (r * dv) };
			out.offset = { (0.5f - umin * sx) / r, (0.5f - vmin * sy) / r };
			return true;
		}

		// ---- Builds ----------------------------------------------------------------

		bool build_terrain(IDirect3DDevice9* dev, const DrawDesc& d, int layers, bool normals,
			const LandLayout& L, uint64_t key, MeshEntry& e, const char*& reject)
		{
			PROFILE_ZONE_N("api::terrain::build");
			const auto& ffp = shared::common::ffp_state::get();
			const auto& cfg = shared::common::config::get().remix_api;

			Geometry g;
			if (!read_geometry(dev, d, g, reject)) return false;

			std::vector<remixapi_HardcodedVertex> verts(g.count);
			for (UINT i = 0; i < g.count; i++)
			{
				const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride;
				auto& o = verts[i];
				o = {};
				for (int k = 0; k < 3; k++) o.position[k] = read_comp(p + L.pos.off, L.pos.type, k);
				float nx = read_comp(p + L.nrm.off, L.nrm.type, 0);
				float ny = read_comp(p + L.nrm.off, L.nrm.type, 1);
				float nz = read_comp(p + L.nrm.off, L.nrm.type, 2);
				const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
				else { nx = 0.0f; ny = 0.0f; nz = 1.0f; }
				o.normal[0] = nx; o.normal[1] = ny; o.normal[2] = nz;
				o.texcoord[0] = read_comp(p + L.uv.off, L.uv.type, 0);
				o.texcoord[1] = read_comp(p + L.uv.off, L.uv.type, 1);
				// White: the vertex colour still goes through the texture-op
				// modulate; the land tint rides in the tint texture instead.
				o.color = 0xFFFFFFFFu;
				if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2]))
				{
					reject = "non-finite POSITION";
					return false;
				}
			}

			// RetainedKeepCulled: land stays in the scene while culled, until
			// its cell leaves the loaded grid.
			set_bound(e, verts);
			e.residency = Residency::Cell;

			WeightBake wb;
			if (!bake_weights(g, L, layers, cfg.terrain_weight_res, wb, reject)) return false;

			// Layer textures. Albedo layers are mandatory; a normal layer that
			// can't be read just contributes the flat normal.
			uint64_t albedo[kMaxLayers] = {}, normal[kMaxLayers] = {};
			for (int i = 0; i < layers; i++)
			{
				albedo[i] = textures::acquire(ffp.cur_texture(static_cast<UINT>(i)), textures::Flavor::Unorm, reject);
				if (!albedo[i]) return false;
				e.textures.push_back(albedo[i]);
				// Layer 0 is the material's albedo, the texture the runtime's lists match.
				if (i == 0) note_hiding_list(e, ffp.cur_texture(0), "land quad");
				if (normals)
				{
					const char* nrej = nullptr;
					// NormalMap: gloss-in-alpha layers stay BC3 / BGRA8, alpha-less ones become BC1;
					// the runtime reads per-layer gloss from the upload format (no bridge field).
					normal[i] = textures::acquire(ffp.cur_texture(static_cast<UINT>(kNormalBase + i)), textures::Flavor::NormalMap, nrej);
					if (normal[i]) e.textures.push_back(normal[i]);
				}
			}

			// Height layers: optional, a layer without one (none shipped, still
			// loading, upload refused) is flat in the blended height field.
			uint64_t height[kMaxLayers] = {};
			e.height_on = heights_wanted();
			e.height_layers = 0;
			e.height_pending = false;
			if (e.height_on)
			{
				for (int i = 0; i < layers; i++)
				{
					bool pending = false;
					IDirect3DTexture9* h = layer_height(i, pending);
					if (pending) e.height_pending = true;
					if (!h) continue;
					const char* hrej = nullptr;
					height[i] = textures::acquire(h, textures::Flavor::Unorm, hrej);
					if (!height[i]) continue;
					e.textures.push_back(height[i]);
					e.height_layers++;
				}
			}

			const auto res = static_cast<uint32_t>(wb.res);
			const size_t tex_bytes = wb.w0.size();
			const uint64_t w0 = textures::acquire_generated(res, res, 1, wb.w0.data(), tex_bytes, REMIXAPI_FORMAT_R8G8B8A8_UNORM, reject);
			if (!w0) return false;
			e.textures.push_back(w0);
			uint64_t w1 = 0, tint = 0;
			if (wb.has_w1)
			{
				w1 = textures::acquire_generated(res, res, 1, wb.w1.data(), tex_bytes, REMIXAPI_FORMAT_R8G8B8A8_UNORM, reject);
				if (!w1) return false;
				e.textures.push_back(w1);
			}
			if (wb.has_tint)
			{
				tint = textures::acquire_generated(res, res, 1, wb.tint.data(), tex_bytes, REMIXAPI_FORMAT_R8G8B8A8_UNORM, reject);
				if (!tint) return false;
				e.textures.push_back(tint);
			}

			// Path strings must outlive CreateMaterial (the bridge client
			// serialises them during the call).
			std::wstring p_albedo[kMaxLayers], p_normal[kMaxLayers], p_height[kMaxLayers];
			for (int i = 0; i < layers; i++)
			{
				p_albedo[i] = texture_path(albedo[i]);
				if (normal[i]) p_normal[i] = texture_path(normal[i]);
				if (height[i]) p_height[i] = texture_path(height[i]);
			}
			const std::wstring p_w0 = texture_path(w0);
			const std::wstring p_w1 = w1 ? texture_path(w1) : std::wstring();
			const std::wstring p_tint = tint ? texture_path(tint) : std::wstring();

			remixapi_MaterialInfoTerrainLayersEXT terr = {};
			terr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TERRAIN_LAYERS_EXT;
			terr.pNext = nullptr;
			terr.layerCount = static_cast<uint32_t>(layers);
			for (int i = 0; i < REMIXAPI_TERRAIN_MAX_LAYERS; i++)
			{
				terr.albedoTextures[i] = i < layers ? p_albedo[i].c_str() : L"";
				terr.normalTextures[i] = i < layers ? p_normal[i].c_str() : L"";
				terr.heightTextures[i] = i < layers ? p_height[i].c_str() : L"";
			}
			terr.weightTextures[0] = p_w0.c_str();
			terr.weightTextures[1] = p_w1.c_str();
			terr.tintTexture = p_tint.c_str();
			terr.weightUvScale = wb.scale;
			terr.weightUvOffset = wb.offset;
			terr.normalsRgbTangentSpace = 1;

			// MaterialInfo -> OpaqueEXT -> TerrainLayersEXT: the runtime drops
			// the terrain extension unless OpaqueEXT is in the chain too.
			remixapi_MaterialInfoOpaqueEXT op;
			init_opaque_ext(op, cfg.terrain_roughness);
			op.pNext = &terr;
			if (e.height_layers)
			{
				// The depth of the blended layer heights (OpaqueEXT.heightTexture
				// stays empty: the runtime takes the layers' maps).
				op.displaceIn = rtxconf::scalars().height_displace_in;
				op.displaceOut = 0.0f;
			}

			remixapi_MaterialInfo mi;
			init_material_info(mi, next_object_hash(key, kTerrainDomain ^ 0x4D4154ull), &op);
			// Fallback for any path that ignores the extension; same UNORM
			// family as the layers.
			mi.albedoTexture = p_albedo[0].c_str();

			e.material = create_material(mi);
			if (!e.material) { reject = "CreateMaterial failed"; return false; }

			e.mesh = create_mesh(next_object_hash(key, kTerrainDomain), verts, g.tris, e.material,
				asset_keys::of(verts, g.tris));
			if (!e.mesh) { reject = "CreateMesh failed"; return false; }
			return true;
		}

		// MeshEntry::lod_sank.
		constexpr uint8_t kSankNone = 0, kSankAll = 1, kSankSome = 2;

		// The bounds and the sink verdict of a far-LOD block built with `consts`.
		void note_sink_bounds(MeshEntry& e, const float* consts)
		{
			const size_t n = e.lod_z.size();
			e.lod_sank = kSankSome;
			e.lod_g0 = consts[12];
			e.lod_g1 = consts[13];
			if (!n || e.lod_src.size() != n * 5) return;
			size_t sank = 0;
			for (size_t i = 0; i < n; i++)
			{
				const float* s = &e.lod_src[i * 5];
				const float v[4] = { s[0], s[1], lod_sink::morphed_z(consts, s[2], s[4]), s[3] };
				for (int k = 0; k < 4; k++)
				{
					if (!std::isfinite(v[k])) return;
					e.lod_lo[k] = i ? (std::min)(e.lod_lo[k], v[k]) : v[k];
					e.lod_hi[k] = i ? (std::max)(e.lod_hi[k], v[k]) : v[k];
				}
				if (lod_sink::inside(consts, v[0], v[1], v[2], v[3])) sank++;
			}
			e.lod_sank = sank == 0 ? kSankNone : sank == n ? kSankAll : kSankSome;
		}

		// Whether `consts` sinks no vertex of the block (kSankNone), every
		// vertex (kSankAll), or can't tell (kSankSome), from the vertex bounds.
		// The margin is far above the float rounding of the per-vertex test,
		// so a whole-block verdict holds for every vertex.
		uint8_t block_sinks(const MeshEntry& e, const float* c)
		{
			for (int k = 0; k < 14; k++)
				if (!std::isfinite(c[k])) return kSankSome;
			const float* h = c + 8;
			// |d| < h never holds for h <= 0.
			if (!(h[2] > 0.0f) || !(h[3] > 0.0f)) return kSankNone;
			bool outside = false, inside = true;
			for (int axis = 0; axis < 2; axis++)
			{
				const float* row = c + axis * 4;
				double lo = 0.0, hi = 0.0, mag = 0.0;
				for (int k = 0; k < 4; k++)
				{
					const double a = static_cast<double>(row[k]) * e.lod_lo[k];
					const double b = static_cast<double>(row[k]) * e.lod_hi[k];
					lo += (std::min)(a, b);
					hi += (std::max)(a, b);
					mag += (std::max)(std::fabs(a), std::fabs(b));
				}
				const double margin = 1e-3 * mag + 1e-3;
				const double centre = h[axis], half = h[2 + axis];
				if (hi + margin <= centre - half || lo - margin >= centre + half) outside = true;
				if (!(lo - margin > centre - half && hi + margin < centre + half)) inside = false;
			}
			return outside ? kSankNone : inside ? kSankAll : kSankSome;
		}

		// True when the sink constants would move any vertex of the entry by
		// more than a hundredth of a unit.
		bool sink_moves_vertices(const MeshEntry& e, const float* consts)
		{
			const size_t n = e.lod_z.size();
			if (e.lod_src.size() != n * 5) return true;
			// Whole-block verdicts: the same morph, and no vertex sinks now or
			// at the build (or every vertex, by the same depth), leave every
			// Z as built.
			if (e.lod_sank != kSankSome && consts[12] == e.lod_g0)
			{
				const uint8_t now = block_sinks(e, consts);
				if (now == kSankNone && e.lod_sank == kSankNone) return false;
				if (now == kSankAll && e.lod_sank == kSankAll && consts[13] == e.lod_g1) return false;
			}
			for (size_t i = 0; i < n; i++)
			{
				const float* s = &e.lod_src[i * 5];
				const float z = lod_sink::sunk_z(consts, s[0], s[1], s[2], s[3], s[4]);
				if (!(std::fabs(z - e.lod_z[i]) <= 0.01f)) return true;
			}
			return false;
		}

		// tc1 (the morph target) is missing from a block's lit pass.
		struct FarLodLayout
		{
			Attr pos, uv, tc1;
		};

		bool far_lod_decl(FarLodLayout& l)
		{
			const auto& ffp = shared::common::ffp_state::get();
			if (ffp.cur_decl_multi_stream()) return false;
			l.pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			l.uv  = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			l.tc1 = find_attr(D3DDECLUSAGE_TEXCOORD, 1);
			if (!l.pos.ok() || !is_pos_type(l.pos.type) || !l.uv.ok() || !l.tc1.ok()) return false;
			const UINT stride = ffp.stream_stride(0);
			return attr_fits(l.pos, stride) && attr_fits(l.uv, stride) && attr_fits(l.tc1, stride);
		}

		// The stream-0 element with this usage in `el`, or an invalid Attr.
		Attr find_in(const D3DVERTEXELEMENT9* el, UINT n, BYTE usage, BYTE usage_index)
		{
			for (UINT i = 0; i < n && el[i].Stream != 0xFF; i++)
				if (el[i].Stream == 0 && el[i].Usage == usage && el[i].UsageIndex == usage_index)
					return Attr{ static_cast<int>(el[i].Offset), static_cast<int>(el[i].Type) };
			return {};
		}

		bool layout_fits(FarLodLayout& l, UINT stride)
		{
			if (!l.pos.ok() || !is_pos_type(l.pos.type) || !l.uv.ok()) return false;
			if (l.tc1.ok() && !attr_fits(l.tc1, stride)) l.tc1 = {};
			return attr_fits(l.pos, stride) && attr_fits(l.uv, stride);
		}

		// A block's lit pass: POSITION and TEXCOORD0 in stream 0, TEXCOORD1
		// when present; other streams are ignored. The engine binds the lit
		// shader's declaration (72-byte vertices) over the block's own 24-byte
		// far-LOD vertices, so when the bound declaration does not fit the
		// stream, the layout comes from the declaration of the geometry's own
		// buffer.
		bool lit_lod_decl(FarLodLayout& l)
		{
			const auto& ffp = shared::common::ffp_state::get();
			const UINT stride = ffp.stream_stride(0);
			l.pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			l.uv  = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			l.tc1 = find_attr(D3DDECLUSAGE_TEXCOORD, 1);
			if (layout_fits(l, stride)) return true;

			scene_graph::GeoRead gr;
			uint32_t bd = 0;
			scene_graph::BufferRead b;
			if (!scene_graph::read_geo(scene_graph::current_geometry(), gr) || !gr.data ||
				!scene_graph::read_u32(gr.data + scene_graph::fnv::NGD_buffer, bd) || !bd ||
				!scene_graph::read_buffer(bd, b) || !b.decl || b.streams != 1 || b.stride[0] != stride) return false;
			D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
			UINT n = static_cast<UINT>(std::size(el));
			if (FAILED(b.decl->GetDeclaration(el, &n))) return false;
			l.pos = find_in(el, n, D3DDECLUSAGE_POSITION, 0);
			l.uv  = find_in(el, n, D3DDECLUSAGE_TEXCOORD, 0);
			l.tc1 = find_in(el, n, D3DDECLUSAGE_TEXCOORD, 1);
			return layout_fits(l, stride);
		}

		// The sink constants of the latest far-LOD draw, with ObjToCubeSpace's
		// translation less that draw's WORLD translation: the same for every
		// block in a frame, so a block drawn by another shader gets its own
		// ObjToCubeSpace from its WORLD.
		struct SinkRef
		{
			UINT frame = 0;
			bool ok = false;
			float c[lod_sink::kNumConstants] = {};
			float off[2] = {};
		};
		SinkRef g_sink_ref;
		// A lit pass can come before the frame's far-LOD draws.
		constexpr UINT kSinkRefFrames = 4;

		void note_sink_ref(const float* consts)
		{
			const float* w = world_matrix();
			// Rows 0-1 of ObjToCubeSpace are a pure translation for every block.
			if (!w || consts[0] != 1.0f || consts[1] != 0.0f || consts[2] != 0.0f ||
				consts[4] != 0.0f || consts[5] != 1.0f || consts[6] != 0.0f) return;
			std::memcpy(g_sink_ref.c, consts, sizeof(g_sink_ref.c));
			g_sink_ref.off[0] = consts[3] - w[12];
			g_sink_ref.off[1] = consts[7] - w[13];
			g_sink_ref.frame = frame();
			g_sink_ref.ok = true;
		}

		bool lit_sink_constants(float* out)
		{
			const float* w = world_matrix();
			if (!w || !g_sink_ref.ok || frame() - g_sink_ref.frame > kSinkRefFrames) return false;
			std::memcpy(out, g_sink_ref.c, sizeof(g_sink_ref.c));
			out[3] = w[12] + g_sink_ref.off[0];
			out[7] = w[13] + g_sink_ref.off[1];
			// GeomorphParams.x is per block: the lit blocks are the ones around
			// the loaded cells, which the engine draws fully morphed (POSITION.z,
			// not the coarser TEXCOORD1 heights).
			out[12] = 1.0f;
			return true;
		}

		bool lod_landscape_geometry()
		{
			const uintptr_t geo = scene_graph::current_geometry();
			scene_graph::GeoRead gr;
			scene_graph::ShadeFlags sf;
			return geo && scene_graph::read_geo(geo, gr) &&
				scene_graph::read_shade_flags(gr.prop(scene_graph::fnv::kPropShade), sf) &&
				(sf.flags2 & (1u << 1)) != 0;
		}

		// IEEE half of a finite float in 0..65504 (round to nearest).
		uint16_t to_half(float f)
		{
			uint32_t x;
			std::memcpy(&x, &f, sizeof(x));
			const uint16_t sign = static_cast<uint16_t>((x >> 16) & 0x8000u);
			const int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
			const uint32_t mant = x & 0x7FFFFFu;
			if (exp <= 0) return sign;
			if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
			uint32_t h = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
			if (mant & 0x1000u) h++;
			return static_cast<uint16_t>(sign | (h > 0x7BFFu ? 0x7C00u : h));
		}

		// The far-LOD shader's noise term as a tint over the whole block:
		// albedo *= LODLandNoise(uv * 1.75).r * 0.8 + 0.55. One texture for
		// every block (the noise and its tiling are global), with the tiling
		// baked in, since the tint is sampled clamped. The cache holds one
		// reference; a material using it adds its own.
		struct NoiseTint
		{
			IDirect3DBaseTexture9* source = nullptr;
			uint64_t hash = 0;
			bool failed = false;
		};
		NoiseTint g_noise;

		void update_noise_tint(const PsSlotMap* psmap)
		{
			if (!psmap || psmap->lod_noise_slot == PsSlotMap::kNoSlot) return;
			IDirect3DBaseTexture9* src = shared::common::ffp_state::get().cur_texture(psmap->lod_noise_slot);
			if (!src || src == g_noise.source) return;
			g_noise.source = src;
			if (g_noise.hash) { textures::release(g_noise.hash); g_noise.hash = 0; }

			uint32_t w = 0, h = 0, levels = 0;
			std::vector<uint8_t> px;
			const char* reject = nullptr;
			if (!textures::read_bgra8(src, w, h, levels, px, reject) || !w || !h)
			{
				if (!g_noise.failed)
					shared::common::log("RemixApi", std::format("farlod: the LOD noise texture cannot be read ({}): no noise",
						reject ? reject : "?"), shared::common::LOG_TYPE::LOG_TYPE_WARN);
				g_noise.failed = true;
				return;
			}
			constexpr float kTiles = 1.75f;
			const uint32_t ow = (std::max)(4u, static_cast<uint32_t>(std::lround(w * kTiles)));
			const uint32_t oh = (std::max)(4u, static_cast<uint32_t>(std::lround(h * kTiles)));
			// Top level: bilinear, wrapping, red channel (B8G8R8A8: byte 2).
			std::vector<float> f(static_cast<size_t>(ow) * oh);
			auto red = [&](uint32_t x, uint32_t y) { return px[(static_cast<size_t>(y % h) * w + x % w) * 4 + 2] / 255.0f; };
			for (uint32_t y = 0; y < oh; y++)
				for (uint32_t x = 0; x < ow; x++)
				{
					const float sx = (x + 0.5f) * w / ow * kTiles - 0.5f + w, sy = (y + 0.5f) * h / oh * kTiles - 0.5f + h;
					const uint32_t x0 = static_cast<uint32_t>(sx), y0 = static_cast<uint32_t>(sy);
					const float fx = sx - x0, fy = sy - y0;
					const float n = (red(x0, y0) * (1 - fx) + red(x0 + 1, y0) * fx) * (1 - fy) +
						(red(x0, y0 + 1) * (1 - fx) + red(x0 + 1, y0 + 1) * fx) * fy;
					f[static_cast<size_t>(y) * ow + x] = n * 0.8f + 0.55f;
				}
			// Mips by 2x2 average.
			std::vector<std::vector<float>> chain{ std::move(f) };
			std::vector<std::pair<uint32_t, uint32_t>> dims{ { ow, oh } };
			while (dims.back().first > 1 || dims.back().second > 1)
			{
				const auto [pw, ph] = dims.back();
				const uint32_t nw = (std::max)(1u, pw / 2), nh = (std::max)(1u, ph / 2);
				std::vector<float> m(static_cast<size_t>(nw) * nh);
				const auto& prev = chain.back();
				for (uint32_t y = 0; y < nh; y++)
					for (uint32_t x = 0; x < nw; x++)
					{
						float s = 0.0f;
						int n = 0;
						for (uint32_t dy = 0; dy < 2; dy++)
							for (uint32_t dx = 0; dx < 2; dx++)
							{
								const uint32_t sx = (std::min)(x * 2 + dx, pw - 1), sy = (std::min)(y * 2 + dy, ph - 1);
								s += prev[static_cast<size_t>(sy) * pw + sx];
								n++;
							}
						m[static_cast<size_t>(y) * nw + x] = s / n;
					}
				chain.push_back(std::move(m));
				dims.push_back({ nw, nh });
			}
			const uint32_t mips = static_cast<uint32_t>(chain.size());
			if (textures::half_float_supported())
			{
				std::vector<uint16_t> out;
				const uint16_t one = to_half(1.0f);
				for (const auto& lvl : chain)
					for (const float v : lvl) { const uint16_t hv = to_half(v); out.insert(out.end(), { hv, hv, hv, one }); }
				g_noise.hash = textures::acquire_generated_half(ow, oh, mips, out.data(), out.size() * sizeof(uint16_t), reject);
			}
			else
			{
				// Without float textures the brightening half of the noise is clipped at 1.
				std::vector<uint8_t> out;
				for (const auto& lvl : chain)
					for (const float v : lvl)
					{
						const uint8_t b = static_cast<uint8_t>(std::lround((std::min)(v, 1.0f) * 255.0f));
						out.insert(out.end(), { b, b, b, 255 });
					}
				g_noise.hash = textures::acquire_generated(ow, oh, mips, out.data(), out.size(), REMIXAPI_FORMAT_R8G8B8A8_UNORM,
					reject);
			}
			shared::common::log("RemixApi", g_noise.hash
				? std::format("farlod: LOD noise {}x{} baked into a {}x{} tint ({} tiles per block)", w, h, ow, oh, kTiles)
				: std::format("farlod: the LOD noise tint upload failed ({})", reject ? reject : "?"),
				g_noise.hash ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		}

		// A block's normal map in the runtime's tangent space. The game's LOD
		// normal maps are world space (the far-LOD shader dots them with the
		// light direction as it is), and the block's UVs run along X and Y.
		// With every vertex normal straight up, the runtime's tangent frame
		// (genTangSpace) is (+-X, +-Y, +Z) by the UVs' directions, so the map
		// becomes tangent space by flipping X and Y where u / v run against
		// them. Converted maps are cached by source and flips; the cache holds
		// one reference each.
		struct LodNormal
		{
			uint64_t hash = 0;
			UINT used = 0;
		};
		std::unordered_map<uint64_t, LodNormal> g_lod_normals;
		constexpr size_t kLodNormalCache = 96;

		uint64_t lod_normal_map(IDirect3DBaseTexture9* tex, const std::vector<remixapi_HardcodedVertex>& verts,
			const char*& reject)
		{
			// UV directions against X / Y.
			const size_t n = verts.size();
			if (n < 3) { reject = "no vertices"; return 0; }
			double mu = 0, mv = 0, mx = 0, my = 0;
			for (const auto& v : verts) { mu += v.texcoord[0]; mv += v.texcoord[1]; mx += v.position[0]; my += v.position[1]; }
			mu /= n; mv /= n; mx /= n; my /= n;
			double ux = 0, uy = 0, vx = 0, vy = 0;
			for (const auto& v : verts)
			{
				const double du = v.texcoord[0] - mu, dv = v.texcoord[1] - mv, dx = v.position[0] - mx, dy = v.position[1] - my;
				ux += du * dx; uy += du * dy; vx += dv * dx; vy += dv * dy;
			}
			if (!(std::fabs(ux) > 4.0 * std::fabs(uy)) || !(std::fabs(vy) > 4.0 * std::fabs(vx)))
			{
				reject = "UVs not along X / Y";
				return 0;
			}
			const bool flip_x = ux < 0, flip_y = vy < 0;

			const uint64_t src = textures::acquire(tex, textures::Flavor::NormalMapNoGloss, reject);
			if (!src || (!flip_x && !flip_y)) return src;

			const uint64_t ck = src ^ (flip_x ? 0x9E3779B97F4A7C15ull : 0) ^ (flip_y ? 0xC2B2AE3D27D4EB4Full : 0);
			if (auto it = g_lod_normals.find(ck); it != g_lod_normals.end())
			{
				textures::release(src);
				textures::add_ref(it->second.hash);
				it->second.used = frame();
				return it->second.hash;
			}

			uint32_t w = 0, h = 0, levels = 0;
			std::vector<uint8_t> px;
			const bool read = textures::read_bgra8(tex, w, h, levels, px, reject);
			textures::release(src);
			if (!read) return 0;
			for (size_t i = 0; i + 3 < px.size(); i += 4)
			{
				if (flip_x) px[i + 2] = static_cast<uint8_t>(255 - px[i + 2]);   // R: X
				if (flip_y) px[i + 1] = static_cast<uint8_t>(255 - px[i + 1]);   // G: Y
				px[i + 3] = 255;
			}
			const uint64_t conv = textures::acquire_generated_bc1(w, h, levels, px, reject);
			if (!conv) return 0;

			if (g_lod_normals.size() >= kLodNormalCache)
			{
				auto oldest = g_lod_normals.begin();
				for (auto it = g_lod_normals.begin(); it != g_lod_normals.end(); ++it)
					if (it->second.used < oldest->second.used) oldest = it;
				textures::release(oldest->second.hash);
				g_lod_normals.erase(oldest);
			}
			textures::add_ref(conv);
			g_lod_normals[ck] = { conv, frame() };
			static bool s_logged = false;
			if (!s_logged)
			{
				s_logged = true;
				shared::common::log("RemixApi", std::format("farlod: block normal maps converted to tangent space ({}x{}, "
					"X {}, Y {})", w, h, flip_x ? "flipped" : "kept", flip_y ? "flipped" : "kept"),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
			return conv;
		}

		void release_lod_detail()
		{
			for (auto& kv : g_lod_normals) textures::release(kv.second.hash);
			g_lod_normals.clear();
			if (g_noise.hash) textures::release(g_noise.hash);
			g_noise = {};
		}

		// `from`: the live entry being rebuilt, or null. When the new build keeps
		// its triangles, its mesh is refreshed in place and `refreshed` is set:
		// `e` then holds only the geometry fields, and `from` keeps its handles.
		bool build_far_lod(IDirect3DDevice9* dev, const DrawDesc& d, const FarLodLayout& L,
			IDirect3DBaseTexture9* albedo_tex, IDirect3DBaseTexture9* normal_tex, const float* consts, uint64_t key,
			const MeshEntry* from, MeshEntry& e, bool& refreshed, const char*& reject)
		{
			refreshed = false;
			PROFILE_ZONE_N("api::farlod::build");
			const auto& cfg = shared::common::config::get().remix_api;

			Geometry g;
			if (!read_geometry(dev, d, g, reject)) return false;

			std::vector<remixapi_HardcodedVertex> verts(g.count);
			e.lod_src.resize(static_cast<size_t>(g.count) * 5);
			e.lod_z.resize(g.count);
			for (UINT i = 0; i < g.count; i++)
			{
				const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride;
				const float px = read_comp(p + L.pos.off, L.pos.type, 0);
				const float py = read_comp(p + L.pos.off, L.pos.type, 1);
				const float pz = read_comp(p + L.pos.off, L.pos.type, 2);
				const float pw = read_comp(p + L.pos.off, L.pos.type, 3);
				const float coarse = L.tc1.ok() ? lod_sink::read_coarse_z(p + L.tc1.off, L.tc1.type) : pz;
				auto& o = verts[i];
				o = {};
				o.position[0] = px;
				o.position[1] = py;
				o.position[2] = lod_sink::sunk_z(consts, px, py, pz, pw, coarse);
				float* src = &e.lod_src[static_cast<size_t>(i) * 5];
				src[0] = px; src[1] = py; src[2] = pz; src[3] = pw; src[4] = coarse;
				e.lod_z[i] = o.position[2];
				if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2]))
				{
					reject = "non-finite POSITION";
					return false;
				}
				// SLS2003's half-texel inset of the block texture.
				o.texcoord[0] = read_comp(p + L.uv.off, L.uv.type, 0) * 0.9921875f + 0.00390625f;
				o.texcoord[1] = read_comp(p + L.uv.off, L.uv.type, 1) * 0.9921875f + 0.00390625f;
				o.color = 0xFFFFFFFFu;
			}

			// The game draws the LOD under a BSClearZNode: all of it first, then
			// the depth buffer is cleared, so the loaded terrain always covers it
			// and the sink only has to close the seam. A path tracer has no draw
			// order, and a coarse block rises above steep loaded terrain by more
			// than the sink. Triangles with all three vertices inside the loaded
			// area are left out; those across its edge stay, sunk.
			{
				std::vector<uint8_t> inside(g.count);
				for (UINT i = 0; i < g.count; i++)
				{
					const float* src = &e.lod_src[static_cast<size_t>(i) * 5];
					inside[i] = lod_sink::inside(consts, src[0], src[1], lod_sink::morphed_z(consts, src[2], src[4]), src[3])
						? 1 : 0;
				}
				std::vector<uint32_t> kept;
				kept.reserve(g.tris.size());
				for (size_t t = 0; t + 2 < g.tris.size(); t += 3)
				{
					const uint32_t a = g.tris[t], b = g.tris[t + 1], c = g.tris[t + 2];
					if (inside[a] && inside[b] && inside[c]) continue;
					kept.insert(kept.end(), { a, b, c });
				}
				g.tris.swap(kept);
			}
			if (g.tris.empty())
			{
				// Wholly inside the loaded area: nothing to draw until the sink
				// constants change and the block is rebuilt.
				set_bound(e, verts);
				e.residency = Residency::LodOnScreen;
				std::memcpy(e.consts, consts, sizeof(e.consts));
				note_sink_bounds(e, consts);
				return true;
			}

			// Area-weighted face normals, each oriented to +Z: the block is a
			// height field in object space, whatever its triangle winding.
			std::vector<float> acc(static_cast<size_t>(g.count) * 3, 0.0f);
			for (size_t t = 0; t + 2 < g.tris.size(); t += 3)
			{
				const float* a = verts[g.tris[t]].position;
				const float* b = verts[g.tris[t + 1]].position;
				const float* c = verts[g.tris[t + 2]].position;
				const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
				const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
				float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
				if (n[2] < 0.0f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
				for (int k = 0; k < 3; k++)
					for (int j = 0; j < 3; j++)
						acc[static_cast<size_t>(g.tris[t + k]) * 3 + j] += n[j];
			}
			for (UINT i = 0; i < g.count; i++)
			{
				float* n = &acc[static_cast<size_t>(i) * 3];
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				auto& o = verts[i];
				if (len > 1e-12f) { o.normal[0] = n[0] / len; o.normal[1] = n[1] / len; o.normal[2] = n[2] / len; }
				else { o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f; }
			}

			// RetainedKeepCulled: a block stays in the scene while culled and
			// is hidden only once it is in view and the engine stops drawing
			// it (its cells came in at full detail, or a finer block took it).
			set_bound(e, verts);
			e.residency = Residency::LodOnScreen;
			e.lod_tris = g.tris;

			// A morph step or a sink that moves no triangle in or out: the same
			// mesh with new positions, so the block keeps its motion history.
			if (from && from->mesh && from->material && from->lod_tris == g.tris)
			{
				// The detail normal map overrides the vertex normals (see below).
				if (from->lod_flat_normals)
					for (auto& v : verts) { v.normal[0] = 0.0f; v.normal[1] = 0.0f; v.normal[2] = 1.0f; }
				if (refresh_mesh(from->mesh_hash, from->mesh, verts, g.tris, from->material))
				{
					refreshed = true;
					std::memcpy(e.consts, consts, sizeof(e.consts));
					note_sink_bounds(e, consts);
					return true;
				}
			}

			// The game's far-LOD detail: the block's normal map (with every
			// vertex normal straight up, see lod_normal_map) and the noise tint.
			uint64_t normal = 0, tint = 0;
			if (cfg.far_lod_detail)
			{
				const char* nrej = nullptr;
				if (normal_tex) normal = lod_normal_map(normal_tex, verts, nrej);
				if (normal)
				{
					e.textures.push_back(normal);
					for (auto& v : verts) { v.normal[0] = 0.0f; v.normal[1] = 0.0f; v.normal[2] = 1.0f; }
					e.lod_flat_normals = true;
				}
				if (g_noise.hash)
				{
					tint = g_noise.hash;
					textures::add_ref(tint);
					e.textures.push_back(tint);
				}
			}
			const bool layered = normal || tint;

			// A layered material blends in the textures' own encoding and
			// linearises once, so its albedo goes up UNORM.
			const uint64_t albedo = textures::acquire(albedo_tex, layered ? textures::Flavor::Unorm : textures::Flavor::Srgb,
				reject);
			if (!albedo) return false;
			e.textures.push_back(albedo);
			note_hiding_list(e, albedo_tex, "farlod block");
			const std::wstring p_albedo = texture_path(albedo);
			const std::wstring p_normal = normal ? texture_path(normal) : std::wstring();
			const std::wstring p_tint = tint ? texture_path(tint) : std::wstring();
			std::wstring p_weight;

			remixapi_MaterialInfoOpaqueEXT op;
			init_opaque_ext(op, cfg.far_lod_roughness);
			remixapi_MaterialInfoTerrainLayersEXT layer = {};
			if (layered)
			{
				// One layer at weight 1 (a 1x1 weight texture); the tint is
				// sampled at the mesh UV.
				static const uint8_t kOneLayer[4] = { 255, 0, 0, 0 };
				const uint64_t wt = textures::acquire_generated(1, 1, 1, kOneLayer, sizeof(kOneLayer),
					REMIXAPI_FORMAT_R8G8B8A8_UNORM, reject);
				if (!wt) return false;
				e.textures.push_back(wt);
				p_weight = texture_path(wt);
				layer.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TERRAIN_LAYERS_EXT;
				layer.pNext = nullptr;
				layer.layerCount = 1;
				for (int i = 0; i < REMIXAPI_TERRAIN_MAX_LAYERS; i++)
				{
					layer.albedoTextures[i] = i == 0 ? p_albedo.c_str() : L"";
					layer.normalTextures[i] = i == 0 ? p_normal.c_str() : L"";
					layer.heightTextures[i] = L"";
				}
				layer.weightTextures[0] = p_weight.c_str();
				layer.weightTextures[1] = L"";
				layer.tintTexture = p_tint.c_str();
				layer.weightUvScale = remixapi_Float2D{ 1.0f, 1.0f };
				layer.weightUvOffset = remixapi_Float2D{ 0.0f, 0.0f };
				layer.normalsRgbTangentSpace = 1;
				op.pNext = &layer;
			}
			remixapi_MaterialInfo mi;
			init_material_info(mi, next_object_hash(key, kFarLodDomain ^ 0x4D4154ull), &op);
			mi.albedoTexture = p_albedo.c_str();

			e.material = create_material(mi);
			if (!e.material) { reject = "CreateMaterial failed"; return false; }
			// Keyed by the positions before the sink, which constant changes move.
			e.mesh_hash = next_object_hash(key, kFarLodDomain);
			e.mesh = create_mesh(e.mesh_hash, verts, g.tris, e.material,
				asset_keys::of(e.lod_src.data(), 5, g.count, g.tris.data(), g.tris.size()));
			if (!e.mesh) { reject = "CreateMesh failed"; return false; }

			std::memcpy(e.consts, consts, sizeof(e.consts));
			note_sink_bounds(e, consts);
			return true;
		}

		static_assert(sizeof(MeshEntry::consts) == lod_sink::kNumConstants * sizeof(float));

		// A rebuilt block's old objects. Destroying them in the middle of a
		// frame would pull the block out of the frame being traced, while its
		// replacement is first drawn by the next frame's replay: a one-frame
		// hole. The old instance is hidden at that replay instead, and its
		// objects are destroyed once that replay has run.
		struct Retired
		{
			uint64_t retained = 0;
			remixapi_MeshHandle mesh = nullptr;
			remixapi_MaterialHandle material = nullptr;
			std::vector<uint64_t> textures;
			UINT frame = 0;
		};
		std::vector<Retired> g_retired;

		void retire_entry(MeshEntry& e)
		{
			Retired r;
			r.retained = retained::retire(e);
			r.mesh = e.mesh;
			r.material = e.material;
			r.textures = std::move(e.textures);
			r.frame = frame();
			e.mesh = nullptr;
			e.material = nullptr;
			e.textures.clear();
			g_retired.push_back(std::move(r));
		}

		// Two frames on, the replay that hid them has run.
		void destroy_retired(bool all)
		{
			const UINT now = frame();
			auto* b = bridge();
			std::erase_if(g_retired, [&](Retired& r) {
				if (!all && now - r.frame < 2) return false;
				retained::destroy_handle(r.retained);
				if (b)
				{
					if (r.mesh) b->DestroyMesh(r.mesh);
					if (r.material) b->DestroyMaterial(r.material);
				}
				for (const uint64_t t : r.textures) textures::release(t);
				return true;
			});
		}

		bool submit_far_lod(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d, const FarLodLayout& L,
			const float (&consts)[lod_sink::kNumConstants])
		{
			const auto& cfg = shared::common::config::get().remix_api;
			auto& st = far_lod_stats();
			const auto& ffp = shared::common::ffp_state::get();
			const uint8_t diffuse = psmap->slot(PsSlotRole::Diffuse);
			IDirect3DBaseTexture9* albedo = (diffuse != PsSlotMap::kNoSlot) ? ffp.cur_texture(diffuse) : nullptr;
			if (!albedo) { st.fallbacks++; note_failure(st, "no BaseMap bound"); return true; }

			const char* reject = nullptr;
			uint64_t geo = 0;
			if (!geo_key(dev, d, geo, reject))
			{
				st.fallbacks++;
				note_failure(st, reject);
				return true;
			}
			const uint8_t normal_slot = psmap->slot(PsSlotRole::Normal);
			IDirect3DBaseTexture9* normal_tex = normal_slot != PsSlotMap::kNoSlot ? ffp.cur_texture(normal_slot) : nullptr;
			if (cfg.far_lod_detail) update_noise_tint(psmap);
			Hash64 kh;
			kh.mix(geo);
			kh.ptr(albedo);
			// A detail change builds the block again.
			kh.ptr(cfg.far_lod_detail ? normal_tex : nullptr);
			kh.mix(cfg.far_lod_detail ? g_noise.hash : 0);
			const uint64_t key = kh.get();

			const UINT now = frame();
			if (auto it = g_far_lod.find(key); it != g_far_lod.end())
			{
				auto& e = it->second;
				// The sink moves whenever the loaded-cell rectangle or the morph
				// changes. Rebuild at most once per FarLodRebuildFrames; the old
				// mesh keeps drawing until the new one exists.
				if (std::memcmp(e.consts, consts, sizeof(consts)) != 0 &&
					now - e.built_frame >= static_cast<UINT>(cfg.far_lod_rebuild_frames) &&
					!sink_moves_vertices(e, consts))
				{
					// HighDetailRange moves on every cell crossing, but a block
					// outside the loaded rectangle before and after keeps every Z.
					std::memcpy(e.consts, consts, sizeof(consts));
				}
				if (std::memcmp(e.consts, consts, sizeof(consts)) != 0 &&
					now - e.built_frame >= static_cast<UINT>(cfg.far_lod_rebuild_frames) &&
					take_build_budget(key))
				{
					MeshEntry ne;
					bool refreshed = false;
					if (build_far_lod(dev, d, L, albedo, normal_tex, consts, key, &e, ne, refreshed, reject))
					{
						if (refreshed)
						{
							// Same mesh, handles and retained instance: only the
							// geometry fields change.
							std::memcpy(e.consts, ne.consts, sizeof(e.consts));
							e.lod_src = std::move(ne.lod_src);
							e.lod_z = std::move(ne.lod_z);
							std::memcpy(e.lod_lo, ne.lod_lo, sizeof(e.lod_lo));
							std::memcpy(e.lod_hi, ne.lod_hi, sizeof(e.lod_hi));
							e.lod_sank = ne.lod_sank;
							e.lod_g0 = ne.lod_g0;
							e.lod_g1 = ne.lod_g1;
							e.has_bound = ne.has_bound;
							std::memcpy(e.bound_c, ne.bound_c, sizeof(e.bound_c));
							e.bound_r = ne.bound_r;
							e.built_frame = now;
							st.refreshed++;
						}
						else
						{
							ne.geo = geo;
							ne.built_frame = now;
							ne.last_frame = e.last_frame;
							retire_entry(e);
							e = std::move(ne);
							st.created++;
						}
					}
					else
					{
						destroy_entry(ne);
						e.built_frame = now;   // don't retry every frame
						note_failure(st, reject, "rebuild");
					}
				}
				// A block wholly inside the loaded area has no mesh.
				if (!e.mesh) { e.last_frame = now; return true; }
				if (!draw_entry(e, REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN, double_sided_for_cull(dev), st))
				{
					st.fallbacks++;
					return true;
				}
				return true;
			}

			if (retry_blocked(key) || !take_build_budget(key)) { st.fallbacks++; return true; }

			MeshEntry e;
			bool refreshed = false;
			if (!build_far_lod(dev, d, L, albedo, normal_tex, consts, key, nullptr, e, refreshed, reject))
			{
				destroy_entry(e);
				block_retry(key);
				st.fallbacks++;
				note_failure(st, reject);
				return true;
			}
			e.geo = geo;
			e.built_frame = now;
			auto& ins = g_far_lod[key];
			ins = std::move(e);
			st.created++;
			st.live = static_cast<uint32_t>(g_far_lod.size());
			note_success(st, std::format("PS 0x{:08X}, {} verts, sink c19.y={:.1f}",
				psmap->ps_hash, d.num_vtx, consts[13]));

			if (!ins.mesh) { ins.last_frame = now; return true; }
			if (!draw_entry(ins, REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN, double_sided_for_cull(dev), st))
			{
				st.fallbacks++;
				return true;
			}
			return true;
		}
	}

	// ---- Entry points ------------------------------------------------------------

	bool try_terrain(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_terrain");
		const auto& cfg = shared::common::config::get().remix_api;
		int layers = 0;
		bool normals = false;
		LandLayout L;
		// The engine's LAND* pass says whether this is land; the layer count
		// comes from the PS's BaseMap[N] / NormalMap[N] layout.
		const bool layout = land_shader(psmap, layers, normals);
		if (!engine_class::land(layout) || !land_decl(L)) return false;
		if (!layout)
		{
			if (!g_logged_no_layout)
			{
				g_logged_no_layout = true;
				shared::common::log("RemixApi", std::format(
					"terrain: a land pass drawn with PS 0x{:08X}, whose sampler layout is not BaseMap[N] / NormalMap[N]: "
					"no layer count, so it goes to draw capture", psmap ? psmap->ps_hash : 0u),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
			return false;
		}

		auto& st = terrain_stats();
		if (!bridge()) { st.fallbacks++; st.last_reject = "Remix API unavailable"; return true; }

		const char* reject = nullptr;
		uint64_t geo = 0;
		if (!geo_key(dev, d, geo, reject))
		{
			st.fallbacks++;
			note_failure(st, reject);
			return true;
		}

		const auto& ffp = shared::common::ffp_state::get();
		Hash64 kh;
		kh.mix(geo);
		kh.mix(static_cast<uint64_t>(layers) | (normals ? 0x100ull : 0ull));
		for (int i = 0; i < layers; i++)
		{
			kh.ptr(ffp.cur_texture(static_cast<UINT>(i)));
			if (normals) kh.ptr(ffp.cur_texture(static_cast<UINT>(kNormalBase + i)));
		}
		const uint64_t key = kh.get();

		const UINT now = frame();
		if (g_land_drawn_frame != now) { g_land_drawn.clear(); g_land_drawn_frame = now; }

		// The same land geometry already went out this frame with a different
		// layer set: a follow-up pass of a quad carrying more layers than one
		// pass binds. The first pass's blend is what the path tracer gets;
		// drawing a second co-planar mesh would only z-fight it. Checked before
		// the cache lookup, so an entry built for that layer set in an earlier
		// frame isn't drawn over it either.
		if (const auto g = g_land_drawn.find(geo); g != g_land_drawn.end() && g->second != key)
		{
			st.skipped++;
			if (!g_logged_extra_pass)
			{
				g_logged_extra_pass = true;
				shared::common::log("RemixApi", std::format(
					"terrain: dropping an extra layer pass (PS 0x{:08X}, {} layers) over land already submitted this frame",
					psmap->ps_hash, layers), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
			return true;
		}

		if (auto it = g_terrain.find(key); it != g_terrain.end())
		{
			auto& e = it->second;
			// Height maps: rebuild once a lookup that was pending at build has
			// finished, or when TerrainHeightMaps / the POM depth changed.
			const bool want_height = heights_wanted();
			const bool stale = e.height_on != want_height ||
				(e.height_pending && now - e.built_frame >= kHeightRecheckFrames && !any_height_pending(layers));
			if (e.height_pending && !stale && now - e.built_frame >= kHeightRecheckFrames)
				e.built_frame = now;   // still loading: look again in kHeightRecheckFrames
			if (stale && take_build_budget(key))
			{
				MeshEntry ne;
				if (build_terrain(dev, d, layers, normals, L, key, ne, reject))
				{
					ne.geo = geo;
					ne.built_frame = now;
					ne.last_frame = e.last_frame;
					destroy_entry(e);
					e = std::move(ne);
					st.created++;
				}
				else
				{
					// Keep the old entry; don't retry until the next change.
					destroy_entry(ne);
					e.built_frame = now;
					e.height_on = want_height;
					e.height_pending = false;
					note_failure(st, reject, "height rebuild");
				}
			}
			if (!draw_entry(e, REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN, double_sided_for_cull(dev), st))
			{
				st.fallbacks++;
				return true;
			}
			g_land_drawn[geo] = key;
			return true;
		}

		if (retry_blocked(key) || !take_build_budget(key)) { st.fallbacks++; return true; }

		MeshEntry e;
		if (!build_terrain(dev, d, layers, normals, L, key, e, reject))
		{
			destroy_entry(e);
			block_retry(key);
			st.fallbacks++;
			note_failure(st, reject, std::format("PS 0x{:08X}, {} layers", psmap->ps_hash, layers));
			return true;
		}
		e.geo = geo;
		e.built_frame = now;
		auto& ins = g_terrain[key];
		ins = std::move(e);
		st.created++;
		st.live = static_cast<uint32_t>(g_terrain.size());
		note_success(st, std::format("PS 0x{:08X}, {} layers{}, {} verts, {} prims, weights {}x{}, {} height layers{}",
			psmap->ps_hash, layers, normals ? " + normals" : "", d.num_vtx, d.prim_count,
			cfg.terrain_weight_res, cfg.terrain_weight_res, ins.height_layers,
			ins.height_pending ? " (some still loading)" : ""));

		if (!draw_entry(ins, REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN, double_sided_for_cull(dev), st))
		{
			st.fallbacks++;
			return true;
		}
		g_land_drawn[geo] = key;
		return true;
	}

	bool try_far_lod(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_far_lod");
		if (!psmap) return false;

		auto& st = far_lod_stats();
		if (!bridge()) { st.fallbacks++; st.last_reject = "Remix API unavailable"; return true; }

		FarLodLayout L;
		if (!far_lod_decl(L))
		{
			st.fallbacks++;
			note_failure(st, "declaration lacks POSITION / TEXCOORD0 / TEXCOORD1");
			return true;
		}

		float consts[lod_sink::kNumConstants];
		lod_sink::gather_constants(consts);
		note_sink_ref(consts);
		// GeomorphParams.x slides with the camera's distance, and every
		// change rebuilds the block, which swaps its mesh while the player
		// moves (flicker). It is snapped to quarters, so a block rebuilds
		// only when its morph crosses a step or the sink moves.
		constexpr float kMorphSteps = 4.0f;
		consts[12] = std::round(std::clamp(consts[12], 0.0f, 1.0f) * kMorphSteps) / kMorphSteps;
		return submit_far_lod(dev, psmap, d, L, consts);
	}

	bool is_lod_block_draw() { return lod_landscape_geometry(); }

	void try_lit_lod_block(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_lit_lod_block");
		if (!psmap) return;

		auto& st = far_lod_stats();
		if (!bridge()) { st.fallbacks++; st.last_reject = "Remix API unavailable"; return; }
		FarLodLayout L;
		float consts[lod_sink::kNumConstants];
		const char* why = !lit_lod_decl(L) ? "declaration lacks POSITION / TEXCOORD0"
			: !lit_sink_constants(consts) ? "no far-LOD draw to take the sink constants from" : nullptr;
		if (why)
		{
			static bool s_logged = false;
			if (!s_logged)
			{
				s_logged = true;
				const auto& ffp = shared::common::ffp_state::get();
				std::string decl;
				for (UINT i = 0; i < ffp.cur_decl_num_elements(); i++)
				{
					const auto& el = ffp.cur_decl_elements()[i];
					decl += std::format("{}s{} u{}.{} t{} @{}", decl.empty() ? "" : ", ", el.Stream, el.Usage,
						el.UsageIndex, el.Type, el.Offset);
				}
				// The engine's buffer for the geometry, against the bound state.
				std::string buf = "no buffer";
				scene_graph::GeoRead gr;
				uint32_t bd = 0;
				scene_graph::BufferRead b;
				if (scene_graph::read_geo(scene_graph::current_geometry(), gr) && gr.data &&
					scene_graph::read_u32(gr.data + scene_graph::fnv::NGD_buffer, bd) && bd && scene_graph::read_buffer(bd, b))
					buf = std::format("{} streams, strides {} {}, decl {}, fvf 0x{:X}", b.streams, b.stride[0], b.stride[1],
						b.decl == ffp.last_decl() ? "bound" : "another", b.fvf);
				shared::common::log("RemixApi", std::format("farlod: a lit LOD block (PS 0x{:08X}) is not drawn: {} "
					"[decl {}] stream strides {} {} multiStream {} | engine buffer: {}", psmap->ps_hash, why, decl,
					ffp.stream_stride(0), ffp.stream_stride(1), ffp.cur_decl_multi_stream() ? 1 : 0, buf),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
			st.fallbacks++;
			note_failure(st, why);
			return;
		}
		if (!g_logged_lit_block)
		{
			g_logged_lit_block = true;
			shared::common::log("RemixApi", std::format("farlod: LOD blocks drawn by a lit pass (PS 0x{:08X}{}) go "
				"through the far-LOD path", psmap->ps_hash, L.tc1.ok() ? "" : ", no TEXCOORD1"),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
		submit_far_lod(dev, psmap, d, L, consts);
	}

	namespace
	{
		std::atomic<bool> g_dump_requested{ false };

		// [LodTrace] For two frames after a dump: every draw of a geometry
		// whose shade property has LOD_Landscape (flags2 bit 1) or whose pass
		// is a land-LOD pass, with the routes it took, then the scene-graph
		// subtree above those geometries with each object's cull flag.
		constexpr uint32_t kSF2_lodLandscape = 1u << 1;
		constexpr int kTraceFrames = 2;
		int g_trace_frames = 0;
		struct Traced
		{
			std::string routes;
			std::string passes;
			uint32_t flags2 = 0;
			bool lod_land = false;
		};
		std::unordered_map<uintptr_t, Traced> g_traced;

		std::string object_name(uintptr_t obj)
		{
			uint32_t p = 0;
			if (!scene_graph::read_u32(obj + 0x08, p) || !p) return {};
			std::string s;
			for (uint32_t i = 0; i < 63; i++)
			{
				uint8_t c = 0;
				if (!scene_graph::read_u8(p + i, c) || !c) break;
				s.push_back(static_cast<char>(c));
			}
			return s;
		}

		bool names_lod(const std::string& s)
		{
			for (size_t i = 0; i + 3 <= s.size(); i++)
				if (std::toupper(static_cast<unsigned char>(s[i])) == 'L' &&
					std::toupper(static_cast<unsigned char>(s[i + 1])) == 'O' &&
					std::toupper(static_cast<unsigned char>(s[i + 2])) == 'D') return true;
			return false;
		}

		std::string describe(uintptr_t obj)
		{
			const auto* ci = scene_graph::class_of(obj);
			scene_graph::AvRead av;
			const bool ok = scene_graph::read_av(obj, av);
			const float* cam = camera_position_ptr;
			std::string out = std::format("0x{:08X} {} '{}'", obj, ci ? ci->name : "?", object_name(obj));
			if (ok)
			{
				out += std::format(" pos ({:.0f}, {:.0f}, {:.0f})", av.pos[0], av.pos[1], av.pos[2]);
				if (cam) out += std::format(" rel ({:.0f}, {:.0f})", av.pos[0] - cam[0], av.pos[1] - cam[1]);
				out += std::format(" flags 0x{:X}{}", av.flags, av.app_culled() ? " APP_CULLED" : "");
			}
			if (const auto it = g_traced.find(obj); it != g_traced.end())
				out += std::format(" | drawn via {} pass {} flags2 0x{:X}", it->second.routes, it->second.passes,
					it->second.flags2);
			else if (ci && ci->geometry)
				out += " | not drawn";
			return out;
		}

		void walk_lod(uintptr_t node, int depth, int& budget)
		{
			if (budget <= 0 || depth > 8) return;
			budget--;
			shared::common::log_event("LodTrace", std::string(static_cast<size_t>(depth) * 2, ' ') + describe(node),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
			const auto* ci = scene_graph::class_of(node);
			if (!ci || !ci->node) return;
			uintptr_t kids[256];
			scene_graph::NodeChildren info;
			if (!scene_graph::read_children(node, kids, 256, info)) return;
			for (uint32_t i = 0; i < info.read; i++)
				if (kids[i]) walk_lod(kids[i], depth + 1, budget);
		}

		void finish_trace()
		{
			int lod_land = 0;
			for (const auto& kv : g_traced) lod_land += kv.second.lod_land ? 1 : 0;
			const float* cam = camera_position_ptr;
			shared::common::log_event("LodTrace", std::format("{} land-LOD geometries drawn in {} frames ({} with "
				"LOD_Landscape); camera ({:.0f}, {:.0f}, {:.0f})", g_traced.size(), kTraceFrames, lod_land,
				cam ? cam[0] : 0.0f, cam ? cam[1] : 0.0f, cam ? cam[2] : 0.0f), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);

			// The roots: the highest ancestor named "...LOD..." above each
			// traced geometry (its grandparent when none is named so).
			std::vector<uintptr_t> roots;
			for (const auto& kv : g_traced)
			{
				uintptr_t top = 0, cur = kv.first, chain[12] = {};
				int n = 0;
				for (; n < 12 && cur; n++)
				{
					chain[n] = cur;
					uintptr_t parent = 0;
					uint32_t flags = 0;
					if (!scene_graph::read_link(cur, parent, flags)) break;
					cur = parent;
				}
				for (int i = 1; i < n; i++)
					if (names_lod(object_name(chain[i]))) top = chain[i];
				if (!top) top = n > 2 ? chain[2] : n > 1 ? chain[1] : 0;
				if (top && std::find(roots.begin(), roots.end(), top) == roots.end()) roots.push_back(top);
			}
			int budget = 3000;
			for (const uintptr_t r : roots)
			{
				uintptr_t parent = 0;
				uint32_t flags = 0;
				scene_graph::read_link(r, parent, flags);
				shared::common::log_event("LodTrace", "root " + describe(r) + (parent ? " under " + describe(parent) : ""),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
				walk_lod(r, 1, budget);
			}
			g_traced.clear();
		}

		// Far-LOD vertices whose final Z is at or below this: the LOD meshes'
		// filler height (-14098 in the source positions and TEXCOORD1).
		constexpr float kLodFillerZ = -10000.0f;

		// [TerrainDump] Every land quad and far-LOD block the game drew in the
		// last two frames, and the far-LOD blocks alive but not drawn: the
		// bound centre relative to the camera (FNV units, x / y / z), its
		// distance on the ground and its radius. Far-LOD blocks also give
		// whether the loaded-area sink lowered none, all or some of their
		// vertices, the morph (GeomorphParams.x), and the vertices whose final
		// Z is the filler height, with their centre relative to the camera.
		// Written in full: a mark asks for it.
		void dump_terrain()
		{
			const UINT now = frame();
			const double s = stable_origin::scale();
			const double* d = stable_origin::shift();
			auto rel = [&](const MeshEntry& e, const float* p, double (&c)[3]) {
				for (int k = 0; k < 3; k++)
				{
					const float* row = e.drawn_xform.matrix[k];
					const double r = row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3];
					c[k] = r / s - (d ? d[k] : 0.0);
				}
			};
			auto lod_facts = [&](const MeshEntry& e) {
				const size_t n = e.lod_z.size();
				size_t filler = 0;
				double fc[3] = { 0.0, 0.0, 0.0 };
				if (e.lod_src.size() == n * 5)
				{
					for (size_t i = 0; i < n; i++)
					{
						if (!(e.lod_z[i] <= kLodFillerZ)) continue;
						const float p[3] = { e.lod_src[i * 5], e.lod_src[i * 5 + 1], e.lod_z[i] };
						double c[3];
						rel(e, p, c);
						for (int k = 0; k < 3; k++) fc[k] += c[k];
						filler++;
					}
				}
				std::string out = std::format(" sank {} morph {:.2f} filler {}/{}",
					e.lod_sank == kSankNone ? "none" : e.lod_sank == kSankAll ? "all" : "some", e.lod_g0, filler, n);
				if (filler)
					out += std::format(" at ({:.0f}, {:.0f}, {:.0f})", fc[0] / filler, fc[1] / filler, fc[2] / filler);
				return out;
			};
			auto dump = [&](const char* kind, const std::unordered_map<uint64_t, MeshEntry>& map, bool lod) {
				int drawn = 0, idle = 0;
				for (const auto& kv : map)
				{
					const MeshEntry& e = kv.second;
					if (e.drawn_frame == 0xFFFFFFFFu || !e.has_bound) continue;
					const bool recent = now - e.last_frame <= 2;
					if (!recent && !lod) continue;
					double c[3];
					rel(e, e.bound_c, c);
					shared::common::log_event("TerrainDump", std::format("{} geo 0x{:08X}{} rel ({:.0f}, {:.0f}, {:.0f}) dist {:.0f} r {:.0f}{}",
						kind, static_cast<uint32_t>(e.geo), recent ? "" : std::format(" not drawn for {}f", now - e.last_frame),
						c[0], c[1], c[2], std::sqrt(c[0] * c[0] + c[1] * c[1]), e.bound_r, lod ? lod_facts(e) : std::string()),
						shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
					(recent ? drawn : idle)++;
				}
				shared::common::log_event("TerrainDump", std::format("{}: {} drawn{}", kind, drawn,
					lod ? std::format(", {} alive but not drawn", idle) : std::string()), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
			};
			dump("land", g_terrain, false);
			dump("farlod", g_far_lod, true);
		}
	}

	void request_terrain_dump() { g_dump_requested = true; }

	void terrain_note_route(const char* route)
	{
		if (!g_trace_frames) return;
		const uintptr_t geo = scene_graph::current_geometry();
		if (!geo) return;
		const uint16_t pass = scene_graph::current_pass_type();
		const bool lod_pass = pass == 0x0FE || pass == 0x10B || pass == 0x230;
		scene_graph::GeoRead gr;
		scene_graph::ShadeFlags sf;
		const bool lod_land = scene_graph::read_geo(geo, gr) &&
			scene_graph::read_shade_flags(gr.prop(scene_graph::fnv::kPropShade), sf) && (sf.flags2 & kSF2_lodLandscape);
		if (!lod_land && !lod_pass) return;
		Traced& t = g_traced[geo];
		t.lod_land = lod_land;
		t.flags2 = sf.flags2;
		const std::string r = route ? route : "?";
		if (t.routes.find(r) == std::string::npos) t.routes += (t.routes.empty() ? "" : ",") + r;
		const std::string p = std::format("0x{:03X}", pass);
		if (t.passes.find(p) == std::string::npos) t.passes += (t.passes.empty() ? "" : ",") + p;
	}

	// Called from api_submit.cpp.
	void terrain_on_present()
	{
		if (g_trace_frames && --g_trace_frames == 0) finish_trace();
		if (g_dump_requested.exchange(false))
		{
			dump_terrain();
			g_traced.clear();
			g_trace_frames = kTraceFrames;
		}

		if (!g_retired.empty()) destroy_retired(false);

		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.evict_frames);
		evict_stale(g_terrain, keep, terrain_stats());
		evict_stale(g_far_lod, keep, far_lod_stats());

		// Status line: height layers across the live land quads, and the land
		// quads / far-LOD blocks a texture list hides.
		auto& st = terrain_stats();
		st.height_layers = 0;
		st.height_pending = 0;
		st.list_hidden = 0;
		for (const auto& kv : g_terrain)
		{
			st.height_layers += kv.second.height_layers;
			if (kv.second.height_pending) st.height_pending++;
			if (kv.second.list_hidden) st.list_hidden++;
		}
		auto& lod = far_lod_stats();
		lod.list_hidden = 0;
		for (const auto& kv : g_far_lod)
			if (kv.second.list_hidden) lod.list_hidden++;

		const UINT now = frame();
		for (auto it = g_retry_at.begin(); it != g_retry_at.end();)
		{
			if (now >= it->second) it = g_retry_at.erase(it);
			else ++it;
		}
	}

	void terrain_on_reset()
	{
		g_land_drawn.clear();
		g_land_drawn_frame = 0xFFFFFFFFu;
		g_retry_at.clear();
	}

	void terrain_drop_retained()
	{
		drop_retained(g_terrain);
		drop_retained(g_far_lod);
	}

	void terrain_drop_all()
	{
		destroy_all(g_terrain, terrain_stats());
		destroy_all(g_far_lod, far_lod_stats());
		release_lod_detail();
		g_land_drawn.clear();
		g_land_drawn_frame = 0xFFFFFFFFu;
		g_retry_at.clear();
	}
}
