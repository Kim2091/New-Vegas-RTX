#include "common.hpp"
#include "monitor.hpp"
#include "camera.hpp"
#include "cells.hpp"
#include "menu.hpp"
#include "integrity.hpp"
#include "queue.hpp"
#include "owners.hpp"
#include "lights.hpp"
#include "pins.hpp"
#include "water.hpp"
#include "status.hpp"
#include "mods.hpp"

// FnvRemix_GetApi: the table the wrapper calls. Every entry but log_line
// runs on the main thread, and those that read engine state first apply the
// queued engine messages.

namespace fnvr::api
{
	namespace
	{
		// A call from another thread (the loading screen renders on its own)
		// gets nothing; logged the first time and every 1000th after.
		bool main_thread_or_log(const char* what)
		{
			if (monitor::on_main_thread()) return true;
			static uint32_t s_count = 0;
			if (s_count++ % 1000 == 0)
				log::f("Api", "{} asked from thread {}, not the main thread: answered not ready ({} such calls so far)", what,
					GetCurrentThreadId(), s_count);
			return false;
		}

		void player_pos(float (&out)[3])
		{
			uint32_t pc = 0, vtbl = 0;
			float p[3] = {};
			if (guard::get(fnv_exe::addr().player, pc) && guard::plausible(pc) && guard::get(pc, vtbl) &&
				vtbl == fnv_exe::addr().vtbl_player_character && guard::read(pc + engine::REFR_pos, p, sizeof(p)) &&
				std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
				std::memcpy(out, p, sizeof(p));
		}

		// The integrity monitor's bits plus the live reads of this call.
		constexpr uint32_t kSiteBits = FNVR_F_CELLS | FNVR_F_LOADING | FNVR_F_DECALS | FNVR_F_MENU_BG | FNVR_F_HOOK_P1 | FNVR_F_HOOK_P2 |
			FNVR_F_HOOK_P3 | FNVR_F_HOOK_P5;

		void facts(FnvRemixFacts* out)
		{
			if (!out) return;
			*out = {};
			if (!main_thread_or_log("facts")) return;
			monitor::apply_queue();
			const menu::State m = menu::poll();
			const camera::View v = camera::read_world_camera(true);
			camera::update();

			FnvRemixFacts& f = *out;
			// Nothing is known before the startup report (xNVSE's hooks are
			// written during its start-up, after the plugins load).
			f.ready = integrity::reported() ? 1 : 0;
			f.frame = monitor::frame();
			f.features_ok = (integrity::features_ok() & kSiteBits & monitor::features_built()) |
				(v.ok ? FNVR_F_CAMERA : 0u) | (m.ok ? FNVR_F_MENU : 0u) |
				((integrity::features_ok() & FNVR_F_OWNERS) && owners::events_ok() ? FNVR_F_OWNERS : 0u) |
				(lights::ok() ? FNVR_F_LIGHTS : 0u);
			const water::State w = water::read();
			if (w.ok) f.features_ok |= FNVR_F_WATER;
			f.underwater = w.underwater ? 1 : 0;
			f.water_valid = w.height_valid ? 1 : 0;
			f.water_height = w.height;
			f.cut_serial = camera::cut_serial();
			f.cells_serial = cells::serial();
			f.frames_since_cut = camera::frames_since_cut();
			f.loading = camera::loading() ? 1 : 0;
			f.settled = camera::settled() && !f.loading ? 1 : 0;
			if (f.loading) f.game_state = FNVR_GS_LOADING;
			else if (!camera::in_world()) f.game_state = FNVR_GS_MAIN_MENU;
			else if (!m.ok || m.menu_mode) f.game_state = FNVR_GS_MENU;
			else f.game_state = FNVR_GS_IN_GAME;
			f.pipboy = m.ok ? menu::pipboy_of(m.pipboy_raw) : static_cast<uint8_t>(FNVR_PB_CLOSED);
			f.pipboy_raw = m.ok ? static_cast<uint8_t>((std::min)(m.pipboy_raw, 255u)) : 0;
			f.top_menu = m.ok ? m.top_menu : static_cast<uint16_t>(0);
			f.third_person = camera::read_third_person() == 1 ? 1 : 0;
			f.interior = cells::interior() ? 1 : 0;
			int32_t cx = 0, cy = 0;
			uint32_t dim = 0;
			if (cells::grid(cx, cy, dim))
			{
				f.grid_cx = static_cast<int16_t>(cx);
				f.grid_cy = static_cast<int16_t>(cy);
				f.grid_dim = static_cast<uint16_t>(dim);
			}
			if (v.ok)
			{
				std::memcpy(f.world_cam_pos, v.pos, sizeof(f.world_cam_pos));
				std::memcpy(f.world_cam_rot, v.rot, sizeof(f.world_cam_rot));
				f.world_fov_deg = v.fov;
			}
			f.pins = pins::live();
			f.owners = owners::count();
			f.owner_geos = owners::geometry_count();
			player_pos(f.player_pos);
			f.light_fixups = lights::fixups();
			f.read_faults = static_cast<uint16_t>((std::min)(guard::faults(), 0xFFFFu));
		}

		bool owner_of(uintptr_t geo, FnvRemixOwner* out)
		{
			if (!out || !geo || !main_thread_or_log("owner_of")) return false;
			return owners::owner_of(geo, *out);
		}

		uint32_t members(FnvRemixMember* out, uint32_t max)
		{
			if (!main_thread_or_log("take_members")) return 0;
			monitor::apply_queue();
			if (!max)
			{
				owners::resend_members();
				return 0;
			}
			return out ? owners::take_members(out, max) : 0;
		}

		uint32_t lights_copy(FnvRemixLight* out, uint32_t max)
		{
			if (!out || !main_thread_or_log("lights")) return 0;
			monitor::apply_queue();
			return lights::copy(out, max);
		}

		uint32_t cells_copy(FnvRemixCell* out, uint32_t max)
		{
			if (!out || !main_thread_or_log("cells")) return 0;
			monitor::apply_queue();
			return cells::copy(out, max);
		}

		uint32_t drain(FnvRemixEvent* out, uint32_t max)
		{
			if (!out || !main_thread_or_log("drain_events")) return 0;
			monitor::apply_queue();
			return queue::drain(out, max);
		}

		const char* feature_status(uint32_t bit)
		{
			if (!(monitor::features_built() & bit)) return "not built";
			if (bit == FNVR_F_OWNERS && !owners::events_ok())
				return "OFF: no ref 3D event after a cell attach (xNVSE's Set3D / attach hooks cut?)";
			if (bit == FNVR_F_LIGHTS && !lights::ok()) return "OFF: no ShadowSceneNode";
			if (bit == FNVR_F_WATER && !water::read().ok) return "OFF: the water system's globals are unreadable";
			const char* s = integrity::status(bit);
			return s && *s ? s : "ok";
		}

		bool place_read(FnvRemixPlace* out)
		{
			if (!out || !main_thread_or_log("place")) return false;
			return status::place(*out);
		}

		uint32_t load_order(char* out, uint32_t size)
		{
			if (!main_thread_or_log("load_order")) return 0;
			return mods::load_order(out, size);
		}

		void log_line(const char* tag, const char* text)
		{
			log::event(tag ? tag : "Wrapper", text ? text : "");
		}

		FnvRemixApi g_api = {};
	}
}

extern "C" const FnvRemixApi* FnvRemix_GetApi(uint32_t abi)
{
	using namespace fnvr;
	if (abi != FNV_REMIX_ABI)
	{
		log::e("Api", "FnvRemix_GetApi: the wrapper asks for ABI {}, this plugin is ABI {} (commit {}): refused -- "
			"rebuild both from one commit", abi, FNV_REMIX_ABI, FNVR_STR(FNVR_COMMIT));
		return nullptr;
	}
	auto& a = api::g_api;
	a.abi = FNV_REMIX_ABI;
	a.xnvse_version = monitor::xnvse_version();
	a.plugin_commit = FNVR_STR(FNVR_COMMIT);
	a.features_built = monitor::features_built();
	a.feature_status = &api::feature_status;
	a.facts = &api::facts;
	a.cells = &api::cells_copy;
	a.drain_events = &api::drain;
	a.owner_of = &api::owner_of;
	a.lights = &api::lights_copy;
	a.take_members = &api::members;
	a.place = &api::place_read;
	a.load_order = &api::load_order;
	a.log_line = &api::log_line;
	static bool s_logged = false;
	if (!s_logged)
	{
		s_logged = true;
		log::e("Api", "wrapper linked (ABI {})", abi);
	}
	return &a;
}
