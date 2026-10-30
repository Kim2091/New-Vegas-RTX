#include "common.hpp"
#include "fog.hpp"
#include "path_rules.hpp"
#include "rtti.hpp"

namespace fnvr::fog
{
	namespace
	{
		constexpr uint32_t kAvFlag_AppCulled = 1u;
		constexpr uint64_t kStatusMs = 60000;
		constexpr int kMaxControllers = 16;    // controllers read per object

		struct Rules
		{
			bool enabled = true;
			uint32_t removed_logs = 128, candidate_logs = 0;
		};
		Rules g_rules;

		// Since start.
		uint64_t g_geos_total = 0, g_refs_total = 0;
		// Since the last status line.
		uint32_t g_geos_now = 0, g_refs_now = 0;
		uint64_t g_last_status = 0;
		uint32_t g_removed_logged = 0, g_candidates_logged = 0;
		std::unordered_set<std::string> g_removed_keys, g_candidate_keys;
		bool g_layout_logged = false;

		// What a geometry's own render data says.
		struct Signals
		{
			bool unlit = false;          // BSShaderNoLightingProperty
			bool blended = false;        // NiAlphaProperty blend on
			bool over = false;           // SrcAlpha / InvSrcAlpha (or the inverted pair)
			bool additive = false;       // destination One: glow
			bool particles = false;      // NiParticles
			bool shade_read = false, zwrite = true;
			uint16_t af = 0;
			uintptr_t shade = 0;
			const char* uv = nullptr;    // the texture transform controller's class, null: none
		};

		// A texture transform controller among the object's controllers: its
		// class name, or null. Each controller's target must be the object (the
		// controller layout's check); a mismatch is logged once and ends the read.
		const char* uv_controller(uintptr_t obj)
		{
			if (!(rtti::of(obj) & rtti::kObjectNET)) return nullptr;
			uint32_t c = 0;
			if (!guard::get(obj + engine::NET_controller, c)) return nullptr;
			for (int hop = 0; hop < kMaxControllers && c; hop++)
			{
				uint32_t target = 0;
				if (!(rtti::of(c) & rtti::kController) || !guard::get(c + engine::CTRL_target, target)) return nullptr;
				if (target && target != obj)
				{
					if (!g_layout_logged)
					{
						g_layout_logged = true;
						log::e("GameFog", "controller {} at 0x{:X} of {} 0x{:X} names target 0x{:X}: the controller layout "
							"does not read, so no geometry counts as UV-animated and nothing is hidden", rtti::name(c), c,
							rtti::name(obj), obj, target);
					}
					return nullptr;
				}
				const char* n = rtti::name(c);
				if (std::strstr(n, "TextureTransform") || std::strstr(n, "UVController")) return n;
				if (!guard::get(c + engine::CTRL_next, c)) return nullptr;
			}
			return nullptr;
		}

		Signals read(uintptr_t geo)
		{
			Signals s;
			s.particles = (rtti::of(geo) & rtti::kParticles) != 0;
			uint32_t alpha = 0;
			if (guard::get(geo + engine::GEO_alphaProp, alpha) && guard::plausible(alpha) &&
				guard::get(alpha + engine::ALPHA_flags, s.af))
			{
				const uint32_t src = (s.af >> 1) & 0xFu, dst = (s.af >> 5) & 0xFu;
				s.blended = (s.af & 1u) != 0;
				s.over = s.blended && ((src == engine::kAlphaFunc_SrcAlpha && dst == engine::kAlphaFunc_InvSrcAlpha) ||
					(src == engine::kAlphaFunc_InvSrcAlpha && dst == engine::kAlphaFunc_SrcAlpha));
				s.additive = s.blended && dst == engine::kAlphaFunc_One;
			}
			uint32_t shade = 0, vtbl = 0, f2 = 0;
			if (guard::get(geo + engine::GEO_shadeProp, shade) && guard::plausible(shade) && guard::get(shade, vtbl))
			{
				s.shade = shade;
				s.unlit = vtbl == fnv_exe::addr().vtbl_no_lighting_shade;
				if (guard::get(shade + engine::SHADE_flags2, f2))
				{
					s.shade_read = true;
					s.zwrite = (f2 & engine::SF2_zBufferWrite) != 0;
				}
			}
			s.uv = uv_controller(geo);
			for (uint32_t at = engine::GEO_propFirst; !s.uv && at <= engine::GEO_propLast; at += 4)
			{
				uint32_t prop = 0;
				if (guard::get(geo + at, prop) && guard::plausible(prop)) s.uv = uv_controller(prop);
			}
			return s;
		}

		std::string describe(uintptr_t geo, const Signals& s)
		{
			const uint32_t src = (s.af >> 1) & 0xFu, dst = (s.af >> 5) & 0xFu;
			return std::format("{} geo 0x{:X}, shader {}, alpha 0x{:04X} (blend {} src {} dst {}), UV {}, z-write {}",
				rtti::name(geo), geo, s.shade ? rtti::name(s.shade) : "none", s.af, s.blended ? 1 : 0, src, dst,
				s.uv ? s.uv : "static", !s.shade_read ? "?" : s.zwrite ? "on" : "off");
		}

		// The one signal a near miss lacks, or null when it lacks none or more
		// than one (or is additive glow, which stays by design).
		const char* near_miss(const Signals& s)
		{
			if (!s.blended || s.additive) return nullptr;
			const int missing = !s.unlit + !s.over + !s.uv;
			if (missing == 0) return s.particles ? "a particle system" : nullptr;
			if (missing != 1 || s.particles) return nullptr;
			if (!s.unlit) return "unlit (lit shader)";
			if (!s.over) return "alpha-over (another blend mode)";
			return "UV-animated (static UVs)";
		}
	}

	void load_config()
	{
		const std::wstring ini = log::dir() + L"fnv_remix_nvse.ini";
		g_rules.enabled = GetPrivateProfileIntW(L"GameFog", L"Enabled", 1, ini.c_str()) != 0;
		g_rules.removed_logs = GetPrivateProfileIntW(L"GameFog", L"RemovedLogs", 128, ini.c_str());
		g_rules.candidate_logs = GetPrivateProfileIntW(L"GameFog", L"CandidateLogs", 0, ini.c_str());
		log::e("GameFog", "Enabled={} (unlit, alpha-over, UV-animated, not particles; placed references) removed logs {} "
			"candidate logs {}", g_rules.enabled ? 1 : 0, g_rules.removed_logs, g_rules.candidate_logs);
	}

	void on_walk(uintptr_t ref, uint8_t ref_type, uint8_t base_type, uintptr_t base_form,
		const std::vector<uintptr_t>& geos)
	{
		if (!g_rules.enabled || geos.empty() || ref_type != engine::kFormType_REFR) return;

		uint32_t hidden = 0;
		uintptr_t first_geo = 0;
		Signals first;
		// The reference's model (the log key: once per model) and its line
		// text, read when a line needs them.
		std::string model, who;
		auto identify = [&]() {
			if (!who.empty()) return;
			uint32_t ref_id = 0, base_id = 0;
			guard::get(ref + engine::FORM_refID, ref_id);
			if (base_form) guard::get(base_form + engine::FORM_refID, base_id);
			if (!base_form || !path_rules::form_model_path(base_form, model)) model = std::format("base {:08X}", base_id);
			who = std::format("{} (base {:08X} type 0x{:02X}, ref {:08X})", model, base_id, base_type, ref_id);
		};

		for (const uintptr_t geo : geos)
		{
			uint32_t flags = 0;
			if (!(rtti::of(geo) & rtti::kGeometry) || !guard::get(geo + engine::AV_flags, flags) ||
				(flags & kAvFlag_AppCulled))
				continue;
			const Signals s = read(geo);
			if (s.unlit && s.over && s.uv && !s.particles)
			{
				if (!guard::mapped(geo + engine::AV_flags, 4)) continue;
				*reinterpret_cast<volatile uint32_t*>(geo + engine::AV_flags) = flags | kAvFlag_AppCulled;
				if (!hidden++)
				{
					first_geo = geo;
					first = s;
				}
				continue;
			}
			const char* miss = near_miss(s);
			if (!miss || g_candidates_logged >= g_rules.candidate_logs) continue;
			// Once per model and missing signal: drawn.
			identify();
			if (!g_candidate_keys.insert(model + "|" + miss).second) continue;
			g_candidates_logged++;
			log::e("GameFog", "candidate {}: {} -- not {}: drawn{}", who, describe(geo, s), miss,
				g_candidates_logged == g_rules.candidate_logs ? " (last candidate line)" : "");
		}
		if (!hidden) return;
		g_geos_total += hidden;
		g_geos_now += hidden;
		g_refs_total++;
		g_refs_now++;
		identify();
		if (!g_removed_keys.insert(model).second || g_removed_logged >= g_rules.removed_logs) return;
		g_removed_logged++;
		log::e("GameFog", "hid {} of {} geometries of {}: first {}{}", hidden, geos.size(), who,
			describe(first_geo, first), g_removed_logged == g_rules.removed_logs ? " (last model line)" : "");
	}

	void tick()
	{
		const uint64_t now = GetTickCount64();
		if (!g_last_status) g_last_status = now;
		if (now - g_last_status < kStatusMs) return;
		g_last_status = now;
		if (g_geos_now)
			log::e("GameFog", "hid +{} geometries of +{} references in the last minute (session {} geometries, {} references, "
				"{} models)", g_geos_now, g_refs_now, g_geos_total, g_refs_total, g_removed_keys.size());
		g_geos_now = g_refs_now = 0;
	}
}
