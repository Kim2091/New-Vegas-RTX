#include "common.hpp"
#include "decals.hpp"
#include "integrity.hpp"
#include "path_rules.hpp"
#include "rtti.hpp"

namespace fnvr::decals
{
	using path_rules::match;
	using path_rules::model_path;
	using path_rules::normalise;
	using path_rules::read_cstr;

	namespace
	{
		struct Rules
		{
			bool enabled = true;
			std::vector<std::string> tex, model;
			uint32_t candidate_logs = 0;
		};
		Rules g_rules;

		// Since start.
		uint32_t g_props_flagged = 0, g_candidates = 0, g_shared_skips = 0;
		// Since the last status line.
		uint32_t g_flagged_now = 0, g_walks = 0;
		uint32_t g_last_status = 0;
		bool g_logged_off = false;
		std::unordered_set<std::string> g_logged_paths;

		const char* class_name(uintptr_t vtbl)
		{
			const auto& addr = fnv_exe::addr();
			if (!vtbl) return nullptr;
			if (vtbl == addr.vtbl_ppl_shade) return "PPLighting";
			if (vtbl == addr.vtbl_lighting30_shade || vtbl == addr.vtbl_lighting30_shade2) return "Lighting30";
			if (vtbl == addr.vtbl_no_lighting_shade) return "NoLighting";
			return nullptr;
		}

		// The property's diffuse texture file (PPL / Lighting30 role 0).
		bool texture_path(uintptr_t prop, uintptr_t vtbl, std::string& out)
		{
			if (vtbl == fnv_exe::addr().vtbl_no_lighting_shade) return false;
			uint16_t layers = 0;
			uint32_t arr = 0, tex = 0, name = 0;
			if (!guard::get(prop + engine::PPL_layerCount, layers) || !layers ||
				!guard::get(prop + engine::PPL_texArrays, arr) || !guard::plausible(arr) || !guard::get(arr, tex) ||
				!(rtti::of(tex) & rtti::kSourceTexture) || !guard::get(tex + engine::SRCTEX_fileName, name) ||
				!read_cstr(name, out))
				return false;
			out = normalise(out);
			return true;
		}

		// Blended SrcAlpha / InvSrcAlpha, no alpha test.
		bool overlay_blend(uintptr_t geo)
		{
			uint32_t alpha = 0;
			uint16_t af = 0;
			if (!guard::get(geo + engine::GEO_alphaProp, alpha) || !guard::plausible(alpha) ||
				!guard::get(alpha + engine::ALPHA_flags, af))
				return false;
			const uint32_t src = (af >> 1) & 0xFu, dst = (af >> 5) & 0xFu;
			return (af & 1u) && src == engine::kAlphaFunc_SrcAlpha && dst == engine::kAlphaFunc_InvSrcAlpha &&
				!((af >> 9) & 1u);
		}

		// The geometry's CPU positions span a flat box (smallest extent under
		// 0.5% of the largest). False when they aren't kept on the CPU.
		bool flat(uintptr_t geo)
		{
			uint32_t data = 0, pos = 0;
			uint16_t n = 0;
			if (!guard::get(geo + engine::GEO_data, data) || !guard::plausible(data) ||
				!guard::get(data + engine::NGD_numVerts, n) || n < 3 || !guard::get(data + engine::NGD_pos, pos) ||
				!guard::plausible(pos))
				return false;
			static std::vector<float> s_p;
			const uint32_t count = (std::min)(static_cast<uint32_t>(n), 4096u);
			s_p.resize(count * 3);
			if (!guard::read(pos, s_p.data(), s_p.size() * sizeof(float))) return false;
			float lo[3] = { s_p[0], s_p[1], s_p[2] }, hi[3] = { s_p[0], s_p[1], s_p[2] };
			for (uint32_t v = 1; v < count; v++)
				for (int k = 0; k < 3; k++)
				{
					lo[k] = (std::min)(lo[k], s_p[v * 3 + k]);
					hi[k] = (std::max)(hi[k], s_p[v * 3 + k]);
				}
			const float e[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
			const float big = (std::max)({ e[0], e[1], e[2] }), small = (std::min)({ e[0], e[1], e[2] });
			return big > 0.0f && small < 0.005f * big;
		}

		struct Prop
		{
			uintptr_t vtbl = 0;
			uint32_t flags1 = 0, flags2 = 0;
			bool all_pass = true, any_pass = false;
			std::string tex, rule;
		};
	}

	void load_config()
	{
		const std::wstring ini = log::dir() + L"fnv_remix_nvse.ini";
		auto read = [](const wchar_t* key, const wchar_t* def) { return path_rules::read_ini(L"Decals", key, def); };
		g_rules.enabled = GetPrivateProfileIntW(L"Decals", L"Enabled", 1, ini.c_str()) != 0;
		g_rules.tex = path_rules::split(read(L"TexturePaths",
			L"textures\\clutter\\nvstain*;textures\\decals\\*;*\\blood*decal*;*\\dirtdecal*;*decal_d.dds"));
		g_rules.model = path_rules::split(read(L"ModelPaths", L"meshes\\clutter\\decals\\*"));
		g_rules.candidate_logs = GetPrivateProfileIntW(L"Decals", L"CandidateLogs", 0, ini.c_str());
		log::e("Decal", "Enabled={} texture rules [{}] model rules [{}] candidate logs {}", g_rules.enabled ? 1 : 0,
			path_rules::join(g_rules.tex), path_rules::join(g_rules.model), g_rules.candidate_logs);
	}

	void on_walk(uintptr_t ref, uint8_t base_type, uintptr_t base_form, const std::vector<uintptr_t>& geos)
	{
		if (!g_rules.enabled || geos.empty()) return;
		if (!(integrity::features_ok() & FNVR_F_DECALS))
		{
			if (!g_logged_off)
			{
				g_logged_off = true;
				log::e("Decal", "the decal path's code is not vanilla ({}): no decal flag is written",
					integrity::status(FNVR_F_DECALS));
			}
			return;
		}
		g_walks++;
		static std::unordered_map<uintptr_t, Prop> s_props;
		s_props.clear();
		std::string model;
		const bool have_model = model_path(base_type, base_form, model);
		const std::string* model_rule = have_model ? match(g_rules.model, model) : nullptr;

		for (const uintptr_t geo : geos)
		{
			uint32_t shade = 0, vtbl = 0;
			if (!guard::get(geo + engine::GEO_shadeProp, shade) || !guard::plausible(shade) || !guard::get(shade, vtbl) ||
				!class_name(vtbl))
				continue;
			auto [it, fresh] = s_props.try_emplace(shade);
			Prop& p = it->second;
			if (fresh)
			{
				p.vtbl = vtbl;
				guard::get(shade + engine::SHADE_flags1, p.flags1);
				guard::get(shade + engine::SHADE_flags2, p.flags2);
				texture_path(shade, vtbl, p.tex);
			}
			const bool blend = overlay_blend(geo);
			const std::string* rule = !p.tex.empty() ? match(g_rules.tex, p.tex) : nullptr;
			if (!rule) rule = model_rule;
			const bool pass = blend && rule;
			if (pass)
			{
				p.any_pass = true;
				p.rule = *rule;
			}
			else p.all_pass = false;

			// A flat blended overlay no rule names: logged as a rule candidate.
			if (blend && !rule && (base_type == engine::kFormType_STAT || base_type == engine::kFormType_MSTT) &&
				g_candidates < g_rules.candidate_logs && !(p.flags1 & (engine::SF1_decal | engine::SF1_dynamicDecal)) &&
				!(p.flags2 & engine::SF2_alphaDecal) && flat(geo))
			{
				g_candidates++;
				log::f("Decal", "candidate {} {} ({}) f1=0x{:08X} f2=0x{:08X} ref 0x{:X} base type 0x{:02X} -- flat and blended, "
					"no rule: not written{}", have_model ? model : std::string("(no STAT model path)"),
					p.tex.empty() ? std::string("(no texture path)") : p.tex, class_name(vtbl), p.flags1, p.flags2, ref, base_type,
					g_candidates == g_rules.candidate_logs ? " (last candidate line)" : "");
			}
		}

		for (const auto& [shade, p] : s_props)
		{
			if (!p.any_pass) continue;
			if ((p.flags1 & (engine::SF1_decal | engine::SF1_dynamicDecal)) || (p.flags2 & engine::SF2_alphaDecal)) continue;
			if (!p.all_pass)
			{
				if (g_shared_skips++ < 32)
					log::f("Decal", "skipped property 0x{:X} ({}) of {}: shared with geometry that fails the rule", shade,
						p.tex.empty() ? std::string("no texture path") : p.tex, have_model ? model : std::string("?"));
				continue;
			}
			// Written only while the property is still what was read.
			uint32_t now = 0, vtbl = 0;
			if (!guard::get(shade, vtbl) || vtbl != p.vtbl || !guard::get(shade + engine::SHADE_flags1, now) ||
				now != p.flags1 || !guard::mapped(shade + engine::SHADE_flags1, 4))
				continue;
			*reinterpret_cast<volatile uint32_t*>(shade + engine::SHADE_flags1) = now | engine::SF1_decal;
			g_props_flagged++;
			g_flagged_now++;
			const std::string key = p.tex + "|" + (have_model ? model : std::string());
			if (g_logged_paths.size() < 512 && g_logged_paths.insert(key).second)
				log::f("Decal", "set flags1 bit 26 on {} ({}, property 0x{:X}) texture {} -- rule {}",
					have_model ? model : std::string("(no STAT model path)"), class_name(p.vtbl), shade,
					p.tex.empty() ? std::string("-") : p.tex, p.rule);
		}
	}

	void tick(uint32_t frame)
	{
		if (frame - g_last_status < 600) return;
		g_last_status = frame;
		// The 600-frame status is a dev diagnostic: [Logging] Verbose only.
		if (g_flagged_now && log::verbose())
			log::f("Decal", "flagged +{} properties in {} walks (total {}, candidates logged {}, shared skips {})",
				g_flagged_now, g_walks, g_props_flagged, g_candidates, g_shared_skips);
		g_flagged_now = g_walks = 0;
	}
}
