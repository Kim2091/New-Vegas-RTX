#include "std_include.hpp"
#include "shader_bake.hpp"
#include "ps_interp.hpp"

#include <chrono>
#include <format>

// See shader_bake.hpp. Textures are decoded on demand, a 4x4 block at a time,
// to the same floats a full float decode of the chain would give, so a bake
// holds only the captured bytes and a small block cache per texture.

namespace comp::game::shader_bake
{
	namespace
	{
		namespace pi = ps_interp;

		constexpr uint32_t kMaxBakeSide = 1024;
		// Chains larger than this go up as BC3 (one upload is capped at 4 MB).
		constexpr size_t kMaxRawChainBytes = 2u << 20;
		// Probe grid: kGrid x kGrid texels spread over the bake.
		constexpr uint32_t kGrid = 32;
		// Probe tolerances.
		constexpr float kTolEmissive = 1e-6f, kTolLinear = 1e-5f, kTolSeparable = 1e-4f, kTolPlan = 1e-6f,
			kTolAlpha = 1e-6f;

		bool is_block(TexFormat f) { return f == TexFormat::Dxt1 || f == TexFormat::Dxt3 || f == TexFormat::Dxt5; }

		// ---- Texel decode (B, G, R, A in; R, G, B, A floats out) ----------------

		void rgb565(uint16_t c, float o[3])
		{
			const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
			o[0] = static_cast<float>((r << 3) | (r >> 2)) / 255.0f;
			o[1] = static_cast<float>((g << 2) | (g >> 4)) / 255.0f;
			o[2] = static_cast<float>((b << 3) | (b >> 2)) / 255.0f;
		}

		// One DXT colour block; `dxt1` allows its 3-colour mode.
		void color_block(const uint8_t* p, bool dxt1, float out[16][4])
		{
			uint16_t c0, c1;
			uint32_t bits;
			std::memcpy(&c0, p, 2);
			std::memcpy(&c1, p + 2, 2);
			std::memcpy(&bits, p + 4, 4);
			float pal[4][4];
			rgb565(c0, pal[0]);
			rgb565(c1, pal[1]);
			pal[0][3] = pal[1][3] = 1.0f;
			if (!dxt1 || c0 > c1)
			{
				for (int k = 0; k < 3; k++)
				{
					pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3.0f;
					pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3.0f;
				}
				pal[2][3] = pal[3][3] = 1.0f;
			}
			else
			{
				for (int k = 0; k < 3; k++)
				{
					pal[2][k] = (pal[0][k] + pal[1][k]) / 2.0f;
					pal[3][k] = 0.0f;
				}
				pal[2][3] = 1.0f;
				pal[3][3] = 0.0f;
			}
			for (int i = 0; i < 16; i++)
			{
				const int idx = static_cast<int>((bits >> (2 * i)) & 3);
				for (int k = 0; k < 4; k++) out[i][k] = pal[idx][k];
			}
		}

		void alpha_block_dxt5(const uint8_t* p, float out[16][4])
		{
			const int a0 = p[0], a1 = p[1];
			float pal[8];
			pal[0] = static_cast<float>(a0) / 255.0f;
			pal[1] = static_cast<float>(a1) / 255.0f;
			if (a0 > a1)
				for (int i = 1; i < 7; i++) pal[i + 1] = static_cast<float>((7 - i) * a0 + i * a1) / 7.0f / 255.0f;
			else
			{
				for (int i = 1; i < 5; i++) pal[i + 1] = static_cast<float>((5 - i) * a0 + i * a1) / 5.0f / 255.0f;
				pal[6] = 0.0f;
				pal[7] = 1.0f;
			}
			uint64_t bits = 0;
			for (int i = 0; i < 6; i++) bits |= static_cast<uint64_t>(p[2 + i]) << (8 * i);
			for (int i = 0; i < 16; i++) out[i][3] = pal[(bits >> (3 * i)) & 7];
		}

		// A captured texture, sampled bilinear / trilinear with wrap addressing.
		class TexView
		{
		public:
			explicit TexView(const Texture& t) : tex_(t)
			{
				for (auto& b : cache_) b.key = ~0ull;
			}

			// Trilinear at `lod` mip levels (0 = top).
			void sample(float u, float v, float lod, float out[4])
			{
				lod = std::clamp(lod, 0.0f, static_cast<float>(tex_.levels - 1));
				const int l0 = static_cast<int>(std::floor(lod));
				const float f = lod - static_cast<float>(l0);
				bilinear(l0, u, v, out);
				if (f > 1e-6f && l0 + 1 < static_cast<int>(tex_.levels))
				{
					float o2[4];
					bilinear(l0 + 1, u, v, o2);
					for (int k = 0; k < 4; k++) out[k] = out[k] * (1 - f) + o2[k] * f;
				}
			}

			// The lod of a bake bake_w texels wide (levels_needed's).
			float bake_lod(uint32_t bake_w) const
			{
				return std::log2((std::max)(1.0f, static_cast<float>(tex_.width) / static_cast<float>(bake_w)));
			}

		private:
			struct Block
			{
				uint64_t key;
				float px[16][4];
			};

			const Level* level(int index) const
			{
				for (const auto& l : tex_.captured)
					if (static_cast<int>(l.index) == index) return &l;
				return nullptr;
			}

			// Texel (x, y) of level `m`, wrapped, as R, G, B, A.
			void texel(const Level& m, int x, int y, float out[4])
			{
				x %= static_cast<int>(m.width);
				if (x < 0) x += static_cast<int>(m.width);
				y %= static_cast<int>(m.height);
				if (y < 0) y += static_cast<int>(m.height);
				if (!is_block(tex_.format))
				{
					const uint8_t* p = m.bytes.data() + (static_cast<size_t>(y) * m.width + x) * 4;
					out[0] = static_cast<float>(p[2]) / 255.0f;
					out[1] = static_cast<float>(p[1]) / 255.0f;
					out[2] = static_cast<float>(p[0]) / 255.0f;
					out[3] = tex_.format == TexFormat::Bgra8 ? static_cast<float>(p[3]) / 255.0f : 1.0f;
					return;
				}
				const uint32_t bx = static_cast<uint32_t>(x) / 4, by = static_cast<uint32_t>(y) / 4;
				const uint64_t key = (static_cast<uint64_t>(m.index) << 48) | (static_cast<uint64_t>(by) << 24) | bx;
				Block& b = cache_[(bx * 7 + by * 131 + m.index * 977) & (kCache - 1)];
				if (b.key != key)
				{
					b.key = key;
					const uint32_t bw = (std::max)(1u, (m.width + 3) / 4);
					const size_t bs = tex_.format == TexFormat::Dxt1 ? 8 : 16;
					const uint8_t* p = m.bytes.data() + (static_cast<size_t>(by) * bw + bx) * bs;
					color_block(tex_.format == TexFormat::Dxt1 ? p : p + 8, tex_.format == TexFormat::Dxt1, b.px);
					if (tex_.format == TexFormat::Dxt3)
						for (int i = 0; i < 16; i++) b.px[i][3] = static_cast<float>((p[i / 2] >> (4 * (i & 1))) & 15) / 15.0f;
					else if (tex_.format == TexFormat::Dxt5)
						alpha_block_dxt5(p, b.px);
				}
				std::memcpy(out, b.px[(y & 3) * 4 + (x & 3)], 4 * sizeof(float));
			}

			void bilinear(int index, float u, float v, float out[4])
			{
				const Level* m = level(std::clamp(index, 0, static_cast<int>(tex_.levels) - 1));
				if (!m)
				{
					for (int k = 0; k < 4; k++) out[k] = 0.5f;
					return;
				}
				const float x = u * static_cast<float>(m->width) - 0.5f, y = v * static_cast<float>(m->height) - 0.5f;
				const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
				const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
				float t[4][4];
				texel(*m, x0, y0, t[0]);
				texel(*m, x0 + 1, y0, t[1]);
				texel(*m, x0, y0 + 1, t[2]);
				texel(*m, x0 + 1, y0 + 1, t[3]);
				for (int k = 0; k < 4; k++)
					out[k] = (t[0][k] * (1 - fx) + t[1][k] * fx) * (1 - fy) + (t[2][k] * (1 - fx) + t[3][k] * fx) * fy;
			}

			static constexpr uint32_t kCache = 256;
			const Texture& tex_;
			Block cache_[kCache];
		};

		// The job's samplers: BaseMap and LayerMap by CTAB stage; any other
		// stage reads a constant 0.5.
		class JobSampler final : public pi::Sampler
		{
		public:
			JobSampler(const Job& job, int base_stage, int layer_stage)
				: base_(job.base), layer_(job.layer), base_stage_(base_stage), layer_stage_(layer_stage),
				  lod_base_(base_.bake_lod(job.width)), lod_layer_(layer_.bake_lod(job.width))
			{
			}

			pi::Vec sample(int stage, const pi::Vec& c, float lod_arg, int mode) override
			{
				TexView* t = stage == base_stage_ ? &base_ : stage == layer_stage_ ? &layer_ : nullptr;
				if (!t) return { 0.5f, 0.5f, 0.5f, 0.5f };
				float lod = t == &base_ ? lod_base_ : lod_layer_;
				if (mode == 1) lod += lod_arg;
				else if (mode == 2) lod = lod_arg;
				float o[4];
				t->sample(c[0], c[1], lod, o);
				return { o[0], o[1], o[2], o[3] };
			}

			// BaseMap's alpha at the bake's lod.
			float base_alpha(float u, float v)
			{
				float o[4];
				base_.sample(u, v, lod_base_, o);
				return o[3];
			}

		private:
			TexView base_, layer_;
			int base_stage_, layer_stage_;
			float lod_base_, lod_layer_;
		};

		// Constants by CTAB name: lighting neutralised, the rest the draw's.
		bool set_constants(const pi::Program& p, pi::State& s, const Job& job, float ambient, std::string& why)
		{
			for (const auto& e : p.ctab)
			{
				if (e.regset == 3) continue;   // samplers
				if (e.regset != 2) { why = std::format("{} is a bool / int constant", e.name); return false; }
				for (int r = 0; r < e.count; r++) s.c[(e.reg + r) & 255] = { 0, 0, 0, 0 };
				const int c = e.reg;
				if (e.name == "AmbientColor") s.c[c] = { ambient, ambient, ambient, 1.0f };
				else if (e.name == "MatAlpha") s.c[c] = { 1.0f, 0.0f, 0.0f, 0.0f };   // alpha x1, ref 0, no fog mode
				else if (e.name == "EyePosition") s.c[c] = { 0.0f, 0.0f, 100.0f, 1.0f };
				else if (e.name != "LightData" && e.name != "PSLightColor" && e.name != "ToggleNumLights")
				{
					if (e.reg + e.count > 32)
					{
						why = std::format("{} (c{}) is beyond the tracked constants", e.name, e.reg);
						return false;
					}
					for (int r = 0; r < e.count; r++)
						for (int k = 0; k < 4; k++) s.c[c + r][k] = job.constants[(c + r) * 4 + k];
				}
			}
			pi::load_defs(p, s);
			return true;
		}

		// Inputs by declaration: TEXCOORD0 is the UV; TEXCOORD3..5 the identity
		// tangent frame; the vertex colour (g, g, g, 1); the rest 0.
		void set_inputs(const pi::Program& p, pi::State& s, float g, float u, float v)
		{
			auto frame = [](int k, pi::Vec& r) {
				r = { 0, 0, 0, 0 };
				r[k] = 1.0f;
			};
			if (p.major >= 3)
			{
				for (const auto& d : p.inputs)
				{
					if (d.type != pi::REG_INPUT) continue;
					pi::Vec& r = s.v[d.idx % 12];
					if (d.usage == 5)   // TEXCOORD
					{
						switch (d.usage_index)
						{
						case 0: r = { u, v, 0, 1 }; break;
						case 3: frame(0, r); break;
						case 4: frame(1, r); break;
						case 5: frame(2, r); break;
						default: r = { 0, 0, 0, 0 }; break;   // 6 position, 7 fog colour + factor
						}
					}
					else if (d.usage == 10) r = d.usage_index == 0 ? pi::Vec{ g, g, g, 1 } : pi::Vec{ 0, 0, 0, 0 };
				}
			}
			else
			{
				s.t[0] = { u, v, 0, 1 };
				frame(0, s.t[3]);
				frame(1, s.t[4]);
				frame(2, s.t[5]);
				s.t[6] = { 0, 0, 0, 1 };
				s.t[7] = { 0, 0, 0, 0 };
				s.v[0] = { g, g, g, 1 };
				s.v[1] = { 0, 0, 0, 0 };
			}
		}

		uint8_t input_varying(const pi::Program& p, uint8_t type, int idx)
		{
			if (p.major >= 3)
			{
				if (type != pi::REG_INPUT) return 0;
				for (const auto& d : p.inputs)
					if (d.type == pi::REG_INPUT && d.idx == idx && d.usage == 5 && d.usage_index == 0) return 3;
				return 0;
			}
			return (type == pi::REG_TEXTURE && idx == 0) ? 3 : 0;
		}

		int sampler_stage(const pi::Program& p, const char* name)
		{
			for (const auto& e : p.ctab)
				if (e.regset == 3 && e.name == name) return e.reg;
			return -1;
		}

		// One setting of the bake (ambient, g): the specialised program and the
		// unspecialised one, per texel.
		class Runner
		{
		public:
			Runner(const pi::Program& p, const Job& job, JobSampler& smp) : prog_(p), job_(job), ex_(p)
			{
				ex_.sampler = &smp;
			}

			bool setup(float ambient, float g, std::string& why)
			{
				g_ = g;
				base_ = pi::State{};
				if (!set_constants(prog_, base_, job_, ambient, why)) return false;
				// Reference run: every instruction, results kept for replay.
				const size_t n = prog_.code.size();
				cache_.assign(n, pi::Vec{});
				std::vector<pi::SrcValues> refs(n);
				pi::State s = base_;
				set_inputs(prog_, s, g_, 0.5f / static_cast<float>(job_.width), 0.5f / static_cast<float>(job_.height));
				for (size_t pc = 0; pc < n; pc++)
				{
					const pi::Inst& in = prog_.code[pc];
					if (!in.has_dst) continue;
					for (int si = 0; si < in.nsrc; si++) refs[pc][si] = ex_.read(s, in.src[si]);
					cache_[pc] = ex_.compute(s, in);
					ex_.write(s, in, cache_[pc]);
				}
				plan_ = pi::specialize(prog_, [&](uint8_t t, int i) { return input_varying(prog_, t, i); }, &refs);
				if (!plan_.ok) { why = "not specialised"; return false; }
				work_ = base_;
				return true;
			}

			const pi::Plan& plan() const { return plan_; }

			// The specialised program at texel (x, y).
			pi::Vec texel(uint32_t x, uint32_t y, bool& killed)
			{
				pi::State& s = work_;
				std::memset(s.r, 0, sizeof(s.r));
				std::memset(s.oc, 0, sizeof(s.oc));
				s.p = {};
				s.killed = false;
				set_inputs(prog_, s, g_, uv(x, job_.width), uv(y, job_.height));
				const size_t n = prog_.code.size();
				for (size_t pc = 0; pc < n; pc++)
				{
					const auto k = plan_.kind[pc];
					if (k == pi::Plan::Dead) continue;
					if (k == pi::Plan::Uniform) ex_.write(s, prog_.code[pc], cache_[pc]);
					else ex_.step(s, static_cast<int>(pc), loops_);
				}
				killed = s.killed;
				return s.oc[0];
			}

			// The whole program at texel (x, y).
			pi::Vec full(uint32_t x, uint32_t y, bool& killed)
			{
				pi::State s = base_;
				set_inputs(prog_, s, g_, uv(x, job_.width), uv(y, job_.height));
				ex_.run(s);
				killed = s.killed;
				return s.oc[0];
			}

			static float uv(uint32_t i, uint32_t n) { return (static_cast<float>(i) + 0.5f) / static_cast<float>(n); }

		private:
			const pi::Program& prog_;
			const Job& job_;
			pi::Exec ex_;
			pi::State base_, work_;
			std::vector<pi::Vec> cache_;
			pi::Plan plan_;
			std::vector<std::pair<int, int>> loops_;
			float g_ = 0.0f;
		};

		// The checks a program must pass before it is run at all.
		bool check_program(const pi::Program& p, int& base_stage, int& layer_stage, std::string& why)
		{
			if (!p.error.empty()) { why = p.error; return false; }
			if (p.flow) { why = "flow control (not specialised)"; return false; }
			base_stage = sampler_stage(p, "BaseMap");
			layer_stage = sampler_stage(p, "LayerMap");
			if (base_stage < 0 || layer_stage < 0) { why = "no BaseMap / LayerMap sampler"; return false; }
			if (!p.find("HairTint")) { why = "no HairTint constant"; return false; }
			return true;
		}

		// The residual may read only plain texld taps of BaseMap / LayerMap and
		// no derivatives.
		bool check_plan(const pi::Program& p, const pi::Plan& plan, int base_stage, int layer_stage, std::string& why)
		{
			for (size_t pc = 0; pc < p.code.size(); pc++)
			{
				if (plan.kind[pc] == pi::Plan::Dead) continue;
				const pi::Inst& in = p.code[pc];
				if (in.op == pi::OP_DSX || in.op == pi::OP_DSY)
				{
					why = std::format("the albedo uses a derivative ({})", pi::disasm(in));
					return false;
				}
				if (in.op != pi::OP_TEX && in.op != pi::OP_TEXLDL && in.op != pi::OP_TEXLDD) continue;
				const int stage = in.src[1].idx;
				if (stage != base_stage && stage != layer_stage)
				{
					const char* name = "?";
					for (const auto& e : p.ctab)
						if (e.regset == 3 && e.reg == stage) name = e.name.c_str();
					why = std::format("the albedo samples {} (s{})", name, stage);
					return false;
				}
				if (in.op != pi::OP_TEX || in.ctrl == 2)
				{
					why = std::format("the albedo samples with a lod bias / explicit lod ({})", pi::disasm(in));
					return false;
				}
			}
			return true;
		}

		uint8_t unorm8(float x)
		{
			return static_cast<uint8_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		// ---- BC3 encode ----------------------------------------------------------

		uint16_t pack565(int r, int g, int b)
		{
			const int r5 = (r * 31 + 127) / 255, g6 = (g * 63 + 127) / 255, b5 = (b * 31 + 127) / 255;
			return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
		}

		void unpack565(uint16_t v, int& r, int& g, int& b)
		{
			const int r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
			r = (r5 << 3) | (r5 >> 2);
			g = (g6 << 2) | (g6 >> 4);
			b = (b5 << 3) | (b5 >> 2);
		}

		// The squared error of 16 B8G8R8A8 texels against the 4-colour palette of
		// endpoints e0 / e1, and the best index per texel.
		uint64_t color_error(const uint8_t (&px)[16][4], uint16_t e0, uint16_t e1, uint32_t& idx)
		{
			int pal[4][3];
			unpack565(e0, pal[0][0], pal[0][1], pal[0][2]);
			unpack565(e1, pal[1][0], pal[1][1], pal[1][2]);
			for (int ch = 0; ch < 3; ch++)
			{
				pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
				pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
			}
			uint64_t err = 0;
			idx = 0;
			for (int i = 0; i < 16; i++)
			{
				const int r = px[i][2], g = px[i][1], b = px[i][0];
				int best = 0, best_d = INT32_MAX;
				for (int k = 0; k < 4; k++)
				{
					const int dr = r - pal[k][0], dg = g - pal[k][1], db = b - pal[k][2];
					const int d = dr * dr + dg * dg + db * db;
					if (d < best_d) { best_d = d; best = k; }
				}
				err += static_cast<uint64_t>(best_d);
				idx |= static_cast<uint32_t>(best) << (2 * i);
			}
			return err;
		}

		// 16 B8G8R8A8 texels -> one BC3 block. Alpha: the block's min / max with
		// six interpolants. Colour (always 4-colour in BC3): the diagonal of the
		// colour bounding box with the smallest squared error, then the
		// least-squares endpoints for its indices while they lower the error.
		void encode_bc3_block(const uint8_t (&px)[16][4], uint8_t* dst)
		{
			int alo = 255, ahi = 0;
			for (const auto& p : px)
			{
				alo = (std::min)(alo, static_cast<int>(p[3]));
				ahi = (std::max)(ahi, static_cast<int>(p[3]));
			}
			uint64_t abits = 0;
			if (ahi > alo)
			{
				int pal[8];
				pal[0] = ahi;
				pal[1] = alo;
				for (int k = 1; k <= 6; k++) pal[k + 1] = ((7 - k) * ahi + k * alo) / 7;
				for (int i = 0; i < 16; i++)
				{
					int best = 0, best_d = 1 << 30;
					for (int k = 0; k < 8; k++)
					{
						const int d = std::abs(pal[k] - px[i][3]);
						if (d < best_d) { best_d = d; best = k; }
					}
					abits |= static_cast<uint64_t>(best) << (3 * i);
				}
			}
			dst[0] = static_cast<uint8_t>(ahi);
			dst[1] = static_cast<uint8_t>(alo);
			for (int i = 0; i < 6; i++) dst[2 + i] = static_cast<uint8_t>(abits >> (8 * i));

			int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
			for (const auto& p : px)
				for (int ch = 0; ch < 3; ch++)
				{
					const int v = p[2 - ch];   // BGRA -> r, g, b
					lo[ch] = (std::min)(lo[ch], v);
					hi[ch] = (std::max)(hi[ch], v);
				}
			uint64_t best_err = UINT64_MAX;
			uint16_t best_e0 = 0, best_e1 = 0;
			uint32_t best_idx = 0;
			for (int diag = 0; diag < 4; diag++)
			{
				// diag bit 0 flips green, bit 1 flips blue, relative to red.
				const int a[3] = { hi[0], (diag & 1) ? lo[1] : hi[1], (diag & 2) ? lo[2] : hi[2] };
				const int z[3] = { lo[0], (diag & 1) ? hi[1] : lo[1], (diag & 2) ? hi[2] : lo[2] };
				const uint16_t e0 = pack565(a[0], a[1], a[2]), e1 = pack565(z[0], z[1], z[2]);
				uint32_t idx = 0;
				const uint64_t err = color_error(px, e0, e1, idx);
				if (err < best_err) { best_err = err; best_e0 = e0; best_e1 = e1; best_idx = idx; }
			}
			// Index k weighs the endpoints (w0, w1) = (1, 0), (0, 1), (2/3, 1/3),
			// (1/3, 2/3); solve the 2x2 normal equations per channel.
			for (int pass = 0; pass < 2 && best_err > 0; pass++)
			{
				static constexpr float kW0[4] = { 1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
				float aa = 0, ab = 0, bb = 0, ax[3] = {}, bx[3] = {};
				for (int i = 0; i < 16; i++)
				{
					const float w0 = kW0[(best_idx >> (2 * i)) & 3], w1 = 1.0f - w0;
					aa += w0 * w0;
					ab += w0 * w1;
					bb += w1 * w1;
					for (int ch = 0; ch < 3; ch++)
					{
						ax[ch] += w0 * px[i][2 - ch];
						bx[ch] += w1 * px[i][2 - ch];
					}
				}
				const float det = aa * bb - ab * ab;
				if (std::fabs(det) < 1e-6f) break;
				int p0[3], p1[3];
				for (int ch = 0; ch < 3; ch++)
				{
					p0[ch] = static_cast<int>(std::clamp((ax[ch] * bb - bx[ch] * ab) / det + 0.5f, 0.0f, 255.0f));
					p1[ch] = static_cast<int>(std::clamp((bx[ch] * aa - ax[ch] * ab) / det + 0.5f, 0.0f, 255.0f));
				}
				const uint16_t e0 = pack565(p0[0], p0[1], p0[2]), e1 = pack565(p1[0], p1[1], p1[2]);
				uint32_t idx = 0;
				const uint64_t err = color_error(px, e0, e1, idx);
				if (err >= best_err) break;
				best_err = err;
				best_e0 = e0;
				best_e1 = e1;
				best_idx = idx;
			}
			uint8_t* c = dst + 8;
			c[0] = static_cast<uint8_t>(best_e0);
			c[1] = static_cast<uint8_t>(best_e0 >> 8);
			c[2] = static_cast<uint8_t>(best_e1);
			c[3] = static_cast<uint8_t>(best_e1 >> 8);
			for (int i = 0; i < 4; i++) c[4 + i] = static_cast<uint8_t>(best_idx >> (8 * i));
		}

		// A B8G8R8A8 chain (levels of w x h down to 1 x 1, tightly packed) as BC3.
		std::vector<uint8_t> chain_to_bc3(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, uint32_t mips)
		{
			std::vector<uint8_t> out;
			size_t at = 0;
			for (uint32_t l = 0; l < mips; l++)
			{
				const uint32_t lw = (std::max)(1u, w >> l), lh = (std::max)(1u, h >> l);
				const uint32_t bw = (std::max)(1u, (lw + 3) / 4), bh = (std::max)(1u, (lh + 3) / 4);
				const uint8_t* lvl = bgra.data() + at;
				const size_t o = out.size();
				out.resize(o + static_cast<size_t>(bw) * bh * 16);
				for (uint32_t by = 0; by < bh; by++)
					for (uint32_t bx = 0; bx < bw; bx++)
					{
						uint8_t px[16][4];
						for (uint32_t y = 0; y < 4; y++)
							for (uint32_t x = 0; x < 4; x++)
							{
								// Edge blocks of small mips repeat the last texel.
								const uint32_t sx = (std::min)(bx * 4 + x, lw - 1), sy = (std::min)(by * 4 + y, lh - 1);
								std::memcpy(px[y * 4 + x], lvl + (static_cast<size_t>(sy) * lw + sx) * 4, 4);
							}
						encode_bc3_block(px, out.data() + o + (static_cast<size_t>(by) * bw + bx) * 16);
					}
				at += static_cast<size_t>(lw) * lh * 4;
			}
			return out;
		}
	}

	size_t level_bytes(TexFormat f, uint32_t width, uint32_t height)
	{
		if (is_block(f))
		{
			const size_t bw = (std::max)(1u, (width + 3) / 4), bh = (std::max)(1u, (height + 3) / 4);
			return bw * bh * (f == TexFormat::Dxt1 ? 8u : 16u);
		}
		return static_cast<size_t>(width) * height * 4;
	}

	void bake_size(uint32_t base_w, uint32_t base_h, uint32_t& w, uint32_t& h)
	{
		w = (std::max)(1u, base_w);
		h = (std::max)(1u, base_h);
		while (w > kMaxBakeSide || h > kMaxBakeSide)
		{
			w = (std::max)(1u, w / 2);
			h = (std::max)(1u, h / 2);
		}
	}

	void levels_needed(uint32_t tex_w, uint32_t levels, uint32_t bake_w, uint32_t& first, uint32_t& count)
	{
		levels = (std::max)(1u, levels);
		float lod = std::log2((std::max)(1.0f, static_cast<float>(tex_w) / static_cast<float>((std::max)(1u, bake_w))));
		lod = std::clamp(lod, 0.0f, static_cast<float>(levels - 1));
		const int l0 = static_cast<int>(std::floor(lod));
		first = static_cast<uint32_t>(l0);
		count = (lod - static_cast<float>(l0) > 1e-6f && first + 1 < levels) ? 2u : 1u;
	}

	bool render(const Job& job, float ambient, float g, std::vector<float>& rgba, std::string& why)
	{
		const pi::Program p = pi::parse(job.bytecode.data(), job.bytecode.size());
		int base_stage = -1, layer_stage = -1;
		if (!check_program(p, base_stage, layer_stage, why)) return false;
		JobSampler smp(job, base_stage, layer_stage);
		Runner run(p, job, smp);
		if (!run.setup(ambient, g, why)) return false;
		rgba.assign(static_cast<size_t>(job.width) * job.height * 4, 0.0f);
		for (uint32_t y = 0; y < job.height; y++)
			for (uint32_t x = 0; x < job.width; x++)
			{
				bool killed = false;
				const pi::Vec o = run.texel(x, y, killed);
				float* d = &rgba[(static_cast<size_t>(y) * job.width + x) * 4];
				for (int k = 0; k < 4; k++) d[k] = o[k];
				if (killed) d[3] = 0.0f;
			}
		return true;
	}

	Result bake(const Job& job)
	{
		const auto t0 = std::chrono::steady_clock::now();
		Result res;
		auto done = [&](std::string why) {
			res.ok = why.empty();
			res.refusal = std::move(why);
			res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			if (!res.ok) res.data.clear();
			return std::move(res);
		};
		if (!job.width || !job.height) return done("empty bake");
		for (const Texture* t : { &job.base, &job.layer })
			for (const auto& l : t->captured)
				if (l.bytes.size() < level_bytes(t->format, l.width, l.height)) return done("short texture capture");

		const pi::Program p = pi::parse(job.bytecode.data(), job.bytecode.size());
		std::string why;
		int base_stage = -1, layer_stage = -1;
		if (!check_program(p, base_stage, layer_stage, why)) return done(why);
		res.instructions = static_cast<int>(p.code.size());
		const pi::CtabEntry* tint_e = p.find("HairTint");
		if (tint_e->regset != 2 || tint_e->reg >= 32) return done("HairTint is not a tracked float constant");
		const float* tint = job.constants + tint_e->reg * 4;

		JobSampler smp(job, base_stage, layer_stage);
		Runner main(p, job, smp);
		if (!main.setup(1.0f, 0.0f, why)) return done(why);
		if (!check_plan(p, main.plan(), base_stage, layer_stage, why)) return done(why);
		res.live = main.plan().live;
		res.varying = main.plan().varying;
		for (size_t pc = 0; pc < p.code.size(); pc++)
			if (main.plan().kind[pc] == pi::Plan::Varying)
			{
				if (!res.residual.empty()) res.residual += "; ";
				res.residual += pi::disasm(p.code[pc]);
			}

		// The probes, unspecialised, on the grid.
		Runner amb0(p, job, smp), amb_half(p, job, smp), g1(p, job, smp);
		if (!amb0.setup(0.0f, 0.0f, why) || !amb_half.setup(0.5f, 0.0f, why) || !g1.setup(1.0f, 1.0f, why)) return done(why);
		for (uint32_t gy = 0; gy < kGrid; gy++)
			for (uint32_t gx = 0; gx < kGrid; gx++)
			{
				const uint32_t x = static_cast<uint32_t>((2ull * gx + 1) * job.width / (2 * kGrid));
				const uint32_t y = static_cast<uint32_t>((2ull * gy + 1) * job.height / (2 * kGrid));
				bool k_spec = false, k_full = false, k_e = false, k_h = false, k_g = false;
				const pi::Vec spec = main.texel(x, y, k_spec);
				const pi::Vec a1 = main.full(x, y, k_full);
				const pi::Vec e = amb0.full(x, y, k_e);
				const pi::Vec ah = amb_half.full(x, y, k_h);
				const pi::Vec o1 = g1.full(x, y, k_g);
				for (int c = 0; c < 4; c++)
				{
					const float s = k_spec && c == 3 ? 0.0f : spec[c], f = k_full && c == 3 ? 0.0f : a1[c];
					if (!std::isfinite(s) || !std::isfinite(f)) return done("the output is not finite");
					res.plan_error = (std::max)(res.plan_error, std::fabs(s - f));
				}
				if (k_spec != k_full) res.plan_error = (std::max)(res.plan_error, 1.0f);
				for (int c = 0; c < 3; c++)
				{
					res.emissive = (std::max)(res.emissive, std::fabs(e[c]));
					res.linearity = (std::max)(res.linearity, std::fabs((ah[c] - e[c]) - 0.5f * (a1[c] - e[c])));
					const float want = a1[c] * (2.0f * tint[c]);
					res.separability = (std::max)(res.separability, std::fabs(o1[c] - want));
				}
				const float alpha = k_full ? 0.0f : a1[3];
				res.alpha_error = (std::max)(res.alpha_error,
					std::fabs(alpha - smp.base_alpha(Runner::uv(x, job.width), Runner::uv(y, job.height))));
			}
		if (res.emissive > kTolEmissive) return done(std::format("emissive residue {:.3g} (out(ambient 0) != 0)", res.emissive));
		if (res.linearity > kTolLinear) return done(std::format("not linear in the ambient term (error {:.3g})", res.linearity));
		if (res.separability > kTolSeparable)
			return done(std::format("the vertex colour is not a factor 1 + g (2 HairTint - 1) (error {:.3g})", res.separability));
		if (res.plan_error > kTolPlan) return done(std::format("specialised != unspecialised (error {:.3g})", res.plan_error));
		if (res.alpha_error > kTolAlpha) return done(std::format("the alpha is not the BaseMap's (error {:.3g})", res.alpha_error));

		// The bake: the top level as B8G8R8A8, then each level the 2x2 average
		// of the one above.
		const uint32_t w = job.width, h = job.height;
		uint32_t mips = 1;
		for (uint32_t lw = w, lh = h; lw > 1 || lh > 1; lw = (std::max)(1u, lw / 2), lh = (std::max)(1u, lh / 2)) mips++;
		size_t chain = 0;
		for (uint32_t l = 0; l < mips; l++) chain += static_cast<size_t>((std::max)(1u, w >> l)) * (std::max)(1u, h >> l) * 4;
		std::vector<uint8_t> bgra(chain);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
			{
				bool killed = false;
				const pi::Vec o = main.texel(x, y, killed);
				uint8_t* d = &bgra[(static_cast<size_t>(y) * w + x) * 4];
				if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2]) || !std::isfinite(o[3]))
					return done("the output is not finite");
				d[0] = unorm8(o[2]);
				d[1] = unorm8(o[1]);
				d[2] = unorm8(o[0]);
				d[3] = killed ? 0 : unorm8(o[3]);
			}
		size_t at = 0;
		for (uint32_t l = 1; l < mips; l++)
		{
			const uint32_t pw = (std::max)(1u, w >> (l - 1)), ph = (std::max)(1u, h >> (l - 1));
			const uint32_t lw = (std::max)(1u, w >> l), lh = (std::max)(1u, h >> l);
			const uint8_t* src = bgra.data() + at;
			uint8_t* dst = bgra.data() + at + static_cast<size_t>(pw) * ph * 4;
			for (uint32_t y = 0; y < lh; y++)
				for (uint32_t x = 0; x < lw; x++)
				{
					const uint32_t x0 = (std::min)(2 * x, pw - 1), x1 = (std::min)(2 * x + 1, pw - 1);
					const uint32_t y0 = (std::min)(2 * y, ph - 1), y1 = (std::min)(2 * y + 1, ph - 1);
					for (int c = 0; c < 4; c++)
					{
						const int sum = src[(static_cast<size_t>(y0) * pw + x0) * 4 + c] + src[(static_cast<size_t>(y0) * pw + x1) * 4 + c] +
							src[(static_cast<size_t>(y1) * pw + x0) * 4 + c] + src[(static_cast<size_t>(y1) * pw + x1) * 4 + c];
						dst[(static_cast<size_t>(y) * lw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
					}
				}
			at += static_cast<size_t>(pw) * ph * 4;
		}
		res.width = w;
		res.height = h;
		res.mips = mips;
		res.bc3 = chain > kMaxRawChainBytes;
		res.data = res.bc3 ? chain_to_bc3(bgra, w, h, mips) : std::move(bgra);
		return done({});
	}
}
