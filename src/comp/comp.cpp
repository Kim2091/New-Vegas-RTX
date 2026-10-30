#include "std_include.hpp"

#include "modules/imgui.hpp"
#include "modules/renderer.hpp"
#include "modules/diagnostics.hpp"
#include "modules/tracer.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/fnv_exe.hpp"
#include "game/nvse_link.hpp"
#include "game/stable_origin.hpp"
#include "game/weather.hpp"

namespace comp
{
	// Sky drive: sun, moon, weather and camera position pushed to Remix options.
	//
	// Every push is a SetConfigVariable across the bridge, and the runtime
	// applies it by writing an RtxOption layer that its render thread resolves
	// at the frame boundary. Pushes happen once per frame (FNV opens several
	// scenes a frame and on_begin_scene_cb fires for each), only when the
	// formatted value changed (the sun moves ~0.004 deg a frame at normal
	// timescale, so most frames send nothing), and only once the sky has
	// settled. Fast travel and cell transitions free and rebuild the Sky
	// objects, and for a few frames the chain can read half-built nodes, so a
	// reading is used only after SETTLE_FRAMES consecutive valid frames from
	// the same Sky object.
	namespace sky_drive
	{
		static constexpr int WARMUP_FRAMES = 120;
		static constexpr int SETTLE_FRAMES = 30;

		// Remembers the last string pushed for one option, so an unchanged value
		// never crosses the bridge twice. Cleared by forget() whenever the
		// option might have been changed elsewhere (disable paths).
		struct pushed_value
		{
			const char* key;
			char last[96] = {};

			void push(const char* value)
			{
				if (std::strcmp(last, value) == 0) return;
				auto& api = shared::common::remix_api::get();
				api.m_bridge.SetConfigVariable(key, value);
				strncpy_s(last, value, _TRUNCATE);
			}

			void forget() { last[0] = '\0'; }
		};

		static pushed_value s_sun_elev   { "rtx.atmosphere.sunElevation" };
		static pushed_value s_sun_rot    { "rtx.atmosphere.sunRotation" };
		static pushed_value s_moon_on    { "rtx.atmosphere.moon0.enabled" };
		static pushed_value s_moon_elev  { "rtx.atmosphere.moon0.elevation" };
		static pushed_value s_moon_rot   { "rtx.atmosphere.moon0.rotation" };
		static pushed_value s_moon_phase { "rtx.atmosphere.moon0.phase" };
		static int s_shown_phase = -1;
		static pushed_value s_cam_anchor { "rtx.atmosphere.useCameraWorldOverride" };
		static pushed_value s_cam_pos    { "rtx.atmosphere.cameraWorldOverride" };

		static int  s_warmup = 0;
		static UINT s_last_frame = 0xFFFFFFFFu;

		// Settle tracking, keyed on the Sky object's address: a rebuilt sky is a
		// new allocation, so an address change restarts the count even if every
		// read in between happened to succeed.
		static const void* s_sky = nullptr;
		static int s_valid_frames = 0;

		static std::uint32_t s_log_counter = 0;
		static bool s_logged_hours = false;

		static void log_horizon_hours_once()
		{
			if (s_logged_hours) return;
			float a, b;
			if (!game::get_sun_horizon_hours(a, b)) return;
			s_logged_hours = true;
			shared::common::log("SunCycle",
				std::format("horizon crossings: sunrise={:.2f} sunset={:.2f}", a, b),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static void update_sun(bool enabled)
		{
			if (!enabled)
			{
				// sunElevation/sunRotation are NoSave, so they cold-reset every run;
				// pushing the defaults keeps the toggle live within a session.
				s_sun_elev.push("15.00");
				s_sun_rot.push("0.00");
				return;
			}

			float elevation, rotation, hour = -1.0f;
			bool night = false;
			if (!game::try_get_sun_orientation(elevation, rotation, &hour, &night))
			{
				static bool s_logged = false;
				if (!s_logged)
				{
					s_logged = true;
					shared::common::log("SunCycle", "sky found but the sun's scene-graph chain did not read back -- sun not driven",
						shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
				return;
			}

			char buf[32];
			snprintf(buf, sizeof(buf), "%.2f", elevation);
			s_sun_elev.push(buf);
			snprintf(buf, sizeof(buf), "%.2f", rotation);
			s_sun_rot.push(buf);

			if ((s_log_counter % 300) == 0)
				shared::common::log("SunCycle",
					std::format("hour={:.2f} elev={:.2f} rot={:.2f} {}",
						hour, elevation, rotation, night ? "night" : "day"),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}

		static void update_moon(bool enabled)
		{
			// moon0.enabled persists in rtx.conf, so turning the cycle off has to
			// push the 0 explicitly or the moon stays up from the last session.
			s_moon_on.push(enabled ? "1" : "0");
			if (!enabled) return;

			float elevation, rotation;
			if (!game::try_get_masser_orientation(elevation, rotation))
			{
				static bool s_logged = false;
				if (!s_logged)
				{
					s_logged = true;
					shared::common::log("MoonCycle", "sky found but the moon's scene-graph chain did not read back -- moon not driven",
						shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
				return;
			}

			char buf[32];
			snprintf(buf, sizeof(buf), "%.2f", elevation);
			s_moon_elev.push(buf);
			snprintf(buf, sizeof(buf), "%.2f", rotation);
			s_moon_rot.push(buf);

			// The game's phase: an index stepped once per in-game day by the
			// climate's phase length (0 full .. 4 new .. 7), whose texture the
			// moon takes only once it is below the horizon. The sky's phase is
			// 0 new, 0.5 full; without it the sky lights the moon from the sun,
			// and the game's moon track runs a whole cycle in one night.
			const uintptr_t phase_addr = fnv_exe::addr().moon_phase;
			if (phase_addr)
			{
				const int index = static_cast<int>(*reinterpret_cast<const volatile uint32_t*>(phase_addr) & 7u);
				if (s_shown_phase < 0 || elevation <= 0.0f) s_shown_phase = index;
				snprintf(buf, sizeof(buf), "%.3f", static_cast<float>((s_shown_phase + 4) & 7) / 8.0f);
				s_moon_phase.push(buf);
			}

			if ((s_log_counter % 300) == 0)
				shared::common::log("MoonCycle",
					std::format("elev={:.2f} rot={:.2f} phase {}", elevation, rotation, s_shown_phase),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}

		// FNV renders camera-relative (the engine subtracts the camera position
		// out of all geometry and lights), so Remix's own camera sits at the
		// origin and a cloud volume anchored to it follows the player. Pushing
		// the world camera (the engine monitor's, which the Pip-Boy's menu
		// camera never replaces) restores world-anchored clouds and
		// fly-through. Without the plugin the override is off.
		static void update_camera()
		{
			if (!game::nvse_link::has(FNVR_F_CAMERA))
			{
				s_cam_anchor.push("False");
				return;
			}
			s_cam_anchor.push("True");
			// The frame's camera (the stable origin's), else the world-camera
			// fact.
			float anchor[3];
			const double* c = game::stable_origin::camera();
			for (int i = 0; i < 3; i++)
				anchor[i] = game::stable_origin::active() ? static_cast<float>(c[i]) : game::nvse_link::facts().world_cam_pos[i];

			// In render units (the world unit scale). Vector3 RtxOption parses
			// "x, y, z". One decimal is ~1 mm; finer only resends while
			// standing still with idle sway.
			const double s = game::stable_origin::scale();
			char buf[96];
			snprintf(buf, sizeof(buf), "%.1f, %.1f, %.1f", anchor[0] * s, anchor[1] * s, anchor[2] * s);
			s_cam_pos.push(buf);
		}

		static void update()
		{
			auto& api = shared::common::remix_api::get();
			if (!api.is_initialized()) return;

			const UINT frame = shared::common::ffp_state::get().frame_count();
			if (frame == s_last_frame) return;
			s_last_frame = frame;

			// Let the bridge finish its handshake before the first push.
			if (s_warmup < WARMUP_FRAMES) { s_warmup++; return; }

			update_camera();
			game::weather::update();

			// Disabled cycles push their defaults without touching the sky, so
			// the toggle also works in interiors and menus.
			const auto& cfg = shared::common::config::get();
			if (!cfg.sun_cycle.enabled)  update_sun(false);
			if (!cfg.moon_cycle.enabled) update_moon(false);
			if (!cfg.sun_cycle.enabled && !cfg.moon_cycle.enabled) return;

			const void* sky = game::try_get_sky();
			if (!sky)
			{
				// Logged once, after long enough that menus and loading screens
				// don't trip it: the drive is silently off without this line.
				static int s_no_sky_frames = 0;
				if (++s_no_sky_frames == 1800)
					shared::common::log("SunCycle", "no Sky object for 1800 frames -- sun/moon not being driven",
						shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			if (!sky || sky != s_sky)
			{
				s_sky = sky;
				s_valid_frames = 0;
				// A new sky (a load): the game shows its phase at once.
				s_shown_phase = -1;
				return;
			}
			if (s_valid_frames < SETTLE_FRAMES) { s_valid_frames++; return; }

			log_horizon_hours_once();

			if (cfg.sun_cycle.enabled)  update_sun(true);
			if (cfg.moon_cycle.enabled) update_moon(true);
			s_log_counter++;
		}
	}

	void on_begin_scene_cb()
	{
		if (!tex_addons::initialized) {
			tex_addons::init_texture_addons();
		}

		// Sun, moon and world-anchored clouds. Self-gates to once per frame.
		sky_drive::update();

		// Backbuffer tracking starts on the first scene, once the device exists.
		if (game::backbuffer_width == 0)
			game::init_backbuffer_tracking(shared::globals::d3d_device);

		// World starts as identity; per-draw transforms are set in renderer.cpp.
		shared::globals::d3d_device->SetTransform(D3DTS_WORLD, &shared::globals::IDENTITY);
	}


	void main()
	{
		shared::common::remix_api::initialize(nullptr, nullptr, nullptr, false);

		shared::common::loader::module_loader::register_module(std::make_unique<tracer>());
		shared::common::loader::module_loader::register_module(std::make_unique<imgui>());
		shared::common::loader::module_loader::register_module(std::make_unique<renderer>());

		auto& cfg = shared::common::config::get();
		if (cfg.diagnostics.enabled)
			shared::common::loader::module_loader::register_module(std::make_unique<diagnostics>());
	}
}
