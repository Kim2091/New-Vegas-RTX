#include "std_include.hpp"
#include "api_textures.hpp"

#include <atomic>

#include "api_identity.hpp"
#include "shared/common/config.hpp"

#pragma warning(push, 0)
#define XXH_INLINE_ALL
#include "xxhash.h"
#pragma warning(pop)

namespace comp::game::api::textures
{
	namespace
	{
		// Salt folded into every content hash, so an API upload can never share
		// a hash with a D3D9-captured texture (whose hash is XXH3 of the same
		// bytes); the runtime keys its texture table by that hash.
		constexpr uint64_t kSalt = 0x464E56415049545Aull;   // "FNVAPITZ"

		// Every upload rides the bridge client's data queue (clientChannelMemSize,
		// 96 MB by default), and the queue has to hold everything the client
		// sends between flushes: the game's own per-frame traffic plus every
		// upload issued that frame. Overflowing it is not an error return: the
		// bridge client shows a message box and exits, which under a fullscreen
		// game looks like a hang. Uploads are bounded three ways:
		//   - per texture: top mips are dropped until the chain fits 4 MB
		//     (2048^2 DXT5 loses its top level; typical land layers fit whole);
		//   - soft, per frame: once 8 MB went out, no new object build starts
		//     (take_build_budget), so builds spread over the next frames;
		//   - hard, per frame: an upload that would pass 24 MB is refused, and
		//     the draw is not drawn until a later frame builds it.
		// Worst case in one frame: one build of 16 textures at 4 MB started
		// just under the soft budget, capped at 24 MB by the hard limit.
		constexpr uint64_t kMaxUploadBytes   = 4ull << 20;
		constexpr uint64_t kFrameSoftBudget  = 8ull << 20;
		constexpr uint64_t kFrameHardCap     = 24ull << 20;

		uint64_t g_frame_upload_bytes = 0;

		struct Upload
		{
			remixapi_TextureHandle handle = nullptr;
			uint32_t refs = 0;
			UINT last_used = 0;
			uint64_t bytes = 0;
		};
		std::unordered_map<uint64_t, Upload> g_uploads;

		// Pointer + flavour -> upload, with what's needed to notice the pointer
		// now names a different image.
		struct PtrEntry
		{
			uint32_t width = 0, height = 0, levels = 0;
			D3DFORMAT format = D3DFMT_UNKNOWN;
			uint64_t sample = 0;
			uint64_t hash = 0;
			UINT last_seen = 0;
		};
		std::unordered_map<uint64_t, PtrEntry> g_ptrs;

		UINT g_sweep_tick = 0;

		uint64_t ptr_key(const void* p, Flavor f)
		{
			return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)) << 3) | static_cast<uint64_t>(f);
		}

		// ---- Formats -----------------------------------------------------------

		enum class Conv : uint8_t
		{
			Copy,        // bytes already in the output layout (BC1, BC3, A8R8G8B8)
			Dxt3ToBc3,   // explicit 4-bit alpha re-encoded as an interpolated BC3 alpha block
			X8,          // X8R8G8B8: copy, force alpha to 255
			R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, L8, A8L8,
		};

		struct FormatInfo
		{
			bool block = false;          // BC / DXT
			uint32_t src_bytes = 0;      // bytes per source block (block) or texel
			uint32_t dst_bytes = 0;      // bytes per output block (block) or texel
			Conv conv = Conv::Copy;
			remixapi_Format unorm = REMIXAPI_FORMAT_B8G8R8A8_UNORM;
			remixapi_Format srgb = REMIXAPI_FORMAT_B8G8R8A8_SRGB;
		};

		bool format_info(D3DFORMAT f, FormatInfo& fi)
		{
			switch (f)
			{
			case D3DFMT_DXT1:
				fi = { true, 8, 8, Conv::Copy, REMIXAPI_FORMAT_BC1_RGB_UNORM, REMIXAPI_FORMAT_BC1_RGB_SRGB };
				return true;
			case D3DFMT_DXT2:
			case D3DFMT_DXT3:
				fi = { true, 16, 16, Conv::Dxt3ToBc3, REMIXAPI_FORMAT_BC3_UNORM, REMIXAPI_FORMAT_BC3_SRGB };
				return true;
			case D3DFMT_DXT4:
			case D3DFMT_DXT5:
				fi = { true, 16, 16, Conv::Copy, REMIXAPI_FORMAT_BC3_UNORM, REMIXAPI_FORMAT_BC3_SRGB };
				return true;
			case D3DFMT_A8R8G8B8: fi = { false, 4, 4, Conv::Copy };     return true;
			case D3DFMT_X8R8G8B8: fi = { false, 4, 4, Conv::X8 };       return true;
			case D3DFMT_R5G6B5:   fi = { false, 2, 4, Conv::R5G6B5 };   return true;
			case D3DFMT_X1R5G5B5: fi = { false, 2, 4, Conv::X1R5G5B5 }; return true;
			case D3DFMT_A1R5G5B5: fi = { false, 2, 4, Conv::A1R5G5B5 }; return true;
			case D3DFMT_A4R4G4B4: fi = { false, 2, 4, Conv::A4R4G4B4 }; return true;
			case D3DFMT_L8:       fi = { false, 1, 4, Conv::L8 };       return true;
			case D3DFMT_A8L8:     fi = { false, 2, 4, Conv::A8L8 };     return true;
			default:              return false;
			}
		}

		// Rows / bytes-per-row of one level as the D3D9 lock lays it out.
		void level_layout(const FormatInfo& fi, uint32_t w, uint32_t h,
			uint32_t& rows, uint32_t& src_row, uint32_t& dst_row)
		{
			if (fi.block)
			{
				const uint32_t bw = (std::max)(1u, (w + 3) / 4);
				rows = (std::max)(1u, (h + 3) / 4);
				src_row = bw * fi.src_bytes;
				dst_row = bw * fi.dst_bytes;
			}
			else
			{
				rows = h;
				src_row = w * fi.src_bytes;
				dst_row = w * fi.dst_bytes;
			}
		}

		uint8_t expand5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
		uint8_t expand6(uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
		uint8_t expand4(uint32_t v) { return static_cast<uint8_t>((v << 4) | v); }

		// DXT3 -> BC3: the colour half is identical (BC3 colour blocks always
		// decode in 4-colour mode, as DXT3's do). The 4-bit explicit alpha is
		// re-encoded as an 8-value interpolated block between the block's own
		// min and max, which reproduces 0/255 and any 2-level mask exactly.
		void dxt3_to_bc3(const uint8_t* src, uint8_t* dst)
		{
			uint8_t a[16];
			for (int i = 0; i < 16; i++)
			{
				const uint8_t nib = static_cast<uint8_t>((src[i / 2] >> ((i & 1) * 4)) & 0xF);
				a[i] = expand4(nib);
			}
			uint8_t lo = 255, hi = 0;
			for (uint8_t v : a) { lo = (std::min)(lo, v); hi = (std::max)(hi, v); }

			uint64_t bits = 0;
			if (hi > lo)
			{
				// a0 > a1: palette a0, a1, then 6 interpolants.
				int pal[8];
				pal[0] = hi; pal[1] = lo;
				for (int k = 1; k <= 6; k++) pal[k + 1] = ((7 - k) * hi + k * lo) / 7;
				for (int i = 0; i < 16; i++)
				{
					int best = 0, best_d = 1 << 30;
					for (int k = 0; k < 8; k++)
					{
						const int d = std::abs(pal[k] - a[i]);
						if (d < best_d) { best_d = d; best = k; }
					}
					bits |= static_cast<uint64_t>(best) << (3 * i);
				}
			}
			dst[0] = hi;
			dst[1] = lo;
			for (int i = 0; i < 6; i++) dst[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
			std::memcpy(dst + 8, src + 8, 8);
		}

		void convert_row(const FormatInfo& fi, const uint8_t* src, uint8_t* dst, uint32_t src_row)
		{
			switch (fi.conv)
			{
			case Conv::Copy:
				std::memcpy(dst, src, src_row);
				return;
			case Conv::Dxt3ToBc3:
				for (uint32_t b = 0; b < src_row / 16; b++) dxt3_to_bc3(src + b * 16, dst + b * 16);
				return;
			case Conv::X8:
				std::memcpy(dst, src, src_row);
				for (uint32_t i = 3; i < src_row; i += 4) dst[i] = 0xFF;
				return;
			default:
				break;
			}

			// Everything else expands to B8G8R8A8.
			const uint32_t n = src_row / fi.src_bytes;
			for (uint32_t i = 0; i < n; i++)
			{
				uint8_t b = 0, g = 0, r = 0, a = 0xFF;
				if (fi.src_bytes == 2)
				{
					uint16_t v;
					std::memcpy(&v, src + i * 2, 2);
					switch (fi.conv)
					{
					case Conv::R5G6B5:   r = expand5((v >> 11) & 31); g = expand6((v >> 5) & 63); b = expand5(v & 31); break;
					case Conv::X1R5G5B5: r = expand5((v >> 10) & 31); g = expand5((v >> 5) & 31); b = expand5(v & 31); break;
					case Conv::A1R5G5B5: r = expand5((v >> 10) & 31); g = expand5((v >> 5) & 31); b = expand5(v & 31);
						a = (v & 0x8000) ? 0xFF : 0x00; break;
					case Conv::A4R4G4B4: a = expand4((v >> 12) & 15); r = expand4((v >> 8) & 15); g = expand4((v >> 4) & 15);
						b = expand4(v & 15); break;
					case Conv::A8L8:     r = g = b = static_cast<uint8_t>(v & 0xFF); a = static_cast<uint8_t>(v >> 8); break;
					default: break;
					}
				}
				else
				{
					r = g = b = src[i];   // L8
				}
				dst[i * 4 + 0] = b; dst[i * 4 + 1] = g; dst[i * 4 + 2] = r; dst[i * 4 + 3] = a;
			}
		}

		// ---- Normal maps: gloss-in-alpha detection and BC1 re-encode -----------
		//
		// The runtime decides per terrain layer from the uploaded texture's own
		// format whether its alpha is gloss (BC3 / B8G8R8A8: yes, BC1: no), so no
		// extra material field has to cross the bridge. Everything here runs once
		// per upload, never per frame.

		// D3D9 source formats whose alpha channel can hold a gloss map. 1-bit
		// alpha (DXT1, A1R5G5B5) is a cut-out at most, never gloss.
		bool source_has_alpha(D3DFORMAT f)
		{
			switch (f)
			{
			case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
			case D3DFMT_A8R8G8B8: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8:
				return true;
			default:
				return false;
			}
		}

		// True when every texel of a BC3 alpha block decodes to 255.
		bool bc3_alpha_block_opaque(const uint8_t* b)
		{
			const int a0 = b[0], a1 = b[1];
			int pal[8];
			pal[0] = a0; pal[1] = a1;
			if (a0 > a1)
				for (int k = 1; k <= 6; k++) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
			else
			{
				for (int k = 1; k <= 4; k++) pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
				pal[6] = 0; pal[7] = 255;
			}
			uint64_t bits = 0;
			for (int i = 0; i < 6; i++) bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
			for (int i = 0; i < 16; i++)
				if (pal[(bits >> (3 * i)) & 7] != 255) return false;
			return true;
		}

		// Alpha is 255 across the whole level (BC3 blocks or B8G8R8A8 texels).
		bool level_alpha_opaque(const uint8_t* data, size_t bytes, bool bc3)
		{
			if (bc3)
			{
				for (size_t o = 0; o + 16 <= bytes; o += 16)
					if (!bc3_alpha_block_opaque(data + o)) return false;
				return true;
			}
			for (size_t o = 3; o < bytes; o += 4)
				if (data[o] != 0xFF) return false;
			return true;
		}

		bool is_normal_map(Flavor f)
		{
			return f == Flavor::NormalMap || f == Flavor::NormalMapNoGloss || f == Flavor::NormalMapFormatAlpha;
		}

		// A normal map keeps its alpha as gloss (else it goes BC1). `top` is the
		// uploaded top level in the output layout.
		bool normal_keeps_gloss(Flavor f, D3DFORMAT src, const uint8_t* top, size_t bytes, bool bc3)
		{
			if (f == Flavor::NormalMapNoGloss || !shared::common::config::get().remix_api.terrain_normal_gloss ||
				!source_has_alpha(src))
				return false;
			return f == Flavor::NormalMapFormatAlpha || !level_alpha_opaque(top, bytes, bc3);
		}

		// BC3 colour block -> BC1 block, lossless. BC3 colour blocks always decode
		// in 4-colour mode; BC1 does so only when c0 > c1, so c0 < c1 swaps the
		// endpoints (index 0<->1, 2<->3), and c0 == c1 (every index = c0) writes
		// index 0 everywhere, since BC1's 3-colour mode would turn index 3 black.
		void bc3_color_to_bc1(const uint8_t* src16, uint8_t* dst8)
		{
			const uint8_t* c = src16 + 8;
			const uint16_t c0 = static_cast<uint16_t>(c[0] | (c[1] << 8));
			const uint16_t c1 = static_cast<uint16_t>(c[2] | (c[3] << 8));
			uint32_t idx = static_cast<uint32_t>(c[4]) | (static_cast<uint32_t>(c[5]) << 8) |
				(static_cast<uint32_t>(c[6]) << 16) | (static_cast<uint32_t>(c[7]) << 24);
			uint16_t e0 = c0, e1 = c1;
			if (c0 < c1) { e0 = c1; e1 = c0; idx ^= 0x55555555u; }
			else if (c0 == c1) idx = 0;
			dst8[0] = static_cast<uint8_t>(e0); dst8[1] = static_cast<uint8_t>(e0 >> 8);
			dst8[2] = static_cast<uint8_t>(e1); dst8[3] = static_cast<uint8_t>(e1 >> 8);
			for (int i = 0; i < 4; i++) dst8[4 + i] = static_cast<uint8_t>(idx >> (8 * i));
		}

		uint16_t pack565(int r, int g, int b)
		{
			const int r5 = (r * 31 + 127) / 255, g6 = (g * 63 + 127) / 255, b5 = (b * 31 + 127) / 255;
			return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
		}

		void unpack565(uint16_t v, int& r, int& g, int& b)
		{
			r = expand5((v >> 11) & 31); g = expand6((v >> 5) & 63); b = expand5(v & 31);
		}

		// One 4x4 block of B8G8R8A8 texels -> BC1 (4-colour mode). Endpoints: the
		// four diagonals of the block's colour bounding box, the one with the
		// smallest squared error wins. Plenty for normal maps (vanilla ships them
		// as DXT anyway); this only runs for uncompressed alpha-less sources.
		void encode_bc1_block(const uint8_t (&px)[16][4], uint8_t* dst8)
		{
			int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
			for (const auto& p : px)
				for (int ch = 0; ch < 3; ch++)
				{
					const int v = p[2 - ch];   // BGRA -> r, g, b
					lo[ch] = (std::min)(lo[ch], v); hi[ch] = (std::max)(hi[ch], v);
				}

			uint64_t best_err = UINT64_MAX;
			uint16_t best_e0 = 0, best_e1 = 0;
			uint32_t best_idx = 0;
			for (int diag = 0; diag < 4; diag++)
			{
				// diag bit 0 flips green, bit 1 flips blue, relative to red.
				const int a[3] = { hi[0], (diag & 1) ? lo[1] : hi[1], (diag & 2) ? lo[2] : hi[2] };
				const int z[3] = { lo[0], (diag & 1) ? hi[1] : lo[1], (diag & 2) ? hi[2] : lo[2] };
				uint16_t e0 = pack565(a[0], a[1], a[2]), e1 = pack565(z[0], z[1], z[2]);
				if (e0 < e1) std::swap(e0, e1);

				int pal[4][3];
				unpack565(e0, pal[0][0], pal[0][1], pal[0][2]);
				unpack565(e1, pal[1][0], pal[1][1], pal[1][2]);
				for (int ch = 0; ch < 3; ch++)
				{
					pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
					pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
				}
				const int ncol = (e0 == e1) ? 1 : 4;   // e0 == e1: index 0 only

				uint64_t err = 0;
				uint32_t idx = 0;
				for (int i = 0; i < 16; i++)
				{
					const int r = px[i][2], g = px[i][1], b = px[i][0];
					int best = 0, best_d = INT32_MAX;
					for (int k = 0; k < ncol; k++)
					{
						const int dr = r - pal[k][0], dg = g - pal[k][1], db = b - pal[k][2];
						const int d = dr * dr + dg * dg + db * db;
						if (d < best_d) { best_d = d; best = k; }
					}
					err += static_cast<uint64_t>(best_d);
					idx |= static_cast<uint32_t>(best) << (2 * i);
				}
				if (err < best_err) { best_err = err; best_e0 = e0; best_e1 = e1; best_idx = idx; }
			}
			dst8[0] = static_cast<uint8_t>(best_e0); dst8[1] = static_cast<uint8_t>(best_e0 >> 8);
			dst8[2] = static_cast<uint8_t>(best_e1); dst8[3] = static_cast<uint8_t>(best_e1 >> 8);
			for (int i = 0; i < 4; i++) dst8[4 + i] = static_cast<uint8_t>(best_idx >> (8 * i));
		}

		// Re-encodes an uploaded chain (levels [first, levels) of a w x h
		// texture, laid out by level_layout) as BC1. `bc3` = the chain is BC3
		// blocks (lossless transcode), else B8G8R8A8 texels (encoded).
		std::vector<uint8_t> chain_to_bc1(const std::vector<uint8_t>& data, bool bc3,
			uint32_t w, uint32_t h, uint32_t first, uint32_t levels)
		{
			std::vector<uint8_t> out;
			if (bc3)
			{
				out.resize(data.size() / 2);
				for (size_t b = 0; b < data.size() / 16; b++) bc3_color_to_bc1(data.data() + b * 16, out.data() + b * 8);
				return out;
			}
			size_t at = 0;
			for (uint32_t l = first; l < levels; l++)
			{
				const uint32_t lw = (std::max)(1u, w >> l), lh = (std::max)(1u, h >> l);
				const uint32_t bw = (std::max)(1u, (lw + 3) / 4), bh = (std::max)(1u, (lh + 3) / 4);
				const uint8_t* lvl = data.data() + at;
				const size_t o = out.size();
				out.resize(o + static_cast<size_t>(bw) * bh * 8);
				for (uint32_t by = 0; by < bh; by++)
					for (uint32_t bx = 0; bx < bw; bx++)
					{
						uint8_t px[16][4];
						for (uint32_t y = 0; y < 4; y++)
							for (uint32_t x = 0; x < 4; x++)
							{
								// Edge blocks of small / odd mips repeat the last texel.
								const uint32_t sx = (std::min)(bx * 4 + x, lw - 1), sy = (std::min)(by * 4 + y, lh - 1);
								std::memcpy(px[y * 4 + x], lvl + (static_cast<size_t>(sy) * lw + sx) * 4, 4);
							}
						encode_bc1_block(px, out.data() + o + (static_cast<size_t>(by) * bw + bx) * 8);
					}
				at += static_cast<size_t>(lw) * lh * 4;
			}
			return out;
		}

		// Sampled hash of level 0 (desc + 64 spots of 16 bytes). One READONLY
		// lock, answered from the bridge client's shadow.
		bool sample_hash(IDirect3DTexture9* t, const D3DSURFACE_DESC& d, const FormatInfo& fi, uint64_t& out)
		{
			uint32_t rows = 0, src_row = 0, dst_row = 0;
			level_layout(fi, d.Width, d.Height, rows, src_row, dst_row);

			D3DLOCKED_RECT lr = {};
			if (FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return false;
			if (lr.Pitch < static_cast<INT>(src_row)) { t->UnlockRect(0); return false; }

			Hash64 h;
			h.mix(d.Width); h.mix(d.Height); h.mix(d.Format);
			const auto* base = static_cast<const uint8_t*>(lr.pBits);
			constexpr uint32_t kSpots = 64, kSpan = 16;
			for (uint32_t k = 0; k < kSpots; k++)
			{
				const uint32_t row = (rows > 1) ? (k * (rows - 1)) / (kSpots - 1) : 0u;
				const uint32_t col = src_row > kSpan ? ((k * 2654435761u) % (src_row - kSpan)) : 0u;
				h.add(base + static_cast<size_t>(row) * lr.Pitch + col, (std::min)(kSpan, src_row));
			}
			t->UnlockRect(0);
			out = h.get();
			return true;
		}

		// `vk_format` nonzero: the texels are in that VkFormat, sent through
		// remixapi_TextureInfoHostImportEXT (half_float_supported()).
		uint64_t create_upload(uint64_t hash, uint32_t w, uint32_t h, uint32_t mips,
			remixapi_Format fmt, const void* data, size_t bytes, const char*& reject, uint32_t vk_format = 0)
		{
			auto* b = bridge();
			if (!b) { reject = "Remix API unavailable"; return 0; }
			if (g_frame_upload_bytes + bytes > kFrameHardCap)
			{
				reject = "upload budget spent this frame";
				return 0;
			}

			remixapi_TextureInfo info = {};
			info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
			info.pNext = nullptr;
			info.hash = hash;
			info.width = w;
			info.height = h;
			info.depth = 1;
			info.mipLevels = mips;
			info.format = fmt;
			info.data = data;
			info.dataSize = bytes;
			remixapi_TextureInfoHostImportEXT host = {};
			if (vk_format)
			{
				host.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO_HOST_IMPORT_EXT;
				host.vkFormat = vk_format;
				info.pNext = &host;
			}

			remixapi_TextureHandle handle = nullptr;
			if (b->CreateTexture(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS || !handle)
			{
				texture_stats().failed++;
				reject = "CreateTexture failed";
				return 0;
			}

			g_frame_upload_bytes += bytes;

			auto& u = g_uploads[hash];
			u.handle = handle;
			u.refs = 1;
			u.last_used = frame();
			u.bytes = bytes;
			auto& ts = texture_stats();
			ts.uploads++;
			ts.upload_bytes += bytes;
			ts.live = static_cast<uint32_t>(g_uploads.size());
			return hash;
		}

		bool reuse(uint64_t hash)
		{
			const auto it = g_uploads.find(hash);
			if (it == g_uploads.end()) return false;
			it->second.refs++;
			it->second.last_used = frame();
			return true;
		}

		// ---- D3D9 import ([RemixApi] ImportD3D9Textures) ------------------------
		//
		// remixapi_ImportD3D9Texture, a named export of the bridge client: the
		// runtime samples the texture the game created on the bridge server's
		// device, so no pixels cross the bridge and Remix keeps no copy. The
		// import is keyed and cached exactly like an upload (same content hash,
		// same g_uploads entry, same DestroyTexture on eviction), so materials
		// and the reference counting don't know the difference.

		PFN_remixapi_ImportD3D9Texture g_import = nullptr;
		// -1: not asked yet. 0: unavailable. 1: available.
		std::atomic<int> g_import_state{ -1 };

		// The import, whatever ImportD3D9Textures says (D3DPOOL_DEFAULT textures
		// need it either way). Render thread: the first call asks the server.
		PFN_remixapi_ImportD3D9Texture import_resolve()
		{
			const int state = g_import_state.load(std::memory_order_acquire);
			if (state >= 0) return state ? g_import : nullptr;

			PFN_remixapi_ImportD3D9Texture fn = nullptr;
			if (HMODULE m = shared::globals::d3d9_chain_module)
				fn = reinterpret_cast<PFN_remixapi_ImportD3D9Texture>(GetProcAddress(m, "remixapi_ImportD3D9Texture"));
			if (!fn)
			{
				shared::common::log("RemixApi",
					"D3D9 texture import: the Remix bridge does not export remixapi_ImportD3D9Texture (older bridge?) -- "
					"textures keep the upload path, and D3DPOOL_DEFAULT textures are created MANAGED",
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				g_import_state.store(0, std::memory_order_release);
				return nullptr;
			}
			// Capability probe (one round trip to the bridge server, once).
			const auto probe = fn(nullptr, 0, 0, 0, nullptr);
			if (probe != REMIXAPI_ERROR_CODE_SUCCESS)
			{
				shared::common::log("RemixApi", std::format(
					"D3D9 texture import: the runtime cannot import D3D9 textures (error {}; needs "
					"rtx.hostImport.enableD3D9TextureImport and a runtime with the export) -- textures keep the upload "
					"path, and D3DPOOL_DEFAULT textures are created MANAGED",
					static_cast<int>(probe)), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				g_import_state.store(0, std::memory_order_release);
				return nullptr;
			}
			shared::common::log("RemixApi", std::format(
				"D3D9 texture import available: D3DPOOL_DEFAULT textures go to Remix by reference; MANAGED ones {} "
				"(ImportD3D9Textures={})",
				shared::common::config::get().remix_api.import_d3d9_textures ? "too" : "are uploaded",
				shared::common::config::get().remix_api.import_d3d9_textures ? 1 : 0));
			g_import = fn;
			g_import_state.store(1, std::memory_order_release);
			return g_import;
		}

		// The import for MANAGED textures: only with ImportD3D9Textures=1.
		PFN_remixapi_ImportD3D9Texture import_fn()
		{
			if (!shared::common::config::get().remix_api.import_d3d9_textures) return nullptr;
			return import_resolve();
		}

		// Formats the import takes: DXVK samples them with the same texels and
		// alpha the upload would produce (DXT2/3 as BC2 instead of re-encoded
		// BC3), and all have an sRGB view. 16-bit and luminance formats keep the
		// upload path, which expands them.
		bool importable_format(D3DFORMAT f)
		{
			switch (f)
			{
			case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
			case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
				return true;
			default:
				return false;
			}
		}

		// The first level whose chain (down to the smallest level) fits one
		// upload, and that chain's size in the output layout. The upload and
		// the import pick the same level, so both get the same hash.
		uint32_t first_level(const FormatInfo& fi, uint32_t w, uint32_t h, uint32_t levels, uint64_t& chain_bytes)
		{
			uint32_t first = 0;
			for (;; first++)
			{
				chain_bytes = 0;
				for (uint32_t l = first; l < levels; l++)
				{
					uint32_t rows, src_row, dst_row;
					level_layout(fi, (std::max)(1u, w >> l), (std::max)(1u, h >> l), rows, src_row, dst_row);
					chain_bytes += static_cast<uint64_t>(rows) * dst_row;
				}
				if (chain_bytes <= kMaxUploadBytes || first + 1 >= levels) break;
			}
			return first;
		}

		// The content hash of an upload or import: the uploaded top level `top`
		// (in the output layout of `fmt`) plus the chain's shape.
		uint64_t content_hash(uint32_t up_w, uint32_t up_h, uint32_t chain_levels, remixapi_Format fmt,
			const uint8_t* top, size_t bytes)
		{
			Hash64 hh;
			hh.mix(kSalt);
			hh.mix(up_w); hh.mix(up_h); hh.mix(chain_levels); hh.mix(static_cast<uint64_t>(fmt));
			hh.add(top, bytes);
			return hh.get();
		}

		// content_hash() of the same top level read as two formats, in one pass
		// (the two hash chains run side by side). Hash64::add, word for word.
		void content_hash_pair(uint32_t up_w, uint32_t up_h, uint32_t chain_levels, remixapi_Format fmt_a,
			remixapi_Format fmt_b, const uint8_t* top, size_t bytes, uint64_t& out_a, uint64_t& out_b)
		{
			Hash64 ha, hb;
			for (Hash64* h : { &ha, &hb })
			{
				h->mix(kSalt);
				h->mix(up_w); h->mix(up_h); h->mix(chain_levels);
			}
			ha.mix(static_cast<uint64_t>(fmt_a));
			hb.mix(static_cast<uint64_t>(fmt_b));
			size_t i = 0;
			for (; i + 8 <= bytes; i += 8)
			{
				uint64_t w;
				std::memcpy(&w, top + i, sizeof(w));
				ha.mix(w);
				hb.mix(w);
			}
			uint64_t tail = 0;
			for (size_t k = 0; i < bytes; i++, k++) tail |= static_cast<uint64_t>(top[i]) << (8 * k);
			ha.mix(tail ^ (static_cast<uint64_t>(bytes) << 56));
			hb.mix(tail ^ (static_cast<uint64_t>(bytes) << 56));
			out_a = ha.get();
			out_b = hb.get();
		}

		// Level `level` of `t` (w x h) converted to the output layout.
		bool read_level(IDirect3DTexture9* t, uint32_t level, const FormatInfo& fi, uint32_t w, uint32_t h,
			std::vector<uint8_t>& out, const char*& why)
		{
			uint32_t rows, src_row, dst_row;
			level_layout(fi, w, h, rows, src_row, dst_row);
			out.resize(static_cast<size_t>(rows) * dst_row);
			D3DLOCKED_RECT lr = {};
			if (FAILED(t->LockRect(level, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) { why = "texture mip lock failed"; return false; }
			if (lr.Pitch < static_cast<INT>(src_row)) { t->UnlockRect(level); why = "unexpected texture pitch"; return false; }
			const auto* src = static_cast<const uint8_t*>(lr.pBits);
			for (uint32_t r = 0; r < rows; r++)
				convert_row(fi, src + static_cast<size_t>(r) * lr.Pitch, out.data() + static_cast<size_t>(r) * dst_row, src_row);
			t->UnlockRect(level);
			return true;
		}

		// Imports `t` under `hash` and caches it like an upload.
		uint64_t register_import(PFN_remixapi_ImportD3D9Texture import_texture, IDirect3DTexture9* t, uint64_t hash,
			uint32_t flags, uint64_t chain_bytes, const char*& why)
		{
			remixapi_TextureHandle handle = nullptr;
			if (import_texture(t, hash, flags, REMIXAPI_TEXTURE_ENCODING_NONE, &handle) != REMIXAPI_ERROR_CODE_SUCCESS || !handle)
			{
				why = "ImportD3D9Texture refused the texture";
				return 0;
			}

			auto& u = g_uploads[hash];
			u.handle = handle;
			u.refs = 1;
			u.last_used = frame();
			u.bytes = 0;   // nothing uploaded: the game's own image
			auto& ts = texture_stats();
			ts.imports++;
			ts.import_bytes += chain_bytes;
			ts.live = static_cast<uint32_t>(g_uploads.size());
			return hash;
		}

		// Imports `t` under the hash its upload would get, or returns 0 (with
		// `why` set) and the caller uploads it instead. Only the uploaded top
		// level is read, for the hash (and a normal map's gloss check), from the
		// bridge client's CPU shadow; nothing counts against the upload budget.
		uint64_t try_import(IDirect3DTexture9* t, const D3DSURFACE_DESC& d, const FormatInfo& fi,
			uint32_t levels, Flavor flavor, const char*& why)
		{
			const auto import_texture = import_fn();
			if (!import_texture) { why = "import unavailable"; return 0; }
			// MANAGED only: a DEFAULT texture has no CPU copy to hash (and is
			// handled before this), SYSTEMMEM / SCRATCH have no GPU image.
			if (d.Pool != D3DPOOL_MANAGED) { why = "not a MANAGED texture"; return 0; }
			if (d.Usage & D3DUSAGE_AUTOGENMIPMAP) { why = "autogen-mip texture"; return 0; }
			if (!importable_format(d.Format)) { why = "format not importable"; return 0; }

			// The same top level the upload path would pick, for the same hash.
			uint64_t chain_bytes = 0;
			const uint32_t first = first_level(fi, d.Width, d.Height, levels, chain_bytes);
			const uint32_t up_w = (std::max)(1u, d.Width >> first), up_h = (std::max)(1u, d.Height >> first);

			std::vector<uint8_t> top;
			if (!read_level(t, first, fi, up_w, up_h, top, why)) return 0;

			remixapi_Format fmt = flavor == Flavor::Srgb ? fi.srgb : fi.unorm;
			uint32_t flags = flavor == Flavor::Srgb ? REMIXAPI_D3D9_TEXTURE_IMPORT_SRGB : 0u;
			// DXVK samples DXT1 as BC1_RGBA, so a colour texture keeps its 1-bit
			// alpha: the cut-out of DXT1 decals (road edges, cracks) and
			// alpha-tested cards. Fully opaque materials ignore it. A DXT1 normal
			// map reads alpha as 1 (the normal-map branch below covers the other
			// formats). X8R8G8B8's view already reads alpha as 1.
			if (d.Format == D3DFMT_DXT1 && is_normal_map(flavor)) flags |= REMIXAPI_D3D9_TEXTURE_IMPORT_OPAQUE_ALPHA;

			// Normal maps: where the upload would re-encode as BC1 (no gloss in
			// alpha), the import reads alpha as 1 instead (the runtime's gloss
			// test sees the same "no alpha" either way), and the hash is taken
			// over the BC1 top level, as the upload's is.
			if (is_normal_map(flavor) && fmt != REMIXAPI_FORMAT_BC1_RGB_UNORM)
			{
				const bool bc3 = fmt == REMIXAPI_FORMAT_BC3_UNORM;
				if (!normal_keeps_gloss(flavor, d.Format, top.data(), top.size(), bc3))
				{
					top = chain_to_bc1(top, bc3, d.Width, d.Height, first, first + 1);
					fmt = REMIXAPI_FORMAT_BC1_RGB_UNORM;
					flags |= REMIXAPI_D3D9_TEXTURE_IMPORT_OPAQUE_ALPHA;
					texture_stats().normal_no_gloss++;
				}
			}

			const uint64_t hash = content_hash(up_w, up_h, levels - first, fmt, top.data(), top.size());
			if (reuse(hash))
			{
				texture_stats().reused++;
				return hash;
			}
			return register_import(import_texture, t, hash, flags, chain_bytes, why);
		}

		// ---- D3DPOOL_DEFAULT textures (tex_pool.hpp) --------------------------
		//
		// What capture_default() read from a DEFAULT texture's UpdateTexture
		// source: the import hash for each format the import can be asked for,
		// which try_import would compute from a MANAGED copy of the same image.

		struct DefaultEntry
		{
			uint32_t gen = 0;   // identity::texture_gen at capture
			uint32_t width = 0, height = 0, levels = 0;
			D3DFORMAT format = D3DFMT_UNKNOWN;
			uint64_t chain_bytes = 0;
			uint64_t hash_unorm = 0, hash_srgb = 0;
			uint64_t hash_bc1 = 0;        // the top level re-encoded as BC1 (normal map without gloss)
			bool alpha_opaque = true;     // alpha 255 across the top level
			bool has_d3d9 = false;
			uint64_t d3d9 = 0;            // XXH3 of level 0 (rtxconf::d3d9_image_hash)
		};

		SRWLOCK g_default_lock = SRWLOCK_INIT;
		std::unordered_map<const void*, DefaultEntry> g_default;
		std::atomic<uint64_t> g_default_captured{ 0 }, g_default_imports{ 0 }, g_default_uncaptured{ 0 },
			g_default_refused{ 0 };

		// DEFAULT textures the API paths asked for while no capture has worked
		// yet. Past kCaptureFailLimit of them the textures are evidently filled
		// some other way, and new ones are created MANAGED (tex_pool.hpp).
		constexpr uint64_t kCaptureFailLimit = 32;
		std::atomic<bool> g_capture_failed{ false };

		bool find_default(IDirect3DTexture9* t, const D3DSURFACE_DESC& d, uint32_t levels, DefaultEntry& out)
		{
			const uint32_t gen = identity::texture_gen(t);
			AcquireSRWLockShared(&g_default_lock);
			const auto it = g_default.find(t);
			const bool ok = it != g_default.end() && it->second.gen == gen && it->second.width == d.Width &&
				it->second.height == d.Height && it->second.levels == levels && it->second.format == d.Format;
			if (ok) out = it->second;
			ReleaseSRWLockShared(&g_default_lock);
			return ok;
		}

		// normal_keeps_gloss() for a captured texture.
		bool captured_keeps_gloss(Flavor f, const DefaultEntry& e)
		{
			if (f == Flavor::NormalMapNoGloss || !shared::common::config::get().remix_api.terrain_normal_gloss ||
				!source_has_alpha(e.format))
				return false;
			return f == Flavor::NormalMapFormatAlpha || !e.alpha_opaque;
		}

		// XXH3 of level `level` of `t` packed without pitch padding, as DXVK
		// stores it (rtxconf::d3d9_image_hash). `d` is that level's desc.
		bool level_xxh3(IDirect3DTexture9* t, uint32_t level, const D3DSURFACE_DESC& d, uint64_t& out)
		{
			FormatInfo fi;
			if (!importable_format(d.Format) || !format_info(d.Format, fi)) return false;
			uint32_t rows, src_row, dst_row;
			level_layout(fi, d.Width, d.Height, rows, src_row, dst_row);
			D3DLOCKED_RECT lr = {};
			if (FAILED(t->LockRect(level, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return false;
			if (lr.Pitch < static_cast<INT>(src_row)) { t->UnlockRect(level); return false; }
			bool ok = true;
			if (lr.Pitch == static_cast<INT>(src_row))
			{
				out = XXH3_64bits(lr.pBits, static_cast<size_t>(rows) * src_row);
			}
			else if (XXH3_state_t* st = XXH3_createState())
			{
				const auto* src = static_cast<const uint8_t*>(lr.pBits);
				XXH3_64bits_reset(st);
				for (uint32_t r = 0; r < rows; r++) XXH3_64bits_update(st, src + static_cast<size_t>(r) * lr.Pitch, src_row);
				out = XXH3_64bits_digest(st);
				XXH3_freeState(st);
			}
			else ok = false;
			t->UnlockRect(level);
			return ok;
		}

		// Imports a D3DPOOL_DEFAULT texture under its captured hash.
		uint64_t acquire_default(IDirect3DTexture9* t, const D3DSURFACE_DESC& d, Flavor flavor, const char*& reject)
		{
			const uint32_t levels = (std::max)(1ul, t->GetLevelCount());
			DefaultEntry e;
			if (!find_default(t, d, levels, e))
			{
				const uint64_t n = g_default_uncaptured.fetch_add(1, std::memory_order_relaxed) + 1;
				const bool keep = shared::common::config::get().remix_api.keep_default_pool_textures;
				if (keep && n == 1)
					shared::common::log("Textures", std::format(
						"a D3DPOOL_DEFAULT texture ({}x{}, format {}, {} levels) reached the Remix API with no captured "
						"content: it was not filled by UpdateTexture, so it can't be imported and its draws are refused. "
						"If world textures are missing, set [RemixApi] KeepDefaultPoolTextures=0",
						d.Width, d.Height, static_cast<int>(d.Format), levels), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				if (keep && n >= kCaptureFailLimit && !g_default_captured.load(std::memory_order_relaxed) &&
					!g_capture_failed.exchange(true))
					shared::common::log("Textures", std::format(
						"{} D3DPOOL_DEFAULT textures reached the Remix API and none was ever filled by UpdateTexture: "
						"textures created from now on are created MANAGED, as with KeepDefaultPoolTextures=0 (the ones "
						"already loaded stay missing until the game reloads them)", n),
						shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				reject = shared::common::config::get().remix_api.keep_default_pool_textures
					? "D3DPOOL_DEFAULT texture with no captured content (not filled by UpdateTexture)"
					: "D3DPOOL_DEFAULT texture (no CPU copy; KeepDefaultPoolTextures=0 makes plain textures MANAGED, so this "
					  "one has usage flags or a D3D9Ex device)";
				return 0;
			}
			const auto import_texture = import_resolve();
			if (!import_texture)
			{
				g_default_refused.fetch_add(1, std::memory_order_relaxed);
				reject = "D3DPOOL_DEFAULT texture and the D3D9 texture import is unavailable";
				return 0;
			}

			// The choices try_import makes, from the captured hashes.
			FormatInfo fi;
			format_info(e.format, fi);
			const remixapi_Format fmt = flavor == Flavor::Srgb ? fi.srgb : fi.unorm;
			uint32_t flags = flavor == Flavor::Srgb ? REMIXAPI_D3D9_TEXTURE_IMPORT_SRGB : 0u;
			if (e.format == D3DFMT_DXT1 && is_normal_map(flavor)) flags |= REMIXAPI_D3D9_TEXTURE_IMPORT_OPAQUE_ALPHA;
			uint64_t hash = fmt == fi.srgb ? e.hash_srgb : e.hash_unorm;
			if (is_normal_map(flavor) && fmt != REMIXAPI_FORMAT_BC1_RGB_UNORM && !captured_keeps_gloss(flavor, e))
			{
				hash = e.hash_bc1;
				flags |= REMIXAPI_D3D9_TEXTURE_IMPORT_OPAQUE_ALPHA;
				texture_stats().normal_no_gloss++;
			}

			if (reuse(hash))
			{
				texture_stats().reused++;
				return hash;
			}
			const char* why = "";
			if (!register_import(import_texture, t, hash, flags, e.chain_bytes, why))
			{
				g_default_refused.fetch_add(1, std::memory_order_relaxed);
				reject = "ImportD3D9Texture refused a D3DPOOL_DEFAULT texture";
				return 0;
			}
			g_default_imports.fetch_add(1, std::memory_order_relaxed);
			return hash;
		}
	}

	uint64_t acquire(IDirect3DBaseTexture9* base, Flavor flavor, const char*& reject)
	{
		PROFILE_ZONE_N("api::textures::acquire");
		if (!base) { reject = "texture slot empty"; return 0; }
		if (base->GetType() != D3DRTYPE_TEXTURE) { reject = "not a 2D texture"; return 0; }
		auto* t = static_cast<IDirect3DTexture9*>(base);

		D3DSURFACE_DESC d = {};
		if (FAILED(t->GetLevelDesc(0, &d))) { reject = "GetLevelDesc failed"; return 0; }
		if (d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC))
		{
			reject = "render-target / dynamic texture";
			return 0;
		}
		// No CPU copy to lock: the hash captured at UpdateTexture, and an import.
		if (d.Pool == D3DPOOL_DEFAULT) return acquire_default(t, d, flavor, reject);
		FormatInfo fi;
		if (!format_info(d.Format, fi)) { reject = "unsupported texture format"; return 0; }
		const uint32_t levels = (std::max)(1ul, t->GetLevelCount());

		uint64_t sample = 0;
		if (!sample_hash(t, d, fi, sample)) { reject = "texture lock failed"; return 0; }

		const uint64_t pk = ptr_key(base, flavor);
		if (auto it = g_ptrs.find(pk); it != g_ptrs.end())
		{
			auto& e = it->second;
			if (e.width == d.Width && e.height == d.Height && e.levels == levels &&
				e.format == d.Format && e.sample == sample && reuse(e.hash))
			{
				e.last_seen = frame();
				return e.hash;
			}
			g_ptrs.erase(it);   // recycled address or evicted upload: rebuild below
		}

		// [RemixApi] ImportD3D9Textures: hand the texture over by reference when
		// the import takes it; anything it refuses uploads below.
		if (shared::common::config::get().remix_api.import_d3d9_textures)
		{
			const char* why = "";
			if (const uint64_t hash = try_import(t, d, fi, levels, flavor, why))
			{
				PtrEntry pe;
				pe.width = d.Width; pe.height = d.Height; pe.levels = levels; pe.format = d.Format;
				pe.sample = sample; pe.hash = hash; pe.last_seen = frame();
				g_ptrs[pk] = pe;
				return hash;
			}
			if (import_fn())
			{
				texture_stats().import_fallbacks++;
				texture_stats().last_import_reject = why;
			}
		}

		// Choose the first level so the chain fits one bridge upload.
		uint64_t chain_bytes = 0;
		const uint32_t first = first_level(fi, d.Width, d.Height, levels, chain_bytes);

		std::vector<uint8_t> data;
		for (uint32_t l = first; l < levels; l++)
		{
			const uint32_t w = (std::max)(1u, d.Width >> l), h = (std::max)(1u, d.Height >> l);
			uint32_t rows, src_row, dst_row;
			level_layout(fi, w, h, rows, src_row, dst_row);

			D3DLOCKED_RECT lr = {};
			if (FAILED(t->LockRect(l, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits)
			{
				reject = "texture mip lock failed";
				return 0;
			}
			if (lr.Pitch < static_cast<INT>(src_row))
			{
				t->UnlockRect(l);
				reject = "unexpected texture pitch";
				return 0;
			}
			const size_t at = data.size();
			data.resize(at + static_cast<size_t>(rows) * dst_row);
			const auto* src = static_cast<const uint8_t*>(lr.pBits);
			for (uint32_t r = 0; r < rows; r++)
				convert_row(fi, src + static_cast<size_t>(r) * lr.Pitch, data.data() + at + static_cast<size_t>(r) * dst_row, src_row);
			t->UnlockRect(l);
		}
		if (data.size() > kMaxUploadBytes) { reject = "texture too large for one upload"; return 0; }

		const uint32_t up_w = (std::max)(1u, d.Width >> first), up_h = (std::max)(1u, d.Height >> first);
		remixapi_Format fmt = flavor == Flavor::Srgb ? fi.srgb : fi.unorm;

		uint32_t rows0, src_row0, dst_row0;
		level_layout(fi, up_w, up_h, rows0, src_row0, dst_row0);

		// Normal maps: no meaningful alpha -> BC1, so the runtime keeps the
		// material roughness instead of reading alpha 1.0 as a mirror (see Flavor).
		if (is_normal_map(flavor) && fmt != REMIXAPI_FORMAT_BC1_RGB_UNORM)
		{
			const bool bc3 = fmt == REMIXAPI_FORMAT_BC3_UNORM;
			const size_t top_bytes = (std::min)(data.size(), static_cast<size_t>(rows0) * dst_row0);
			if (!normal_keeps_gloss(flavor, d.Format, data.data(), top_bytes, bc3))
			{
				data = chain_to_bc1(data, bc3, d.Width, d.Height, first, levels);
				fmt = REMIXAPI_FORMAT_BC1_RGB_UNORM;
				FormatInfo bc1;
				format_info(D3DFMT_DXT1, bc1);
				level_layout(bc1, up_w, up_h, rows0, src_row0, dst_row0);
				texture_stats().normal_no_gloss++;
			}
		}

		// Content hash: the whole uploaded top level plus the chain's shape.
		// Lower mips are derived data; hashing them buys nothing.
		const uint64_t hash = content_hash(up_w, up_h, levels - first, fmt, data.data(),
			(std::min)(data.size(), static_cast<size_t>(rows0) * dst_row0));

		PtrEntry pe;
		pe.width = d.Width; pe.height = d.Height; pe.levels = levels; pe.format = d.Format;
		pe.sample = sample; pe.hash = hash; pe.last_seen = frame();

		if (reuse(hash))
		{
			texture_stats().reused++;
			g_ptrs[pk] = pe;
			return hash;
		}

		if (!create_upload(hash, up_w, up_h, levels - first, fmt, data.data(), data.size(), reject)) return 0;
		g_ptrs[pk] = pe;
		return hash;
	}

	uint64_t acquire_generated_bc1(uint32_t width, uint32_t height, uint32_t mips, const std::vector<uint8_t>& bgra,
		const char*& reject)
	{
		if (bgra.empty() || width == 0 || height == 0 || mips == 0) { reject = "empty generated texture"; return 0; }
		const std::vector<uint8_t> bc1 = chain_to_bc1(bgra, false, width, height, 0, mips);
		return acquire_generated(width, height, mips, bc1.data(), bc1.size(), REMIXAPI_FORMAT_BC1_RGB_UNORM, reject);
	}

	uint64_t acquire_generated(uint32_t width, uint32_t height, uint32_t mips,
		const void* data, size_t bytes, remixapi_Format format, const char*& reject)
	{
		if (!data || bytes == 0 || width == 0 || height == 0) { reject = "empty generated texture"; return 0; }
		Hash64 hh;
		hh.mix(kSalt ^ 0x47454E00ull);   // generated-image domain
		hh.mix(width); hh.mix(height); hh.mix(mips); hh.mix(static_cast<uint64_t>(format));
		hh.add(data, bytes);
		const uint64_t hash = hh.get();
		if (reuse(hash)) { texture_stats().reused++; return hash; }
		return create_upload(hash, width, height, mips, format, data, bytes, reject);
	}

	namespace
	{
		constexpr uint32_t kVkFormatR16G16B16A16Sfloat = 97;   // VK_FORMAT_R16G16B16A16_SFLOAT

		int g_half_float = -1;   // -1 not checked, 0 no, 1 yes

		// One DXT1-5 colour block (8 bytes) -> 16 B8G8R8A8 texels. `four` =
		// always 4-colour mode (the colour half of DXT2-5); alpha = 255, or 0 for
		// DXT1's transparent index.
		void decode_color_block(const uint8_t* b, bool four, uint8_t (&px)[16][4])
		{
			const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8));
			const uint16_t c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
			int pal[4][4];
			for (int k = 0; k < 2; k++)
			{
				const uint16_t c = k ? c1 : c0;
				pal[k][0] = expand5((c >> 11) & 31); pal[k][1] = expand6((c >> 5) & 63); pal[k][2] = expand5(c & 31);
				pal[k][3] = 255;
			}
			const bool opaque4 = four || c0 > c1;
			for (int ch = 0; ch < 3; ch++)
			{
				pal[2][ch] = opaque4 ? (2 * pal[0][ch] + pal[1][ch]) / 3 : (pal[0][ch] + pal[1][ch]) / 2;
				pal[3][ch] = opaque4 ? (pal[0][ch] + 2 * pal[1][ch]) / 3 : 0;
			}
			pal[2][3] = 255;
			pal[3][3] = opaque4 ? 255 : 0;
			const uint32_t idx = static_cast<uint32_t>(b[4]) | (static_cast<uint32_t>(b[5]) << 8) |
				(static_cast<uint32_t>(b[6]) << 16) | (static_cast<uint32_t>(b[7]) << 24);
			for (int i = 0; i < 16; i++)
			{
				const int* c = pal[(idx >> (2 * i)) & 3];
				px[i][0] = static_cast<uint8_t>(c[2]); px[i][1] = static_cast<uint8_t>(c[1]);
				px[i][2] = static_cast<uint8_t>(c[0]); px[i][3] = static_cast<uint8_t>(c[3]);
			}
		}
	}

	bool read_bgra8(IDirect3DBaseTexture9* base, uint32_t& width, uint32_t& height, uint32_t& levels,
		std::vector<uint8_t>& out, const char*& reject)
	{
		out.clear();
		if (!base) { reject = "texture slot empty"; return false; }
		if (base->GetType() != D3DRTYPE_TEXTURE) { reject = "not a 2D texture"; return false; }
		auto* t = static_cast<IDirect3DTexture9*>(base);
		D3DSURFACE_DESC d = {};
		if (FAILED(t->GetLevelDesc(0, &d))) { reject = "GetLevelDesc failed"; return false; }
		if (d.Pool == D3DPOOL_DEFAULT || (d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)))
		{
			reject = "no CPU copy (DEFAULT pool, render target or dynamic texture)";
			return false;
		}
		FormatInfo fi;
		if (!format_info(d.Format, fi)) { reject = "unsupported texture format"; return false; }
		width = d.Width;
		height = d.Height;
		levels = (std::max)(1ul, t->GetLevelCount());

		std::vector<uint8_t> row;
		for (uint32_t l = 0; l < levels; l++)
		{
			const uint32_t w = (std::max)(1u, d.Width >> l), h = (std::max)(1u, d.Height >> l);
			uint32_t rows, src_row, dst_row;
			level_layout(fi, w, h, rows, src_row, dst_row);
			D3DLOCKED_RECT lr = {};
			if (FAILED(t->LockRect(l, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) { reject = "texture mip lock failed"; return false; }
			if (lr.Pitch < static_cast<INT>(src_row)) { t->UnlockRect(l); reject = "unexpected texture pitch"; return false; }
			const auto* src = static_cast<const uint8_t*>(lr.pBits);
			const size_t at = out.size();
			out.resize(at + static_cast<size_t>(w) * h * 4);
			uint8_t* lvl = out.data() + at;
			row.resize(dst_row);
			for (uint32_t r = 0; r < rows; r++)
			{
				const uint8_t* s = src + static_cast<size_t>(r) * lr.Pitch;
				if (!fi.block)
				{
					convert_row(fi, s, row.data(), src_row);
					std::memcpy(lvl + static_cast<size_t>(r) * w * 4, row.data(), static_cast<size_t>(w) * 4);
					continue;
				}
				// Block rows: the colour block is the last 8 bytes of each block.
				for (uint32_t bx = 0; bx * fi.src_bytes < src_row; bx++)
				{
					uint8_t px[16][4];
					decode_color_block(s + bx * fi.src_bytes + (fi.src_bytes - 8), d.Format != D3DFMT_DXT1, px);
					for (uint32_t y = 0; y < 4; y++)
						for (uint32_t x = 0; x < 4; x++)
						{
							const uint32_t tx = bx * 4 + x, ty = r * 4 + y;
							if (tx < w && ty < h) std::memcpy(lvl + (static_cast<size_t>(ty) * w + tx) * 4, px[y * 4 + x], 4);
						}
				}
			}
			t->UnlockRect(l);
		}
		return true;
	}

	bool half_float_supported()
	{
		if (g_half_float < 0)
		{
			static constexpr char kMarker[] = "Forwarding remixapi_TextureInfoHostImportEXT";
			const bool ok = bridge_client_has(kMarker);
			g_half_float = ok ? 1 : 0;
			shared::common::log("RemixApi", ok
				? "textures: the bridge client forwards TextureInfoHostImportEXT -- float textures can be uploaded"
				: "textures: the bridge client does NOT forward TextureInfoHostImportEXT (older bridge) -- no float "
				  "textures (FaceGen skin tint stays off)",
				ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		}
		return g_half_float == 1;
	}

	uint64_t acquire_generated_half(uint32_t width, uint32_t height, uint32_t mips,
		const uint16_t* data, size_t bytes, const char*& reject)
	{
		if (!data || bytes == 0 || width == 0 || height == 0) { reject = "empty generated texture"; return 0; }
		if (!half_float_supported()) { reject = "the bridge can't upload float textures"; return 0; }
		Hash64 hh;
		hh.mix(kSalt ^ 0x47454E00ull);   // generated-image domain
		hh.mix(width); hh.mix(height); hh.mix(mips); hh.mix(static_cast<uint64_t>(kVkFormatR16G16B16A16Sfloat) << 32);
		hh.add(data, bytes);
		const uint64_t hash = hh.get();
		if (reuse(hash)) { texture_stats().reused++; return hash; }
		// format is a placeholder the bridge requires; vkFormat overrides it.
		return create_upload(hash, width, height, mips, REMIXAPI_FORMAT_R8G8B8A8_UNORM, data, bytes, reject,
			kVkFormatR16G16B16A16Sfloat);
	}

	bool add_ref(uint64_t hash)
	{
		return reuse(hash);
	}

	void release(uint64_t hash)
	{
		if (!hash) return;
		const auto it = g_uploads.find(hash);
		if (it == g_uploads.end() || it->second.refs == 0) return;
		it->second.refs--;
		it->second.last_used = frame();
	}

	void purge_unreferenced()
	{
		auto* b = bridge();
		for (auto it = g_uploads.begin(); it != g_uploads.end();)
		{
			if (it->second.refs == 0)
			{
				if (b && it->second.handle) b->DestroyTexture(it->second.handle);
				texture_stats().evicted++;
				it = g_uploads.erase(it);
			}
			else ++it;
		}
		texture_stats().live = static_cast<uint32_t>(g_uploads.size());
		for (auto it = g_ptrs.begin(); it != g_ptrs.end();)
		{
			if (!g_uploads.contains(it->second.hash)) it = g_ptrs.erase(it);
			else ++it;
		}
	}

	bool upload_budget_spent()
	{
		return g_frame_upload_bytes >= kFrameSoftBudget;
	}

	void on_present()
	{
		g_frame_upload_bytes = 0;
		if (++g_sweep_tick < 60) return;
		g_sweep_tick = 0;

		const UINT now = frame();
		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.texture_evict_frames);
		auto* b = bridge();

		for (auto it = g_uploads.begin(); it != g_uploads.end();)
		{
			const auto& u = it->second;
			if (u.refs == 0 && now - u.last_used > keep)
			{
				if (b && u.handle) b->DestroyTexture(u.handle);
				texture_stats().evicted++;
				it = g_uploads.erase(it);
			}
			else ++it;
		}
		texture_stats().live = static_cast<uint32_t>(g_uploads.size());

		// Pointer entries whose upload is gone, or that haven't been looked up
		// for a long time, only cost memory; drop them.
		for (auto it = g_ptrs.begin(); it != g_ptrs.end();)
		{
			if (!g_uploads.contains(it->second.hash) || now - it->second.last_seen > keep)
				it = g_ptrs.erase(it);
			else ++it;
		}
	}

	bool importable(D3DFORMAT format)
	{
		return importable_format(format);
	}

	void resolve_import()
	{
		import_resolve();
	}

	bool import_available()
	{
		return g_import_state.load(std::memory_order_acquire) == 1;
	}

	bool capture_failed()
	{
		return g_capture_failed.load(std::memory_order_relaxed);
	}

	void capture_default(IDirect3DTexture9* src, IDirect3DTexture9* dst)
	{
		PROFILE_ZONE_N("api::textures::capture_default");
		if (!src || !dst) return;
		D3DSURFACE_DESC dd = {};
		if (FAILED(dst->GetLevelDesc(0, &dd)) || dd.Pool != D3DPOOL_DEFAULT) return;
		forget_default(dst);
		if (dd.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC | D3DUSAGE_AUTOGENMIPMAP)) return;
		FormatInfo fi;
		if (!importable_format(dd.Format) || !format_info(dd.Format, fi)) return;

		// UpdateTexture fills the destination's levels from the source's
		// smallest ones.
		const uint32_t levels = (std::max)(1ul, dst->GetLevelCount());
		const uint32_t src_levels = src->GetLevelCount();
		if (src_levels < levels) return;
		const uint32_t skip = src_levels - levels;
		D3DSURFACE_DESC sd = {};
		if (FAILED(src->GetLevelDesc(skip, &sd)) || sd.Pool != D3DPOOL_SYSTEMMEM || sd.Format != dd.Format ||
			sd.Width != dd.Width || sd.Height != dd.Height)
			return;

		DefaultEntry e;
		e.width = dd.Width; e.height = dd.Height; e.levels = levels; e.format = dd.Format;
		const uint32_t first = first_level(fi, dd.Width, dd.Height, levels, e.chain_bytes);
		const uint32_t up_w = (std::max)(1u, dd.Width >> first), up_h = (std::max)(1u, dd.Height >> first);
		std::vector<uint8_t> top;
		const char* why = "";
		if (!read_level(src, skip + first, fi, up_w, up_h, top, why)) return;

		// Every hash acquire can ask for: the top level as is, read as UNORM or
		// sRGB, and re-encoded as BC1 for a normal map without gloss.
		const uint32_t chain_levels = levels - first;
		content_hash_pair(up_w, up_h, chain_levels, fi.unorm, fi.srgb, top.data(), top.size(), e.hash_unorm, e.hash_srgb);
		if (fi.unorm != REMIXAPI_FORMAT_BC1_RGB_UNORM)
		{
			const bool bc3 = fi.unorm == REMIXAPI_FORMAT_BC3_UNORM;
			e.alpha_opaque = level_alpha_opaque(top.data(), top.size(), bc3);
			const std::vector<uint8_t> bc1 = chain_to_bc1(top, bc3, dd.Width, dd.Height, first, first + 1);
			e.hash_bc1 = content_hash(up_w, up_h, chain_levels, REMIXAPI_FORMAT_BC1_RGB_UNORM, bc1.data(), bc1.size());
		}
		e.has_d3d9 = level_xxh3(src, skip, sd, e.d3d9);
		e.gen = identity::texture_gen(dst);

		AcquireSRWLockExclusive(&g_default_lock);
		g_default[dst] = e;
		ReleaseSRWLockExclusive(&g_default_lock);
		g_default_captured.fetch_add(1, std::memory_order_relaxed);
	}

	void forget_default(const void* tex)
	{
		if (!tex) return;
		AcquireSRWLockExclusive(&g_default_lock);
		g_default.erase(tex);
		ReleaseSRWLockExclusive(&g_default_lock);
	}

	bool default_d3d9_hash(IDirect3DTexture9* tex, uint64_t& out)
	{
		if (!tex) return false;
		D3DSURFACE_DESC d = {};
		if (FAILED(tex->GetLevelDesc(0, &d))) return false;
		DefaultEntry e;
		if (!find_default(tex, d, (std::max)(1ul, tex->GetLevelCount()), e) || !e.has_d3d9) return false;
		out = e.d3d9;
		return true;
	}

	DefaultStats default_stats()
	{
		DefaultStats s;
		s.captured = g_default_captured.load(std::memory_order_relaxed);
		s.imports = g_default_imports.load(std::memory_order_relaxed);
		s.uncaptured = g_default_uncaptured.load(std::memory_order_relaxed);
		s.refused = g_default_refused.load(std::memory_order_relaxed);
		AcquireSRWLockShared(&g_default_lock);
		s.entries = g_default.size();
		ReleaseSRWLockShared(&g_default_lock);
		return s;
	}
}
