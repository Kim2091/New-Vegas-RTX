#include "common.hpp"
#include "rtti.hpp"

namespace fnvr::rtti
{
	namespace
	{
		std::unordered_map<uintptr_t, uint32_t> g_cache;
		std::unordered_map<uintptr_t, std::string> g_names;
		std::unordered_map<uint64_t, int32_t> g_bases;   // (vtable, name hash) -> base offset

		// The NiRTTI* a GetRTTI body returns, 0 when the code is not one:
		//   B8 imm32 C3                                  mov eax, imm; ret
		//   55 8B EC 51 89 4D FC B8 imm32 8B E5 5D C3    the same in a frame
		uintptr_t rtti_of_code(uintptr_t fn)
		{
			uint8_t c[16] = {};
			if (!guard::read(fn, c, sizeof(c))) return 0;
			uint32_t imm = 0;
			if (c[0] == 0xB8 && c[5] == 0xC3)
			{
				std::memcpy(&imm, c + 1, 4);
				return imm;
			}
			static constexpr uint8_t kFramed[] = { 0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC, 0xB8 };
			if (std::memcmp(c, kFramed, sizeof(kFramed)) == 0 && c[12] == 0x8B && c[13] == 0xE5 && c[14] == 0x5D &&
				c[15] == 0xC3)
			{
				std::memcpy(&imm, c + 8, 4);
				return imm;
			}
			return 0;
		}

		uint32_t bit_of(const char* name)
		{
			if (std::strcmp(name, "NiNode") == 0) return kNode;
			if (std::strcmp(name, "NiGeometry") == 0) return kGeometry;
			if (std::strcmp(name, "NiLight") == 0) return kLight;
			if (std::strcmp(name, "NiSourceTexture") == 0) return kSourceTexture;
			if (std::strcmp(name, "NiParticles") == 0) return kParticles;
			if (std::strcmp(name, "NiObjectNET") == 0) return kObjectNET;
			if (std::strcmp(name, "NiTimeController") == 0) return kController;
			return 0;
		}

		// The vtable's NiRTTI (0: not an NiObject's).
		uintptr_t rtti_of_vtbl(uintptr_t vtbl)
		{
			uint32_t fn = 0;
			if (!guard::get(vtbl + 8, fn) || !guard::in_image(fn)) return 0;
			return rtti_of_code(fn);
		}

		uint32_t resolve(uintptr_t vtbl)
		{
			uintptr_t rtti = rtti_of_vtbl(vtbl);
			uint32_t bits = 0;
			for (int depth = 0; rtti && depth < 32; depth++)
			{
				if (!guard::in_image(rtti) || (rtti & 3)) return depth ? bits | kKnown : 0;
				uint32_t name = 0, parent = 0;
				if (!guard::get(rtti, name) || !guard::get(rtti + 4, parent) || !guard::in_image(name))
					return depth ? bits | kKnown : 0;
				char n[64] = {};
				if (!guard::read(name, n, sizeof(n) - 1)) return depth ? bits | kKnown : 0;
				bits |= bit_of(n);
				rtti = parent;
			}
			return bits | kKnown;
		}
	}

	uint32_t of_vtbl(uintptr_t vtbl)
	{
		if (!guard::in_image(vtbl)) return 0;
		if (const auto it = g_cache.find(vtbl); it != g_cache.end()) return it->second;
		const uint32_t bits = resolve(vtbl);
		g_cache.emplace(vtbl, bits);
		return bits;
	}

	uint32_t of(uintptr_t obj)
	{
		uint32_t vtbl = 0;
		if (!guard::plausible(obj) || !guard::get(obj, vtbl)) return 0;
		return of_vtbl(vtbl);
	}

	const char* name(uintptr_t obj)
	{
		uint32_t vtbl = 0;
		if (!guard::plausible(obj) || !guard::get(obj, vtbl) || !guard::in_image(vtbl)) return "?";
		if (const auto it = g_names.find(vtbl); it != g_names.end()) return it->second.c_str();
		std::string n = "?";
		const uintptr_t rtti = rtti_of_vtbl(vtbl);
		uint32_t text = 0;
		char buf[64] = {};
		if (rtti && guard::in_image(rtti) && guard::get(rtti, text) && guard::in_image(text) &&
			guard::read(text, buf, sizeof(buf) - 1))
			n = buf;
		return g_names.emplace(vtbl, std::move(n)).first->second.c_str();
	}

	int32_t base_offset(uintptr_t obj, const char* type_name)
	{
		uint32_t vtbl = 0;
		if (!guard::plausible(obj) || !guard::get(obj, vtbl) || !guard::in_image(vtbl)) return -1;
		const uint64_t key = (static_cast<uint64_t>(vtbl) << 32) ^ std::hash<std::string_view>{}(type_name);
		if (const auto it = g_bases.find(key); it != g_bases.end()) return it->second;
		int32_t found = -1;
		// vtable[-1]: the CompleteObjectLocator {signature, offset, cdOffset,
		// TypeDescriptor*, ClassHierarchyDescriptor*}; its hierarchy
		// {signature, attributes, numBaseClasses, BaseClassDescriptor**};
		// each base {TypeDescriptor*, numContainedBases, mdisp, pdisp, vdisp,
		// attributes}; a TypeDescriptor's decorated name is at +8.
		uint32_t col = 0, col_offset = 1, chd = 0, count = 0, arr = 0;
		if (guard::get(vtbl - 4, col) && guard::in_image(col) && guard::get(col + 4, col_offset) && col_offset == 0 &&
			guard::get(col + 0x10, chd) && guard::in_image(chd) && guard::get(chd + 8, count) && count && count <= 64 &&
			guard::get(chd + 0xC, arr) && guard::in_image(arr))
		{
			const size_t want = std::strlen(type_name);
			for (uint32_t i = 0; i < count && found < 0; i++)
			{
				uint32_t bcd = 0, td = 0;
				int32_t mdisp = 0, pdisp = 0;
				char buf[64] = {};
				if (!guard::get(arr + i * 4, bcd) || !guard::in_image(bcd) || !guard::get(bcd, td) || !guard::in_image(td) ||
					!guard::get(bcd + 8, mdisp) || !guard::get(bcd + 0xC, pdisp) || !guard::read(td + 8, buf, sizeof(buf) - 1))
					break;
				if (pdisp == -1 && mdisp >= 0 && std::strlen(buf) == want && std::memcmp(buf, type_name, want) == 0) found = mdisp;
			}
		}
		g_bases.emplace(key, found);
		return found;
	}
}
