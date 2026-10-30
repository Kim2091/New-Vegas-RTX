#include "std_include.hpp"
#include "menu_bg.hpp"
#include "nvse_link.hpp"
#include "save_thumbnail.hpp"
#include "call_chain.hpp"
#include "scene_graph.hpp"
#include "menu_scene.hpp"
#include "shared/fnv_exe.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/remix_api.hpp"

/*
 * Menu backgrounds. See menu_bg.hpp for the overview.
 *
 * Engine facts this relies on (FalloutNV.exe 1.4.0.525, static analysis):
 *
 *  The menu state comes from the engine monitor plugin (nvse_link facts):
 *  menu mode (game_state MENU), the top of InterfaceManager's open-menu stack
 *  (top_menu) and InterfaceManager+0x4BC, the Pip-Boy's 0-5 state machine
 *  (0x70EE80: closed, open requested, opening, open, close requested,
 *  closing, then closed) as FnvRemixPipBoy CLOSED / OPENING (1, 2) /
 *  OPEN (3) / CLOSING (4, 5). FNVR_F_MENU_BG says the capture code below is
 *  vanilla; without it the Pip-Boy's live background stays off.
 *
 *  0x871DC0       the capture: the world rendered from the player camera
 *                 (1st-person pass skipped) into render-target type 6, then
 *                 0x011DEA29 = 1 (0x8722E9).
 *
 *  0x011D890A u8  the Pip-Boy's capture request. Set to 1 only by the
 *                 Pip-Boy-open handler (0x7F805E). Main::Render 0x86FF70 reads
 *                 it at its very top, clears it and runs the capture. Writing
 *                 it every frame re-runs the engine's own capture.
 *
 *  0x011DEA29 u8  "menu background captured". Every other menu's capture is
 *                 run once by 0x86F450 (from the frame loop, before
 *                 Main::Render) while in menu mode with this byte 0, unless
 *                 DialogMenu is on top, VATS or a message box is open or the
 *                 Pip-Boy is up. Main::Render then pastes the capture instead
 *                 of rendering the world (0x870187: 1 = frozen paste, 0 = live
 *                 world), draws the menus that need it (Render3DMenuScene, the
 *                 rendered terminals) and keeps the capture until menu mode
 *                 ends, when 0x86F450 releases the target (0x877430) and
 *                 clears the byte.
 *
 *  0x011F3330     the interface's lock (CRITICAL_SECTION). The capture holds
 *                 it from 0x871E71 to 0x872320, around its world render.
 *
 *  0x871A50       the Pip-Boy screen render, called from 0x871290 (0x8719F8)
 *                 on every world render. With the Pip-Boy up it first waits
 *                 for the frame's interface update (0x7058E0 -> 0x713C70), which
 *                 the AI linear task thread runs alongside Main::Render and
 *                 which takes the interface's lock to build tiles (the HUD's
 *                 compass markers, for one). Inside the capture that wait can
 *                 never end. The guard on 0x8719F8 skips the screen render
 *                 while the calling thread holds the lock; every path of
 *                 Main::Render renders the screen again after the capture,
 *                 outside the lock. Without the guard no Pip-Boy session starts.
 *
 * The Pip-Boy. A session runs while the Pip-Boy opens, is open (or closes,
 * [MenuBackground] KeepDuringClose) and re-arms the request byte, so the
 * engine captures every frame and the capture is routed as on-screen world
 * geometry. Every wait on the engine is capped: a request never consumed, or
 * captures no paste draw matches, within WATCHDOG_FRAMES end the re-runs for
 * that session.
 *
 * Other menus. The engine's single capture stays, and its texture gets the
 * runtime's thumbnail of a recent path-traced frame. Once the engine has
 * drawn into the capture (its surface bound at SetRenderTarget), the first
 * draw that samples it (the menu's image-space pass, in the capture's own
 * frame) is preceded by a fill with the newest periodic thumbnail
 * (save_thumbnail's refresh). The thumbnail is uploaded to a plain surface
 * and stretched by the GPU into the top level of the capture's own texture,
 * which is found from *0x011DED3C (capture_texture). The paste draws the
 * menu's image-space effects from that texture into another one every
 * frame, so the capture itself is what gets filled. Without a current
 * thumbnail the paste shows the engine's own capture, which under Remix is
 * nearly empty. Menu frames are rasterised (menu_scene.hpp), so the runtime
 * writes no new thumbnail while the menu is open.
 *
 * D3D9-level mechanics of the Pip-Boy session:
 *
 *  - The capture pass is bounded by the byte armed at Present: the request
 *    reads 0 once consumed. The pass ends when the engine binds the
 *    backbuffer again. Only that window is ever redirected; local-map tiles
 *    and every other off-screen pass keep their normal routing.
 *
 *  - The paste is MenuBackground_Blit 0x874B90: the image-space copy effect
 *    0x22 from the type-6 target (*0x011DED3C) into the target Main::Render
 *    has bound.
 *
 *  - Inside the window the menu-background RT is identified by pointer
 *    identity, learned from the paste: the first backbuffer draw after the
 *    window whose stage-0 texture is the container of a screen-sized target
 *    bound inside it. That draw is the frozen-background paste; it is dropped
 *    from then on, and the next frames' capture into that target is routed
 *    as on-screen.
 *
 *  - dxvk-remix treats a draw as primary when its RT has the backbuffer's
 *    size (isRenderTargetPrimary). A same-sized menu RT therefore only needs
 *    the wrapper's rendering_to_backbuffer flag set; the game's own binding is
 *    left untouched. A differently-sized one gets the real backbuffer bound in
 *    its place (plus the gameplay depth-stencil if the game's is too small).
 *
 * No surface or texture is ever AddRef'd past the call that produced it
 * (D3DPOOL_DEFAULT references would make Reset fail); they are compared by
 * address only and forgotten on reset and when a session or capture ends.
 */

namespace comp::game::menu_bg
{
	namespace
	{
		constexpr uint32_t  PS_ISMENUBG            = 0xB546C4B6u;

		constexpr int MAX_CANDIDATES   = 8;
		constexpr int MAX_DIAG_TEXTURES = 16;
		// The cap on every wait for the engine or the runtime.
		constexpr UINT WATCHDOG_FRAMES  = 120;

		// The engine's two capture bytes (fnv_exe menu_capture_request /
		// menu_bg_captured); a local stand-in when the executable is unknown.
		volatile uint8_t g_no_engine_byte = 0;
		volatile uint8_t& engine_byte(uintptr_t addr)
		{
			return addr ? *reinterpret_cast<volatile uint8_t*>(addr) : g_no_engine_byte;
		}
		volatile uint8_t& request_byte()  { return engine_byte(fnv_exe::addr().menu_capture_request); }
		volatile uint8_t& captured_byte() { return engine_byte(fnv_exe::addr().menu_bg_captured); }

		// The capture guard (file comment). g_screen_render: what 0x8719F8
		// called before the guard. Written once at install; the counter only
		// on the engine's render thread, which also calls Present.
		using ScreenRender = void(__thiscall*)(void* main);
		ScreenRender g_screen_render = nullptr;
		bool g_guard = false;
		UINT g_screen_skips = 0;

		constexpr const char* kGuardIfReplaced = "If that code does not call what it replaced, the live background's "
			"capture can wait for the interface update while holding the interface's lock and freeze the game with the "
			"Pip-Boy up; set [MenuBackground] LiveBackground=0 then.";

		bool interface_lock_held_here()
		{
			const uintptr_t lock = fnv_exe::addr().interface_lock;
			if (!lock) return false;
			const HANDLE owner = reinterpret_cast<const volatile CRITICAL_SECTION*>(lock)->OwningThread;
			return owner == reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentThreadId()));
		}

		// Called by 0x8719F8 in place of the screen render (thiscall: the
		// object in ECX, no stack arguments, no result used).
		void __fastcall pipboy_screen_guard(void* main, void*)
		{
			if (interface_lock_held_here())
			{
				++g_screen_skips;
				return;
			}
			g_screen_render(main);
		}

		enum class Window { Idle, Capture, Done };

		struct Candidate
		{
			IDirect3DSurface9*     surf = nullptr;  // target bound in the capture
			IDirect3DBaseTexture9* tex  = nullptr;  // texture the paste would sample
			UINT w = 0, h = 0;
		};

		// --- Pip-Boy session state ---
		bool    g_session     = false;          // sampled at Present
		bool    g_active      = false;          // the session runs and has not given up
		bool    g_req_pending = false;          // the capture was armed at the last Present
		Window  g_window      = Window::Idle;
		bool    g_window_saw_offscreen = false;

		Candidate g_cand[MAX_CANDIDATES];
		int       g_cand_n = 0;

		// Learned menu-background target (identity only; see file comment).
		IDirect3DSurface9*     g_menu_rt  = nullptr;
		IDirect3DBaseTexture9* g_menu_tex = nullptr;
		UINT g_menu_w = 0, g_menu_h = 0;

		// --- binding state ---
		IDirect3DSurface9* g_game_rt0 = nullptr;  // what the game last asked for on RT0
		IDirect3DSurface9* g_game_ds  = nullptr;  // what the game last asked for
		IDirect3DSurface9* g_bb_ds    = nullptr;  // depth-stencil the game pairs with the backbuffer
		bool g_redirect_in_place = false;         // RT0 is the menu RT, routed as on-screen
		bool g_substituted       = false;         // RT0 is the backbuffer standing in for it
		bool g_ds_substituted    = false;         // DS is g_bb_ds standing in for g_game_ds

		// --- Pip-Boy session stats / one-shot logs ---
		UINT g_frames = 0, g_captures = 0, g_redirects = 0, g_pastes = 0, g_tints = 0;
		UINT g_skips_at_begin = 0;
		bool g_logged_capture = false, g_logged_redirect = false, g_logged_paste = false;
		bool g_logged_tint = false, g_logged_no_redirect = false;
		bool g_logged_substitute_fail = false;
		uint32_t g_last_pip_mode = 0xFFFFFFFFu;
		int g_mode_logs = 0;
		bool g_capture_ok = true;  // FNVR_F_MENU_BG as last logged

		// Diagnostic: RT-textured backbuffer draws seen while nothing is learned.
		const void* g_diag_seen[MAX_DIAG_TEXTURES] = {};
		int g_diag_n = 0;

		// --- frozen menu background (other menus) ---
		bool g_from_game = false;                  // the last state before menu mode was in game
		bool g_logged_capture_texture = false;
		IDirect3DBaseTexture9* g_cap_tex = nullptr; // the capture texture, by address
		bool g_cap_dirty = false;                  // the engine drew into the capture since it was last filled
		bool g_width_set = false;                  // rtx.screenshot.thumbnailWidth sent

		const auto& cfg() { return shared::common::config::get().menu_bg; }

		bool surface_size(IDirect3DSurface9* s, UINT& w, UINT& h)
		{
			D3DSURFACE_DESC d;
			if (!s || FAILED(s->GetDesc(&d))) return false;
			w = d.Width; h = d.Height;
			return true;
		}

		// Texture whose level this surface is, or null for a plain surface.
		// The bridge answers GetContainer locally (parent QueryInterface).
		IDirect3DBaseTexture9* container_texture(IDirect3DSurface9* s)
		{
			IDirect3DTexture9* t = nullptr;
			if (!s || FAILED(s->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&t))) || !t)
				return nullptr;
			t->Release();  // the game holds its own reference; identity only
			return t;
		}

		bool is_backbuffer(IDirect3DSurface9* s)
		{
			return s && backbuffer_surface && s == backbuffer_surface;
		}

		// The capture pass is over once the backbuffer is bound again after it
		// drew off-screen.
		bool capture_finished(IDirect3DSurface9* next)
		{
			return g_window_saw_offscreen && is_backbuffer(next);
		}

		void add_candidate(IDirect3DSurface9* surf, IDirect3DBaseTexture9* tex)
		{
			if (!surf || !tex) return;
			for (int i = 0; i < g_cand_n; ++i)
				if (g_cand[i].surf == surf && g_cand[i].tex == tex) return;
			if (g_cand_n >= MAX_CANDIDATES) return;
			auto& c = g_cand[g_cand_n++];
			c.surf = surf; c.tex = tex;
			if (!surface_size(surf, c.w, c.h)) { c.w = 0; c.h = 0; }
		}

		// The menu background is captured at (or near) screen resolution.
		// Local-map tiles (the other world renders a Pip-Boy frame can contain,
		// sampled by 2D backbuffer draws on the map page) are far smaller, so a
		// failed capture is never learned as a tile.
		bool screen_sized(UINT w, UINT h)
		{
			return w * 2 >= backbuffer_width && h * 2 >= backbuffer_height;
		}

		void forget_targets()
		{
			g_menu_rt = nullptr; g_menu_tex = nullptr; g_menu_w = g_menu_h = 0;
			g_cand_n = 0;
			g_diag_n = 0;
		}

		void reset_session_stats()
		{
			g_frames = g_captures = g_redirects = g_pastes = g_tints = 0;
			g_skips_at_begin = g_screen_skips;
			g_logged_capture = g_logged_redirect = g_logged_paste = false;
			g_logged_tint = g_logged_no_redirect = false;
			g_logged_substitute_fail = false;
		}

		bool ds_too_small_for_backbuffer(IDirect3DSurface9* ds)
		{
			if (!ds) return false;  // no depth: always valid
			UINT w, h;
			if (!surface_size(ds, w, h)) return false;
			return w < backbuffer_width || h < backbuffer_height;
		}

		// With the backbuffer standing in for a smaller menu RT, the game's
		// depth-stencil may be smaller than the colour target, which D3D9
		// rejects. Bind the gameplay one instead, and give the game its own back
		// the moment the stand-in ends (its renderer caches the bound DS).
		void reconcile_depth(IDirect3DDevice9* dev)
		{
			const bool want_sub = g_substituted && g_bb_ds && g_game_ds != g_bb_ds
				&& ds_too_small_for_backbuffer(g_game_ds);
			if (want_sub == g_ds_substituted) return;
			dev->SetDepthStencilSurface(want_sub ? g_bb_ds : g_game_ds);
			g_ds_substituted = want_sub;
		}

		void log_learned(const Candidate& c, uint32_t ps_hash)
		{
			shared::common::log("MenuBg", std::format(
				"learned the Pip-Boy background RT {}x{} (backbuffer {}x{}) from the paste draw (PS 0x{:08X}, 2D={}) -- "
				"that draw is skipped from now on; the next capture is routed as world geometry",
				c.w, c.h, backbuffer_width, backbuffer_height, ps_hash, is_2d() ? 1 : 0),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN);
		}

		void diag_rt_texture_draw(IDirect3DBaseTexture9* tex, uint32_t ps_hash)
		{
			if (g_diag_n >= MAX_DIAG_TEXTURES) return;
			for (int i = 0; i < g_diag_n; ++i)
				if (g_diag_seen[i] == tex) return;
			g_diag_seen[g_diag_n++] = tex;

			if (tex->GetType() != D3DRTYPE_TEXTURE) return;
			D3DSURFACE_DESC d;
			if (FAILED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d))) return;
			if (!(d.Usage & D3DUSAGE_RENDERTARGET)) return;

			shared::common::log("MenuBg", std::format(
				"backbuffer draw samples a {}x{} render-target texture (PS 0x{:08X}, 2D={}) that was not bound in the "
				"capture pass ({} capture targets seen) -- not treated as the paste",
				d.Width, d.Height, ps_hash, is_2d() ? 1 : 0, g_cand_n),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}

		// Undo whatever this session armed for the next frame.
		void disarm()
		{
			if (g_req_pending && g_session && request_byte() == 1)
				request_byte() = 0;
			g_req_pending = false;
		}

		void give_up(const std::string& why)
		{
			shared::common::log("MenuBg", std::format("{} -- the engine's own background for the rest of this Pip-Boy "
				"session", why), shared::common::LOG_TYPE::LOG_TYPE_WARN);
			disarm();
			g_active = false;
		}

		void end_session(int pip_raw)
		{
			disarm();
			shared::common::log("MenuBg", std::format(
				"Pip-Boy closed (state {}) -- live background OFF. {} frames, {} captures, {} redirected, {} paste quads, "
				"{} tint passes and {} Pip-Boy screen renders inside the capture skipped",
				pip_raw, g_frames, g_captures, g_redirects, g_pastes, g_tints, g_screen_skips - g_skips_at_begin),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
			g_session = false;
			g_active = false;
			// The engine frees the background target shortly after a close; its
			// address may be recycled, so learn it afresh next time.
			forget_targets();
		}

		void begin_session(int pip_raw)
		{
			g_session = true;
			g_active = true;
			reset_session_stats();
			forget_targets();
			shared::common::log("MenuBg", std::format(
				"Pip-Boy open (state {}) -- live background ON: re-running the engine's menu-background capture every "
				"frame", pip_raw), shared::common::LOG_TYPE::LOG_TYPE_GREEN);
		}

		// --- frozen menu background ---

		// The D3D texture of the capture: *0x011DED3C is a BSRenderedTexture,
		// and the NiRenderedTexture among its first members holds it. That
		// member is found by its class, not by a fixed offset.
		IDirect3DTexture9* capture_texture(std::string& why)
		{
			namespace sg = scene_graph;
			uint32_t rendered = 0;
			if (!fnv_exe::addr().menu_bg_rt || !sg::read_u32(fnv_exe::addr().menu_bg_rt, rendered) || !rendered)
			{
				why = "the engine has no capture target";
				return nullptr;
			}
			for (uint32_t off = 4; off <= 0x40; off += 4)
			{
				uint32_t member = 0;
				if (!sg::read_u32(rendered + off, member) || !member) continue;
				const sg::ClassInfo* ci = sg::class_of(member);
				if (!ci || ci->name != "NiRenderedTexture") continue;
				bool has_data = false;
				IDirect3DBaseTexture9* base = sg::d3d_texture(member, has_data);
				if (!base || base->GetType() != D3DRTYPE_TEXTURE)
				{
					why = "the capture target has no D3D texture";
					return nullptr;
				}
				auto* tex = static_cast<IDirect3DTexture9*>(base);
				D3DSURFACE_DESC d;
				if (FAILED(tex->GetLevelDesc(0, &d)) || !(d.Usage & D3DUSAGE_RENDERTARGET))
				{
					why = "the capture target's texture is not a render target";
					return nullptr;
				}
				if (!g_logged_capture_texture)
				{
					g_logged_capture_texture = true;
					shared::common::log("MenuBg", std::format("menu background capture: {}x{} texture (format {}), "
						"NiRenderedTexture at BSRenderedTexture+0x{:X}", d.Width, d.Height, static_cast<int>(d.Format), off),
						shared::common::LOG_TYPE::LOG_TYPE_STATUS);
				}
				return tex;
			}
			why = "the capture target has no NiRenderedTexture";
			return nullptr;
		}

		// The thumbnail is uploaded to a plain surface of its own size, which
		// the GPU stretches (and converts) into the capture texture's top level.
		bool fill_backdrop(IDirect3DDevice9* dev, const save_thumbnail::Thumbnail& t, std::string& why)
		{
			IDirect3DTexture9* tex = capture_texture(why);
			if (!tex) return false;
			D3DSURFACE_DESC d;
			if (FAILED(tex->GetLevelDesc(0, &d))) { why = "the capture texture has no top level"; return false; }

			IDirect3DSurface9* upload = nullptr;
			IDirect3DSurface9* plain = nullptr;
			IDirect3DSurface9* level = nullptr;
			bool ok = false;
			if (FAILED(dev->CreateOffscreenPlainSurface(t.width, t.height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &upload, nullptr)))
				why = "creating the upload surface failed";
			else if (FAILED(dev->CreateOffscreenPlainSurface(t.width, t.height, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &plain, nullptr)))
				why = "creating the plain surface failed";
			else if (FAILED(tex->GetSurfaceLevel(0, &level)))
				why = "the capture texture's top level is not available";
			else
			{
				D3DLOCKED_RECT lr;
				if (FAILED(upload->LockRect(&lr, nullptr, 0)))
					why = "locking the upload surface failed";
				else
				{
					for (uint32_t y = 0; y < t.height; y++)
					{
						const uint8_t* src = t.rgb.data() + static_cast<size_t>(y) * t.width * 3;
						uint8_t* dst = static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
						for (uint32_t x = 0; x < t.width; x++, src += 3, dst += 4)
						{
							dst[0] = src[2];
							dst[1] = src[1];
							dst[2] = src[0];
							dst[3] = 0xFF;
						}
					}
					upload->UnlockRect();
					if (FAILED(dev->UpdateSurface(upload, nullptr, plain, nullptr)))
						why = "uploading the thumbnail failed";
					else if (FAILED(dev->StretchRect(plain, nullptr, level, nullptr, D3DTEXF_LINEAR)))
						why = std::format("stretching it into the {}x{} capture (format {}) failed", d.Width, d.Height,
							static_cast<int>(d.Format));
					else
						ok = true;
				}
			}
			if (level) level->Release();
			if (plain) plain->Release();
			if (upload) upload->Release();
			return ok;
		}

		// Present: thumbnails of frames before menu mode show the place the
		// menu opened in; from the main menu or a loading screen they would
		// show the last session.
		void backdrop_present(const FnvRemixFacts& f, bool menu_ok)
		{
			if (menu_ok && f.game_state == FNVR_GS_IN_GAME) g_from_game = f.ready && !f.loading;
			else if (menu_ok && f.game_state != FNVR_GS_MENU) g_from_game = false;
		}

		// Every SetRenderTarget outside a Pip-Boy session: binding the
		// capture's own surface means the engine is about to draw into it.
		void note_capture_target(IDirect3DSurface9* surface)
		{
			if (g_session || !surface || is_backbuffer(surface)) return;
			std::string why;
			IDirect3DTexture9* tex = capture_texture(why);
			if (!tex || container_texture(surface) != tex) return;
			g_cap_tex = tex;
			g_cap_dirty = true;
		}

		// Every draw: the first draw since the engine drew into the capture
		// that samples it (the menu's image-space pass, which runs in the
		// capture's own frame) gets the newest periodic thumbnail in the
		// capture first, so the menu never shows the empty capture.
		void fill_before_sample(IDirect3DDevice9* dev, IDirect3DBaseTexture9* tex)
		{
			if (!g_cap_tex || tex != g_cap_tex || !g_cap_dirty || g_session) return;
			g_cap_dirty = false;
			if (!g_from_game || !nvse_link::has(FNVR_F_MENU)) return;
			// RaceSexMenu's capture also draws the face preview (0x871DC0 with
			// menu 1036 open), which the thumbnail would cover.
			const FnvRemixFacts& f = nvse_link::facts();
			if (f.pipboy != FNVR_PB_CLOSED || f.top_menu == 1036) return;
			save_thumbnail::Thumbnail t;
			std::string why;
			bool ok = save_thumbnail::read_thumbnail(t, why);
			if (ok && !save_thumbnail::is_current(t))
			{
				why = "the latest thumbnail is from before the last load or cell change";
				ok = false;
			}
			if (ok && fill_backdrop(dev, t, why))
			{
				shared::common::log("MenuBg", std::format("menu background: the frozen capture shows thumbnail #{} ({}x{}) "
					"of the last path-traced frame", t.request, t.width, t.height), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
				return;
			}
			shared::common::log("MenuBg", std::format("menu background left as the engine's capture: {}", why),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		// The thumbnails are as wide as half the screen: the frozen
		// background is shown under the menu's tint, so that is sharp enough.
		void send_thumbnail_width()
		{
			if (g_width_set || !backbuffer_width || !shared::common::remix_api::is_initialized()) return;
			auto& api = shared::common::remix_api::get();
			if (!api.m_bridge.SetConfigVariable) return;
			const UINT width = std::clamp(backbuffer_width / 2, 512u, 1920u);
			api.m_bridge.SetConfigVariable("rtx.screenshot.thumbnailWidth", std::to_string(width).c_str());
			g_width_set = true;
		}
	}


	void install()
	{
		if (!fnv_exe::known() || !cfg().live_background) return;
		const fnv_exe::Code& site = fnv_exe::addr().pipboy_screen_call;
		constexpr const char* what = "the world render's call to the Pip-Boy screen render";
		const call_chain::Site found = call_chain::inspect(site);
		call_chain::log_site("MenuBg", site, what, found);
		if (found.state == call_chain::State::Foreign)
		{
			shared::common::log("MenuBg", "live background OFF: the capture guard cannot be installed",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		g_screen_render = reinterpret_cast<ScreenRender>(found.target);
		const auto guard = reinterpret_cast<uintptr_t>(&pipboy_screen_guard);
		if (!call_chain::patch_rel32(site.addr, 0xE8, guard))
		{
			shared::common::log("MenuBg", std::format("live background OFF: 0x{:X} could not be made writable",
				site.addr), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		call_chain::track(call_chain::Kind::Call, site.addr, guard, found, "MenuBg", what, kGuardIfReplaced);
		g_guard = true;
		shared::common::log("MenuBg", std::format("capture guard on @0x{:X}: the Pip-Boy screen render is skipped "
			"while its thread holds the interface's lock", site.addr), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
	}

	IDirect3DSurface9* filter_render_target(IDirect3DDevice9* dev, DWORD idx, IDirect3DSurface9* surface)
	{
		if (idx != 0) return surface;

		note_capture_target(surface);

		g_game_rt0 = surface;
		g_redirect_in_place = false;
		const bool was_substituted = g_substituted;
		g_substituted = false;

		// Gameplay pairing, used as the stand-in depth for a smaller menu RT.
		if (is_backbuffer(surface) && !was_substituted && g_game_ds)
			g_bb_ds = g_game_ds;

		if (!g_active)
		{
			if (was_substituted) reconcile_depth(dev);
			return surface;
		}

		// The armed capture is next: Main::Render consumed the request.
		if (g_req_pending && request_byte() == 0)
		{
			g_req_pending = false;
			g_window = Window::Capture;
			g_window_saw_offscreen = false;
			g_cand_n = 0;
			++g_captures;
			if (!g_logged_capture)
			{
				g_logged_capture = true;
				shared::common::log("MenuBg", "Pip-Boy capture armed and running -- per-frame capture is on",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS);
			}
		}

		IDirect3DSurface9* bind = surface;

		if (g_window == Window::Capture)
		{
			if (capture_finished(surface))
			{
				g_window = Window::Done;
			}
			else if (surface && !is_backbuffer(surface))
			{
				g_window_saw_offscreen = true;
				add_candidate(surface, container_texture(surface));

				if (g_menu_rt && surface == g_menu_rt)
				{
					const bool same_size = g_menu_w == backbuffer_width && g_menu_h == backbuffer_height;
					const int  mode = cfg().redirect_mode;
					const bool substitute = mode == 2 || (mode == 1 && !same_size);

					if (substitute && backbuffer_surface)
					{
						g_substituted = true;
						bind = backbuffer_surface;
					}
					else if (!substitute && same_size)
					{
						g_redirect_in_place = true;
					}

					if (g_substituted || g_redirect_in_place)
					{
						++g_redirects;
						if (!g_logged_redirect)
						{
							g_logged_redirect = true;
							UINT dw = 0, dh = 0;
							const bool have_ds = surface_size(g_game_ds, dw, dh);
							shared::common::log("MenuBg", std::format(
								"capture pass redirected: Pip-Boy background RT {}x{} (backbuffer {}x{}) {}; "
								"capture depth-stencil {}x{} ({})",
								g_menu_w, g_menu_h, backbuffer_width, backbuffer_height,
								g_substituted ? "-> real backbuffer bound in its place"
								              : "-> routed in place as on-screen world geometry",
								have_ds ? dw : 0u, have_ds ? dh : 0u,
								!g_game_ds ? "none bound" : (g_game_ds == g_bb_ds ? "the gameplay one" : "NOT the gameplay one")),
								shared::common::LOG_TYPE::LOG_TYPE_GREEN);
						}
					}
					else if (substitute && !g_logged_substitute_fail)
					{
						g_logged_substitute_fail = true;
						shared::common::log("MenuBg", "capture pass NOT redirected: no backbuffer surface to stand in",
							shared::common::LOG_TYPE::LOG_TYPE_WARN);
					}
					else if (!g_logged_no_redirect)
					{
						g_logged_no_redirect = true;
						shared::common::log("MenuBg", std::format(
							"capture pass NOT redirected: Pip-Boy background RT is {}x{}, backbuffer {}x{}, and "
							"RedirectMode=0 only redirects same-sized targets -- set [MenuBackground] RedirectMode=1",
							g_menu_w, g_menu_h, backbuffer_width, backbuffer_height),
							shared::common::LOG_TYPE::LOG_TYPE_WARN);
					}
				}
			}
		}

		if (g_substituted || was_substituted) reconcile_depth(dev);
		return bind;
	}

	void post_render_target(DWORD idx)
	{
		if (idx == 0 && g_redirect_in_place)
			rendering_to_backbuffer = true;
	}

	IDirect3DSurface9* filter_depth_stencil(IDirect3DSurface9* ds)
	{
		g_game_ds = ds;
		if (is_backbuffer(g_game_rt0) && !g_substituted)
			g_bb_ds = ds;

		if (g_substituted && g_bb_ds && ds != g_bb_ds && ds_too_small_for_backbuffer(ds))
		{
			g_ds_substituted = true;
			return g_bb_ds;
		}
		g_ds_substituted = false;
		return ds;
	}

	void on_stretch_rect(IDirect3DSurface9* src, IDirect3DSurface9* dst)
	{
		if (!g_active || g_window != Window::Capture || !src || !dst || is_backbuffer(src))
			return;
		add_candidate(src, container_texture(dst));
	}

	bool should_skip_draw(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();
		fill_before_sample(dev, ffp.cur_texture(0));

		// The Pip-Boy's paste and tint are drawn on the backbuffer.
		if (!g_active || !is_backbuffer(g_game_rt0)) return false;

		const auto* psmap = shared::common::g_ps_classifier.classify(ffp.last_ps());
		const uint32_t ps_hash = psmap ? psmap->ps_hash : 0u;

		// ISMENUBG: the sepia/darken pass over the menu background.
		if (cfg().skip_tint && ps_hash == PS_ISMENUBG)
		{
			++g_tints;
			if (!g_logged_tint)
			{
				g_logged_tint = true;
				shared::common::log("MenuBg", "skipping the ISMENUBG tint pass (PS 0xB546C4B6) while the live background runs",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS);
			}
			return true;
		}

		auto* tex = ffp.cur_texture(0);
		if (!tex) return false;

		if (g_menu_tex)
		{
			if (tex != g_menu_tex) return false;
			++g_pastes;
			if (!g_logged_paste)
			{
				g_logged_paste = true;
				shared::common::log("MenuBg", std::format(
					"skipping the frozen-background paste quad (PS 0x{:08X})", ps_hash),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS);
			}
			return true;
		}

		// Nothing learned yet: the first draw after a finished capture that
		// samples one of the capture's targets (and is not drawing into it) is
		// the paste.
		if (g_window == Window::Capture && g_window_saw_offscreen && captured_byte() == 1)
			g_window = Window::Done;
		if (g_window == Window::Done && captured_byte() == 1)
		{
			for (int i = 0; i < g_cand_n; ++i)
			{
				if (g_cand[i].tex != tex || g_cand[i].surf == g_game_rt0 || !screen_sized(g_cand[i].w, g_cand[i].h)) continue;
				g_menu_rt  = g_cand[i].surf;
				g_menu_tex = g_cand[i].tex;
				g_menu_w   = g_cand[i].w;
				g_menu_h   = g_cand[i].h;
				log_learned(g_cand[i], ps_hash);
				++g_pastes;
				g_logged_paste = true;
				return true;
			}
		}

		diag_rt_texture_draw(tex, ps_hash);
		return false;
	}

	void on_present()
	{
		// The frame is over: whatever window was open is closed.
		g_window = Window::Idle;
		g_redirect_in_place = false;
		if (!fnv_exe::known()) return;

		send_thumbnail_width();

		const auto& c = cfg();
		const bool menu_ok = nvse_link::has(FNVR_F_MENU);
		const bool capture_ok = nvse_link::has(FNVR_F_MENU_BG);
		const FnvRemixFacts& f = nvse_link::facts();
		const uint32_t raw = menu_ok ? f.pipboy_raw : 0xFFFFFFFFu;
		const int raw_log = menu_ok ? static_cast<int>(raw) : -1;

		if (menu_ok && raw != g_last_pip_mode && c.live_background && g_mode_logs < 64)
		{
			++g_mode_logs;
			shared::common::log("MenuBg", std::format("Pip-Boy state {} -> {} (engine monitor)",
				g_last_pip_mode == 0xFFFFFFFFu ? -1 : static_cast<int>(g_last_pip_mode), raw),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		if (menu_ok) g_last_pip_mode = raw;

		if (menu_ok && capture_ok != g_capture_ok)
		{
			g_capture_ok = capture_ok;
			shared::common::log("MenuBg", capture_ok
				? "menu-background capture code is vanilla again -- the Pip-Boy's live background is available"
				: "menu-background capture code is not vanilla (another module changed it; the engine monitor's "
				  "Integrity lines name the site) -- the Pip-Boy's live background is OFF",
				capture_ok ? shared::common::LOG_TYPE::LOG_TYPE_STATUS : shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		// Opening or open switches the session on. Closing (close requested,
		// the lowering animation) only keeps a running session alive; the
		// state machine then returns to closed, so no later menu can re-arm
		// the capture.
		const uint8_t pb = menu_ok ? f.pipboy : static_cast<uint8_t>(FNVR_PB_CLOSED);
		const bool pipboy_up = pb == FNVR_PB_OPENING || pb == FNVR_PB_OPEN;
		const bool closing = c.keep_during_close && g_session && pb == FNVR_PB_CLOSING;
		const bool want = c.live_background && capture_ok && g_guard && (pipboy_up || closing);

		if (want != g_session)
		{
			if (g_session) end_session(raw_log);
			else begin_session(raw_log);
		}

		backdrop_present(f, menu_ok);

		if (!g_session || !g_active) return;

		++g_frames;
		if (g_frames == WATCHDOG_FRAMES)
		{
			if (g_captures == 0)
			{
				give_up(std::format("{} frames and the armed capture never ran (request byte={}, captured byte={})",
					g_frames, static_cast<int>(request_byte()), static_cast<int>(captured_byte())));
				return;
			}
			// The Pip-Boy's capture is the only world render while it is open,
			// and its world draws carry the frame's world camera to the
			// runtime, so it keeps running without a redirect.
			if (!g_menu_tex)
				shared::common::log("MenuBg", std::format("{} frames, {} captures, but no paste draw matched any of the {} "
					"capture targets (see the 'render-target texture' lines above) -- the Pip-Boy capture keeps running "
					"for the world camera", g_frames, g_captures, g_cand_n), shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		// Arm the next frame's capture.
		request_byte() = 1;
		g_req_pending = true;
	}

	uint32_t last_pipboy_mode()
	{
		return g_last_pip_mode;
	}

	bool frozen_menu()
	{
		if (g_session || captured_byte() != 1 || !nvse_link::has(FNVR_F_MENU)) return false;
		const FnvRemixFacts& f = nvse_link::facts();
		return f.game_state == FNVR_GS_MENU && f.pipboy == FNVR_PB_CLOSED;
	}

	void on_reset()
	{
		forget_targets();
		g_cap_tex = nullptr;
		g_cap_dirty = false;
		g_window = Window::Idle;
		g_game_rt0 = nullptr;
		g_game_ds  = nullptr;
		g_bb_ds    = nullptr;
		g_redirect_in_place = false;
		g_substituted       = false;
		g_ds_substituted    = false;
	}
}
