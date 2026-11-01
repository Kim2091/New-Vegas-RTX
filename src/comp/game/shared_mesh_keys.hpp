#pragma once
#include <charconv>
#include <cstdint>
#include <string_view>
#include <vector>

// The table of static meshes named by geometry and diffuse, for a geometry
// vanilla draws with several textures, and by vertex colours too, for one it
// draws with the same texture and different colours. remix-mod-converter
// writes it. Each line is "<geometry key> <diffuse hash>" or "<geometry key>
// <diffuse hash> <colour hash>", all 16 hex digits. Include xxhash.h first.
namespace comp::game::api::asset_keys
{
	// remix-mod-converter looks for this text in the DLL, and writes colour
	// lines only for a wrapper that holds it.
	inline constexpr char kColourNamesMarker[] = "remix-mod-converter-colour-names-1";

	// The name of geometry `key` drawn with `diffuse` (rtxconf::d3d9_image_hash).
	// remix-mod-converter's hash::scoped_mesh_name is the same function.
	inline uint64_t scoped_name(uint64_t key, uint64_t diffuse)
	{
		const uint64_t name = XXH64(&diffuse, sizeof(diffuse), key);
		return name ? name : 1;
	}

	// The name of geometry `key` drawn with `diffuse` and these vertex colours.
	// remix-mod-converter's hash::colour_mesh_name is the same function.
	inline uint64_t colour_name(uint64_t key, uint64_t diffuse, uint64_t colour)
	{
		const uint64_t name = XXH64(&colour, sizeof(colour), scoped_name(key, diffuse));
		return name ? name : 1;
	}

	// The hash of one D3DCOLOR per vertex, for the vertices the indices use,
	// lowest index first, which is the order of the position hash. All black
	// and all white count as no colours, because a draw with none shows one or
	// the other. 0 only for no colours, and for an empty `colours`.
	// remix-mod-converter's hash::mesh_colours is the same function.
	inline uint64_t colour_hash(const std::vector<uint32_t>& colours, const std::vector<uint32_t>& indices)
	{
		if (colours.empty()) return 0;
		std::vector<uint8_t> used(colours.size(), 0);
		for (const uint32_t i : indices)
			if (i < colours.size()) used[i] = 1;
		std::vector<uint32_t> values;
		values.reserve(colours.size());
		for (size_t v = 0; v < colours.size(); v++)
			if (used[v]) values.push_back(colours[v]);
		if (values.empty()) return 0;
		bool none = values.front() == 0u || values.front() == 0xFFFFFFFFu;
		for (const uint32_t c : values)
			if (c != values.front()) none = false;
		if (none) return 0;
		const uint64_t hash = XXH3_64bits(values.data(), values.size() * sizeof(uint32_t));
		return hash ? hash : 1;
	}

	inline bool parse_colour_line(std::string_view line, uint64_t& key, uint64_t& diffuse, uint64_t& colour)
	{
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		if (line.size() != 50 || line[16] != ' ' || line[33] != ' ') return false;
		const char* s = line.data();
		const auto a = std::from_chars(s, s + 16, key, 16);
		const auto b = std::from_chars(s + 17, s + 33, diffuse, 16);
		const auto c = std::from_chars(s + 34, s + 50, colour, 16);
		return a.ec == std::errc() && a.ptr == s + 16 && b.ec == std::errc() && b.ptr == s + 33 && c.ec == std::errc() && c.ptr == s + 50;
	}

	inline bool parse_scoped_line(std::string_view line, uint64_t& key, uint64_t& diffuse)
	{
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		if (line.size() != 33 || line[16] != ' ') return false;
		const char* s = line.data();
		const auto a = std::from_chars(s, s + 16, key, 16);
		const auto b = std::from_chars(s + 17, s + 33, diffuse, 16);
		return a.ec == std::errc() && a.ptr == s + 16 && b.ec == std::errc() && b.ptr == s + 33;
	}
}
