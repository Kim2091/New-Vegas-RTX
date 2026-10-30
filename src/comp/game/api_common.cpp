#include "std_include.hpp"
#include "api_common.hpp"
#include "api_textures.hpp"
#include "api_hair_bake.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "api_identity.hpp"
#include "api_material.hpp"
#include "nvse_link.hpp"
#include "pipboy_cursor.hpp"
#include "tex_pool.hpp"

#include <algorithm>
#include <unordered_map>

#include "shared/common/ffp_state.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_protocol.hpp"

namespace comp::game::api
{
	namespace
	{
		FeatureStats g_terrain{ "terrain" };
		FeatureStats g_far_lod{ "farlod" };
		FeatureStats g_water{ "water" };
		FeatureStats g_statics{ "statics" };
		FeatureStats g_skinned{ "skinned" };
		FeatureStats g_foliage{ "foliage" };
		FeatureStats g_unlit{ "unlit" };
		FeatureStats g_particles{ "particles" };
		TextureStats g_textures;

		UINT g_stats_tick = 0;

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

		UINT index_count(D3DPRIMITIVETYPE type, UINT prims)
		{
			switch (type)
			{
			case D3DPT_TRIANGLELIST:  return prims * 3;
			case D3DPT_TRIANGLESTRIP:
			case D3DPT_TRIANGLEFAN:   return prims ? prims + 2 : 0;
			default:                  return 0;
			}
		}

		// Locks the draw's stream-0 vertex window READONLY. On success the
		// caller must vb->Unlock().
		bool lock_vertex_window(const DrawDesc& d, IDirect3DVertexBuffer9*& vb, UINT& stride,
			const uint8_t*& data, UINT& first, const char*& reject)
		{
			const auto& ffp = shared::common::ffp_state::get();
			if (ffp.cur_decl_multi_stream()) { reject = "declaration reads more than stream 0"; return false; }

			vb = ffp.stream_vb(0);
			stride = ffp.stream_stride(0);
			const UINT offset = ffp.stream_offset(0);
			if (!vb || stride == 0 || d.num_vtx == 0) { reject = "no stream-0 vertex buffer"; return false; }

			const INT first_signed = d.base + static_cast<INT>(d.indexed ? d.min_vtx : 0u);
			if (first_signed < 0) { reject = "negative vertex window"; return false; }
			first = static_cast<UINT>(first_signed);

			D3DVERTEXBUFFER_DESC desc = {};
			if (FAILED(vb->GetDesc(&desc))) { reject = "VB GetDesc failed"; return false; }
			// Dynamic buffers are refilled every frame (their content, and so
			// the cache key, never settles), and with bridge.conf
			// client.optimizedDynamicLock a READONLY lock of one doesn't even
			// return the data.
			if (desc.Usage & D3DUSAGE_DYNAMIC) { reject = "dynamic vertex buffer"; return false; }

			const uint64_t end = static_cast<uint64_t>(offset) + (static_cast<uint64_t>(first) + d.num_vtx) * stride;
			if (end > desc.Size) { reject = "vertex window overruns the buffer"; return false; }

			void* p = nullptr;
			if (FAILED(vb->Lock(offset + first * stride, d.num_vtx * stride, &p, D3DLOCK_READONLY)) || !p)
			{
				reject = "VB lock failed";
				return false;
			}
			data = static_cast<const uint8_t*>(p);
			return true;
		}

		// Locks the draw's index range READONLY. On success the caller must
		// ib->Unlock() and ib->Release().
		bool lock_index_range(IDirect3DDevice9* dev, const DrawDesc& d, IDirect3DIndexBuffer9*& ib,
			const uint8_t*& data, UINT& count, UINT& isz, const char*& reject)
		{
			ib = nullptr;
			if (FAILED(dev->GetIndices(&ib)) || !ib) { reject = "no index buffer"; return false; }

			D3DINDEXBUFFER_DESC desc = {};
			if (FAILED(ib->GetDesc(&desc)))
			{
				ib->Release(); ib = nullptr;
				reject = "IB GetDesc failed";
				return false;
			}
			if (desc.Usage & D3DUSAGE_DYNAMIC)
			{
				ib->Release(); ib = nullptr;
				reject = "dynamic index buffer";
				return false;
			}
			isz = (desc.Format == D3DFMT_INDEX32) ? 4u : 2u;
			count = index_count(d.type, d.prim_count);
			if (count == 0)
			{
				ib->Release(); ib = nullptr;
				reject = "not a triangle primitive";
				return false;
			}
			const uint64_t end = (static_cast<uint64_t>(d.start_index) + count) * isz;
			if (end > desc.Size)
			{
				ib->Release(); ib = nullptr;
				reject = "index range overruns the buffer";
				return false;
			}

			void* p = nullptr;
			if (FAILED(ib->Lock(d.start_index * isz, count * isz, &p, D3DLOCK_READONLY)) || !p)
			{
				ib->Release(); ib = nullptr;
				reject = "IB lock failed";
				return false;
			}
			data = static_cast<const uint8_t*>(p);
			return true;
		}

		uint32_t read_index(const uint8_t* data, UINT isz, UINT i)
		{
			if (isz == 4)
			{
				uint32_t v;
				std::memcpy(&v, data + i * 4u, 4);
				return v;
			}
			uint16_t v;
			std::memcpy(&v, data + i * 2u, 2);
			return v;
		}

		void write_status_line()
		{
			const auto& tx = g_textures;
			const bool retained = shared::common::config::get().remix_api.retained_instances;
			auto feat = [retained](const FeatureStats& s) {
				return std::format("{}: live={} draws={} dropped={} skipped={} created={}{} evicted={} failed={}{}{}{}",
					s.name, s.live, s.draws_last, s.fallbacks_last, s.skipped_last,
					s.created, s.refreshed ? std::format(" refreshed={}", s.refreshed) : std::string(), s.evicted, s.failed,
					retained ? std::format(" retained={} retainedUpdates={} retainedFallback={}",
						s.retained_live, s.retained_updates_last, s.retained_fallbacks_last) : std::string(),
					s.list_hidden ? std::format(" listHidden={}", s.list_hidden) : std::string(),
					(s.last_reject && s.last_reject[0]) ? std::format(" [last reject: {}]", s.last_reject) : std::string());
			};
			const bool imports = shared::common::config::get().remix_api.import_d3d9_textures;
			shared::common::log("RemixApi", std::format(
				"{} heightLayers={} heightPending={} | {} | {} | textures: live={} uploads={} ({:.1f} MB) reused={} evicted={} failed={} normalNoGloss={} defaultPoolToManaged={}{} | protocol RS writes/frame={}",
				feat(g_terrain), g_terrain.height_layers, g_terrain.height_pending, feat(g_far_lod), feat(g_water),
				tx.live, tx.uploads, static_cast<double>(tx.upload_bytes) / (1024.0 * 1024.0),
				tx.reused, tx.evicted, tx.failed, tx.normal_no_gloss, comp::game::tex_pool::promoted_count(),
				imports ? std::format(" imports={} ({:.1f} MB not uploaded) importFallbacks={}{}",
					tx.imports, static_cast<double>(tx.import_bytes) / (1024.0 * 1024.0), tx.import_fallbacks,
					(tx.last_import_reject && tx.last_import_reject[0])
						? std::format(" [last: {}]", tx.last_import_reject) : std::string()) : std::string(),
				remix_protocol::writes_last_frame()),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			comp::game::tex_pool::log_once();
			// [RemixApi] RetainedInstances: the table and where it was replayed.
			if (shared::common::config::get().remix_api.retained_instances)
				shared::common::log("RemixApi", comp::game::api::retained::status(),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			// Draw capture (api_statics.cpp): its own line (reasons make it long).
			shared::common::log("RemixApi", api_submit::statics_status(), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			shared::common::log("RemixApi", api_submit::skinned_status(), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			// [RemixApi] ShaderBakeHair, once a hair draw asked for a bake.
			if (const std::string hb = hair_bake::status(); !hb.empty())
				shared::common::log("RemixApi", hb, shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			// Grass, leaves, distant objects / trees.
			shared::common::log("RemixApi", api_submit::foliage_status(), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			// Unlit FX and particles.
			shared::common::log("RemixApi", api_submit::unlit_status(), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			shared::common::log("RemixApi", api_submit::particles_status(), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}
	}

	// ---- Vertex attributes -----------------------------------------------------

	Attr find_attr(BYTE usage, BYTE usage_index)
	{
		const auto& ffp = shared::common::ffp_state::get();
		const D3DVERTEXELEMENT9* el = ffp.cur_decl_elements();
		for (UINT i = 0; i < ffp.cur_decl_num_elements(); i++)
		{
			if (el[i].Stream == 0 && el[i].Usage == usage && el[i].UsageIndex == usage_index)
				return Attr{ static_cast<int>(el[i].Offset), static_cast<int>(el[i].Type) };
		}
		return {};
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
		default:                    return 0;
		}
	}

	bool attr_fits(const Attr& a, UINT stride)
	{
		const UINT sz = decl_type_size(a.type);
		return a.off >= 0 && sz != 0 && static_cast<UINT>(a.off) + sz <= stride;
	}

	float read_comp(const uint8_t* p, int type, int comp)
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
			unsigned short hv;
			std::memcpy(&hv, p + comp * 2, sizeof(hv));
			return half_to_float(hv);
		}
		default:
			return fill;
		}
	}

	uint64_t decl_hash()
	{
		const auto& ffp = shared::common::ffp_state::get();
		Hash64 h;
		h.add(ffp.cur_decl_elements(), ffp.cur_decl_num_elements() * sizeof(D3DVERTEXELEMENT9));
		return h.get();
	}

	// ---- Geometry --------------------------------------------------------------

	namespace
	{
		// geo_key memo: the draw's buffers, window and declaration plus the
		// buffers' write generations (api_identity) -> the key computed from
		// their contents. Any write lock bumps a generation, so a hit means the
		// sampled contents can't have changed. Used only while the Lock detours
		// are in place and for static buffers whose creation the proxy saw.
		std::unordered_map<uint64_t, uint64_t> g_geo_memo;
		constexpr size_t kGeoMemoMax = 1u << 16;

		bool geo_memo_key(IDirect3DDevice9* dev, const DrawDesc& d, uint64_t& memo)
		{
			const auto& ffp = shared::common::ffp_state::get();
			IDirect3DVertexBuffer9* vb = ffp.stream_vb(0);
			if (!vb || ffp.cur_decl_multi_stream()) return false;
			IDirect3DIndexBuffer9* ib = nullptr;
			if (d.indexed && (FAILED(dev->GetIndices(&ib)) || !ib)) return false;
			const bool tracked = identity::track_buffers(vb, ib);
			const identity::BufferInfo vbi = identity::buffer(vb);
			const identity::BufferInfo ibi = identity::buffer(ib);
			if (ib) ib->Release();   // only compared by address from here on
			if (!tracked || !vbi.created || vbi.dynamic || (ib && !ibi.created)) return false;

			Hash64 h;
			h.ptr(vb);
			h.ptr(ib);
			h.mix(vbi.gen);
			h.mix(ibi.gen);
			h.mix(ffp.stream_offset(0));
			h.mix(ffp.stream_stride(0));
			h.mix(static_cast<uint64_t>(static_cast<uint32_t>(d.base)) | (static_cast<uint64_t>(d.min_vtx) << 32));
			h.mix(static_cast<uint64_t>(d.num_vtx) | (static_cast<uint64_t>(d.start_index) << 32));
			h.mix(static_cast<uint64_t>(d.type) | (static_cast<uint64_t>(d.prim_count) << 8) | (d.indexed ? 1ull << 40 : 0ull));
			h.mix(decl_hash());
			memo = h.get();
			return true;
		}
	}

	bool geo_key(IDirect3DDevice9* dev, const DrawDesc& d, uint64_t& out, const char*& reject)
	{
		PROFILE_ZONE_N("api::geo_key");
		const auto& ffp = shared::common::ffp_state::get();

		uint64_t memo = 0;
		const bool memoable = geo_memo_key(dev, d, memo);
		if (memoable)
		{
			if (const auto it = g_geo_memo.find(memo); it != g_geo_memo.end())
			{
				out = it->second;
				return true;
			}
		}
		const auto remember = [&](uint64_t key) {
			if (!memoable) return;
			if (g_geo_memo.size() >= kGeoMemoMax) g_geo_memo.clear();
			g_geo_memo.emplace(memo, key);
		};

		IDirect3DVertexBuffer9* vb = nullptr;
		UINT stride = 0, first = 0;
		const uint8_t* verts = nullptr;
		if (!lock_vertex_window(d, vb, stride, verts, first, reject)) return false;

		Hash64 h;
		h.ptr(vb);
		h.mix(ffp.stream_offset(0));
		h.mix(stride);
		h.mix(first);
		h.mix(d.num_vtx);
		h.mix(static_cast<uint64_t>(d.type) | (static_cast<uint64_t>(d.prim_count) << 8) | (d.indexed ? 1ull << 40 : 0ull));
		h.mix(decl_hash());

		constexpr UINT kSamples = 16;
		const UINT nv = d.num_vtx;
		for (UINT k = 0; k < kSamples; k++)
		{
			const UINT v = (nv > 1) ? static_cast<UINT>((static_cast<uint64_t>(k) * (nv - 1)) / (kSamples - 1)) : 0u;
			h.add(verts + static_cast<size_t>(v) * stride, stride);
		}
		vb->Unlock();

		if (d.indexed)
		{
			IDirect3DIndexBuffer9* ib = nullptr;
			const uint8_t* idx = nullptr;
			UINT count = 0, isz = 0;
			if (!lock_index_range(dev, d, ib, idx, count, isz, reject)) return false;
			h.ptr(ib);
			h.mix(d.start_index);
			h.mix(d.base);
			h.mix(d.min_vtx);
			for (UINT k = 0; k < kSamples; k++)
			{
				const UINT i = (count > 1) ? static_cast<UINT>((static_cast<uint64_t>(k) * (count - 1)) / (kSamples - 1)) : 0u;
				h.mix(read_index(idx, isz, i));
			}
			ib->Unlock();
			ib->Release();
		}

		out = h.get();
		remember(out);
		return true;
	}

	bool content_key(IDirect3DDevice9* dev, const DrawDesc& d, uint64_t& out, const char*& reject)
	{
		PROFILE_ZONE_N("api::content_key");
		IDirect3DVertexBuffer9* vb = nullptr;
		UINT stride = 0, first = 0;
		const uint8_t* verts = nullptr;
		if (!lock_vertex_window(d, vb, stride, verts, first, reject)) return false;
		Hash64 h;
		h.add(verts, static_cast<size_t>(d.num_vtx) * stride);
		vb->Unlock();
		if (d.indexed)
		{
			IDirect3DIndexBuffer9* ib = nullptr;
			const uint8_t* idx = nullptr;
			UINT count = 0, isz = 0;
			if (!lock_index_range(dev, d, ib, idx, count, isz, reject)) return false;
			h.add(idx, static_cast<size_t>(count) * isz);
			ib->Unlock();
			ib->Release();
		}
		out = h.get();
		return true;
	}

	bool read_geometry(IDirect3DDevice9* dev, const DrawDesc& d, Geometry& g, const char*& reject)
	{
		PROFILE_ZONE_N("api::read_geometry");
		IDirect3DVertexBuffer9* vb = nullptr;
		UINT stride = 0, first = 0;
		const uint8_t* verts = nullptr;
		if (!lock_vertex_window(d, vb, stride, verts, first, reject)) return false;

		g.stride = stride;
		g.count = d.num_vtx;
		g.verts.assign(verts, verts + static_cast<size_t>(d.num_vtx) * stride);
		vb->Unlock();

		g.tris.clear();
		auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
			if (a == b || b == c || a == c) return;   // degenerate (strip stitching)
			g.tris.push_back(a); g.tris.push_back(b); g.tris.push_back(c);
		};

		// Vertex k of the primitive stream, relative to the window start;
		// returns false for an index that points outside the window.
		std::vector<uint32_t> stream;
		if (d.indexed)
		{
			IDirect3DIndexBuffer9* ib = nullptr;
			const uint8_t* idx = nullptr;
			UINT count = 0, isz = 0;
			if (!lock_index_range(dev, d, ib, idx, count, isz, reject)) return false;
			stream.resize(count);
			bool bad = false;
			for (UINT i = 0; i < count; i++)
			{
				const uint32_t v = read_index(idx, isz, i);
				// Indices address (base + index); the window starts at base + min.
				if (v < d.min_vtx || v - d.min_vtx >= d.num_vtx) { bad = true; break; }
				stream[i] = v - d.min_vtx;
			}
			ib->Unlock();
			ib->Release();
			if (bad) { reject = "index outside the declared vertex window"; return false; }
		}
		else
		{
			const UINT count = index_count(d.type, d.prim_count);
			if (count == 0 || count > d.num_vtx) { reject = "not a triangle primitive"; return false; }
			stream.resize(count);
			for (UINT i = 0; i < count; i++) stream[i] = i;
		}

		switch (d.type)
		{
		case D3DPT_TRIANGLELIST:
			for (size_t i = 0; i + 2 < stream.size(); i += 3) emit(stream[i], stream[i + 1], stream[i + 2]);
			break;
		case D3DPT_TRIANGLESTRIP:
			// Odd triangles of a strip are wound the other way round.
			for (size_t i = 0; i + 2 < stream.size(); i++)
			{
				if (i & 1) emit(stream[i + 1], stream[i], stream[i + 2]);
				else       emit(stream[i], stream[i + 1], stream[i + 2]);
			}
			break;
		case D3DPT_TRIANGLEFAN:
			for (size_t i = 1; i + 1 < stream.size(); i++) emit(stream[0], stream[i], stream[i + 1]);
			break;
		default:
			reject = "not a triangle primitive";
			return false;
		}

		if (g.tris.empty()) { reject = "no non-degenerate triangles"; return false; }
		return true;
	}

	bool copy_vertex_bytes(IDirect3DVertexBuffer9* vb, UINT offset, UINT bytes, std::vector<uint8_t>& out,
		const char*& reject)
	{
		PROFILE_ZONE_N("api::copy_vertex_bytes");
		if (!vb || !bytes) { reject = "no vertex buffer"; return false; }
		D3DVERTEXBUFFER_DESC desc = {};
		if (FAILED(vb->GetDesc(&desc))) { reject = "VB GetDesc failed"; return false; }
		if (desc.Usage & D3DUSAGE_DYNAMIC) { reject = "dynamic vertex buffer"; return false; }
		if (static_cast<uint64_t>(offset) + bytes > desc.Size) { reject = "vertex range overruns the buffer"; return false; }
		void* p = nullptr;
		if (FAILED(vb->Lock(offset, bytes, &p, D3DLOCK_READONLY)) || !p) { reject = "VB lock failed"; return false; }
		out.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + bytes);
		vb->Unlock();
		return true;
	}

	bool copy_index_bytes(IDirect3DIndexBuffer9* ib, UINT first, UINT count, std::vector<uint32_t>& out,
		const char*& reject)
	{
		PROFILE_ZONE_N("api::copy_index_bytes");
		if (!ib || !count) { reject = "no index buffer"; return false; }
		D3DINDEXBUFFER_DESC desc = {};
		if (FAILED(ib->GetDesc(&desc))) { reject = "IB GetDesc failed"; return false; }
		if (desc.Usage & D3DUSAGE_DYNAMIC) { reject = "dynamic index buffer"; return false; }
		const UINT isz = (desc.Format == D3DFMT_INDEX32) ? 4u : 2u;
		if ((static_cast<uint64_t>(first) + count) * isz > desc.Size) { reject = "index range overruns the buffer"; return false; }
		void* p = nullptr;
		if (FAILED(ib->Lock(first * isz, count * isz, &p, D3DLOCK_READONLY)) || !p) { reject = "IB lock failed"; return false; }
		out.resize(count);
		for (UINT i = 0; i < count; i++) out[i] = read_index(static_cast<const uint8_t*>(p), isz, i);
		ib->Unlock();
		return true;
	}

	// ---- Transforms ------------------------------------------------------------

	const float* world_matrix()
	{
		return game::get_renderer_matrix(RENDERER_WORLD_OFF);
	}

	remixapi_Transform to_remix(const float* m)
	{
		remixapi_Transform t = {};
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++)
				t.matrix[r][c] = m[c * 4 + r];
		return t;
	}

	bool double_sided_for_cull(IDirect3DDevice9* dev)
	{
		DWORD cull = D3DCULL_CCW;
		if (!dev || FAILED(dev->GetRenderState(D3DRS_CULLMODE, &cull))) return true;
		return cull != D3DCULL_CCW;
	}

	// ---- Runtime ---------------------------------------------------------------

	remixapi_Interface* bridge()
	{
		auto& api = shared::common::remix_api::get();
		if (!api.is_initialized()) return nullptr;
		auto& b = api.m_bridge;
		if (!b.CreateMesh || !b.DestroyMesh || !b.CreateMaterial || !b.DestroyMaterial ||
			!b.CreateTexture || !b.DestroyTexture || !b.DrawInstance)
			return nullptr;
		return &b;
	}

	UINT frame()
	{
		return shared::common::ffp_state::get().frame_count();
	}

	namespace
	{
		// The build budget's fair queue (take_build_budget). A waiter is a
		// build key that was refused and keeps asking; one that stopped asking
		// for kForgetFrames frames (culled, unloaded) leaves the queue.
		constexpr UINT kForgetFrames = 2;

		struct Waiter
		{
			UINT first = 0;   // frame of its first refusal
			UINT last = 0;    // frame it last asked
			bool small_mesh = false;
		};
		std::unordered_map<uint64_t, Waiter> g_waiters;

		struct Pool
		{
			int built = 0;           // grants this frame
			int reserved_left = 0;   // this frame's reservations not claimed yet
		};
		Pool g_main_pool, g_small_pool;
		// This frame's reservations: key -> its pool is the small one.
		std::unordered_map<uint64_t, bool> g_reserved;

		struct BudgetCounters
		{
			uint64_t granted = 0, small_meshes = 0, reserved = 0, refused = 0, long_waits = 0;
			UINT max_wait = 0;
		};
		BudgetCounters g_budget;   // since the last status line

		// Frame start: the oldest waiters still asking get this frame's slots.
		void reserve_budget()
		{
			const UINT now = frame();
			g_reserved.clear();
			g_main_pool = {};
			g_small_pool = {};
			std::vector<std::pair<UINT, uint64_t>> big_waiters, small_waiters;
			for (auto it = g_waiters.begin(); it != g_waiters.end();)
			{
				if (now - it->second.last > kForgetFrames)
				{
					it = g_waiters.erase(it);
					continue;
				}
				(it->second.small_mesh ? small_waiters : big_waiters).emplace_back(it->second.first, it->first);
				++it;
			}
			auto reserve = [](std::vector<std::pair<UINT, uint64_t>>& v, int cap, Pool& pool, bool is_small) {
				const size_t n = (std::min)(v.size(), static_cast<size_t>((std::max)(cap, 0)));
				if (n < v.size()) std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(n), v.end());
				for (size_t i = 0; i < n; i++) g_reserved[v[i].second] = is_small;
				pool.reserved_left = static_cast<int>(n);
			};
			reserve(big_waiters, shared::common::config::get().remix_api.max_builds_per_frame, g_main_pool, false);
			reserve(small_waiters, kSmallBuildsPerFrame, g_small_pool, true);
		}
	}

	bool take_build_budget(uint64_t key, uint32_t verts)
	{
		const UINT now = frame();
		const bool small_mesh = verts && verts <= kSmallMeshVerts;
		Pool& pool = small_mesh ? g_small_pool : g_main_pool;
		const int cap = small_mesh ? kSmallBuildsPerFrame : shared::common::config::get().remix_api.max_builds_per_frame;

		// Texture uploads past this frame's soft budget wait for the next
		// frame, reserved or not (a reservation stands until it is claimed).
		bool grant = false, reserved = false;
		if (small_mesh || !textures::upload_budget_spent())
		{
			if (const auto rit = g_reserved.find(key); rit != g_reserved.end())
			{
				Pool& owner = rit->second ? g_small_pool : g_main_pool;
				owner.reserved_left = (std::max)(owner.reserved_left - 1, 0);
				g_reserved.erase(rit);
				grant = reserved = true;
			}
			else grant = pool.built + pool.reserved_left < cap;
		}

		const auto wit = g_waiters.find(key);
		if (!grant)
		{
			g_budget.refused++;
			if (wit == g_waiters.end()) g_waiters.emplace(key, Waiter{ now, now, small_mesh });
			else wit->second.last = now;
			return false;
		}
		pool.built++;
		g_budget.granted++;
		if (small_mesh) g_budget.small_meshes++;
		if (reserved) g_budget.reserved++;
		if (wit != g_waiters.end())
		{
			const UINT waited = now - wit->second.first;
			if (waited > kLongBudgetWait) g_budget.long_waits++;
			g_budget.max_wait = (std::max)(g_budget.max_wait, waited);
			g_waiters.erase(wit);
		}
		return true;
	}

	std::string budget_status(double frames)
	{
		const UINT now = frame();
		size_t waiting_long = 0;
		UINT oldest = 0;
		for (const auto& entry : g_waiters)
		{
			const UINT age = now - entry.second.first;
			oldest = (std::max)(oldest, age);
			if (age > kLongBudgetWait) waiting_long++;
		}
		frames = (std::max)(frames, 1.0);
		std::string s = std::format(
			"budget: built={:.1f}/frame (small {:.1f}, reserved for the oldest {:.1f}) refused={:.1f}/frame | "
			"queue={} oldest={}f | waited>{}f: {} built, {} still waiting (both should be ~0) maxWait={}f",
			g_budget.granted / frames, g_budget.small_meshes / frames, g_budget.reserved / frames, g_budget.refused / frames,
			g_waiters.size(), oldest, kLongBudgetWait, g_budget.long_waits, waiting_long, g_budget.max_wait);
		g_budget = {};
		return s;
	}

	namespace
	{
		// Searches the readable, non-executable sections of a loaded module
		// for `needle`. No C++ objects here: __try can't share a frame with
		// unwinding.
		bool module_has_bytes_unguarded(const uint8_t* base, const char* needle, size_t n)
		{
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
			const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
			{
				if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)) continue;
				if (sec->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE)) continue;
				const uint8_t* p = base + sec->VirtualAddress;
				const size_t len = sec->Misc.VirtualSize;
				if (len < n) continue;
				for (size_t k = 0; k + n <= len; k++)
					if (p[k] == static_cast<uint8_t>(needle[0]) && std::memcmp(p + k, needle, n) == 0) return true;
			}
			return false;
		}

		bool module_has_bytes(const uint8_t* base, const char* needle, size_t n)
		{
			__try { return module_has_bytes_unguarded(base, needle, n); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

	}

	bool bridge_client_has(const char* marker)
	{
		const auto* base = reinterpret_cast<const uint8_t*>(shared::globals::d3d9_chain_module);
		return base && marker && marker[0] && module_has_bytes(base, marker, std::strlen(marker));
	}

	bool pipboy_own_draw()
	{
		if (!nvse_link::linked()) return false;
		// The Pip-Boy's model, screen and arm render with its zoomed
		// projection; the world behind it renders with the world one.
		return nvse_link::facts().pipboy != FNVR_PB_CLOSED && game::uses_viewmodel_projection();
	}

	bool pipboy_draw()
	{
		if (!nvse_link::linked()) return false;
		const FnvRemixFacts& f = nvse_link::facts();
		if (f.game_state == FNVR_GS_MAIN_MENU || f.game_state == FNVR_GS_LOADING) return true;
		return pipboy_own_draw();
	}

	void scope_per_draw_path()
	{
		const bool own = pipboy_own_draw();
		remix_protocol::set_per_draw_path(own);
		if (own) pipboy_cursor::note_pipboy_draw();
		static bool s_logged = false;
		if (!own || s_logged) return;
		s_logged = true;
		shared::common::log("RemixApi", "first Pip-Boy draw: the Pip-Boy's own draws ask the runtime for the per-draw "
			"path (RS 42 bit 31), decided per draw; the world, the sky and the D3D9-only lists stay with D3D9 v2",
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
	}

	// ---- Stats / logging -------------------------------------------------------

	FeatureStats& terrain_stats() { return g_terrain; }
	FeatureStats& far_lod_stats() { return g_far_lod; }
	FeatureStats& water_stats()   { return g_water; }
	FeatureStats& statics_stats() { return g_statics; }
	FeatureStats& skinned_stats() { return g_skinned; }
	FeatureStats& foliage_stats() { return g_foliage; }
	FeatureStats& unlit_stats()   { return g_unlit; }
	FeatureStats& particle_stats() { return g_particles; }
	TextureStats& texture_stats() { return g_textures; }

	void note_success(FeatureStats& s, const std::string& detail)
	{
		if (s.logged_ok) return;
		s.logged_ok = true;
		shared::common::log("RemixApi", std::format("{}: first API submission OK -- {}", s.name, detail),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	void note_failure(FeatureStats& s, const char* reason, const std::string& detail)
	{
		s.failed++;
		s.last_reject = reason ? reason : "";
		if (s.logged_fail) return;
		s.logged_fail = true;
		shared::common::log("RemixApi", std::format("{}: first API build FAILED ({}){}{} -- this draw is not drawn until a build succeeds",
			s.name, s.last_reject, detail.empty() ? "" : ": ", detail),
			shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
	}

	void end_frame()
	{
		// The next frame's build slots: the oldest waiters first.
		reserve_budget();
		g_terrain.end_frame();
		g_far_lod.end_frame();
		g_water.end_frame();
		g_statics.end_frame();
		g_skinned.end_frame();
		g_foliage.end_frame();
		g_unlit.end_frame();
		g_particles.end_frame();
		// rtx.fnvProtocol.apiGlowTint follows [RemixApi] ApiGlowTint.
		material::push_runtime_options();

		const int interval = shared::common::config::get().remix_api.stats_interval_frames;
		if (interval <= 0) return;
		const bool active = g_terrain.live || g_far_lod.live || g_water.live || g_textures.live ||
			g_terrain.failed || g_far_lod.failed || g_water.failed ||
			g_statics.live || g_statics.failed || g_statics.fallbacks_last ||
			g_skinned.live || g_skinned.failed || g_skinned.fallbacks_last ||
			g_foliage.live || g_foliage.failed || g_foliage.fallbacks_last ||
			g_unlit.live || g_unlit.fallbacks_last || g_particles.live || g_particles.fallbacks_last;
		if (active && ++g_stats_tick >= static_cast<UINT>(interval))
		{
			g_stats_tick = 0;
			write_status_line();
		}
	}

	std::wstring texture_path(uint64_t hash)
	{
		wchar_t buf[24];
		swprintf_s(buf, L"0x%016llX", static_cast<unsigned long long>(hash));
		return buf;
	}

	void init_opaque_ext(remixapi_MaterialInfoOpaqueEXT& ext, float roughness)
	{
		ext = {};
		ext.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
		ext.pNext = nullptr;
		ext.roughnessTexture = L"";
		ext.metallicTexture = L"";
		ext.anisotropy = 0.0f;
		ext.albedoConstant = { 1.0f, 1.0f, 1.0f };
		ext.opacityConstant = 1.0f;
		ext.roughnessConstant = roughness;
		ext.metallicConstant = 0.0f;
		ext.thinFilmThickness_hasvalue = 0;
		ext.thinFilmThickness_value = 0.0f;
		ext.alphaIsThinFilmThickness = 0;
		ext.heightTexture = L"";
		ext.displaceIn = 0.0f;
		ext.useDrawCallAlphaState = 0;
		ext.blendType_hasvalue = 0;
		ext.blendType_value = 0;
		ext.invertedBlend = 0;
		ext.alphaTestType = 7;          // AlphaTestType::kAlways
		ext.alphaReferenceValue = 0;
		ext.displaceOut = 0.0f;
		ext.enableDlssControlMask = 1;  // remix.h MaterialInfoOpaqueEXT defaults
		ext.dlssControlMaskIntensity = 1.0f;
		ext.dlssControlMaskToneStrength = 0.3f;
		ext.dlssControlMaskStructuralStrength = 0.7f;
	}

	void init_material_info(remixapi_MaterialInfo& info, uint64_t hash, void* pnext)
	{
		info = {};
		info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
		info.pNext = pnext;
		info.hash = hash;
		info.albedoTexture = L"";
		info.normalTexture = L"";
		info.tangentTexture = L"";
		info.emissiveTexture = L"";
		info.emissiveIntensity = 0.0f;
		info.emissiveColorConstant = { 0.0f, 0.0f, 0.0f };
		info.spriteSheetRow = 0;
		info.spriteSheetCol = 0;
		info.spriteSheetFps = 0;
		info.filterMode = 1;            // lss::Mdl::Filter::Linear
		info.wrapModeU = 1;             // lss::Mdl::WrapMode::Repeat
		info.wrapModeV = 1;
	}
}
