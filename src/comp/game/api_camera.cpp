#include "std_include.hpp"
#include "api_camera.hpp"
#include "stable_origin.hpp"
#include "menu_scene.hpp"
#include "scopes.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/remix_api.hpp"

namespace comp::game::api_camera
{
	namespace
	{
		UINT g_sent_frame = 0xFFFFFFFFu;
		bool g_warned_missing = false;
		bool g_logged_first = false;
		bool g_logged_first_pass = false;

		// Where each frame's world camera came from, over the diagnostic
		// window: a D3D9 world draw, a suppressed engine pass of a claimed
		// static (the frame had no D3D9 world draw before it), or nowhere.
		enum class Source { Draw, StaticPass };
		uint32_t g_window = 0, g_from_draw = 0, g_from_pass = 0, g_none = 0;
		uint32_t g_none_run = 0, g_none_run_max = 0;
		constexpr uint32_t kWindow = 600;

		// The VIEW_MODEL camera: the frame it was decided for, and whether it
		// went out.
		UINT g_vm_frame = 0xFFFFFFFFu;
		bool g_vm_ok = false;
		bool g_vm_logged = false;
		uint64_t g_vm_sent = 0;
		constexpr remixapi_InstanceCategoryFlags kViewModelBit =
			static_cast<remixapi_InstanceCategoryFlags>(REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL);

		// The world camera the runtime took this frame (raw renderer VIEW and
		// PROJECTION) and the frame it went out.
		D3DXMATRIX g_world_view, g_world_proj;
		UINT g_world_frame = 0xFFFFFFFFu;

		// The Pip-Boy fold's first use, for the log.
		bool g_fold_logged = false;

		bool perspective(const float* p)
		{
			return p[11] == 1.0f && p[15] == 0.0f && p[0] > 1e-4f && p[5] > 1e-4f;
		}
	}

	bool world_camera(D3DMATRIX& view, D3DMATRIX& proj)
	{
		if (g_world_frame != shared::common::ffp_state::get().frame_count()) return false;
		view = g_world_view;
		proj = g_world_proj;
		return true;
	}

	bool pipboy_fold(D3DMATRIX& fold)
	{
		if (g_world_frame != shared::common::ffp_state::get().frame_count()) return false;
		const float* view = game::get_renderer_matrix(RENDERER_VIEW_OFF);
		const float* proj = game::get_renderer_matrix(RENDERER_PROJ_OFF);
		if (!view || !proj || !perspective(proj)) return false;
		if (std::memcmp(view, &g_world_view, sizeof(g_world_view)) == 0 &&
			std::memcmp(proj, &g_world_proj, sizeof(g_world_proj)) == 0) return false;

		// S, in view space: x and y such that the world projection maps them
		// where the Pip-Boy's maps the originals (x' = (P00 x + P20 z) / Pw00
		// - Pw20 z / Pw00, the same for y); z is kept.
		const float* pw = g_world_proj;
		D3DXMATRIX s;
		D3DXMatrixIdentity(&s);
		s._11 = proj[0] / pw[0];
		s._22 = proj[5] / pw[5];
		s._31 = (proj[8] - pw[8]) / pw[0];
		s._32 = (proj[9] - pw[9]) / pw[5];

		D3DXMATRIX inv_world_view;
		if (!D3DXMatrixInverse(&inv_world_view, nullptr, &g_world_view)) return false;
		D3DXMATRIX vs, out;
		D3DXMatrixMultiply(&vs, reinterpret_cast<const D3DXMATRIX*>(view), &s);
		D3DXMatrixMultiply(&out, &vs, &inv_world_view);
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				if (!std::isfinite(out.m[r][c])) return false;
		fold = out;

		if (!g_fold_logged)
		{
			g_fold_logged = true;
			shared::common::log("RemixApi", std::format("PipBoyFov: the Pip-Boy is traced through the world camera "
				"at its own FOV -- Pip-Boy _11={:.3f} _22={:.3f}, world _11={:.3f} _22={:.3f} (view x / y scale "
				"{:.3f} / {:.3f})", proj[0], proj[5], pw[0], pw[5], s._11, s._22),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}
		return true;
	}

	remixapi_InstanceCategoryFlags view_model_categories(IDirect3DDevice9* dev)
	{
		if (!shared::common::config::get().remix_api.view_model_camera) return 0;
		if (!game::uses_viewmodel_projection()) return 0;
		const UINT frame = shared::common::ffp_state::get().frame_count();
		if (g_vm_frame == frame) return g_vm_ok ? kViewModelBit : 0u;
		g_vm_frame = frame;
		g_vm_ok = false;

		auto& api = shared::common::remix_api::get();
		if (!shared::common::remix_api::is_initialized() || !api.m_bridge.SetupCamera) return 0;
		const float* view = game::get_renderer_matrix(RENDERER_VIEW_OFF);
		const float* proj = game::get_renderer_matrix(RENDERER_PROJ_OFF);
		// A left-handed perspective projection (w = view z), nothing else.
		if (!view || !proj || proj[11] != 1.0f || proj[15] != 0.0f || !(proj[0] > 1e-4f) || !(proj[5] > 1e-4f))
			return 0;

		// The same frame as every API transform: the stable origin's D and the
		// world unit scale.
		stable_origin::begin_frame(dev);
		const D3DMATRIX v = stable_origin::device_view(*reinterpret_cast<const D3DMATRIX*>(view));
		const D3DMATRIX p = stable_origin::device_projection(*reinterpret_cast<const D3DMATRIX*>(proj));

		remixapi_CameraInfo info = {};
		info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
		info.pNext = nullptr;
		info.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
		std::memcpy(info.view, &v, sizeof(info.view));
		std::memcpy(info.projection, &p, sizeof(info.projection));
		const auto rc = api.m_bridge.SetupCamera(&info);
		g_vm_ok = rc == REMIXAPI_ERROR_CODE_SUCCESS;
		if (g_vm_ok) g_vm_sent++;
		if (!g_vm_logged)
		{
			g_vm_logged = true;
			shared::common::log("RemixApi", std::format("ViewModelCamera: first VIEW_MODEL camera sent (frame {}, result {}, "
				"proj _11={:.3f} _22={:.3f}) -- first-person API draws carry the VIEW_MODEL category", frame,
				static_cast<int>(rc), proj[0], proj[5]), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}
		return g_vm_ok ? kViewModelBit : 0u;
	}

	namespace
	{
		void send_world_camera(IDirect3DDevice9* dev, Source source)
		{
			auto& ffp = shared::common::ffp_state::get();
			const UINT frame = ffp.frame_count();
			if (g_sent_frame == frame) return;
			if (!shared::common::config::get().remix_api.fallback_camera) return;
			if (!shared::common::remix_api::is_initialized()) return;
			// A frame rasterised for a menu scene has no camera (menu_scene.hpp).
			if (menu_scene::raster_frame()) return;

			// The world draw gates of the renderer's FFP routes. A claimed
			// static's pass is world geometry: the sky test (the previous
			// pass's shade property at that point) does not apply to it.
			if (!ffp.is_enabled() || !ffp.view_proj_valid() || !game::rendering_to_backbuffer ||
				game::is_2d() || (source == Source::Draw && game::is_sky())) return;

			const float* view = game::get_renderer_matrix(RENDERER_VIEW_OFF);
			const float* proj = game::get_renderer_matrix(RENDERER_PROJ_OFF);
			// A left-handed perspective projection (w = view z), nothing else.
			if (!view || !proj || proj[11] != 1.0f || proj[15] != 0.0f || !(proj[0] > 1e-4f) || !(proj[5] > 1e-4f))
				return;
			g_sent_frame = frame;

			auto& api = shared::common::remix_api::get();
			if (!api.m_bridge.SetupCamera)
			{
				if (!g_warned_missing)
				{
					g_warned_missing = true;
					shared::common::log("RemixApi", "FallbackCamera: the Remix bridge does not forward SetupCamera "
						"(older bridge?) -- no fallback camera", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
				return;
			}

			// This frame's render frame, as the draw's own transforms will carry it.
			stable_origin::begin_frame(dev);
			const D3DMATRIX v = stable_origin::device_view(*reinterpret_cast<const D3DMATRIX*>(view));
			const D3DMATRIX p = stable_origin::device_projection(*reinterpret_cast<const D3DMATRIX*>(proj));

			remixapi_CameraInfoFrameEventsEXT events = {};
			events.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_FRAME_EVENTS_EXT;
			events.flags = REMIXAPI_CAMERA_FRAME_EVENT_FALLBACK;

			// The player's scope lens, in the same render frame (scopes.hpp).
			remixapi_CameraInfoLensEXT lens = {};
			if (scopes::lens(lens)) events.pNext = &lens;

			remixapi_CameraInfo info = {};
			info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
			info.pNext = &events;
			info.type = REMIXAPI_CAMERA_TYPE_WORLD;
			std::memcpy(info.view, &v, sizeof(info.view));
			std::memcpy(info.projection, &p, sizeof(info.projection));
			if (events.pNext == &lens) scopes::lens_sent(v, lens);

			const auto rc = api.m_bridge.SetupCamera(&info);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS)
			{
				std::memcpy(&g_world_view, view, sizeof(g_world_view));
				std::memcpy(&g_world_proj, proj, sizeof(g_world_proj));
				g_world_frame = frame;
			}
			if (source == Source::Draw) g_from_draw++;
			else g_from_pass++;
			if (!g_logged_first)
			{
				g_logged_first = true;
				shared::common::log("RemixApi", std::format("FallbackCamera: first camera sent (frame {}, result {})",
					frame, static_cast<int>(rc)), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			}
			if (source == Source::StaticPass && !g_logged_first_pass)
			{
				g_logged_first_pass = true;
				shared::common::log("RemixApi", std::format("FallbackCamera: first camera sent at a suppressed static's "
					"engine pass, a frame with no D3D9 world draw before it (frame {}, result {})", frame,
					static_cast<int>(rc)), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			}
		}
	}

	void on_draw(IDirect3DDevice9* dev)
	{
		send_world_camera(dev, Source::Draw);
	}

	void on_claimed_static_pass()
	{
		IDirect3DDevice9* dev = game::frame_device;
		if (!dev) return;
		// The frame's first world pass work (D, the world FOV, the lights),
		// as a D3D9 world draw does it, then the camera.
		game::on_world_draw(dev, true);
		send_world_camera(dev, Source::StaticPass);
	}

	void on_present()
	{
		if (!shared::common::config::get().remix_api.fallback_camera) return;
		if (!shared::common::remix_api::is_initialized()) return;
		const UINT frame = shared::common::ffp_state::get().frame_count();
		if (g_sent_frame != frame)
		{
			g_none++;
			g_none_run++;
			g_none_run_max = (std::max)(g_none_run_max, g_none_run);
		}
		else g_none_run = 0;
		if (++g_window < kWindow) return;
		shared::common::log("RemixApi", std::format("FallbackCamera over {} frames: world camera sent at a D3D9 world "
			"draw {}, at a suppressed static's engine pass {} (no D3D9 world draw that frame), not sent {} (longest run "
			"{}; such a frame has no Remix camera and is not path traced unless a draw set one)", g_window, g_from_draw,
			g_from_pass, g_none, g_none_run_max), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		g_window = g_from_draw = g_from_pass = g_none = g_none_run_max = 0;
	}
}
