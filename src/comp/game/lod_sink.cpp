#include "std_include.hpp"
#include "lod_sink.hpp"
#include "scene_graph.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/config.hpp"

namespace comp::game::lod_sink
{
	namespace
	{
		// VS constant registers the distant-terrain shader reads, from
		// SLS2002.vso's constant table (ObjToCubeSpace c8[2], HighDetailRange c12,
		// GeomorphParams c19). lod_sink.hpp has the decoded arithmetic.
		constexpr int REG_OBJ_TO_CUBE = 8;
		constexpr int REG_HIGH_DETAIL = 12;
		constexpr int REG_GEOMORPH    = 19;

		float read_scalar(const unsigned char* p, int type)
		{
			switch (type)
			{
			case D3DDECLTYPE_FLOAT1:
			case D3DDECLTYPE_FLOAT2:
			case D3DDECLTYPE_FLOAT3:
			case D3DDECLTYPE_FLOAT4:
			{
				float v;
				std::memcpy(&v, p, sizeof(v));
				return v;
			}
			case D3DDECLTYPE_FLOAT16_2:
			case D3DDECLTYPE_FLOAT16_4:
			{
				// half -> float. FNV's LOD decls use FLOAT1; this keeps an
				// unexpected packing correct instead of producing garbage geometry.
				unsigned short h;
				std::memcpy(&h, p, sizeof(h));
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
			default:
				return 0.0f;
			}
		}
	}

	void gather_constants(float out[kNumConstants])
	{
		const float* vs = shared::common::ffp_state::get().vs_const_data();
		std::memcpy(out + 0, &vs[REG_OBJ_TO_CUBE * 4], 8 * sizeof(float));   // c8, c9
		std::memcpy(out + 8, &vs[REG_HIGH_DETAIL * 4], 4 * sizeof(float));   // c12
		std::memcpy(out + 12, &vs[REG_GEOMORPH * 4], 4 * sizeof(float));     // c19
	}

	float morphed_z(const float* c, float pz, float coarse_z)
	{
		// lrp r1.z, c19.x, r0.z, v2.x  ->  lerp(v2.x, pos.z, GeomorphParams.x)
		return coarse_z + c[12] * (pz - coarse_z);
	}

	bool inside(const float* c, float px, float py, float mz, float pw)
	{
		const float* row0 = c + 0;
		const float* row1 = c + 4;
		const float* hdr  = c + 8;

		// The box test runs on the MORPHED position (r1 = x, y, morphedZ, w).
		const float cx = row0[0] * px + row0[1] * py + row0[2] * mz + row0[3] * pw;
		const float cy = row1[0] * px + row1[1] * py + row1[2] * mz + row1[3] * pw;

		return (std::fabs(cx - hdr[0]) < hdr[2])
		    && (std::fabs(cy - hdr[1]) < hdr[3]);
	}

	float sunk_z(const float* c, float px, float py, float pz, float pw, float coarse_z)
	{
		const float mz = morphed_z(c, pz, coarse_z);
		return inside(c, px, py, mz, pw) ? (mz - c[13]) : mz;
	}

	float read_coarse_z(const unsigned char* p, int decl_type)
	{
		return read_scalar(p, decl_type);
	}
}

namespace comp::game::lod_debug
{
	namespace
	{
		int g_far = 0, g_near = 0, g_far_shown = 0, g_near_shown = 0;
		// What the near (with-normal, PS 0x6626FACE) LOD draws are: the
		// NiGeometry of each (dispatcher hook), against the land quads drawn
		// this frame (API_TERRAIN / FFP_TERRAIN).
		std::unordered_set<uintptr_t> g_near_geos, g_land_geos;
		int g_near_no_geo = 0;
		int g_near_geos_shown = 0, g_near_over_land_shown = 0, g_near_no_geo_shown = 0;
	}

	void count_far()  { g_far++; }
	void count_near()
	{
		g_near++;
		if (const uintptr_t geo = scene_graph::current_geometry()) g_near_geos.insert(geo);
		else g_near_no_geo++;
	}
	void note_land()
	{
		if (const uintptr_t geo = scene_graph::current_geometry()) g_land_geos.insert(geo);
	}
	int  far_draws()  { return g_far_shown; }
	int  near_draws() { return g_near_shown; }

	void on_present()
	{
		// Latch last frame's totals so the ImGui readout isn't whatever partial
		// count the overlay happened to be drawn at.
		g_far_shown = g_far;  g_far = 0;
		g_near_shown = g_near; g_near = 0;
		g_near_geos_shown = static_cast<int>(g_near_geos.size());
		g_near_over_land_shown = 0;
		for (const uintptr_t geo : g_near_geos)
			if (g_land_geos.count(geo)) g_near_over_land_shown++;
		g_near_no_geo_shown = g_near_no_geo;
		g_near_geos.clear();
		g_land_geos.clear();
		g_near_no_geo = 0;

		// Periodic status to the log so the readout is available with the
		// overlay closed. Only while LOD terrain is actually being drawn, so an
		// interior cell doesn't fill the log with zeroes.
		static UINT s_tick = 0;
		if ((g_far_shown || g_near_shown) && (++s_tick % 300u) == 0u)
		{
			// nearOverLand: near-LOD draws whose NiGeometry is a land quad
			// drawn this frame (the land-blend pass over loaded land), out of
			// the distinct near-LOD geometries; noGeo = no dispatcher hook.
			shared::common::log("LOD", std::format(
				"far={} near={} (nearOverLand {}/{}{})",
				g_far_shown, g_near_shown, g_near_over_land_shown, g_near_geos_shown,
				g_near_no_geo_shown ? std::format(", noGeo {}", g_near_no_geo_shown) : std::string()),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}
	}
}
