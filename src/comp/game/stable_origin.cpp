#include "std_include.hpp"
#include "stable_origin.hpp"
#include "nvse_link.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/remix_api.hpp"

namespace comp::game::stable_origin
{
	namespace
	{
		bool   g_active = false;
		bool   g_have_origin = false;
		double g_cam[3] = {};
		double g_origin[3] = {};
		// s * O, what the runtime is told (render units).
		float  g_origin_render[3] = {};
		double g_shift[3] = {};
		uint32_t g_generation = 0;
		UINT   g_frame = 0xFFFFFFFFu;
		// The engine monitor's cut serial the origin was last snapped for.
		uint32_t g_cut_serial = 0;
		// The engine camera global this frame is the Pip-Boy's menu camera
		// (held at the last world reading).
		bool g_menu_camera = false;
		// Over the diagnostic window: frames whose camera was re-taken at the
		// first world draw, and frames on the menu camera.
		uint32_t g_retaken = 0, g_menu_frames = 0;

		// The global this far from the engine monitor's world camera, while a
		// menu can be up, is the menu camera. The world-camera fact may be a
		// frame old (sprinting moves ~20 units a frame), so the margin is wide.
		constexpr double kMenuCameraUnits = 256.0;

		// Not shared::globals::IDENTITY: that one is dynamically initialised
		// in another translation unit.
		constexpr D3DMATRIX kIdentity = { { {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f } } };

		// What the device's WORLD / VIEW / PROJECTION were last set to (raw,
		// as the caller passed them) and the D they carry. D3D9 starts them
		// at identity.
		D3DMATRIX g_raw_world = kIdentity;
		D3DMATRIX g_raw_view = kIdentity;
		D3DMATRIX g_raw_proj = kIdentity;
		double g_applied[3] = {};
		constexpr double kNoShift[3] = {};

		// FOGSTART, FOGEND, FOGDENSITY as last set (raw), and which were set.
		DWORD g_raw_fog[3] = {};
		bool  g_raw_fog_set[3] = {};

		int fog_slot(D3DRENDERSTATETYPE state)
		{
			switch (state)
			{
			case D3DRS_FOGSTART:   return 0;
			case D3DRS_FOGEND:     return 1;
			case D3DRS_FOGDENSITY: return 2;
			default:               return -1;
			}
		}

		UINT     g_last_log_frame = 0;
		bool     g_logged_once = false;
		uint32_t g_unlogged_snaps = 0;

		constexpr UINT kLogIntervalFrames = 60;

		// [RemixApi] PublishWorldOrigin: what the runtime was last told
		// (__mcrtx.world_origin.*). Cleared on a device reset and whenever the
		// API is not up, so the next frame sends it again.
		bool   g_origin_published = false;
		float  g_published_origin[3] = {};
		bool   g_publish_warned = false;

		// Tells the runtime this frame's O (remixapi_SetGameValue
		// __mcrtx.world_origin.x/y/z, render units), so that with
		// rtx.worldOrigin.enable render position + O = world position: the
		// runtime shifts camera and instance history by an origin move before
		// the frame's camera update, and anchors the clouds (and their ground
		// shadows) to the world. The render frame is W * T(D) * S with D =
		// camera - O applied to FNV's camera-relative WORLD (object - camera)
		// and S the world unit scale, i.e. render = s * (world - O), so the
		// origin goes out as s * O. Runs from begin_frame, before any
		// transform or draw of the frame reaches the bridge; SetGameValue is
		// a synchronous bridge call, so the runtime sees it ahead of this
		// frame's draws. Sent only
		// when it changes (and again after a reset / API init).
		void publish_world_origin()
		{
			const auto& cfg = shared::common::config::get().remix_api;
			auto& api = shared::common::remix_api::get();
			if (!shared::common::remix_api::is_initialized())
			{
				g_origin_published = false;
				return;
			}

			float want[3] = { 0.0f, 0.0f, 0.0f };
			const bool publish = cfg.publish_world_origin && cfg.stable_origin;
			if (publish)
			{
				if (!g_have_origin) return; // no camera yet: keep what was sent
				std::memcpy(want, origin_render(), sizeof(want));
			}
			else if (!g_origin_published || (want[0] == g_published_origin[0] &&
				want[1] == g_published_origin[1] && want[2] == g_published_origin[2]))
			{
				// Off: never create the values; only take back a non-zero one.
				return;
			}

			if (g_origin_published && std::memcmp(want, g_published_origin, sizeof(want)) == 0) return;

			if (!api.m_bridge.SetGameValue)
			{
				if (!g_publish_warned)
				{
					g_publish_warned = true;
					shared::common::log("StableOrigin", "PublishWorldOrigin: the bridge has no SetGameValue -- "
						"world origin not published", shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
				}
				return;
			}

			static const char* const kKeys[3] = {
				"__mcrtx.world_origin.x", "__mcrtx.world_origin.y", "__mcrtx.world_origin.z" };
			bool ok = true;
			for (int i = 0; i < 3; i++)
			{
				char buf[48];
				std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(want[i]));
				ok = api.m_bridge.SetGameValue(kKeys[i], buf) == REMIXAPI_ERROR_CODE_SUCCESS && ok;
			}
			if (!ok) return; // try again next frame

			const bool first = !g_origin_published;
			std::memcpy(g_published_origin, want, sizeof(want));
			g_origin_published = true;
			shared::common::log("StableOrigin", std::format("published world origin ({:.0f}, {:.0f}, {:.0f}){}",
				want[0], want[1], want[2], first ? " [first / after reset]" : ""),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}

		bool is_world_state(D3DTRANSFORMSTATETYPE s)
		{
			return s >= D3DTS_WORLDMATRIX(0) && s <= D3DTS_WORLDMATRIX(255);
		}

		// W * T(D) * S: column c < 3 gains W[r][3] * D[c] (for an affine W
		// only the translation row moves), then is scaled by s.
		D3DMATRIX shift_world(const D3DMATRIX& w, const double* d)
		{
			const double s = scale();
			D3DMATRIX out = w;
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 3; c++)
					out.m[r][c] = static_cast<float>((static_cast<double>(w.m[r][c]) + static_cast<double>(w.m[r][3]) * d[c]) * s);
			return out;
		}

		// S^-1 * T(-D) * V * S: the translation row changes, and the camera
		// stays a rigid view (the rotation block is unchanged; the
		// translation row is scaled by s).
		D3DMATRIX shift_view(const D3DMATRIX& v, const double* d)
		{
			const double s = scale();
			D3DMATRIX out = v;
			for (int c = 0; c < 4; c++)
			{
				double t = v.m[3][c];
				for (int i = 0; i < 3; i++) t -= d[i] * static_cast<double>(v.m[i][c]);
				out.m[3][c] = static_cast<float>(c < 3 ? t * s : t);
			}
			for (int r = 0; r < 3; r++)
				out.m[r][3] = static_cast<float>(static_cast<double>(v.m[r][3]) / s);
			return out;
		}

		// S^-1 * P, so WORLD * VIEW * PROJECTION is unchanged. A perspective
		// projection (w = view z) is multiplied through by s, which leaves
		// the image as it is and keeps its usual form: only its last row
		// (the near / far planes) is scaled.
		D3DMATRIX scale_projection(const D3DMATRIX& p)
		{
			const double s = scale();
			D3DMATRIX out = p;
			if (p.m[3][3] == 0.0f)
			{
				for (int c = 0; c < 4; c++) out.m[3][c] = static_cast<float>(static_cast<double>(p.m[3][c]) * s);
			}
			else
			{
				for (int r = 0; r < 3; r++)
					for (int c = 0; c < 4; c++) out.m[r][c] = static_cast<float>(static_cast<double>(p.m[r][c]) / s);
			}
			return out;
		}

		// Nearest grid point to the camera.
		void snap_origin(const char* reason)
		{
			const double grid = shared::common::config::get().remix_api.stable_origin_grid;
			for (int i = 0; i < 3; i++)
			{
				g_origin[i] = std::round(g_cam[i] / grid) * grid;
				g_origin_render[i] = static_cast<float>(g_origin[i] * scale());
			}
			g_have_origin = true;
			g_generation++;

			// One line per move, at most one per kLogIntervalFrames; moves in
			// between are counted into the next line.
			const UINT now = g_frame;
			if (g_logged_once && now - g_last_log_frame < kLogIntervalFrames)
			{
				g_unlogged_snaps++;
				return;
			}
			g_logged_once = true;
			g_last_log_frame = now;
			shared::common::log("StableOrigin", std::format(
				"origin #{} -> ({:.0f}, {:.0f}, {:.0f}), camera ({:.1f}, {:.1f}, {:.1f}) [{}]{}",
				g_generation, g_origin[0], g_origin[1], g_origin[2], g_cam[0], g_cam[1], g_cam[2], reason,
				g_unlogged_snaps ? std::format(" (+{} moves not logged)", g_unlogged_snaps) : std::string()),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			g_unlogged_snaps = 0;
		}

		// The global is the Pip-Boy's menu camera, not the world's: a menu can
		// be up and the global is far from the engine monitor's world camera.
		bool is_menu_camera(const float* cp)
		{
			const FnvRemixFacts& f = nvse_link::facts();
			if (f.pipboy == FNVR_PB_CLOSED && f.game_state != FNVR_GS_MENU) return false;
			double d2 = 0.0;
			for (int i = 0; i < 3; i++)
			{
				const double d = static_cast<double>(cp[i]) - static_cast<double>(f.world_cam_pos[i]);
				d2 += d * d;
			}
			return d2 > kMenuCameraUnits * kMenuCameraUnits;
		}

		bool finite3(const float* p)
		{
			return p && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
		}

		// The camera is the engine's camera global, what the frame's WORLD
		// matrices are relative to (this frame's, as every draw uses it). The
		// engine monitor's world camera, which can be a frame old, only says
		// when the global is the Pip-Boy's menu camera (then the last world
		// reading holds). A cut (load, door, fast travel, teleport) snaps the
		// origin.
		void update_camera()
		{
			const float* cp = game::camera_position_ptr;
			if (!nvse_link::has(FNVR_F_CAMERA) || !finite3(cp))
			{
				g_active = false;
				return;
			}
			const FnvRemixFacts& f = nvse_link::facts();
			g_menu_camera = g_have_origin && f.cut_serial == g_cut_serial && is_menu_camera(cp);
			if (g_menu_camera) g_menu_frames++;
			else
				for (int i = 0; i < 3; i++) g_cam[i] = cp[i];

			if (!g_have_origin)
			{
				snap_origin("first");
				g_cut_serial = f.cut_serial;
			}
			else if (f.cut_serial != g_cut_serial)
			{
				snap_origin("cut");
				g_cut_serial = f.cut_serial;
			}
			else
			{
				const double dx = g_cam[0] - g_origin[0], dy = g_cam[1] - g_origin[1], dz = g_cam[2] - g_origin[2];
				const double radius = shared::common::config::get().remix_api.stable_origin_radius;
				if (dx * dx + dy * dy + dz * dz > radius * radius)
					snap_origin("radius");
			}
			g_active = true;
		}
	}

	namespace
	{
		// The device still holds the pair with the last D; move both to the
		// current D before anything draws with one of them re-set and the
		// other not.
		void apply_shift(IDirect3DDevice9* dev)
		{
			if (!dev || std::memcmp(g_applied, g_shift, sizeof(g_shift)) == 0) return;
			const D3DMATRIX w = shift_world(g_raw_world, g_shift);
			const D3DMATRIX v = shift_view(g_raw_view, g_shift);
			dev->SetTransform(D3DTS_WORLD, &w);
			dev->SetTransform(D3DTS_VIEW, &v);
			std::memcpy(g_applied, g_shift, sizeof(g_shift));
		}
	}

	bool enabled()
	{
		return shared::common::config::get().remix_api.stable_origin;
	}

	bool active()
	{
		return g_active && enabled();
	}

	void begin_frame(IDirect3DDevice9* dev)
	{
		if (!enabled())
		{
			// Switched off: a published origin is set back to zero once; nothing is sent if none was.
			publish_world_origin();
			return;
		}
		const UINT now = shared::common::ffp_state::get().frame_count();
		if (now == g_frame) return;
		g_frame = now;

		update_camera();
		for (int i = 0; i < 3; i++) g_shift[i] = g_active ? g_cam[i] - g_origin[i] : 0.0;
		publish_world_origin();
		apply_shift(dev);
	}

	void at_first_world_draw(IDirect3DDevice9* dev)
	{
		begin_frame(dev);
		if (!active() || g_menu_camera) return;
		const float* cp = game::camera_position_ptr;
		if (!finite3(cp)) return;
		bool same = true;
		for (int i = 0; i < 3; i++) same = same && static_cast<double>(cp[i]) == g_cam[i];
		if (same || is_menu_camera(cp)) return;
		// The global moved since BeginScene: this is the camera the world
		// draws use. D follows it; O stays (the frame's retained replay
		// already went out with it).
		for (int i = 0; i < 3; i++)
		{
			g_cam[i] = cp[i];
			g_shift[i] = g_cam[i] - g_origin[i];
		}
		g_retaken++;
		apply_shift(dev);
	}

	HRESULT set_transform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
	{
		if (!m) return dev->SetTransform(state, m);
		if (state == D3DTS_PROJECTION)
		{
			g_raw_proj = *m;
			const D3DMATRIX p = scale_projection(*m);
			return dev->SetTransform(state, &p);
		}
		if (state != D3DTS_VIEW && !is_world_state(state)) return dev->SetTransform(state, m);

		// The raw pair is kept while the origin is off too, so switching it
		// on mid-game (set_enabled) starts from the pair the device holds.
		// Off, D (g_applied) is zero and only the scale applies.
		if (state == D3DTS_VIEW) g_raw_view = *m;
		else if (state == D3DTS_WORLD) g_raw_world = *m;
		if (enabled()) begin_frame(dev);

		const D3DMATRIX out = state == D3DTS_VIEW ? shift_view(*m, g_applied) : shift_world(*m, g_applied);
		return dev->SetTransform(state, &out);
	}

	D3DMATRIX device_view(const D3DMATRIX& raw)
	{
		return shift_view(raw, g_applied);
	}

	D3DMATRIX device_projection(const D3DMATRIX& raw)
	{
		return scale_projection(raw);
	}

	bool get_raw_transform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* out)
	{
		if (!out || (!enabled() && scale() == 1.0)) return false;
		if (state == D3DTS_VIEW)       { *out = g_raw_view;  return true; }
		if (state == D3DTS_WORLD)      { *out = g_raw_world; return true; }
		if (state == D3DTS_PROJECTION) { *out = g_raw_proj;  return true; }
		return false;
	}

	double scale()
	{
		return static_cast<double>(shared::common::config::get().remix_api.world_unit_scale);
	}

	remixapi_Transform to_render(const remixapi_Transform& t)
	{
		const double s = scale();
		remixapi_Transform out;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++)
				out.matrix[r][c] = static_cast<float>(static_cast<double>(t.matrix[r][c]) * s);
		return out;
	}

	const double* shift()  { return g_shift; }
	const double* origin() { return g_origin; }
	const double* camera() { return g_cam; }
	const float* origin_render() { return g_origin_render; }

	bool camera_matches_now()
	{
		if (!active()) return false;
		const float* cp = game::camera_position_ptr;
		if (!cp) return false;
		constexpr double kTolerance = 0.01;
		bool match = true;
		double worst = 0.0;
		for (int i = 0; i < 3; i++)
		{
			const double d = std::fabs(static_cast<double>(cp[i]) - g_cam[i]);
			if (!(d <= kTolerance)) match = false;
			worst = (std::max)(worst, std::isfinite(d) ? d : 1e30);
		}
		// The diagnostic, once per 600 frames: the camera this frame's D (and
		// so the Remix camera and every API placement) was computed from,
		// against the engine camera at the frame's API draws. Outside the
		// Pip-Boy it must agree, difference 0.
		static UINT s_frame = 0xFFFFFFFFu, s_window = 0;
		static uint32_t s_agree = 0, s_differ = 0;
		static double s_worst = 0.0;
		if (s_frame != g_frame)
		{
			s_frame = g_frame;
			(match ? s_agree : s_differ)++;
			if (!match && !g_menu_camera) s_worst = (std::max)(s_worst, worst);
			if (++s_window >= 600)
			{
				shared::common::log("StableOrigin", std::format("camera check over {} frames: the frame's camera (D, the "
					"Remix camera) vs the engine camera at its API draws: agree {}, differ {} (largest difference outside "
					"the menu camera {:.3f}) | re-taken at the first world draw {} | menu-camera frames {}", s_window, s_agree,
					s_differ, s_worst, g_retaken, g_menu_frames),
					s_worst > kTolerance ? shared::common::LOG_TYPE::LOG_TYPE_WARN : shared::common::LOG_TYPE::LOG_TYPE_DEFAULT,
					false);
				s_window = s_agree = s_differ = 0;
				s_worst = 0.0;
				g_retaken = g_menu_frames = 0;
			}
		}
		return match;
	}

	void light_to_render(D3DLIGHT9& light)
	{
		if (light.Type == D3DLIGHT_DIRECTIONAL) return;
		const double s = scale();
		const double* d = active() ? g_shift : kNoShift;
		light.Position.x = static_cast<float>((static_cast<double>(light.Position.x) + d[0]) * s);
		light.Position.y = static_cast<float>((static_cast<double>(light.Position.y) + d[1]) * s);
		light.Position.z = static_cast<float>((static_cast<double>(light.Position.z) + d[2]) * s);
		// The same falloff over distances s times as long: 1 / (a0 + a1 d + a2 d^2)
		// keeps its value at d * s with a1 / s and a2 / s^2.
		light.Range = static_cast<float>(static_cast<double>(light.Range) * s);
		light.Attenuation1 = static_cast<float>(static_cast<double>(light.Attenuation1) / s);
		light.Attenuation2 = static_cast<float>(static_cast<double>(light.Attenuation2) / (s * s));
	}

	void set_enabled(IDirect3DDevice9* dev, bool on)
	{
		auto& cfg = shared::common::config::get().remix_api;
		if (cfg.stable_origin == on) return;
		cfg.stable_origin = on;

		// Either way the next frame starts over: a fresh origin, snapped at
		// the camera.
		g_have_origin = false;
		g_frame = 0xFFFFFFFFu;

		if (!on)
		{
			// The device's WORLD / VIEW still carry D. Put the unshifted pair
			// back together, so nothing draws with one unshifted and the
			// other not.
			const bool shifted = g_applied[0] != 0.0 || g_applied[1] != 0.0 || g_applied[2] != 0.0;
			if (dev && shifted)
			{
				const D3DMATRIX w = shift_world(g_raw_world, kNoShift);
				const D3DMATRIX v = shift_view(g_raw_view, kNoShift);
				dev->SetTransform(D3DTS_WORLD, &w);
				dev->SetTransform(D3DTS_VIEW, &v);
			}
			std::memset(g_applied, 0, sizeof(g_applied));
			std::memset(g_shift, 0, sizeof(g_shift));
			g_active = false;
		}
		// On: the device holds the raw pair (set_transform tracks it while
		// off); the next begin_frame computes D and re-issues both shifted.

		shared::common::log("StableOrigin", on
			? "switched on -- the origin is snapped at the camera from the next frame"
			: "switched off -- camera-relative frame from the next frame",
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
	}

	DWORD set_fog_state(D3DRENDERSTATETYPE state, DWORD value)
	{
		const int slot = fog_slot(state);
		const double s = scale();
		if (slot < 0 || s == 1.0) return value;
		g_raw_fog[slot] = value;
		g_raw_fog_set[slot] = true;
		float f;
		std::memcpy(&f, &value, sizeof(f));
		f = static_cast<float>(slot == 2 ? static_cast<double>(f) / s : static_cast<double>(f) * s);
		DWORD out;
		std::memcpy(&out, &f, sizeof(out));
		return out;
	}

	bool get_raw_fog_state(D3DRENDERSTATETYPE state, DWORD* out)
	{
		const int slot = fog_slot(state);
		if (slot < 0 || !g_raw_fog_set[slot] || scale() == 1.0) return false;
		*out = g_raw_fog[slot];
		return true;
	}

	void on_reset()
	{
		// Reset puts identity (no shift) back on the device. The rest of this
		// frame sets its transforms with no shift either, so each pair stays
		// consistent; the next begin_frame re-applies D to both.
		g_raw_world = kIdentity;
		g_raw_view = kIdentity;
		g_raw_proj = kIdentity;
		std::memset(g_raw_fog_set, 0, sizeof(g_raw_fog_set));
		std::memset(g_applied, 0, sizeof(g_applied));
		// The runtime may have been rebuilt with the device: send O again.
		g_origin_published = false;
	}
}
