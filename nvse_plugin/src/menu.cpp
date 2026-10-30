#include "common.hpp"
#include "menu.hpp"

namespace fnvr::menu
{
	namespace
	{
		constexpr const char* kStateNames[] = { "closed", "open requested", "opening", "open", "close requested", "closing" };
		constexpr uint32_t kFullLogs = 600;     // state changes logged in full; every 100th after

		uint32_t g_last_raw = 0xFFFFFFFFu;
		uint16_t g_last_top = 0;
		bool g_last_menu = false;
		bool g_have = false;
		uint32_t g_changes = 0;
		uint32_t g_top_changes = 0;
		bool g_logged_unreadable = false;
		bool g_logged_odd_top = false;

		const char* state_name(uint32_t raw)
		{
			return raw < std::size(kStateNames) ? kStateNames[raw] : "unknown";
		}

		// The last non-zero entry of the open-menu stack. Menu IDs are
		// 1001-1084; anything else is reported as no menu.
		uint16_t read_top_menu(uint32_t im)
		{
			uint32_t stack[engine::IM_menuStackLen] = {};
			if (!guard::read(im + engine::IM_menuStack, stack, sizeof(stack))) return 0;
			uint32_t top = 0;
			for (uint32_t id : stack)
			{
				if (!id) break;
				top = id;
			}
			if (top && (top < 1001 || top > 1084))
			{
				if (!g_logged_odd_top)
				{
					g_logged_odd_top = true;
					log::e("Menu", "open-menu stack top {} is outside 1001-1084: reported as no menu", top);
				}
				return 0;
			}
			return static_cast<uint16_t>(top);
		}
	}

	uint8_t pipboy_of(uint32_t raw)
	{
		switch (raw)
		{
		case 1: case 2: return FNVR_PB_OPENING;
		case 3: return FNVR_PB_OPEN;
		case 4: case 5: return FNVR_PB_CLOSING;
		default: return FNVR_PB_CLOSED;
		}
	}

	State poll()
	{
		State s;
		uint32_t im = 0, mode = 0, pip = 0;
		uint8_t init = 0;
		if (guard::get(fnv_exe::addr().interface_manager, im) && guard::plausible(im) &&
			guard::get(im + engine::IM_initialised, init) && init && guard::get(im + engine::IM_menuMode, mode) &&
			guard::get(im + engine::IM_pipBoyState, pip))
		{
			s.ok = true;
			s.menu_mode = mode != 1;
			s.pipboy_raw = pip;
			s.top_menu = read_top_menu(im);
		}
		if (!s.ok)
		{
			if (g_have && !g_logged_unreadable)
			{
				g_logged_unreadable = true;
				log::e("Menu", "InterfaceManager unreadable (it was readable before)");
			}
			return s;
		}
		g_logged_unreadable = false;
		if (!g_have)
			log::e("Menu", "InterfaceManager readable: Pip-Boy state {} ({}), menu mode {}", pip, state_name(pip),
				s.menu_mode ? 1 : 0);
		if (g_have && (pip != g_last_raw || s.menu_mode != g_last_menu))
		{
			g_changes++;
			if (g_changes <= kFullLogs || g_changes % 100 == 0)
				log::e("Menu", "Pip-Boy state {} ({}) -> {} ({}), menu mode {}{}", g_last_raw, state_name(g_last_raw), pip,
					state_name(pip), s.menu_mode ? 1 : 0, g_changes == kFullLogs ? " (further changes: every 100th)" : "");
			if (pip > 5)
				log::e("Menu", "Pip-Boy state {} is outside 0-5: reported as closed", pip);
		}
		if (g_have && s.top_menu != g_last_top)
		{
			g_top_changes++;
			if (g_top_changes <= kFullLogs || g_top_changes % 100 == 0)
				log::e("Menu", "top menu {} -> {}{}", g_last_top, s.top_menu,
					g_top_changes == kFullLogs ? " (further changes: every 100th)" : "");
		}
		g_last_top = s.top_menu;
		g_last_raw = pip;
		g_last_menu = s.menu_mode;
		g_have = true;
		return s;
	}
}
