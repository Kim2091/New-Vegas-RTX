#include "std_include.hpp"
#include "vs_expand.hpp"
#include "api_common.hpp"
#include "engine_class.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/config.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace comp::game::vs_expand
{
	namespace
	{
		using shared::common::VsDesc;
		using shared::common::VsFamily;

		// ================================================================
		// Vertex attribute access
		// ================================================================

		float half_to_float(unsigned short h)
		{
			const unsigned int sign = (h >> 15) & 0x1u;
			unsigned int exp  = (h >> 10) & 0x1Fu;
			unsigned int mant = h & 0x3FFu;
			unsigned int f;
			if (exp == 0)
			{
				if (mant == 0) f = sign << 31;
				else
				{
					while (!(mant & 0x400u)) { mant <<= 1; exp--; }
					exp++; mant &= ~0x400u;
					f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
				}
			}
			else if (exp == 31) f = (sign << 31) | 0x7F800000u | (mant << 13);
			else                f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
			float v;
			std::memcpy(&v, &f, sizeof(v));
			return v;
		}

		UINT decl_type_size(int type)
		{
			switch (type)
			{
			case D3DDECLTYPE_FLOAT1:    return 4;
			case D3DDECLTYPE_FLOAT2:    return 8;
			case D3DDECLTYPE_FLOAT3:    return 12;
			case D3DDECLTYPE_FLOAT4:    return 16;
			case D3DDECLTYPE_D3DCOLOR:  return 4;
			case D3DDECLTYPE_UBYTE4:    return 4;
			case D3DDECLTYPE_SHORT2:    return 4;
			case D3DDECLTYPE_SHORT4:    return 8;
			case D3DDECLTYPE_UBYTE4N:   return 4;
			case D3DDECLTYPE_SHORT2N:   return 4;
			case D3DDECLTYPE_SHORT4N:   return 8;
			case D3DDECLTYPE_USHORT2N:  return 4;
			case D3DDECLTYPE_USHORT4N:  return 8;
			case D3DDECLTYPE_FLOAT16_2: return 4;
			case D3DDECLTYPE_FLOAT16_4: return 8;
			default:                    return 0;   // UDEC3 / DEC3N / unknown: unsupported
			}
		}

		// One component of a vertex element as the vertex shader sees it,
		// including D3D9's (0, 0, 0, 1) fill for components the type lacks.
		// The FNV declarations these families use are FLOAT in practice; the
		// other types are here so an unexpected packing reads correctly
		// instead of producing garbage geometry.
		float read_comp(const unsigned char* p, int type, int comp)
		{
			const float fill = (comp == 3) ? 1.0f : 0.0f;
			switch (type)
			{
			case D3DDECLTYPE_FLOAT1:
			case D3DDECLTYPE_FLOAT2:
			case D3DDECLTYPE_FLOAT3:
			case D3DDECLTYPE_FLOAT4:
			{
				const int n = type - D3DDECLTYPE_FLOAT1 + 1;
				if (comp >= n) return fill;
				float v;
				std::memcpy(&v, p + comp * 4, sizeof(v));
				return v;
			}
			case D3DDECLTYPE_D3DCOLOR:
			{
				// Stored B, G, R, A; the shader sees (R, G, B, A).
				static constexpr int kSwizzle[4] = { 2, 1, 0, 3 };
				return static_cast<float>(p[kSwizzle[comp]]) / 255.0f;
			}
			case D3DDECLTYPE_UBYTE4:  return static_cast<float>(p[comp]);
			case D3DDECLTYPE_UBYTE4N: return static_cast<float>(p[comp]) / 255.0f;
			case D3DDECLTYPE_SHORT2:
			case D3DDECLTYPE_SHORT4:
			case D3DDECLTYPE_SHORT2N:
			case D3DDECLTYPE_SHORT4N:
			{
				const int n = (type == D3DDECLTYPE_SHORT2 || type == D3DDECLTYPE_SHORT2N) ? 2 : 4;
				if (comp >= n) return fill;
				short s;
				std::memcpy(&s, p + comp * 2, sizeof(s));
				if (type == D3DDECLTYPE_SHORT2N || type == D3DDECLTYPE_SHORT4N)
					return (std::max)(static_cast<float>(s) / 32767.0f, -1.0f);
				return static_cast<float>(s);
			}
			case D3DDECLTYPE_USHORT2N:
			case D3DDECLTYPE_USHORT4N:
			{
				const int n = (type == D3DDECLTYPE_USHORT2N) ? 2 : 4;
				if (comp >= n) return fill;
				unsigned short s;
				std::memcpy(&s, p + comp * 2, sizeof(s));
				return static_cast<float>(s) / 65535.0f;
			}
			case D3DDECLTYPE_FLOAT16_2:
			case D3DDECLTYPE_FLOAT16_4:
			{
				const int n = (type == D3DDECLTYPE_FLOAT16_2) ? 2 : 4;
				if (comp >= n) return fill;
				unsigned short h;
				std::memcpy(&h, p + comp * 2, sizeof(h));
				return half_to_float(h);
			}
			default:
				return fill;
			}
		}

		// floor() of a VS index input, with the non-finite / absurd values a
		// corrupt vertex could carry mapped to an index no register range
		// accepts.
		int floor_index(float x)
		{
			if (!(x > -65536.0f && x < 65536.0f)) return INT_MIN / 2;
			return static_cast<int>(std::floor(x));
		}

		float sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

		// Stream-0 byte layout of the elements the expanders read or write.
		// -1 offset = element absent.
		struct Layout
		{
			int pos_off = -1, pos_type = -1;
			int nrm_off = -1, nrm_type = -1;
			int tc0_off = -1, tc0_type = -1;
			int tc1_off = -1, tc1_type = -1;
			int col_off = -1, col_type = -1;
			int bi_off  = -1, bi_type  = -1;
		};

		bool fits(int off, int type, UINT stride)
		{
			const UINT sz = decl_type_size(type);
			return off >= 0 && sz != 0 && static_cast<UINT>(off) + sz <= stride;
		}

		Layout current_layout()
		{
			const auto& ffp = shared::common::ffp_state::get();
			Layout l;
			l.pos_off = ffp.cur_decl_pos_off();
			l.pos_type = ffp.cur_decl_pos_type();
			if (ffp.cur_decl_has_normal()) { l.nrm_off = ffp.cur_decl_normal_off(); l.nrm_type = ffp.cur_decl_normal_type(); }
			if (ffp.cur_decl_has_texcoord()) { l.tc0_off = ffp.cur_decl_texcoord_off(); l.tc0_type = ffp.cur_decl_texcoord_type(); }
			l.tc1_off = ffp.cur_decl_texcoord1_off();
			l.tc1_type = ffp.cur_decl_texcoord1_type();
			l.col_off = ffp.cur_decl_color_off();
			l.col_type = ffp.cur_decl_color_type();
			if (ffp.cur_decl_has_blendindices()) { l.bi_off = ffp.cur_decl_blend_indices_off(); l.bi_type = ffp.cur_decl_blend_indices_type(); }
			return l;
		}

		// ================================================================
		// Hashing
		// ================================================================

		struct Hasher
		{
			uint64_t h = 0xcbf29ce484222325ull;

			void mix(uint64_t w)
			{
				h ^= w;
				h *= 0x9E3779B97F4A7C15ull;
				h = (h << 27) | (h >> 37);
			}

			void add(const void* data, size_t n)
			{
				const auto* p = static_cast<const unsigned char*>(data);
				size_t i = 0;
				for (; i + 8 <= n; i += 8)
				{
					uint64_t w;
					std::memcpy(&w, p + i, sizeof(w));
					mix(w);
				}
				uint64_t tail = 0;
				for (size_t k = 0; i < n; ++i, ++k)
					tail |= static_cast<uint64_t>(p[i]) << (8 * k);
				mix(tail ^ (static_cast<uint64_t>(n) << 56));
			}

			void add_u64(uint64_t v) { mix(v); }
			void add_regs(const float* consts, int first_reg, int num_regs)
			{
				if (first_reg < 0) { num_regs += first_reg; first_reg = 0; }
				if (first_reg + num_regs > 256) num_regs = 256 - first_reg;
				if (num_regs <= 0) return;
				add(consts + first_reg * 4, static_cast<size_t>(num_regs) * 4 * sizeof(float));
			}

			// Like add_regs, but each of the first `comps` components of a register
			// is keyed on a 1/kQuantum grid, so float noise in a constant the engine
			// re-derives every frame does not change the key. Non-finite values are
			// keyed on their bits.
			static constexpr float kQuantum = 1024.0f;

			// llround(v * kQuantum) without the CRT call: v * 1024 has at most
			// 24 significant bits, so adding +-0.5 in double is exact and the
			// truncation rounds half away from zero, as llround does.
			static uint64_t quantize(float v)
			{
				const double x = static_cast<double>(v) * kQuantum;
				if (x > -2147483648.0 && x < 2147483647.0)
					return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(x + (x < 0.0 ? -0.5 : 0.5))));
				return static_cast<uint64_t>(std::llround(x));
			}

			void add_regs_quantized(const float* consts, int first_reg, int num_regs, int comps = 4)
			{
				if (first_reg < 0) { num_regs += first_reg; first_reg = 0; }
				if (first_reg + num_regs > 256) num_regs = 256 - first_reg;
				for (int r = 0; r < num_regs; r++)
				{
					const float* v = consts + (first_reg + r) * 4;
					for (int k = 0; k < comps; k++)
					{
						uint32_t bits;
						std::memcpy(&bits, &v[k], sizeof(bits));
						mix(std::isfinite(v[k]) ? quantize(v[k]) : (0x7F00000000000000ull | bits));
					}
				}
			}
		};

		// Content fingerprint of the source window. Dynamic buffers (particles)
		// are rewritten every frame, so all of it is hashed; they are small.
		// Static buffers can be large (a grass batch is ~instances x blade
		// vertices) and change only when the game reuses the allocation, so a
		// spread of samples catches that at a fraction of the cost.
		uint64_t content_hash(const unsigned char* data, UINT bytes, bool full)
		{
			Hasher h;
			h.add_u64(bytes);
			if (full || bytes <= 2048)
			{
				h.add(data, bytes);
				return h.h;
			}
			h.add(data, 256);
			h.add(data + bytes - 256, 256);
			constexpr UINT kSamples = 16, kChunk = 32;
			for (UINT i = 1; i <= kSamples; i++)
			{
				const UINT at = static_cast<UINT>((static_cast<uint64_t>(bytes - kChunk) * i) / (kSamples + 1));
				h.add(data + at, kChunk);
			}
			return h.h;
		}

		// ================================================================
		// Dynamic vertex buffer readability
		//
		// The bridge client keeps a CPU shadow of every vertex buffer and a
		// READONLY lock returns a pointer straight into it: the same memory
		// the game wrote through its own (DISCARD / NOOVERWRITE) locks, so it
		// is current, and the unlock sends nothing across the bridge.
		//
		// The exception is client.optimizedDynamicLock=True in bridge.conf: a
		// DYNAMIC buffer's lock then hands out a fresh blob reserved in the IPC
		// channel instead of the shadow: uninitialised memory, plus a Lock
		// command on the wire. The two cases are told apart by locking twice:
		// the shadow (or shared-heap) pointer is stable, a channel blob isn't.
		// One probe decides for the process (the option is global).
		// ================================================================

		enum class DynReads { Unknown, Ok, Unavailable };
		DynReads g_dyn_reads = DynReads::Unknown;

		bool dynamic_reads_ok(IDirect3DVertexBuffer9* vb)
		{
			if (g_dyn_reads != DynReads::Unknown) return g_dyn_reads == DynReads::Ok;

			void* p1 = nullptr;
			void* p2 = nullptr;
			if (SUCCEEDED(vb->Lock(0, 16, &p1, D3DLOCK_READONLY))) vb->Unlock();
			if (SUCCEEDED(vb->Lock(0, 16, &p2, D3DLOCK_READONLY))) vb->Unlock();

			g_dyn_reads = (p1 && p1 == p2) ? DynReads::Ok : DynReads::Unavailable;
			shared::common::log("VSExpand", g_dyn_reads == DynReads::Ok
				? "Dynamic VB reads: bridge client returns its CPU shadow for READONLY locks -- particle expansion active."
				: "Dynamic VB reads: READONLY locks do NOT return stable CPU data (bridge.conf client.optimizedDynamicLock=True?). "
				  "Particle flipbooks keep the legacy full-atlas look.",
				g_dyn_reads == DynReads::Ok ? shared::common::LOG_TYPE::LOG_TYPE_GREEN : shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return g_dyn_reads == DynReads::Ok;
		}

		// ================================================================
		// Caches
		// ================================================================

		UINT g_frame = 0;

		// Frames an entry may go undrawn before its buffer is released. Only
		// bounds memory; correctness against recycled buffers comes from the
		// content hash in the key, not from aging.
		constexpr UINT kMaxIdleFrames = 600;
		constexpr UINT kAgeScanPeriod = 128;

		// Instance-index range of an instanced window. Pure function of the
		// source vertices, so it is cached on the geometry alone and the
		// per-draw work on a hit is the constant hash, not a vertex scan.
		struct GeoInfo
		{
			IDirect3DVertexBuffer9* src = nullptr;
			INT first = 0;
			UINT count = 0, stride = 0, offset = 0;
			uint64_t content = 0;
			int min_idx = 0, max_idx = -1;
			UINT last_frame = 0;
		};
		std::unordered_map<uint64_t, GeoInfo> g_geo;

		struct Entry
		{
			IDirect3DVertexBuffer9* dst = nullptr;
			UINT capacity = 0;
			bool dyn_pool = false;

			IDirect3DVertexBuffer9* src = nullptr;
			IDirect3DVertexDeclaration9* decl = nullptr;
			INT first = 0;          // absolute first source vertex
			UINT skip = 0;          // where that vertex lands in the copy
			UINT count = 0, stride = 0, offset = 0;
			uint32_t vs_hash = 0;
			uint64_t content = 0;
			uint64_t params = 0;

			UINT last_frame = 0;
			bool valid = false;
		};

		// Sized to hold every expanded draw of a busy exterior frame (grass
		// alone can be a few hundred) so the steady state is all hits.
		constexpr UINT CACHE_SIZE = 1024;   // power of two
		constexpr UINT PROBES = 8;
		Entry g_cache[CACHE_SIZE];

		// Saved stream-0 binding for unbind().
		IDirect3DVertexBuffer9* g_saved_vb = nullptr;
		UINT g_saved_offset = 0;
		UINT g_saved_stride = 0;
		bool g_bound = false;
		// bind_smooth_normals also swaps the declaration.
		IDirect3DVertexDeclaration9* g_saved_decl = nullptr;
		bool g_decl_swapped = false;

		// Smooth-normal copies (bind_smooth_normals), one per mesh.
		struct NormalsEntry
		{
			IDirect3DVertexBuffer9* dst = nullptr;
			IDirect3DVertexDeclaration9* decl = nullptr;
			UINT stride = 0;   // the copy's: source stride + 12
			SmoothNormalsInfo info;
			UINT last_frame = 0;
		};
		std::unordered_map<uint64_t, NormalsEntry> g_normals;
		constexpr size_t kMaxNormalsEntries = 32;
		constexpr UINT kMaxSmoothVerts = 4096;

		void release_normals_entry(NormalsEntry& e)
		{
			if (e.dst) e.dst->Release();
			if (e.decl) e.decl->Release();
			e = NormalsEntry{};
		}

		// Stats, per family, for the periodic status line.
		struct Stats { UINT draws = 0, rebuilds = 0, culled = 0, declined = 0; };
		Stats g_stats[static_cast<size_t>(VsFamily::Count)];

		Stats& stats(VsFamily f) { return g_stats[static_cast<size_t>(f)]; }

		// Grass key diagnostic: for each batch slot, the constants its expansion
		// read last time. A draw whose source vertices are unchanged but whose
		// constants moved is counted by what moved and how far, in the 600-frame
		// status line. Deltas below 1/Hasher::kQuantum do not change the key.
		struct GrassSeen
		{
			uint64_t content = 0;
			int min_idx = 0, max_idx = -1;
			std::vector<float> inst;   // (max_idx - min_idx + 1) * 4 floats
			float scale[4] = {};
			uint32_t last_frame = 0;
		};
		struct GrassDiag
		{
			UINT compared = 0, changed = 0, range = 0, keyed = 0;
			UINT xyz = 0, w = 0, scale_xyz = 0, scale_w = 0;   // instances / registers that moved
			float max_xyz = 0.0f, max_w = 0.0f, max_scale_xyz = 0.0f, max_scale_w = 0.0f;
		};
		std::unordered_map<uint64_t, GrassSeen> g_grass_seen;
		GrassDiag g_grass_diag;

		void note_grass_params(IDirect3DVertexBuffer9* src, UINT offset, INT first, UINT count, uint64_t content,
			const float* c, const VsDesc& d, int min_idx, int max_idx)
		{
			uint64_t slot = reinterpret_cast<uintptr_t>(src);
			slot = slot * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(offset) << 32 | static_cast<uint32_t>(first));
			slot = slot * 0x9E3779B97F4A7C15ull ^ count;
			const int regs = max_idx - min_idx + 1;
			const int base = d.reg_instance + min_idx;
			if (base < 0 || base + regs > 256 || regs <= 0) return;
			const float* inst = c + base * 4;
			const float* scale = c + d.reg_scale_mask * 4;

			GrassSeen& s = g_grass_seen[slot];
			const bool comparable = s.max_idx >= s.min_idx && s.content == content;
			if (comparable)
			{
				auto& g = g_grass_diag;
				g.compared++;
				const float q = 1.0f / Hasher::kQuantum;
				bool moved = false, keyed = false;
				if (s.min_idx != min_idx || s.max_idx != max_idx)
				{
					g.range++;
					moved = keyed = true;
				}
				else
				{
					for (int i = 0; i < regs; i++)
					{
						const float* a = &s.inst[i * 4];
						const float* b = inst + i * 4;
						const float dxyz = std::max({ std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2]) });
						const float dw = std::fabs(a[3] - b[3]);
						if (dxyz > 0.0f) { g.xyz++; g.max_xyz = std::max(g.max_xyz, dxyz); moved = true; keyed |= dxyz >= q; }
						if (dw > 0.0f) { g.w++; g.max_w = std::max(g.max_w, dw); moved = true; keyed |= dw >= q; }
					}
				}
				const float dsxyz = std::max({ std::fabs(s.scale[0] - scale[0]), std::fabs(s.scale[1] - scale[1]), std::fabs(s.scale[2] - scale[2]) });
				const float dsw = std::fabs(s.scale[3] - scale[3]);
				if (dsxyz > 0.0f) { g.scale_xyz++; g.max_scale_xyz = std::max(g.max_scale_xyz, dsxyz); moved = true; keyed |= dsxyz >= q; }
				if (dsw > 0.0f) { g.scale_w++; g.max_scale_w = std::max(g.max_scale_w, dsw); moved = true; }
				if (moved) g.changed++;
				if (keyed) g.keyed++;
			}
			s.content = content;
			s.min_idx = min_idx;
			s.max_idx = max_idx;
			s.inst.assign(inst, inst + regs * 4);
			std::memcpy(s.scale, scale, sizeof(s.scale));
			s.last_frame = g_frame;
		}

		void decline_once(const VsDesc& d, const char* reason, bool quiet = false)
		{
			// quiet: a probe for the Remix-API foliage path (describe_for_api).
			// A declined draw then comes back through bind_expanded, which
			// counts and logs it; counting here too would count it twice.
			if (quiet) return;
			stats(d.family).declined++;
			static std::set<std::string> s_seen;
			std::string key = std::format("{}|{}", static_cast<int>(d.family), reason);
			if (!s_seen.insert(key).second) return;
			shared::common::log("VSExpand", std::format("{} (VS 0x{:08X}) declined: {} -- draw keeps its legacy route.",
				shared::common::vs_family_name(d.family), d.vs_hash, reason),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		// One-shot per family: does the VS's ModelViewProj equal the
		// NiDX9Renderer's WORLD*VIEW*PROJ at this draw? The expanders emit
		// positions in the shader's input (model) space and rely on FFP's
		// WORLD/VIEW/PROJ (the renderer's) to finish the job, which is only
		// right if the two agree. Logged so the claim is checkable in-game.
		void log_mvp_check_once(const VsDesc& d)
		{
			static bool s_done[static_cast<size_t>(VsFamily::Count)] = {};
			bool& done = s_done[static_cast<size_t>(d.family)];
			if (done || !d.has(d.reg_mvp) || d.reg_mvp > 252) return;
			done = true;

			const float* w = get_renderer_matrix(RENDERER_WORLD_OFF);
			const float* v = get_renderer_matrix(RENDERER_VIEW_OFF);
			const float* p = get_renderer_matrix(RENDERER_PROJ_OFF);
			if (!w || !v || !p) return;

			float wv[16], wvp[16];
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++)
				{
					wv[r * 4 + c] = 0.0f;
					for (int k = 0; k < 4; k++) wv[r * 4 + c] += w[r * 4 + k] * v[k * 4 + c];
				}
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++)
				{
					wvp[r * 4 + c] = 0.0f;
					for (int k = 0; k < 4; k++) wvp[r * 4 + c] += wv[r * 4 + k] * p[k * 4 + c];
				}

			// The shader computes dp4(c[mvp + r], pos), so register r holds
			// column r of the row-vector D3D matrix.
			const float* c = shared::common::ffp_state::get().vs_const_data() + d.reg_mvp * 4;
			float max_diff = 0.0f, max_mag = 1e-6f;
			for (int r = 0; r < 4; r++)
				for (int k = 0; k < 4; k++)
				{
					max_diff = (std::max)(max_diff, std::fabs(c[r * 4 + k] - wvp[k * 4 + r]));
					max_mag = (std::max)(max_mag, std::fabs(wvp[k * 4 + r]));
				}
			const float rel = max_diff / max_mag;
			shared::common::log("VSExpand", std::format(
				"{}: VS ModelViewProj vs renderer WORLD*VIEW*PROJ relative diff = {:.2e} ({})",
				shared::common::vs_family_name(d.family), rel,
				rel < 1e-3f ? "match -- instance positions are model-space, FFP WORLD is correct"
				            : "MISMATCH -- expanded geometry will be misplaced; report this"),
				rel < 1e-3f ? shared::common::LOG_TYPE::LOG_TYPE_GREEN : shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		// ================================================================
		// Per-family arithmetic
		// ================================================================

		// Instance-register stride and index decode for the instanced families.
		//   Grass / DistLod: frc/add/mova -> floor(TEXCOORD1.x), 1 register each.
		//   DistTree (vs_1_1): mov a0.x, 2*TEXCOORD1.x, 2 registers each. The
		//   instance index is TEXCOORD1.x. The engine writes whole numbers there,
		//   so floor and round agree; rounding keeps a 2.9999-style value on the
		//   right slot.
		int instance_reg_offset(VsFamily f, float tc1x)
		{
			if (f == VsFamily::DistTree)
				return floor_index(tc1x + 0.5f);
			return floor_index(tc1x);
		}

		int instance_stride_regs(VsFamily f) { return f == VsFamily::DistTree ? 2 : 1; }

		// Per-instance visibility, from the VS's own distance fade. The PS turns
		// fade == 0 into alpha 0 (a binary cutout), so those instances are
		// absent in the game's image and must be absent here too; otherwise
		// the path tracer would draw grass out to the batch's full
		// extent. bit (i - min_idx) set = visible.
		struct InstanceMask
		{
			uint64_t bits[4] = {};
			int visible = 0;

			void set(int i) { bits[i >> 6] |= (1ull << (i & 63)); visible++; }
			bool get(int i) const { return (bits[i >> 6] >> (i & 63)) & 1ull; }
		};

		void compute_mask(const VsDesc& d, const float* c, int min_idx, int max_idx, InstanceMask& m)
		{
			for (int i = min_idx; i <= max_idx; i++)
			{
				const int reg = d.reg_instance + i * instance_stride_regs(d.family);
				if (reg < 0 || reg > 255) continue;
				const float* inst = c + reg * 4;
				bool visible = true;

				if (d.family == VsFamily::Grass)
				{
					// GRASS VS: clip = ModelViewProj * (I.xyz, 1)
					//           fade = 1 - sat((length(clip) - AlphaParam.z) / AlphaParam.w)
					// length() is over all four clip components (dp4 r2, r2).
					const float* mvp = c + d.reg_mvp * 4;
					float len2 = 0.0f;
					for (int r = 0; r < 4; r++)
					{
						const float* row = mvp + r * 4;
						const float cc = row[0] * inst[0] + row[1] * inst[1] + row[2] * inst[2] + row[3];
						len2 += cc * cc;
					}
					const float* ap = c + d.reg_alpha_param * 4;
					const float len = std::sqrt(len2);
					if (ap[3] > 0.0f) visible = (1.0f - sat((len - ap[2]) / ap[3])) > 0.0f;
					else              visible = len <= ap[2];   // rcp(0): a hard cut at z
				}
				else if (d.family == VsFamily::DistLod)
				{
					// DISTLOD VS: dist = |EyePos.xyz - I.xyz|
					//   fade = dist > AlphaParam.x ? sat(1 - (dist - x) / y) : 1
					const float* eye = c + d.reg_eye_pos * 4;
					const float* ap = c + d.reg_alpha_param * 4;
					const float dx = eye[0] - inst[0], dy = eye[1] - inst[1], dz = eye[2] - inst[2];
					const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (dist > ap[0])
						visible = (ap[1] > 0.0f) && (sat(1.0f - (dist - ap[0]) / ap[1]) > 0.0f);
				}

				if (visible) m.set(i - min_idx);
			}
		}

		struct Frame
		{
			const VsDesc* d = nullptr;
			const Layout* l = nullptr;
			const float* c = nullptr;     // VS constant mirror
			const float* ps = nullptr;    // PS constant mirror
			const shared::common::PsSlotMap* psmap = nullptr;
			int min_idx = 0;
			// Leaf card size (the engine's BillboardRight / Up lengths).
			float right_len = 0.0f, up_len = 0.0f;
			// NoLightUv: bake TexCoordTranform into TEXCOORD0 (the Remix-API
			// copy; the D3D9 copy keeps the stage-0 texture matrix).
			bool bake_uv = false;
		};

		void write_pos(unsigned char* vert, const Layout& l, float x, float y, float z, bool force_w1)
		{
			float* pos = reinterpret_cast<float*>(vert + l.pos_off);
			pos[0] = x; pos[1] = y; pos[2] = z;
			if (force_w1 && l.pos_type == D3DDECLTYPE_FLOAT4) pos[3] = 1.0f;
		}

		void expand_instanced(unsigned char* verts, UINT count, UINT stride, const Frame& f)
		{
			const auto& l = *f.l;
			const auto& d = *f.d;
			const float* scale = d.has(d.reg_scale_mask) ? f.c + d.reg_scale_mask * 4 : nullptr;

			for (UINT v = 0; v < count; v++)
			{
				unsigned char* vert = verts + v * stride;
				const float* pos = reinterpret_cast<const float*>(vert + l.pos_off);
				const float px = pos[0], py = pos[1], pz = pos[2];

				const int idx = instance_reg_offset(d.family, read_comp(vert + l.tc1_off, l.tc1_type, 0));
				const int reg = d.reg_instance + idx * instance_stride_regs(d.family);
				const int rel = idx - f.min_idx;
				const bool in_range = reg >= 0 && reg + instance_stride_regs(d.family) - 1 <= 255
					&& rel >= 0 && rel < 256;

				if (!in_range)
				{
					// Faded-out (or unaddressable) instance: collapse every vertex
					// onto one point so its triangles have zero area: the path
					// tracer never hits them and the index buffer stays untouched.
					const float* inst = in_range ? f.c + reg * 4 : nullptr;
					write_pos(vert, l, inst ? inst[0] : 0.0f, inst ? inst[1] : 0.0f, inst ? inst[2] : 0.0f, true);
					continue;
				}

				const float* inst = f.c + reg * 4;
				switch (d.family)
				{
				case VsFamily::Grass:
				{
					// pos = v0 * (1 + ScaleMask.xyz * 0.01 * I.w) + I.xyz. The wind
					// term (sin(phase) * WindData.z * COLOR.a^2 along WindData.xy)
					// is deliberately dropped: an animated blade re-expands every
					// frame and flickers in accumulated RT output.
					const float k = 0.01f * inst[3];
					write_pos(vert, l,
						px * (1.0f + scale[0] * k) + inst[0],
						py * (1.0f + scale[1] * k) + inst[1],
						pz * (1.0f + scale[2] * k) + inst[2], true);
					// The NORMAL variants light with the vertex normal itself
					// (dp3_sat DiffuseDir, v1) and never rotate an instance, so
					// the stored normal is already right and is left alone.
					break;
				}
				case VsFamily::DistLod:
				{
					// pos.xyz = v0.xyz * (0.01 * I.w) + I.xyz; pos.w = v0.w.
					const float k = 0.01f * inst[3];
					write_pos(vert, l, px * k + inst[0], py * k + inst[1], pz * k + inst[2], false);
					break;
				}
				case VsFamily::DistTree:
				{
					// s = v0 * P.w; pos.xy = Rz(cos, sin) * s.xy; pos.z = s.z; += P.xyz
					const float* r = inst + 4;
					const float sx = px * inst[3], sy = py * inst[3], sz = pz * inst[3];
					write_pos(vert, l,
						r[0] * sx - r[1] * sy + inst[0],
						r[1] * sx + r[0] * sy + inst[1],
						sz + inst[2], false);
					break;
				}
				default:
					break;
				}
			}
		}

		// Camera-independent leaf orientation angle: a hash of the leaf centre
		// (all four corners of a leaf share POSITION), so the same leaf gets the
		// same card every frame and neighbouring leaves differ.
		float leaf_angle(float x, float y, float z)
		{
			uint32_t b[3];
			std::memcpy(&b[0], &x, 4); std::memcpy(&b[1], &y, 4); std::memcpy(&b[2], &z, 4);
			uint32_t h = 2166136261u;
			for (uint32_t w : b) { h ^= w; h *= 16777619u; h ^= h >> 15; }
			return static_cast<float>(h & 0xFFFFu) * (3.14159265f / 65536.0f);
		}

		void expand_leaves(unsigned char* verts, UINT count, UINT stride, const Frame& f)
		{
			const auto& l = *f.l;
			const auto& d = *f.d;

			for (UINT v = 0; v < count; v++)
			{
				unsigned char* vert = verts + v * stride;
				const float* pos = reinterpret_cast<const float*>(vert + l.pos_off);
				const float px = pos[0], py = pos[1], pz = pos[2];

				// LeafBase index = floor(BLENDINDICES.z), corner scale = BLENDINDICES.w
				// (STLEAF VS: frc/add/mova a0.x from v3.z; r4 = v3.w * LeafBase[a0.x]).
				const unsigned char* bi = vert + l.bi_off;
				const int li = floor_index(read_comp(bi, l.bi_type, 2));
				const float s = read_comp(bi, l.bi_type, 3);
				if (li < 0 || li >= d.leaf_base_count || d.reg_leaf_base + li > 255) continue;

				const float* lb = f.c + (d.reg_leaf_base + li) * 4;
				const float a = s * lb[1];   // along Right
				const float b = s * lb[2];   // along Up

				// World-up card spun about Z by the per-leaf hash (camera-
				// independent, so cached and stable in RT); lengths carried over
				// from the engine's vectors so leaf size matches.
				const float phi = leaf_angle(px, py, pz);
				const float rx = std::cos(phi) * f.right_len, ry = std::sin(phi) * f.right_len;
				const float uz = f.up_len;

				write_pos(vert, l, px + a * rx, py + a * ry, pz + b * uz, false);
			}
		}

		// MaterialColor (NOLIGHT PSs: out = tex * COLOR * MaterialColor) as the
		// factor folded into the vertex colour, which FFP (and the Remix-API
		// particle mesh) modulates the texture by. Clamped to [0,1] because
		// D3DCOLOR can't hold more (the API path carries the part above 1 as
		// the albedo tint). False when it is white or there is no D3DCOLOR /
		// FLOAT4 COLOR to fold it into.
		bool material_color_fold(const shared::common::PsSlotMap* psmap, const float* ps, const Layout& l, UINT stride,
			float (&mc)[4])
		{
			if (!psmap || psmap->material_color_reg == shared::common::PsSlotMap::kNoReg || !ps ||
				(l.col_type != D3DDECLTYPE_D3DCOLOR && l.col_type != D3DDECLTYPE_FLOAT4) || !fits(l.col_off, l.col_type, stride))
				return false;
			const float* src = ps + psmap->material_color_reg * 4;
			bool fold = false;
			for (int i = 0; i < 4; i++)
			{
				mc[i] = std::isfinite(src[i]) ? sat(src[i]) : 1.0f;
				if (mc[i] != 1.0f) fold = true;
			}
			return fold;
		}

		void fold_color(unsigned char* vert, const Layout& l, const float (&mc)[4])
		{
			unsigned char* col = vert + l.col_off;
			if (l.col_type == D3DDECLTYPE_D3DCOLOR)
			{
				// D3DCOLOR bytes are B, G, R, A.
				static constexpr int kChan[4] = { 2, 1, 0, 3 };
				for (int i = 0; i < 4; i++)
				{
					const float x = static_cast<float>(col[kChan[i]]) * mc[i] + 0.5f;
					col[kChan[i]] = static_cast<unsigned char>(x > 255.0f ? 255.0f : x);
				}
				return;
			}
			float c[4];
			std::memcpy(c, col, sizeof(c));
			for (int i = 0; i < 4; i++) c[i] *= mc[i];
			std::memcpy(col, c, sizeof(c));
		}

		// NoLightUv (NOLIGHT000-018 even): the effect particle systems (fire,
		// embers) whose quads the engine streams into a DYNAMIC VB, and any draw
		// of the family the engine tints or fades with MaterialColor. The D3D9
		// route's stage 0 can modulate the texture by only one of COLOR and
		// MaterialColor, so the copy carries MaterialColor in its vertex colours
		// (tint, and the alpha the engine fades a whole system with). bake_uv
		// (the Remix-API copy) also applies the VS's UV transform:
		//   u' = dot(TexCoordTranform[0].xyz, (u, v, 1))
		//   v' = dot(TexCoordTranform[1].xyz, (u, v, 1))
		// The D3D9 copy keeps the game's UVs under the stage-0 texture matrix
		// (nolight_uv_matrix), so its texcoords (and Remix's asset hash of the
		// draw) stay the game's.
		void expand_nolight(unsigned char* verts, UINT count, UINT stride, const Frame& f)
		{
			const auto& l = *f.l;
			const auto& d = *f.d;

			float mc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			const bool fold_mc = material_color_fold(f.psmap, f.ps, l, stride, mc);
			const float* r0 = nullptr;
			if (f.bake_uv && d.has(d.reg_tc_xform) && d.reg_tc_xform <= 254) r0 = f.c + d.reg_tc_xform * 4;
			if (!fold_mc && !r0) return;

			for (UINT v = 0; v < count; v++)
			{
				unsigned char* vert = verts + v * stride;
				if (r0)
				{
					const float* r1 = r0 + 4;
					float uv[2];
					std::memcpy(uv, vert + l.tc0_off, sizeof(uv));
					const float u0 = uv[0], v0 = uv[1];
					uv[0] = r0[0] * u0 + r0[1] * v0 + r0[2];
					uv[1] = r1[0] * u0 + r1[1] * v0 + r1[2];
					std::memcpy(vert + l.tc0_off, uv, sizeof(uv));
				}
				if (fold_mc) fold_color(vert, l, mc);
			}
		}

		void expand_particles(unsigned char* verts, UINT count, UINT stride, const Frame& f)
		{
			const auto& l = *f.l;
			const auto& d = *f.d;

			float mc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			const bool fold_mc = material_color_fold(f.psmap, f.ps, l, stride, mc);

			for (UINT v = 0; v < count; v++)
			{
				unsigned char* vert = verts + v * stride;

				// NOLIGHT017/019: idx = floor(TEXCOORD1.x); S = SubTexOffsets[idx]
				//   oT0.xy = S.yw * TEXCOORD0.xy + S.xz
				const int idx = floor_index(read_comp(vert + l.tc1_off, l.tc1_type, 0));
				const int reg = d.reg_sub_tex + idx;
				if (idx >= 0 && reg <= 255)
				{
					const float* s = f.c + reg * 4;
					float* uv = reinterpret_cast<float*>(vert + l.tc0_off);
					uv[0] = s[1] * uv[0] + s[0];
					uv[1] = s[3] * uv[1] + s[2];
				}

				if (fold_mc) fold_color(vert, l, mc);
			}
		}

		// ================================================================
		// Entry lookup / build
		// ================================================================

		bool key_matches(const Entry& e, IDirect3DVertexBuffer9* src, IDirect3DVertexDeclaration9* decl,
			INT first, UINT skip, UINT count, UINT stride, UINT offset, uint32_t vs_hash, uint64_t content, uint64_t params)
		{
			return e.valid && e.src == src && e.decl == decl && e.first == first && e.skip == skip && e.count == count &&
				e.stride == stride && e.offset == offset && e.vs_hash == vs_hash &&
				e.content == content && e.params == params;
		}

		void release_entry(Entry& e)
		{
			if (e.dst) e.dst->Release();
			e = Entry{};
		}

		const GeoInfo* geo_info(const VsDesc& d, const Layout& l, IDirect3DVertexBuffer9* src,
			INT first, UINT count, UINT stride, UINT offset, uint64_t content, const unsigned char* data)
		{
			Hasher h;
			h.add_u64(reinterpret_cast<uintptr_t>(src));
			h.add_u64((static_cast<uint64_t>(static_cast<uint32_t>(first)) << 32) | count);
			h.add_u64((static_cast<uint64_t>(stride) << 32) | offset);
			h.add_u64(content);
			h.add_u64((static_cast<uint64_t>(static_cast<uint32_t>(l.tc1_off)) << 32) | static_cast<uint32_t>(l.tc1_type));
			h.add_u64(static_cast<uint64_t>(d.family));

			auto& g = g_geo[h.h];
			if (g.src == src && g.first == first && g.count == count && g.stride == stride &&
				g.offset == offset && g.content == content && g.max_idx >= g.min_idx)
			{
				g.last_frame = g_frame;
				return &g;
			}

			int lo = INT_MAX, hi = INT_MIN;
			for (UINT v = 0; v < count; v++)
			{
				const int idx = instance_reg_offset(d.family, read_comp(data + v * stride + l.tc1_off, l.tc1_type, 0));
				if (idx < -65536 || idx > 65536) continue;
				lo = (std::min)(lo, idx);
				hi = (std::max)(hi, idx);
			}
			if (lo > hi) return nullptr;

			g.src = src; g.first = first; g.count = count; g.stride = stride; g.offset = offset;
			g.content = content; g.min_idx = lo; g.max_idx = hi; g.last_frame = g_frame;
			return &g;
		}

		// ================================================================
		// Smooth normals (bind_smooth_normals)
		// ================================================================

		struct V3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
		V3 v_sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		V3 v_add(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		V3 v_scale(const V3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		float v_dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		float v_len(const V3& a) { return std::sqrt(v_dot(a, a)); }
		V3 v_cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		bool v_normalize(V3& a)
		{
			const float l = v_len(a);
			if (!(l > 1e-20f) || !std::isfinite(l)) return false;
			a = v_scale(a, 1.0f / l);
			return true;
		}

		// Per-vertex normals for `count` vertices `stride` bytes apart (POSITION
		// at pos_off / pos_type) and the triangle list `tris` (indices into
		// them): see kSmoothCreaseDeg in the header. `out` gets 3 floats per
		// vertex; a vertex no triangle uses gets (0, 0, 1).
		bool smooth_normals(const unsigned char* verts, UINT count, UINT stride, int pos_off, int pos_type,
			const std::vector<uint32_t>& tris, std::vector<float>& out, SmoothNormalsInfo& info)
		{
			const size_t nf = tris.size() / 3;
			if (count == 0 || nf == 0) return false;

			constexpr float kBig = (std::numeric_limits<float>::max)();
			std::vector<V3> p(count);
			V3 lo = { kBig, kBig, kBig }, hi = { -kBig, -kBig, -kBig };
			for (UINT v = 0; v < count; v++)
			{
				const unsigned char* at = verts + static_cast<size_t>(v) * stride + static_cast<size_t>(pos_off);
				V3 q = { read_comp(at, pos_type, 0), read_comp(at, pos_type, 1), read_comp(at, pos_type, 2) };
				if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return false;
				p[v] = q;
				lo = { (std::min)(lo.x, q.x), (std::min)(lo.y, q.y), (std::min)(lo.z, q.z) };
				hi = { (std::max)(hi.x, q.x), (std::max)(hi.y, q.y), (std::max)(hi.z, q.z) };
			}

			// Weld: each vertex joins the first earlier representative within tol.
			const float tol = (std::max)(v_len(v_sub(hi, lo)) * 1e-4f, 1e-6f);
			const float tol2 = tol * tol;
			std::vector<uint32_t> group(count);
			UINT welded = 0;
			for (UINT i = 0; i < count; i++)
			{
				group[i] = i;
				for (UINT j = 0; j < i; j++)
				{
					if (group[j] != j) continue;
					const V3 d = v_sub(p[i], p[j]);
					if (v_dot(d, d) <= tol2) { group[i] = j; break; }
				}
				if (group[i] == i) welded++;
			}

			// Faces: winding normal (length = 2 x area) and its unit direction.
			std::vector<V3> fn(nf), fu(nf);
			std::vector<std::vector<uint32_t>> by_vert(count), by_group(count);
			UINT faces = 0;
			for (size_t f = 0; f < nf; f++)
			{
				const uint32_t a = tris[f * 3], b = tris[f * 3 + 1], c = tris[f * 3 + 2];
				if (a >= count || b >= count || c >= count) return false;
				fn[f] = v_cross(v_sub(p[b], p[a]), v_sub(p[c], p[a]));
				fu[f] = fn[f];
				if (!v_normalize(fu[f])) { fn[f] = {}; fu[f] = {}; continue; }   // degenerate
				faces++;
				const uint32_t fi = static_cast<uint32_t>(f);
				for (const uint32_t v : { a, b, c })
				{
					by_vert[v].push_back(fi);
					auto& gl = by_group[group[v]];
					if (gl.empty() || gl.back() != fi) gl.push_back(fi);
				}
			}
			if (faces == 0) return false;

			constexpr float kPi = 3.14159265358979f;
			const float cos_crease = std::cos(kSmoothCreaseDeg * kPi / 180.0f);
			float min_cos = 1.0f;
			out.assign(static_cast<size_t>(count) * 3, 0.0f);
			for (UINT v = 0; v < count; v++)
			{
				V3 n = { 0.0f, 0.0f, 1.0f };
				const auto& direct = by_vert[v];
				if (!direct.empty())
				{
					// The vertex's own faces, aligned to the winding of the largest.
					uint32_t anchor = direct[0];
					for (const uint32_t f : direct)
						if (v_dot(fn[f], fn[f]) > v_dot(fn[anchor], fn[anchor])) anchor = f;
					const V3& au = fu[anchor];
					V3 ref = {};
					for (const uint32_t f : direct)
						ref = v_add(ref, v_scale(fn[f], v_dot(fu[f], au) >= 0.0f ? 1.0f : -1.0f));
					if (!v_normalize(ref)) ref = au;

					// Every face at the welded position within the crease angle of them.
					V3 sum = {};
					for (const uint32_t f : by_group[group[v]])
					{
						const float d = v_dot(fu[f], ref);
						if (std::fabs(d) >= cos_crease) sum = v_add(sum, v_scale(fn[f], d >= 0.0f ? 1.0f : -1.0f));
					}
					n = sum;
					if (!v_normalize(n)) n = ref;
					if (v_dot(n, au) < 0.0f) n = v_scale(n, -1.0f);

					for (const uint32_t f : direct)
					{
						const float c = v_dot(n, v_scale(fu[f], v_dot(fu[f], au) >= 0.0f ? 1.0f : -1.0f));
						min_cos = (std::min)(min_cos, c);
					}
				}
				out[v * 3] = n.x;
				out[v * 3 + 1] = n.y;
				out[v * 3 + 2] = n.z;
			}

			info.verts = count;
			info.faces = faces;
			info.welded = welded;
			info.max_bend_deg = std::acos(std::clamp(min_cos, -1.0f, 1.0f)) * 180.0f / kPi;
			return true;
		}
	}

	// ====================================================================

	namespace
	{
		// The engine uploads each family's constants to the same registers in
		// every vanilla shader package (all 16 shaderpackage*.sdp agree):
		//   Grass     ModelViewProj c9, ScaleMask c2, AlphaParam c5, InstanceData c20
		//   DistLod   ModelViewProj c0, EyePos c9, AlphaParam c12, InstanceData c20
		//   DistTree  ModelViewProj c0, InstanceData c9
		//   Leaf      ModelViewProj c0, BillboardRight c13, BillboardUp c14, LeafBase c34 (x48)
		//   Particle  ModelViewProj c0, SubTexOffsets c15 (x16)
		//   NoLightUv ModelViewProj c0, TexCoordTranform c4 (2 registers)
		// A draw the engine names is read with these registers, whatever the
		// bound VS declares; a VS whose constant table says otherwise is logged
		// once.
		VsDesc engine_layout(VsFamily f, const VsDesc& reflected)
		{
			VsDesc g;
			g.vs_hash = reflected.vs_hash;
			g.has_table = reflected.has_table;
			g.family = f;
			g.reg_tex_scale = reflected.reg_tex_scale;
			switch (f)
			{
			case VsFamily::Grass:
				g.reg_mvp = 9; g.reg_scale_mask = 2; g.reg_alpha_param = 5; g.reg_instance = 20;
				break;
			case VsFamily::DistLod:
				g.reg_mvp = 0; g.reg_eye_pos = 9; g.reg_alpha_param = 12; g.reg_instance = 20;
				break;
			case VsFamily::DistTree:
				g.reg_mvp = 0; g.reg_instance = 9;
				break;
			case VsFamily::Leaf:
				g.reg_mvp = 0; g.reg_bb_right = 13; g.reg_bb_up = 14; g.reg_leaf_base = 34; g.leaf_base_count = 48;
				break;
			case VsFamily::ParticleSubTex:
				g.reg_mvp = 0; g.reg_sub_tex = 15; g.sub_tex_count = 16;
				break;
			case VsFamily::NoLightUv:
				g.reg_mvp = 0; g.reg_tc_xform = 4;
				break;
			default:
				break;
			}
			return g;
		}

		// The registers the expansion of family f reads, for the mismatch log.
		bool same_inputs(const VsDesc& a, const VsDesc& b)
		{
			switch (a.family)
			{
			case VsFamily::Grass:
				return a.reg_mvp == b.reg_mvp && a.reg_scale_mask == b.reg_scale_mask &&
					a.reg_alpha_param == b.reg_alpha_param && a.reg_instance == b.reg_instance;
			case VsFamily::DistLod:
				return a.reg_mvp == b.reg_mvp && a.reg_eye_pos == b.reg_eye_pos &&
					a.reg_alpha_param == b.reg_alpha_param && a.reg_instance == b.reg_instance;
			case VsFamily::DistTree:
				return a.reg_mvp == b.reg_mvp && a.reg_instance == b.reg_instance;
			case VsFamily::Leaf:
				return a.reg_mvp == b.reg_mvp && a.reg_bb_right == b.reg_bb_right && a.reg_bb_up == b.reg_bb_up &&
					a.reg_leaf_base == b.reg_leaf_base;
			case VsFamily::ParticleSubTex:
				return a.reg_mvp == b.reg_mvp && a.reg_sub_tex == b.reg_sub_tex;
			case VsFamily::NoLightUv:
				return a.reg_mvp == b.reg_mvp && a.reg_tc_xform == b.reg_tc_xform;
			default:
				return true;
			}
		}

		// Engine-layout descriptors by (family, VS hash).
		std::unordered_map<uint64_t, VsDesc> g_engine_descs;

		const VsDesc* engine_desc(VsFamily f, const VsDesc& reflected)
		{
			const uint64_t key = (static_cast<uint64_t>(f) << 32) | reflected.vs_hash;
			const auto [it, inserted] = g_engine_descs.try_emplace(key);
			if (inserted)
			{
				it->second = engine_layout(f, reflected);
				const VsDesc& g = it->second;
				if (reflected.family != f || !same_inputs(g, reflected))
					shared::common::log("VSExpand", std::format(
						"{} pass drawn with VS 0x{:08X}, which {}: expanded with the engine's registers "
						"(mvp=c{} scale=c{} alpha=c{} inst=c{} subTex=c{} tcXform=c{} leafBase=c{} bbR=c{} bbU=c{})",
						shared::common::vs_family_name(f), reflected.vs_hash,
						!reflected.has_table ? "has no constant table"
							: reflected.family == f ? "declares them at other registers"
							: "is not that family by its constant names",
						g.reg_mvp, g.reg_scale_mask, g.reg_alpha_param, g.reg_instance, g.reg_sub_tex, g.reg_tc_xform,
						g.reg_leaf_base, g.reg_bb_right, g.reg_bb_up),
						shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
			return &it->second;
		}

		// The expander family the engine's pass (or, for the distant
		// objects, its shader) names, Unknown when it has no answer.
		// NoLighting passes draw with and without a UV transform
		// (NOLIGHT000 / 001, ...); the engine is asked whether the pass is
		// NoLighting, the bound VS whether it transforms the UVs.
		engine_class::Tri engine_says(VsFamily f, const engine_class::Draw& e, const VsDesc& reflected)
		{
			using engine_class::Pass;
			using engine_class::Tri;
			if (!e.known()) return Tri::Unknown;
			bool yes = false;
			switch (f)
			{
			case VsFamily::Grass:          yes = e.pass == Pass::Grass; break;
			case VsFamily::Leaf:           yes = e.pass == Pass::Leaves; break;
			case VsFamily::DistTree:       yes = e.pass == Pass::DistantTree; break;
			case VsFamily::ParticleSubTex: yes = e.pass == Pass::ParticleSubTex; break;
			case VsFamily::NoLightUv:
				yes = (e.pass == Pass::NoLighting || e.pass == Pass::NoLightingPsys) && reflected.has(reflected.reg_tc_xform);
				break;
			case VsFamily::DistLod:
				// DistantLODShader has no pass type of its own: its class id.
				yes = engine_class::shader_class() == engine_class::ShaderClass::DistantLod;
				break;
			default:
				return Tri::Unknown;
			}
			return yes ? Tri::Yes : Tri::No;
		}

		engine_class::Route route_of(VsFamily f)
		{
			using engine_class::Route;
			switch (f)
			{
			case VsFamily::Grass:          return Route::Grass;
			case VsFamily::Leaf:           return Route::Leaf;
			case VsFamily::DistTree:       return Route::DistTree;
			case VsFamily::DistLod:        return Route::DistLod;
			case VsFamily::ParticleSubTex: return Route::ParticleSubTex;
			default:                       return Route::Unlit;
			}
		}
	}

	const shared::common::VsDesc* candidate(const shared::common::PsSlotMap* psmap)
	{
		const auto& ffp = shared::common::ffp_state::get();

		const auto* d = shared::common::g_vs_classifier.classify(ffp.last_vs());
		if (!d) return nullptr;
		if (!psmap || !psmap->has(shared::common::PsSlotRole::Diffuse)) return nullptr;

		// The family: the engine's pass where it names one, else the VS's
		// constant names. Every family is asked, so each route's engine and
		// shader answers are counted.
		const auto& e = engine_class::current();
		VsFamily family = VsFamily::None;
		for (const VsFamily f : { VsFamily::Grass, VsFamily::Leaf, VsFamily::DistTree, VsFamily::DistLod,
			VsFamily::ParticleSubTex, VsFamily::NoLightUv })
		{
			const engine_class::Tri says = engine_says(f, e, *d);
			if (engine_class::decide(route_of(f), says, d->family == f) && family == VsFamily::None)
			{
				family = f;
				if (says == engine_class::Tri::Yes) d = engine_desc(f, *d);
			}
		}
		if (family == VsFamily::None) return nullptr;

		if (ffp.cur_decl_is_skinned() || ffp.cur_decl_has_pos_t() || ffp.cur_decl_multi_stream()) return nullptr;
		if (ffp.cur_decl_pos_type() != D3DDECLTYPE_FLOAT3 && ffp.cur_decl_pos_type() != D3DDECLTYPE_FLOAT4) return nullptr;

		const bool has_tc1 = ffp.cur_decl_texcoord1_off() >= 0;
		switch (family)
		{
		case VsFamily::ParticleSubTex:
			return (ffp.cur_decl_has_texcoord() && has_tc1) ? d : nullptr;
		case VsFamily::NoLightUv:
			return ffp.cur_decl_has_texcoord() ? d : nullptr;
		case VsFamily::Grass:
		case VsFamily::DistLod:
		case VsFamily::DistTree:
			return has_tc1 ? d : nullptr;
		case VsFamily::Leaf:
			return ffp.cur_decl_has_blendindices() ? d : nullptr;
		default:
			return nullptr;
		}
	}

	namespace
	{
		// Everything one draw's expansion needs, computed once per draw and
		// shared by bind_expanded (the D3D9 copy) and describe_for_api (the
		// Remix-API foliage path). Holds the source VB locked READONLY while
		// `locked` (release()). `f` points into this object: not copyable.
		struct Prep
		{
			Layout l;
			Frame f;
			InstanceMask mask;
			IDirect3DVertexBuffer9* src = nullptr;
			UINT stride = 0, offset = 0, bytes = 0, dst_off = 0, dst_bytes = 0;
			INT first = 0;
			bool dynamic = false;
			const unsigned char* src_bytes = nullptr;
			uint64_t content = 0, params = 0;
			bool locked = false;

			Prep() = default;
			Prep(const Prep&) = delete;
			Prep& operator=(const Prep&) = delete;
			~Prep() { release(); }

			void release()
			{
				if (locked && src) src->Unlock();
				locked = false;
			}
		};

		enum class PrepResult { Declined, Ready, AllCulled };

		// The checks, the source read and the parameter hash of bind_expanded,
		// without touching the device. `quiet`: declines are neither counted
		// nor logged (the caller retries the draw through bind_expanded).
		PrepResult prepare(const VsDesc& d, const shared::common::PsSlotMap* psmap, INT base, UINT skip, UINT count,
			Prep& p, bool quiet, const char*& reject)
		{
			auto& ffp = shared::common::ffp_state::get();
			auto decline = [&](const char* why) {
				reject = why;
				decline_once(d, why, quiet);
				return PrepResult::Declined;
			};

			p.src = ffp.stream_vb(0);
			p.stride = ffp.stream_stride(0);
			p.offset = ffp.stream_offset(0);
			if (!p.src || p.stride == 0 || count == 0 || base < 0) return decline("no stream-0 VB / empty window");
			if (skip > (16u << 20)) return decline("MinVertexIndex out of range");
			p.first = base + static_cast<INT>(skip);   // first vertex actually referenced

			// Element checks for exactly what this family reads and writes.
			p.l = current_layout();
			const Layout& l = p.l;
			if (!fits(l.pos_off, l.pos_type, p.stride)) return decline("POSITION outside the vertex stride");
			switch (d.family)
			{
			case VsFamily::ParticleSubTex:
				if (!fits(l.tc1_off, l.tc1_type, p.stride)) return decline("TEXCOORD1 unreadable");
				if (!fits(l.tc0_off, l.tc0_type, p.stride) ||
					(l.tc0_type != D3DDECLTYPE_FLOAT2 && l.tc0_type != D3DDECLTYPE_FLOAT3 && l.tc0_type != D3DDECLTYPE_FLOAT4))
					return decline("TEXCOORD0 is not FLOAT2+ (cannot rewrite UVs)");
				break;
			case VsFamily::NoLightUv:
				// The API copy (quiet) bakes the UV transform into TEXCOORD0.
				if (quiet && (!fits(l.tc0_off, l.tc0_type, p.stride) ||
					(l.tc0_type != D3DDECLTYPE_FLOAT2 && l.tc0_type != D3DDECLTYPE_FLOAT3 && l.tc0_type != D3DDECLTYPE_FLOAT4)))
					return decline("TEXCOORD0 is not FLOAT2+ (cannot rewrite UVs)");
				break;
			case VsFamily::Grass:
			case VsFamily::DistLod:
			case VsFamily::DistTree:
				if (!fits(l.tc1_off, l.tc1_type, p.stride)) return decline("TEXCOORD1 unreadable");
				break;
			case VsFamily::Leaf:
				if (!fits(l.bi_off, l.bi_type, p.stride)) return decline("BLENDINDICES unreadable");
				break;
			default:
				return decline("not an expander family");
			}

			if (skip + count > (32u << 20) / p.stride) return decline("vertex window too large");
			p.bytes = count * p.stride;
			// The copy keeps the referenced vertices at the index they had relative
			// to BaseVertexIndex, so the index buffer is used unchanged with a base
			// of 0. The [0, skip) prefix is allocated but never written or sent.
			p.dst_off = skip * p.stride;
			p.dst_bytes = p.dst_off + p.bytes;

			D3DVERTEXBUFFER_DESC vbd = {};
			if (FAILED(p.src->GetDesc(&vbd))) return decline("GetDesc failed");
			if (static_cast<uint64_t>(p.offset) + (static_cast<uint64_t>(p.first) + count) * p.stride > vbd.Size)
				return decline("vertex window runs past the end of the VB");

			p.dynamic = (vbd.Usage & D3DUSAGE_DYNAMIC) != 0;
			if (p.dynamic && !dynamic_reads_ok(p.src)) return decline("dynamic VB not readable through the bridge");

			void* src_data = nullptr;
			if (FAILED(p.src->Lock(p.offset + static_cast<UINT>(p.first) * p.stride, p.bytes, &src_data, D3DLOCK_READONLY)) || !src_data)
				return decline("READONLY lock of the source VB failed");
			p.locked = true;
			p.src_bytes = static_cast<const unsigned char*>(src_data);

			const float* c = ffp.vs_const_data();
			p.content = content_hash(p.src_bytes, p.bytes, p.dynamic);

			// ---- Parameters: every constant the expansion reads ----
			Frame& f = p.f;
			f.d = &d; f.l = &p.l; f.c = c; f.ps = ffp.ps_const_data(); f.psmap = psmap;
			f.bake_uv = quiet && d.family == VsFamily::NoLightUv;

			Hasher ph;
			ph.add_u64(static_cast<uint64_t>(d.family));
			switch (d.family)
			{
			case VsFamily::ParticleSubTex:
			{
				ph.add_regs(c, d.reg_sub_tex, d.sub_tex_count);
				if (psmap && psmap->material_color_reg != shared::common::PsSlotMap::kNoReg)
					ph.add(f.ps + psmap->material_color_reg * 4, 4 * sizeof(float));
				break;
			}
			case VsFamily::NoLightUv:
			{
				ph.add_u64(f.bake_uv ? 1u : 0u);
				if (f.bake_uv && d.has(d.reg_tc_xform)) ph.add_regs(c, d.reg_tc_xform, 2);
				if (psmap && psmap->material_color_reg != shared::common::PsSlotMap::kNoReg)
					ph.add(f.ps + psmap->material_color_reg * 4, 4 * sizeof(float));
				break;
			}
			case VsFamily::Grass:
			case VsFamily::DistLod:
			case VsFamily::DistTree:
			{
				const GeoInfo* g = geo_info(d, l, p.src, p.first, count, p.stride, p.offset, p.content, p.src_bytes);
				if (!g) { p.release(); return decline("no instance index in range"); }
				if (g->max_idx - g->min_idx >= 256) { p.release(); return decline("instance index span exceeds 256"); }
				f.min_idx = g->min_idx;
				const int rs = instance_stride_regs(d.family);
				ph.add_u64(static_cast<uint64_t>(static_cast<uint32_t>(g->min_idx)));
				// Quantised: an exact-bit key would rebuild grass batches every
				// frame with the player standing still. The expansion itself
				// still reads the exact constants.
				ph.add_regs_quantized(c, d.reg_instance + g->min_idx * rs, (g->max_idx - g->min_idx + 1) * rs);

				// The expansion reads ScaleMask.xyz only.
				if (d.family == VsFamily::Grass) ph.add_regs_quantized(c, d.reg_scale_mask, 1, 3);
				if (d.family == VsFamily::Grass && !quiet)
					note_grass_params(p.src, p.offset, p.first, count, p.content, c, d, g->min_idx, g->max_idx);
				if (d.family != VsFamily::DistTree)
				{
					compute_mask(d, c, g->min_idx, g->max_idx, p.mask);
					if (p.mask.visible == 0)
					{
						p.release();
						return PrepResult::AllCulled;
					}
					// Only the whole-batch cull above is applied: the per-instance
					// mask depends on the camera, and keying on it would rebuild
					// the batch whenever one instance crosses the fade ring.
				}
				log_mvp_check_once(d);
				break;
			}
			case VsFamily::Leaf:
			{
				ph.add_regs(c, d.reg_leaf_base, d.leaf_base_count);
				const float* r = c + d.reg_bb_right * 4;
				const float* u = c + d.reg_bb_up * 4;
				f.right_len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
				f.up_len = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
				if (f.right_len < 1e-6f || f.up_len < 1e-6f)
				{
					p.release();
					return decline("BillboardRight/Up not set");
				}
				{
					// Only the lengths matter (world-up cards). The engine re-derives
					// them from the camera basis every frame, so quantise away the
					// float noise that would otherwise rebuild on every camera turn.
					const auto q = [](float x) { return static_cast<uint64_t>(static_cast<uint32_t>(std::lround(x * 4096.0f))); };
					f.right_len = static_cast<float>(q(f.right_len)) / 4096.0f;
					f.up_len = static_cast<float>(q(f.up_len)) / 4096.0f;
					ph.add_u64((q(f.right_len) << 32) | q(f.up_len));
				}
				break;
			}
			default:
				break;
			}
			p.params = ph.h;
			return PrepResult::Ready;
		}

		// The family's CPU replay over `count` vertices already copied to `out`.
		void run_expansion(const VsDesc& d, unsigned char* out, UINT count, UINT stride, const Frame& f)
		{
			switch (d.family)
			{
			case VsFamily::ParticleSubTex: expand_particles(out, count, stride, f); break;
			case VsFamily::NoLightUv:      expand_nolight(out, count, stride, f); break;
			case VsFamily::Grass:
			case VsFamily::DistLod:
			case VsFamily::DistTree:       expand_instanced(out, count, stride, f); break;
			case VsFamily::Leaf:           expand_leaves(out, count, stride, f); break;
			default: break;
			}
		}
	}

	Bind bind_expanded(IDirect3DDevice9* dev, const shared::common::VsDesc& d,
		const shared::common::PsSlotMap* psmap, INT base, UINT skip, UINT count)
	{
		PROFILE_ZONE_N("vs_expand::bind_expanded");
		if (!dev || g_bound) return Bind::Declined;
		if (d.family == VsFamily::None) return Bind::Declined;

		auto& ffp = shared::common::ffp_state::get();
		// NoLightUv needs a copy only to carry a MaterialColor below white in
		// its vertex colours; otherwise the game's vertices are drawn as they
		// are (not counted: that is the family's normal route).
		if (d.family == VsFamily::NoLightUv)
		{
			float mc[4];
			if (!material_color_fold(psmap, ffp.ps_const_data(), current_layout(), ffp.stream_stride(0), mc))
				return Bind::Declined;
		}
		auto& st = stats(d.family);
		st.draws++;

		Prep p;
		const char* reject = nullptr;
		switch (prepare(d, psmap, base, skip, count, p, false, reject))
		{
		case PrepResult::Declined: return Bind::Declined;
		case PrepResult::AllCulled: st.culled++; return Bind::AllCulled;
		case PrepResult::Ready: break;
		}
		IDirect3DVertexBuffer9* src = p.src;
		const UINT stride = p.stride, offset = p.offset;
		const INT first = p.first;

		// ---- Lookup ----
		auto* decl = ffp.last_decl();
		Hasher kh;
		kh.add_u64(reinterpret_cast<uintptr_t>(src));
		kh.add_u64((static_cast<uint64_t>(static_cast<uint32_t>(first)) << 32) | count);
		kh.add_u64(skip);
		kh.add_u64(p.content);
		kh.add_u64(p.params);
		const UINT home = static_cast<UINT>(kh.h) & (CACHE_SIZE - 1);

		Entry* hit = nullptr;
		Entry* victim = nullptr;
		for (UINT probe = 0; probe < PROBES; probe++)
		{
			Entry& e = g_cache[(home + probe) & (CACHE_SIZE - 1)];
			if (key_matches(e, src, decl, first, skip, count, stride, offset, d.vs_hash, p.content, p.params)) { hit = &e; break; }
			if (!victim) victim = &e;
			else if (victim->valid && (!e.valid || e.last_frame < victim->last_frame)) victim = &e;
		}

		if (!hit)
		{
			// ---- Build ----
			Entry& e = *victim;
			e.valid = false;

			// Particles are re-expanded every frame, so they get a dynamic
			// buffer (DISCARD renames it on the server instead of stalling on
			// the previous frame's draw); so do NoLightUv copies, rebuilt
			// whenever the engine fades the effect's MaterialColor. Everything
			// else is effectively static once built and lives in MANAGED, like
			// lod_sink's copies.
			const bool want_dyn = p.dynamic || d.family == VsFamily::NoLightUv;
			if (e.dst && (e.capacity < p.dst_bytes || e.dyn_pool != want_dyn))
			{
				e.dst->Release();
				e.dst = nullptr;
				e.capacity = 0;
			}
			if (!e.dst)
			{
				const HRESULT hr = want_dyn
					? dev->CreateVertexBuffer(p.dst_bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &e.dst, nullptr)
					: dev->CreateVertexBuffer(p.dst_bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &e.dst, nullptr);
				if (FAILED(hr) || !e.dst)
				{
					e.dst = nullptr;
					decline_once(d, "CreateVertexBuffer failed");
					return Bind::Declined;
				}
				e.capacity = p.dst_bytes;
				e.dyn_pool = want_dyn;
			}

			void* dst_data = nullptr;
			// DISCARD on a dynamic buffer invalidates all of it, which is fine:
			// the prefix below dst_off is never referenced.
			if (FAILED(e.dst->Lock(p.dst_off, p.bytes, &dst_data, want_dyn ? D3DLOCK_DISCARD : 0)) || !dst_data)
			{
				decline_once(d, "Lock of the expanded VB failed");
				return Bind::Declined;
			}

			auto* out = static_cast<unsigned char*>(dst_data);
			std::memcpy(out, p.src_bytes, p.bytes);
			run_expansion(d, out, count, stride, p.f);
			e.dst->Unlock();

			e.src = src; e.decl = decl; e.first = first; e.skip = skip; e.count = count;
			e.stride = stride; e.offset = offset; e.vs_hash = d.vs_hash;
			e.content = p.content; e.params = p.params;
			e.valid = true;
			st.rebuilds++;
			hit = &e;
		}
		p.release();

		hit->last_frame = g_frame;

		g_saved_vb = src;
		g_saved_offset = offset;
		g_saved_stride = stride;
		g_bound = true;
		dev->SetStreamSource(0, hit->dst, 0, stride);
		return Bind::Bound;
	}

	Bind bind_smooth_normals(IDirect3DDevice9* dev, const api::DrawDesc& draw, SmoothNormalsInfo& info,
		const char*& reject)
	{
		PROFILE_ZONE_N("vs_expand::bind_smooth_normals");
		info = {};
		reject = nullptr;
		if (!dev) { reject = "no device"; return Bind::Declined; }
		if (g_bound) { reject = "a copy is still bound"; return Bind::Declined; }

		auto& ffp = shared::common::ffp_state::get();
		IDirect3DVertexDeclaration9* src_decl = ffp.last_decl();
		IDirect3DVertexBuffer9* src = ffp.stream_vb(0);
		const UINT stride = ffp.stream_stride(0), offset = ffp.stream_offset(0);
		const UINT skip = draw.indexed ? draw.min_vtx : 0u;
		const UINT count = draw.num_vtx;
		if (!src_decl) { reject = "no vertex declaration"; return Bind::Declined; }
		if (ffp.cur_decl_has_normal()) { reject = "the declaration already has a NORMAL"; return Bind::Declined; }
		if (!src || stride == 0 || count == 0) { reject = "no stream-0 VB / empty window"; return Bind::Declined; }
		if (count > kMaxSmoothVerts) { reject = "more than 4096 vertices"; return Bind::Declined; }
		if (skip > (1u << 20)) { reject = "MinVertexIndex out of range"; return Bind::Declined; }
		if (stride + 12u > 255u) { reject = "vertex stride too large for a NORMAL"; return Bind::Declined; }
		const UINT nstride = stride + 12u;

		// Every vertex and index byte of the draw, so a recycled buffer or a
		// rewritten window rebuilds.
		uint64_t content = 0;
		if (!api::content_key(dev, draw, content, reject)) return Bind::Declined;
		Hasher kh;
		kh.add_u64(reinterpret_cast<uintptr_t>(src));
		kh.add_u64((static_cast<uint64_t>(offset) << 32) | stride);
		kh.add_u64(reinterpret_cast<uintptr_t>(src_decl));
		kh.add_u64((static_cast<uint64_t>(static_cast<uint32_t>(draw.base)) << 32) | skip);
		kh.add_u64((static_cast<uint64_t>(count) << 32) | draw.start_index);
		kh.add_u64((static_cast<uint64_t>(draw.prim_count) << 32) |
			(static_cast<uint64_t>(static_cast<uint32_t>(draw.type)) << 1) | (draw.indexed ? 1u : 0u));
		kh.add_u64(content);

		bool built = false;
		auto it = g_normals.find(kh.h);
		if (it == g_normals.end())
		{
			api::Geometry g;
			if (!api::read_geometry(dev, draw, g, reject)) return Bind::Declined;
			const Layout l = current_layout();
			if (!fits(l.pos_off, l.pos_type, stride) || g.stride != stride || g.count != count)
			{
				reject = "POSITION outside the vertex stride";
				return Bind::Declined;
			}
			std::vector<float> normals;
			SmoothNormalsInfo si;
			if (!smooth_normals(g.verts.data(), count, stride, l.pos_off, l.pos_type, g.tris, normals, si))
			{
				reject = "no usable triangles";
				return Bind::Declined;
			}

			// The game's declaration plus NORMAL (FLOAT3) after its last byte.
			D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1] = {};
			UINT n = 0;
			if (FAILED(src_decl->GetDeclaration(nullptr, &n)) || n == 0 || n > MAXD3DDECLLENGTH + 1 ||
				FAILED(src_decl->GetDeclaration(el, &n)))
			{
				reject = "GetDeclaration failed";
				return Bind::Declined;
			}
			UINT end = 0;
			while (end < n && el[end].Stream != 0xFF) end++;
			if (end >= n || end + 1 > MAXD3DDECLLENGTH)
			{
				reject = "declaration has no room for a NORMAL";
				return Bind::Declined;
			}
			for (UINT i = 0; i < end; i++)
			{
				if (el[i].Stream != 0) { reject = "declaration reads more than stream 0"; return Bind::Declined; }
				if (el[i].Usage == D3DDECLUSAGE_NORMAL) { reject = "the declaration already has a NORMAL"; return Bind::Declined; }
			}
			const D3DVERTEXELEMENT9 decl_end = el[end];
			D3DVERTEXELEMENT9 nrm = {};
			nrm.Stream = 0;
			nrm.Offset = static_cast<WORD>(stride);
			nrm.Type = static_cast<BYTE>(D3DDECLTYPE_FLOAT3);
			nrm.Method = static_cast<BYTE>(D3DDECLMETHOD_DEFAULT);
			nrm.Usage = static_cast<BYTE>(D3DDECLUSAGE_NORMAL);
			nrm.UsageIndex = 0;
			el[end] = nrm;
			el[end + 1] = decl_end;

			if (g_normals.size() >= kMaxNormalsEntries)
			{
				for (auto& [k, e] : g_normals) release_normals_entry(e);
				g_normals.clear();
			}

			NormalsEntry e;
			e.stride = nstride;
			e.info = si;
			if (FAILED(dev->CreateVertexDeclaration(el, &e.decl)) || !e.decl)
			{
				e.decl = nullptr;
				reject = "CreateVertexDeclaration failed";
				return Bind::Declined;
			}
			if (FAILED(dev->CreateVertexBuffer((skip + count) * nstride, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
				&e.dst, nullptr)) || !e.dst)
			{
				e.dst = nullptr;
				release_normals_entry(e);
				reject = "CreateVertexBuffer failed";
				return Bind::Declined;
			}
			void* dst_data = nullptr;
			if (FAILED(e.dst->Lock(skip * nstride, count * nstride, &dst_data, 0)) || !dst_data)
			{
				release_normals_entry(e);
				reject = "Lock of the copy failed";
				return Bind::Declined;
			}
			auto* out = static_cast<unsigned char*>(dst_data);
			for (UINT v = 0; v < count; v++)
			{
				std::memcpy(out + static_cast<size_t>(v) * nstride, g.verts.data() + static_cast<size_t>(v) * stride, stride);
				std::memcpy(out + static_cast<size_t>(v) * nstride + stride, &normals[static_cast<size_t>(v) * 3],
					3 * sizeof(float));
			}
			e.dst->Unlock();

			it = g_normals.emplace(kh.h, e).first;
			built = true;
		}

		NormalsEntry& e = it->second;
		e.last_frame = g_frame;
		info = e.info;
		info.built = built;

		g_saved_vb = src;
		g_saved_offset = offset;
		g_saved_stride = stride;
		g_saved_decl = src_decl;
		g_decl_swapped = true;
		g_bound = true;
		dev->SetVertexDeclaration(e.decl);
		dev->SetStreamSource(0, e.dst, 0, e.stride);
		return Bind::Bound;
	}

	Bind describe_for_api(const shared::common::VsDesc& d, const shared::common::PsSlotMap* psmap,
		INT base, UINT skip, UINT count, ApiWindow& out, std::vector<uint8_t>* expanded, const char*& reject)
	{
		PROFILE_ZONE_N("vs_expand::describe_for_api");
		reject = nullptr;
		if (g_bound) { reject = "a copy is still bound"; return Bind::Declined; }
		if (d.family != VsFamily::Grass && d.family != VsFamily::Leaf && d.family != VsFamily::DistLod &&
			d.family != VsFamily::DistTree && d.family != VsFamily::ParticleSubTex && d.family != VsFamily::NoLightUv)
		{
			reject = "not an expander family";
			return Bind::Declined;
		}

		Prep p;
		switch (prepare(d, psmap, base, skip, count, p, true, reject))
		{
		case PrepResult::Declined: return Bind::Declined;
		case PrepResult::AllCulled:
		{
			// Only the slot is known (no parameter hash): enough to find what
			// this batch drew last.
			Hasher sh;
			sh.add_u64(reinterpret_cast<uintptr_t>(p.src));
			sh.add_u64((static_cast<uint64_t>(p.offset) << 32) | p.stride);
			sh.add_u64((static_cast<uint64_t>(static_cast<uint32_t>(p.first)) << 32) | count);
			sh.add_u64(reinterpret_cast<uintptr_t>(shared::common::ffp_state::get().last_decl()));
			sh.add_u64(d.vs_hash);
			out = {};
			out.slot = sh.h;
			out.src = p.src;
			out.stride = p.stride;
			out.dynamic = p.dynamic;
			return Bind::AllCulled;
		}
		case PrepResult::Ready: break;
		}

		// The draw's identity (buffer, window, layout, shader), and what the
		// expanded copy is a pure function of (that + the source bytes + every
		// constant the expansion reads).
		Hasher sh;
		sh.add_u64(reinterpret_cast<uintptr_t>(p.src));
		sh.add_u64((static_cast<uint64_t>(p.offset) << 32) | p.stride);
		sh.add_u64((static_cast<uint64_t>(static_cast<uint32_t>(p.first)) << 32) | count);
		sh.add_u64(reinterpret_cast<uintptr_t>(shared::common::ffp_state::get().last_decl()));
		sh.add_u64(d.vs_hash);
		Hasher kh;
		kh.add_u64(sh.h);
		kh.add_u64(p.content);
		kh.add_u64(p.params);

		out = {};
		out.slot = sh.h;
		out.key = kh.h;
		out.src = p.src;
		out.stride = p.stride;
		out.dynamic = p.dynamic;
		out.pos_off = p.l.pos_off;
		out.pos_type = p.l.pos_type;

		if (expanded)
		{
			expanded->assign(p.src_bytes, p.src_bytes + p.bytes);
			run_expansion(d, expanded->data(), count, p.stride, p.f);
		}
		return Bind::Bound;
	}

	bool describe_instances(const shared::common::VsDesc& d, const shared::common::PsSlotMap* psmap,
		INT base, UINT skip, UINT count, ApiInstances& out, const char*& reject)
	{
		PROFILE_ZONE_N("vs_expand::describe_instances");
		reject = nullptr;
		out = {};
		if (g_bound) { reject = "a copy is still bound"; return false; }
		if (d.family != VsFamily::Grass && d.family != VsFamily::DistLod && d.family != VsFamily::DistTree)
		{
			reject = "not an instanced family";
			return false;
		}

		Prep p;
		if (prepare(d, psmap, base, skip, count, p, true, reject) != PrepResult::Ready)
		{
			if (!reject) reject = "every instance is faded out";
			return false;
		}
		const Layout& l = p.l;
		const float* c = p.f.c;
		const int rs = instance_stride_regs(d.family);
		const float* scale = d.has(d.reg_scale_mask) ? c + d.reg_scale_mask * 4 : nullptr;
		out.tc1_off = l.tc1_off;
		out.tc1_size = static_cast<int>(decl_type_size(l.tc1_type));
		out.vertex_instance.assign(count, -1);

		// Instance index -> slot in out.xforms, as expand_instanced addresses it.
		std::unordered_map<int, int> slot_of;
		for (UINT v = 0; v < count; v++)
		{
			const unsigned char* vert = p.src_bytes + static_cast<size_t>(v) * p.stride;
			const int idx = instance_reg_offset(d.family, read_comp(vert + l.tc1_off, l.tc1_type, 0));
			const int reg = d.reg_instance + idx * rs;
			const int rel = idx - p.f.min_idx;
			if (!(reg >= 0 && reg + rs - 1 <= 255 && rel >= 0 && rel < 256)) continue;

			const auto [it, fresh] = slot_of.try_emplace(idx, static_cast<int>(out.xforms.size()));
			out.vertex_instance[v] = it->second;
			if (!fresh) continue;

			const float* inst = c + reg * 4;
			ApiInstances::Xform x = {};
			switch (d.family)
			{
			case VsFamily::Grass:
			{
				// pos = v0 * (1 + ScaleMask.xyz * 0.01 * I.w) + I.xyz
				const float k = 0.01f * inst[3];
				for (int r = 0; r < 3; r++)
				{
					x.m[r][r] = 1.0f + (scale ? scale[r] : 0.0f) * k;
					x.m[r][3] = inst[r];
				}
				break;
			}
			case VsFamily::DistLod:
			{
				// pos.xyz = v0.xyz * (0.01 * I.w) + I.xyz
				const float k = 0.01f * inst[3];
				for (int r = 0; r < 3; r++)
				{
					x.m[r][r] = k;
					x.m[r][3] = inst[r];
				}
				break;
			}
			case VsFamily::DistTree:
			{
				// pos.xy = Rz(cos, sin) * (v0.xy * P.w); pos.z = v0.z * P.w; += P.xyz
				const float* rot = inst + 4;
				const float s = inst[3];
				x.m[0][0] = rot[0] * s; x.m[0][1] = -rot[1] * s;
				x.m[1][0] = rot[1] * s; x.m[1][1] = rot[0] * s;
				x.m[2][2] = s;
				for (int r = 0; r < 3; r++) x.m[r][3] = inst[r];
				break;
			}
			default:
				break;
			}
			out.xforms.push_back(x);
		}
		if (out.xforms.empty()) { reject = "no addressable instance"; return false; }
		return true;
	}

	void count_api_draw(const shared::common::VsDesc& d, bool culled)
	{
		auto& st = stats(d.family);
		st.draws++;
		if (culled) st.culled++;
	}

	void unbind(IDirect3DDevice9* dev)
	{
		if (!g_bound || !dev) return;
		if (g_decl_swapped) dev->SetVertexDeclaration(g_saved_decl);
		dev->SetStreamSource(0, g_saved_vb, g_saved_offset, g_saved_stride);
		g_bound = false;
		g_saved_vb = nullptr;
		g_decl_swapped = false;
		g_saved_decl = nullptr;
	}

	bool nolight_uv_matrix(const shared::common::VsDesc& d, D3DMATRIX& out)
	{
		if (!d.has(d.reg_tc_xform) || d.reg_tc_xform > 254) return false;
		const float* c = shared::common::ffp_state::get().vs_const_data() + d.reg_tc_xform * 4;
		const float* r0 = c;       // u' = dot(r0.xyz, (u, v, 1))
		const float* r1 = c + 4;   // v' = dot(r1.xyz, (u, v, 1))

		if (r0[0] == 1.0f && r0[1] == 0.0f && r0[2] == 0.0f &&
			r1[0] == 0.0f && r1[1] == 1.0f && r1[2] == 0.0f)
			return false;

		// D3D texture transforms multiply a row vector: (u, v, z, w) * M. The
		// translation goes in _41/_42, not _31/_32: the Remix path-traced
		// surface shader transforms (u, v, 1, 1) (so either row would work
		// there), but its CPU-side consumers (opacity-micromap baking and the
		// instance manager's texcoord readback) transform (u, v, 0, 1) and
		// would drop a _31/_32 offset. Rasterised FFP (dxvk's fixed-function
		// VS pads a FLOAT2 texcoord to (u, v, 1, 0)) is the one consumer that
		// wants _31; the raster image is not what Remix presents, so the path-
		// traced consumers win.
		std::memset(&out, 0, sizeof(out));
		out._11 = r0[0]; out._21 = r0[1]; out._41 = r0[2];
		out._12 = r1[0]; out._22 = r1[1]; out._42 = r1[2];
		out._33 = 1.0f;
		out._44 = 1.0f;
		return true;
	}

	void note_family_used(const shared::common::VsDesc& d, const char* route)
	{
		static bool s_logged[static_cast<size_t>(VsFamily::Count)] = {};
		bool& done = s_logged[static_cast<size_t>(d.family)];
		if (done) return;
		done = true;
		const auto& ffp = shared::common::ffp_state::get();
		shared::common::log("VSExpand", std::format(
			"first use: {} (VS 0x{:08X}) via {} | normal={} tc1={} color={} bi={}",
			shared::common::vs_family_name(d.family), d.vs_hash, route,
			ffp.cur_decl_has_normal() ? 1 : 0, ffp.cur_decl_texcoord1_type(),
			ffp.cur_decl_color_type(), ffp.cur_decl_blend_indices_type()),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN);
	}

	void release_cache()
	{
		for (auto& e : g_cache) release_entry(e);
		g_geo.clear();
		for (auto& [k, e] : g_normals) release_normals_entry(e);
		g_normals.clear();
		g_bound = false;
		g_saved_vb = nullptr;
		g_decl_swapped = false;
		g_saved_decl = nullptr;
	}

	void on_present()
	{
		g_frame++;

		if ((g_frame % kAgeScanPeriod) == 0)
		{
			for (auto& e : g_cache)
				if ((e.dst || e.valid) && g_frame - e.last_frame > kMaxIdleFrames)
					release_entry(e);
			for (auto it = g_geo.begin(); it != g_geo.end(); )
				it = (g_frame - it->second.last_frame > kMaxIdleFrames) ? g_geo.erase(it) : std::next(it);
			for (auto it = g_normals.begin(); it != g_normals.end(); )
			{
				if (g_frame - it->second.last_frame > kMaxIdleFrames)
				{
					release_normals_entry(it->second);
					it = g_normals.erase(it);
				}
				else ++it;
			}
		}

		// Periodic status so hit rates are visible without the overlay: a
		// steady scene should show rebuilds near 0 for everything but
		// particles.
		static UINT s_tick = 0;
		if (++s_tick >= 600)
		{
			s_tick = 0;
			std::string line;
			int entries = 0;
			for (const auto& e : g_cache) if (e.valid) entries++;
			for (size_t i = 1; i < static_cast<size_t>(VsFamily::Count); i++)
			{
				const Stats& s = g_stats[i];
				if (!s.draws && !s.declined) continue;
				line += std::format(" {}: draws={} rebuilds={} culled={} declined={};",
					shared::common::vs_family_name(static_cast<VsFamily>(i)), s.draws, s.rebuilds, s.culled, s.declined);
			}
			if (const auto& g = g_grass_diag; g.compared)
			{
				// Grass key diagnostic (note_grass_params): draws whose constants
				// moved with unchanged vertices, and how many of those still
				// change the quantised key (keyed = a rebuild).
				line += std::format(" grassKey: compared={} changed={} keyed={} range={} instXyz={} (max {:.4g}) instW={} (max {:.4g})"
					" scaleXyz={} (max {:.4g}) scaleW={} (max {:.4g});",
					g.compared, g.changed, g.keyed, g.range, g.xyz, g.max_xyz, g.w, g.max_w, g.scale_xyz, g.max_scale_xyz,
					g.scale_w, g.max_scale_w);
			}
			g_grass_diag = GrassDiag{};
			for (auto it = g_grass_seen.begin(); it != g_grass_seen.end(); )
				it = (g_frame - it->second.last_frame > 600) ? g_grass_seen.erase(it) : std::next(it);
			if (!line.empty())
			{
				shared::common::log("VSExpand", std::format("last 600 frames:{} cached={}", line, entries),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			}
			for (auto& s : g_stats) s = Stats{};
		}
	}
}
