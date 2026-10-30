#include "common.hpp"
#include "status.hpp"
#include "camera.hpp"
#include "cells.hpp"
#include "owners.hpp"
#include "pins.hpp"

namespace fnvr::status
{
	namespace
	{
		constexpr uint64_t kStatusMs = 60000;

		uint32_t g_serial = 0xFFFFFFFF;
		uintptr_t g_place = 0;
		std::string g_place_text = "none";
		uint64_t g_last_status_ms = 0;
		uint64_t g_held_back = 0;

		// "'Name' (0x0011ABCD)" for a cell or worldspace: the form ID and the
		// TESFullName text, when it reads back as printable.
		std::string describe(uintptr_t form)
		{
			uint32_t id = 0;
			guard::get(form + engine::FORM_refID, id);
			uint32_t text = 0;
			uint16_t len = 0;
			char name[64] = {};
			if (guard::get(form + engine::FULLNAME_text, text) && text >= 0x10000 && text <= guard::user_address_limit() &&
				guard::get(form + engine::FULLNAME_text + 4, len) && len > 0 && len < sizeof(name) &&
				guard::read(text, name, len))
			{
				bool printable = true;
				for (uint16_t i = 0; i < len; i++) printable &= name[i] >= 0x20 && name[i] < 0x7F;
				if (printable) return std::format("'{}' (0x{:08X})", std::string_view(name, len), id);
			}
			return std::format("0x{:08X}", id);
		}

		// The interior cell, else the worldspace of an attached exterior
		// cell, else 0.
		uintptr_t place(bool& interior)
		{
			interior = false;
			uint32_t tes = 0, cell = 0;
			if (guard::get(fnv_exe::addr().tes, tes) && guard::plausible(tes) &&
				guard::get(tes + engine::TES_interior, cell) && guard::plausible(cell))
			{
				interior = true;
				return cell;
			}
			FnvRemixCell cells[64];
			const uint32_t n = cells::copy(cells, 64);
			for (uint32_t i = 0; i < n; i++)
			{
				uint32_t world = 0;
				if (!cells[i].interior && guard::get(cells[i].cell + engine::CELL_worldSpace, world) &&
					guard::plausible(world))
					return world;
			}
			return 0;
		}
	}

	bool place(FnvRemixPlace& out)
	{
		out = {};
		uint32_t pc = 0, vtbl = 0, cell = 0;
		if (!guard::get(fnv_exe::addr().player, pc) || !guard::plausible(pc) || !guard::get(pc, vtbl) ||
			vtbl != fnv_exe::addr().vtbl_player_character || !guard::get(pc + engine::REFR_parentCell, cell) ||
			!guard::plausible(cell))
			return false;
		float p[3] = {};
		if (guard::read(pc + engine::REFR_pos, p, sizeof(p)) && std::isfinite(p[0]) && std::isfinite(p[1]) &&
			std::isfinite(p[2]))
			std::memcpy(out.pos, p, sizeof(p));
		strncpy_s(out.cell, describe(cell).c_str(), _TRUNCATE);
		uint32_t world = 0;
		if (!guard::get(cell + engine::CELL_worldSpace, world) || !guard::plausible(world))
		{
			out.interior = 1;
			return true;
		}
		strncpy_s(out.worldspace, describe(world).c_str(), _TRUNCATE);
		uint32_t coords = 0;
		int32_t xy[2] = {};
		if (guard::get(cell + engine::CELL_coords, coords) && guard::plausible(coords) && guard::read(coords, xy, sizeof(xy)))
		{
			out.cell_x = xy[0];
			out.cell_y = xy[1];
		}
		return true;
	}

	void tick()
	{
		const uint32_t serial = cells::serial();
		if (serial != g_serial)
		{
			g_serial = serial;
			bool interior = false;
			const uintptr_t p = place(interior);
			if (p != g_place)
			{
				g_place = p;
				g_place_text = p ? std::format("{} {}", interior ? "interior" : "worldspace", describe(p)) : "none";
				log::e("Location", "{}", g_place_text);
			}
		}

		const uint64_t now = GetTickCount64();
		if (!g_last_status_ms)
		{
			g_last_status_ms = now;
			return;
		}
		if (now - g_last_status_ms < kStatusMs) return;
		g_last_status_ms = now;

		std::string grid;
		int32_t cx = 0, cy = 0;
		uint32_t dim = 0;
		if (!cells::interior() && cells::grid(cx, cy, dim) && dim)
			grid = std::format(", grid ({}, {})", cx, cy);
		const uint64_t held = log::held_back();
		log::e("Status", "{}{} | cells {} attached{}, {} | cut #{} | pins {}, owners {} ({} geometries) | read faults {} | "
			"log lines held back +{}", g_place_text, grid, cells::count(),
			cells::all_refs_loaded() ? "" : " (references still loading)", camera::loading() ? "loading" : "not loading",
			camera::cut_serial(), pins::live(), owners::count(), owners::geometry_count(), guard::faults(),
			held - g_held_back);
		g_held_back = held;
	}
}
