#include "common.hpp"
#include "camera.hpp"
#include "cells.hpp"
#include "config.hpp"
#include "queue.hpp"

namespace fnvr::camera
{
	namespace
	{
		bool g_load_msgs = false;          // PreLoadGame seen, PostLoadGame not yet
		bool g_present_loading = false;    // the last present was the loading screen's
		bool g_was_loading = false;
		bool g_in_world = false;           // a game is loaded (PostLoadGame ok / NewGame), not back at the main menu
		uint32_t g_pending = 0;            // FnvRemixCutReason waiting for the load to end
		uint32_t g_cut_serial = 0;
		uint32_t g_frames_since_cut = 0;
		uint32_t g_cut_frame_count = 0;    // main-loop frames since the cut, for the settle line
		bool g_settled = false;
		bool g_have_prev = false;          // the teleport rule's last camera position
		float g_prev[3] = {};
		ULONGLONG g_load_ms = 0;

		const char* reason_name(uint32_t r)
		{
			switch (r)
			{
			case FNVR_CUT_LOAD: return "load ended";
			case FNVR_CUT_NEW_GAME: return "new game";
			case FNVR_CUT_MAIN_MENU: return "main menu";
			case FNVR_CUT_WORLD_CHANGE: return "interior / worldspace change";
			case FNVR_CUT_TELEPORT: return "teleport";
			default: return "?";
			}
		}

		bool loading_menu()
		{
			uint32_t lm = 0;
			return guard::get(fnv_exe::addr().loading_menu, lm) && lm != 0;
		}

		void cut(uint32_t reason, float distance)
		{
			g_cut_serial++;
			g_frames_since_cut = 0;
			g_cut_frame_count = 0;
			g_settled = false;
			g_have_prev = false;
			queue::emit(FNVR_EV_CUT, reason, static_cast<uintptr_t>(distance));
			log::e("Cut", "cut #{}: {}{} ({} cells attached)", g_cut_serial, reason_name(reason),
				reason == FNVR_CUT_TELEPORT ? std::format(", the world camera moved {:.0f} units", distance) : std::string(),
				cells::count());
			cells::resync("cut");
		}
	}

	void request_cut(uint32_t reason, float distance)
	{
		if (loading())
		{
			if (!g_pending || reason == FNVR_CUT_NEW_GAME) g_pending = reason;
			return;
		}
		cut(reason, distance);
	}

	void on_pre_load()
	{
		g_load_msgs = true;
		update();
	}

	void on_post_load(bool success)
	{
		g_load_msgs = false;
		if (success) g_in_world = true;
		log::e("Cut", "PostLoadGame: {}", success ? "loaded" : "the load failed");
		update();
	}

	void on_present(bool loading_screen)
	{
		g_present_loading = loading_screen;
		update();
	}

	void on_new_game()
	{
		g_in_world = true;
		request_cut(FNVR_CUT_NEW_GAME, 0.0f);
	}

	void on_main_menu()
	{
		g_in_world = false;
		request_cut(FNVR_CUT_MAIN_MENU, 0.0f);
	}

	bool loading()
	{
		return g_load_msgs || g_present_loading || loading_menu();
	}

	void update()
	{
		const bool now = loading();
		if (now && !g_was_loading)
		{
			g_load_ms = GetTickCount64();
			queue::emit(FNVR_EV_LOAD_BEGIN);
			log::e("Cut", "load begins (load messages {}, loading-screen present {}, LoadingMenu {})", g_load_msgs ? 1 : 0,
				g_present_loading ? 1 : 0, loading_menu() ? 1 : 0);
		}
		else if (!now && g_was_loading)
		{
			queue::emit(FNVR_EV_LOAD_END);
			log::e("Cut", "load ends after {} ms", GetTickCount64() - g_load_ms);
			g_was_loading = false;
			// Every load moves the camera: one cut, whatever was asked meanwhile.
			const uint32_t reason = g_pending ? g_pending : FNVR_CUT_LOAD;
			g_pending = 0;
			cut(reason == FNVR_CUT_WORLD_CHANGE ? FNVR_CUT_LOAD : reason, 0.0f);
		}
		g_was_loading = now;
		if (!g_settled && !now && cells::all_refs_loaded())
		{
			g_settled = true;
			// A load into the same cells sends no events that would rebuild the owner map.
			cells::adopt_all("settled");
			log::e("Cut", "settled after cut #{}: {} main-loop frames, {} cells attached, all references loaded",
				g_cut_serial, g_cut_frame_count, cells::count());
		}
	}

	void tick()
	{
		g_frames_since_cut++;
		g_cut_frame_count++;
		update();
	}

	bool settled() { return g_settled; }
	bool in_world() { return g_in_world; }
	uint32_t cut_serial() { return g_cut_serial; }
	uint32_t frames_since_cut() { return g_frames_since_cut; }

	View read_world_camera(bool check_teleport)
	{
		View v;
		uint32_t sg = 0, cam = 0, vtbl = 0;
		if (!guard::get(fnv_exe::addr().world_scene_graph, sg) || !guard::plausible(sg)) return v;
		if (!guard::get(sg + engine::SG_camera, cam) || !guard::plausible(cam)) return v;
		if (!guard::get(cam, vtbl) || vtbl != fnv_exe::addr().vtbl_ni_camera) return v;
		if (!guard::read(cam + engine::AV_worldRot, v.rot, sizeof(v.rot)) ||
			!guard::read(cam + engine::AV_worldPos, v.pos, sizeof(v.pos)) || !guard::get(sg + engine::SG_fov, v.fov))
			return v;
		for (const float f : v.pos)
			if (!std::isfinite(f)) return v;
		v.ok = true;
		if (check_teleport && g_in_world && !loading())
		{
			if (g_have_prev)
			{
				const float dx = v.pos[0] - g_prev[0], dy = v.pos[1] - g_prev[1], dz = v.pos[2] - g_prev[2];
				const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (d > config::get().cut_distance) request_cut(FNVR_CUT_TELEPORT, d);
			}
			std::memcpy(g_prev, v.pos, sizeof(g_prev));
			g_have_prev = true;
		}
		return v;
	}

	int read_third_person()
	{
		uint32_t pc = 0, vtbl = 0;
		uint8_t tp = 0;
		if (!guard::get(fnv_exe::addr().player, pc) || !guard::plausible(pc) || !guard::get(pc, vtbl) ||
			vtbl != fnv_exe::addr().vtbl_player_character || !guard::get(pc + engine::PC_thirdPerson, tp))
			return -1;
		return tp ? 1 : 0;
	}
}
