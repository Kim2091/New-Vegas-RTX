#include "std_include.hpp"
#include "api_material.hpp"
#include "api_mesh.hpp"
#include "api_identity.hpp"
#include "api_rtxconf.hpp"
#include "scene_graph.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"

// See api_material.hpp. The draw route and the scene graph build the same
// material from the same description.

namespace comp::game::api::material
{
	bool blend_test_as_cutout(Desc& m)
	{
		if (!m.blend || (m.alpha_func != D3DCMP_GREATER && m.alpha_func != D3DCMP_GREATEREQUAL)) return false;
		m.blend = false;
		return true;
	}

	uint64_t key(const Desc& m)
	{
		Hash64 h;
		for (auto* t : { m.diffuse, m.normal, m.glow, m.height, m.facegen0, m.facegen1, m.hair_layer })
		{
			h.ptr(t);
			h.mix(identity::texture_gen(t));
		}
		h.mix((static_cast<uint64_t>(m.plain) << 40) | (static_cast<uint64_t>(m.filter) << 48));
		h.mix(static_cast<uint64_t>(m.normal_role) | (static_cast<uint64_t>(m.gloss_alpha) << 1) |
			(static_cast<uint64_t>(m.alpha_func) << 8) | (static_cast<uint64_t>(m.alpha_ref) << 16) |
			(static_cast<uint64_t>(m.wrap_u) << 24) | (static_cast<uint64_t>(m.wrap_v) << 32));
		uint32_t gb;
		std::memcpy(&gb, &m.gloss, sizeof(gb));
		h.mix(gb);
		// The scene path's fields: draw-call alpha state and glow scale.
		uint32_t gs;
		std::memcpy(&gs, &m.glow_scale, sizeof(gs));
		h.mix(static_cast<uint64_t>(gs) | (static_cast<uint64_t>(m.blend) << 32));
		// Tints and the emissive override (unlit FX, coloured glow).
		h.add(m.glow_tint, sizeof(m.glow_tint));
		h.add(m.albedo_tint, sizeof(m.albedo_tint));
		h.pod(m.emissive_intensity);
		if (m.hair_bake) h.mix(m.hair_bake);
		return h.get();
	}

	uint8_t wrap_mode(DWORD address)
	{
		switch (address)
		{
		case D3DTADDRESS_CLAMP:  return 0;   // lss::Mdl::WrapMode::Clamp
		case D3DTADDRESS_MIRROR: return 2;   // Mirrored_Repeat
		case D3DTADDRESS_BORDER: return 3;   // Clip
		default:                 return 1;   // Repeat (WRAP, MIRRORONCE)
		}
	}

	void sampler_from_stage(IDirect3DDevice9* dev, uint8_t stage, Desc& m)
	{
		if (!dev || stage >= 8) return;
		DWORD au = D3DTADDRESS_WRAP, av = D3DTADDRESS_WRAP, mag = D3DTEXF_LINEAR;
		dev->GetSamplerState(stage, D3DSAMP_ADDRESSU, &au);
		dev->GetSamplerState(stage, D3DSAMP_ADDRESSV, &av);
		dev->GetSamplerState(stage, D3DSAMP_MAGFILTER, &mag);
		m.wrap_u = wrap_mode(au);
		m.wrap_v = wrap_mode(av);
		m.stage_wrap_u = m.wrap_u;
		m.stage_wrap_v = m.wrap_v;
		m.filter = static_cast<uint8_t>(mag == D3DTEXF_POINT ? 0 : 1);   // VK_FILTER_NEAREST / LINEAR, as vkToMdl(magFilter)
		m.albedo_stage = stage;
		m.address_source = AddressSource::Stage;
	}

	bool wrap_from_clamp_mode(uint32_t mode, Desc& m)
	{
		if (mode > 3) return false;
		m.wrap_u = static_cast<uint8_t>((mode & 2u) ? 1 : 0);   // WRAP_S_*: modes 2, 3
		m.wrap_v = static_cast<uint8_t>((mode & 1u) ? 1 : 0);   // *_WRAP_T: modes 1, 3
		m.address_source = AddressSource::Engine;
		return true;
	}

	bool clamp_from_geometry(uintptr_t geo, Desc& m)
	{
		if (!geo) return false;
		scene_graph::GeoRead gr;
		if (!scene_graph::read_geo(geo, gr)) return false;
		uint32_t mode = kNoClampMode;
		const char* cls = nullptr;
		if (!scene_graph::read_clamp_mode(gr.props[scene_graph::fnv::kPropShade], mode, cls)) return false;
		m.clamp_class = cls;
		m.clamp_raw = mode;
		return wrap_from_clamp_mode(mode, m);
	}

	const char* wrap_name(uint8_t wrap)
	{
		switch (wrap)
		{
		case 0:  return "clamp";
		case 1:  return "wrap";
		case 2:  return "mirror";
		case 3:  return "border";
		default: return "?";
		}
	}

	const char* source_name(AddressSource s)
	{
		switch (s)
		{
		case AddressSource::Stage:  return "stage sampler";
		case AddressSource::Engine: return "engine clamp mode";
		default:                    return "default";
		}
	}

	namespace
	{
		bool white(const float (&t)[3]) { return t[0] == 1.0f && t[1] == 1.0f && t[2] == 1.0f; }

		// [Addressing] lines written (build's material lines and
		// log_mesh_addressing's mesh lines together).
		constexpr uint32_t kAddressingLogCap = 500;
		uint32_t g_addressing_lines = 0;

		bool addressing_line(std::string text)
		{
			if (g_addressing_lines >= kAddressingLogCap) return false;
			if (++g_addressing_lines == kAddressingLogCap) text += " (last [Addressing] line)";
			shared::common::log("Addressing", text, shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			return true;
		}

		// rtx.fnvProtocol.apiGlowTint as last pushed, and when.
		int g_glow_pushed = -1;            // -1 never, else 0 / 1
		UINT g_glow_push_frame = 0;
		constexpr UINT kGlowSettleFrames = 3;   // the runtime applies option layers at a frame boundary

		int g_albedo_tint = -1;            // -1 not checked, 0 no, 1 yes

		// IEEE half of a finite float (round to nearest; magnitudes below the
		// smallest normal half become 0, above the largest become infinity).
		uint16_t to_half(float f)
		{
			uint32_t x;
			std::memcpy(&x, &f, sizeof(x));
			const uint16_t sign = static_cast<uint16_t>((x >> 16) & 0x8000u);
			const int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
			uint32_t mant = x & 0x7FFFFFu;
			if (exp <= 0) return sign;
			if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
			uint32_t h = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
			if (mant & 0x1000u) h++;   // the carry into the exponent is the correct rounding
			return static_cast<uint16_t>(sign | (h > 0x7BFFu ? 0x7C00u : h));
		}

		// One FaceGen layer texture: every level of `tex` with each colour
		// channel c (0..1) mapped to scale * c + bias, alpha 1, as half floats.
		// 0 with `unreadable` set: the texture has no CPU copy or format this
		// can read. 0 otherwise: the upload failed.
		uint64_t facegen_layer(IDirect3DBaseTexture9* tex, float scale, float bias, const char*& reject, bool& unreadable)
		{
			uint32_t w = 0, h = 0, levels = 0;
			std::vector<uint8_t> px;
			unreadable = !textures::read_bgra8(tex, w, h, levels, px, reject);
			if (unreadable) return 0;
			std::vector<uint16_t> half(px.size());
			const uint16_t one = to_half(1.0f);
			for (size_t i = 0; i < px.size(); i += 4)
			{
				// B8G8R8A8 in, R16G16B16A16 out.
				for (int ch = 0; ch < 3; ch++)
					half[i + ch] = to_half(scale * (static_cast<float>(px[i + 2 - ch]) / 255.0f) + bias);
				half[i + 3] = one;
			}
			return textures::acquire_generated_half(w, h, levels, half.data(), half.size() * sizeof(uint16_t), reject);
		}

		// The hair layer from the LayerMap, at its largest level of at most
		// 512 texels: `color`, the LayerMap's colour with every texel outside
		// its strands (alpha < 1/4) filled from the nearest strand texels, and
		// `weights`, the blend weights R = 1 - w, G = w with w the LayerMap's
		// alpha widened by 2 texels. The BaseMap's strands reach past the
		// LayerMap's, and there the BaseMap's own colour (dark, the shader
		// lets the LayerMap carry the colour) would outline the hair; the game
		// hides that edge with alpha blending, a path tracer draws it.
		bool hair_layer_textures(IDirect3DBaseTexture9* tex, uint64_t& color, uint64_t& weights, const char*& reject)
		{
			color = weights = 0;
			uint32_t w = 0, h = 0, levels = 0;
			std::vector<uint8_t> px;
			if (!textures::read_bgra8(tex, w, h, levels, px, reject)) return false;
			size_t offset = 0;
			uint32_t first = 0;
			while (first + 1 < levels && (std::max)(1u, w >> first) > 512u)
			{
				offset += static_cast<size_t>((std::max)(1u, w >> first)) * (std::max)(1u, h >> first) * 4;
				first++;
			}
			const int fw = static_cast<int>((std::max)(1u, w >> first)), fh = static_cast<int>((std::max)(1u, h >> first));
			const size_t n = static_cast<size_t>(fw) * fh;
			const uint8_t* src = px.data() + offset;
			auto at = [&](int x, int y) { return static_cast<size_t>((y + fh) % fh) * fw + static_cast<size_t>((x + fw) % fw); };

			// Fill outward from the strands, one ring of texels per pass.
			std::vector<float> rgb(n * 3);
			std::vector<uint8_t> known(n);
			double sum[3] = {};
			size_t nknown = 0;
			for (size_t i = 0; i < n; i++)
			{
				for (int c = 0; c < 3; c++) rgb[i * 3 + c] = src[i * 4 + c];
				known[i] = src[i * 4 + 3] >= 64 ? 1 : 0;
				if (known[i]) { for (int c = 0; c < 3; c++) sum[c] += rgb[i * 3 + c]; nknown++; }
			}
			if (!nknown) { reject = "LayerMap has no opaque texel"; return false; }
			std::vector<uint8_t> next;
			for (int pass = 0; pass < 32; pass++)
			{
				next = known;
				bool grew = false;
				for (int y = 0; y < fh; y++)
					for (int x = 0; x < fw; x++)
					{
						const size_t i = at(x, y);
						if (known[i]) continue;
						float acc[3] = {};
						int cnt = 0;
						for (const auto& d : { std::pair{ 1, 0 }, std::pair{ -1, 0 }, std::pair{ 0, 1 }, std::pair{ 0, -1 } })
						{
							const size_t j = at(x + d.first, y + d.second);
							if (!known[j]) continue;
							for (int c = 0; c < 3; c++) acc[c] += rgb[j * 3 + c];
							cnt++;
						}
						if (!cnt) continue;
						for (int c = 0; c < 3; c++) rgb[i * 3 + c] = acc[c] / static_cast<float>(cnt);
						next[i] = 1;
						grew = true;
					}
				known.swap(next);
				if (!grew) break;
			}
			for (size_t i = 0; i < n; i++)
				if (!known[i])
					for (int c = 0; c < 3; c++) rgb[i * 3 + c] = static_cast<float>(sum[c] / static_cast<double>(nknown));

			// Every level from the top: colour as B8G8R8A8 (alpha 1), weights as
			// R8G8B8A8, each level the 2x2 average of the one above.
			std::vector<float> wv(n);
			for (int y = 0; y < fh; y++)
				for (int x = 0; x < fw; x++)
				{
					uint8_t m = 0;
					for (int dy = -2; dy <= 2; dy++)
						for (int dx = -2; dx <= 2; dx++) m = (std::max)(m, src[at(x + dx, y + dy) * 4 + 3]);
					wv[at(x, y)] = m;
				}
			std::vector<uint8_t> out_c, out_w;
			uint32_t mips = 0;
			int lw = fw, lh = fh;
			for (;;)
			{
				for (size_t i = 0; i < static_cast<size_t>(lw) * lh; i++)
				{
					for (int c = 0; c < 3; c++) out_c.push_back(static_cast<uint8_t>(std::clamp(rgb[i * 3 + c] + 0.5f, 0.0f, 255.0f)));
					out_c.push_back(255);
					const uint8_t wt = static_cast<uint8_t>(std::clamp(wv[i] + 0.5f, 0.0f, 255.0f));
					out_w.insert(out_w.end(), { static_cast<uint8_t>(255 - wt), wt, 0, 0 });
				}
				mips++;
				if (lw == 1 && lh == 1) break;
				const int nw = (std::max)(1, lw / 2), nh = (std::max)(1, lh / 2);
				std::vector<float> rgb2(static_cast<size_t>(nw) * nh * 3), wv2(static_cast<size_t>(nw) * nh);
				for (int y = 0; y < nh; y++)
					for (int x = 0; x < nw; x++)
					{
						const int x0 = (std::min)(2 * x, lw - 1), x1 = (std::min)(2 * x + 1, lw - 1);
						const int y0 = (std::min)(2 * y, lh - 1), y1 = (std::min)(2 * y + 1, lh - 1);
						const size_t o = static_cast<size_t>(y) * nw + x;
						const size_t q[4] = { static_cast<size_t>(y0) * lw + x0, static_cast<size_t>(y0) * lw + x1,
							static_cast<size_t>(y1) * lw + x0, static_cast<size_t>(y1) * lw + x1 };
						for (int c = 0; c < 3; c++)
							rgb2[o * 3 + c] = 0.25f * (rgb[q[0] * 3 + c] + rgb[q[1] * 3 + c] + rgb[q[2] * 3 + c] + rgb[q[3] * 3 + c]);
						wv2[o] = 0.25f * (wv[q[0]] + wv[q[1]] + wv[q[2]] + wv[q[3]]);
					}
				rgb.swap(rgb2);
				wv.swap(wv2);
				lw = nw;
				lh = nh;
			}
			color = textures::acquire_generated(static_cast<uint32_t>(fw), static_cast<uint32_t>(fh), mips, out_c.data(),
				out_c.size(), REMIXAPI_FORMAT_B8G8R8A8_UNORM, reject);
			if (!color) return false;
			weights = textures::acquire_generated(static_cast<uint32_t>(fw), static_cast<uint32_t>(fh), mips, out_w.data(),
				out_w.size(), REMIXAPI_FORMAT_R8G8B8A8_UNORM, reject);
			if (!weights) { textures::release(color); color = 0; return false; }
			return true;
		}

		// D3DBLEND -> VkBlendFactor; false for factors without one.
		bool vk_blend_factor(DWORD f, uint32_t& out)
		{
			switch (f)
			{
			case D3DBLEND_ZERO:            out = 0;  return true;   // VK_BLEND_FACTOR_ZERO
			case D3DBLEND_ONE:             out = 1;  return true;   // ONE
			case D3DBLEND_SRCCOLOR:        out = 2;  return true;   // SRC_COLOR
			case D3DBLEND_INVSRCCOLOR:     out = 3;  return true;   // ONE_MINUS_SRC_COLOR
			case D3DBLEND_DESTCOLOR:       out = 4;  return true;   // DST_COLOR
			case D3DBLEND_INVDESTCOLOR:    out = 5;  return true;   // ONE_MINUS_DST_COLOR
			case D3DBLEND_SRCALPHA:        out = 6;  return true;   // SRC_ALPHA
			case D3DBLEND_INVSRCALPHA:     out = 7;  return true;   // ONE_MINUS_SRC_ALPHA
			case D3DBLEND_DESTALPHA:       out = 8;  return true;   // DST_ALPHA
			case D3DBLEND_INVDESTALPHA:    out = 9;  return true;   // ONE_MINUS_DST_ALPHA
			case D3DBLEND_SRCALPHASAT:     out = 14; return true;   // SRC_ALPHA_SATURATE
			default:                       return false;
			}
		}

		// D3DBLENDOP -> VkBlendOp.
		bool vk_blend_op(DWORD op, uint32_t& out)
		{
			switch (op)
			{
			case D3DBLENDOP_ADD:         out = 0; return true;   // VK_BLEND_OP_ADD
			case D3DBLENDOP_SUBTRACT:    out = 1; return true;   // SUBTRACT
			case D3DBLENDOP_REVSUBTRACT: out = 2; return true;   // REVERSE_SUBTRACT
			case D3DBLENDOP_MIN:         out = 3; return true;   // MIN
			case D3DBLENDOP_MAX:         out = 4; return true;   // MAX
			default:                     return false;
			}
		}

		DWORD rs(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD fallback)
		{
			DWORD v = fallback;
			return SUCCEEDED(dev->GetRenderState(s, &v)) ? v : fallback;
		}
	}

	bool draw_state_blend(IDirect3DDevice9* dev, bool vertex_color, bool drop_alpha,
		remixapi_InstanceInfoBlendEXT& out, const char*& reject)
	{
		out = vertex_color_blend();
		if (vertex_color) out.textureAlphaOperation = 3;   // Modulate: TEXTURE x VertexColor0
		else
		{
			out.textureColorOperation = 1;                // SelectArg1: TEXTURE
			out.textureAlphaOperation = 1;
		}
		out.tFactor = rs(dev, D3DRS_TEXTUREFACTOR, 0xFFFFFFFFu);
		out.writeMask = rs(dev, D3DRS_COLORWRITEENABLE, 0xF) & 0xFu;
		// Unlit FX and particles: the vertex colour is a real colour (a tint,
		// a fade), not baked lighting the runtime should normalise away, as
		// the runtime's own particle system treats it.
		out.isVertexColorBakedLighting = 0;
		if (drop_alpha) return true;

		if (rs(dev, D3DRS_ALPHATESTENABLE, FALSE))
		{
			const DWORD func = rs(dev, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
			if (func < static_cast<DWORD>(D3DCMP_NEVER) || func > static_cast<DWORD>(D3DCMP_ALWAYS)) { reject = "alpha function"; return false; }
			if (func != D3DCMP_ALWAYS)
			{
				out.alphaTestEnabled = 1;
				out.alphaTestCompareOp = func - 1u;   // D3DCMPFUNC -> VkCompareOp
				out.alphaTestReferenceValue = static_cast<uint8_t>(rs(dev, D3DRS_ALPHAREF, 0) & 0xFF);
			}
		}
		if (!rs(dev, D3DRS_ALPHABLENDENABLE, FALSE)) return true;

		uint32_t src = 0, dst = 0, op = 0;
		if (!vk_blend_factor(rs(dev, D3DRS_SRCBLEND, D3DBLEND_ONE), src) ||
			!vk_blend_factor(rs(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO), dst) ||
			!vk_blend_op(rs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD), op))
		{
			reject = "blend factor / op";
			return false;
		}
		uint32_t asrc = src, adst = dst, aop = op;
		if (rs(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE) &&
			(!vk_blend_factor(rs(dev, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE), asrc) ||
			 !vk_blend_factor(rs(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO), adst) ||
			 !vk_blend_op(rs(dev, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD), aop)))
		{
			reject = "separate alpha blend factor / op";
			return false;
		}
		out.alphaBlendEnabled = 1;
		out.srcColorBlendFactor = src;
		out.dstColorBlendFactor = dst;
		out.colorBlendOp = op;
		out.srcAlphaBlendFactor = asrc;
		out.dstAlphaBlendFactor = adst;
		out.alphaBlendOp = aop;
		return true;
	}

	bool draw_is_additive(IDirect3DDevice9* dev)
	{
		if (!rs(dev, D3DRS_ALPHABLENDENABLE, FALSE)) return false;
		const DWORD src = rs(dev, D3DRS_SRCBLEND, D3DBLEND_ONE), dst = rs(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
		return dst == D3DBLEND_ONE && (src == D3DBLEND_ONE || src == D3DBLEND_SRCALPHA);
	}

	bool translucent_card(const remixapi_InstanceInfoBlendEXT& blend)
	{
		constexpr uint32_t kSrcAlpha = 6, kInvSrcAlpha = 7;   // VK_BLEND_FACTOR_SRC_ALPHA / ONE_MINUS_SRC_ALPHA
		if (!blend.alphaBlendEnabled) return false;
		return (blend.srcColorBlendFactor == kSrcAlpha && blend.dstColorBlendFactor == kInvSrcAlpha) ||
			(blend.srcColorBlendFactor == kInvSrcAlpha && blend.dstColorBlendFactor == kSrcAlpha);
	}

	const char* decal_rule_name(DecalRule r)
	{
		switch (r)
		{
		case DecalRule::EngineDecal:        return "engine decal flag (flags1 bit 26)";
		case DecalRule::EngineDynamicDecal: return "engine dynamic decal flag (flags1 bit 27)";
		case DecalRule::EngineAlphaDecal:   return "engine alpha decal flag (flags2 bit 21)";
		case DecalRule::RtxConf:            return "rtx.conf decal list";
		default:                            return "none";
		}
	}

	Decal decal_of(uintptr_t geo, IDirect3DDevice9* dev, uint32_t material_categories, bool moving)
	{
		namespace fnv = scene_graph::fnv;
		Decal d;
		if (geo)
		{
			scene_graph::GeoRead gr;
			scene_graph::ShadeFlags sf;
			if (scene_graph::read_geo(geo, gr) && scene_graph::read_shade_flags(gr.props[fnv::kPropShade], sf))
			{
				d.cls = sf.cls;
				d.flags1 = sf.flags1;
				d.flags2 = sf.flags2;
			}
		}
		if (dev)
		{
			d.state = true;
			d.blended = rs(dev, D3DRS_ALPHABLENDENABLE, FALSE) != 0;
			d.src = rs(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
			d.dst = rs(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
			d.zwrite = rs(dev, D3DRS_ZWRITEENABLE, TRUE);
			d.depth_bias = rs(dev, D3DRS_DEPTHBIAS, 0);
			d.slope_bias = rs(dev, D3DRS_SLOPESCALEDEPTHBIAS, 0);
		}
		if (shared::common::config::get().remix_api.scene_decal_category)
		{
			if (d.flags1 & fnv::SF1_dynamicDecal) d.rule = DecalRule::EngineDynamicDecal;
			else if (d.flags1 & fnv::SF1_decal) d.rule = DecalRule::EngineDecal;
			else if (d.flags2 & fnv::SF2_alphaDecal) d.rule = DecalRule::EngineAlphaDecal;
		}
		constexpr uint32_t kDecalBits = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC |
			REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET |
			REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET;
		if (d.rule != DecalRule::None)
			d.categories = (d.rule == DecalRule::EngineDynamicDecal || moving)
				? static_cast<uint32_t>(REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC)
				: static_cast<uint32_t>(REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC);
		else if (material_categories & kDecalBits) d.rule = DecalRule::RtxConf;
		return d;
	}

	namespace
	{
		constexpr uint32_t kDecalLogCap = 512;
		std::unordered_set<uint64_t> g_decal_logged;
	}

	void log_decal(const char* path, uintptr_t object, uint64_t albedo_d3d9, const Decal& d)
	{
		if (!shared::common::log_verbose()) return;
		if (!d.blended && d.rule == DecalRule::None) return;
		if (g_decal_logged.size() >= kDecalLogCap) return;
		Hash64 h;
		h.mix(reinterpret_cast<uintptr_t>(path));
		h.mix(static_cast<uint64_t>(object));
		h.mix(albedo_d3d9);
		if (!g_decal_logged.insert(h.get()).second) return;
		std::string text = std::format("path={} object=0x{:X} albedo={} class={} flags1=0x{:08X} flags2=0x{:08X}",
			path, object, albedo_d3d9 ? std::format("0x{:016X}", albedo_d3d9) : std::string("(no hash)"),
			d.cls ? d.cls : "(none)", d.flags1, d.flags2);
		if (d.state)
		{
			float bias = 0.0f, slope = 0.0f;
			std::memcpy(&bias, &d.depth_bias, sizeof(bias));
			std::memcpy(&slope, &d.slope_bias, sizeof(slope));
			text += std::format(" | blend {} src {} dst {} zwrite {} depthBias {:g} slopeBias {:g}", d.blended ? 1 : 0, d.src,
				d.dst, d.zwrite, bias, slope);
		}
		else text += std::format(" | blend {}", d.blended ? 1 : 0);
		text += d.categories
			? std::format(" -> {} ({})", (d.categories & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC) ? "DECAL_DYNAMIC"
				: "DECAL_STATIC", decal_rule_name(d.rule))
			: d.rule == DecalRule::RtxConf ? std::string(" -> decal by rtx.conf list") : std::string(" -> not a decal");
		if (g_decal_logged.size() == kDecalLogCap) text += " (last [Decal] line)";
		shared::common::log("Decal", text, shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	bool hair_tint(const shared::common::PsSlotMap* psmap, float (&tint)[3])
	{
		if (!psmap || psmap->hair_tint_reg == shared::common::PsSlotMap::kNoReg) return false;
		const float* pc = shared::common::ffp_state::get().ps_const_data();
		for (int k = 0; k < 3; k++)
		{
			const float t = pc[psmap->hair_tint_reg * 4 + k];
			tint[k] = std::isfinite(t) ? t : 0.5f;
		}
		return true;
	}

	uint32_t hair_color(float green, const float (&tint)[3], const float* scale)
	{
		uint8_t c[3];
		for (int k = 0; k < 3; k++)
		{
			const float v = (1.0f + green * (2.0f * tint[k] - 1.0f)) / (scale ? scale[k] : 1.0f);
			c[k] = !(v > 0.0f) ? 0 : v >= 1.0f ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f);
		}
		return D3DCOLOR_ARGB(255, c[0], c[1], c[2]);
	}

	bool hair_scale(const float (&tint)[3], float (&scale)[3])
	{
		const bool on = albedo_tint_supported();
		bool any = false;
		for (int k = 0; k < 3; k++)
		{
			scale[k] = on ? (std::max)(1.0f, 2.0f * tint[k]) : 1.0f;
			any = any || scale[k] > 1.0f;
		}
		return any;
	}

	bool hair_material_tint(const float (&tint)[3], float (&albedo_tint)[3])
	{
		if (!albedo_tint_supported()) return false;
		float scale[3];
		hair_scale(tint, scale);
		const float brightness = shared::common::config::get().hair.brightness;
		for (int k = 0; k < 3; k++) albedo_tint[k] = scale[k] * brightness;
		return true;
	}

	void log_hair(const char* path, uint32_t ps_hash, const char* texture, const float (&tint)[3])
	{
		if (!shared::common::log_verbose()) return;
		static std::unordered_set<uint64_t> s_logged;
		if (s_logged.size() >= 64) return;
		Hash64 h;
		h.mix(reinterpret_cast<uintptr_t>(path));
		h.mix(ps_hash);
		for (const float t : tint) h.mix(static_cast<uint64_t>(t * 1000.0f));
		if (!s_logged.insert(h.get()).second) return;
		shared::common::log("Hair", std::format("path={} ps 0x{:08X} tex '{}' HairTint ({:.3f}, {:.3f}, {:.3f}) -> baked into the "
			"vertex colours (1 + g (2 HairTint - 1))", path, ps_hash, texture ? texture : "?", tint[0], tint[1], tint[2]),
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	bool is_decal(uint32_t categories)
	{
		return (categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC |
			REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET)) != 0;
	}

	bool named_decal_layer(uintptr_t geo)
	{
		namespace fnv = scene_graph::fnv;
		scene_graph::GeoRead gr;
		if (!geo || !scene_graph::read_geo(geo, gr)) return false;
		const uintptr_t tex = scene_graph::ppl_texture(gr.props[fnv::kPropShade], 0);
		char name[260];
		if (!tex || !scene_graph::texture_name(tex, name, sizeof(name))) return false;
		for (char* p = name; *p; p++)
			if (*p >= 'A' && *p <= 'Z') *p = static_cast<char>(*p + ('a' - 'A'));
		return std::strstr(name, "decal") != nullptr;
	}

	bool shader_alpha(const Decal& d, remixapi_InstanceInfoBlendEXT& ext)
	{
		if (!d.cls || (d.flags1 & scene_graph::fnv::SF1_vertexAlpha)) return false;
		if (ext.textureAlphaOperation != 3) return false;   // not TEXTURE x VertexColor0
		ext.textureAlphaOperation = 1;                       // SelectArg1
		ext.textureAlphaArg1Source = 1;                      // Texture
		return true;
	}

	void append_reversed(std::vector<uint32_t>& tris)
	{
		const size_t n = tris.size() - tris.size() % 3;
		tris.reserve(n * 2);
		for (size_t t = 0; t < n; t += 3)
		{
			const uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
			tris.push_back(a);
			tris.push_back(c);
			tris.push_back(b);
		}
	}

	MeshFacts mesh_facts(const std::vector<remixapi_HardcodedVertex>& verts, const std::vector<uint32_t>& tris,
		bool vertex_color)
	{
		MeshFacts f;
		for (size_t t = 0; t + 2 < tris.size(); t += 3)
		{
			if (tris[t] >= verts.size() || tris[t + 1] >= verts.size() || tris[t + 2] >= verts.size()) continue;
			const remixapi_HardcodedVertex& a = verts[tris[t]];
			const remixapi_HardcodedVertex& b = verts[tris[t + 1]];
			const remixapi_HardcodedVertex& c = verts[tris[t + 2]];
			const float e1[3] = { b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2] };
			const float e2[3] = { c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2] };
			const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			float dot = 0.0f;
			for (int k = 0; k < 3; k++) dot += n[k] * (a.normal[k] + b.normal[k] + c.normal[k]);
			if (dot > 0.0f) f.wind_agree++;
			else if (dot < 0.0f) f.wind_disagree++;
		}
		if (vertex_color && !verts.empty())
		{
			f.alpha_lo = 255;
			f.alpha_hi = 0;
			for (const auto& v : verts)
			{
				const uint8_t al = static_cast<uint8_t>(v.color >> 24);   // B8G8R8A8: alpha in the top byte
				f.alpha_lo = (std::min)(f.alpha_lo, al);
				f.alpha_hi = (std::max)(f.alpha_hi, al);
			}
		}
		return f;
	}

	namespace
	{
		constexpr uint32_t kDecalRouteLogCap = 512;
		std::unordered_set<uint64_t> g_decal_route_logged;

		const char* cmp_name(uint32_t vk_op)
		{
			constexpr const char* kNames[] = { "NEVER", "LESS", "EQUAL", "LESSEQUAL", "GREATER", "NOTEQUAL", "GREATEREQUAL",
				"ALWAYS" };
			return vk_op < std::size(kNames) ? kNames[vk_op] : "?";
		}
	}

	void log_decal_route(const DecalRoute& r)
	{
		if (!shared::common::log_verbose()) return;
		if (g_decal_route_logged.size() >= kDecalRouteLogCap) return;
		Hash64 h;
		h.mix(reinterpret_cast<uintptr_t>(r.path));
		h.mix(static_cast<uint64_t>(r.object));
		h.mix(r.mesh_key);
		h.mix(r.categories);
		if (!g_decal_route_logged.insert(h.get()).second) return;

		// The alpha state the runtime derives (rtx_fork_material_derivation.cpp
		// deriveAlphaState): the instance's ext with useDrawCallAlphaState, else
		// the material's own test and no blend.
		const remixapi_InstanceInfoBlendEXT* e = r.ext;
		bool blend = false, test = false;
		uint32_t test_op = 7, test_ref = 0;
		std::string alpha;
		if (r.draw_alpha && e)
		{
			blend = e->alphaBlendEnabled != 0;
			test = e->alphaTestEnabled != 0 && e->alphaTestCompareOp != 7;
			test_op = e->alphaTestCompareOp;
			test_ref = e->alphaTestReferenceValue;
			alpha = std::format("draw state on the instance: blend {} (src {} dst {}), test {}", blend ? "on" : "off",
				e->srcColorBlendFactor, e->dstColorBlendFactor,
				test ? std::format("{} {}", cmp_name(test_op), test_ref) : std::string("off"));
		}
		else
		{
			test = r.alpha_func != 0 && r.alpha_func != D3DCMP_ALWAYS;
			test_op = r.alpha_func ? r.alpha_func - 1u : 7u;
			test_ref = r.alpha_ref;
			alpha = std::format("material: no blend, test {}",
				test ? std::format("{} {}", cmp_name(test_op), test_ref) : std::string("off"));
		}
		const char* runtime = blend ? "blended decal (unordered TLAS), opacity = alpha"
			: test ? "alpha-tested decal (unordered TLAS, decal route for test-only decals), opacity = test 0 / 1"
			: "fully opaque: NOT a decal in the runtime (traced as opaque geometry)";
		const char* alpha_src = !e ? "texture"
			: e->textureAlphaOperation == 3 ? "texture x vertex alpha"
			: e->textureAlphaOperation == 1 && e->textureAlphaArg1Source == 1 ? "texture" : "other";
		std::string text = std::format(
			"path={} object=0x{:X} albedo={} instance={} categories=0x{:X} ({}{}) | {} -> runtime: {} | alpha source {}{} "
			"| vertex alpha {}..{} | {} | winding vs vertex normals: agree {} disagree {}",
			r.path, r.object, r.albedo_d3d9 ? std::format("0x{:016X}", r.albedo_d3d9) : std::string("(no hash)"), r.instance,
			r.categories, (r.categories & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC) ? "DECAL_DYNAMIC" : "",
			(r.categories & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC)
				? ((r.categories & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC) ? " DECAL_STATIC" : "DECAL_STATIC") : "",
			alpha, runtime, alpha_src, r.vertex_alpha_dropped ? " (vertex alpha dropped: no Vertex_Alpha flag)" : "",
			r.mesh.alpha_lo, r.mesh.alpha_hi,
			r.double_sided ? (r.mesh.reversed_copy ? "two-sided (cull NONE): reversed copy of every triangle"
				: "two-sided (cull NONE)") : "single-sided",
			r.mesh.wind_agree, r.mesh.wind_disagree);
		if (g_decal_route_logged.size() == kDecalRouteLogCap) text += " (last [DecalRoute] line)";
		shared::common::log("DecalRoute", text, shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	bool glow_tint_ready()
	{
		return shared::common::config::get().remix_api.api_glow_tint && g_glow_pushed == 1 &&
			frame() - g_glow_push_frame >= kGlowSettleFrames;
	}

	bool glow_tint_pending()
	{
		return shared::common::config::get().remix_api.api_glow_tint && !glow_tint_ready();
	}

	bool albedo_tint_supported()
	{
		if (g_albedo_tint < 0)
		{
			static constexpr char kMarker[] = "Forwarding MaterialInfoOpaqueAlbedoTintEXT";
			const bool ok = bridge_client_has(kMarker);
			g_albedo_tint = ok ? 1 : 0;
			shared::common::log("RemixApi", ok
				? "materials: the bridge client forwards MaterialInfoOpaqueAlbedoTintEXT -- HDR tints ride on the material"
				: "materials: the bridge client does NOT forward MaterialInfoOpaqueAlbedoTintEXT (older bridge) -- tints "
				  "above 1 are clamped into the vertex colours",
				ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		}
		return g_albedo_tint == 1;
	}

	void push_runtime_options()
	{
		const int want = shared::common::config::get().remix_api.api_glow_tint ? 1 : 0;
		if (want == g_glow_pushed) return;
		auto* b = bridge();
		if (!b || !b->SetConfigVariable) return;
		if (b->SetConfigVariable("rtx.fnvProtocol.apiGlowTint", want ? "True" : "False") != REMIXAPI_ERROR_CODE_SUCCESS)
			return;
		g_glow_pushed = want;
		g_glow_push_frame = frame();
		shared::common::log("RemixApi", std::format("materials: rtx.fnvProtocol.apiGlowTint = {} (coloured glow / unlit tints "
			"{} through the API; a runtime without the option leaves API glow untinted)", want ? "True" : "False",
			want ? "go" : "no longer go"), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	void build(const Desc& md, uint64_t key, uint64_t domain, Built& m)
	{
		const auto& rc = rtxconf::scalars();
		auto decline = [&](Decline r) { m.why = r; };

		// The runtime tags a D3D9 draw by its albedo's own hash: the lists
		// naming it become the instance's categories. A list only D3D9 draws
		// can carry (lightConverter, uiTextures, ...: rtxconf::d3d9_only) declines;
		// the renderer sends those draws to D3D9 before any API path, so this
		// is reached only by paths with no D3D9 route (skinned, foliage); a
		// texture whose hash can't be reproduced is taken without categories.
		uint64_t d3d9 = 0;
		const bool hashed = rtxconf::d3d9_image_hash(md.diffuse, d3d9);
		if (hashed) m.albedo_d3d9 = d3d9;
		// Non-repeating, or the engine's clamp mode overrode a stage sampler
		// that said something else (either way round).
		const bool engine_override = md.address_source == AddressSource::Engine &&
			(md.wrap_u != md.stage_wrap_u || md.wrap_v != md.stage_wrap_v);
		if (shared::common::log_verbose() && (md.wrap_u != 1 || md.wrap_v != 1 || engine_override))
			addressing_line(std::format("material path={} albedo={} stage={} U={} V={} filter={} source={} class={} mode={}{}",
				md.path, hashed ? std::format("0x{:016X}", d3d9) : std::string("(no hash)"), md.albedo_stage,
				wrap_name(md.wrap_u), wrap_name(md.wrap_v), md.filter ? "linear" : "nearest", source_name(md.address_source),
				md.clamp_class ? md.clamp_class : "(none)",
				md.clamp_raw == kNoClampMode ? std::string("(none)") : std::to_string(md.clamp_raw),
				engine_override ? std::format(" (stage sampler said U={} V={})", wrap_name(md.stage_wrap_u),
					wrap_name(md.stage_wrap_v)) : std::string()));
		if (hashed)
			if (const rtxconf::Listing* l = rtxconf::lookup(d3d9))
			{
				m.list = l->d3d9_only ? l->d3d9_only : l->first;
				if (l->d3d9_only) return decline(Decline::RtxConf);
				m.categories = l->categories;
				m.alpha_dropped = l->drop_alpha;
			}
		m.no_vertex_color = md.plain || rc.ignore_all_vertex_color || (hashed && rtxconf::ignores_baked_lighting(d3d9));

		const char* reject = nullptr;
		auto take = [&](IDirect3DBaseTexture9* t, textures::Flavor f) -> uint64_t {
			const uint64_t h = textures::acquire(t, f, reject);
			if (h) m.textures.push_back(h);
			return h;
		};

		const uint64_t albedo = take(md.diffuse, textures::Flavor::Unorm);
		if (!albedo) return decline(Decline::Texture);

		if (md.plain)
		{
			// StaticsDebugPlainMaterial: the albedo alone, fully opaque, as
			// plain an API material as there is.
			const std::wstring p_plain = texture_path(albedo);
			remixapi_MaterialInfoOpaqueEXT op_plain;
			init_opaque_ext(op_plain, std::clamp(rc.legacy_roughness, 0.0f, 1.0f));
			remixapi_MaterialInfo mi_plain;
			init_material_info(mi_plain, next_object_hash(key, domain), &op_plain);
			mi_plain.albedoTexture = p_plain.c_str();
			mi_plain.filterMode = md.filter;
			mi_plain.wrapModeU = md.wrap_u;
			mi_plain.wrapModeV = md.wrap_v;
			m.handle = create_material(mi_plain);
			if (!m.handle) return decline(Decline::Build);
			return;
		}
		uint64_t normal = 0, glow = 0, height = 0;
		if (md.normal)
		{
			normal = take(md.normal, md.gloss_alpha ? textures::Flavor::NormalMapFormatAlpha : textures::Flavor::NormalMapNoGloss);
			if (!normal) return decline(Decline::Texture);
		}
		const float glow_intensity = md.emissive_intensity >= 0.0f ? md.emissive_intensity
			: rc.glow_intensity * md.glow_scale;
		const bool use_glow = md.glow && glow_intensity > 0.0f;
		if (use_glow)
		{
			glow = take(md.glow, textures::Flavor::Unorm);
			if (!glow) return decline(Decline::Texture);
		}
		const bool use_height = md.height && rc.height_displace_in > 0.0f;
		if (use_height)
		{
			height = take(md.height, textures::Flavor::Unorm);
			if (!height) return decline(Decline::Texture);
		}
		// FaceGen skin (Desc::facegen0): the offset layer 2 FaceGenMap0 - 1 and
		// the tint 4 FaceGenMap1. Face maps are created MANAGED so they can be
		// read (tex_pool.hpp). One that still can't be read leaves the BaseMap
		// alone (a warning, once per reason). A failed upload retries.
		uint64_t fg_offset = 0, fg_tint = 0;
		bool facegen = md.facegen0 && md.facegen1;
		if (facegen)
		{
			bool unreadable = false;
			fg_offset = facegen_layer(md.facegen0, 2.0f, -1.0f, reject, unreadable);
			if (fg_offset) m.textures.push_back(fg_offset);
			if (fg_offset) fg_tint = facegen_layer(md.facegen1, 4.0f, 0.0f, reject, unreadable);
			if (fg_tint) m.textures.push_back(fg_tint);
			if (!fg_offset || !fg_tint)
			{
				if (!unreadable) return decline(Decline::Texture);
				static std::unordered_set<std::string> s_logged;
				if (s_logged.size() < 16 && s_logged.insert(reject ? reject : "").second)
					shared::common::log("RemixApi", std::format("FaceGen: unexpected: a FaceGen map can't be read ({}; "
						"face maps are kept MANAGED, so this one was not loaded from textures\\characters\\facemods\\ by the "
						"engine's texture file load) -- the skin shows its BaseMap alone", reject ? reject : "?"),
						shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				facegen = false;
			}
		}

		// Hair (Desc::hair_layer): the LayerMap as layer 1, weighted by its alpha.
		// Without it the BaseMap shows alone (dark where the colour is the layer's).
		uint64_t hair_layer = 0, hair_weights = 0;
		bool hair = md.hair_layer != nullptr && !facegen;
		if (hair)
		{
			if (hair_layer_textures(md.hair_layer, hair_layer, hair_weights, reject))
			{
				m.textures.push_back(hair_layer);
				m.textures.push_back(hair_weights);
			}
			if (!hair_layer || !hair_weights)
			{
				static bool s_logged = false;
				if (!s_logged)
				{
					s_logged = true;
					shared::common::log("RemixApi", std::format("hair: a LayerMap could not be used ({}) -- that hair shows "
						"its BaseMap alone", reject ? reject : "?"), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
				hair = false;
			}
		}
		// Hair baked from its own PS (Desc::hair_bake): the albedo itself.
		uint64_t baked = 0;
		if (md.hair_bake && !facegen)
		{
			if (!textures::add_ref(md.hair_bake)) return decline(Decline::Texture);
			baked = md.hair_bake;
			m.textures.push_back(baked);
		}

		// Path strings must outlive CreateMaterial.
		const std::wstring p_albedo = texture_path(albedo);
		const std::wstring p_baked = baked ? texture_path(baked) : std::wstring();
		const std::wstring p_normal = normal ? texture_path(normal) : std::wstring();
		const std::wstring p_glow = glow ? texture_path(glow) : std::wstring();
		const std::wstring p_height = height ? texture_path(height) : std::wstring();
		const std::wstring p_fg_offset = facegen ? texture_path(fg_offset) : std::wstring();
		const std::wstring p_fg_tint = facegen ? texture_path(fg_tint) : std::wstring();
		const std::wstring p_hair_layer = hair ? texture_path(hair_layer) : std::wstring();
		std::wstring p_weight;

		// Blinn-Phong exponent -> GGX alpha -> perceptual roughness, as
		// FnvMaterialProtocol::applyToOpaque decodes RS 169.
		const float roughness = md.gloss > 0.0f
			? std::sqrt(std::sqrt(2.0f / (md.gloss + 2.0f)))
			: std::clamp(rc.legacy_roughness, 0.0f, 1.0f);

		remixapi_MaterialInfoTerrainLayersEXT layer = {};
		remixapi_MaterialInfoOpaqueEXT op;
		init_opaque_ext(op, roughness);
		if (normal || facegen || hair)
		{
			// Layers at weight 1: the extension's RGB tangent-space decode and
			// per-layer gloss (read from the texture's format). Layer 0 is the
			// albedo. A FaceGen head adds the offset layer with the same normal
			// map (the blend averages the layers' normals and gloss) and the
			// tint, sampled at the mesh UV (weightUV = uv; the 1x1 weight
			// texture reads the same everywhere). Otherwise weightUV is the
			// constant (0.5, 0.5).
			// Hair: the LayerMap as layer 1, both weights from its alpha at the
			// mesh UV; baked hair: the bake as layer 0, alone.
			static const uint8_t kOneLayer[4] = { 255, 0, 0, 0 };
			static const uint8_t kTwoLayers[4] = { 255, 255, 0, 0 };
			uint64_t w = hair_weights;
			if (!hair)
			{
				w = textures::acquire_generated(1, 1, 1, facegen ? kTwoLayers : kOneLayer, sizeof(kOneLayer),
					REMIXAPI_FORMAT_R8G8B8A8_UNORM, reject);
				if (!w) return decline(Decline::Texture);
				m.textures.push_back(w);
			}
			p_weight = texture_path(w);

			layer.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TERRAIN_LAYERS_EXT;
			layer.pNext = nullptr;
			layer.layerCount = facegen || hair ? 2 : 1;
			for (int i = 0; i < REMIXAPI_TERRAIN_MAX_LAYERS; i++)
			{
				const bool used = i == 0 || ((facegen || hair) && i == 1);
				layer.albedoTextures[i] = i == 0 ? (baked ? p_baked.c_str() : p_albedo.c_str())
					: !used ? L"" : hair ? p_hair_layer.c_str() : p_fg_offset.c_str();
				layer.normalTextures[i] = used ? p_normal.c_str() : L"";
				layer.heightTextures[i] = L"";   // the static's height rides in OpaqueEXT.heightTexture
			}
			layer.weightTextures[0] = p_weight.c_str();
			layer.weightTextures[1] = L"";
			layer.tintTexture = facegen ? p_fg_tint.c_str() : L"";
			layer.weightUvScale = facegen || hair ? remixapi_Float2D{ 1.0f, 1.0f } : remixapi_Float2D{ 0.0f, 0.0f };
			layer.weightUvOffset = facegen || hair ? remixapi_Float2D{ 0.0f, 0.0f } : remixapi_Float2D{ 0.5f, 0.5f };
			layer.normalsRgbTangentSpace = 1;
			op.pNext = &layer;
		}
		// The part of a tint vertex colours can't hold (> 1), at the end of
		// the chain (the runtime finds it anywhere after OpaqueEXT).
		remixapi_MaterialInfoOpaqueAlbedoTintEXT albedo_tint = {};
		if (!white(md.albedo_tint) && albedo_tint_supported())
		{
			albedo_tint.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_ALBEDO_TINT_EXT;
			albedo_tint.pNext = nullptr;
			albedo_tint.albedoTint = { std::clamp(md.albedo_tint[0], 0.0f, 16.0f), std::clamp(md.albedo_tint[1], 0.0f, 16.0f),
				std::clamp(md.albedo_tint[2], 0.0f, 16.0f) };
			if (normal || facegen || hair) layer.pNext = &albedo_tint;
			else op.pNext = &albedo_tint;
		}
		if (use_height)
		{
			op.heightTexture = p_height.c_str();
			op.displaceIn = rc.height_displace_in;
			op.displaceOut = 0.0f;
		}
		if (m.alpha_dropped)
		{
			// rtx.ignoreAlphaOnTextures: the runtime's material ignores the
			// albedo's alpha, so the same draw through D3D9 is opaque whatever it blends /
			// tests: no alpha state here either (the material's defaults).
		}
		else if (md.blend)
		{
			// The instance's InstanceInfoBlendEXT is the alpha state (test and
			// blend), as a D3D9 draw's state is for its legacy material.
			op.useDrawCallAlphaState = 1;
		}
		else if (md.alpha_func)
		{
			op.alphaTestType = md.alpha_func - 1;   // D3DCMPFUNC -> AlphaTestType
			op.alphaReferenceValue = md.alpha_ref;
		}

		remixapi_MaterialInfo mi;
		init_material_info(mi, next_object_hash(key, domain), &op);
		// Opacity (cutout), and the terrain fallback. Baked hair without layers:
		// the bake, whose alpha is the BaseMap's.
		mi.albedoTexture = baked && !(normal || facegen || hair) ? p_baked.c_str() : p_albedo.c_str();
		mi.filterMode = md.filter;
		mi.wrapModeU = md.wrap_u;
		mi.wrapModeV = md.wrap_v;
		if (use_glow)
		{
			mi.emissiveTexture = p_glow.c_str();
			mi.emissiveIntensity = glow_intensity;
			// A tint other than white is applied by the runtime only with
			// rtx.fnvProtocol.apiGlowTint (callers check glow_tint_ready()).
			mi.emissiveColorConstant = { md.glow_tint[0], md.glow_tint[1], md.glow_tint[2] };
		}

		m.handle = create_material(mi);
		if (!m.handle) return decline(Decline::Build);
	}

	void log_mesh_addressing(const char* path, uintptr_t object, uint64_t albedo_d3d9, uint8_t wrap_u,
		uint8_t wrap_v, const std::vector<remixapi_HardcodedVertex>& verts)
	{
		if (!shared::common::log_verbose() || (wrap_u == 1 && wrap_v == 1) || verts.empty() ||
			g_addressing_lines >= kAddressingLogCap)
			return;
		float lo[2] = { verts[0].texcoord[0], verts[0].texcoord[1] };
		float hi[2] = { lo[0], lo[1] };
		for (const auto& v : verts)
			for (int k = 0; k < 2; k++)
			{
				lo[k] = (std::min)(lo[k], v.texcoord[k]);
				hi[k] = (std::max)(hi[k], v.texcoord[k]);
			}
		// A little slack: UVs of exactly 0 / 1 stored as halves or rounded.
		constexpr float kSlack = 1.0f / 512.0f;
		const bool out_u = wrap_u != 1 && (lo[0] < -kSlack || hi[0] > 1.0f + kSlack);
		const bool out_v = wrap_v != 1 && (lo[1] < -kSlack || hi[1] > 1.0f + kSlack);
		addressing_line(std::format("mesh path={} object=0x{:X} albedo={} U={} V={} uv=[{:.3f}..{:.3f}]x[{:.3f}..{:.3f}] "
			"leaves [0,1] on a non-repeating axis: {}",
			path, object, albedo_d3d9 ? std::format("0x{:016X}", albedo_d3d9) : std::string("(no hash)"),
			wrap_name(wrap_u), wrap_name(wrap_v), lo[0], hi[0], lo[1], hi[1],
			out_u && out_v ? "U+V" : out_u ? "U" : out_v ? "V" : "no"));
	}

	remixapi_InstanceInfoBlendEXT vertex_color_blend()
	{
		remixapi_InstanceInfoBlendEXT blend = {};
		blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
		blend.pNext = nullptr;
		blend.alphaTestEnabled = 0;
		blend.alphaTestReferenceValue = 0;
		blend.alphaTestCompareOp = 7;          // VK_COMPARE_OP_ALWAYS
		blend.alphaBlendEnabled = 0;
		blend.srcColorBlendFactor = 1;         // VK_BLEND_FACTOR_ONE
		blend.dstColorBlendFactor = 0;         // VK_BLEND_FACTOR_ZERO
		blend.colorBlendOp = 0;                // VK_BLEND_OP_ADD
		blend.textureColorArg1Source = 1;      // RtTextureArgSource::Texture
		blend.textureColorArg2Source = 2;      // RtTextureArgSource::VertexColor0
		blend.textureColorOperation = 3;       // DxvkRtTextureOperation::Modulate
		blend.textureAlphaArg1Source = 1;      // Texture
		blend.textureAlphaArg2Source = 2;      // VertexColor0
		blend.textureAlphaOperation = 1;       // SelectArg1
		blend.tFactor = 0xFFFFFFFFu;
		blend.isTextureFactorBlend = 0;
		blend.srcAlphaBlendFactor = 1;
		blend.dstAlphaBlendFactor = 0;
		blend.alphaBlendOp = 0;
		blend.writeMask = 0xF;
		blend.isVertexColorBakedLighting = rtxconf::scalars().vertex_color_baked ? 1 : 0;
		return blend;
	}

	remixapi_InstanceInfoBlendEXT alpha_blend(uint8_t alpha_func, uint8_t alpha_ref, bool vertex_color)
	{
		remixapi_InstanceInfoBlendEXT blend = vertex_color_blend();
		if (alpha_func && alpha_func != D3DCMP_ALWAYS)
		{
			blend.alphaTestEnabled = 1;
			blend.alphaTestCompareOp = alpha_func - 1u;   // D3DCMPFUNC -> VkCompareOp
			blend.alphaTestReferenceValue = alpha_ref;
		}
		blend.alphaBlendEnabled = 1;
		blend.srcColorBlendFactor = 6;         // VK_BLEND_FACTOR_SRC_ALPHA
		blend.dstColorBlendFactor = 7;         // VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
		blend.srcAlphaBlendFactor = 6;         // D3D9 blends alpha with the colour factors
		blend.dstAlphaBlendFactor = 7;
		if (vertex_color)
			blend.textureAlphaOperation = 3;   // Modulate: TEXTURE * VertexColor0 (stage 0 ALPHAOP MODULATE)
		return blend;
	}
}
