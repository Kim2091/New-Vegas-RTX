#include "std_include.hpp"
#include "api_mesh.hpp"
#include "session_files.hpp"

#pragma warning(push, 0)
#define XXH_INLINE_ALL
#include "xxhash.h"
#pragma warning(pop)

#include "shared_mesh_keys.hpp"

// Mesh asset keys (remixapi_AssetKeyEXT). See api_mesh.hpp, asset_keys.

namespace comp::game::api::asset_keys
{
	namespace
	{
		// The runtime's rtx_hashing.cpp hashGeometryDescriptor, for a uint32 triangle list
		// (VK_INDEX_TYPE_UINT32 = 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3).
		uint64_t descriptor_hash(uint32_t index_count, uint32_t vertex_count)
		{
			const uint32_t topology = 3;
			const uint32_t index_type = 1;
			uint64_t h = XXH3_64bits_withSeed(&index_count, sizeof(index_count), 0);
			h = XXH3_64bits_withSeed(&vertex_count, sizeof(vertex_count), h);
			h = XXH3_64bits_withSeed(&topology, sizeof(topology), h);
			return XXH3_64bits_withSeed(&index_type, sizeof(index_type), h);
		}

		int g_forwarded = -1;

		// rtx-remix\mods\ConvertedMods\shared_mesh_keys.txt: the names by geometry
		// and diffuse, the pairs named by colours too, and those names.
		struct Table
		{
			std::unordered_set<uint64_t> names;
			std::unordered_set<uint64_t> colour_pairs;
			std::unordered_set<uint64_t> colour_names;
			std::unordered_set<uint64_t> logged;
			uint32_t hits = 0, misses = 0;
		};

		Table load_table()
		{
			Table t;
			std::string text;
			if (!session_files::read_file(session_files::game_dir() + L"rtx-remix\\mods\\ConvertedMods\\shared_mesh_keys.txt", text,
				16u << 20))
			{
				shared::common::log("RemixApi", "meshes: no shared_mesh_keys.txt -- every static mesh is named by its geometry",
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				return t;
			}
			size_t bad = 0;
			for (size_t at = 0; at < text.size();)
			{
				const size_t end = (std::min)(text.find('\n', at), text.size());
				const std::string_view line = std::string_view(text).substr(at, end - at);
				uint64_t key = 0, diffuse = 0, colour = 0;
				if (parse_scoped_line(line, key, diffuse)) t.names.insert(scoped_name(key, diffuse));
				else if (parse_colour_line(line, key, diffuse, colour))
				{
					t.colour_pairs.insert(scoped_name(key, diffuse));
					t.colour_names.insert(colour_name(key, diffuse, colour));
				}
				else bad++;
				at = end + 1;
			}
			shared::common::log("RemixApi", std::format("meshes: shared_mesh_keys.txt names {} static meshes by geometry and "
				"diffuse, and {} draws of {} pairs by vertex colours too ({} bad lines, {})", t.names.size(), t.colour_names.size(),
				t.colour_pairs.size(), bad, kColourNamesMarker), bad ? shared::common::LOG_TYPE::LOG_TYPE_WARN
				: shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			return t;
		}

		Table& table()
		{
			static Table s_table = load_table();
			return s_table;
		}
	}

	uint64_t of(const float* positions, size_t stride, size_t vertex_count, const uint32_t* indices, size_t index_count)
	{
		if (!positions || !indices || vertex_count == 0 || index_count == 0) return 0;

		// Positions: each vertex the indices use, lowest index first, 12 bytes
		// each (hashVertexRegionIndexed over the sorted unique indices).
		static std::vector<uint8_t> s_used;
		s_used.assign(vertex_count, 0);
		for (size_t i = 0; i < index_count; i++)
			if (indices[i] < vertex_count) s_used[indices[i]] = 1;
		uint64_t pos = 0;
		for (size_t v = 0; v < vertex_count; v++)
			if (s_used[v]) pos = XXH3_64bits_withSeed(positions + v * stride, 3 * sizeof(float), pos);

		const uint64_t idx = XXH3_64bits(indices, index_count * sizeof(uint32_t));
		const uint64_t desc = descriptor_hash(static_cast<uint32_t>(index_count), static_cast<uint32_t>(vertex_count));

		// GeometryHashes::getHashForRuleImpl: the first component as is, the
		// next ones seeded with the result, in component order.
		uint64_t key = pos;
		key = XXH64(&idx, sizeof(idx), key);
		key = XXH64(&desc, sizeof(desc), key);
		return key ? key : 1;
	}

	uint64_t of(const std::vector<remixapi_HardcodedVertex>& verts, const std::vector<uint32_t>& indices)
	{
		if (verts.empty()) return 0;
		return of(verts.front().position, sizeof(remixapi_HardcodedVertex) / sizeof(float), verts.size(), indices.data(),
			indices.size());
	}

	uint64_t for_static(uint64_t key, uint64_t diffuse)
	{
		const Table& t = table();
		if (!key || !diffuse || t.names.empty()) return key;
		const uint64_t name = scoped_name(key, diffuse);
		return t.names.contains(name) ? name : key;
	}

	bool wants_colours(uint64_t key, uint64_t diffuse)
	{
		const Table& t = table();
		return key && diffuse && !t.colour_pairs.empty() && t.colour_pairs.contains(scoped_name(key, diffuse));
	}

	uint64_t for_colours(uint64_t key, uint64_t diffuse, const std::vector<uint32_t>& colours, const std::vector<uint32_t>& indices)
	{
		static constexpr size_t kMaxLogged = 2000;
		Table& t = table();
		const uint64_t colour = colour_hash(colours, indices);
		const uint64_t name = colour_name(key, diffuse, colour);
		const bool hit = t.colour_names.contains(name);
		(hit ? t.hits : t.misses)++;
		if (t.logged.size() < kMaxLogged && t.logged.insert(name).second)
			shared::common::log("RemixApi", std::format("meshes: colour name {}: geometry {:016X} diffuse {:016X} colours {:016X} "
				"({} matched, {} missed so far)", hit ? "matched" : "MISSED", key, diffuse, colour, t.hits, t.misses),
				hit ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		// A miss takes the pair's name, which no replacement has, so the draw stays as it is.
		return hit ? name : scoped_name(key, diffuse);
	}

	void read_colours(const uint8_t* vertices, size_t stride, size_t count, const Attr& col, std::vector<uint32_t>& out)
	{
		out.clear();
		if (!col.ok() || !vertices || !attr_fits(col, static_cast<UINT>(stride))) return;
		const auto unorm = [](float v) -> uint32_t { return !(v > 0.0f) ? 0u : v >= 1.0f ? 255u : static_cast<uint32_t>(v * 255.0f + 0.5f); };
		out.resize(count);
		for (size_t i = 0; i < count; i++)
		{
			const uint8_t* p = vertices + i * stride + col.off;
			if (col.type == D3DDECLTYPE_D3DCOLOR) std::memcpy(&out[i], p, sizeof(uint32_t));
			else out[i] = unorm(read_comp(p, col.type, 3)) << 24 | unorm(read_comp(p, col.type, 0)) << 16 |
				unorm(read_comp(p, col.type, 1)) << 8 | unorm(read_comp(p, col.type, 2));
		}
	}

	bool forwarded()
	{
		if (g_forwarded < 0)
		{
			static constexpr char kMarker[] = "Forwarding AssetKeyEXT";
			const bool ok = bridge_client_has(kMarker);
			g_forwarded = ok ? 1 : 0;
			shared::common::log("RemixApi", ok
				? "meshes: the bridge client forwards AssetKeyEXT -- mods and captures name API meshes by their geometry"
				: "meshes: the bridge client does NOT forward AssetKeyEXT (older bridge) -- API meshes have no stable "
				  "asset key, so mesh replacements and captures cannot name them",
				ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		}
		return g_forwarded == 1;
	}

	void chain(remixapi_MeshInfo& mi, remixapi_AssetKeyEXT& ext, uint64_t key)
	{
		if (!key || !forwarded()) return;
		ext = {};
		ext.sType = REMIXAPI_STRUCT_TYPE_ASSET_KEY_EXT;
		ext.pNext = mi.pNext;
		ext.key = key;
		mi.pNext = &ext;
	}
}
