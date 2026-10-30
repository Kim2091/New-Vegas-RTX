#include "std_include.hpp"
#include "frame_stats.hpp"
#include "api_common.hpp"
#include "api_submit.hpp"
#include "nvse_link.hpp"

/*
 * Per-frame route histogram + RT0 census, logged around game-state changes
 * (the engine monitor's: main menu, loading, in game, menu).
 *
 * A frame whose world renders into a full-size non-backbuffer target (an
 * image-space pass such as DOF reads it back) sends every world draw down
 * PASS_OFFSCREEN_RT. A [Frame] line shows that: the route counts, the
 * distinct RT0 surfaces the frame bound (with sizes, backbuffer identity and
 * the draws each received) and the API instance counts.
 *
 * Cost when nothing is logged: one cached-pointer compare per draw (the tag
 * table is searched only when the tag changes) and the cached facts per
 * Present.
 */

namespace comp::game::frame_stats
{
	namespace
	{
		constexpr int  MAX_ROUTES          = 48;
		constexpr int  MAX_TARGETS         = 16;
		constexpr int  FRAMES_AFTER_CHANGE = 3;
		// Rate limit: a game-state change spends one burst (the previous frame +
		// the change frame + FRAMES_AFTER_CHANGE). Bursts refill one per
		// BURST_REFILL_FRAMES, up to MAX_BURSTS.
		constexpr int  MAX_BURSTS          = 8;
		constexpr UINT BURST_REFILL_FRAMES = 600;

		struct Route
		{
			const char* tag;
			uint32_t n;
		};

		struct Target
		{
			IDirect3DSurface9* surf;   // identity only, never dereferenced after the bind
			UINT w, h;
			bool backbuffer;           // the swap-chain backbuffer itself
			bool onscreen;             // what the wrapper routed it as (rendering_to_backbuffer)
			uint32_t binds;
			uint32_t draws;
		};

		struct Frame
		{
			Route    routes[MAX_ROUTES];
			int      route_n;
			uint32_t route_overflow;
			Target   targets[MAX_TARGETS];
			int      target_n;
			uint32_t target_overflow;
			uint32_t draws_onscreen;
			uint32_t draws_offscreen;
		};

		Frame g_cur = {};
		Frame g_prev = {};
		int   g_last_route = -1;
		int   g_cur_target = -1;

		bool     g_have_mode = false;
		uint32_t g_mode = 0;
		int      g_log_frames_left = 0;
		int      g_since_change = 0;
		int      g_bursts = MAX_BURSTS;
		UINT     g_refill_tick = 0;
		bool     g_logged_limit = false;
		UINT     g_frame = 0;

		const char* mode_name(uint32_t mode)
		{
			switch (mode)
			{
			case FNVR_GS_MAIN_MENU: return "main menu";
			case FNVR_GS_LOADING: return "loading";
			case FNVR_GS_IN_GAME: return "game";
			case FNVR_GS_MENU: return "menu";
			default: return "?";
			}
		}

		std::string format_frame(const Frame& f)
		{
			std::string s = std::format("draws onscreen={} offscreen={} | RT0 {} target{}:",
				f.draws_onscreen, f.draws_offscreen, f.target_n, f.target_n == 1 ? "" : "s");
			for (int i = 0; i < f.target_n; ++i)
			{
				const auto& t = f.targets[i];
				s += std::format(" [{}{}x{} binds={} draws={}{}]",
					t.backbuffer ? "BACKBUFFER " : "", t.w, t.h, t.binds, t.draws,
					!t.backbuffer && t.onscreen ? " routed-onscreen" : "");
			}
			if (f.target_overflow) s += std::format(" (+{} binds past the table)", f.target_overflow);

			s += " | routes:";
			if (!f.route_n) s += " none";
			for (int i = 0; i < f.route_n; ++i)
				s += std::format(" {}={}", f.routes[i].tag, f.routes[i].n);
			if (f.route_overflow) s += std::format(" (+{} untabled)", f.route_overflow);
			return s;
		}

		std::string format_api()
		{
			const auto& t = api::terrain_stats();
			const auto& l = api::far_lod_stats();
			const auto& w = api::water_stats();
			return std::format("api instances terrain={} farlod={} water={} (fallbacks {}/{}/{}, skipped passes {}/{}/{})",
				t.draws_last, l.draws_last, w.draws_last,
				t.fallbacks_last, l.fallbacks_last, w.fallbacks_last,
				t.skipped_last, l.skipped_last, w.skipped_last);
		}

		void start_frame()
		{
			// The target bound across Present is still bound: carry it over so
			// the next frame's draws are attributed before its first bind.
			Target carried = {};
			const bool carry = g_cur_target >= 0;
			if (carry)
			{
				carried = g_cur.targets[g_cur_target];
				carried.binds = 0;
				carried.draws = 0;
			}

			g_cur.route_n = 0;
			g_cur.route_overflow = 0;
			g_cur.target_n = 0;
			g_cur.target_overflow = 0;
			g_cur.draws_onscreen = 0;
			g_cur.draws_offscreen = 0;
			g_last_route = -1;
			g_cur_target = -1;

			if (carry)
			{
				g_cur.targets[0] = carried;
				g_cur.target_n = 1;
				g_cur_target = 0;
			}
		}
	}

	namespace
	{
		const char* g_draw_route = nullptr;
	}

	const char* take_route()
	{
		return std::exchange(g_draw_route, nullptr);
	}

	void note_route(const char* tag)
	{
		api_submit::terrain_note_route(tag);
		g_draw_route = tag;
		if (rendering_to_backbuffer) ++g_cur.draws_onscreen;
		else ++g_cur.draws_offscreen;
		if (g_cur_target >= 0) ++g_cur.targets[g_cur_target].draws;

		if (!tag) return;
		if (g_last_route >= 0 && g_cur.routes[g_last_route].tag == tag)
		{
			++g_cur.routes[g_last_route].n;
			return;
		}
		for (int i = 0; i < g_cur.route_n; ++i)
		{
			if (g_cur.routes[i].tag == tag || std::strcmp(g_cur.routes[i].tag, tag) == 0)
			{
				g_last_route = i;
				++g_cur.routes[i].n;
				return;
			}
		}
		if (g_cur.route_n >= MAX_ROUTES)
		{
			++g_cur.route_overflow;
			return;
		}
		g_last_route = g_cur.route_n;
		g_cur.routes[g_cur.route_n++] = { tag, 1u };
	}

	void note_render_target(IDirect3DSurface9* surface)
	{
		for (int i = 0; i < g_cur.target_n; ++i)
		{
			auto& t = g_cur.targets[i];
			if (t.surf != surface) continue;
			++t.binds;
			t.onscreen = rendering_to_backbuffer;
			g_cur_target = i;
			return;
		}
		if (g_cur.target_n >= MAX_TARGETS)
		{
			++g_cur.target_overflow;
			g_cur_target = -1;
			return;
		}

		Target t = {};
		t.surf = surface;
		D3DSURFACE_DESC d;
		if (surface && SUCCEEDED(surface->GetDesc(&d))) { t.w = d.Width; t.h = d.Height; }
		t.backbuffer = surface && surface == backbuffer_surface;
		t.onscreen = rendering_to_backbuffer;
		t.binds = 1;
		g_cur_target = g_cur.target_n;
		g_cur.targets[g_cur.target_n++] = t;
	}

	void on_present()
	{
		++g_frame;
		if (g_bursts < MAX_BURSTS && ++g_refill_tick >= BURST_REFILL_FRAMES)
		{
			g_refill_tick = 0;
			++g_bursts;
		}

		if (nvse_link::has(FNVR_F_MENU))
		{
			const uint32_t mode = nvse_link::facts().game_state;
			if (g_have_mode && mode != g_mode && shared::common::log_verbose())
			{
				if (g_bursts > 0)
				{
					--g_bursts;
					g_logged_limit = false;
					shared::common::log("Frame", std::format(
						"game state {} ({}) -> {} ({}) at frame {} -- route histogram for the previous frame, this one and the next {}",
						g_mode, mode_name(g_mode), mode, mode_name(mode), g_frame, FRAMES_AFTER_CHANGE),
						shared::common::LOG_TYPE::LOG_TYPE_STATUS);
					shared::common::log("Frame", std::format("  f{} (before) {}", g_frame - 1, format_frame(g_prev)),
						shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
					g_log_frames_left = 1 + FRAMES_AFTER_CHANGE;
					g_since_change = 0;
				}
				else if (!g_logged_limit)
				{
					g_logged_limit = true;
					shared::common::log("Frame", std::format(
						"game state {} -> {} at frame {}: histogram rate limit reached, not logged (one burst per {} frames)",
						g_mode, mode, g_frame, BURST_REFILL_FRAMES),
						shared::common::LOG_TYPE::LOG_TYPE_STATUS);
				}
			}
			g_mode = mode;
			g_have_mode = true;
		}

		if (g_log_frames_left > 0)
		{
			--g_log_frames_left;
			shared::common::log("Frame", std::format("  f{} (+{}, state {}) {} | {}",
				g_frame, g_since_change++, g_mode, format_frame(g_cur), format_api()),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
		}

		g_prev = g_cur;
		start_frame();
	}

	void on_reset()
	{
		g_cur_target = -1;
		start_frame();
		g_prev = {};
	}
}
