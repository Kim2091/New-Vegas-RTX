#include "std_include.hpp"
#include "shared/common/flags.hpp"
#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "fault_report.hpp"
#include "call_chain.hpp"
#include "vs_expand.hpp"
#include "tex_pack.hpp"
#include "tex_pool.hpp"
#include "ui_boundary.hpp"
#include "menu_bg.hpp"
#include "ps_protocol.hpp"
#include "shared/common/remix_protocol.hpp"
#include "stable_origin.hpp"
#include "scene_graph.hpp"
#include "screen_space.hpp"
#include "nvse_link.hpp"
#include "underwater.hpp"
#include "vats.hpp"
#include "save_thumbnail.hpp"

/*
 * Fallout: New Vegas game-specific code: renderer reads, the fake-shadow
 * skip, render-target tracking, lights, bone capture and the game-code
 * patches.
 */

namespace comp::game
{
	// --- Skinned-batch state, written by the engine-side stubs ---

	// Nonzero while the engine is inside its skinned render batch.
	static volatile int g_in_skinned_batch = 0;
	// Nonzero when the next bone upload belongs to a new object.
	static volatile int g_palette_restart = 0;
	// The stubs are live, so the two flags above are meaningful.
	static bool g_batch_stubs_live = false;

	// World projection [0][0] (FOV term) latched once per frame from the first
	// world draw. FNV renders the first-person arm/Pip-Boy with a distinct
	// (zoomed) projection, so a gray draw whose proj[0] differs from this is the
	// viewmodel, not a baked shadow. 0 until the first world draw of the session.
	static float g_world_proj00 = 0.0f;
	static float g_world_fwd[3] = { 0.0f, 0.0f, 0.0f };


	// ================================================================
	// NiDX9Renderer helpers
	// ================================================================

	float* get_renderer_matrix(unsigned int byte_offset)
	{
		if (!renderer_singleton_ptr) return nullptr;
		void* ren = *renderer_singleton_ptr;
		if (!ren) return nullptr;
		return reinterpret_cast<float*>(static_cast<unsigned char*>(ren) + byte_offset);
	}

	namespace
	{
		// The pointer stored at `base + off`, or null when `base` is null.
		const unsigned char* follow(const void* base, unsigned int off)
		{
			if (!base) return nullptr;
			return *reinterpret_cast<const unsigned char* const*>(static_cast<const unsigned char*>(base) + off);
		}

		// The shader type of the material being drawn (structs.hpp: the
		// renderer's property state, its shade slot, the type field), or 0
		// when any link is missing.
		unsigned int current_shader_type()
		{
			if (!renderer_singleton_ptr) return 0;
			const unsigned char* shade = follow(follow(*renderer_singleton_ptr, RENDERER_PROPERTY_STATE_OFF),
				PROPERTY_STATE_SHADE_OFF);
			return shade ? *reinterpret_cast<const unsigned int*>(shade + SHADE_PROPERTY_TYPE_OFF) : 0;
		}
	}

	bool is_2d()
	{
		// A perspective projection copies view z into w (_34 = 1, _44 = 0);
		// the orthographic one the HUD and menus use keeps w at 1.
		const auto* proj = reinterpret_cast<const D3DMATRIX*>(get_renderer_matrix(RENDERER_PROJ_OFF));
		return proj && proj->_44 == 1.0f && proj->_34 == 0.0f;
	}

	bool is_sky()
	{
		return current_shader_type() == KPROP_SKY;
	}

	// ================================================================
	// Fake-shadow skip
	//
	// FNV draws planar shadow overlays as separate NOLIGHTING geometry whose
	// vertex colors are all grayscale with at least one dark vertex. They exist
	// only to darken the surface beneath; Remix ray-traces real shadows, so
	// these draws are dropped.
	//
	// The in-world UI (Pip-Boy / terminal screens) is also NOLIGHTING and gray,
	// so colour alone can't tell them apart. Two signals separate them, found
	// empirically: baked shadows write depth (ZWRITEENABLE on) while the UI does
	// not, and the first-person viewmodel renders with a distinct (zoomed) FOV.
	// A gray draw is only skipped when it writes depth and uses the world FOV.
	// ================================================================

	namespace
	{
		constexpr unsigned int KPROP_NOLIGHTING  = 0x15;
		constexpr int          SHADOW_CACHE_SIZE  = 64;   // power of two
		constexpr int          SHADOW_GRAY_TOL    = 5;    // max |R-G|,|R-B|,|G-B|
		constexpr int          SHADOW_DARK_THRESH = 250;  // >=1 vert R < this
		enum { SHADOW_UNKNOWN = 0, SHADOW_IS_FAKE = 1, SHADOW_NOT_FAKE = 2 };

		// Verdicts are keyed on the full stream-0 window, not just (vb, base).
		// Two different meshes routinely share a vertex buffer at the same base
		// with different counts/strides, and the 6-bit slot hash collides freely.
		struct ShadowCacheEntry
		{
			IDirect3DVertexBuffer9* vb = nullptr;
			int  base_vtx  = 0;
			UINT num_verts = 0;
			UINT stride    = 0;
			UINT offset    = 0;
			int  result    = SHADOW_UNKNOWN;
		};
		ShadowCacheEntry g_shadow_cache[SHADOW_CACHE_SIZE];

		// A vertex buffer address freed on cell unload can be handed straight
		// back out by the next allocation, and the replacement mesh would then
		// inherit the old verdict (geometry vanishing, or baked shadows coming
		// back). Holding a reference to keep the address unique would pin
		// D3DPOOL_DEFAULT buffers and break Reset, so the whole table is aged
		// out on a fixed cadence instead. Locking a vertex buffer crosses the
		// Remix bridge, so this trades a bounded ~1 s window of staleness for
		// 1/N of the re-lock cost a per-frame flush would pay.
		constexpr UINT SHADOW_CACHE_FLUSH_FRAMES = 64;
		UINT g_shadow_cache_frame = 0;

		// PS hashes whose NOLIGHTING+gray draws are meaningful UI, not shadows.
		constexpr uint32_t kFakeShadowKeep[] = { 0x95C1E039u /* Pip-Boy body */ };

		int check_vertex_colors_gray(const unsigned char* vb, UINT num_verts,
			UINT stride, UINT color_off, int color_type)
		{
			bool has_dark = false;
			for (UINT v = 0; v < num_verts; v++)
			{
				const unsigned char* vert = vb + v * stride;
				int r, g, b;
				if (color_type == D3DDECLTYPE_D3DCOLOR)
				{
					const unsigned char* c = vert + color_off;
					b = c[0]; g = c[1]; r = c[2];
				}
				else if (color_type == D3DDECLTYPE_FLOAT4)
				{
					const float* c = reinterpret_cast<const float*>(vert + color_off);
					r = static_cast<int>(c[0] * 255.0f + 0.5f);
					g = static_cast<int>(c[1] * 255.0f + 0.5f);
					b = static_cast<int>(c[2] * 255.0f + 0.5f);
				}
				else
				{
					return SHADOW_NOT_FAKE;
				}

				int dRG = r - g; if (dRG < 0) dRG = -dRG;
				int dRB = r - b; if (dRB < 0) dRB = -dRB;
				int dGB = g - b; if (dGB < 0) dGB = -dGB;
				if (dRG > SHADOW_GRAY_TOL || dRB > SHADOW_GRAY_TOL || dGB > SHADOW_GRAY_TOL)
					return SHADOW_NOT_FAKE;

				if (r < SHADOW_DARK_THRESH)
					has_dark = true;
			}
			return has_dark ? SHADOW_IS_FAKE : SHADOW_NOT_FAKE;
		}

		// FNV's baked planar shadow geometry writes depth (ZWRITEENABLE on); the
		// in-world UI overlay (Pip-Boy / terminal screens) does not. So a gray
		// NOLIGHTING draw is a fake shadow when it writes depth, and a depth-less
		// gray draw is UI to keep, without a PS keep-list.
		bool draw_writes_depth(IDirect3DDevice9* dev)
		{
			if (!dev) return true;  // no device: treated as depth-writing
			DWORD zwrite = 1;
			dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
			return zwrite != 0;
		}

		// True when the current draw uses a projection whose FOV term differs
		// from the frame's world projection, i.e. the first-person viewmodel
		// (Pip-Boy / arm), which FNV renders zoomed. Such draws are never fake
		// shadows even when their material matches (black, depth-writing).
		bool draw_uses_viewmodel_projection()
		{
			if (g_world_proj00 <= 0.0001f) return false;  // no reference yet
			float* p = get_renderer_matrix(RENDERER_PROJ_OFF);
			if (!p) return false;
			float d = p[0] - g_world_proj00;
			if (d < 0) d = -d;
			return d > 0.05f * g_world_proj00;  // >5% FOV difference
		}
	}

	bool is_no_lighting()
	{
		return current_shader_type() == KPROP_NOLIGHTING;
	}

	bool uses_viewmodel_projection()
	{
		return draw_uses_viewmodel_projection();
	}

	void clear_fake_shadow_cache()
	{
		for (auto& e : g_shadow_cache) e = ShadowCacheEntry{};
	}

	void on_frame_end()
	{
		vs_expand::on_present();
		lod_debug::on_present();
		vats::on_frame_end();
		save_thumbnail::on_present();

		if (++g_shadow_cache_frame >= SHADOW_CACHE_FLUSH_FRAMES)
		{
			g_shadow_cache_frame = 0;
			clear_fake_shadow_cache();
		}
	}

	bool should_skip_fake_shadow(IDirect3DDevice9* dev, INT base_vtx, UINT num_verts,
		IDirect3DPixelShader9* ps)
	{
		PROFILE_ZONE_N("game::should_skip_fake_shadow");
		const auto& cfg = shared::common::config::get();
		if (!cfg.ffp.skip_fake_shadows) return false;

		// NOLIGHTING shade property is the cheap first gate.
		if (!is_no_lighting()) return false;

		auto& ffp = shared::common::ffp_state::get();
		if (!ffp.cur_decl_has_color() || ffp.cur_decl_color_off() < 0) return false;

		const auto* psm = ps ? shared::common::g_ps_classifier.classify(ps) : nullptr;

		// Untextured vertex-colour-only draws (NOLIGHT001.vso + NOLIGHTVCONLY.pso: no
		// TEXCOORD, a PS without samplers) are the baked shadow quads, whether they
		// write depth or not. Remix would trace them with whatever texture the last
		// draw left on stage 0: white or random quads. The in-world UI the depth
		// rule spares is always textured, so it never matches here.
		if (psm && psm->sampler_count == 0 && ffp.cur_decl_n_texcoords() == 0 && !draw_uses_viewmodel_projection())
		{
			static std::unordered_set<uint32_t> s_logged;
			if (s_logged.size() < 16 && s_logged.insert(psm->ps_hash).second)
				shared::common::log("FakeShadow", std::format("skipping untextured vertex-colour draws of PS 0x{:08X} "
					"(no samplers, no TEXCOORD)", psm->ps_hash), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			return true;
		}

		// Spare meaningful UI shaders (Pip-Boy body etc.) whose verts are also gray.
		if (psm)
			for (uint32_t h : kFakeShadowKeep)
				if (psm->ps_hash == h) return false;

		auto* vb = ffp.stream_vb(0);
		UINT stride = ffp.stream_stride(0);
		UINT offset = ffp.stream_offset(0);
		if (!vb || stride == 0 || num_verts == 0) return false;

		const unsigned int slot =
			((static_cast<unsigned int>(reinterpret_cast<uintptr_t>(vb) >> 4))
				^ static_cast<unsigned int>(base_vtx)
				^ (num_verts * 2654435761u)) & (SHADOW_CACHE_SIZE - 1);
		auto& e = g_shadow_cache[slot];
		if (e.vb == vb && e.base_vtx == base_vtx && e.num_verts == num_verts &&
			e.stride == stride && e.offset == offset)
			return e.result == SHADOW_IS_FAKE
				&& draw_writes_depth(dev) && !draw_uses_viewmodel_projection();

		const UINT read_off = offset + static_cast<UINT>(base_vtx) * stride;
		const UINT read_size = num_verts * stride;
		void* data = nullptr;
		if (FAILED(vb->Lock(read_off, read_size, &data, D3DLOCK_READONLY)) || !data)
			return false;

		int result = check_vertex_colors_gray(static_cast<const unsigned char*>(data),
			num_verts, stride, static_cast<UINT>(ffp.cur_decl_color_off()), ffp.cur_decl_color_type());
		vb->Unlock();

		e.vb = vb; e.base_vtx = base_vtx; e.num_verts = num_verts;
		e.stride = stride; e.offset = offset; e.result = result;

		return result == SHADOW_IS_FAKE
			&& draw_writes_depth(dev) && !draw_uses_viewmodel_projection();
	}

	void on_world_draw(IDirect3DDevice9* dev, bool claimed_static)
	{
		static UINT s_frame = 0xFFFFFFFFu;
		auto& ffp = shared::common::ffp_state::get();
		const UINT fc = ffp.frame_count();
		if (s_frame == fc) return;
		// The renderer's world draw gates.
		if (!ffp.is_enabled() || !ffp.view_proj_valid() || !rendering_to_backbuffer || is_2d() ||
			(!claimed_static && is_sky())) return;
		const float* view = get_renderer_matrix(RENDERER_VIEW_OFF);
		const float* proj = get_renderer_matrix(RENDERER_PROJ_OFF);
		// A left-handed perspective projection (w = view z), nothing else.
		if (!view || !proj || proj[11] != 1.0f || proj[15] != 0.0f || !(proj[0] > 1e-4f)) return;
		s_frame = fc;
		// This frame's camera: D re-taken if the global moved since
		// BeginScene, before the Remix camera, the lights and the API draws.
		stable_origin::at_first_world_draw(dev);
		// The underwater look, in this frame's render space.
		underwater::on_world_frame();
		// The world FOV: world geometry renders before the first-person
		// viewmodel, so the frame's first world draw carries the world
		// projection (not the zoomed Pip-Boy one).
		g_world_proj00 = proj[0];
		// The world camera's forward (+Z of the D3D view in world space), for
		// the Pip-Boy light: before the world draws the renderer's VIEW can
		// hold another camera (it read (0, 1, 0) at BeginScene).
		g_world_fwd[0] = view[2];
		g_world_fwd[1] = view[6];
		g_world_fwd[2] = view[10];
		// The lights, now: the engine has updated this frame's transforms
		// (lights read at BeginScene would trail what they hang on by a frame).
		update_lights(dev);
	}

	void apply_transforms(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("game::apply_transforms");
		float* world = get_renderer_matrix(RENDERER_WORLD_OFF);
		float* view  = get_renderer_matrix(RENDERER_VIEW_OFF);
		float* proj  = get_renderer_matrix(RENDERER_PROJ_OFF);

		if (world && view && proj)
		{
			// Into the render frame (stable origin and world unit scale).
			stable_origin::set_transform(dev, D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(world));
			stable_origin::set_transform(dev, D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(view));
			stable_origin::set_transform(dev, D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(proj));
			screen_space::note_applied_projection(proj);
		}
	}


	// ================================================================
	// Sky reads
	//
	// No C++ objects in these two: __try can't share a frame with anything
	// that needs unwinding.
	// ================================================================

	bool try_get_sun_orientation(float& elev_deg, float& rot_deg, float* hour, bool* night)
	{
		__try { return get_sun_orientation(elev_deg, rot_deg, hour, night); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool try_get_masser_orientation(float& elev_deg, float& rot_deg)
	{
		__try { return get_masser_orientation(elev_deg, rot_deg); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static const void* get_sky_unguarded()
	{
		auto* mgr = world_manager();
		if (!is_valid_ptr(mgr)) return nullptr;
		auto* sky = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(mgr) + WM_SKY_OFF);
		return is_valid_ptr(sky) ? sky : nullptr;
	}

	const void* try_get_sky()
	{
		__try { return get_sky_unguarded(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	bool try_get_weather_state(WeatherState& out)
	{
		__try { return get_weather_state(out); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}


	// ================================================================
	// Render target tracking
	// ================================================================

	void note_default_render_target(IDirect3DDevice9* dev)
	{
		default_render_target = nullptr;
		IDirect3DSurface9* rt = nullptr;
		if (!dev || FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
		default_render_target = rt;
		rt->Release();  // identity only, as backbuffer_surface
	}

	bool is_screen_surface(IDirect3DSurface9* s)
	{
		return s && (s == backbuffer_surface || s == default_render_target);
	}

	void init_backbuffer_tracking(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("game::init_backbuffer_tracking");
		IDirect3DSurface9* bb = nullptr;
		if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
		{
			D3DSURFACE_DESC desc;
			if (SUCCEEDED(bb->GetDesc(&desc)))
			{
				backbuffer_width = desc.Width;
				backbuffer_height = desc.Height;
				shared::common::log("Game", std::format("Backbuffer: {}x{}", desc.Width, desc.Height));
			}

			// Keep the address for identity comparisons and drop the reference
			// (see backbuffer_surface in game.hpp). The pointer is never
			// dereferenced.
			backbuffer_surface = bb;
			backbuffer_surface_seen = false;
			bb->Release();
		}
		rendering_to_backbuffer = true;
	}

	void on_set_render_target(IDirect3DDevice9* /*dev*/, DWORD idx, IDirect3DSurface9* surface)
	{
		PROFILE_ZONE_N("game::on_set_render_target");
		if (idx != 0) return;

		// D3D9 rejects a null RT0, but the wrapper is called before the forward,
		// so a call that will fail must not leave a stale verdict behind.
		if (!surface)
		{
			rendering_to_backbuffer = false;
			offscreen_screen_sized = false;
			return;
		}

		// By identity and size, whatever rendering_to_backbuffer decides below:
		// until the backbuffer's identity is seen that one compares sizes only.
		// Both backbuffer identities must be known to call a target off-screen.
		D3DSURFACE_DESC desc;
		const bool have_desc = SUCCEEDED(surface->GetDesc(&desc));
		offscreen_screen_sized = have_desc && backbuffer_surface && default_render_target && !is_screen_surface(surface) &&
			desc.Width == backbuffer_width && desc.Height == backbuffer_height;

		if (backbuffer_surface && surface == backbuffer_surface)
		{
			if (!backbuffer_surface_seen)
			{
				backbuffer_surface_seen = true;
				shared::common::log("Game", "Backbuffer RT identity confirmed (exact off-screen detection active)");
			}
			rendering_to_backbuffer = true;
			return;
		}

		// Once the backbuffer's identity has been seen bound, anything else is
		// off-screen, including full-resolution targets.
		if (backbuffer_surface_seen)
		{
			rendering_to_backbuffer = false;
			return;
		}

		// The engine has not bound the cached backbuffer yet: compare sizes, so
		// an unexpected swap-chain arrangement can't mark every draw off-screen
		// and disable FFP conversion.
		if (have_desc)
			rendering_to_backbuffer = (desc.Width == backbuffer_width && desc.Height == backbuffer_height);
		else
			rendering_to_backbuffer = true; // assume backbuffer on failure
	}

	namespace
	{
		IDirect3DQuery9* g_raster_query = nullptr;
		bool g_raster_active = false;
		bool g_raster_failed = false;
		bool g_raster_logged = false;
	}

	bool begin_raster_only(IDirect3DDevice9* dev)
	{
		if (!dev || g_raster_active || !offscreen_screen_sized || !is_2d()) return false;
		if (!g_raster_query)
		{
			if (g_raster_failed) return false;
			if (FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_raster_query)) || !g_raster_query)
			{
				g_raster_query = nullptr;
				g_raster_failed = true;
				shared::common::log("Game", "no occlusion query: orthographic draws into screen-sized off-screen targets "
					"can trigger the runtime's RTX injection there", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}
		}
		if (FAILED(g_raster_query->Issue(D3DISSUE_BEGIN))) return false;
		g_raster_active = true;
		if (!g_raster_logged)
		{
			g_raster_logged = true;
			shared::common::log("Game", std::format("first orthographic draw into a screen-sized off-screen target "
				"({}x{}): drawn inside an occlusion query, so the runtime rasterises it and never takes it as the "
				"frame's UI", backbuffer_width, backbuffer_height), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		return true;
	}

	void end_raster_only(IDirect3DDevice9* /*dev*/)
	{
		if (!g_raster_active) return;
		g_raster_active = false;
		if (g_raster_query) g_raster_query->Issue(D3DISSUE_END);
	}

	bool pause_raster_only(IDirect3DDevice9* dev)
	{
		if (!g_raster_active) return false;
		end_raster_only(dev);
		return true;
	}

	void resume_raster_only(IDirect3DDevice9* /*dev*/)
	{
		if (g_raster_active || !g_raster_query) return;
		if (SUCCEEDED(g_raster_query->Issue(D3DISSUE_BEGIN))) g_raster_active = true;
	}


	// ================================================================
	// Point lights
	//
	// The engine monitor plugin's light registry (nvse_link::lights): placed
	// lights while their reference's 3D is loaded, the lights in the engine's
	// active lists (point +0xB4, shadow-casting +0xC0), and the player's light
	// (the Pip-Boy light). The plugin pins each one, so an id names one light
	// for as long as it is listed, and copies the values: nothing here reads
	// an engine light.
	// ================================================================

	// Where the player's light goes this frame: third person (the engine's
	// light position plus EffectLightThirdPersonForward) or first person (the
	// eye plus EffectLightForward), from the plugin's PlayerCharacter read.
	static bool g_player_third_person = false;

	// One line per NiLight the engine makes the player's light (a new one each
	// time it is switched on), and one per on / off change.
	static void log_player_light(const FnvRemixLight* l, bool on)
	{
		static uintptr_t s_logged = 0;
		static bool s_on = false;
		if (l && l->id != s_logged)
		{
			s_logged = l->id;
			shared::common::log("Lights", std::format(
				"Pip-Boy light: NiLight 0x{:X} (the plugin's player light: PlayerCharacter effect list -> LightEffect) "
				"radius {:.1f} colour ({:.3f},{:.3f},{:.3f}) dimmer {:.2f} atten ({:.3f},{:.3f},{:.3f}) on={}",
				l->id, l->radius, l->diffuse[0], l->diffuse[1], l->diffuse[2], l->dimmer, l->atten[0], l->atten[1],
				l->atten[2], on ? 1 : 0), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		else if (on != s_on)
		{
			shared::common::log("Lights", on
				? std::format("Pip-Boy light on (NiLight 0x{:X})", l ? l->id : 0)
				: std::string("Pip-Boy light off"), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		s_on = on;
	}

	namespace
	{
		// Switches off the device's light slots from `first` up to the last
		// slot in use; `first` becomes the number of slots in use.
		void release_light_slots(IDirect3DDevice9* dev, int first)
		{
			for (int slot = first; slot < device_light_count; ++slot)
				dev->LightEnable(static_cast<DWORD>(slot), FALSE);
			device_light_count = first;
		}

		// [Lights] RangeMode. Any other value sends the light unbounded.
		enum RangeMode { kRangeRadius = 0, kRangeFromFalloff = 1 };

		// The distance at which the falloff's distance terms, a1 d + a2 d^2,
		// reach 255, where the light is down to about 1/255. The root is
		// taken as 2K / (a1 + sqrt(a1^2 + 4 a2 K)), which also gives K / a1
		// for a light with no quadratic term. 0 for a light that does not
		// fade with distance.
		float range_from_falloff(float a1, float a2)
		{
			constexpr float kLimit = 255.0f;
			const float quad = a2 > 0.0f ? a2 : 0.0f;
			if (!(quad > 0.0f || a1 > 0.0f)) return 0.0f;
			return 2.0f * kLimit / (a1 + std::sqrt(a1 * a1 + 4.0f * quad * kLimit));
		}

		float light_range(const FnvRemixLight& l)
		{
			switch (light_range_mode)
			{
			case kRangeRadius:      return l.radius;
			case kRangeFromFalloff: return range_from_falloff(l.atten[1], l.atten[2]);
			default:                return std::numeric_limits<float>::infinity();
			}
		}
	}

	void update_lights(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("game::update_lights");
		if (!lights_enabled)
		{
			// Switched off at run time (Host Settings): the lights set last
			// frame are still enabled on the device.
			release_light_slots(dev, 0);
			return;
		}
		if (lights_updated_frame) return;
		if (!camera_position_ptr) return;
		lights_updated_frame = true;

		if (!nvse_link::has(FNVR_F_LIGHTS))
		{
			static bool s_logged = false;
			if (!s_logged && (nvse_link::linked() || !nvse_link::problem().empty()))
			{
				s_logged = true;
				shared::common::log("Lights", "no light registry (the engine monitor plugin is missing, or it can't read "
					"the ShadowSceneNode; see [NvseLink]) -- no engine lights are sent",
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			release_light_slots(dev, 0);
			return;
		}

		constexpr uint32_t kRegistryMax = 1024;
		static FnvRemixLight s_reg[kRegistryMax];
		const uint32_t n = nvse_link::lights(s_reg, kRegistryMax);
		const FnvRemixFacts& facts = nvse_link::facts();
		const float strength = light_intensity;

		// First / third person from the plugin; each switch logged once.
		{
			static int s_logged_mode = -1;
			const int tp = facts.third_person ? 1 : 0;
			g_player_third_person = tp == 1;
			if (tp != s_logged_mode)
			{
				s_logged_mode = tp;
				shared::common::log("Lights", tp == 1
					? std::format("Pip-Boy light: third person -- at the engine's light position + {:.0f} units ahead "
						"(EffectLightThirdPersonForward)", effect_light_third_person_forward)
					: std::format("Pip-Boy light: first person -- {:.0f} units ahead of the eye (EffectLightForward)",
						effect_light_forward), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
			}
		}

		int i = 0;
		auto emit = [&](const FnvRemixLight& l, bool player_light) {
			const float* cam = camera_position_ptr;
			const float gain = l.dimmer * strength * (player_light ? pipboy_light_intensity : 1.0f);
			D3DLIGHT9 light = {};
			light.Type = D3DLIGHT_POINT;
			light.Diffuse = { l.diffuse[0] * gain, l.diffuse[1] * gain, l.diffuse[2] * gain, 1.0f };
			light.Ambient = { l.ambient[0], l.ambient[1], l.ambient[2], 1.0f };
			// The device's lights are camera-relative, like its transforms.
			light.Position = { l.pos[0] - cam[0], l.pos[1] - cam[1], l.pos[2] - cam[2] };

			// The engine hangs the player's light on the player's body, inside the
			// traced player mesh. First person: it goes to the eye (camera-relative
			// 0) plus EffectLightForward along the view (+Z of the D3D view matrix
			// in world space), a headlamp. Third person the eye is behind the
			// player, and a light there is shadowed by the player's own body: it
			// stays where the engine hangs it, plus EffectLightThirdPersonForward
			// along the view's horizontal direction (out of the body, ahead of the
			// player; the world is Z-up).
			if (player_light)
			{
				const float fx = g_world_fwd[0], fy = g_world_fwd[1], fz = g_world_fwd[2];
				if (g_player_third_person)
				{
					const float hlen = std::sqrt(fx * fx + fy * fy);
					if (effect_light_third_person_forward > 0.0f && hlen > 1e-4f)
					{
						const float k = effect_light_third_person_forward / hlen;
						light.Position.x += fx * k;
						light.Position.y += fy * k;
					}
				}
				else if (effect_light_forward > 0.0f)
				{
					const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
					if (len > 1e-4f)
					{
						const float k = effect_light_forward / len;
						light.Position.x = fx * k;
						light.Position.y = fy * k;
						light.Position.z = fz * k;
					}
				}
			}

			light.Range = light_range(l);
			light.Attenuation0 = l.atten[0];
			light.Attenuation1 = l.atten[1];
			light.Attenuation2 = l.atten[2];
			// The player's light is linear-only (0, 1, 0: the NiPointLight ctor's,
			// never overridden); the runtime's legacy conversion makes that nearly
			// black. Give it the placed lights' quadratic falloff (they carry
			// (0, 0, ~0.001)).
			if (player_light && l.atten[0] == 0.0f && l.atten[2] == 0.0f && l.atten[1] > 0.0f)
			{
				light.Attenuation1 = 0.0f;
				light.Attenuation2 = 0.001f;
			}

			// The light's identity for the runtime (LightUtils::legacyIdentity): the
			// NiLight's address in the point light's unused spot fields, two exact
			// 16-bit halves. The runtime names its legacy lights by position, so a
			// light that moves (the Pip-Boy light, a swinging lamp) would arrive as
			// a new light each frame and leave its old copy behind; with the
			// identity it is matched to last frame's copy whatever the distance. The D3D9
			// fixed-function raster ignores Theta / Phi on point lights, and the
			// bridge copies the struct whole. The plugin pins the NiLight while it
			// is listed, so the address names this one light.
			const uint32_t light_id = static_cast<uint32_t>(l.id);
			light.Theta = static_cast<float>(light_id & 0xFFFFu);
			light.Phi = static_cast<float>(light_id >> 16);


			// The light's asset key, which Remix mods and captures name it by
			// (LightUtils::legacyAssetKey, with rtx.scene.assetKeys): Falloff
			// marks it, Direction holds bits 0-21, 22-42 and 43-63 as exact
			// integers. A point light uses neither field.
			if (l.asset_key)
			{
				light.Falloff = 4932953.0f;
				light.Direction.x = static_cast<float>(l.asset_key & 0x3FFFFFull);
				light.Direction.y = static_cast<float>((l.asset_key >> 22) & 0x1FFFFFull);
				light.Direction.z = static_cast<float>(l.asset_key >> 43);
			}

			// Into the render frame: the stable origin's D and the world unit scale.
			stable_origin::light_to_render(light);

			const auto slot = static_cast<DWORD>(i++);
			dev->SetLight(slot, &light);
			dev->LightEnable(slot, TRUE);
		};

		// The player's light first, while the engine holds it and it is lit.
		const FnvRemixLight* player = nullptr;
		for (uint32_t k = 0; k < n && !player; k++)
			if (s_reg[k].kind == FNVR_LK_PLAYER) player = &s_reg[k];
		const bool player_on = player && player->lit;
		log_player_light(player, player_on);
		if (player_on)
		{
			FnvRemixLight pl = *player;
			// Third person it stays where the engine hangs it, unless that
			// position is nowhere near the player (a node the engine no
			// longer updates): then at the player's chest.
			const float* pp = facts.player_pos;
			const bool have_player = pp[0] != 0.0f || pp[1] != 0.0f || pp[2] != 0.0f;
			float far_by = 0.0f;
			if (g_player_third_person && have_player)
			{
				const float dx = pl.pos[0] - pp[0], dy = pl.pos[1] - pp[1], dz = pl.pos[2] - pp[2];
				far_by = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (far_by > 600.0f)
				{
					pl.pos[0] = pp[0];
					pl.pos[1] = pp[1];
					pl.pos[2] = pp[2] + 100.0f;
				}
			}
			emit(pl, true);

			// What went out, once per change of light / mode / placement and
			// once per 600 frames: the D3D9 position (render units, (camera-
			// relative + D) * s), the world position it stands for, the camera
			// and the player.
			D3DLIGHT9 sent = {};
			dev->GetLight(0, &sent);
			const double* d = stable_origin::shift();
			const double s = stable_origin::scale();
			const float wx = static_cast<float>(sent.Position.x / s - d[0] + camera_position_ptr[0]);
			const float wy = static_cast<float>(sent.Position.y / s - d[1] + camera_position_ptr[1]);
			const float wz = static_cast<float>(sent.Position.z / s - d[2] + camera_position_ptr[2]);
			static uintptr_t s_id = 0;
			static bool s_tp = false, s_away = false;
			static UINT s_frame = 0;
			const UINT fnow = shared::common::ffp_state::get().frame_count();
			const bool away = far_by > 600.0f;
			if (pl.id != s_id || g_player_third_person != s_tp || away != s_away || fnow - s_frame >= 600)
			{
				s_id = pl.id;
				s_tp = g_player_third_person;
				s_away = away;
				s_frame = fnow;
				shared::common::log("Lights", std::format(
					"Pip-Boy light sent [slot 0]: {} person, D3D9 position ({:.1f}, {:.1f}, {:.1f}) = world ({:.1f}, {:.1f}, "
					"{:.1f}) | engine light ({:.1f}, {:.1f}, {:.1f}){} | camera ({:.1f}, {:.1f}, {:.1f}) player ({:.1f}, "
					"{:.1f}, {:.1f}) forward ({:.2f}, {:.2f}, {:.2f}) | diffuse ({:.2f}, {:.2f}, {:.2f}) range {:.0f} atten "
					"({:.3f}, {:.3f}, {:.4f})", g_player_third_person ? "third" : "first", sent.Position.x, sent.Position.y,
					sent.Position.z, wx, wy, wz, player->pos[0], player->pos[1], player->pos[2],
					away ? std::format(" -- {:.0f} units from the player, placed at the player", far_by) : std::string(),
					camera_position_ptr[0], camera_position_ptr[1], camera_position_ptr[2], pp[0], pp[1], pp[2],
					g_world_fwd[0], g_world_fwd[1], g_world_fwd[2], sent.Diffuse.r, sent.Diffuse.g, sent.Diffuse.b, sent.Range,
					sent.Attenuation0, sent.Attenuation1, sent.Attenuation2), shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			}
		}

		// The rest, nearest first, up to the device limit (a stable order:
		// the runtime takes the first enabled slots). Distance alone decides:
		// the engine's own list follows the view (frustum, rooms), and a
		// light that left it would lose its slot on a camera turn.
		static std::vector<const FnvRemixLight*> s_order;
		s_order.clear();
		uint32_t n_placed = 0, n_effect = 0, n_listed = 0, n_unlit = 0;
		for (uint32_t k = 0; k < n; k++)
		{
			const FnvRemixLight& l = s_reg[k];
			if (l.kind == FNVR_LK_PLAYER) continue;
			(l.kind == FNVR_LK_PLACED ? n_placed : n_effect)++;
			if (!l.lit) { n_unlit++; continue; }
			if (l.listed_by_engine) n_listed++;
			s_order.push_back(&l);
		}
		auto dist2 = [&](const FnvRemixLight* l) {
			const float dx = l->pos[0] - camera_position_ptr[0], dy = l->pos[1] - camera_position_ptr[1],
				dz = l->pos[2] - camera_position_ptr[2];
			return dx * dx + dy * dy + dz * dz;
		};
		std::stable_sort(s_order.begin(), s_order.end(),
			[&](const FnvRemixLight* a, const FnvRemixLight* b) { return dist2(a) < dist2(b); });

		// Lights by what they are (world position to 2 units, colour): two
		// NiLights can name the same lamp; each lamp goes out once.
		static std::unordered_set<uint64_t> s_lamps;
		s_lamps.clear();
		auto lamp_key = [](const FnvRemixLight& l) {
			uint64_t h = 1469598103934665603ull;
			auto mix = [&h](int64_t v) { h = (h ^ static_cast<uint64_t>(v)) * 1099511628211ull; };
			for (int k = 0; k < 3; k++) mix(static_cast<int64_t>(std::floor(l.pos[k] * 0.5f)));
			for (int k = 0; k < 3; k++) mix(static_cast<int64_t>(l.diffuse[k] * 64.0f));
			return h;
		};
		uint32_t n_dup = 0, n_over = 0;
		for (const FnvRemixLight* l : s_order)
		{
			if (!s_lamps.insert(lamp_key(*l)).second) { n_dup++; continue; }
			if (i >= MAX_EXTRACTED_LIGHTS) { n_over++; continue; }
			emit(*l, false);
		}

		// Slots last frame filled and this one did not.
		release_light_slots(dev, i);

		const UINT now = shared::common::ffp_state::get().frame_count();
		static UINT s_count_log = 0;
		if (now - s_count_log >= 600)
		{
			s_count_log = now;
			shared::common::log("Lights", std::format("sent {} (registry: placed {}, effect {}, player {}; listed {}, unlit {}, "
				"over the {} cap {}), duplicates dropped {}, pins {}, Pip-Boy light {}", i, n_placed, n_effect,
				player ? 1 : 0, n_listed, n_unlit, MAX_EXTRACTED_LIGHTS, n_over, n_dup, facts.pins, player_on ? "on" : "off"),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}
	}


	// ================================================================
	// Skinning: bone capture
	// ================================================================

	void skinned_pass_done()
	{
		// The palette is only ever cleared by the next bone upload
		// (on_set_vs_const_f), never here: a following pass of the same object
		// either re-uploads its bones (fresh palette, same matrices) or uploads
		// nothing and draws with the palette as it stands.
		bones_drawn = true;
	}


	// ================================================================
	// Immediate bone upload (called from d3d9ex SetVertexShaderConstantF)
	// ================================================================

	// Register range [lo, hi) of bone uploads whose device forward was swallowed
	// for the current object. Used by flush_bones_to_device to restore the
	// device c[] file before a real-shader fallback draw. Reset per object.
	static UINT bone_flush_lo = 256;
	static UINT bone_flush_hi = 0;

	bool on_set_vs_const_f([[maybe_unused]] IDirect3DDevice9* dev, UINT start_reg, const float* data, UINT count)
	{
		PROFILE_ZONE_N("game::on_set_vs_const_f");
		constexpr int REGS_PER_BONE = 3;

		if (count != REGS_PER_BONE) return false;
		if (!(g_batch_stubs_live ? g_in_skinned_batch : shared::common::ffp_state::get().cur_decl_is_skinned()))
			return false;

		// New bone batch: reset counter on per-object boundary or after draw
		if (g_palette_restart || bones_drawn)
		{
			prev_num_bones = num_bones;
			num_bones = 0;
			bones_drawn = false;
			g_palette_restart = 0;
			bone_flush_lo = 256;
			bone_flush_hi = 0;
		}

		if (num_bones < MAX_FFP_BONES && data)
		{
			// The palette slot, raw (4x3 packed): the API skinned path reads it
			// as is.
			std::memcpy(bone_rows[num_bones], data, sizeof(bone_rows[0]));
			num_bones++;

			// Record the range so a real-shader fallback can restore it; the
			// raw constant is otherwise swallowed (dead under the nulled FFP VS).
			if (start_reg < bone_flush_lo) bone_flush_lo = start_reg;
			if (start_reg + count > bone_flush_hi) bone_flush_hi = start_reg + count;
			return true;
		}

		// At/over the FFP bone limit: not represented in the bone palette, so let
		// the raw constant forward normally to keep a fallback draw correct.
		return false;
	}

	void flush_bones_to_device(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("game::flush_bones_to_device");
		if (bone_flush_hi <= bone_flush_lo) return;
		const float* cache = shared::common::ffp_state::get().vs_const_data();
		dev->SetVertexShaderConstantF(bone_flush_lo, &cache[bone_flush_lo * 4],
			bone_flush_hi - bone_flush_lo);
	}


	// ================================================================
	// Lifecycle
	// ================================================================

	void on_reset()
	{
		clear_fake_shadow_cache();
		// The raster-only occlusion query goes before the real Reset.
		g_raster_active = false;
		if (g_raster_query)
		{
			g_raster_query->Release();
			g_raster_query = nullptr;
		}
		offscreen_screen_sized = false;
		// Holds D3DPOOL_DEFAULT (dynamic) copies for particles, which must be
		// gone before the real Reset or it fails with D3DERR_INVALIDCALL.
		vs_expand::release_cache();

		// Reset destroys the swap chain; the old backbuffer address is dead and
		// a resolution change invalidates the cached extents. Zeroing the width
		// is what makes on_begin_scene_cb re-run init_backbuffer_tracking.
		backbuffer_surface = nullptr;
		default_render_target = nullptr;
		backbuffer_surface_seen = false;
		backbuffer_width = 0;
		backbuffer_height = 0;
		rendering_to_backbuffer = true;

		num_bones = 0;
		prev_num_bones = 0;
		bones_drawn = false;
		lights_updated_frame = false;
		device_light_count = 0;
	}


	// ================================================================
	// Skinned-batch markers
	//
	// The bone capture needs two facts from the engine: whether the vertex
	// constants being uploaded belong to the skinned batch, and where one
	// object's bones end and the next object's begin. Two engine calls give
	// them: the call that runs the skinned batch, and the per-object bone
	// setup inside it. Each call site calls a thunk in a page of our own
	// (call_chain::emit_call_wrapper): the batch thunk raises
	// g_in_skinned_batch around the call and asks for a new palette
	// afterwards; the setup thunk asks for a new palette after the setup runs.
	// The thunks call whatever the site called, another mod's hook included,
	// and the sites stay E8 calls a later mod can chain from.
	//
	// The engine also branches over the skinned path for some geometry
	// (skin_path_jz); that branch is made unconditional so every skinned
	// object reaches the batch. It is a byte, not a hook: the site must hold
	// the engine's jz, or already the unconditional jmp.
	// ================================================================

	namespace
	{
		constexpr uint8_t kJmpShort = 0xEB;

		constexpr const char* kSkinIfReplaced = "Skinned meshes keep their bone palettes only if that code calls "
			"what it replaced; if it does not, the FFP path falls back to the vertex-declaration heuristic.";
	}

	static void install_skinned_batch_markers()
	{
		// Game code is patched only while [Skinning] Enabled is set.

		if (!shared::common::config::get().skinning.enabled)
		{
			shared::common::log("Game", "Skinning: batch markers SKIPPED (config disabled)");
			return;
		}

		// All three sites are checked before any is written: either every
		// site is patched or none is. The two calls may already go to another
		// mod's code; the thunks then call that code.
		const auto& addr = fnv_exe::addr();
		const fnv_exe::Code& jz = addr.skin_path_jz;
		const call_chain::Site batch = call_chain::inspect(addr.skin_render_call);
		const call_chain::Site setup = call_chain::inspect(addr.skin_reset_call);
		call_chain::log_site("Game", addr.skin_render_call, "Skinning: skinned batch call", batch);
		call_chain::log_site("Game", addr.skin_reset_call, "Skinning: bone setup call", setup);
		const auto* jz_now = reinterpret_cast<const uint8_t*>(jz.addr);
		const bool jz_vanilla = jz.addr && std::memcmp(jz_now, jz.bytes, jz.len) == 0;
		const bool jz_taken = jz.addr && jz_now[0] == kJmpShort && std::memcmp(jz_now + 1, jz.bytes + 1, jz.len - 1u) == 0;
		if (jz.addr && !jz_vanilla && !jz_taken)
			shared::common::log("Game", std::format("Skinning: 0x{:X} (skinned-path jz) holds [{}], expected the "
				"engine's [{}] or the unconditional jmp -- another mod rewrote it, so the wrapper leaves it alone.",
				jz.addr, call_chain::hex(jz_now, jz.len), call_chain::hex(jz.bytes, jz.len)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		if ((!jz_vanilla && !jz_taken) || batch.state == call_chain::State::Foreign ||
			setup.state == call_chain::State::Foreign)
		{
			shared::common::log("Game",
				"Skinning: batch markers SKIPPED - patch sites did not match. The FFP path falls back "
				"to the vertex-declaration heuristic; the game is left untouched.",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}

		// The thunks' code, then each thunk's return stack.
		auto* page = static_cast<uint8_t*>(
			VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!page)
		{
			shared::common::log("Game", "Skinning: WARNING - no memory for the batch thunks",
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return;
		}
		auto* stacks = reinterpret_cast<call_chain::ReturnStack*>(page + 2048);

		uint8_t* const batch_thunk = page;
		uint8_t* at = call_chain::emit_call_wrapper(batch_thunk, &stacks[0], batch.target,
			{ { &g_in_skinned_batch, 1 } }, { { &g_in_skinned_batch, 0 }, { &g_palette_restart, 1 } });
		while (reinterpret_cast<uintptr_t>(at) & 15) *at++ = 0xCC;
		uint8_t* const setup_thunk = at;
		at = call_chain::emit_call_wrapper(setup_thunk, &stacks[1], setup.target, {}, { { &g_palette_restart, 1 } });
		FlushInstructionCache(GetCurrentProcess(), page, static_cast<SIZE_T>(at - page));

		// jz -> jmp: the skinned path is always taken.
		if (jz_taken)
			shared::common::log("Game", std::format("Skinning: 0x{:X} already always takes the skinned path", jz.addr));
		else if (call_chain::patch_byte(jz.addr, kJmpShort))
			shared::common::log("Game", std::format("Skinning: 0x{:X} now always takes the skinned path", jz.addr));
		else
			shared::common::log("Game", std::format("Skinning: WARNING - could not write 0x{:X}", jz.addr),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
		call_chain::Site jz_site;
		jz_site.state = call_chain::State::Vanilla;
		call_chain::track(call_chain::Kind::Byte, jz.addr, kJmpShort, jz_site, "Game", "Skinning: skinned-path jz",
			kSkinIfReplaced);

		const auto batch_at = reinterpret_cast<uintptr_t>(batch_thunk);
		const auto setup_at = reinterpret_cast<uintptr_t>(setup_thunk);
		const bool batch_ok = call_chain::patch_rel32(addr.skin_render_call.addr, 0xE8, batch_at);
		const bool setup_ok = batch_ok && call_chain::patch_rel32(addr.skin_reset_call.addr, 0xE8, setup_at);
		if (batch_ok)
			call_chain::track(call_chain::Kind::Call, addr.skin_render_call.addr, batch_at, batch, "Game",
				"Skinning: skinned batch call", kSkinIfReplaced);
		if (setup_ok)
			call_chain::track(call_chain::Kind::Call, addr.skin_reset_call.addr, setup_at, setup, "Game",
				"Skinning: bone setup call", kSkinIfReplaced);
		if (batch_ok && setup_ok)
		{
			g_batch_stubs_live = true;
			shared::common::log("Game", "Skinning: batch markers installed (skinned batch + bone setup)");
		}
		else
		{
			shared::common::log("Game", "Skinning: WARNING - could not redirect the batch call sites",
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}
	}


	// ================================================================
	// Address init
	// ================================================================

	void init_game_addresses()
	{
		// The executable picks its row of the address table. An unknown one
		// leaves every engine pointer null and every engine feature off; the
		// D3D9 side runs as usual.
		const auto& exe = fnv_exe::identity();
		const auto* steamstub = exe.steamstub ? ", SteamStub-wrapped" : "";
		if (exe.variant)
		{
			shared::common::log("Game", std::format("Executable: {}{} (timestamp 0x{:08X}, image 0x{:X}, .rsrc 0x{:016X})",
				exe.variant->name, steamstub, exe.timestamp, exe.size_of_image, exe.rsrc_fnv1a),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		else
		{
			shared::common::log("Game", std::format("UNKNOWN FalloutNV.exe (timestamp 0x{:08X}, image 0x{:X}, .rsrc 0x{:016X}, "
				"store import '{}'{}; no match on: {}). Engine features are OFF: no hooks, patches or engine reads. "
				"Supported: FalloutNV.exe 1.4.0.525 (GOG or Steam).",
				exe.timestamp, exe.size_of_image, exe.rsrc_fnv1a, exe.store_import, steamstub, exe.mismatch),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}

		const auto& addr = fnv_exe::addr();
		renderer_singleton_ptr = addr.renderer ? reinterpret_cast<void**>(addr.renderer) : nullptr;
		camera_position_ptr = addr.camera_position ? reinterpret_cast<float*>(addr.camera_position) : nullptr;

		const auto& l = shared::common::config::get().lights;
		lights_enabled = l.enabled;
		light_intensity = static_cast<float>(l.intensity_percent) / 100.0f;
		light_range_mode = l.range_mode;
		effect_light_forward = static_cast<float>(l.effect_forward);
		effect_light_third_person_forward = static_cast<float>(l.effect_third_person_forward);
		pipboy_light_intensity = static_cast<float>(l.follower_intensity_percent) / 100.0f;

		shared::common::log("Game", std::format("Lights: enabled={} intensity={:.0f}% rangeMode={} Pip-Boy light forward={:.0f} "
			"(third person {:.0f}) intensity={:.0f}%", lights_enabled, light_intensity * 100.0f, light_range_mode,
			effect_light_forward, effect_light_third_person_forward, pipboy_light_intensity * 100.0f));

		// The game-code patches don't run here: this runs from DllMain, which
		// is too early to touch .text (see install_game_patches).
	}


	// ================================================================
	// Game code patches
	//
	// Not from DllMain. d3d9.dll is a static import of FalloutNV.exe, so
	// DllMain runs before the executable's entry point, and on the Steam
	// executable that entry point is SteamStub's .bind stub, which decrypts
	// .text in place. A patch written from DllMain would land in ciphertext
	// and the decrypt would turn it into garbage instructions. These run from
	// the window thread, after the entry point; call_chain checks every site
	// before it is written.
	// ================================================================

	void install_game_patches()
	{
		if (!fnv_exe::known())
		{
			shared::common::log("Game", "Game patches SKIPPED: unknown executable",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			fault_report::report(fault_report::Fault::UnknownExe);
			return;
		}

		install_skinned_batch_markers();

		// Texture-pack height maps: D3D texture -> file path (tex_pack.cpp)
		tex_pack::install_hook();

		// FaceGen face maps stay readable under NVTF's DEFAULT pool (tex_pool.cpp)
		tex_pool::install_hook();

		// The frame's UI boundary: Main_Render's call to the HUD's 3D render (ui_boundary.cpp)
		ui_boundary::install();

		// The Pip-Boy live background's capture guard (menu_bg.cpp)
		menu_bg::install();

		// VATS limb visibility from line of sight (vats.cpp)
		vats::install();

		// Save screenshots from the runtime's thumbnail (save_thumbnail.cpp)
		save_thumbnail::install();
	}

#undef PATTERN_OFFSET_SIMPLE
#undef PATTERN_OFFSET_DWORD_PTR_CAST_TYPE
}
