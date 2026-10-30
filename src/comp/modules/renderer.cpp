#include "std_include.hpp"
#include "renderer.hpp"

#include "diagnostics.hpp"
#include "imgui.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "comp/game/vs_expand.hpp"
#include "shared/common/remix_protocol.hpp"
#include "shared/common/config.hpp"
#include "comp/game/api_submit.hpp"
#include "comp/game/pipboy_cursor.hpp"
#include "comp/game/api_rtxconf.hpp"
#include "comp/game/scene_statics.hpp"
#include "comp/game/ps_protocol.hpp"
#include "comp/game/frame_stats.hpp"
#include "comp/game/stable_origin.hpp"
#include "comp/game/screen_space.hpp"
#include "comp/game/api_camera.hpp"
#include "comp/game/traced.hpp"
#include "comp/game/menu_scene.hpp"
#include "comp/game/engine_class.hpp"
#include "comp/game/ui_boundary.hpp"

/*
 * Draw routing (docs/fnv-api-only.md).
 *
 * Every draw the runtime would ray-trace goes through the Remix API: land,
 * far LOD, water, the vertex-expander families (particles, unlit FX, grass,
 * leaves, distant objects / trees), skinned actors, and, for every other
 * world draw, the scene-graph statics and the draw capture
 * (api_statics.cpp). A traced draw no API path takes is not drawn; it is
 * counted per route and reason (game/traced.*: the "traced->D3D9" status
 * line). The only traced draws that still reach D3D9 (with their FFP
 * conversion) are the allowed exceptions: the Pip-Boy's own first-person
 * draws while it is not closed and every draw at the main menu or while
 * loading (game::api::pipboy_draw), which the API paths refuse, and world /
 * unlit / particle draws whose albedo is in a runtime list the runtime
 * reads only on D3D9 draws (rtx.lightConverter above all:
 * the effect lights of bulbs and neon; game::api::rtxconf::d3d9_only),
 * checked before any API path.
 *
 * D3D9 stays for draws that are never traced: offscreen targets (shadow maps,
 * render targets that are not the back buffer), orthographic / screen-space
 * draws (HUD, menus, the Pip-Boy screen), POSITIONT, draws without a view /
 * projection, and the Ignore-tagged passthroughs (no diffuse role, the
 * with-normal near land LOD). Sky-shader draws are dropped: NUMOS draws the
 * sky.
 */

namespace comp
{
	namespace tex_addons
	{
		bool initialized = false;
		LPDIRECT3DTEXTURE9 berry = nullptr;

		void init_texture_addons(bool release)
		{
			if (release)
			{
				// The pointer is nulled and the flag dropped: D3DXCreateTextureFromFileA
				// leaves *tex untouched when it fails (missing berry.png), so a
				// release without a successful reload would leave a freed pointer
				// for the next Reset / destructor to release again and for
				// tab_about() to draw with.
				if (tex_addons::berry)
				{
					tex_addons::berry->Release();
					tex_addons::berry = nullptr;
				}
				tex_addons::initialized = false;
				return;
			}

			shared::common::log("Renderer", "Loading CompMod Textures ...", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);

			auto load_texture = [](IDirect3DDevice9* dev, const char* path, LPDIRECT3DTEXTURE9* tex)
				{
					HRESULT hr;
					hr = D3DXCreateTextureFromFileA(dev, path, tex);
					if (FAILED(hr)) shared::common::log("Renderer", std::format("Failed to load {}", path), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				};

			const auto dev = shared::globals::d3d_device;
			load_texture(dev, "rtx_comp\\textures\\berry.png", &tex_addons::berry);
			tex_addons::initialized = true;
		}
	}


	drawcall_mod_context& setup_context(IDirect3DDevice9* /*dev*/)
	{
		return renderer::dc_ctx;
	}


	// Material-protocol helpers (game/ps_protocol.*).
	using game::apply_ps_protocol;

	// Route tag for one draw: always counted in the per-frame histogram
	// (game/frame_stats.*), and written to ffp_proxy.log while the
	// diagnostics module is capturing.
	static void route_tag(diagnostics* diag, const char* tag)
	{
		game::frame_stats::note_route(tag);
		if (diag) diag->route(tag);
	}

	// Whether the device's WORLD matrix is identity, so
	// back-to-back passthrough draws skip the redundant SetTransform.
	// fnv_engage clears it (game::apply_transforms writes a non-identity
	// WORLD); device reset re-asserts identity (D3D9 default).
	static bool s_world_is_identity_ = true;

	void renderer_on_reset() { s_world_is_identity_ = true; }

	// FFP engage with the NiDX9Renderer matrices instead of the VS constants:
	// ffp.engage() nulls the shaders and sets up texture stages and lighting,
	// then game::apply_transforms overwrites the transforms. Used by the D3D9
	// world / expander routes.
	static void fnv_engage(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();
		ffp.engage(dev);
		game::apply_transforms(dev);
		s_world_is_identity_ = false;

		// Alpha from the texture alone (SELECTARG1), not modulated by the
		// vertex diffuse, which keeps foliage and cutout alpha.
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	}

	// Restores the game's shaders and sets WORLD back to identity.
	static void fnv_disengage(IDirect3DDevice9* dev)
	{
		auto& ffp = shared::common::ffp_state::get();

		// Bone uploads for a skinned object are swallowed by game::on_set_vs_const_f
		// (captured for the Remix-API skinned path) and never reach the device's
		// c[] file. A passthrough draw with a skinned declaration (offscreen
		// targets: shadow maps, FaceGen renders) runs the game's skinning
		// vertex shader, which reads those registers, so they are flushed.
		if (ffp.cur_decl_is_skinned()) {
			game::flush_bones_to_device(dev);
		}

		ffp.disengage(dev);
		if (!s_world_is_identity_)
		{
			game::stable_origin::set_transform(dev, D3DTS_WORLD, &shared::globals::IDENTITY);
			s_world_is_identity_ = true;
		}
	}

	// Once per PS on the skinned route: the PS's classified roles and whether
	// the pass is dropped (lighting-only) or offered to the API skinned path.
	static void log_skinned_ps_once(IDirect3DDevice9* dev, const shared::common::PsSlotMap* map, bool skipped)
	{
		using shared::common::PsSlotRole;
		static std::unordered_set<uint32_t> s_logged;
		if (!s_logged.insert(map ? map->ps_hash : 0u).second) return;

		if (!map)
		{
			shared::common::log("Skin", "PS none (null / unreadable) -> Remix API",
				shared::common::LOG_TYPE::LOG_TYPE_GREEN);
			return;
		}

		DWORD blend = 0, srcb = 0, dstb = 0;
		dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		dev->GetRenderState(D3DRS_SRCBLEND, &srcb);
		dev->GetRenderState(D3DRS_DESTBLEND, &dstb);

		shared::common::log("Skin", std::format(
			"PS 0x{:08X} diff=s{} norm=s{} glow=s{} height=s{} fg0=s{} fg1=s{} toggles=c{} emit=c{} "
			"| blend={} src={} dst={} -> {}",
			map->ps_hash,
			map->slot(PsSlotRole::Diffuse), map->slot(PsSlotRole::Normal),
			map->slot(PsSlotRole::Glow), map->slot(PsSlotRole::Height),
			map->facegen0_slot, map->facegen1_slot, map->toggles_reg, map->emittance_reg,
			blend, srcb, dstb,
			skipped ? "SKIP (no diffuse: lighting-only pass)" : "Remix API"),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN);
	}


	// Remix-API draw descriptions (see comp/game/api_submit.hpp).
	static game::api::DrawDesc api_desc_indexed(D3DPRIMITIVETYPE type, INT base, UINT min_vtx,
		UINT num_vtx, UINT start_index, UINT prims)
	{
		game::api::DrawDesc d;
		d.type = type;
		d.indexed = true;
		d.base = base;
		d.min_vtx = min_vtx;
		d.num_vtx = num_vtx;
		d.start_index = start_index;
		d.prim_count = prims;
		return d;
	}

	static UINT primitive_vertex_count(D3DPRIMITIVETYPE type, UINT prims);

	static game::api::DrawDesc api_desc_prim(D3DPRIMITIVETYPE type, UINT start_vertex, UINT prims)
	{
		game::api::DrawDesc d;
		d.type = type;
		d.indexed = false;
		d.base = static_cast<INT>(start_vertex);
		d.num_vtx = primitive_vertex_count(type, prims);
		d.prim_count = prims;
		return d;
	}

	// Vertex count of a non-indexed draw, for the DrawPrimitive expander path.
	static UINT primitive_vertex_count(D3DPRIMITIVETYPE type, UINT prims)
	{
		switch (type)
		{
		case D3DPT_POINTLIST:     return prims;
		case D3DPT_LINELIST:      return prims * 2;
		case D3DPT_LINESTRIP:     return prims + 1;
		case D3DPT_TRIANGLELIST:  return prims * 3;
		case D3DPT_TRIANGLESTRIP:
		case D3DPT_TRIANGLEFAN:   return prims + 2;
		default:                  return 0;
		}
	}

	/*
	 * Premultiplied-alpha flames (NOLIGHTTEXVCPMA, PsSlotMap::premultiplied_alpha).
	 *
	 * The PS outputs rgb * a with alpha 1 and the engine blends it ONE / ONE.
	 * FFP stage 0 and the Remix API instance give the runtime rgb and a apart,
	 * and the runtime reads a ONE / ONE surface as emissive with its opacity
	 * taken from colour luminance and alpha unused, so the texture's colour
	 * outside the flame would fill every card (solid fire quads). SRCALPHA /
	 * ONE is the same sum with the mask applied by the blend, and the runtime
	 * reads it as alpha-emissive: opacity and emission scaled by alpha. Called
	 * before any route, so the API paths (draw_state_blend) see it too; the
	 * context puts the game's value back after the draw, as Gamebryo shadows
	 * render state. Raster-only draws (offscreen targets, orthographic /
	 * Pip-Boy) keep the game's blend: there the rasterised colour is what is
	 * shown, and SRCALPHA would apply the alpha twice.
	 */
	static void premultiplied_alpha_blend(IDirect3DDevice9* dev, drawcall_mod_context& ctx,
		const shared::common::PsSlotMap* psmap)
	{
		if (!game::rendering_to_backbuffer || game::is_2d() || !game::engine_class::premultiplied(psmap)) return;
		DWORD blend = FALSE, src = D3DBLEND_ZERO;
		if (FAILED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) || !blend) return;
		if (FAILED(dev->GetRenderState(D3DRS_SRCBLEND, &src)) || src != D3DBLEND_ONE) return;
		ctx.save_rs(dev, D3DRS_SRCBLEND);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	}

	static const char* expand_route_name(const shared::common::VsDesc& xd)
	{
		using shared::common::VsFamily;
		switch (xd.family)
		{
		case VsFamily::ParticleSubTex: return "FFP_EXPAND_PARTICLE";
		case VsFamily::NoLightUv:      return "FFP_NOLIGHT_UVXFORM";
		case VsFamily::Grass:          return "FFP_EXPAND_GRASS";
		case VsFamily::DistLod:        return "FFP_EXPAND_DISTLOD";
		case VsFamily::DistTree:       return "FFP_EXPAND_DISTTREE";
		case VsFamily::Leaf:           return "FFP_EXPAND_LEAF";
		default:                       return "FFP_EXPAND";
		}
	}

	// The route tag of an expander-family draw the Remix API took.
	static const char* api_route_name(const shared::common::VsDesc& xd, bool draw_primitive)
	{
		using shared::common::VsFamily;
		switch (xd.family)
		{
		case VsFamily::Leaf:           return draw_primitive ? "DP_API_LEAF" : "API_LEAF";
		case VsFamily::Grass:          return draw_primitive ? "DP_API_GRASS" : "API_GRASS";
		case VsFamily::DistLod:        return draw_primitive ? "DP_API_DISTLOD" : "API_DISTLOD";
		case VsFamily::DistTree:       return draw_primitive ? "DP_API_DISTTREE" : "API_DISTTREE";
		case VsFamily::NoLightUv:      return draw_primitive ? "DP_API_UNLIT" : "API_UNLIT";
		case VsFamily::ParticleSubTex: return draw_primitive ? "DP_API_PARTICLE" : "API_PARTICLE";
		default:                       return draw_primitive ? "DP_API_EXPAND" : "API_EXPAND";
		}
	}

	/*
	 * The Pip-Boy screen.
	 *
	 * The engine renders the Pip-Boy interface into an offscreen render target
	 * (1280x800 at a 2560x1600 backbuffer) and lays it onto the screen mesh
	 * with the NOLIGHT FX shader (PS 0x815B7F80, the FFP_NOLIGHT_UVXFORM route),
	 * blended SRCALPHA / ONE over the screen's own opaque passes (a 64x64 DXT1,
	 * same mesh). The runtime reads that additive pass as alpha-emissive at the
	 * global rtx.emissiveBlendOverrideEmissiveIntensity (0.2, sized for
	 * particles) on top of a base it lights like any diffuse grey surface, so
	 * sun or a lamp on the Pip-Boy buries the interface.
	 *
	 * Contents. Tagged WorldUI (RS 42), the runtime makes the pass emit its own
	 * texture at rtx.worldSpaceUiEmissiveIntensity instead (rtx_instance_manager.cpp,
	 * world-space UI; 2.0 upstream, pushed to kWorldUiEmissive here, see
	 * push_world_ui_emissive), still scaled by the surface alpha of the
	 * alpha-emissive blend and still with no opacity of its own. Stage 0 sets
	 * the brightness to [PipBoy] ScreenEmissive:
	 *   colour = TEXTURE + TEXTURE  (2 x the contents; vertex colour dropped)
	 *   alpha  = TEXTURE x TFACTOR  (TFACTOR alpha = ScreenEmissive / max)
	 * so the emission is contents x contents.a x ScreenEmissive, max being
	 * 2 x the runtime intensity.
	 *
	 * Glass. The opaque base passes are the dark CRT glass behind the text:
	 *   colour = TEXTURE x TFACTOR  (TFACTOR rgb = [PipBoy] ScreenGlassBrightness,
	 *                                a touch of green; vertex colour dropped)
	 *   RS 149 / 169: a protocol material on stage 0 with gloss [PipBoy]
	 *   ScreenGlassGloss (default 10, roughness ~0.64), so the runtime makes
	 *   it glossy and non-metallic.
	 * Sun and sky then light a near-black surface and the emitted contents
	 * read against it. A glass pass is an opaque Pip-Boy NOLIGHT draw on the
	 * mesh the contents were last drawn on (stream-0 buffer and offset, base
	 * vertex, vertex window), or whose albedo is the vanilla screen texture
	 * (kGlassTextureHash, the runtime's hash of it).
	 *
	 * Smooth normals. The NOLIGHT declaration has no NORMAL, so the runtime
	 * would shade the low-poly screen (56 vertices, 104 triangles) with flat
	 * face normals, and the glossy glass would show every facet as a flat band
	 * in the sky's reflection. Glass and contents draws bind a copy of the mesh with
	 * smoothed normals instead (vs_expand::bind_smooth_normals: positions
	 * welded, area-weighted, a 50-degree crease, built once per mesh), so the
	 * curved screen reflects as one surface; the copy replaces the MaterialColor
	 * fold, which neither stage 0 below reads (both drop the vertex colour).
	 *
	 * These draws are ray-traced, not rasterised: the stage states only reach
	 * the runtime.
	 */
	constexpr float kWorldUiEmissive = 8.0f;          // pushed rtx.worldSpaceUiEmissiveIntensity
	constexpr float kWorldUiEmissiveDefault = 2.0f;   // the runtime's own (no push / older runtime)
	constexpr uint64_t kGlassTextureHash = 0x4DA1D6FC344DD726ull;
	// [PipBoy] ScreenGlassGloss: a Blinn-Phong exponent, GGX alpha =
	// sqrt(2 / (g + 2)), roughness = sqrt(alpha) (rtx_fork_fnv_material.cpp):
	// 10 is roughness 0.64, 200 is 0.32, 500 is 0.25, 1000 is 0.21.
	constexpr float kGlassGlossMin = 10.0f;
	constexpr float kGlassGlossMax = 1000.0f;

	// The contents' mesh, as last drawn (see the glass above).
	struct ScreenMesh
	{
		IDirect3DVertexBuffer9* vb = nullptr;
		UINT offset = 0;
		INT base = 0;
		UINT skip = 0;
		UINT count = 0;
		bool operator==(const ScreenMesh&) const = default;
	};
	static ScreenMesh s_screen_mesh;

	static bool s_world_ui_pushed = false;

	// Once, at the first screen-contents draw: rtx.worldSpaceUiEmissiveIntensity
	// = kWorldUiEmissive, so ScreenEmissive reaches 2 x that. A runtime without
	// the option keeps 2.0; the wrapper then caps ScreenEmissive at 4.
	static void push_world_ui_emissive()
	{
		static bool s_tried = false;
		if (s_tried) return;
		auto& api = shared::common::remix_api::get();
		if (!shared::common::remix_api::is_initialized() || !api.m_bridge.SetConfigVariable) return;
		s_tried = true;
		const std::string value = std::format("{:.2f}", kWorldUiEmissive);
		s_world_ui_pushed = api.m_bridge.SetConfigVariable("rtx.worldSpaceUiEmissiveIntensity", value.c_str())
			== REMIXAPI_ERROR_CODE_SUCCESS;
		shared::common::log("PipBoy", std::format("rtx.worldSpaceUiEmissiveIntensity = {} {} (a runtime without the option "
			"keeps 2.0 and the screen tops out at ScreenEmissive 4)", value, s_world_ui_pushed ? "pushed" : "push FAILED"),
			s_world_ui_pushed ? shared::common::LOG_TYPE::LOG_TYPE_STATUS : shared::common::LOG_TYPE::LOG_TYPE_WARN);
	}

	static float screen_emissive_max()
	{
		return 2.0f * (s_world_ui_pushed ? kWorldUiEmissive : kWorldUiEmissiveDefault);
	}

	static bool is_render_target_texture(IDirect3DBaseTexture9* tex)
	{
		if (!tex || tex->GetType() != D3DRTYPE_TEXTURE) return false;
		D3DSURFACE_DESC d;
		return SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d)) &&
			(d.Usage & D3DUSAGE_RENDERTARGET) != 0;
	}

	// The stage-0 texture apply_ps_protocol leaves bound: the PS's diffuse
	// texture when it names one and the game bound it, else the AlbedoStage pick.
	static IDirect3DBaseTexture9* protocol_albedo(const shared::common::ffp_state& ffp,
		const shared::common::PsSlotMap* psmap)
	{
		using shared::common::PsSlotMap;
		const uint8_t slot = psmap && psmap->any_classified
			? psmap->slot(shared::common::PsSlotRole::Diffuse) : PsSlotMap::kNoSlot;
		IDirect3DBaseTexture9* tex = slot != PsSlotMap::kNoSlot ? ffp.cur_texture(slot) : nullptr;
		return tex ? tex : ffp.last_albedo_stage0();
	}

	// A Pip-Boy NOLIGHT draw that blends a render-target texture over the mesh.
	static bool is_pipboy_screen_contents(IDirect3DDevice9* dev, const shared::common::ffp_state& ffp,
		const shared::common::PsSlotMap* psmap)
	{
		DWORD blend = FALSE;
		if (FAILED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) || !blend) return false;
		IDirect3DBaseTexture9* tex = protocol_albedo(ffp, psmap);
		if (!is_render_target_texture(tex)) return false;

		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			D3DSURFACE_DESC d = {};
			static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d);
			DWORD src = 0, dst = 0;
			dev->GetRenderState(D3DRS_SRCBLEND, &src);
			dev->GetRenderState(D3DRS_DESTBLEND, &dst);
			shared::common::log("PipBoy", std::format(
				"screen contents: PS 0x{:08X} samples a {}x{} render target (blend {}/{}) -- tagged WorldUI, emits its "
				"contents at [PipBoy] ScreenEmissive={:.2f}",
				psmap ? psmap->ps_hash : 0u, d.Width, d.Height, src, dst,
				shared::common::config::get().pipboy.screen_emissive),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		return true;
	}

	// Stage 0 for the screen contents (see above). Returns the game's
	// TEXTUREFACTOR for screen_emissive_restore.
	static DWORD screen_emissive_stage0(IDirect3DDevice9* dev)
	{
		DWORD saved = 0xFFFFFFFFu;
		dev->GetRenderState(D3DRS_TEXTUREFACTOR, &saved);
		const float top = screen_emissive_max();
		const float intensity = std::clamp(shared::common::config::get().pipboy.screen_emissive, 0.0f, top);
		const DWORD a = static_cast<DWORD>(intensity / top * 255.0f + 0.5f);
		dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(a, 255, 255, 255));
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
		return saved;
	}

	// Back to ffp_state::setup_texture_stages' stage 0 (it caches that state
	// and won't rewrite it). ALPHAOP is rewritten by every fnv_engage.
	static void screen_emissive_restore(IDirect3DDevice9* dev, DWORD saved_tfactor)
	{
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		dev->SetRenderState(D3DRS_TEXTUREFACTOR, saved_tfactor);
	}

	// The runtime's hash of `tex` is the vanilla screen glass texture. One-entry
	// cache: only 64x64 DXT1 textures are ever hashed (a 2 KB read-only lock
	// the bridge client answers from its shadow).
	static bool is_glass_texture(IDirect3DBaseTexture9* tex)
	{
		static IDirect3DBaseTexture9* s_tex = nullptr;
		static bool s_match = false;
		if (tex == s_tex) return s_match;
		s_tex = tex;
		s_match = false;
		if (!tex || tex->GetType() != D3DRTYPE_TEXTURE) return false;
		D3DSURFACE_DESC d = {};
		if (FAILED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d)) ||
			d.Format != D3DFMT_DXT1 || d.Width != 64 || d.Height != 64)
			return false;
		uint64_t h = 0;
		s_match = game::api::rtxconf::d3d9_image_hash(tex, h) && h == kGlassTextureHash;
		return s_match;
	}

	// An opaque Pip-Boy NOLIGHT draw of the screen glass (see above).
	static bool is_pipboy_screen_glass(IDirect3DDevice9* dev, const shared::common::ffp_state& ffp,
		const shared::common::PsSlotMap* psmap, const ScreenMesh& mesh)
	{
		DWORD blend = FALSE;
		if (FAILED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) || blend) return false;
		IDirect3DBaseTexture9* tex = protocol_albedo(ffp, psmap);
		if (!tex || is_render_target_texture(tex)) return false;
		const bool same_mesh = mesh.vb && mesh == s_screen_mesh;
		const bool glass_tex = !same_mesh && is_glass_texture(tex);
		if (!same_mesh && !glass_tex) return false;

		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			shared::common::log("PipBoy", std::format(
				"screen glass: PS 0x{:08X}, matched by {} -- dark glossy glass at [PipBoy] ScreenGlassBrightness={:.2f}",
				psmap ? psmap->ps_hash : 0u, same_mesh ? "the contents' mesh" : "the vanilla glass texture",
				shared::common::config::get().pipboy.screen_glass_brightness),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		return true;
	}

	// Stage 0 and the protocol material for the glass (see above). Call after
	// apply_ps_protocol (it replaces that payload). Returns the game's
	// TEXTUREFACTOR for screen_glass_restore.
	static DWORD screen_glass_stage0(IDirect3DDevice9* dev)
	{
		DWORD saved = 0xFFFFFFFFu;
		dev->GetRenderState(D3DRS_TEXTUREFACTOR, &saved);
		const float b = std::clamp(shared::common::config::get().pipboy.screen_glass_brightness, 0.0f, 1.0f);
		auto u8 = [](float v) { return static_cast<DWORD>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
		dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, u8(b * 0.8f), u8(b), u8(b * 0.85f)));
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);

		remix_protocol::Payload p;
		p.modifier = remix_protocol::encode_slot_roles(0, remix_protocol::kSlotAbsent);
		p.gloss = remix_protocol::float_bits(std::clamp(shared::common::config::get().pipboy.screen_glass_gloss,
			kGlassGlossMin, kGlassGlossMax));
		remix_protocol::submit(dev, p);
		return saved;
	}

	static void screen_glass_restore(IDirect3DDevice9* dev, DWORD saved_tfactor)
	{
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
		dev->SetRenderState(D3DRS_TEXTUREFACTOR, saved_tfactor);
	}

	// Binds the screen mesh with smooth normals (see above) for a glass or
	// contents draw. The first few builds are logged, and the first refusal
	// of each pass.
	static game::vs_expand::Bind bind_screen_normals(IDirect3DDevice9* dev, const game::api::DrawDesc& draw,
		bool contents)
	{
		game::vs_expand::SmoothNormalsInfo info;
		const char* reject = nullptr;
		const auto b = game::vs_expand::bind_smooth_normals(dev, draw, info, reject);
		static int s_built_logs = 0;
		static bool s_refusal_logged[2] = {};
		if (b == game::vs_expand::Bind::Bound && info.built && s_built_logs < 4)
		{
			s_built_logs++;
			shared::common::log("PipBoy", std::format(
				"screen glass: smooth normals for {} verts ({} faces) -- {} welded positions, crease {:.0f} deg, "
				"max bend {:.1f} deg between a vertex normal and its own faces",
				info.verts, info.faces, info.welded, game::vs_expand::kSmoothCreaseDeg, info.max_bend_deg),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}
		else if (b != game::vs_expand::Bind::Bound && !s_refusal_logged[contents ? 1 : 0])
		{
			s_refusal_logged[contents ? 1 : 0] = true;
			shared::common::log("PipBoy", std::format("screen {}: no smooth normals ({}) -- this pass keeps flat face "
				"normals", contents ? "contents" : "glass", reject ? reject : "?"), shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}
		return b;
	}

	/*
	 * One draw of a vertex-expander family (see game/vs_expand.hpp): flipbook
	 * particles, the NOLIGHT TexCoordTranform FX, instanced grass / distant
	 * objects / distant trees, SpeedTree leaves.
	 *
	 * The Remix API takes it (NoLightUv: the particle path for a dynamic VB,
	 * then the unlit path; ParticleSubTex: the particle path; the rest: the
	 * foliage path) or it is not drawn. Only a NoLightUv / ParticleSubTex
	 * draw the API refuses as the Pip-Boy's own (its screen FX) takes
	 * the D3D9 expander route: the expanded copy (or, for NoLightUv, the
	 * game's vertices under a stage-0 texture matrix) through FFP. So does
	 * every NoLightUv / ParticleSubTex draw whose albedo is in a runtime list
	 * only D3D9 draws can carry (rtx.lightConverter, ...:
	 * rtxconf::d3d9_only), without trying the API.
	 *
	 * `draw(rebased)` issues the game's draw call, with the vertex window
	 * rebased to 0 when rebased is true. Returns the route tag.
	 */
	template <typename DrawFn>
	static const char* draw_expanded(IDirect3DDevice9* dev, const shared::common::VsDesc& xd,
		const shared::common::PsSlotMap* psmap, INT base, UINT skip, UINT count, DrawFn&& draw, HRESULT& hr,
		const game::api::DrawDesc& api, bool draw_primitive)
	{
		using shared::common::VsFamily;
		auto& ffp = shared::common::ffp_state::get();
		hr = S_OK;
		const bool unlit = xd.family == VsFamily::NoLightUv;
		const bool particle = xd.family == VsFamily::ParticleSubTex;

		if (!unlit && !particle)
		{
			// Grass, leaves, distant objects / trees.
			const auto f = game::api_submit::try_foliage(dev, xd, psmap, api);
			if (f == game::api_submit::Foliage::Taken) return api_route_name(xd, draw_primitive);
			// Every instance faded out: the game's own output is empty.
			if (f == game::api_submit::Foliage::Culled) return draw_primitive ? "DP_API_FOLIAGE_CULLED" : "API_FOLIAGE_CULLED";
			game::traced::note_dropped(expand_route_name(xd), "foliage refused");
			return draw_primitive ? "DP_DROP_EXPAND" : "DROP_EXPAND";
		}

		// A particle system the engine streams into a dynamic VB (fire,
		// embers) goes to the particle path first (it re-sends the quads
		// every frame, which the unlit path's built-once meshes can't), and
		// the unlit path takes the rest (try_particles passes over NoLightUv
		// draws from static buffers without counting them).
		// An albedo in a runtime list only D3D9 draws can carry
		// (rtx.lightConverter: the effect light of a bulb, a neon tube, a
		// flame) skips the API paths and takes the D3D9 expander route below,
		// in or out of the Pip-Boy.
		const char* d3d9_list = game::api::rtxconf::d3d9_only(protocol_albedo(ffp, psmap));
		if (!d3d9_list && (game::api_submit::try_particles(dev, xd, psmap, api) ||
			(unlit && game::api_submit::try_unlit(dev, xd, psmap, api))))
			return api_route_name(xd, draw_primitive);

		const char* route = expand_route_name(xd);
		if (!d3d9_list && !game::api::pipboy_draw())
		{
			game::traced::note_dropped(route, unlit ? "unlit refused" : "particles refused");
			return draw_primitive ? "DP_DROP_EXPAND" : "DROP_EXPAND";
		}

		// The Pip-Boy's FX, or a D3D9-only albedo: the D3D9 expander route.
		game::traced::note_d3d9(route, d3d9_list ? d3d9_list : "pipboy");
		// The game's mesh, before bind_expanded swaps in a copy (screen glass).
		const ScreenMesh mesh = { ffp.stream_vb(0), ffp.stream_offset(0), base, skip, count };
		// ParticleSubTex: the flipbook copy, or the game's own draw if the
		// expander can't build it.
		auto b = game::vs_expand::Bind::Declined;
		if (particle)
		{
			b = game::vs_expand::bind_expanded(dev, xd, psmap, base, skip, count);
			if (b != game::vs_expand::Bind::Bound)
			{
				fnv_disengage(dev);
				remix_protocol::none(dev);
				hr = draw(false);
				return draw_primitive ? "DP_PASS_EXPAND_DECLINED" : "PASS_EXPAND_DECLINED";
			}
		}

		game::vs_expand::note_family_used(xd, route);
		fnv_engage(dev);
		game::screen_space::note_ffp_draw(dev, route, count, 0u);
		ffp.setup_albedo_texture(dev, psmap);
		if (unlit)
		{
			D3DMATRIX m;
			ffp.set_stage0_tex_transform(dev, game::vs_expand::nolight_uv_matrix(xd, m) ? &m : nullptr);
		}
		// NOLIGHT PSs output alpha = tex.a * COLOR.a (* MaterialColor.a, folded
		// into COLOR for particles). fnv_engage's SELECTARG1 would drop the
		// vertex alpha, which is how particles fade over their lifetime.
		// ALPHAARG2 is already DIFFUSE from setup_texture_stages; fnv_engage
		// rewrites ALPHAOP on the next FFP draw, so this cannot leak.
		if (ffp.cur_decl_has_color())
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		// The screen's contents: emitted, at [PipBoy] ScreenEmissive; its
		// opaque base passes: dark glossy glass (see is_pipboy_screen_contents).
		const bool screen = unlit && !d3d9_list && is_pipboy_screen_contents(dev, ffp, psmap);
		if (screen)
		{
			s_screen_mesh = mesh;
			push_world_ui_emissive();
		}
		const bool glass = unlit && !screen && !d3d9_list && is_pipboy_screen_glass(dev, ffp, psmap, mesh);
		// NoLightUv: the screen mesh with smooth normals (glass and contents).
		// Otherwise out = tex * COLOR * MaterialColor, but stage 0 modulates
		// the texture by one of them only: a MaterialColor that is not white
		// is folded into the COLOR of a copy, else the game's vertices are
		// drawn as they are.
		if (unlit)
		{
			if (screen || glass) b = bind_screen_normals(dev, api, screen);
			if (b != game::vs_expand::Bind::Bound) b = game::vs_expand::bind_expanded(dev, xd, psmap, base, skip, count);
		}
		const bool bound = b == game::vs_expand::Bind::Bound;
		apply_ps_protocol(dev, psmap,
			particle ? remix_protocol::category_mask(remix_protocol::CategoryBit::Particle)
			: screen ? remix_protocol::category_mask(remix_protocol::CategoryBit::WorldUI)
			: 0u);
		const DWORD saved_tfactor = screen ? screen_emissive_stage0(dev)
			: glass ? screen_glass_stage0(dev) : 0u;

		hr = draw(bound);

		if (screen) screen_emissive_restore(dev, saved_tfactor);
		else if (glass) screen_glass_restore(dev, saved_tfactor);
		if (bound) game::vs_expand::unbind(dev);
		ffp.restore_textures(dev);
		return route;
	}

	/*
	 * A traced world draw that keeps its FFP conversion: the NiDX9Renderer
	 * transforms, the PS classifier's albedo at stage 0 and the protocol
	 * payload. `reason` (counted as d3d9=route:reason) is "pipboy" (the
	 * draw capture refused it as the Pip-Boy's own draw) or the runtime
	 * list that makes its albedo D3D9-only (rtxconf::d3d9_only:
	 * rtx.lightConverter, ...), which the renderer checks before the scene
	 * graph statics and the draw capture. Every pass of such an object goes
	 * through D3D9. `route` is what the draw is
	 * (FFP_WORLD, FFP_BI, FFP_TERRAIN, PASS_NO_NORMAL, DP_FFP, ...). Returns
	 * the route tag.
	 */
	template <typename DrawFn>
	static const char* d3d9_world_draw(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap,
		const char* route, const char* reason, UINT verts, UINT prims, DrawFn&& draw, HRESULT& hr)
	{
		game::traced::note_d3d9(route, reason);
		auto& ffp = shared::common::ffp_state::get();
		fnv_engage(dev);
		game::screen_space::note_ffp_draw(dev, route, verts, prims);
		ffp.setup_albedo_texture(dev, psmap);
		apply_ps_protocol(dev, psmap, 0u, true);
		hr = draw();
		ffp.restore_textures(dev);
		return route;
	}

	/*
	 * A traced world draw the draw capture refused: not drawn, except
	 * "pipboy" (d3d9_world_draw).
	 */
	template <typename DrawFn>
	static const char* refused_world_draw(IDirect3DDevice9* dev, const shared::common::PsSlotMap* psmap,
		const char* route, const char* why, UINT verts, UINT prims, DrawFn&& draw, HRESULT& hr)
	{
		hr = S_OK;
		if (!why || std::strcmp(why, "pipboy") != 0)
		{
			game::traced::note_dropped(route, why ? why : "refused");
			return "DROP_WORLD";
		}
		return d3d9_world_draw(dev, psmap, route, "pipboy", verts, prims, std::forward<DrawFn>(draw), hr);
	}


	/*
	 * A draw of a menu's own 3D scene (game/menu_scene.hpp): the game's
	 * shaders straight to its target, in a frame the runtime rasterises as
	 * drawn. None of the world routes or camera hooks see it.
	 */
	template <typename DrawFn>
	static HRESULT menu_scene_draw(IDirect3DDevice9* dev, const char* route, DrawFn&& draw)
	{
		game::frame_stats::note_route(route);
		fnv_disengage(dev);
		remix_protocol::none(dev);
		return draw();
	}

	/*
	 * A draw after the frame's UI boundary (game/ui_boundary.hpp): the 2D
	 * interface, rasterised as the game draws it. The runtime's RTX injection
	 * runs before the first one to the back buffer; until it has run, a
	 * screen-space draw is still tagged Hidden so it is never traced.
	 */
	template <typename DrawFn>
	static HRESULT ui_draw(IDirect3DDevice9* dev, const char* route, UINT verts, UINT prims, DrawFn&& draw)
	{
		game::menu_scene::before_draw(dev, false);
		game::ui_boundary::before_draw(dev);
		game::frame_stats::note_route(route);
		fnv_disengage(dev);
		const bool screen_space = game::rendering_to_backbuffer && game::is_2d();
		if (screen_space && !game::ui_boundary::injected())
			game::screen_space::tag_draw(dev, route, verts, prims);
		else
			remix_protocol::none(dev);
		// The Pip-Boy's cursor is drawn where the game's pick lands (game/pipboy_cursor.hpp).
		const bool cursor = screen_space && game::pipboy_cursor::before_2d_draw(dev);
		const HRESULT hr = draw();
		if (cursor) game::pipboy_cursor::after_2d_draw(dev);
		return hr;
	}

	HRESULT renderer::on_draw_primitive(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const UINT& StartVertex, const UINT& PrimitiveCount)
	{
		PROFILE_ZONE();
		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			remix_protocol::none(dev);
			return dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		}

		static auto im = imgui::get();
		im->m_stats._drawcall_prim_incl_ignored.track_single();

		auto& ctx = setup_context(dev);
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();

		if (auto* d = diagnostics::get()) {
			d->harvest_draw();
			d->on_draw_primitive(ffp.draw_call_count(), PrimitiveType, StartVertex, PrimitiveCount);
		}

		// The HUD and interface after the frame's UI boundary: rasterised as drawn.
		if (game::ui_boundary::in_ui())
		{
			const auto hr = ui_draw(dev, "DP_PASS_UI", primitive_vertex_count(PrimitiveType, PrimitiveCount), PrimitiveCount,
				[&]() { return dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); });
			ctx.restore_all(dev);
			ctx.reset_context();
			return hr;
		}

		// A menu's own 3D scene (the lockpick's lock): on screen, never traced.
		const bool menu_3d = game::menu_scene::draw_is_menu_scene();
		game::menu_scene::before_draw(dev, menu_3d);
		if (menu_3d)
		{
			const auto hr = menu_scene_draw(dev, "DP_PASS_MENU_3D",
				[&]() { return dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); });
			ctx.restore_all(dev);
			ctx.reset_context();
			return hr;
		}

		// The frame's first world draw: its camera (D), world FOV, camera
		// forward, lights, before the Remix camera below takes D.
		game::on_world_draw(dev);
		// The Pip-Boy's own draws ask for the runtime's per-draw path.
		game::api::scope_per_draw_path();
		// [RemixApi] FallbackCamera (see DrawIndexedPrimitive).
		game::api_camera::on_draw(dev);

		// Drop FNV fake-shadow overlays (see DrawIndexedPrimitive note).
		if (game::should_skip_fake_shadow(dev, static_cast<INT>(StartVertex), PrimitiveCount * 3, ffp.last_ps()))
		{
			game::frame_stats::note_route("DP_SKIP_FAKE_SHADOW");
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		// One classifier lookup per draw, shared by every route below (a PS
		// without a constant table reads as the engine pass's vanilla layout).
		const auto* psmap = game::engine_class::ps_map(shared::common::g_ps_classifier.classify(ffp.last_ps()));
		premultiplied_alpha_blend(dev, ctx, psmap);

		auto hr = S_OK;
		const UINT nverts = primitive_vertex_count(PrimitiveType, PrimitiveCount);
		const auto desc = api_desc_prim(PrimitiveType, StartVertex, PrimitiveCount);
		auto draw = [&]() { return dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); };

		/*
		 * FNV DrawPrimitive routing:
		 *   offscreen RT / no view-proj / orthographic / POSITIONT -> passthrough (never traced)
		 *   sky                     -> dropped (NUMOS draws the sky)
		 *   skinned                 -> not drawn (the skinned path is indexed only)
		 *   water / expander family -> their API paths
		 *   no diffuse role, near land LOD -> passthrough tagged Ignore (never traced)
		 *   far land LOD            -> the API far-LOD path
		 *   else                    -> draw capture (api_statics.cpp)
		 */
		const bool world = ffp.is_enabled() && ffp.view_proj_valid() && game::rendering_to_backbuffer && !game::is_2d();
		if (!world || ffp.cur_decl_has_pos_t())
		{
			const bool screen_space = game::rendering_to_backbuffer && game::is_2d();
			game::frame_stats::note_route(screen_space ? "DP_PASS_2D"
				: !game::rendering_to_backbuffer ? "DP_PASS_OFFSCREEN_RT"
				: ffp.cur_decl_has_pos_t() ? "DP_PASS_POSTT" : "DP_PASS_NO_VP");
			fnv_disengage(dev);
			// An orthographic draw on screen: as PASS_2D (game/screen_space.hpp).
			if (screen_space)
				game::screen_space::tag_draw(dev, "DP_PASS_2D", nverts, PrimitiveCount);
			else
				remix_protocol::none(dev);
			// The Pip-Boy's cursor is drawn where the game's pick lands (game/pipboy_cursor.hpp).
			const bool cursor = screen_space && game::pipboy_cursor::before_2d_draw(dev);
			hr = draw();
			if (cursor) game::pipboy_cursor::after_2d_draw(dev);
			im->m_stats._drawcall_prim.track_single();
			im->m_stats._drawcall_using_vs.track_single();
		}
		else if (game::is_sky())
		{
			// See the indexed path: NUMOS draws the sky.
			game::frame_stats::note_route("DP_SKIP_GAME_SKY");
		}
		else if (!ffp.last_decl())
		{
			// No declaration seen yet (an FVF draw before any
			// SetVertexDeclaration): nothing to describe it with. Passthrough,
			// counted.
			game::traced::note_d3d9("DP_PASS", "no declaration");
			game::frame_stats::note_route("DP_PASS");
			fnv_disengage(dev);
			remix_protocol::none(dev);
			hr = draw();
			im->m_stats._drawcall_prim.track_single();
			im->m_stats._drawcall_using_vs.track_single();
		}
		else if (ffp.cur_decl_is_skinned())
		{
			game::traced::note_dropped("DP_SKINNED", "non-indexed skinned draw");
			game::frame_stats::note_route("DP_DROP_SKINNED");
		}
		else if (const auto water = game::engine_class::water(psmap);
			water != game::engine_class::Water::None && game::api_submit::try_water(dev, psmap, water, desc))
		{
			// API water: the surface as a cached translucent mesh (or a dropped
			// point-light specular pass, or a water draw not drawn this frame).
			game::frame_stats::note_route("DP_API_WATER");
			im->m_stats._drawcall_prim.track_single();
		}
		else if (const auto* expand = game::vs_expand::candidate(psmap))
		{
			game::frame_stats::note_route(draw_expanded(dev, *expand, psmap, static_cast<INT>(StartVertex), 0u, nverts,
				[&](bool rebased) { return dev->DrawPrimitive(PrimitiveType, rebased ? 0u : StartVertex, PrimitiveCount); },
				hr, desc, true));
			im->m_stats._drawcall_prim.track_single();
		}
		else if (!game::engine_class::carries_base(psmap) ||
			(ffp.cur_decl_has_normal() && game::engine_class::land_lod(psmap, true)))
		{
			// No diffuse role identified: FX / normal-only / environment-cubemap
			// shaders. The with-normal near land LOD: its VS fades it out near
			// the player. Rasterised for the screen, tagged Ignore so the path
			// tracer skips them (InstanceCategories::Ignore via RS 42).
			game::frame_stats::note_route("DP_PASS_NO_DIFFUSE_OR_LOD");
			fnv_disengage(dev);
			remix_protocol::ignore(dev);
			hr = draw();
			im->m_stats._drawcall_prim.track_single();
			im->m_stats._drawcall_using_vs.track_single();
		}
		else if (!ffp.cur_decl_has_normal() && game::engine_class::land_lod(psmap, false))
		{
			// The no-normal far land LOD.
			game::lod_debug::count_far();
			if (!game::api_submit::try_far_lod(dev, psmap, desc)) game::traced::note_dropped("DP_FAR_LOD", "far LOD refused");
			game::frame_stats::note_route("DP_API_FAR_LOD");
		}
		else if (const char* d3d9_list = game::api::rtxconf::d3d9_only(protocol_albedo(ffp, psmap)))
		{
			// A D3D9-only albedo (rtx.lightConverter, ...): the D3D9 route.
			game::frame_stats::note_route(d3d9_world_draw(dev, psmap,
				ffp.cur_decl_has_normal() ? "DP_FFP" : "DP_PASS_NO_NORMAL", d3d9_list, nverts, PrimitiveCount, draw, hr));
			im->m_stats._drawcall_prim.track_single();
		}
		else
		{
			const char* why = nullptr;
			const auto cap = game::api_submit::try_static(dev, psmap, desc, why);
			if (cap != game::api_submit::Capture::Refused)
				game::frame_stats::note_route(cap == game::api_submit::Capture::Taken ? "DP_API_STATIC" : "DP_API_STATIC_SKIP_PASS");
			else
				game::frame_stats::note_route(refused_world_draw(dev, psmap,
					ffp.cur_decl_has_normal() ? "DP_FFP" : "DP_PASS_NO_NORMAL", why, nverts, PrimitiveCount, draw, hr));
			im->m_stats._drawcall_prim.track_single();
		}

		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}


	HRESULT renderer::on_draw_indexed_prim(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const INT& BaseVertexIndex, const UINT& MinVertexIndex, const UINT& NumVertices, const UINT& startIndex, const UINT& primCount)
	{
		PROFILE_ZONE();
		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			remix_protocol::none(dev);
			return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		}

		auto& ctx = setup_context(dev);
		static auto im = imgui::get();
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();

		if (auto* d = diagnostics::get()) {
			d->harvest_draw();
			d->on_draw_indexed_prim(ffp.draw_call_count(), dev, PrimitiveType, BaseVertexIndex, NumVertices, primCount);
		}

		im->m_stats._drawcall_indexed_prim_incl_ignored.track_single();

		// The HUD and interface after the frame's UI boundary: rasterised as drawn.
		if (game::ui_boundary::in_ui())
		{
			const auto hr = ui_draw(dev, "PASS_UI", NumVertices, primCount, [&]() {
				return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			});
			ctx.restore_all(dev);
			ctx.reset_context();
			return hr;
		}

		// A menu's own 3D scene (the lockpick's lock): on screen, never traced.
		const bool menu_3d = game::menu_scene::draw_is_menu_scene();
		game::menu_scene::before_draw(dev, menu_3d);
		if (menu_3d)
		{
			const auto hr = menu_scene_draw(dev, "PASS_MENU_3D", [&]() {
				return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			});
			ctx.restore_all(dev);
			ctx.reset_context();
			return hr;
		}

		// The frame's first world draw: its camera (D), world FOV, camera
		// forward, lights, before the Remix camera below takes D.
		game::on_world_draw(dev);
		// The Pip-Boy's own draws ask for the runtime's per-draw path.
		game::api::scope_per_draw_path();
		// [RemixApi] FallbackCamera: the frame's world camera, at its first
		// world draw, whichever route the draw then takes.
		game::api_camera::on_draw(dev);

		// Drop FNV fake-shadow overlays (NOLIGHTING + all-gray vertex colors);
		// Remix ray-traces real shadows. Keep-listed PSes (Pip-Boy) are spared.
		if (game::should_skip_fake_shadow(dev, BaseVertexIndex, NumVertices, ffp.last_ps()))
		{
			game::frame_stats::note_route("SKIP_FAKE_SHADOW");
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		// One classifier lookup per draw, shared by every route below (a PS
		// without a constant table reads as the engine pass's vanilla layout).
		const auto* psmap = game::engine_class::ps_map(shared::common::g_ps_classifier.classify(ffp.last_ps()));
		premultiplied_alpha_blend(dev, ctx, psmap);

		auto hr = S_OK;
		const auto desc = api_desc_indexed(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		auto draw = [&]() {
			return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		};

		/*
		 * FNV DrawIndexedPrimitive routing:
		 *
		 *   !renderingToBackbuffer  -> passthrough (shadow maps, FaceGen renders)
		 *   no view-proj            -> passthrough
		 *   is2D (orthographic)     -> passthrough, tagged Hidden (HUD, menus, Pip-Boy screen)
		 *   skinned                 -> the API skinned path (lighting-only passes dropped)
		 *   POSITIONT               -> passthrough (screen-space pre-transformed)
		 *   sky                     -> dropped (NUMOS draws the sky)
		 *   water / expander family -> their API paths
		 *   no diffuse role         -> passthrough tagged Ignore
		 *   near land LOD           -> dropped
		 *   far land LOD / land     -> the API far-LOD / terrain paths
		 *   else                    -> scene-graph statics (recorded), then draw capture
		 */
		auto* diag = diagnostics::get();

		if (!game::rendering_to_backbuffer)
		{
			PROFILE_ZONE_N("route_PASS_OFFSCREEN_RT");
			route_tag(diag, "PASS_OFFSCREEN_RT");
			fnv_disengage(dev);
			remix_protocol::none(dev);
			hr = draw();
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (!ffp.is_enabled() || !ffp.view_proj_valid())
		{
			PROFILE_ZONE_N("route_PASS_NO_VP");
			route_tag(diag, "PASS_NO_VP");
			// Disengages like every other passthrough route. FFP stays engaged
			// across draws and is_enabled() can turn false mid-frame (the
			// overlay's routing toggle); without this the draw would run with
			// nulled shaders and the engine WORLD matrix still bound.
			fnv_disengage(dev);
			remix_protocol::none(dev);
			hr = draw();
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (game::is_2d())
		{
			PROFILE_ZONE_N("route_PASS_2D");
			route_tag(diag, "PASS_2D");
			fnv_disengage(dev);
			// Hidden, not plain: the runtime would otherwise trace this
			// orthographic quad at the last FFP projection's near plane
			// ([FFP] HideScreenSpaceDraws, game/screen_space.hpp).
			game::screen_space::tag_draw(dev, "PASS_2D", NumVertices, primCount);
			// The Pip-Boy's cursor is drawn where the game's pick lands (game/pipboy_cursor.hpp).
			const bool cursor = game::pipboy_cursor::before_2d_draw(dev);
			hr = draw();
			if (cursor) game::pipboy_cursor::after_2d_draw(dev);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (ffp.cur_decl_is_skinned())
		{
			PROFILE_ZONE_N("route_SKINNED");
			// Gamebryo lights skinned meshes in several passes: a base pass
			// with BaseMap (SKIN2000-2003, SLS base) and additive per-light /
			// environment / rim passes with no diffuse sampler at all
			// (SKIN2008/2009, SLS2045-2059, SLS2089, PAR2021-2028, ...), whose
			// s0 is the NormalMap. They carry nothing but raster lighting,
			// which the path tracer replaces, so they are dropped.
			const bool lighting_only = (psmap || game::engine_class::current().known()) &&
				!game::engine_class::carries_base(psmap);
			log_skinned_ps_once(dev, psmap, lighting_only);

			using game::api_submit::SkinnedPass;
			const char* why = nullptr;
			const SkinnedPass api = lighting_only ? SkinnedPass::Skip : game::api_submit::try_skinned(dev, psmap, desc, why);
			if (lighting_only) route_tag(diag, "SKINNED_SKIP_LIGHT");
			else if (api == SkinnedPass::Taken) route_tag(diag, "API_SKINNED");
			else if (api == SkinnedPass::Skip) route_tag(diag, "API_SKINNED_SKIP_EXTRA");
			else
			{
				// Another pass of the object may still be its API instance.
				game::traced::note_dropped("SKINNED", why ? why : "refused");
				route_tag(diag, "DROP_SKINNED");
			}
			// No D3D9 draw: the bone state as after a drawn pass (the next
			// upload starts a fresh palette).
			game::skinned_pass_done();
			im->m_stats._drawcall_indexed_prim.track_single();
		}
		else if (ffp.cur_decl_has_pos_t())
		{
			PROFILE_ZONE_N("route_PASS_POSTT");
			route_tag(diag, "PASS_POSTT");
			fnv_disengage(dev);
			remix_protocol::none(dev);
			hr = draw();
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (game::is_sky())
		{
			// NUMOS draws the whole sky: atmosphere, clouds, sun, moon and stars.
			// Every draw with the game's sky shader is dropped, whatever mesh a
			// mod puts under the sky, so none of it becomes a dome in the scene.
			route_tag(diag, "SKIP_GAME_SKY");
		}
		else if (const auto water = game::engine_class::water(psmap);
			water != game::engine_class::Water::None && game::api_submit::try_water(dev, psmap, water, desc))
		{
			// API water: the surface as a cached translucent mesh (or a dropped
			// point-light specular pass, or a water draw not drawn this frame).
			// Ahead of the no-diffuse gate below: water declares no Diffuse
			// sampler, and WATER034 (distant water) no sampler at all.
			PROFILE_ZONE_N("route_API_WATER");
			route_tag(diag, "API_WATER");
			im->m_stats._drawcall_indexed_prim.track_single();
		}
		else if (const auto* expand = game::vs_expand::candidate(psmap))
		{
			// Vertex-expander families: particles (NOLIGHT017/019), the NOLIGHT
			// TexCoordTranform FX, instanced grass / distant objects / distant
			// trees, SpeedTree leaves. Ahead of the no-normal / no-diffuse gates
			// because most of them carry no NORMAL.
			PROFILE_ZONE_N("route_EXPAND");
			route_tag(diag, draw_expanded(dev, *expand, psmap, BaseVertexIndex, MinVertexIndex, NumVertices,
				[&](bool rebased) {
					return dev->DrawIndexedPrimitive(PrimitiveType, rebased ? 0 : BaseVertexIndex,
						MinVertexIndex, NumVertices, startIndex, primCount);
				},
				hr, desc, false));
			im->m_stats._drawcall_indexed_prim.track_single();
		}
		else if (!game::engine_class::carries_base(psmap))
		{
			// FX / normal-only / environment-cubemap / postprocess shaders
			// that don't expose an albedo sampler. Passed through so the
			// rasterised output is correct, and tagged InstanceCategories::Ignore
			// via RS 42 so Remix does not trace the draw.
			PROFILE_ZONE_N("route_PASS_NO_DIFFUSE_ROLE");
			route_tag(diag, "PASS_NO_DIFFUSE_ROLE");
			fnv_disengage(dev);
			remix_protocol::ignore(dev);
			hr = draw();
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (ffp.cur_decl_has_normal() && game::engine_class::land_lod(psmap, true))
		{
			// The with-normal near land LOD (e.g. 0x6626FACE): its vertex shader
			// computes a distance alpha fade (oT4.x) that reaches 0 within ~6964
			// units of the blend centre, so near the player it adds nothing but
			// a z-fight with the path-traced terrain, so it is dropped. Its
			// declaration has only POSITION and TEXCOORD0; carrying the fade to
			// the runtime would need an expanded VB and a cloned declaration.
			PROFILE_ZONE_N("route_PASS_LOD_SHADER");
			route_tag(diag, "PASS_LOD_SHADER");
			game::lod_debug::count_near();
		}
		else
		{
			PROFILE_ZONE_N("route_WORLD");
			const bool is_terrain_shape = (ffp.cur_decl_has_color() && ffp.cur_decl_n_texcoords() >= 2);
			const bool is_bi_shape = ffp.cur_decl_has_blendindices();
			// No NORMAL: the with-normal LOD is above.
			const bool is_far_lod = !ffp.cur_decl_has_normal() && game::engine_class::land_lod(psmap, false);
			if (is_terrain_shape) game::lod_debug::note_land();
			const char* route = is_far_lod ? "FFP_FAR_LOD" : is_terrain_shape ? "FFP_TERRAIN"
				: is_bi_shape ? "FFP_BI" : ffp.cur_decl_has_normal() ? "FFP_WORLD" : "PASS_NO_NORMAL";

			// A far-LOD block drawn by a lit pass (the blocks around the
			// loaded cells) goes through the far-LOD path too.
			if (!is_far_lod && !is_terrain_shape && !is_bi_shape && game::api_submit::is_lod_block_draw())
			{
				game::api_submit::try_lit_lod_block(dev, psmap, desc);
				route_tag(diag, "API_LIT_LOD");
				im->m_stats._drawcall_indexed_prim.track_single();
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			if (is_far_lod)
			{
				game::lod_debug::count_far();
				// API far LOD: a cached mesh with the engine's per-vertex sink
				// baked in (lod_sink), or not drawn this frame.
				if (!game::api_submit::try_far_lod(dev, psmap, desc)) game::traced::note_dropped(route, "far LOD refused");
				route_tag(diag, "API_FAR_LOD");
				im->m_stats._drawcall_indexed_prim.track_single();
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			// [RemixApi] SceneGraphStatics: a candidate drawn in a shape the
			// statics path never takes is refused (and counted); a land-shaped
			// draw of a placed object (not land) is recorded like a plain
			// static (SceneLandShapeStatics).
			const bool scene_static_shape = (is_terrain_shape || is_bi_shape)
				? game::scene_statics::on_shaped_draw(is_terrain_shape ? game::scene_statics::ShapedDraw::Terrain
					: game::scene_statics::ShapedDraw::BlendIndices, psmap)
				: true;

			// API land: up to 7 BaseMap layers blended by per-vertex weights
			// (TerrainLayersEXT), or, for a land draw over the build budget, not
			// drawn this frame. A land-shaped draw whose PS is not a land
			// shader (a placed object) is not land: it continues below.
			if (is_terrain_shape && game::api_submit::try_terrain(dev, psmap, desc))
			{
				route_tag(diag, "API_TERRAIN");
				im->m_stats._drawcall_indexed_prim.track_single();
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			// [RemixApi] SceneGraphStatics records a candidate's base pass here
			// (material, layout, buffers, window).
			if (scene_static_shape && game::scene_statics::on_static_draw(dev, psmap, desc))
			{
				// A claimed geometry's pass that went on to its record: its
				// instance draws it.
				route_tag(diag, "SCENE_RECORDED");
				im->m_stats._drawcall_indexed_prim.track_single();
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			// A D3D9-only albedo (rtx.lightConverter: bulbs, neon; ...): the
			// D3D9 route, every pass (the scene graph refused it above:
			// never claimed).
			if (const char* d3d9_list = game::api::rtxconf::d3d9_only(protocol_albedo(ffp, psmap)))
			{
				route_tag(diag, d3d9_world_draw(dev, psmap, route, d3d9_list, NumVertices, primCount, draw, hr));
				im->m_stats._drawcall_indexed_prim.track_single();
				ctx.restore_all(dev);
				ctx.reset_context();
				return hr;
			}

			// A pass of a provisional scene claim that the statics shape didn't
			// record: its instance draws it.
			if (game::scene_statics::instance_draws_now())
			{
				route_tag(diag, "SCENE_RECORDED");
				im->m_stats._drawcall_indexed_prim.track_single();
				ctx.restore_all(dev);
				ctx.reset_context();
				return S_OK;
			}

			// Draw capture (api_statics.cpp): every other world draw, the
			// first-person weapon included, one DrawInstance per frame.
			const char* why = nullptr;
			const auto cap = game::api_submit::try_static(dev, psmap, desc, why);
			if (cap != game::api_submit::Capture::Refused)
				route_tag(diag, cap == game::api_submit::Capture::Taken ? "API_STATIC" : "API_STATIC_SKIP_PASS");
			else
				route_tag(diag, refused_world_draw(dev, psmap, route, why, NumVertices, primCount, draw, hr));
			im->m_stats._drawcall_indexed_prim.track_single();
		}

		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}

	renderer::renderer()
	{
		p_this = this;

		shared::common::ffp_state::get().init(shared::globals::d3d_device);

		m_initialized = true;
		shared::common::log("Renderer", "FNV renderer module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	renderer::~renderer()
	{
		tex_addons::init_texture_addons(true);
	}
}
