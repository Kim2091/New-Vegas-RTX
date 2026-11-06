#include "common.hpp"
#include "lights.hpp"
#include "monitor.hpp"
#include "owners.hpp"
#include "pins.hpp"
#include "rtti.hpp"

namespace fnvr::lights
{
	namespace
	{
		constexpr uint32_t kMaxListEntries = 4096;

		struct Light
		{
			uintptr_t light = 0;
			uintptr_t owner = 0;          // the reference (placed), 0 otherwise
			uint8_t kind = FNVR_LK_EFFECT;
			uint32_t listed_scan = 0;     // the scan that last found it in an engine list
			uint64_t key = 0;             // FnvRemixLight::asset_key
			uintptr_t ligh = 0;           // placed: the reference's base LIGH (its flags say whether it is animated)
			uint8_t anim = 0;             // FnvRemixLightAnim seen so far: sticky, nothing gives the floats back
			uint8_t atten_changes = 0;    // effect lights without a LIGH: changes of the attenuation floats
			bool atten_seen = false;
			float last_atten[3] = {};
		};

		std::unordered_map<uintptr_t, Light> g_lights;                      // NiLight -> entry
		std::unordered_map<uintptr_t, std::vector<uintptr_t>> g_by_ref;    // reference -> its placed lights
		uintptr_t g_player = 0;
		uint32_t g_scan = 0;
		bool g_ssn_ok = false;
		// Since the last status line.
		uint32_t g_effect_added = 0, g_effect_released = 0, g_placed_added = 0, g_player_changes = 0;
		uint32_t g_last_status = 0;
		uint32_t g_fixups = 0, g_fixups_now = 0;
		float g_fixup_max = 0.0f;

		// The light's world position from its parent's world transform and its
		// own local one (Gamebryo: parent.t + parent.s * (parent.R * local.t)).
		// False without a readable parent.
		bool composed_position(uintptr_t light, uint32_t flags, float (&out)[3])
		{
			uint32_t parent = 0;
			if (!guard::get(light + engine::AV_parent, parent) || !guard::plausible(parent)) return false;
			float pr[9], pt[3], ps = 1.0f, lt[3];
			if (!guard::read(parent + engine::AV_worldRot, pr, sizeof(pr)) ||
				!guard::read(parent + engine::AV_worldPos, pt, sizeof(pt)) || !guard::get(parent + engine::AV_worldScale, ps) ||
				!guard::read(light + engine::AV_localPos, lt, sizeof(lt)))
				return false;
			if (flags & engine::kAvFlag_CopyParentWorld)
			{
				std::memcpy(out, pt, sizeof(pt));
				return true;
			}
			for (int r = 0; r < 3; r++)
				out[r] = pt[r] + ps * (pr[r * 3 + 0] * lt[0] + pr[r * 3 + 1] * lt[1] + pr[r * 3 + 2] * lt[2]);
			return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
		}

		// An effect or player light the engine detached from the scene graph
		// (the Pip-Boy light's switch-off, 0x80ED60) while a list still holds it.
		bool detached(uintptr_t light)
		{
			uint32_t parent = 0;
			return guard::get(light + engine::AV_parent, parent) && !parent;
		}

		uint64_t mix_key(uint64_t h, uint64_t v)
		{
			h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
			h *= 0xff51afd7ed558ccdull;
			return h ^ (h >> 29);
		}

		constexpr uint64_t kKeySeed = 0x46'4E'56'4C'49'47'48'54ull;   // "FNVLIGHT"

		// A placed light's asset key: the local form IDs (without the load
		// order index) of its reference and base form, the reference's
		// position to the unit, and the light's place among the lights of the
		// reference's 3D. 0 when the reference can't be read.
		uint64_t placed_key(uintptr_t ref, size_t ordinal)
		{
			uint32_t ref_id = 0, base = 0, base_id = 0;
			float pos[3] = {};
			if (!guard::get(ref + engine::FORM_refID, ref_id) || !guard::get(ref + engine::REFR_baseForm, base) ||
				!guard::plausible(base) || !guard::get(base + engine::FORM_refID, base_id) ||
				!guard::read(ref + engine::REFR_pos, pos, sizeof(pos)))
				return 0;
			uint64_t h = mix_key(kKeySeed, FNVR_LK_PLACED);
			h = mix_key(h, ref_id & 0x00FFFFFFu);
			h = mix_key(h, base_id & 0x00FFFFFFu);
			for (const float p : pos)
			{
				if (!std::isfinite(p)) return 0;
				h = mix_key(h, static_cast<uint64_t>(static_cast<int64_t>(std::floor(p))));
			}
			h = mix_key(h, ordinal);
			return h ? h : 1;
		}

		uintptr_t as_ligh(uintptr_t form)   // a live LIGH, else 0
		{
			uint32_t vtbl = 0; uint8_t type = 0;
			return guard::plausible(form) && guard::get(form, vtbl) && guard::in_image(vtbl) &&
				guard::get(form + engine::FORM_type, type) && type == engine::kFormType_LIGH ? form : 0;
		}

		uintptr_t base_ligh(uintptr_t ref)
		{
			uint32_t base = 0;
			return guard::get(ref + engine::REFR_baseForm, base) ? as_ligh(base) : 0;
		}

		uint8_t animation(Light& l, uint32_t av_flags, uint32_t jip_form, const float (&atten)[3], float radius)
		{
			uint8_t now = 0;
			// A JIP light: one of JIP's own, or a placed light JIP's AttachLight took over (CreatePointLight reuses
			// a point light that is a child of the node). JIP animates it by that LIGH's flags, and the engine
			// animates a placed light by its base LIGH's flags as well.
			const uintptr_t jip = (av_flags & engine::kAvFlag_JipLight) ? as_ligh(jip_form) : 0;
			if (jip) now |= FNVR_LA_JIP;
			uint32_t lf = 0, jf = 0;   // read each frame: JIP SetLightFlag can change them
			const bool have_lf = l.ligh && guard::get(l.ligh + engine::LIGH_flags, lf);
			const bool have_jf = jip && guard::get(jip + engine::LIGH_flags, jf);
			if (have_lf || have_jf)
			{
				if ((lf | jf) & engine::kLighFlags_AttenState) now |= FNVR_LA_ATTEN;
				if ((lf | jf) & engine::kLighFlags_JipAnimated) now |= FNVR_LA_AMBIENT;
			}
			else if (l.kind == FNVR_LK_EFFECT)
			{
				// No LIGH to ask. After the engine makes a light only an animation writes these floats again: every
				// frame (the engine's) or every cycle (JIP's). A light made again for another use changes them once.
				if (l.atten_seen && std::memcmp(l.last_atten, atten, sizeof(atten)) != 0 && l.atten_changes < 255)
					l.atten_changes++;
				l.atten_seen = true;
				std::memcpy(l.last_atten, atten, sizeof(atten));
				if (l.atten_changes >= 2) now |= FNVR_LA_ATTEN | FNVR_LA_AMBIENT | FNVR_LA_GUESS;
			}
			if ((now & FNVR_LA_ATTEN) && !(l.anim & FNVR_LA_ATTEN))
			{
				// The base LIGH, else JIP's; a placed light JIP took over names both.
				uint32_t id = 0, jip_id = 0;
				if (l.ligh) guard::get(l.ligh + engine::FORM_refID, id);
				if (jip) guard::get(jip + engine::FORM_refID, jip_id);
				const std::string also = l.ligh && jip && jip != l.ligh
					? std::format(", JIP's LIGH {:08X} flags 0x{:X}", jip_id, jf) : std::string();
				log::f("LightReg", "animated light 0x{:X} ({}, reference 0x{:X}, LIGH {:08X} flags 0x{:X}{}) radius {:.0f}: "
					"its attenuation floats ({:g}, {:g}, {:g}) hold the animation's state, not its attenuation", l.light,
					l.kind == FNVR_LK_PLACED ? (jip ? "placed, JIP light" : "placed")
						: jip ? "JIP light" : "effect light, the floats changed",
					l.owner, l.ligh ? id : jip_id, l.ligh ? lf : jf, also, radius, atten[0], atten[1], atten[2]);
			}
			l.anim |= now;
			return l.anim;
		}

		uintptr_t tag_of(const Light& l)
		{
			return l.kind == FNVR_LK_PLACED ? l.owner : l.kind == FNVR_LK_PLAYER ? pins::kTagPlayer : pins::kTagEffect;
		}

		void forget(uintptr_t light)
		{
			const auto it = g_lights.find(light);
			if (it == g_lights.end()) return;
			pins::release(light, tag_of(it->second));
			g_lights.erase(it);
		}

		// The engine's active lists: unknown lights become effect lights.
		void scan()
		{
			g_scan++;
			uint32_t ssn = 0;
			g_ssn_ok = guard::get(fnv_exe::addr().shadow_scene_node, ssn) && guard::plausible(ssn);
			if (!g_ssn_ok) return;
			for (const uint32_t list : { engine::SSN_pointLights, engine::SSN_shadowLights })
			{
				uint32_t entry = 0;
				if (!guard::get(ssn + list, entry)) continue;
				for (uint32_t n = 0; entry && n < kMaxListEntries; n++)
				{
					uint32_t next = 0, ssl = 0, light = 0;
					if (!guard::get(entry + engine::LIST_next, next) || !guard::get(entry + engine::LIST_data, ssl)) break;
					entry = next;
					if (!guard::plausible(ssl) || !guard::get(ssl + engine::SSL_light, light) || !guard::plausible(light))
						continue;
					if (const auto it = g_lights.find(light); it != g_lights.end())
					{
						it->second.listed_scan = g_scan;
						continue;
					}
					// The list can hold an NiDirectionalLight (0xB5C978); only point
					// lights are sent.
					uint32_t vtbl = 0;
					if (!guard::get(light, vtbl) || vtbl != fnv_exe::addr().vtbl_ni_point_light ||
						!pins::pin(light, pins::kTagEffect))
						continue;
					Light l;
					l.light = light;
					l.kind = FNVR_LK_EFFECT;
					l.listed_scan = g_scan;
					g_lights.emplace(light, l);
					g_effect_added++;
				}
			}
		}

		// The player's light: the NiPointLight of the first LightEffect on the
		// player's active effect list (built at 0x80E970, cleared by the dispel
		// 0x80ED60). 0 = the light is off.
		uintptr_t find_player_light()
		{
			uint32_t pc = 0, vtbl = 0, node = 0;
			if (!guard::get(fnv_exe::addr().player, pc) || !guard::plausible(pc) || !guard::get(pc, vtbl) ||
				vtbl != fnv_exe::addr().vtbl_player_character || !guard::get(pc + engine::PC_effectList, node))
				return 0;
			for (int n = 0; node && n < 256; n++)
			{
				uint32_t pair[2] = {};
				if (!guard::read(node, pair, sizeof(pair))) return 0;
				uint32_t evt = 0, light = 0, lvt = 0;
				if (guard::plausible(pair[0]) && guard::get(pair[0], evt) && evt == fnv_exe::addr().vtbl_light_effect &&
					guard::get(pair[0] + engine::LE_light, light) && guard::plausible(light) && guard::get(light, lvt) &&
					lvt == fnv_exe::addr().vtbl_ni_point_light)
					return light;
				node = pair[1];
			}
			return 0;
		}

		void update_player()
		{
			const uintptr_t light = find_player_light();
			if (light == g_player) return;
			g_player_changes++;
			if (g_player)
			{
				log::f("LightReg", "player light 0x{:X} gone (released at the next main loop)", g_player);
				forget(g_player);
				g_player = 0;
			}
			if (!light) return;
			// Listed already (the engine registers it with AddLight), or found in
			// the player's 3D by the owners walk: it becomes the player's.
			if (const auto it = g_lights.find(light); it != g_lights.end())
			{
				if (it->second.kind == FNVR_LK_PLACED)
					if (const auto r = g_by_ref.find(it->second.owner); r != g_by_ref.end())
						std::erase(r->second, light);
				forget(light);
			}
			if (!pins::pin(light, pins::kTagPlayer)) return;
			Light l;
			l.light = light;
			l.kind = FNVR_LK_PLAYER;
			l.key = mix_key(kKeySeed, FNVR_LK_PLAYER);
			g_lights.emplace(light, l);
			g_player = light;
			log::f("LightReg", "player light 0x{:X} (PlayerCharacter+0x{:X} effect list -> LightEffect+0x{:X}) pinned", light,
				engine::PC_effectList, engine::LE_light);
		}

		bool fill(Light& l, FnvRemixLight& o)
		{
			// NiAVObject world position .. NiLight attenuation, one read.
			float block[(engine::LIGHT_atten + 12 - engine::AV_worldPos) / 4] = {};
			uint32_t flags = 0;
			if (!guard::read(l.light + engine::AV_worldPos, block, sizeof(block)) ||
				!guard::get(l.light + engine::AV_flags, flags))
				return false;
			auto at = [&](uint32_t off) { return block[(off - engine::AV_worldPos) / 4]; };
			o = {};
			o.id = l.light;
			o.owner_ref = l.owner;
			for (int k = 0; k < 3; k++)
			{
				o.pos[k] = at(engine::AV_worldPos + 4 * k);
				o.diffuse[k] = at(engine::LIGHT_diffuse + 4 * k);
				o.ambient[k] = at(engine::LIGHT_ambient + 4 * k);
				o.atten[k] = at(engine::LIGHT_atten + 4 * k);
			}
			o.dimmer = at(engine::LIGHT_dimmer);
			o.radius = at(engine::LIGHT_radius);
			uint32_t jip_form = 0;
			std::memcpy(&jip_form, &block[(engine::LIGHT_jipForm - engine::AV_worldPos) / 4], sizeof(jip_form));
			o.anim = animation(l, flags, jip_form, o.atten, o.radius);
			o.kind = l.kind;
			o.asset_key = l.key;
			o.listed_by_engine = l.listed_scan == g_scan ? 1 : 0;
			// The engine's own off test: APP_CULLED on the light, or a dimmer
			// under kLightOffDimmer (a muzzle flash's light after the flash).
			const bool lit = (o.diffuse[0] + o.diffuse[1] + o.diffuse[2]) > 0.0f && o.dimmer >= engine::kLightOffDimmer;
			o.lit = lit && !(flags & 1u) && !(l.owner && owners::disabled(l.owner)) &&
				!(l.kind != FNVR_LK_PLACED && detached(l.light)) ? 1 : 0;
			// This frame's position from the parent: a light's own world
			// transform can lag the node it hangs on by an update.
			float c[3];
			if (composed_position(l.light, flags, c))
			{
				const float dx = c[0] - o.pos[0], dy = c[1] - o.pos[1], dz = c[2] - o.pos[2];
				const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (d > 1.0f)
				{
					g_fixups++;
					g_fixups_now++;
					g_fixup_max = (std::max)(g_fixup_max, d);
				}
				std::memcpy(o.pos, c, sizeof(c));
			}
			for (const float f : o.pos)
				if (!std::isfinite(f)) return false;
			return true;
		}
	}

	void set_placed(uintptr_t ref, const std::vector<uintptr_t>& found)
	{
		const uintptr_t ligh = found.empty() ? 0 : base_ligh(ref);
		auto& mine = g_by_ref[ref];
		// Gone from its 3D since the last walk.
		for (const uintptr_t l : mine)
			if (std::find(found.begin(), found.end(), l) == found.end())
				if (const auto it = g_lights.find(l); it != g_lights.end() && it->second.owner == ref) forget(l);
		std::vector<uintptr_t> now;
		for (size_t ordinal = 0; ordinal < found.size(); ordinal++)
		{
			const uintptr_t l = found[ordinal];
			if (const auto it = g_lights.find(l); it != g_lights.end())
			{
				if (it->second.kind == FNVR_LK_PLACED && it->second.owner == ref)
				{
					it->second.ligh = ligh;
					now.push_back(l);
					continue;
				}
				if (it->second.kind != FNVR_LK_EFFECT) continue;   // the player's, or another reference's
				forget(l);   // an effect light that turns out to be this reference's
			}
			if (!pins::pin(l, ref)) continue;
			Light e;
			e.light = l;
			e.owner = ref;
			e.kind = FNVR_LK_PLACED;
			e.key = placed_key(ref, ordinal);
			e.ligh = ligh;
			g_lights.emplace(l, e);
			now.push_back(l);
			g_placed_added++;
		}
		if (now.empty()) g_by_ref.erase(ref);
		else mine.swap(now);
	}

	void on_owner_dropped(uintptr_t ref)
	{
		const auto it = g_by_ref.find(ref);
		if (it == g_by_ref.end()) return;
		for (const uintptr_t l : it->second)
			if (const auto e = g_lights.find(l); e != g_lights.end() && e->second.owner == ref) forget(l);
		g_by_ref.erase(it);
	}

	void clear()
	{
		for (const auto& [light, l] : g_lights) pins::release(light, tag_of(l));
		if (!g_lights.empty()) log::f("LightReg", "cleared {} lights", g_lights.size());
		g_lights.clear();
		g_by_ref.clear();
		g_player = 0;
	}

	void tick(uint32_t frame)
	{
		// Effect lights only the plugin's pin holds: the engine let go of them.
		static std::vector<uintptr_t> s_gone;
		s_gone.clear();
		for (const auto& [light, l] : g_lights)
			if (l.kind == FNVR_LK_EFFECT && pins::refcount(light) <= 1) s_gone.push_back(light);
		for (const uintptr_t light : s_gone) forget(light);
		g_effect_released += static_cast<uint32_t>(s_gone.size());

		// The 600-frame registry status is a dev diagnostic: [Logging] Verbose only.
		if (frame - g_last_status >= 600 && log::verbose())
		{
			g_last_status = frame;
			uint32_t placed = 0, effect = 0, player = 0, listed = 0;
			for (const auto& [light, l] : g_lights)
			{
				(l.kind == FNVR_LK_PLACED ? placed : l.kind == FNVR_LK_PLAYER ? player : effect)++;
				if (l.listed_scan == g_scan) listed++;
			}
			log::f("LightReg", "registry: placed {} effect {} player {} (listed at the last scan {}) | placed +{} effect +{} "
				"effect released +{} player changes +{} | positions from the parent that differed from the light's own "
				"by > 1 unit +{} (largest {:.1f}) | pins live {}", placed, effect, player, listed, g_placed_added,
				g_effect_added, g_effect_released, g_player_changes, g_fixups_now, g_fixup_max, pins::live());
			g_effect_added = g_effect_released = g_placed_added = g_player_changes = g_fixups_now = 0;
			g_fixup_max = 0.0f;
		}
	}

	bool ok()
	{
		uint32_t ssn = 0;
		return guard::get(fnv_exe::addr().shadow_scene_node, ssn) && guard::plausible(ssn);
	}

	uint32_t fixups()
	{
		return g_fixups;
	}

	uint32_t copy(FnvRemixLight* out, uint32_t max)
	{
		scan();
		update_player();
		uint32_t n = 0;
		auto put = [&](Light& l) {
			// An effect light only the plugin's pin holds is one the engine let
			// go of since the last main loop: not sent while its release waits.
			if (l.kind == FNVR_LK_EFFECT && pins::refcount(l.light) <= 1) return;
			if (n < max && fill(l, out[n])) n++;
		};
		if (g_player)
			if (const auto it = g_lights.find(g_player); it != g_lights.end()) put(it->second);
		for (auto& [light, l] : g_lights)
			if (l.kind != FNVR_LK_PLAYER && l.listed_scan == g_scan) put(l);
		for (auto& [light, l] : g_lights)
			if (l.kind != FNVR_LK_PLAYER && l.listed_scan != g_scan) put(l);
		return n;
	}
}
