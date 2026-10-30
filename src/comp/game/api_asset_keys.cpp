#include "std_include.hpp"
#include "api_mesh.hpp"

#pragma warning(push, 0)
#define XXH_INLINE_ALL
#include "xxhash.h"
#pragma warning(pop)

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
