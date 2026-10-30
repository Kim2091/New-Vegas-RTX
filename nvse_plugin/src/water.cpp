#include "common.hpp"
#include "water.hpp"

#include <cfloat>

namespace fnvr::water
{
	namespace
	{
		constexpr uint32_t CELL_flags = 0x24;                  // u8: 1 interior, 2 has water
		constexpr uint32_t CELL_waterHeight = 0x50;            // float, FLT_MAX: the worldspace's
		constexpr uint32_t WS_waterHeight = 0xCC;              // float, FLT_MAX: none

		bool usable(float h)
		{
			return std::isfinite(h) && std::fabs(h) < 1.0e7f;
		}

		bool cell_height(float& out)
		{
			uint32_t pc = 0, cell = 0;
			if (!guard::get(fnv_exe::addr().player, pc) || !guard::plausible(pc)) return false;
			uint32_t vtbl = 0;
			if (!guard::get(pc, vtbl) || vtbl != fnv_exe::addr().vtbl_player_character) return false;
			if (!guard::get(pc + engine::REFR_parentCell, cell) || !guard::plausible(cell)) return false;
			uint8_t type = 0, flags = 0;
			if (!guard::get(cell + engine::FORM_type, type) || type != engine::kFormType_CELL) return false;
			if (!guard::get(cell + CELL_flags, flags) || (flags & 1) || !(flags & 2)) return false;
			float h = 0.0f;
			if (!guard::get(cell + CELL_waterHeight, h)) return false;
			if (h != FLT_MAX)
			{
				out = h;
				return usable(h);
			}
			uint32_t ws = 0;
			if (!guard::get(cell + engine::CELL_worldSpace, ws) || !guard::plausible(ws)) return false;
			if (!guard::get(ws + WS_waterHeight, h)) return false;
			out = h != FLT_MAX ? h : 0.0f;
			return usable(out);
		}
	}

	State read()
	{
		State s;
		const auto& addr = fnv_exe::addr();
		uint8_t under = 0, placeable = 0, system_on = 0, setting = 0;
		if (!guard::get(addr.water_underwater, under) || !guard::get(addr.water_in_placeable, placeable) ||
			!guard::get(addr.water_system_on, system_on) || !guard::get(addr.water_setting, setting))
			return s;
		s.ok = true;
		const bool live = system_on != 0 && setting != 0;
		s.underwater = live && under != 0;
		float h = 0.0f;
		if (placeable && guard::get(addr.water_placeable_height, h) && usable(h)) s.height_valid = true;
		else s.height_valid = cell_height(h);
		s.height = s.height_valid ? h : 0.0f;
		if (!live) s.height_valid = false;
		return s;
	}
}
