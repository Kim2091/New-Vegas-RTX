#include "std_include.hpp"
#include "api_rtxconf.hpp"
#include "api_identity.hpp"
#include "api_textures.hpp"

#include <cctype>
#include <deque>

#pragma warning(push, 0)
#define XXH_INLINE_ALL
#include "xxhash.h"
#pragma warning(pop)

// Runtime options the API paths mirror. See api_rtxconf.hpp.

namespace comp::game::api::rtxconf
{
	namespace
	{
		bool g_loaded = false;
		Scalars g_scalars;

		// D3D9 image hash -> what the lists naming it say.
		std::unordered_map<uint64_t, Listing> g_listed;
		std::unordered_set<uint64_t> g_ignore_baked;
		// Hashes a D3D9-only list names; rtx.raytracedRenderTargetTextures has
		// an entry.
		size_t g_d3d9_only_count = 0;
		bool g_rt_list = false;

		std::string trim(const std::string& s)
		{
			size_t a = 0, b = s.size();
			while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
			while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
			return s.substr(a, b - a);
		}

		bool parse_bool(const std::string& v, bool fallback)
		{
			if (_stricmp(v.c_str(), "true") == 0 || v == "1") return true;
			if (_stricmp(v.c_str(), "false") == 0 || v == "0") return false;
			return fallback;
		}

		float parse_float(const std::string& v, float fallback)
		{
			try { return std::stof(v); }
			catch (...) { return fallback; }
		}

		// Keyed by a render target's descriptor hash, not an image hash (see
		// d3d9_only()); compared by pointer.
		constexpr char kRtListKey[] = "rtx.raytracedRenderTargetTextures";

		// The runtime's texture-hash lists (rtx_options.h fast_unordered_set
		// options) that tag a D3D9 draw by its albedo (with an instance
		// category, or for lightConverter by turning it into a light), and the
		// API instance category each becomes. 0: the runtime acts on it only
		// for D3D9 draws; such a draw keeps its D3D9 route (d3d9_only()). An
		// explicit table, not the pattern "rtx.*Textures", which would also
		// match logging lists such as rtx.fnvProtocol.logMaterialTextures.
		struct ListKind
		{
			const char* key;
			uint32_t category;
			bool drop_alpha;
		};
		constexpr ListKind kLists[] = {
			{ "rtx.ignoreTextures",                   REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE, false },
			{ "rtx.hideInstanceTextures",             REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN, false },
			{ "rtx.skyBoxTextures",                   REMIXAPI_INSTANCE_CATEGORY_BIT_SKY, false },
			{ "rtx.particleTextures",                 REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE, false },
			{ "rtx.particleEmitterTextures",          REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER, false },
			{ "rtx.beamTextures",                     REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM, false },
			{ "rtx.decalTextures",                    REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC, false },
			{ "rtx.dynamicDecalTextures",             REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC, false },
			{ "rtx.singleOffsetDecalTextures",        REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET, false },
			{ "rtx.nonOffsetDecalTextures",           REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET, false },
			{ "rtx.terrainTextures",                  REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN, false },
			{ "rtx.animatedWaterTextures",            REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER, false },
			{ "rtx.worldSpaceUiTextures",             REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI, false },
			{ "rtx.worldSpaceUiBackgroundTextures",   REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE, false },
			{ "rtx.playerModelTextures",              REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL, false },
			{ "rtx.playerModelBodyTextures",          REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY, false },
			{ "rtx.hairCardTextures",                 REMIXAPI_INSTANCE_CATEGORY_BIT_HAIR_CARDS, false },
			{ "rtx.smoothNormalsTextures",            REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS, false },
			{ "rtx.ignoreLights",                     REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS, false },
			{ "rtx.antiCulling.antiCullingTextures",  REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING, false },
			{ "rtx.postfx.motionBlurMaskOutTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR, false },
			{ "rtx.opacityMicromapIgnoreTextures",    REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP, false },
			{ "rtx.ignoreAlphaOnTextures",            REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL, true },
			{ "rtx.uiTextures",                       0u, false },
			{ "rtx.lightConverter",                   0u, false },
			{ "rtx.lightmapTextures",                 0u, false },
			{ kRtListKey,                              0u, false },
		};

		const ListKind* list_kind(const std::string& key)
		{
			for (const ListKind& k : kLists)
				if (key == k.key) return &k;
			return nullptr;
		}

		// The runtime resolves each list on its own: its positive entries minus
		// its own negative ("-0x...") entries. A negative entry in one list
		// leaves the hash in every other list.
		struct Accum
		{
			std::unordered_map<uint64_t, std::vector<const ListKind*>> positive;
			std::unordered_map<uint64_t, std::vector<const ListKind*>> negative;
			std::unordered_set<uint64_t> baked_positive, baked_negative;
		};

		void parse_list(const std::string& value, const ListKind* kind, bool baked, Accum& acc)
		{
			size_t i = 0;
			while (i < value.size())
			{
				size_t j = value.find(',', i);
				if (j == std::string::npos) j = value.size();
				const std::string tok = trim(value.substr(i, j - i));
				i = j + 1;
				if (tok.empty()) continue;
				const bool neg = tok[0] == '-';
				uint64_t h = 0;
				try { h = std::stoull(neg ? tok.substr(1) : tok, nullptr, 16); }
				catch (...) { continue; }
				if (baked)
				{
					if (neg) acc.baked_negative.insert(h);
					else acc.baked_positive.insert(h);
				}
				else (neg ? acc.negative : acc.positive)[h].push_back(kind);
			}
		}

		void parse_file(const std::filesystem::path& path, Accum& acc, int& lists)
		{
			std::ifstream f(path);
			if (!f) return;
			std::string line;
			while (std::getline(f, line))
			{
				const size_t eq = line.find('=');
				if (eq == std::string::npos) continue;
				const std::string key = trim(line.substr(0, eq));
				if (key.empty() || key[0] == '#') continue;
				const std::string value = trim(line.substr(eq + 1));

				auto& s = g_scalars;
				if      (key == "rtx.fnvProtocol.enable")                 s.protocol_enabled = parse_bool(value, s.protocol_enabled);
				else if (key == "rtx.fnvProtocol.glowEmissiveIntensity")  s.glow_intensity = parse_float(value, s.glow_intensity);
				else if (key == "rtx.fnvProtocol.normalAlphaMode")        s.normal_alpha_mode = static_cast<int>(parse_float(value, static_cast<float>(s.normal_alpha_mode)));
				else if (key == "rtx.fnvProtocol.heightDisplaceIn")       s.height_displace_in = parse_float(value, s.height_displace_in);
				else if (key == "rtx.legacyMaterial.roughnessConstant")   s.legacy_roughness = parse_float(value, s.legacy_roughness);
				else if (key == "rtx.vertexColorIsBakedLighting")         s.vertex_color_baked = parse_bool(value, s.vertex_color_baked);
				else if (key == "rtx.ignoreAllVertexColorBakedLighting")  s.ignore_all_vertex_color = parse_bool(value, s.ignore_all_vertex_color);
				else if (key == "rtx.ignoreBakedLightingTextures")        parse_list(value, nullptr, true, acc);
				else if (const ListKind* kind = list_kind(key))
				{
					parse_list(value, kind, false, acc);
					lists++;
				}
			}
			shared::common::log("RemixApi", std::format("statics: read runtime options from {}", path.string()),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		void load()
		{
			if (g_loaded) return;
			g_loaded = true;

			char exe[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			const std::filesystem::path dir = std::filesystem::path(exe).parent_path();

			// Lowest priority first; later files override scalars.
			Accum acc;
			int lists = 0;
			for (const char* name : { "dxvk.conf", "rtx.conf", "user.conf" })
				parse_file(dir / name, acc, lists);

			// A texture in several lists gets every list's category, as the
			// runtime ORs them into the draw's categories.
			for (const auto& [h, kinds] : acc.positive)
			{
				const auto neg = acc.negative.find(h);
				auto negated = [&](const ListKind* k) {
					return neg != acc.negative.end() && std::find(neg->second.begin(), neg->second.end(), k) != neg->second.end();
				};
				if (std::all_of(kinds.begin(), kinds.end(), negated)) continue;
				Listing& l = g_listed[h];
				for (const ListKind* k : kinds)
				{
					if (negated(k)) continue;
					if (!l.first) l.first = k->key;
					if (!k->category && !l.d3d9_only) l.d3d9_only = k->key;
					if (k->key == kRtListKey) g_rt_list = true;
					l.categories |= k->category;
					l.drop_alpha = l.drop_alpha || k->drop_alpha;
				}
				if (l.d3d9_only) g_d3d9_only_count++;
			}
			for (const uint64_t h : acc.baked_positive)
				if (!acc.baked_negative.contains(h)) g_ignore_baked.insert(h);

			const auto& s = g_scalars;
			shared::common::log("RemixApi", std::format(
				"statics: runtime options -- {} tagged textures in {} lists ({} in D3D9-only lists{}), {} ignoreBakedLighting; fnvProtocol "
				"enable={} glow={:.2f} normalAlphaMode={} heightDisplaceIn={:.3f}; legacy roughness={:.2f}; "
				"vertex colour baked={} ignoreAll={}",
				g_listed.size(), lists, g_d3d9_only_count,
				g_rt_list ? ", raytracedRenderTargetTextures: every render-target albedo" : "", g_ignore_baked.size(), s.protocol_enabled ? 1 : 0, s.glow_intensity,
				s.normal_alpha_mode, s.height_displace_in, s.legacy_roughness,
				s.vertex_color_baked ? 1 : 0, s.ignore_all_vertex_color ? 1 : 0),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
	}

	const Scalars& scalars()
	{
		load();
		return g_scalars;
	}

	const Listing* lookup(uint64_t d3d9_hash)
	{
		load();
		const auto it = g_listed.find(d3d9_hash);
		return it != g_listed.end() ? &it->second : nullptr;
	}

	const char* listed(uint64_t d3d9_hash)
	{
		const Listing* l = lookup(d3d9_hash);
		return l ? l->first : nullptr;
	}

	const char* hiding_list(uint64_t d3d9_hash)
	{
		const Listing* l = lookup(d3d9_hash);
		if (!l) return nullptr;
		if (l->categories & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE) return "rtx.ignoreTextures";
		if (l->categories & REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN) return "rtx.hideInstanceTextures";
		return nullptr;
	}

	bool ignores_baked_lighting(uint64_t d3d9_hash)
	{
		load();
		return g_ignore_baked.contains(d3d9_hash);
	}

	bool has_lists()
	{
		load();
		return !g_listed.empty();
	}

	bool d3d9_image_hash(IDirect3DBaseTexture9* base, uint64_t& out)
	{
		if (!base || base->GetType() != D3DRTYPE_TEXTURE) return false;
		auto* t = static_cast<IDirect3DTexture9*>(base);
		D3DSURFACE_DESC d = {};
		if (FAILED(t->GetLevelDesc(0, &d))) return false;
		// A DEFAULT texture has no CPU copy to lock (api_textures.hpp).
		if (d.Pool == D3DPOOL_DEFAULT) return textures::default_d3d9_hash(t, out);

		// Mip 0 as DXVK stores it: block rows of whole blocks, or 4-byte
		// texels (rows already 4-aligned), packed without pitch padding.
		uint32_t rows = 0, row_bytes = 0;
		switch (d.Format)
		{
		case D3DFMT_DXT1:
			rows = (std::max)(1u, (d.Height + 3) / 4);
			row_bytes = (std::max)(1u, (d.Width + 3) / 4) * 8;
			break;
		case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
			rows = (std::max)(1u, (d.Height + 3) / 4);
			row_bytes = (std::max)(1u, (d.Width + 3) / 4) * 16;
			break;
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
			rows = d.Height;
			row_bytes = d.Width * 4;
			break;
		default:
			return false;
		}

		// Hashed in place: a packed copy of a 4K level is a 16 MiB block, which
		// a nearly full address space may not have. The bridge's CPU copy has
		// no pitch padding; any other layout is hashed row by row.
		D3DLOCKED_RECT lr = {};
		if (FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return false;
		if (lr.Pitch < static_cast<INT>(row_bytes)) { t->UnlockRect(0); return false; }
		bool ok = true;
		if (lr.Pitch == static_cast<INT>(row_bytes))
		{
			out = XXH3_64bits(lr.pBits, static_cast<size_t>(rows) * row_bytes);
		}
		else if (XXH3_state_t* st = XXH3_createState())
		{
			const auto* src = static_cast<const uint8_t*>(lr.pBits);
			XXH3_64bits_reset(st);
			for (uint32_t r = 0; r < rows; r++) XXH3_64bits_update(st, src + static_cast<size_t>(r) * lr.Pitch, row_bytes);
			out = XXH3_64bits_digest(st);
			XXH3_freeState(st);
		}
		else ok = false;
		t->UnlockRect(0);
		return ok;
	}

	namespace
	{
		// d3d9_only() per texture address: what its image is, as last checked.
		struct OnlyEntry
		{
			uint32_t gen = 0;
			UINT width = 0, height = 0;
			D3DFORMAT format = D3DFMT_UNKNOWN;
			bool render_target = false;
			const char* list = nullptr;
		};
		std::unordered_map<const void*, OnlyEntry> g_only;
		constexpr size_t kOnlyCacheMax = 1u << 15;
		std::unordered_set<uint64_t> g_only_logged;
		constexpr size_t kOnlyLogMax = 64;
	}

	const char* d3d9_only(IDirect3DBaseTexture9* tex)
	{
		load();
		if (!tex || (!g_d3d9_only_count && !g_rt_list)) return nullptr;
		if (tex->GetType() != D3DRTYPE_TEXTURE) return nullptr;

		// The proxy's creation generation tells a recycled address; the size
		// and format catch one created outside its sight (generation 0).
		const uint32_t gen = identity::texture_gen(tex);
		const auto it = g_only.find(tex);
		if (it != g_only.end() && gen && it->second.gen == gen)
			return it->second.render_target ? (g_rt_list ? kRtListKey : nullptr) : it->second.list;

		D3DSURFACE_DESC d = {};
		if (FAILED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d))) return nullptr;
		if (it != g_only.end() && it->second.gen == gen && it->second.width == d.Width &&
			it->second.height == d.Height && it->second.format == d.Format)
			return it->second.render_target ? (g_rt_list ? kRtListKey : nullptr) : it->second.list;

		if (g_only.size() >= kOnlyCacheMax) g_only.clear();
		OnlyEntry& e = g_only[tex];
		e = OnlyEntry{ gen, d.Width, d.Height, d.Format, (d.Usage & D3DUSAGE_RENDERTARGET) != 0, nullptr };
		uint64_t h = 0;
		if (!e.render_target && g_d3d9_only_count && d3d9_image_hash(tex, h))
			if (const Listing* l = lookup(h)) e.list = l->d3d9_only;
		const char* list = e.render_target ? (g_rt_list ? kRtListKey : nullptr) : e.list;
		if (list && g_only_logged.size() < kOnlyLogMax && g_only_logged.insert(e.render_target ? 0ull : h).second)
			shared::common::log("RemixApi", std::format(
				"d3d9-only: albedo {} ({}x{} fmt {}) is in {} -- its draws keep the D3D9 route, where the runtime acts on "
				"that list", e.render_target ? std::string("(render target)") : std::format("0x{:016X}", h), d.Width,
				d.Height, static_cast<int>(d.Format), list), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		return list;
	}
}
