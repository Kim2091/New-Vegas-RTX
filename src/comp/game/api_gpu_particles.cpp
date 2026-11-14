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

#include "shared/common/ffp_state.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/config.hpp"

/*
 * Effect-shader particles through the Remix API ([RemixApi] EffectMembranes):
 * the sparks that fly off a body burnt by a laser, the drops of a plasma kill.
 *
 * What the game draws
 * -------------------
 * The particle half of an effect shader (EFSH) is simulated in the vertex
 * shader: pass BSSM_PARTICLE (0x241), VS PARTICLE.vso (0xEC6F5608), PS
 * PARTICLE.pso (out = SourceTexture x colour). A draw is a template of quads
 * (POSITION = the corner, TEXCOORD0, COLOR1.x = the particle's index); each
 * particle is three constant registers from c18: its birth position and time,
 * its normal and time scale, its rotation, speed factor and sub-texture. The
 * shader moves it (velocity, wind, acceleration by age), sizes it through two
 * scale keys, colours it through three colour keys, and adds the rotated
 * corner to the centre in clip space (a screen-aligned card). No D3D9 state
 * carries any of that, so the pass went out rasterised and untraced.
 *
 * What is taken
 * -------------
 * Draws of that pass (or that VS) that blend additively. The shader's own
 * arithmetic (transcribed from shaderpackage002's disassembly, instruction by
 * instruction: run_vs) runs on the CPU over the draw's vertices with the
 * bound constants. The clip-space corner offset becomes a model-space one
 * along the camera's right and up, read off WorldViewProj's first two rows.
 * One API mesh per draw (geometry and its ordinal this frame), refreshed in
 * place every frame, the instance at the draw's WORLD with the draw's blend,
 * PARTICLE and IGNORE_OPACITY_MICROMAP. Particles past their life, with no
 * size or no alpha are collapsed to a point.
 * A draw it does not take keeps its old route (rasterised, untraced).
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;

	namespace
	{
		constexpr uint64_t kSparkDomain = 0x4750555041525431ull;   // "GPUPART1"
		constexpr uint16_t kPassParticle = 0x241;                  // BSSM_PARTICLE
		constexpr uint32_t kParticleVs = 0xEC6F5608u;              // PARTICLE.vso
		constexpr int kRegInstance = 18;
		constexpr int kLastRegister = 255;
		constexpr UINT kMaxVertices = 4096;
		constexpr UINT kForgetFrames = 120;
		constexpr UINT kRetryFrames = 120;
		constexpr uint32_t kMaxLogLines = 24;

		enum class Reason : uint8_t
		{
			Off, Inactive, PipBoy, Blend, Texture, Layout, Geometry, World, Budget, Material, Build, Retry, Submit, Count
		};
		constexpr const char* kReasonNames[] = {
			"off", "inactive", "pipboy", "blend", "texture", "layout", "geometry", "world", "budget", "material", "build",
			"retry", "submit",
		};
		static_assert(std::size(kReasonNames) == static_cast<size_t>(Reason::Count));

		struct Counters
		{
			uint64_t draws = 0, sent = 0, built = 0, refreshed = 0, evicted = 0;
			uint64_t particles = 0, live = 0;   // particles evaluated, and those drawn
			uint64_t refused[static_cast<size_t>(Reason::Count)] = {};
		};
		Counters g_c;
		uint64_t g_total = 0;

		struct Mat
		{
			remixapi_MaterialHandle handle = nullptr;
			std::vector<uint64_t> textures;
			uint32_t categories = 0;
			bool no_vertex_color = false, alpha_dropped = false, declined = false;
			UINT retry_frame = 0, last_used = 0;
		};
		std::unordered_map<uint64_t, Mat> g_mats;

		struct System
		{
			remixapi_MeshHandle mesh = nullptr;
			uint64_t hash = 0, material_key = 0;
			size_t verts = 0, indices = 0;
			UINT last_frame = 0;
		};
		std::unordered_map<uint64_t, System> g_systems;
		std::unordered_map<uint64_t, UINT> g_retry_at;
		std::unordered_map<uintptr_t, uint32_t> g_ordinal;   // draws of one geometry this frame

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

		uint8_t unorm8(float v)
		{
			if (!(v > 0.0f)) return 0;
			return v >= 1.0f ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f);
		}

		// One vertex of PARTICLE.vso.
		struct VsOut
		{
			float centre[3] = {};   // the particle, in the space WorldViewProj takes
			float off[2] = {};      // the corner's clip-space offset before ScaleAdjust
			float uv[2] = {};
			float col[4] = {};
			float age = 0.0f;       // 0 at birth, 1 at the end of its life
			float size = 0.0f;
			bool live = false;
		};

		// PARTICLE.vso on the CPU. `c`: the vertex-shader constants (256 x 4);
		// v0 the corner, v1 TEXCOORD0, index COLOR1.x. False: an index outside
		// the constant file or a non-finite result.
		bool run_vs(const float* c, const float* v0, const float* v1, float index, VsOut& o)
		{
			const auto C = [c](int reg) { return c + reg * 4; };
			const float fi = index * 3.0f;
			if (!std::isfinite(fi)) return false;
			const int a0 = static_cast<int>(std::lround(fi - (fi - std::floor(fi))));
			if (a0 < 0 || kRegInstance + 2 + a0 > kLastRegister) return false;
			const float* i0 = C(kRegInstance + a0);       // birth position, birth time
			const float* i1 = C(kRegInstance + 1 + a0);   // normal, time scale
			const float* i2 = C(kRegInstance + 2 + a0);   // rotation, rotation speed, speed factor, sub-texture
			const float* f0 = C(4);    // fVars0: time now, life, speed along the normal, acceleration along the normal
			const float* f1 = C(5);    // fVars1: colour key times
			const float* f2 = C(6);    // fVars2: scale key 1, scale key 2, their times
			const float* f3 = C(7);    // fVars3: origin, alpha
			const float* f4 = C(8);    // fVars4: sub-texture size
			const float* wind = C(15);

			// The sub-texture cell.
			const float cell = i2[3] - std::floor(i2[3]);
			o.uv[0] = v1[0] * f4[0] + cell;
			o.uv[1] = (i2[3] - cell) * 0.01f + v1[1] * f4[1];

			// Where it is now.
			float dir[3] = { i0[0] - wind[0], i0[1] - wind[1], i0[2] - wind[2] };
			const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			if (dl > 0.0f && std::isfinite(dl)) { dir[0] /= dl; dir[1] /= dl; dir[2] /= dl; }
			else dir[0] = dir[1] = dir[2] = 0.0f;
			const float t = (f0[0] - i0[3]) * i1[3];
			float pos[3];
			for (int k = 0; k < 3; k++)
			{
				const float vel = dir[k] * wind[3] + C(12)[k] + f0[2] * i1[k] * i2[2];
				const float acc = i1[k] * f0[3] + C(13)[k];
				pos[k] = vel * t + i0[k] + acc * t * t * 0.5f;
				o.centre[k] = pos[k] - f3[k];
			}

			// Its rotation.
			float ang = (i2[1] * f0[0] + i2[0]) * 0.159154937f + 0.5f;
			ang = (ang - std::floor(ang)) * 6.28318548f - 3.14159274f;
			const float cs = std::cos(ang), sn = std::sin(ang);

			// Its size: 0 -> key 1 -> key 2 -> 0 over its life.
			const float n = t / f0[1];
			float seg[4];
			if (f2[3] < n) { seg[0] = f2[1]; seg[1] = 0.0f; seg[2] = f2[3]; seg[3] = 1.0f; }
			else if (f2[2] < n) { seg[0] = f2[0]; seg[1] = f2[1]; seg[2] = f2[2]; seg[3] = f2[3]; }
			else { seg[0] = 0.0f; seg[1] = f2[0]; seg[2] = 0.0f; seg[3] = f2[2]; }
			const float sf = (n - seg[2]) / (seg[3] - seg[2]);
			const float size = sf * seg[1] + (1.0f - sf) * seg[0];
			const float sx = size * v0[0], sy = size * v0[1];
			o.off[0] = cs * sx - sn * sy;
			o.off[1] = sn * sx + cs * sy;

			// Its colour: (key 1, alpha 0) -> key 1 -> key 2 -> key 3 -> (key 3, alpha 0).
			const float* k1 = C(9);
			const float* k2 = C(10);
			const float* k3 = C(11);
			const bool p1 = f1[0] < n, p2 = f1[1] < n, p3 = f1[2] < n;
			float t0, t1, from[4], to[4];
			if (p3) { t0 = f1[2]; t1 = 1.0f; }
			else if (p2) { t0 = f1[1]; t1 = f1[2]; }
			else if (p1) { t0 = f1[0]; t1 = f1[1]; }
			else { t0 = 0.0f; t1 = f1[0]; }
			for (int k = 0; k < 4; k++)
			{
				const float start = k < 3 ? k1[k] : (p1 ? k1[3] : 0.0f);
				from[k] = p3 ? k3[k] : p2 ? k2[k] : start;
				const float end = p2 ? k3[k] : p1 ? k2[k] : k1[k];
				to[k] = p3 ? (k < 3 ? k3[k] : 0.0f) : end;
			}
			const float cf = (n - t0) / (t1 - t0);
			for (int k = 0; k < 4; k++) o.col[k] = cf * to[k] + (1.0f - cf) * from[k];
			o.col[3] *= f3[3];

			o.age = n;
			o.size = size;
			for (const float v : { o.centre[0], o.centre[1], o.centre[2], o.off[0], o.off[1], o.uv[0], o.uv[1], o.col[0],
				o.col[1], o.col[2], o.col[3], n, size })
				if (!std::isfinite(v)) return false;
			o.live = n >= 0.0f && n <= 1.0f && size > 0.0f && o.col[3] > 0.0f;
			return true;
		}

		std::string describe_decl()
		{
			const auto& ffp = shared::common::ffp_state::get();
			std::string els;
			for (UINT i = 0; i < ffp.cur_decl_num_elements(); i++)
			{
				const auto& e = ffp.cur_decl_elements()[i];
				els += std::format("{}s{}+{} u{}.{} t{}", els.empty() ? "" : ", ", e.Stream, e.Offset, e.Usage, e.UsageIndex, e.Type);
			}
			return std::format("stride {}: {}", ffp.stream_stride(0), els);
		}

		void destroy_system(System& s)
		{
			if (s.mesh) if (auto* b = bridge()) b->DestroyMesh(s.mesh);
			s.mesh = nullptr;
			if (s.material_key)
				if (const auto it = g_mats.find(s.material_key); it != g_mats.end()) it->second.last_used = frame();
			s.material_key = 0;
		}

		// One line per distinct (texture, decision), at most kMaxLogLines.
		template <typename Make>
		void log_once(const void* tex, const char* decision, Make&& make)
		{
			if (g_log_lines >= kMaxLogLines) return;
			Hash64 k;
			k.ptr(tex);
			k.ptr(decision);
			if (!g_logged.insert(k.get()).second) return;
			g_log_lines++;
			shared::common::log("RemixApi", std::format("gpu particles #{}: {} -> {}", g_log_lines, make(), decision),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
	}

	bool gpu_particle_pass()
	{
		const auto& e = engine_class::current();
		if (e.type == kPassParticle) return true;
		// The engine named another pass: not one. Only a draw it gives no pass
		// for is told by its vertex shader.
		if (e.known()) return false;
		const auto* vs = shared::common::g_vs_classifier.classify(shared::common::ffp_state::get().last_vs());
		return vs && vs->vs_hash == kParticleVs;
	}

	bool try_gpu_particles(IDirect3DDevice9* dev, const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_gpu_particles");
		g_c.draws++;
		if (!shared::common::config::get().remix_api.effect_membranes) return refuse(Reason::Off);
		auto* b = bridge();
		if (!b) return refuse(Reason::Inactive);
		if (api::pipboy_own_draw()) return refuse(Reason::PipBoy);

		const auto& ffp = shared::common::ffp_state::get();
		IDirect3DBaseTexture9* tex = ffp.cur_texture(0);
		const uintptr_t geo = engine_class::current().geometry ? engine_class::current().geometry
			: scene_graph::current_geometry();
		const float* c = ffp.vs_const_data();
		const auto state = [&] {
			return std::format("geo 0x{:X} pass 0x{:03X} | {} | {} {}v {}p | blend {} src {} dst {} atest {} zfunc {} | fVars0 "
				"({:.3f}, {:.3f}, {:.3f}, {:.3f}) fVars2 ({:.2f}, {:.2f}, {:.2f}, {:.2f}) fVars3 ({:.1f}, {:.1f}, {:.1f}, {:.2f}) "
				"fVars4 ({:.3f}, {:.3f}) ScaleAdjust ({:.3f}, {:.3f}) Color1 ({:.2f}, {:.2f}, {:.2f}, {:.2f}) WVP row0 ({:.3f}, "
				"{:.3f}, {:.3f}, {:.1f})",
				geo, engine_class::current().type, describe_decl(), d.indexed ? "indexed" : "non-indexed", d.num_vtx,
				d.prim_count, rs(dev, D3DRS_ALPHABLENDENABLE, FALSE), rs(dev, D3DRS_SRCBLEND, D3DBLEND_ONE),
				rs(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO), rs(dev, D3DRS_ALPHATESTENABLE, FALSE), rs(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL),
				c[16], c[17], c[18], c[19], c[24], c[25], c[26], c[27], c[28], c[29], c[30], c[31], c[32], c[33], c[56], c[57],
				c[36], c[37], c[38], c[39], c[0], c[1], c[2], c[3]);
		};

		if (!material::draw_is_additive(dev) || !rs(dev, D3DRS_COLORWRITEENABLE, 0xF))
		{
			log_once(tex, "old route (the draw does not blend additively)", state);
			return refuse(Reason::Blend);
		}
		if (!tex || tex->GetType() != D3DRTYPE_TEXTURE)
		{
			log_once(tex, "old route (no texture on stage 0)", state);
			return refuse(Reason::Texture);
		}

		// Layout: the corner, TEXCOORD0, the particle's index in COLOR1.
		const UINT stride = ffp.stream_stride(0);
		const Attr pos = find_attr(D3DDECLUSAGE_POSITION, 0);
		const Attr uv = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
		const Attr idx = find_attr(D3DDECLUSAGE_COLOR, 1);
		if (ffp.cur_decl_multi_stream() || ffp.cur_decl_has_pos_t() || !stride || !pos.ok() || !uv.ok() || !idx.ok() ||
			!attr_fits(pos, stride) || !attr_fits(uv, stride) || !attr_fits(idx, stride) || d.num_vtx > kMaxVertices)
		{
			log_once(tex, "old route (vertex layout)", state);
			return refuse(Reason::Layout);
		}
		const float* w = world_matrix();
		if (!w) return refuse(Reason::World);

		Geometry g;
		const char* reject = nullptr;
		if (!read_geometry(dev, d, g, reject) || !g.count || g.tris.empty())
		{
			log_once(tex, "old route (the vertices can't be read)", [&] { return state() + std::format(" | {}", reject ? reject : "empty"); });
			return refuse(Reason::Geometry);
		}

		// The camera's right and up in the space the particles are in, and what
		// one unit of clip-space x / y is there.
		float right[3] = { c[0], c[1], c[2] }, up[3] = { c[4], c[5], c[6] };
		const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
		const float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
		if (!(rl > 1e-6f) || !(ul > 1e-6f) || !std::isfinite(rl) || !std::isfinite(ul)) return refuse(Reason::World);
		const float kx = c[14 * 4 + 0] / (rl * rl), ky = c[14 * 4 + 1] / (ul * ul);
		float facing[3] = { right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2],
			right[0] * up[1] - right[1] * up[0] };
		const float fl = std::sqrt(facing[0] * facing[0] + facing[1] * facing[1] + facing[2] * facing[2]);
		if (fl > 1e-12f) { for (float& x : facing) x /= fl; }
		else { facing[0] = 0.0f; facing[1] = 0.0f; facing[2] = 1.0f; }

		// This frame's vertices.
		std::vector<remixapi_HardcodedVertex> hv(g.count);
		uint32_t live = 0;
		VsOut first;
		bool have_first = false;
		for (UINT i = 0; i < g.count; i++)
		{
			const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride;
			const float v0[2] = { read_comp(p + pos.off, pos.type, 0), read_comp(p + pos.off, pos.type, 1) };
			const float v1[2] = { read_comp(p + uv.off, uv.type, 0), read_comp(p + uv.off, uv.type, 1) };
			VsOut o;
			const bool ok = run_vs(c, v0, v1, read_comp(p + idx.off, idx.type, 0), o);
			if (!have_first && ok) { first = o; have_first = true; }
			auto& out = hv[i];
			out = {};
			std::memcpy(out.normal, facing, sizeof(facing));
			if (!ok || !o.live)
			{
				// Nothing to show: a point with no alpha (zero area: nothing to hit).
				if (ok) std::memcpy(out.position, o.centre, sizeof(o.centre));
				out.color = 0u;
				continue;
			}
			live++;
			for (int k = 0; k < 3; k++) out.position[k] = o.centre[k] + right[k] * (o.off[0] * kx) + up[k] * (o.off[1] * ky);
			out.texcoord[0] = o.uv[0];
			out.texcoord[1] = o.uv[1];
			out.color = D3DCOLOR_ARGB(unorm8(o.col[3]), unorm8(o.col[0]), unorm8(o.col[1]), unorm8(o.col[2]));
		}
		g_c.particles += g.count / 4;
		g_c.live += live / 4;
		const auto detail = [&] {
			return state() + std::format(" | first vertex: centre ({:.1f}, {:.1f}, {:.1f}) offset ({:.2f}, {:.2f}) age {:.3f} size "
				"{:.2f} colour ({:.2f}, {:.2f}, {:.2f}, {:.2f}) | live vertices {}/{} | world offset per unit ({:.4f}, {:.4f})",
				first.centre[0], first.centre[1], first.centre[2], first.off[0], first.off[1], first.age, first.size, first.col[0],
				first.col[1], first.col[2], first.col[3], live, g.count, kx * rl, ky * ul);
		};

		// Material: the particle texture with the draw's blend.
		material::Desc md;
		md.diffuse = tex;
		md.blend = true;
		material::sampler_from_stage(dev, 0, md);
		md.path = "gpuParticles";
		const uint64_t mkey = material::key(md);
		const UINT now = frame();
		auto mit = g_mats.find(mkey);
		if (mit != g_mats.end() && mit->second.declined && now >= mit->second.retry_frame)
		{
			g_mats.erase(mit);
			mit = g_mats.end();
		}
		if (mit == g_mats.end())
		{
			if (!take_build_budget(mkey)) return refuse(Reason::Budget);
			material::Built bt;
			material::build(md, mkey, kSparkDomain ^ 0x4D4154ull, bt);
			Mat m;
			if (bt.why != material::Decline::None)
			{
				for (const uint64_t t : bt.textures) textures::release(t);
				if (bt.handle) b->DestroyMaterial(bt.handle);
				m.declined = true;
				m.retry_frame = now + kRetryFrames;
			}
			else
			{
				m.handle = bt.handle;
				m.textures = std::move(bt.textures);
				m.categories = bt.categories;
				m.no_vertex_color = bt.no_vertex_color;
				m.alpha_dropped = bt.alpha_dropped;
			}
			mit = g_mats.emplace(mkey, std::move(m)).first;
		}
		Mat& mat = mit->second;
		mat.last_used = now;
		if (mat.declined)
		{
			log_once(tex, "old route (the material was declined)", detail);
			return refuse(Reason::Material);
		}

		remixapi_InstanceInfoBlendEXT blend;
		if (!material::draw_state_blend(dev, true, mat.alpha_dropped, blend, reject))
		{
			log_once(tex, "old route (blend state)", detail);
			return refuse(Reason::Blend);
		}

		// The mesh: this draw of this geometry, refreshed while its shape holds.
		Hash64 kh;
		kh.mix(static_cast<uint64_t>(geo));
		kh.mix(g_ordinal[geo]++);
		const uint64_t key = kh.get();
		System& sys = g_systems[key];
		const bool create = !sys.mesh || sys.verts != hv.size() || sys.indices != g.tris.size() || sys.material_key != mkey;
		if (create)
		{
			if (const auto rt = g_retry_at.find(key); rt != g_retry_at.end() && now < rt->second) return refuse(Reason::Retry);
			if (!take_build_budget(key, static_cast<uint32_t>(hv.size()))) return refuse(Reason::Budget);
			destroy_system(sys);
			sys.hash = next_object_hash(key, kSparkDomain);
		}
		remixapi_MeshInfoSurfaceTriangles surf = {};
		surf.vertices_values = hv.data();
		surf.vertices_count = static_cast<uint64_t>(hv.size());
		surf.indices_values = g.tris.data();
		surf.indices_count = static_cast<uint64_t>(g.tris.size());
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
			log_once(tex, "old route (CreateMesh failed)", detail);
			return refuse(Reason::Build);
		}
		if (create)
		{
			sys.mesh = h;
			sys.verts = hv.size();
			sys.indices = g.tris.size();
			sys.material_key = mkey;
			g_c.built++;
		}
		else g_c.refreshed++;
		sys.last_frame = now;

		remixapi_InstanceInfo ii = {};
		ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		ii.pNext = &blend;
		ii.categoryFlags = mat.categories | REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE |
			REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP | api_camera::view_model_categories(dev);
		ii.mesh = sys.mesh;
		ii.transform = frame_transform(w);
		ii.doubleSided = 1;   // camera-facing cards
		if (b->DrawInstance(&ii) != REMIXAPI_ERROR_CODE_SUCCESS) return refuse(Reason::Submit);
		g_c.sent++;
		g_total++;
		log_once(tex, "API particles", detail);
		return true;
	}

	void gpu_particles_on_present()
	{
		g_ordinal.clear();
		const UINT now = frame();
		for (auto it = g_systems.begin(); it != g_systems.end();)
		{
			if (now - it->second.last_frame > kForgetFrames)
			{
				destroy_system(it->second);
				g_c.evicted++;
				it = g_systems.erase(it);
			}
			else ++it;
		}
		if ((now & 63u) != 0) return;
		std::erase_if(g_retry_at, [now](const auto& p) { return now >= p.second; });
		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.evict_frames);
		for (auto it = g_mats.begin(); it != g_mats.end();)
		{
			Mat& m = it->second;
			const bool used = std::any_of(g_systems.begin(), g_systems.end(),
				[&](const auto& s) { return s.second.material_key == it->first; });
			if (!used && now - m.last_used > keep)
			{
				if (m.handle) if (auto* b = bridge()) b->DestroyMaterial(m.handle);
				for (const uint64_t t : m.textures) textures::release(t);
				it = g_mats.erase(it);
			}
			else ++it;
		}
	}

	void gpu_particles_drop_all()
	{
		for (auto& [key, s] : g_systems) destroy_system(s);
		g_systems.clear();
		for (auto& [key, m] : g_mats)
		{
			if (m.handle) if (auto* b = bridge()) b->DestroyMaterial(m.handle);
			for (const uint64_t t : m.textures) textures::release(t);
		}
		g_mats.clear();
		g_retry_at.clear();
		g_ordinal.clear();
	}

	std::string gpu_particles_status()
	{
		if (!g_c.draws && g_systems.empty()) return {};
		std::string reasons;
		for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
			if (g_c.refused[i]) reasons += std::format("{}{}={}", reasons.empty() ? "" : " ", kReasonNames[i], g_c.refused[i]);
		const std::string s = std::format("gpu particles (effect-shader sparks, since the last line): draws={} sent={} built={} "
			"refreshed={} particles={} live={} systems={} materials={} evicted={} oldRoute=({}) | sent since start {}",
			g_c.draws, g_c.sent, g_c.built, g_c.refreshed, g_c.particles, g_c.live, g_systems.size(), g_mats.size(), g_c.evicted,
			reasons.empty() ? std::string("none") : reasons, g_total);
		g_c = {};
		return s;
	}
}
