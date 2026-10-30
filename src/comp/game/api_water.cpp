#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/config.hpp"

#include <chrono>
#include <complex>

/*
 * FNV water through the Remix API.
 *
 * FNV's WATER shaders have no vertex normal and no colour texture: the
 * surface is an implicit +Z plane, its colour comes from constants
 * (ShallowColor / DeepColor), and its NoiseMap is a per-frame normal-map
 * render target. With the shipped prefs the near variants read only that
 * NoiseMap and the distant one (WATER034) reads nothing at all.
 *
 * So the water is rebuilt from scratch for the path tracer:
 *   * mesh    the draw's own vertices (object space), normal +Z, UV = world
 *             XY / tile size. World XY is anchored in ABSOLUTE world space
 *             (NiDX9Renderer WORLD is camera-relative, so the camera position
 *             is added back), computed once at creation; neighbouring cells'
 *             meshes therefore tile continuously.
 *   * material Translucent (IoR WaterIor, thin-wall off) whose transmittance
 *             colour / measurement distance come from the shader's
 *             ShallowColor and DeepColor, cached by quantised colour, with a
 *             procedurally generated tileable ripple normal map.
 *   * instance ANIMATED_WATER, so the runtime scrolls two normal samples.
 *
 * WATER036/037, the additive point-light specular passes, are dropped: the
 * path tracer lights the translucent surface itself.
 * WATER035 (a colour-less mask pass) is not a water surface and keeps its
 * route.
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;
	using shared::common::PsSlotMap;

	namespace
	{
		constexpr uint64_t kWaterDomain = 0x5741544552303031ull;   // "WATER001"
		constexpr UINT kRetryFrames = 120;
		// Fallback for WaterTileSize=0 before any water VS declared a TexScale.
		constexpr float kDefaultTile = 1024.0f;

		std::unordered_map<uint64_t, MeshEntry> g_water;
		std::unordered_map<uint64_t, UINT> g_retry_at;

		// Shared translucent materials, keyed by quantised look.
		struct SharedMaterial
		{
			remixapi_MaterialHandle handle = nullptr;
			uint32_t refs = 0;
		};
		std::unordered_map<uint64_t, SharedMaterial> g_materials;
		// Mesh entry key -> material key it holds.
		std::unordered_map<uint64_t, uint64_t> g_entry_material;

		uint64_t g_normal_tex = 0;
		int g_normal_res = 0;
		float g_normal_strength = -1.0f;

		bool g_logged_drop = false;

		// ---- Procedural ripple normal map ------------------------------------------
		//
		// One tile of a random wave field, synthesised in the frequency domain
		// (the way ocean height fields are built): every wave vector in a band
		// gets a Gaussian-random complex amplitude with a power-law spectrum and
		// a mild wind bias, and one inverse FFT turns the band into the surface
		// slopes. Every component is a whole number of cycles per tile, so the
		// tile wraps exactly, but no two components share a phase, so nothing
		// inside the tile repeats or lines up. The runtime's
		// animated-water path then adds a second sample of the same map at an
		// incommensurate scale and a different drift (push_runtime_options), so
		// the tile period never lines up with anything either.
		//
		// Spectrum: height amplitude ~ k^-2 per mode (Phillips-like), so the
		// slope variance is spread evenly over the octaves from ~5 m swells down
		// to ~8 cm ripples at the default 16 m tile. The slopes are normalised to
		// an RMS of kRmsSlope * WaterNormalStrength, a light-wind lake per
		// Cox-Munk (the second layer adds its own).
		//
		// Stored as BC5 (the translucent shader decodes an unsigned-octahedral
		// normal from .xy, so two channels are all it reads): 1024^2 with its
		// full mip chain is 1.3 MB, well under the bridge's 4 MB per-upload cap.
		// The mips average the SLOPES, which is the mean normal of the
		// footprint: distant water flattens toward the plane instead of
		// aliasing, which is the distance falloff the runtime has no knob for.

		constexpr float kRmsSlope = 0.075f;
		constexpr int kMaxNormalRes = 1024;

		using cfloat = std::complex<float>;

		// In-place radix-2 FFT of n (power of two) values spaced `stride` apart.
		void fft_1d(cfloat* a, size_t stride, int n, const std::vector<cfloat>& tw, const std::vector<int>& rev,
			std::vector<cfloat>& tmp)
		{
			for (int i = 0; i < n; i++) tmp[static_cast<size_t>(rev[static_cast<size_t>(i)])] = a[static_cast<size_t>(i) * stride];
			for (int len = 2; len <= n; len <<= 1)
			{
				const int half = len >> 1;
				const size_t step = static_cast<size_t>(n / len);
				for (int i = 0; i < n; i += len)
				{
					for (int j = 0; j < half; j++)
					{
						const cfloat u = tmp[static_cast<size_t>(i + j)];
						const cfloat v = tmp[static_cast<size_t>(i + j + half)] * tw[static_cast<size_t>(j) * step];
						tmp[static_cast<size_t>(i + j)] = u + v;
						tmp[static_cast<size_t>(i + j + half)] = u - v;
					}
				}
			}
			for (int i = 0; i < n; i++) a[static_cast<size_t>(i) * stride] = tmp[static_cast<size_t>(i)];
		}

		void fft_2d(std::vector<cfloat>& a, int n)
		{
			constexpr double kTwoPi = 6.283185307179586;
			std::vector<cfloat> tw(static_cast<size_t>(n / 2));
			for (int j = 0; j < n / 2; j++)
			{
				const double t = kTwoPi * static_cast<double>(j) / static_cast<double>(n);
				tw[static_cast<size_t>(j)] = cfloat(static_cast<float>(std::cos(t)), static_cast<float>(std::sin(t)));
			}
			int bits = 0;
			while ((1 << bits) < n) bits++;
			std::vector<int> rev(static_cast<size_t>(n));
			for (int i = 0; i < n; i++)
			{
				int r = 0;
				for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
				rev[static_cast<size_t>(i)] = r;
			}
			std::vector<cfloat> tmp(static_cast<size_t>(n));
			const size_t un = static_cast<size_t>(n);
			for (size_t y = 0; y < un; y++) fft_1d(&a[y * un], 1, n, tw, rev, tmp);
			for (size_t x = 0; x < un; x++) fft_1d(&a[x], un, n, tw, rev, tmp);
		}

		// splitmix64: deterministic, so every run (and every machine) builds the
		// same map, and the upload's content hash is stable.
		struct Rng
		{
			uint64_t s;
			uint64_t next()
			{
				uint64_t z = (s += 0x9E3779B97F4A7C15ull);
				z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
				z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
				return z ^ (z >> 31);
			}
			float uniform() { return (static_cast<float>(next() >> 40) + 0.5f) * (1.0f / 16777216.0f); }
			void gaussian2(float& a, float& b)
			{
				constexpr float kTwoPi = 6.28318530718f;
				const float r = std::sqrt(-2.0f * std::log(uniform()));
				const float t = kTwoPi * uniform();
				a = r * std::cos(t);
				b = r * std::sin(t);
			}
		};

		float smooth01(float e0, float e1, float x)
		{
			const float t = (std::clamp)((x - e0) / (e1 - e0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		// Unsigned octahedral (translucent_surface_material_interaction.slangh:
		// unsignedOctahedralToHemisphereDirection on .xy), in 0..255.
		void encode_octahedral(float sx, float sy, float& ex, float& ey)
		{
			const float x = -sx, y = -sy, z = 1.0f;   // normal of the height field, unnormalised
			const float s = std::fabs(x) + std::fabs(y) + z;
			const float px = x / s, py = y / s;
			ex = (std::clamp)(((px + py) * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f);
			ey = (std::clamp)(((px - py) * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f);
		}

		// One BC4 block (8 bytes) from 16 values in 0..255: endpoints bracket the
		// block (r0 > r1, the 8-value mode), each texel takes the nearest level.
		void bc4_block(const float* v, uint8_t* out)
		{
			float lo = v[0], hi = v[0];
			for (int i = 1; i < 16; i++) { lo = (std::min)(lo, v[i]); hi = (std::max)(hi, v[i]); }
			int r0 = (std::clamp)(static_cast<int>(std::ceil(hi)), 0, 255);
			int r1 = (std::clamp)(static_cast<int>(std::floor(lo)), 0, 255);
			if (r0 == r1) { if (r0 < 255) r0++; else r1--; }
			// The 8 levels are evenly spaced from r0 (step 0) to r1 (step 7);
			// BC4 numbers them 0, 2, 3, 4, 5, 6, 7, 1.
			static constexpr uint64_t kIndexOfStep[8] = { 0, 2, 3, 4, 5, 6, 7, 1 };
			const float to_step = 7.0f / static_cast<float>(r0 - r1);
			uint64_t bits = 0;
			for (int t = 0; t < 16; t++)
			{
				const int step = (std::clamp)(static_cast<int>((static_cast<float>(r0) - v[t]) * to_step + 0.5f), 0, 7);
				bits |= kIndexOfStep[step] << (3 * t);
			}
			out[0] = static_cast<uint8_t>(r0);
			out[1] = static_cast<uint8_t>(r1);
			for (int i = 0; i < 6; i++) out[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
		}

		// Appends one mip level (w x w slopes) as BC5 blocks.
		void append_bc5_level(const std::vector<float>& sx, const std::vector<float>& sy, int w, std::vector<uint8_t>& data)
		{
			const int bw = (std::max)(1, (w + 3) / 4);
			const size_t at = data.size();
			data.resize(at + static_cast<size_t>(bw) * static_cast<size_t>(bw) * 16);
			uint8_t* o = data.data() + at;
			for (int by = 0; by < bw; by++)
			{
				for (int bx = 0; bx < bw; bx++, o += 16)
				{
					float ex[16], ey[16];
					for (int t = 0; t < 16; t++)
					{
						const int x = (std::min)(bx * 4 + (t & 3), w - 1);
						const int y = (std::min)(by * 4 + (t >> 2), w - 1);
						const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
						encode_octahedral(sx[i], sy[i], ex[t], ey[t]);
					}
					bc4_block(ex, o);
					bc4_block(ey, o + 8);
				}
			}
		}

		uint64_t water_normal_texture(const char*& reject)
		{
			const auto& cfg = shared::common::config::get().remix_api;
			int res = 64;
			while (res * 2 <= (std::min)(cfg.water_normal_res, kMaxNormalRes)) res *= 2;
			if (g_normal_tex && g_normal_res == res && g_normal_strength == cfg.water_normal_strength)
				return g_normal_tex;

			const auto t0 = std::chrono::steady_clock::now();
			const int n = res;
			const size_t un = static_cast<size_t>(n);

			// Slope spectrum packed as SX + i*SY: both fields are real, so their
			// spectra are Hermitian and one complex inverse FFT yields sx in the
			// real part and sy in the imaginary part.
			std::vector<cfloat> spec(un * un, cfloat(0.0f, 0.0f));
			auto at = [n, un](int kx, int ky) {
				return static_cast<size_t>((ky + n) % n) * un + static_cast<size_t>((kx + n) % n);
			};
			// Band in cycles per tile: longest ~tile/3, shortest ~tile/200 (8 cm
			// at 16 m), and never past a third of the texture's Nyquist.
			const float kmin = 3.0f;
			const float kmax = (std::min)(static_cast<float>(n) / 3.0f, 200.0f);
			const float wind_x = 0.8f, wind_y = 0.6f;
			const cfloat i1(0.0f, 1.0f);
			Rng rng{ 0x464E5657415445ull };   // "FNVWATE"
			for (int ky = 0; ky < n / 2; ky++)
			{
				for (int kx = -n / 2 + 1; kx < n / 2; kx++)
				{
					if (ky == 0 && kx <= 0) continue;   // canonical half-plane; the mirror is set below
					const float fx = static_cast<float>(kx), fy = static_cast<float>(ky);
					const float k = std::sqrt(fx * fx + fy * fy);
					if (k < kmin * 0.5f || k > kmax) continue;
					const float band = smooth01(kmin * 0.5f, kmin, k) * (1.0f - smooth01(kmax * 0.6f, kmax, k));
					const float c = (fx * wind_x + fy * wind_y) / k;
					const float spread = 0.4f + 0.6f * c * c;   // waves both ways along the wind, some across
					const float amp = band * std::sqrt(spread) / (k * k);
					float g0, g1;
					rng.gaussian2(g0, g1);
					const cfloat h(amp * g0, amp * g1);
					const cfloat sxk = i1 * fx * h;
					const cfloat syk = i1 * fy * h;
					spec[at(kx, ky)] = sxk + i1 * syk;
					spec[at(-kx, -ky)] = std::conj(sxk) + i1 * std::conj(syk);
				}
			}
			fft_2d(spec, n);

			std::vector<float> sx(un * un), sy(un * un);
			double sum = 0.0;
			for (size_t i = 0; i < un * un; i++)
			{
				sx[i] = spec[i].real();
				sy[i] = spec[i].imag();
				sum += static_cast<double>(sx[i]) * sx[i] + static_cast<double>(sy[i]) * sy[i];
			}
			spec = {};
			const float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(un * un)));
			const float scale = rms > 0.0f ? kRmsSlope * cfg.water_normal_strength / rms : 0.0f;
			for (size_t i = 0; i < un * un; i++) { sx[i] *= scale; sy[i] *= scale; }

			std::vector<uint8_t> data;
			uint32_t mips = 0;
			int w = n;
			for (;;)
			{
				append_bc5_level(sx, sy, w, data);
				mips++;
				if (w == 1) break;
				const int h = w / 2;
				const size_t uh = static_cast<size_t>(h), uw = static_cast<size_t>(w);
				std::vector<float> nx(uh * uh), ny(uh * uh);
				for (size_t y = 0; y < uh; y++)
				{
					for (size_t x = 0; x < uh; x++)
					{
						const size_t a = (y * 2) * uw + x * 2;
						const size_t b = a + uw;
						const size_t o = y * uh + x;
						nx[o] = 0.25f * (sx[a] + sx[a + 1] + sx[b] + sx[b + 1]);
						ny[o] = 0.25f * (sy[a] + sy[a + 1] + sy[b] + sy[b + 1]);
					}
				}
				sx.swap(nx);
				sy.swap(ny);
				w = h;
			}

			const uint64_t hash = textures::acquire_generated(static_cast<uint32_t>(res), static_cast<uint32_t>(res), mips,
				data.data(), data.size(), REMIXAPI_FORMAT_BC5_UNORM, reject);
			if (!hash) return 0;
			// Held for the process lifetime (one texture); a changed INI value
			// adds a second one.
			g_normal_tex = hash;
			g_normal_res = res;
			g_normal_strength = cfg.water_normal_strength;
			const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			shared::common::log("RemixApi", std::format(
				"water: ripple normal map {}x{} BC5, {} mips, {:.2f} MB, generated in {:.0f} ms",
				res, res, mips, static_cast<double>(data.size()) / (1024.0 * 1024.0), ms),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			return hash;
		}

		// ---- Material ----------------------------------------------------------------

		struct WaterLook
		{
			float shallow[3] = { 0.35f, 0.45f, 0.45f };
			float deep[3] = { 0.10f, 0.15f, 0.15f };
		};

		// Quantised to 1/32: the key the materials are cached by, and all the
		// precision a transmittance colour needs.
		float quantise_colour(float v)
		{
			v = std::isfinite(v) ? (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) : 0.0f;
			return std::round(v * 32.0f) / 32.0f;
		}

		// The water shader's ShallowColor / DeepColor registers in every SM3
		// shader package (003-019; the SM2 package 002 has c5 / c6). A water
		// PS that doesn't declare them by name is read here.
		constexpr uint8_t kShallowColorReg = 2;
		constexpr uint8_t kDeepColorReg = 3;

		WaterLook read_look(const PsSlotMap* m)
		{
			const bool named = m && m->shallow_color_reg != PsSlotMap::kNoReg && m->deep_color_reg != PsSlotMap::kNoReg;
			const uint8_t shallow = named ? m->shallow_color_reg : kShallowColorReg;
			const uint8_t deep = named ? m->deep_color_reg : kDeepColorReg;
			if (!named)
			{
				static bool s_logged = false;
				if (!s_logged)
				{
					s_logged = true;
					shared::common::log("RemixApi", std::format(
						"water: PS 0x{:08X} doesn't name ShallowColor / DeepColor: read from the engine's c{} / c{}",
						m ? m->ps_hash : 0u, kShallowColorReg, kDeepColorReg), shared::common::LOG_TYPE::LOG_TYPE_WARN);
				}
			}
			WaterLook w;
			const float* pc = shared::common::ffp_state::get().ps_const_data();
			auto q = quantise_colour;
			for (int k = 0; k < 3; k++)
			{
				w.shallow[k] = q(pc[shallow * 4 + k]);
				w.deep[k] = q(pc[deep * 4 + k]);
			}
			return w;
		}

		uint64_t look_key(const WaterLook& w)
		{
			Hash64 h;
			h.mix(0x4C4F4F4Bull);
			for (int k = 0; k < 3; k++)
			{
				h.mix(static_cast<uint64_t>(w.shallow[k] * 32.0f));
				h.mix(static_cast<uint64_t>(w.deep[k] * 32.0f));
			}
			return h.get();
		}

		// ---- Distant (LOD) water look ---------------------------------------------------
		//
		// WATER034, the distant-water variant, is fed the worldspace's default
		// water colours, while the near variants (WATER000-033) get the colours
		// of the water type in the cell. Vanilla FNV hides the difference
		// behind its own distance fog and a flat LOD look, but as two separate
		// translucent media the seam where near water hands over to LOD water
		// is obvious. So the LOD water takes the look
		// of the near water in view (blended by WaterLodNearBlend; 1 = one
		// shared material): the look drawn most often by near water in a
		// frame, adopted once it has been the majority for kNearSettleFrames
		// frames so two water types on screen cannot flip the LOD material
		// (and rebuild its meshes) every frame. Before any near water has been
		// seen, LOD water keeps its own colours.
		constexpr UINT kNearSettleFrames = 30;

		struct LookVote
		{
			WaterLook look;
			uint32_t draws = 0;
		};
		std::unordered_map<uint64_t, LookVote> g_near_votes;   // this frame
		WaterLook g_near_look;
		uint64_t g_near_key = 0;
		bool g_near_valid = false;
		uint64_t g_near_candidate = 0;
		UINT g_near_candidate_frames = 0;

		void settle_near_look()
		{
			if (g_near_votes.empty()) return;   // no near water this frame: keep the current look
			auto best = g_near_votes.begin();
			for (auto it = g_near_votes.begin(); it != g_near_votes.end(); ++it)
				if (it->second.draws > best->second.draws) best = it;

			bool adopt = false;
			if (!g_near_valid) adopt = true;
			else if (best->first == g_near_key) g_near_candidate = 0;
			else if (best->first == g_near_candidate) adopt = ++g_near_candidate_frames >= kNearSettleFrames;
			else { g_near_candidate = best->first; g_near_candidate_frames = 1; }

			if (adopt)
			{
				g_near_look = best->second.look;
				g_near_key = best->first;
				g_near_valid = true;
				g_near_candidate = 0;
				const auto& l = g_near_look;
				shared::common::log("RemixApi", std::format(
					"water: LOD water follows near look shallow ({:.2f} {:.2f} {:.2f}) deep ({:.2f} {:.2f} {:.2f})",
					l.shallow[0], l.shallow[1], l.shallow[2], l.deep[0], l.deep[1], l.deep[2]),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
			g_near_votes.clear();
		}

		WaterLook lod_look(const WaterLook& own)
		{
			const float t = shared::common::config::get().remix_api.water_lod_near_blend;
			if (!g_near_valid || !(t > 0.0f)) return own;
			WaterLook w;
			for (int k = 0; k < 3; k++)
			{
				w.shallow[k] = quantise_colour(own.shallow[k] + (g_near_look.shallow[k] - own.shallow[k]) * t);
				w.deep[k] = quantise_colour(own.deep[k] + (g_near_look.deep[k] - own.deep[k]) * t);
			}
			return w;
		}

		// Heuristic mapping of FNV's two water colours onto a physical medium.
		// The path tracer's translucent medium only absorbs (no in-water
		// scattering), so what reads as "murky green" is light from the bed
		// losing its red and some green over a few metres, plus the Fresnel
		// reflection on top:
		//   hue       mostly DeepColor (the colour FNV converges to over depth)
		//             with some ShallowColor, normalised to its brightest
		//             channel and then scaled by WaterClarity, so even that
		//             channel absorbs. The shader linearises this value, so at
		//             the 0.8 default the dominant channel keeps ~61% per
		//             distance and a channel half as bright ~13%.
		//   distance  WaterMeasurementDistance scaled by the colours' mean
		//             brightness: dark water reads as murkier than bright water.
		struct Medium
		{
			float color[3];
			float distance;
		};

		Medium medium_of(const WaterLook& w)
		{
			const auto& cfg = shared::common::config::get().remix_api;
			float base[3], mean[3];
			for (int k = 0; k < 3; k++)
			{
				base[k] = 0.35f * w.shallow[k] + 0.65f * w.deep[k];
				mean[k] = 0.5f * (w.shallow[k] + w.deep[k]);
			}
			float m = (std::max)({ base[0], base[1], base[2] });
			float brightness = (std::max)({ mean[0], mean[1], mean[2] });
			if (m < 1e-3f)
			{
				base[0] = 0.25f; base[1] = 0.40f; base[2] = 0.38f; m = 0.40f;
				brightness = 0.35f;
			}
			const float clarity = cfg.water_clarity;
			Medium r;
			for (int k = 0; k < 3; k++) r.color[k] = (std::clamp)(base[k] / m * clarity, 0.02f, 0.98f);
			r.distance = cfg.water_measurement_distance * (std::clamp)(brightness / 0.35f, 0.25f, 2.0f);
			return r;
		}

		remixapi_MaterialHandle acquire_material(const WaterLook& w, uint64_t key, uint64_t normal_tex)
		{
			if (auto it = g_materials.find(key); it != g_materials.end())
			{
				it->second.refs++;
				return it->second.handle;
			}

			const auto& cfg = shared::common::config::get().remix_api;
			const Medium med = medium_of(w);

			remixapi_MaterialInfoTranslucentEXT tr = {};
			tr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
			tr.pNext = nullptr;
			tr.transmittanceTexture = L"";
			tr.refractiveIndex = cfg.water_ior;
			tr.transmittanceColor = { med.color[0], med.color[1], med.color[2] };
			tr.transmittanceMeasurementDistance = static_cast<float>(med.distance * stable_origin::scale());
			tr.thinWallThickness_hasvalue = 0;
			tr.thinWallThickness_value = 0.0f;
			tr.useDiffuseLayer = 0;

			const std::wstring p_normal = texture_path(normal_tex);
			remixapi_MaterialInfo mi;
			init_material_info(mi, next_object_hash(key, kWaterDomain ^ 0x4D4154ull), &tr);
			mi.normalTexture = p_normal.c_str();

			const auto h = create_material(mi);
			if (!h) return nullptr;
			g_materials[key] = { h, 1 };
			shared::common::log("RemixApi", std::format(
				"water: material shallow ({:.2f} {:.2f} {:.2f}) deep ({:.2f} {:.2f} {:.2f}) -> transmittance ({:.2f} {:.2f} {:.2f}) over {:.0f} units",
				w.shallow[0], w.shallow[1], w.shallow[2], w.deep[0], w.deep[1], w.deep[2],
				tr.transmittanceColor.x, tr.transmittanceColor.y, tr.transmittanceColor.z,
				tr.transmittanceMeasurementDistance),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			return h;
		}

		void release_material(uint64_t key)
		{
			const auto it = g_materials.find(key);
			if (it == g_materials.end()) return;
			if (it->second.refs > 1) { it->second.refs--; return; }
			if (auto* b = bridge()) b->DestroyMaterial(it->second.handle);
			g_materials.erase(it);
		}

		// Destroys a water entry: its mesh, then the shared material reference.
		void destroy_water_entry(uint64_t entry_key, MeshEntry& e)
		{
			e.material = nullptr;   // shared: released by key below, never by destroy_entry
			destroy_entry(e);
			if (const auto it = g_entry_material.find(entry_key); it != g_entry_material.end())
			{
				release_material(it->second);
				g_entry_material.erase(it);
			}
		}

		// Last TexScale.x a water VS declared, for water drawn by a VS that
		// doesn't (so near and distant water ripple at the same scale).
		float g_last_tile = kDefaultTile;

		float tile_size()
		{
			const auto& cfg = shared::common::config::get().remix_api;
			if (cfg.water_tile_size > 0.0f) return cfg.water_tile_size;
			const auto& ffp = shared::common::ffp_state::get();
			if (const auto* vd = shared::common::g_vs_classifier.classify(ffp.last_vs()))
			{
				if (vd->has(vd->reg_tex_scale))
				{
					const float t = ffp.vs_const_data()[vd->reg_tex_scale * 4];
					if (std::isfinite(t) && t > 1.0f && t < 1.0e6f) g_last_tile = t;
				}
			}
			return g_last_tile;
		}

		// ---- Runtime animated-water options -------------------------------------------
		//
		// The runtime shades ANIMATED_WATER translucents with two scrolled
		// samples of the normal map (rtx.translucentMaterial.animatedWater*),
		// whose velocities are in UV per second, so their defaults (0.05 UV/s,
		// and a second sample at the SAME scale with a one-mip blur) give ~1 m/s
		// drift and a doubled copy of the same tile. Pushed once per frame
		// at most, and only on change, from world-space INI values:
		//   primary    the map at WaterTileSize, drifting WaterFlowSpeed along
		//              the wind axis the map was synthesised with;
		//   secondary  the same map at WaterDetailScale (a larger, incommensurate
		//              scale: a swell layer that never lines up with the
		//              first), drifting WaterDetailFlowSpeed at ~95 degrees to it.
		// animatedWaterSecondaryNormalScale is a fork option; an older runtime
		// ignores it and keeps its same-scale second sample.
		struct PushedOption
		{
			const char* key;
			std::string last;
		};
		PushedOption g_opt_primary{ "rtx.translucentMaterial.animatedWaterPrimaryNormalMotion", {} };
		PushedOption g_opt_secondary{ "rtx.translucentMaterial.animatedWaterSecondaryNormalMotion", {} };
		PushedOption g_opt_scale{ "rtx.translucentMaterial.animatedWaterSecondaryNormalScale", {} };
		PushedOption g_opt_bias{ "rtx.translucentMaterial.animatedWaterSecondaryNormalLodBias", {} };
		UINT g_opt_frame = 0xFFFFFFFFu;

		void push_option(remixapi_Interface* b, PushedOption& o, const std::string& value)
		{
			if (o.last == value) return;
			if (b->SetConfigVariable(o.key, value.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS)
			{
				o.last = value;
				shared::common::log("RemixApi", std::format("water: {} = {}", o.key, value),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
		}

		void push_runtime_options(float tile)
		{
			const auto& cfg = shared::common::config::get().remix_api;
			if (!cfg.water_runtime_options) return;
			if (g_opt_frame == frame()) return;
			g_opt_frame = frame();
			auto* b = bridge();
			if (!b || !b->SetConfigVariable || !(tile > 0.0f)) return;

			// Directions: the map's wind axis, and a cross-drift for the swell.
			constexpr float kPx = 0.8f, kPy = 0.6f;
			constexpr float kSx = -0.515f, kSy = 0.857f;
			const float scale = cfg.water_detail_scale;
			const float vp = cfg.water_flow_speed / tile;                  // primary UV/s
			const float vs = cfg.water_detail_flow_speed * scale / tile;   // secondary, in its scaled UV
			push_option(b, g_opt_primary, std::format("{:.6f}, {:.6f}", vp * kPx, vp * kPy));
			push_option(b, g_opt_secondary, std::format("{:.6f}, {:.6f}", vs * kSx, vs * kSy));
			push_option(b, g_opt_scale, std::format("{:.4f}", scale));
			push_option(b, g_opt_bias, std::format("{:.3f}", cfg.water_detail_lod_bias));
		}

		bool build_water(IDirect3DDevice9* dev, const DrawDesc& d, const Attr& pos, const float* world,
			const float anchor[3], float tile, remixapi_MaterialHandle material, uint64_t key,
			MeshEntry& e, const char*& reject)
		{
			PROFILE_ZONE_N("api::water::build");
			Geometry g;
			if (!read_geometry(dev, d, g, reject)) return false;

			std::vector<remixapi_HardcodedVertex> verts(g.count);
			for (UINT i = 0; i < g.count; i++)
			{
				const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride;
				auto& o = verts[i];
				o = {};
				for (int k = 0; k < 3; k++) o.position[k] = read_comp(p + pos.off, pos.type, k);
				if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2]))
				{
					reject = "non-finite POSITION";
					return false;
				}
				o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f;

				// Absolute world XY = anchor + linear part of WORLD applied to the
				// object-space position.
				const float* v = o.position;
				const float wx = anchor[0] + v[0] * world[0] + v[1] * world[4] + v[2] * world[8];
				const float wy = anchor[1] + v[0] * world[1] + v[1] * world[5] + v[2] * world[9];
				o.texcoord[0] = wx / tile;
				o.texcoord[1] = wy / tile;
				o.color = 0xFFFFFFFFu;
			}

			set_bound(e, verts);
			e.material = material;
			e.mesh = create_mesh(next_object_hash(key, kWaterDomain), verts, g.tris, material,
				asset_keys::of(verts, g.tris));
			if (!e.mesh) { reject = "CreateMesh failed"; return false; }
			return true;
		}
	}

	bool try_water(IDirect3DDevice9* dev, const PsSlotMap* psmap, engine_class::Water kind, const DrawDesc& d)
	{
		PROFILE_ZONE_N("api::try_water");
		if (kind == engine_class::Water::None) return false;
		const uint32_t ps_hash = psmap ? psmap->ps_hash : 0u;

		auto& st = water_stats();

		// WATER036/037: additive point-light specular over a surface the path
		// tracer lights itself.
		if (kind == engine_class::Water::Light)
		{
			st.skipped++;
			if (!g_logged_drop)
			{
				g_logged_drop = true;
				shared::common::log("RemixApi", std::format(
					"water: dropping point-light specular pass PS 0x{:08X}", ps_hash),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
			return true;
		}

		if (!bridge()) { st.fallbacks++; st.last_reject = "Remix API unavailable"; return true; }

		const auto& ffp = shared::common::ffp_state::get();
		if (ffp.cur_decl_multi_stream()) { st.fallbacks++; note_failure(st, "multi-stream declaration"); return true; }
		const Attr pos = find_attr(D3DDECLUSAGE_POSITION, 0);
		if (!pos.ok() || !attr_fits(pos, ffp.stream_stride(0)) ||
			(pos.type != D3DDECLTYPE_FLOAT3 && pos.type != D3DDECLTYPE_FLOAT4))
		{
			st.fallbacks++;
			note_failure(st, "declaration lacks a float POSITION");
			return true;
		}

		const float* world = world_matrix();
		const float* cam = game::camera_position_ptr;
		if (!world || !cam) { st.fallbacks++; note_failure(st, "no WORLD matrix / camera position"); return true; }

		const char* reject = nullptr;
		uint64_t geo = 0;
		if (!geo_key(dev, d, geo, reject))
		{
			st.fallbacks++;
			note_failure(st, reject);
			return true;
		}

		// Object origin in absolute world space, rounded to a whole unit:
		// WORLD's translation is camera-relative and jitters by float error as
		// the camera moves; cell origins are whole numbers, so rounding
		// recovers them exactly and the key stays put.
		const float anchor[3] = {
			std::round(world[12] + cam[0]),
			std::round(world[13] + cam[1]),
			std::round(world[14] + cam[2]) };
		const float tile = tile_size();
		push_runtime_options(tile);
		WaterLook look = read_look(psmap);
		const bool near_water = kind == engine_class::Water::Near;
		if (near_water)
		{
			auto& v = g_near_votes[look_key(look)];
			v.look = look;
			v.draws++;
		}
		else
		{
			look = lod_look(look);
		}
		const uint64_t mat_key = look_key(look);

		Hash64 kh;
		kh.mix(geo);
		kh.pod(anchor);
		kh.pod(tile);
		kh.mix(mat_key);
		// The linear part of WORLD shapes the UVs too (scaled / rotated water).
		for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) kh.pod(world[r * 4 + c]);
		const uint64_t key = kh.get();

		constexpr remixapi_InstanceCategoryFlags kCats = REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER;
		// RetainedKeepCulled: near water belongs to its cell; distant water
		// (WATER034) is LOD, hidden once in view and no longer drawn.
		const Residency residency = near_water ? Residency::Cell : Residency::LodOnScreen;

		if (auto it = g_water.find(key); it != g_water.end())
		{
			it->second.residency = residency;
			if (!draw_entry(it->second, kCats, true, st)) { st.fallbacks++; return true; }
			return true;
		}

		if (const auto r = g_retry_at.find(key); r != g_retry_at.end() && frame() < r->second)
		{
			st.fallbacks++;
			return true;
		}
		if (!take_build_budget(key)) { st.fallbacks++; return true; }

		auto fail = [&](const char* why) {
			g_retry_at[key] = frame() + kRetryFrames;
			st.fallbacks++;
			note_failure(st, why, std::format("PS 0x{:08X}", ps_hash));
			return true;
		};

		const uint64_t normal_tex = water_normal_texture(reject);
		if (!normal_tex) return fail(reject);

		remixapi_MaterialHandle material = acquire_material(look, mat_key, normal_tex);
		if (!material) return fail("CreateMaterial failed");

		MeshEntry e;
		if (!build_water(dev, d, pos, world, anchor, tile, material, key, e, reject))
		{
			e.material = nullptr;
			destroy_entry(e);
			release_material(mat_key);
			return fail(reject);
		}
		e.geo = geo;
		e.built_frame = frame();
		e.residency = residency;
		auto& ins = g_water[key];
		ins = std::move(e);
		g_entry_material[key] = mat_key;
		st.created++;
		st.live = static_cast<uint32_t>(g_water.size());
		note_success(st, std::format("PS 0x{:08X}, {} verts, tile {:.0f}, shallow ({:.2f} {:.2f} {:.2f}) deep ({:.2f} {:.2f} {:.2f})",
			ps_hash, d.num_vtx, tile, look.shallow[0], look.shallow[1], look.shallow[2],
			look.deep[0], look.deep[1], look.deep[2]));

		if (!draw_entry(ins, kCats, true, st)) { st.fallbacks++; return true; }
		return true;
	}

	bool water_near_medium(float color[3], float& distance)
	{
		if (!g_near_valid) return false;
		const Medium m = medium_of(g_near_look);
		for (int k = 0; k < 3; k++) color[k] = m.color[k];
		distance = m.distance;
		return true;
	}

	void water_on_present()
	{
		settle_near_look();
		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.evict_frames);
		const UINT now = frame();
		auto& st = water_stats();
		// RetainedKeepCulled: visible entries kept while culled get the long
		// TTL (retained::stale_after); decided once so both loops agree.
		const bool kept = retained::keep_in_effect(g_water.size(), st);
		destroy_stale_retained(g_water, keep, kept);
		for (auto it = g_water.begin(); it != g_water.end();)
		{
			if (is_stale(it->second, now, keep, kept))
			{
				destroy_water_entry(it->first, it->second);
				st.evicted++;
				it = g_water.erase(it);
			}
			else ++it;
		}
		st.live = static_cast<uint32_t>(g_water.size());

		for (auto it = g_retry_at.begin(); it != g_retry_at.end();)
		{
			if (now >= it->second) it = g_retry_at.erase(it);
			else ++it;
		}
	}

	void water_on_reset()
	{
		g_retry_at.clear();
	}

	void water_drop_retained()
	{
		drop_retained(g_water);
	}

	void water_drop_all()
	{
		drop_retained(g_water);
		auto& st = water_stats();
		for (auto& [key, e] : g_water)
		{
			destroy_water_entry(key, e);
			st.evicted++;
		}
		g_water.clear();
		st.live = 0;
		g_retry_at.clear();
		// Every entry released its material; anything left is a stray.
		for (auto& [key, m] : g_materials)
			if (auto* b = bridge()) b->DestroyMaterial(m.handle);
		g_materials.clear();
		g_entry_material.clear();
		// No material names the ripple normal map any more: let it go, so the
		// next build generates it from the current WaterNormalRes / Strength.
		if (g_normal_tex)
		{
			textures::release(g_normal_tex);
			g_normal_tex = 0;
			g_normal_res = 0;
			g_normal_strength = -1.0f;
		}
	}
}
