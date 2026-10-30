#include "std_include.hpp"
#include "pipboy_cursor.hpp"
#include "game.hpp"
#include "nvse_link.hpp"
#include "scene_graph.hpp"

#include "shared/fnv_exe.hpp"
#include "shared/common/ffp_state.hpp"

namespace comp::game::pipboy_cursor
{
	namespace
	{
		namespace sg = scene_graph;

		// InterfaceManager: the cursor's screen pixel (top-left origin).
		constexpr uint32_t IM_cursorX = 0x38;
		constexpr uint32_t IM_cursorY = 0x40;

		// A draw is the cursor's when its origin lands within this many
		// pixels of the cursor pixel.
		constexpr float kMatchPixels = 2.0f;

		// A camera seen this frame or the one before is current (the raise and
		// lower animations change both every frame; a one-frame-old one is the
		// closest there is).
		constexpr UINT kMaxCameraAge = 1;

		struct Camera
		{
			float view[16] = {};
			float proj[16] = {};
			UINT frame = 0xFFFFFFFFu;
		};
		Camera g_world, g_pip;

		// The ModelViewProj layout of the screen-space VS: Transposed (rows
		// c0-c3 of the transposed matrix; the origin's clip x / y are c0.w /
		// c1.w) or Rows (the matrix's rows; origin = c3.x / c3.y). Learned
		// from the first draw that tells them apart.
		enum class Layout { Unknown, Transposed, Rows };
		Layout g_layout = Layout::Unknown;

		// This frame's mapping.
		UINT g_map_frame = 0xFFFFFFFFu;
		bool g_map_ok = false;
		float g_ndc[2] = {}, g_delta[2] = {};
		float g_cursor_px[2] = {};
		const char* g_map_why = "";

		// The draw in flight: its game c0-c3 are restored after it.
		bool g_patched = false;

		// The Pip-Boy session (engine monitor: not closed).
		bool g_open = false;
		bool g_logged_open = false;
		uint32_t g_frames = 0, g_remapped = 0, g_no_map = 0, g_unmatched_frames = 0;
		UINT g_last_remap_frame = 0xFFFFFFFFu;
		float g_nearest_px = 1e30f;   // closest screen-space origin to the cursor, frames without a match
		bool g_frame_matched = false;
		UINT g_frame_seen = 0xFFFFFFFFu;
		std::string g_last_why;

		bool fresh(const Camera& c, UINT frame)
		{
			return c.frame != 0xFFFFFFFFu && frame - c.frame <= kMaxCameraAge;
		}

		bool perspective(const float* p)
		{
			return p[11] == 1.0f && p[15] == 0.0f && p[0] > 1e-4f && p[5] > 1e-4f;
		}

		// The 3x3 rotation part of a row-major D3D matrix, inverted.
		bool invert3(const float* m, float out[9])
		{
			const float a = m[0], b = m[1], c = m[2];
			const float d = m[4], e = m[5], f = m[6];
			const float g = m[8], h = m[9], i = m[10];
			const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
			const float det = a * A + b * B + c * C;
			if (!(std::fabs(det) > 1e-8f)) return false;
			const float s = 1.0f / det;
			out[0] = A * s; out[1] = -(b * i - c * h) * s; out[2] = (b * f - c * e) * s;
			out[3] = B * s; out[4] = (a * i - c * g) * s;  out[5] = -(a * f - c * d) * s;
			out[6] = C * s; out[7] = -(a * h - b * g) * s; out[8] = (a * e - b * d) * s;
			return true;
		}

		// Row vector v times the 3x3 m (row-major, 3 floats per row, stride).
		void mul3(const float v[3], const float* m, int stride, float out[3])
		{
			for (int c = 0; c < 3; c++)
				out[c] = v[0] * m[c] + v[1] * m[stride + c] + v[2] * m[2 * stride + c];
		}

		// The game's cursor NDC -> where its pick point shows in the Remix render.
		bool map_ndc(const float ndc[2], float out[2])
		{
			const float* pp = g_pip.proj;
			const float* pw = g_world.proj;
			const float v[3] = { (ndc[0] - pp[8]) / pp[0], (ndc[1] - pp[9]) / pp[5], 1.0f };
			float inv[9];
			if (!invert3(g_pip.view, inv)) return false;
			float r[3], o[3];
			mul3(v, inv, 3, r);
			mul3(r, g_world.view, 4, o);
			if (!(o[2] > 1e-4f)) return false;
			out[0] = pw[0] * o[0] / o[2] + pw[8];
			out[1] = pw[5] * o[1] / o[2] + pw[9];
			return std::isfinite(out[0]) && std::isfinite(out[1]);
		}

		bool read_cursor(float& x, float& y)
		{
			const uintptr_t slot = fnv_exe::addr().interface_manager;
			uint32_t im = 0;
			if (!slot || !sg::read_u32(slot, im) || !im) return false;
			return sg::read_f32(im + IM_cursorX, x) && sg::read_f32(im + IM_cursorY, y) &&
				std::isfinite(x) && std::isfinite(y);
		}

		// The rotation between the two VIEWs, degrees (for the log).
		float view_angle_deg()
		{
			float dot = 0.0f;
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					dot += g_pip.view[r * 4 + c] * g_world.view[r * 4 + c];
			const float cosv = std::clamp((dot - 1.0f) * 0.5f, -1.0f, 1.0f);
			return std::acos(cosv) * 57.2957795f;
		}

		void compute_map(UINT frame)
		{
			g_map_frame = frame;
			g_map_ok = false;
			if (!fresh(g_world, frame)) { g_map_why = "no world camera sent this frame"; return; }
			if (!fresh(g_pip, frame)) { g_map_why = "no Pip-Boy draw this frame"; return; }
			const float w = static_cast<float>(game::backbuffer_width);
			const float h = static_cast<float>(game::backbuffer_height);
			if (!(w > 0.0f) || !(h > 0.0f)) { g_map_why = "backbuffer size unknown"; return; }
			if (!read_cursor(g_cursor_px[0], g_cursor_px[1])) { g_map_why = "cursor position unreadable"; return; }
			g_ndc[0] = 2.0f * g_cursor_px[0] / w - 1.0f;
			g_ndc[1] = 1.0f - 2.0f * g_cursor_px[1] / h;
			float mapped[2];
			if (!map_ndc(g_ndc, mapped)) { g_map_why = "the cursor's ray misses the world camera"; return; }
			g_delta[0] = mapped[0] - g_ndc[0];
			g_delta[1] = mapped[1] - g_ndc[1];
			g_map_ok = true;
			g_map_why = "";
		}

		bool near1(float v, float want) { return std::fabs(v - want) < 1e-5f; }
	}

	void note_world_camera(const float* view, const float* proj)
	{
		if (!view || !proj || !perspective(proj)) return;
		std::memcpy(g_world.view, view, sizeof(g_world.view));
		std::memcpy(g_world.proj, proj, sizeof(g_world.proj));
		g_world.frame = shared::common::ffp_state::get().frame_count();
	}

	void note_pipboy_draw()
	{
		const float* view = game::get_renderer_matrix(RENDERER_VIEW_OFF);
		const float* proj = game::get_renderer_matrix(RENDERER_PROJ_OFF);
		if (!view || !proj || !perspective(proj)) return;
		std::memcpy(g_pip.view, view, sizeof(g_pip.view));
		std::memcpy(g_pip.proj, proj, sizeof(g_pip.proj));
		g_pip.frame = shared::common::ffp_state::get().frame_count();
	}

	bool before_2d_draw(IDirect3DDevice9* dev)
	{
		g_patched = false;
		if (!g_open) return false;
		auto& ffp = shared::common::ffp_state::get();
		if (!ffp.last_vs()) return false;

		const UINT frame = ffp.frame_count();
		if (g_frame_seen != frame)
		{
			g_frame_seen = frame;
			g_frame_matched = false;
		}
		if (g_map_frame != frame) compute_map(frame);
		if (!g_map_ok) return false;

		// The draw's origin in clip space, through c0-c3.
		const float* c = ffp.vs_const_data();
		const bool t = near1(c[12], 0.0f) && near1(c[13], 0.0f) && near1(c[14], 0.0f) && near1(c[15], 1.0f);
		const bool r = near1(c[3], 0.0f) && near1(c[7], 0.0f) && near1(c[11], 0.0f) && near1(c[15], 1.0f);
		if (t != r) g_layout = t ? Layout::Transposed : Layout::Rows;
		if (!t && !r) return false;                 // not an orthographic ModelViewProj
		if (g_layout == Layout::Unknown) return false;
		const bool tr = g_layout == Layout::Transposed;
		const float ox = tr ? c[3] : c[12];
		const float oy = tr ? c[7] : c[13];

		const float w = static_cast<float>(game::backbuffer_width);
		const float h = static_cast<float>(game::backbuffer_height);
		const float dx = (ox - g_ndc[0]) * 0.5f * w, dy = (oy - g_ndc[1]) * 0.5f * h;
		const float dist = std::sqrt(dx * dx + dy * dy);
		if (dist > kMatchPixels)
		{
			if (!g_frame_matched) g_nearest_px = (std::min)(g_nearest_px, dist);
			return false;
		}

		float m[16];
		std::memcpy(m, c, sizeof(m));
		if (tr) { m[3] += g_delta[0]; m[7] += g_delta[1]; }
		else    { m[12] += g_delta[0]; m[13] += g_delta[1]; }
		if (FAILED(dev->SetVertexShaderConstantF(0, m, 4))) return false;
		g_patched = true;
		g_frame_matched = true;
		if (g_last_remap_frame != frame)
		{
			g_last_remap_frame = frame;
			g_remapped++;
		}

		if (!g_logged_open)
		{
			g_logged_open = true;
			const float mx = (g_ndc[0] + g_delta[0] + 1.0f) * 0.5f * w;
			const float my = (1.0f - (g_ndc[1] + g_delta[1])) * 0.5f * h;
			shared::common::log("PipBoyCursor", std::format(
				"Pip-Boy open: cursor drawn where the game's pick lands -- game pixel ({:.0f}, {:.0f}) -> shown at "
				"({:.0f}, {:.0f}) of {}x{}; pick camera P00 {:.3f} P11 {:.3f}, render camera P00 {:.3f} P11 {:.3f} "
				"(scale {:.3f} x {:.3f}), views {:.2f} deg apart, view translations ({:.2f}, {:.2f}, {:.2f}) / "
				"({:.2f}, {:.2f}, {:.2f}); ModelViewProj {}",
				g_cursor_px[0], g_cursor_px[1], mx, my, game::backbuffer_width, game::backbuffer_height,
				g_pip.proj[0], g_pip.proj[5], g_world.proj[0], g_world.proj[5],
				g_world.proj[0] / g_pip.proj[0], g_world.proj[5] / g_pip.proj[5], view_angle_deg(),
				g_pip.view[12], g_pip.view[13], g_pip.view[14], g_world.view[12], g_world.view[13], g_world.view[14],
				tr ? "transposed" : "rows"), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		return true;
	}

	void after_2d_draw(IDirect3DDevice9* dev)
	{
		if (!g_patched) return;
		g_patched = false;
		// The game's c0-c3, as the proxy last forwarded them.
		dev->SetVertexShaderConstantF(0, shared::common::ffp_state::get().vs_const_data(), 4);
	}

	void on_present()
	{
		const bool open = nvse_link::linked() && nvse_link::facts().pipboy != FNVR_PB_CLOSED;
		if (open && !g_open)
		{
			g_open = true;
			g_logged_open = false;
			g_frames = g_remapped = g_no_map = g_unmatched_frames = 0;
			g_nearest_px = 1e30f;
			g_last_why.clear();
			return;
		}
		if (!open && g_open)
		{
			g_open = false;
			const std::string nearest = g_nearest_px < 1e29f ? std::format("{:.1f} px", g_nearest_px) : std::string("none");
			shared::common::log("PipBoyCursor", std::format(
				"Pip-Boy closed: {} frames, cursor remapped in {}, no mapping in {} (last reason: {}), mapped but no "
				"cursor draw found in {} (closest screen-space origin {})", g_frames, g_remapped, g_no_map,
				g_last_why.empty() ? "none" : g_last_why, g_unmatched_frames, nearest),
				(g_remapped == 0 && g_frames > 0) ? shared::common::LOG_TYPE::LOG_TYPE_WARN
				: shared::common::LOG_TYPE::LOG_TYPE_STATUS);
			return;
		}
		if (!g_open) return;

		// The frame that ends now.
		const UINT frame = shared::common::ffp_state::get().frame_count();
		g_frames++;
		if (g_map_frame != frame || !g_map_ok)
		{
			g_no_map++;
			if (g_map_frame == frame && g_map_why && g_map_why[0]) g_last_why = g_map_why;
			else if (g_map_frame != frame) g_last_why = "no screen-space draw this frame";
		}
		else if (g_last_remap_frame != frame) g_unmatched_frames++;
	}
}
