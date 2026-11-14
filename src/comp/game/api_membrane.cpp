#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "api_identity.hpp"
#include "api_material.hpp"
#include "api_textures.hpp"
#include "api_camera.hpp"
#include "api_common.hpp"
#include "scene_graph.hpp"
#include "stable_origin.hpp"
#include "stealth_field.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/config.hpp"

/*
 * Effect-shader membranes on skinned actors through the Remix API
 * ([RemixApi] EffectMembranes).
 *
 * What the game draws
 * -------------------
 * An effect shader (EFSH: a plasma kill's GooShader01, effectLaserDisintegration,
 * effectAlienDisintegration ...) adds one more pass over every geometry of the
 * actor: BSSM_TEXEFFECT / _S / 2x_TEXEFFECT / 2x_TEXEFFECT_S (0x24E..0x251,
 * the SM3 pair 0x1E0 / 0x1E1), drawn with the depth test EQUAL and, for the
 * effects taken here, blended SRCALPHA / ONE. Its pixel shader (SLS2089,
 * disassembled from shaderpackage002) is
 *   rim   = pow(saturate(1 - |N.V|), fVars.x)
 *   alpha = SourceTexture.a * FillColor.a
 *   out   = ((SourceTexture.rgb + FillColor.rgb) * alpha + rim * RimColor.rgb,
 *            alpha + rim * RimColor.a)
 * with FillColor c0, RimColor c1, fVars c2: the engine moves both colours and
 * alphas through the effect's fade-in, full and fade-out times every frame.
 * The pass has no albedo, so the skinned route dropped it with the raster
 * lighting passes, and an actor melted or burnt to ash with no glow at all.
 *
 * What is taken
 * -------------
 * Skinned TEXEFFECT passes that blend additively and whose SourceTexture is
 * absent or a dummy of at most 4 x 4 texels (read as a constant). An effect
 * with a real fill texture (the electric tiles of an EMP or Tesla hit) is left
 * dropped, as before. Not under a stealth field, not the Pip-Boy's own draws.
 *
 * How
 * ---
 * One more API instance per pass: the partition's own geometry, lifted
 * kLift along its normals (the film sits just outside the body, which the
 * EQUAL depth test did in the game), with the pixel shader's output
 * evaluated per vertex as its vertex colour: the bind pose is skinned on the
 * CPU with this pass's own palette, the eye is the pass's EyePosition (VS c8).
 * The instance blends as the draw does, so the runtime turns it into emission
 * (colour x alpha x rtx.emissiveBlendOverrideEmissiveIntensity), which follows
 * the exposure like every other additive effect.
 * The colours change with the view and the fade, so the mesh is refreshed in
 * place every frame (MeshInfoRefreshGeometryEXT, same hash); it exists only
 * while the effect plays. One white 1 x 1 material serves every film.
 *
 * Not reproduced
 * --------------
 * The effect's dissolve (its "holes" texture, which the engine applies through
 * a depth-only pass of each geometry, BSSM_ZONLY_TEXEFFECT / _S): the body
 * stays whole under the film until the game removes it.
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;

	namespace
	{
		constexpr uint64_t kMembraneDomain = 0x4D454D4252414E45ull;   // "MEMBRANE"
		constexpr uint16_t kPassTexEffectFirst = 0x24E;   // BSSM_TEXEFFECT
		constexpr uint16_t kPassTexEffectLast = 0x251;    // BSSM_2x_TEXEFFECT_S
		constexpr uint16_t kPass3xTexEffectFirst = 0x1E0; // BSSM_3XTEXEFFECT
		constexpr uint16_t kPass3xTexEffectLast = 0x1E1;  // BSSM_3XTEXEFFECT_S
		constexpr UINT kRegFill = 0, kRegRim = 1, kRegVars = 2;   // pixel shader constants
		constexpr UINT kRegEye = 8;                                // vertex shader constant EyePosition
		constexpr UINT kSourceStage = 1;                           // SourceTexture
		constexpr UINT kDummyTexels = 4;      // a SourceTexture up to this size is a constant
		constexpr float kLift = 0.4f;         // game units along the normal (about 6 mm)
		constexpr float kInvisible = 1.0f / 510.0f;   // both alphas under this: nothing to draw
		constexpr uint32_t kBonesPerVertex = 4;
		constexpr double kAnchorSteps = 16.0;
		constexpr UINT kForgetFrames = 120;
		constexpr UINT kRetryFrames = 120;
		constexpr uint32_t kMaxLogLines = 48;

		enum class Reason : uint8_t
		{
			Off, Inactive, PipBoy, Stealth, NoBones, Constants, Faded, Blend, Texture, Layout, Dynamic, Budget, Build,
			Retry, Palette, Bones, Submit, Count
		};
		constexpr const char* kReasonNames[] = {
			"off", "inactive", "pipboy", "stealth", "nobones", "constants", "faded", "blend", "texture", "layout",
			"dynamic", "budget", "build", "retry", "palette", "bones", "submit",
		};
		static_assert(std::size(kReasonNames) == static_cast<size_t>(Reason::Count));

		struct Counters
		{
			uint64_t passes = 0, drawn = 0, built = 0, refreshed = 0, evicted = 0;
			uint64_t vertices = 0;   // vertices sent (builds and refreshes)
			uint64_t refused[static_cast<size_t>(Reason::Count)] = {};
		};
		Counters g_c;       // since the last status line
		uint64_t g_total = 0;   // films drawn since start

		// One film: a partition of one actor under one effect.
		struct Film
		{
			remixapi_MeshHandle mesh = nullptr;
			uint64_t hash = 0;
			// The bind pose as the partition's buffers hold it, and its skinning
			// (FFP indexed blending, as api_skinned.cpp builds it).
			std::vector<float> pos, nrm;                  // 3 per vertex
			std::vector<float> weights;                   // kBonesPerVertex per vertex
			std::vector<uint32_t> slots;                  // kBonesPerVertex per vertex
			std::vector<uint32_t> tris;
			std::vector<remixapi_HardcodedVertex> verts;  // what the runtime holds; colours rewritten per frame
			int max_slot = 0;
			uint32_t vb_gen = 0, ib_gen = 0;
			UINT last_frame = 0;
		};

		std::unordered_map<uint64_t, Film> g_films;
		std::unordered_map<uint64_t, UINT> g_retry_at;
		// Passes of one partition and geometry this frame (two effects at once
		// are two films).
		std::unordered_map<uint64_t, uint32_t> g_seen;

		remixapi_MaterialHandle g_material = nullptr;
		uint64_t g_white = 0;
		UINT g_material_retry = 0;

		std::unordered_set<uint64_t> g_logged;
		uint32_t g_log_lines = 0;

		DWORD rs(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD fallback)
		{
			DWORD v = fallback;
			return SUCCEEDED(dev->GetRenderState(s, &v)) ? v : fallback;
		}

		bool refuse(Reason r)
		{
			g_c.refused[static_cast<size_t>(r)]++;
			return false;
		}

		bool finite4(const float* v)
		{
			return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]);
		}

		uint8_t unorm8(float v)
		{
			if (!(v > 0.0f)) return 0;
			return v >= 1.0f ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f);
		}

		// The one material: white, with the instance's blend as its alpha state.
		bool ensure_material()
		{
			if (g_material) return true;
			const UINT now = frame();
			if (now < g_material_retry) return false;
			const char* reject = nullptr;
			if (!g_white)
			{
				static const uint8_t kWhite[4] = { 255, 255, 255, 255 };
				g_white = textures::acquire_generated(1, 1, 1, kWhite, sizeof(kWhite), REMIXAPI_FORMAT_R8G8B8A8_UNORM, reject);
			}
			if (g_white)
			{
				const std::wstring path = texture_path(g_white);
				remixapi_MaterialInfoOpaqueEXT op;
				init_opaque_ext(op, 1.0f);
				op.useDrawCallAlphaState = 1;
				remixapi_MaterialInfo mi;
				init_material_info(mi, next_object_hash(0x46494C4Dull, kMembraneDomain), &op);
				mi.albedoTexture = path.c_str();
				g_material = create_material(mi);
			}
			if (!g_material)
			{
				g_material_retry = now + kRetryFrames;
				shared::common::log("RemixApi", std::format("membrane: the film material could not be built ({}) -- retried "
					"in {} frames", reject ? reject : "CreateMaterial failed", kRetryFrames),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
				return false;
			}
			return true;
		}

		// SourceTexture as a constant: nothing bound reads (0, 0, 0, 1), a dummy
		// of at most kDummyTexels its first texel. False for a real texture.
		bool source_constant(float (&t)[4], std::string& what)
		{
			t[0] = t[1] = t[2] = 0.0f;
			t[3] = 1.0f;
			IDirect3DBaseTexture9* base = shared::common::ffp_state::get().cur_texture(kSourceStage);
			if (!base) { what = "none"; return true; }
			if (base->GetType() != D3DRTYPE_TEXTURE) { what = std::format("type {}", static_cast<int>(base->GetType())); return false; }
			D3DSURFACE_DESC sd = {};
			if (FAILED(static_cast<IDirect3DTexture9*>(base)->GetLevelDesc(0, &sd))) { what = "unreadable description"; return false; }
			what = std::format("{}x{} fmt {}", sd.Width, sd.Height, static_cast<int>(sd.Format));
			if (sd.Width > kDummyTexels || sd.Height > kDummyTexels) return false;
			uint32_t w = 0, h = 0, levels = 0;
			std::vector<uint8_t> bgra;
			const char* reject = nullptr;
			if (textures::read_bgra8(base, w, h, levels, bgra, reject) && bgra.size() >= 4)
			{
				t[0] = bgra[2] / 255.0f;
				t[1] = bgra[1] / 255.0f;
				t[2] = bgra[0] / 255.0f;
				t[3] = bgra[3] / 255.0f;
				what += std::format(" = ({:.2f}, {:.2f}, {:.2f}, {:.2f})", t[0], t[1], t[2], t[3]);
			}
			else what += std::format(" (not read: {}; taken as black, alpha 1)", reject ? reject : "?");
			return true;
		}

		struct Layout
		{
			Attr pos, nrm, bw, bi;
			int nw = 0;
		};

		bool film_layout(Layout& l)
		{
			const auto& ffp = shared::common::ffp_state::get();
			if (ffp.cur_decl_multi_stream() || ffp.cur_decl_has_pos_t() || !ffp.cur_decl_is_skinned()) return false;
			l.pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			l.nrm = find_attr(D3DDECLUSAGE_NORMAL, 0);
			l.bw = find_attr(D3DDECLUSAGE_BLENDWEIGHT, 0);
			l.bi = find_attr(D3DDECLUSAGE_BLENDINDICES, 0);
			if (!l.pos.ok() || (l.pos.type != D3DDECLTYPE_FLOAT3 && l.pos.type != D3DDECLTYPE_FLOAT4)) return false;
			if (!l.nrm.ok() || l.nrm.type != D3DDECLTYPE_FLOAT3) return false;
			if (!l.bw.ok() || !((l.bw.type >= D3DDECLTYPE_FLOAT1 && l.bw.type <= D3DDECLTYPE_FLOAT4) ||
				l.bw.type == D3DDECLTYPE_UBYTE4N))
				return false;
			if (!l.bi.ok() || (l.bi.type != D3DDECLTYPE_D3DCOLOR && l.bi.type != D3DDECLTYPE_UBYTE4)) return false;
			const UINT stride = ffp.stream_stride(0);
			if (!attr_fits(l.pos, stride) || !attr_fits(l.nrm, stride) || !attr_fits(l.bw, stride) || !attr_fits(l.bi, stride))
				return false;
			l.nw = std::clamp(ffp.cur_decl_num_weights(), 1, 3);
			return true;
		}

		// The partition's bind pose and skinning from its buffers.
		bool read_film(IDirect3DDevice9* dev, const DrawDesc& d, const Layout& L, Film& f, const char*& reject)
		{
			Geometry g;
			if (!read_geometry(dev, d, g, reject)) return false;
			if (!g.count || g.tris.empty()) { reject = "empty geometry"; return false; }
			if (f.mesh && (g.count != f.verts.size() || g.tris.size() != f.tris.size()))
			{
				reject = "the rewritten buffers changed the vertex / triangle count";
				return false;
			}
			f.pos.assign(static_cast<size_t>(g.count) * 3, 0.0f);
			f.nrm.assign(static_cast<size_t>(g.count) * 3, 0.0f);
			f.weights.assign(static_cast<size_t>(g.count) * kBonesPerVertex, 0.0f);
			f.slots.assign(static_cast<size_t>(g.count) * kBonesPerVertex, 0u);
			f.verts.assign(g.count, remixapi_HardcodedVertex{});
			int max_slot = 0;
			for (UINT i = 0; i < g.count; i++)
			{
				const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride;
				float* pos = &f.pos[static_cast<size_t>(i) * 3];
				float* nrm = &f.nrm[static_cast<size_t>(i) * 3];
				for (int k = 0; k < 3; k++) pos[k] = read_comp(p + L.pos.off, L.pos.type, k);
				if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]))
				{
					reject = "non-finite POSITION";
					return false;
				}
				float n[3];
				for (int k = 0; k < 3; k++) n[k] = read_comp(p + L.nrm.off, L.nrm.type, k);
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (std::isfinite(len) && len > 1e-6f) { nrm[0] = n[0] / len; nrm[1] = n[1] / len; nrm[2] = n[2] / len; }
				else { nrm[0] = 0.0f; nrm[1] = 0.0f; nrm[2] = 1.0f; }

				float* w = &f.weights[static_cast<size_t>(i) * kBonesPerVertex];
				uint32_t* ix = &f.slots[static_cast<size_t>(i) * kBonesPerVertex];
				const uint8_t* bi = p + L.bi.off;
				float sum = 0.0f;
				for (int k = 0; k < L.nw; k++)
				{
					const float wk = read_comp(p + L.bw.off, L.bw.type, k);
					if (!std::isfinite(wk)) { reject = "non-finite BLENDWEIGHT"; return false; }
					w[k] = wk;
					sum += wk;
				}
				w[L.nw] = 1.0f - sum;
				for (int k = 0; k <= L.nw; k++)
				{
					if (w[k] == 0.0f) continue;
					ix[k] = bi[k];
					max_slot = (std::max)(max_slot, static_cast<int>(bi[k]));
				}

				auto& o = f.verts[i];
				for (int k = 0; k < 3; k++)
				{
					o.position[k] = pos[k] + nrm[k] * kLift;
					o.normal[k] = nrm[k];
				}
				o.color = 0u;
			}
			if (max_slot >= MAX_FFP_BONES) { reject = "palette slot beyond the FFP bone limit"; return false; }
			f.max_slot = max_slot;
			f.tris = std::move(g.tris);
			return true;
		}

		// The pixel shader's output per vertex, with this pass's palette and eye.
		void shade(Film& f, const float (&src)[4], const float* fill, const float* rim, float falloff, const float* eye)
		{
			const float fa = src[3] * fill[3];
			const float base[3] = { (src[0] + fill[0]) * fa, (src[1] + fill[1]) * fa, (src[2] + fill[2]) * fa };
			const size_t count = f.verts.size();
			for (size_t i = 0; i < count; i++)
			{
				const float* p = &f.pos[i * 3];
				const float* n = &f.nrm[i * 3];
				const float* w = &f.weights[i * kBonesPerVertex];
				const uint32_t* ix = &f.slots[i * kBonesPerVertex];
				float sp[3] = {}, sn[3] = {};
				for (uint32_t k = 0; k < kBonesPerVertex; k++)
				{
					if (w[k] == 0.0f) continue;
					const float* b = game::bone_rows[ix[k]];
					for (int r = 0; r < 3; r++)
					{
						sp[r] += w[k] * (b[r * 4 + 0] * p[0] + b[r * 4 + 1] * p[1] + b[r * 4 + 2] * p[2] + b[r * 4 + 3]);
						sn[r] += w[k] * (b[r * 4 + 0] * n[0] + b[r * 4 + 1] * n[1] + b[r * 4 + 2] * n[2]);
					}
				}
				const float v[3] = { eye[0] - sp[0], eye[1] - sp[1], eye[2] - sp[2] };
				const float nl = std::sqrt(sn[0] * sn[0] + sn[1] * sn[1] + sn[2] * sn[2]);
				const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				float edge = 0.0f;
				if (nl > 1e-6f && vl > 1e-6f && std::isfinite(nl) && std::isfinite(vl))
				{
					// The vertex shader turns the view vector round for a normal
					// that faces away: the rim is the same on both sides.
					const float ndv = std::fabs((sn[0] * v[0] + sn[1] * v[1] + sn[2] * v[2]) / (nl * vl));
					const float e = std::clamp(1.0f - ndv, 0.0f, 1.0f);
					edge = e > 0.0f ? std::pow(e, falloff) : 0.0f;
				}
				f.verts[i].color = D3DCOLOR_ARGB(unorm8(fa + edge * rim[3]), unorm8(base[0] + edge * rim[0]),
					unorm8(base[1] + edge * rim[1]), unorm8(base[2] + edge * rim[2]));
			}
		}

		bool send_mesh(Film& f, uint64_t key)
		{
			const bool refresh = f.mesh != nullptr;
			remixapi_MeshInfoSurfaceTriangles surf = {};
			surf.vertices_values = f.verts.data();
			surf.vertices_count = static_cast<uint64_t>(f.verts.size());
			surf.indices_values = f.tris.data();
			surf.indices_count = static_cast<uint64_t>(f.tris.size());
			surf.skinning_hasvalue = 1;
			surf.skinning_value.bonesPerVertex = kBonesPerVertex;
			surf.skinning_value.blendWeights_values = f.weights.data();
			surf.skinning_value.blendWeights_count = static_cast<uint32_t>(f.weights.size());
			surf.skinning_value.blendIndices_values = f.slots.data();
			surf.skinning_value.blendIndices_count = static_cast<uint32_t>(f.slots.size());
			surf.material = g_material;

			remixapi_MeshInfoRefreshGeometryEXT rg = {};
			rg.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT;
			remixapi_MeshInfo mi = {};
			mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mi.pNext = refresh ? &rg : nullptr;
			// No asset key: a film must never take the replacement of the body it lies on.
			mi.hash = refresh ? f.hash : next_object_hash(key, kMembraneDomain);
			mi.surfaces_values = &surf;
			mi.surfaces_count = 1;

			auto* b = bridge();
			remixapi_MeshHandle h = nullptr;
			if (!b || b->CreateMesh(&mi, &h) != REMIXAPI_ERROR_CODE_SUCCESS || !h) return false;
			f.mesh = h;
			f.hash = mi.hash;
			g_c.vertices += f.verts.size();
			if (refresh) g_c.refreshed++;
			else g_c.built++;
			return true;
		}

		void destroy_film(Film& f)
		{
			if (f.mesh) if (auto* b = bridge()) b->DestroyMesh(f.mesh);
			f.mesh = nullptr;
		}

		// This frame's DrawInstance with the pass's palette (as api_skinned.cpp
		// sends a body: bones relative to an anchor near the actor).
		bool submit(IDirect3DDevice9* dev, const Film& f, const remixapi_InstanceInfoBlendEXT& blend, int nb, Reason& why)
		{
			auto* b = bridge();
			if (!b || !f.mesh) { why = Reason::Inactive; return false; }
			static const double kZero[3] = { 0.0, 0.0, 0.0 };
			const double* dshift = stable_origin::active() ? stable_origin::shift() : kZero;
			double anchor[3];
			for (int r = 0; r < 3; r++)
				anchor[r] = std::round((static_cast<double>(game::bone_rows[0][r * 4 + 3]) + dshift[r]) * kAnchorSteps) /
					kAnchorSteps;

			remixapi_Transform bones[MAX_FFP_BONES];
			for (int i = 0; i < nb; i++)
			{
				const float* c = game::bone_rows[i];
				auto& m = bones[i].matrix;
				for (int r = 0; r < 3; r++)
				{
					m[r][0] = c[r * 4 + 0];
					m[r][1] = c[r * 4 + 1];
					m[r][2] = c[r * 4 + 2];
					m[r][3] = static_cast<float>(static_cast<double>(c[r * 4 + 3]) + dshift[r] - anchor[r]);
					if (!std::isfinite(m[r][0]) || !std::isfinite(m[r][1]) || !std::isfinite(m[r][2]) || !std::isfinite(m[r][3]))
					{
						why = Reason::Bones;
						return false;
					}
				}
			}

			remixapi_InstanceInfoBlendEXT blend_ext = blend;
			blend_ext.pNext = nullptr;
			remixapi_InstanceInfoBoneTransformsEXT bones_ext = {};
			bones_ext.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
			bones_ext.pNext = &blend_ext;
			bones_ext.boneTransforms_values = bones;
			bones_ext.boneTransforms_count = static_cast<uint32_t>(nb);

			remixapi_InstanceInfo ii = {};
			ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
			ii.pNext = &bones_ext;
			// Its opacity is the vertex colour's alpha, new every frame: a baked
			// opacity micromap would be stale at once.
			ii.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP | api_camera::view_model_categories(dev);
			ii.mesh = f.mesh;
			const double s = stable_origin::scale();
			ii.transform = {};
			for (int r = 0; r < 3; r++)
			{
				ii.transform.matrix[r][r] = static_cast<float>(s);
				ii.transform.matrix[r][3] = static_cast<float>(anchor[r] * s);
			}
			ii.doubleSided = double_sided_for_cull(dev) ? 1 : 0;
			if (b->DrawInstance(&ii) != REMIXAPI_ERROR_CODE_SUCCESS)
			{
				why = Reason::Submit;
				return false;
			}
			return true;
		}

		std::string object_name(uintptr_t obj)
		{
			uint32_t p = 0;
			char buf[64] = {};
			if (!obj || !scene_graph::read_u32(obj + 0x08, p) || !p || !scene_graph::read_block(p, buf, sizeof(buf) - 1)) return "?";
			buf[sizeof(buf) - 1] = 0;
			for (char& c : buf) if (c && (c < 32 || c > 126)) c = '?';
			return buf[0] ? std::string(buf) : std::string("\"\"");
		}

		// The first sight of each distinct (geometry, colours, decision), at most
		// kMaxLogLines a session.
		void log_pass(IDirect3DDevice9* dev, uintptr_t geo, const float* fill, const float* rim, float falloff,
			const std::string& source, const DrawDesc& d, int nb, const char* decision)
		{
			if (g_log_lines >= kMaxLogLines) return;
			Hash64 k;
			k.mix(static_cast<uint64_t>(geo));
			k.ptr(decision);
			for (int i = 0; i < 3; i++)
				k.mix((static_cast<uint64_t>(unorm8(fill[i])) << 8) | unorm8(rim[i]));
			if (!g_logged.insert(k.get()).second) return;
			g_log_lines++;
			shared::common::log("RemixApi", std::format(
				"membrane #{}: geo 0x{:X} {} pass 0x{:03X} | FillColor ({:.3f}, {:.3f}, {:.3f}, {:.3f}) RimColor ({:.3f}, {:.3f}, "
				"{:.3f}, {:.3f}) falloff {:.2f} | SourceTexture {} | blend {} src {} dst {} op {} atest {} func {} ref {} zfunc {} "
				"cull {} | {}v {}p, {} bones (palette {}) | viewmodel {} -> {}",
				g_log_lines, geo, object_name(geo), engine_class::current().type, fill[0], fill[1], fill[2], fill[3],
				rim[0], rim[1], rim[2], rim[3], falloff, source,
				rs(dev, D3DRS_ALPHABLENDENABLE, FALSE), rs(dev, D3DRS_SRCBLEND, D3DBLEND_ONE), rs(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO),
				rs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD), rs(dev, D3DRS_ALPHATESTENABLE, FALSE), rs(dev, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS),
				rs(dev, D3DRS_ALPHAREF, 0), rs(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL), rs(dev, D3DRS_CULLMODE, D3DCULL_CCW),
				d.num_vtx, d.prim_count, nb, game::bones_drawn ? "of the draw before" : "uploaded for this pass",
				game::uses_viewmodel_projection() ? 1 : 0, decision),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		// The bridge client has the skinning size fix (api_skinned.cpp: without
		// it a skinned CreateMesh kills the game). Checked once.
		bool bridge_skins()
		{
			static int s_ok = -1;
			if (s_ok < 0) s_ok = bridge_client_has("Invalid skinning on surface") ? 1 : 0;
			return s_ok == 1;
		}
	}

	bool membrane_pass()
	{
		const uint16_t t = engine_class::current().type;
		return (t >= kPassTexEffectFirst && t <= kPassTexEffectLast) || (t >= kPass3xTexEffectFirst && t <= kPass3xTexEffectLast);
	}

	bool try_membrane(IDirect3DDevice9* dev, const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_membrane");
		g_c.passes++;
		if (!shared::common::config::get().remix_api.effect_membranes) return refuse(Reason::Off);
		if (!bridge() || !bridge_skins()) return refuse(Reason::Inactive);
		if (api::pipboy_own_draw()) return refuse(Reason::PipBoy);

		const auto& ffp = shared::common::ffp_state::get();
		const int nb = game::num_bones;
		if (!d.indexed || nb <= 0 || nb > MAX_FFP_BONES) return refuse(Reason::NoBones);

		const float* pc = ffp.ps_const_data();
		const float* fill = pc + kRegFill * 4;
		const float* rim = pc + kRegRim * 4;
		float falloff = pc[kRegVars * 4];
		const float* eye = ffp.vs_const_data() + kRegEye * 4;
		if (!finite4(fill) || !finite4(rim) || !std::isfinite(falloff) || !finite4(eye)) return refuse(Reason::Constants);
		falloff = std::clamp(falloff, 0.01f, 64.0f);

		const uintptr_t geo = engine_class::current().geometry ? engine_class::current().geometry
			: scene_graph::current_geometry();
		float src[4];
		std::string source;
		const bool constant = source_constant(src, source);
		const auto note = [&](const char* decision) { log_pass(dev, geo, fill, rim, falloff, source, d, nb, decision); };

		if (stealth_field::part(geo) != stealth_field::Part::None) return refuse(Reason::Stealth);
		if (!material::draw_is_additive(dev) || !rs(dev, D3DRS_COLORWRITEENABLE, 0xF))
		{
			note("not drawn (the pass does not blend additively)");
			return refuse(Reason::Blend);
		}
		if (!constant)
		{
			note("not drawn (a film with a fill texture)");
			return refuse(Reason::Texture);
		}
		// Faded out (before the fade-in, after the fade-out): nothing to add.
		if (!(src[3] * fill[3] > kInvisible) && !(rim[3] > kInvisible)) return refuse(Reason::Faded);

		Layout L;
		if (!film_layout(L))
		{
			note("not drawn (vertex layout)");
			return refuse(Reason::Layout);
		}
		IDirect3DVertexBuffer9* vb = ffp.stream_vb(0);
		IDirect3DIndexBuffer9* ib = nullptr;
		if (!vb || FAILED(dev->GetIndices(&ib)) || !ib) return refuse(Reason::Layout);
		const bool tracked = identity::track_buffers(vb, ib);
		const identity::BufferInfo vbi = identity::buffer(vb);
		const identity::BufferInfo ibi = identity::buffer(ib);
		ib->Release();   // compared by address only from here on
		if (!tracked) return refuse(Reason::Inactive);
		if (vbi.dynamic || ibi.dynamic) return refuse(Reason::Dynamic);

		remixapi_InstanceInfoBlendEXT blend;
		const char* reject = nullptr;
		if (!material::draw_state_blend(dev, true, false, blend, reject))
		{
			note("not drawn (blend state)");
			return refuse(Reason::Blend);
		}
		// Colour and alpha are the vertex colour's: the pixel shader's output.
		blend.textureColorArg1Source = 1;   // RtTextureArgSource::Texture
		blend.textureColorArg2Source = 2;   // VertexColor0
		blend.textureColorOperation = 2;    // DxvkRtTextureOperation::SelectArg2
		blend.textureAlphaArg1Source = 1;
		blend.textureAlphaArg2Source = 2;
		blend.textureAlphaOperation = 2;
		blend.isVertexColorBakedLighting = 0;
		blend.isTextureFactorBlend = 0;

		if (!ensure_material()) return refuse(Reason::Build);

		// The film: this partition of this geometry, and which of its passes
		// this frame.
		Hash64 kh;
		kh.ptr(vb);
		kh.ptr(ib);
		kh.mix(static_cast<uint64_t>(static_cast<uint32_t>(d.base)));
		kh.mix((static_cast<uint64_t>(d.min_vtx) << 32) | d.num_vtx);
		kh.mix((static_cast<uint64_t>(d.start_index) << 32) | d.prim_count);
		kh.mix(decl_hash());
		kh.mix(static_cast<uint64_t>(geo));
		const uint32_t ordinal = g_seen[kh.get()]++;
		kh.mix(ordinal);
		const uint64_t key = kh.get();

		const UINT now = frame();
		auto it = g_films.find(key);
		if (it == g_films.end())
		{
			const auto rt = g_retry_at.find(key);
			if (rt != g_retry_at.end() && now < rt->second) return refuse(Reason::Retry);
			if (!game::uses_viewmodel_projection() && !take_build_budget(key, d.num_vtx)) return refuse(Reason::Budget);
			Film f;
			if (!read_film(dev, d, L, f, reject))
			{
				g_retry_at[key] = now + kRetryFrames;
				shared::common::log("RemixApi", std::format("membrane: a film could not be read ({}; {} verts, {} prims) -- "
					"retried in {} frames", reject ? reject : "?", d.num_vtx, d.prim_count, kRetryFrames),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
				return refuse(Reason::Build);
			}
			f.vb_gen = vbi.gen;
			f.ib_gen = ibi.gen;
			it = g_films.emplace(key, std::move(f)).first;
		}
		Film& f = it->second;
		f.last_frame = now;
		if (f.vb_gen != vbi.gen || f.ib_gen != ibi.gen)
		{
			// The game rewrote the buffers (a morphed head): the bind pose again.
			if (!read_film(dev, d, L, f, reject))
			{
				destroy_film(f);
				g_films.erase(it);
				g_retry_at[key] = now + kRetryFrames;
				return refuse(Reason::Build);
			}
			f.vb_gen = vbi.gen;
			f.ib_gen = ibi.gen;
		}
		if (f.max_slot >= nb) return refuse(Reason::Palette);

		shade(f, src, fill, rim, falloff, eye);
		if (!send_mesh(f, key))
		{
			destroy_film(f);
			g_films.erase(it);
			g_retry_at[key] = now + kRetryFrames;
			shared::common::log("RemixApi", std::format("membrane: CreateMesh failed ({} verts) -- retried in {} frames",
				d.num_vtx, kRetryFrames), shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
			return refuse(Reason::Build);
		}

		Reason why = Reason::Submit;
		if (!submit(dev, f, blend, nb, why)) return refuse(why);
		g_c.drawn++;
		g_total++;
		note("API film");
		return true;
	}

	void membrane_on_present()
	{
		g_seen.clear();
		const UINT now = frame();
		for (auto it = g_films.begin(); it != g_films.end();)
		{
			if (now - it->second.last_frame > kForgetFrames)
			{
				destroy_film(it->second);
				g_c.evicted++;
				it = g_films.erase(it);
			}
			else ++it;
		}
		if ((now & 63u) == 0)
			std::erase_if(g_retry_at, [now](const auto& p) { return now >= p.second; });
	}

	void membrane_on_reset()
	{
		// API objects live in the runtime and survive a device reset.
		g_seen.clear();
		g_retry_at.clear();
	}

	void membrane_drop_all()
	{
		for (auto& [key, f] : g_films) destroy_film(f);
		g_films.clear();
		g_retry_at.clear();
		g_seen.clear();
		if (g_material) { if (auto* b = bridge()) b->DestroyMaterial(g_material); g_material = nullptr; }
		if (g_white) { textures::release(g_white); g_white = 0; }
	}

	std::string membrane_status()
	{
		if (!g_c.passes && g_films.empty()) return {};
		std::string reasons;
		for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
			if (g_c.refused[i]) reasons += std::format("{}{}={}", reasons.empty() ? "" : " ", kReasonNames[i], g_c.refused[i]);
		const std::string s = std::format("membranes (effect-shader films, since the last line): passes={} drawn={} "
			"built={} refreshed={} verticesSent={} live={} evicted={} notDrawn=({}) | films drawn since start {}",
			g_c.passes, g_c.drawn, g_c.built, g_c.refreshed, g_c.vertices, g_films.size(), g_c.evicted,
			reasons.empty() ? std::string("none") : reasons, g_total);
		g_c = {};
		return s;
	}
}
