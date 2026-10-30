#include "std_include.hpp"
#include "menu_scene.hpp"
#include "scene_graph.hpp"
#include "nvse_link.hpp"
#include "shared/fnv_exe.hpp"
#include "shared/common/ffp_state.hpp"
#include "game.hpp"
#include "ui_boundary.hpp"
#include "menu_bg.hpp"

// See menu_scene.hpp.

namespace comp::game::menu_scene
{
	namespace
	{
		namespace sg = scene_graph;

		// The menu entries of the ShadowSceneNode array (fnv_exe shadow_scene_node).
		constexpr uintptr_t kMenuObjectNode = 1 * sizeof(uint32_t);  // "Fallout Menu Object 3D Scene Graph"
		constexpr uintptr_t kMenuPlayerNode = 3 * sizeof(uint32_t);  // "Fallout Menu Player 3D Scene Graph"
		constexpr uint32_t kSceneGraphCamera = 0xAC;                   // SceneGraph -> NiCamera*
		// Deeper than any NIF hierarchy: a cycle or garbage ends the walk.
		constexpr int kMaxDepth = 64;
		constexpr UINT kLogWindow = 600;

		bool g_drawn = false;
		UINT g_raster_frame = 0xFFFFFFFFu;
		uint32_t g_raster_frames = 0, g_trigger_failures = 0;
		bool g_logged_raster = false;
		bool g_hooks_asked = false;
		uintptr_t g_last_geo = 0;      // the last geometry walked, and its verdict
		bool g_last_menu = false;
		UINT g_window = 0;
		uint32_t g_draws = 0, g_frames = 0, g_no_geometry = 0;
		bool g_logged_first = false;

		uintptr_t read_ptr(uintptr_t addr)
		{
			uint32_t v = 0;
			return addr && sg::read_u32(addr, v) ? static_cast<uintptr_t>(v) : 0;
		}

		// The camera of the scene being drawn is the world SceneGraph's.
		bool world_camera_drawing()
		{
			const auto& a = fnv_exe::addr();
			const uintptr_t cam = read_ptr(a.render_camera);
			const uintptr_t sg_world = read_ptr(a.world_scene_graph);
			return cam && sg_world && cam == read_ptr(sg_world + kSceneGraphCamera);
		}

		bool in_menu_mode()
		{
			return nvse_link::linked() && nvse_link::facts().game_state == FNVR_GS_MENU;
		}

		bool pipboy_closed()
		{
			return !nvse_link::linked() || nvse_link::facts().pipboy == FNVR_PB_CLOSED;
		}
	}

	bool geometry_in_menu_scene(uintptr_t geo)
	{
		if (!geo || !fnv_exe::known()) return false;
		if (geo == g_last_geo) return g_last_menu;
		const uintptr_t ssn = fnv_exe::addr().shadow_scene_node;
		const uintptr_t object_node = read_ptr(ssn + kMenuObjectNode);
		const uintptr_t player_node = read_ptr(ssn + kMenuPlayerNode);
		const uintptr_t hud_node = read_ptr(fnv_exe::addr().hud_3d_node);
		bool menu = false;
		if (object_node || player_node)
		{
			uintptr_t obj = geo;
			for (int depth = 0; obj && depth < kMaxDepth; ++depth)
			{
				// The HUD's element is not a menu scene (ui_boundary.hpp).
				if (hud_node && obj == hud_node) break;
				if (obj == object_node || obj == player_node) { menu = true; break; }
				uintptr_t parent = 0;
				uint32_t flags = 0;
				if (!sg::read_link(obj, parent, flags)) break;
				obj = parent;
			}
		}
		g_last_geo = geo;
		g_last_menu = menu;
		return menu;
	}

	bool draw_is_menu_scene()
	{
		if (!fnv_exe::known()) return false;
		// The dispatcher hook names the geometry (shared with the scene-graph
		// features; installed once, prologue-checked).
		if (!g_hooks_asked)
		{
			g_hooks_asked = true;
			sg::install_hooks();
		}
		if (!in_menu_mode() && world_camera_drawing()) return false;
		const uintptr_t geo = sg::current_geometry();
		if (!geo)
		{
			++g_no_geometry;
			return false;
		}
		if (!geometry_in_menu_scene(geo)) return false;
		if (!g_drawn) ++g_frames;
		g_drawn = true;
		++g_draws;
		if (!g_logged_first)
		{
			g_logged_first = true;
			const FnvRemixFacts& f = nvse_link::facts();
			shared::common::log("MenuScene", std::format("first menu 3D scene draw (geometry 0x{:X}, top menu {}) -- "
				"menu scene draws go to the screen with the game's shaders and are never the frame's camera", geo, nvse_link::linked() ? f.top_menu : 0), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		return true;
	}

	bool drawn_this_frame()
	{
		return g_drawn;
	}

	void before_draw(IDirect3DDevice9* dev, bool menu_draw)
	{
		const UINT now = shared::common::ffp_state::get().frame_count();
		if (g_raster_frame == now || !dev) return;
		if (!pipboy_closed()) return;
		if (!menu_draw && !menu_bg::frozen_menu()) return;
		// The runtime only injects at a draw to a primary-sized target.
		if (!game::rendering_to_backbuffer) return;
		if (!ui_boundary::trigger_injection(dev))
		{
			++g_trigger_failures;
			return;
		}
		g_raster_frame = now;
		++g_raster_frames;
		if (!g_logged_raster)
		{
			g_logged_raster = true;
			shared::common::log("MenuScene", std::format("first frame rasterised as the game draws it (top menu {}): RTX "
				"injection triggered before the frame's first draw to a screen-sized target, no Remix camera, nothing "
				"path traced", nvse_link::linked() ? nvse_link::facts().top_menu : 0),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
	}

	bool raster_frame()
	{
		return g_raster_frame == shared::common::ffp_state::get().frame_count();
	}

	void on_present()
	{
		g_drawn = false;

		// A geometry address can be reused by the next frame's objects.
		g_last_geo = 0;
		if (++g_window < kLogWindow) return;
		if (g_draws || g_no_geometry || g_raster_frames)
			shared::common::log("MenuScene", std::format("last {} frames: menu 3D scene draws {} in {} frames; frames "
				"rasterised with no path tracing {} (injection trigger failures {}); draws outside the world camera with "
				"no engine geometry to test {}{}", g_window, g_draws, g_frames, g_raster_frames, g_trigger_failures,
				g_no_geometry, sg::hooks_installed() ? "" : " (render-pass dispatcher hook not installed)"),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		g_window = 0;
		g_draws = g_frames = g_no_geometry = 0;
		g_raster_frames = g_trigger_failures = 0;
	}
}
