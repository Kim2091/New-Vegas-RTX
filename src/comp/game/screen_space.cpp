#include "std_include.hpp"
#include "screen_space.hpp"
#include "game.hpp"
#include "menu_bg.hpp"
#include "stable_origin.hpp"
#include "api_rtxconf.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"
#include "shared/common/remix_protocol.hpp"

/*
 * See screen_space.hpp. Everything here is keyed on pointers the game keeps
 * alive while it draws with them (shaders, textures); they are compared by
 * address only and never dereferenced after the call that saw them, except
 * the texture hash, which is read in the same call.
 */

namespace comp::game::screen_space
{
	namespace
	{
		// FFP draws: "near the camera" is a camera-relative WORLD translation
		// shorter than this (FNV units; the player's eye is ~120 above the
		// feet), and "small" is at most this many vertices (a quad, two).
		constexpr float kNearUnits = 160.0f;
		constexpr UINT  kSmallVerts = 8;
		constexpr UINT  kSummaryFrames = 600;
		constexpr size_t kMaxSeen = 4096;   // identities remembered (bounded)

		// The projection the device holds for the next shader draw, as
		// game::apply_transforms last set it.
		float g_proj00 = 0.0f;
		float g_near = 0.0f;
		bool  g_proj_viewmodel = false;
		bool  g_have_proj = false;

		std::unordered_set<uint64_t> g_seen;
		std::unordered_map<const void*, std::string> g_tex_desc;   // texture -> "hash / list / desc"
		int g_logged = 0;

		// Per-interval counts for the summary line.
		uint32_t g_hidden = 0, g_untextured = 0, g_plain = 0, g_near_ffp = 0;
		UINT g_frames = 0;

		const auto& cfg() { return shared::common::config::get(); }

		int log_cap() { return cfg().diagnostics.screen_space_log; }

		uint64_t mix(uint64_t h, uint64_t v)
		{
			h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
			return h;
		}

		uint32_t ps_hash()
		{
			const auto* m = shared::common::g_ps_classifier.classify(shared::common::ffp_state::get().last_ps());
			return m ? m->ps_hash : 0u;
		}

		uint32_t vs_hash()
		{
			const auto* v = shared::common::g_vs_classifier.classify(shared::common::ffp_state::get().last_vs());
			return v ? v->vs_hash : 0u;
		}

		// "0x<remix hash> [list] fmt WxH pool usage" for the stage-0 texture,
		// computed once per texture pointer (the hash locks mip 0).
		const std::string& describe_tex0(IDirect3DBaseTexture9* tex)
		{
			static const std::string kNone = "none";
			if (!tex) return kNone;
			if (const auto it = g_tex_desc.find(tex); it != g_tex_desc.end()) return it->second;
			if (g_tex_desc.size() >= kMaxSeen) g_tex_desc.clear();

			std::string s;
			D3DSURFACE_DESC sd = {};
			bool is_rt = false;
			if (tex->GetType() == D3DRTYPE_TEXTURE &&
				SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &sd)))
			{
				is_rt = (sd.Usage & D3DUSAGE_RENDERTARGET) != 0;
				s = std::format("fmt={} {}x{} pool={}{}", static_cast<int>(sd.Format), sd.Width, sd.Height,
					static_cast<int>(sd.Pool), is_rt ? " RT" : "");
			}
			else
				s = std::format("type={}", static_cast<int>(tex->GetType()));

			uint64_t h = 0;
			if (!is_rt && api::rtxconf::d3d9_image_hash(tex, h))
			{
				const char* list = api::rtxconf::listed(h);
				s = std::format("remixHash=0x{:016X} [{}] {}", h, list ? list : "no rtx.conf list", s);
			}
			else
				s = std::format("remixHash=? ({}) {}", is_rt ? "render target: the runtime skips such draws" :
					"format / pool not hashable here", s);
			return g_tex_desc.emplace(tex, std::move(s)).first->second;
		}

		std::string state_summary(IDirect3DDevice9* dev)
		{
			DWORD zen = 0, zw = 0, ab = 0, sb = 0, db = 0, at = 0;
			dev->GetRenderState(D3DRS_ZENABLE, &zen);
			dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
			dev->GetRenderState(D3DRS_SRCBLEND, &sb);
			dev->GetRenderState(D3DRS_DESTBLEND, &db);
			dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
			return std::format("z={} zwrite={} blend={} ({}/{}) atest={}", zen, zw, ab, sb, db, at);
		}

		// First sighting of an identity, while the log budget lasts.
		bool first_sighting(uint64_t key)
		{
			const int cap = log_cap();
			if (cap <= 0 || g_logged >= cap) return false;
			if (g_seen.size() >= kMaxSeen) return false;
			if (!g_seen.insert(key).second) return false;
			++g_logged;
			return true;
		}
	}

	void note_applied_projection(const float* proj)
	{
		if (!proj) return;
		g_proj00 = proj[0];
		// D3D perspective: _33 = zf/(zf-zn), _43 = -zn*zf/(zf-zn) -> zn = -_43/_33.
		g_near = (proj[10] != 0.0f) ? -proj[14] / proj[10] : 0.0f;
		g_proj_viewmodel = uses_viewmodel_projection();
		g_have_proj = true;
	}

	void tag_draw(IDirect3DDevice9* dev, const char* route, UINT verts, UINT prims)
	{
		auto& ffp = shared::common::ffp_state::get();
		auto* tex0 = ffp.cur_texture(0);
		const bool on = cfg().ffp.hide_screen_space_draws;

		const char* what;
		if (on && tex0)
		{
			remix_protocol::categories_only(dev,
				remix_protocol::category_mask(remix_protocol::CategoryBit::Hidden));
			++g_hidden;
			what = "tagged Hidden (rasterised only, not traced)";
		}
		else
		{
			remix_protocol::none(dev);
			if (on) { ++g_untextured; what = "NOT hidden: no stage-0 texture, the runtime ignores RS 42 -- it may still be traced"; }
			else    { ++g_plain; what = "plain (HideScreenSpaceDraws=0): traced if drawn before RTX injection"; }
		}

		if (log_cap() <= 0) return;
		const uint32_t ps = ps_hash(), vs = vs_hash();
		uint64_t key = mix(0x5353ull, reinterpret_cast<uintptr_t>(route));
		key = mix(key, ps); key = mix(key, vs);
		key = mix(key, reinterpret_cast<uintptr_t>(tex0));
		key = mix(key, verts <= kSmallVerts ? verts : 0xFFFFu);
		if (!first_sighting(key)) return;

		shared::common::log("ScreenSpace", std::format(
			"{} #{}: PS 0x{:08X} VS 0x{:08X} verts={} prims={} | s0 {} | {} | pipBoyMode={} | runtime un-projects it with "
			"the last FFP projection{} -> {}",
			route, g_logged, ps, vs, verts, prims, describe_tex0(tex0), state_summary(dev),
			static_cast<int>(menu_bg::last_pipboy_mode()),
			g_have_proj ? std::format(" (_11={:.3f}, near={:.2f}, {})", g_proj00, g_near,
				g_proj_viewmodel ? "viewmodel / Pip-Boy FOV" : "world FOV") : std::string(" (none yet)"),
			what), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
	}

	void note_ffp_draw(IDirect3DDevice9* dev, const char* route, UINT verts, UINT prims)
	{
		if (verts == 0 || verts > kSmallVerts) return;
		const float* w = get_renderer_matrix(RENDERER_WORLD_OFF);
		if (!w) return;
		// FNV's WORLD translation is object - camera.
		const float tx = w[12], ty = w[13], tz = w[14];
		const float dist = std::sqrt(tx * tx + ty * ty + tz * tz);
		const bool viewmodel = uses_viewmodel_projection();
		if (!(dist < kNearUnits) && !viewmodel) return;
		++g_near_ffp;

		if (log_cap() <= 0) return;
		auto& ffp = shared::common::ffp_state::get();
		auto* tex0 = ffp.cur_texture(0);
		const uint32_t ps = ps_hash(), vs = vs_hash();
		uint64_t key = mix(0x4646ull, reinterpret_cast<uintptr_t>(route));
		key = mix(key, ps); key = mix(key, vs);
		key = mix(key, reinterpret_cast<uintptr_t>(tex0));
		key = mix(key, verts);
		if (!first_sighting(key)) return;

		const float sx = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
		const double* d = stable_origin::shift();
		shared::common::log("ScreenSpace", std::format(
			"near-camera FFP draw {} #{}: PS 0x{:08X} VS 0x{:08X} verts={} prims={} | s0 {} | {} | WORLD translation "
			"(object - camera) ({:.1f}, {:.1f}, {:.1f}) |{:.1f}| scale {:.3f} | {} projection | stable-origin D "
			"({:.0f}, {:.0f}, {:.0f}) | pipBoyMode={} | ray-traced at camera + that offset",
			route, g_logged, ps, vs, verts, prims, describe_tex0(tex0), state_summary(dev), tx, ty, tz, dist, sx,
			viewmodel ? "viewmodel / Pip-Boy" : "world", d[0], d[1], d[2],
			static_cast<int>(menu_bg::last_pipboy_mode())), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
	}

	void on_present()
	{
		if (++g_frames < kSummaryFrames) return;
		g_frames = 0;
		if (log_cap() > 0 && (g_hidden || g_untextured || g_plain || g_near_ffp))
			shared::common::log("ScreenSpace", std::format(
				"last {} frames: screen-space draws hidden={} untaggable(no s0 texture)={} plain(switch off)={} | "
				"small FFP draws near the camera={} | identities logged {}/{}",
				kSummaryFrames, g_hidden, g_untextured, g_plain, g_near_ffp, g_logged, log_cap()),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
		g_hidden = g_untextured = g_plain = g_near_ffp = 0;
	}
}
