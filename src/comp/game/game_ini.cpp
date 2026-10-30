#include "std_include.hpp"
#include "game_ini.hpp"

#include "shared/common/config.hpp"
#include "shared/fnv_exe.hpp"

namespace comp::game::game_ini
{
	namespace
	{
		using LT = shared::common::LOG_TYPE;

		// Any: the name is looked up in both collections.
		enum class Coll : uint8_t { Ini, Renderer, Any };

		// The value each setting gets. The type follows the name's first
		// letter, as in the engine: b = u8, i = s32, f = float.
		struct Wanted
		{
			const char* name;
			Coll coll;
			double value;
		};

		// Fallout.ini settings go to the INISettingCollection, FalloutPrefs.ini
		// settings to the RendererSettingCollection: each name exists in one
		// collection only.
		constexpr Wanted kWanted[] = {
			{ "bDoCanopyShadowPass:Display", Coll::Ini, 0 },
			{ "bUseRefractionShader:Display", Coll::Ini, 0 },
			{ "bDoActorShadows:Display", Coll::Ini, 0 },
			{ "bLoadFaceGenHeadEGTFiles:General", Coll::Ini, 1 },

			{ "bDoHighDynamicRange:BlurShaderHDR", Coll::Renderer, 0 },
			{ "bDrawShadows:Display", Coll::Renderer, 0 },
			{ "bTransparencyMultisampling:Display", Coll::Renderer, 0 },
			{ "fShadowLODStartFade:Display", Coll::Renderer, 100 },
			{ "fSpecularLODStartFade:Display", Coll::Renderer, 200 },
			{ "iActorShadowCountExt:Display", Coll::Renderer, 0 },
			{ "iActorShadowCountInt:Display", Coll::Renderer, 0 },
			{ "iMaxAnisotropy:Display", Coll::Renderer, 1 },
			{ "iMultiSample:Display", Coll::Renderer, 1 },
			{ "iShadowFilter:Display", Coll::Renderer, 0 },
			{ "iShadowMapResolution:Display", Coll::Renderer, 256 },
			{ "iTexMipMapMinimum:Display", Coll::Renderer, 0 },
			// The launcher's Low preset skips each texture's top mip at load.
			{ "iTexMipMapSkip:Display", Coll::Renderer, 0 },
			{ "fDecalLOD1:Display", Coll::Renderer, 1000 },
			{ "fDecalLOD2:Display", Coll::Renderer, 1500 },
			{ "iWaterMultiSamples:Display", Coll::Renderer, 1 },
			{ "bDoDepthOfField:Imagespace", Coll::Renderer, 0 },
			{ "iRadialBlurLevel:Imagespace", Coll::Renderer, 0 },
			{ "fBlockLoadDistance:TerrainManager", Coll::Renderer, 131072 },
			{ "bAutoWaterSilhouetteReflections:Water", Coll::Renderer, 1 },
			{ "bForceHighDetailReflections:Water", Coll::Renderer, 0 },
			{ "bUseWaterDepth:Water", Coll::Renderer, 0 },
			{ "bUseWaterDisplacements:Water", Coll::Renderer, 0 },
			{ "bUseWaterReflectionBlur:Water", Coll::Renderer, 0 },
			{ "bUseWaterReflections:Water", Coll::Renderer, 0 },
			{ "bUseWaterRefractions:Water", Coll::Renderer, 0 },
			{ "iWaterReflectHeight:Water", Coll::Renderer, 256 },
			{ "iWaterReflectWidth:Water", Coll::Renderer, 256 },

			// Menus over the engine's frozen capture: rendered terminals, character
			// creation and the menus with their own 3D scene are drawn only on that
			// path (menu_bg.hpp).
			{ "bStaticMenuBackground:Display", Coll::Any, 1 },
		};

		// Setting: { vtable, value union at +4, const char* name at +8 }.
		constexpr uint32_t kSettingValue = 0x04;
		constexpr uint32_t kSettingName = 0x08;
		// SettingCollectionList: its list of Settings, a node { Setting*, next* }, at +0x10C.
		constexpr uint32_t kCollList = 0x10C;
		// More nodes than any collection holds: a longer walk is a broken list.
		constexpr uint32_t kMaxNodes = 8192;

		// Guarded memory access. These SEH frames hold no C++ objects.

		bool copy_unguarded(uintptr_t addr, void* out, size_t n)
		{
			std::memcpy(out, reinterpret_cast<const void*>(addr), n);
			return true;
		}

		bool read(uintptr_t addr, void* out, size_t n)
		{
			if (addr < 0x10000) return false;
			__try { return copy_unguarded(addr, out, n); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		template <class T>
		bool get(uintptr_t addr, T& out)
		{
			return read(addr, &out, sizeof(T));
		}

		bool write_unguarded(uintptr_t addr, const void* in, size_t n)
		{
			std::memcpy(reinterpret_cast<void*>(addr), in, n);
			return true;
		}

		bool write(uintptr_t addr, const void* in, size_t n)
		{
			__try { return write_unguarded(addr, in, n); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// A NUL-terminated name of at most 127 characters; empty when unreadable.
		std::string read_name(uintptr_t p)
		{
			char buf[128] = {};
			size_t n = 0;
			for (; n + 1 < sizeof(buf); n++)
				if (!get(p + n, buf[n]) || !buf[n]) break;
			if (n + 1 >= sizeof(buf)) return {};
			return std::string(buf, n);
		}

		struct Found
		{
			uintptr_t setting = 0;
			bool duplicate = false;
		};

		// Walks one collection's list and calls visit(name, setting) for each
		// Setting. Returns the number of nodes walked, or -1 with `why` set
		// when the collection doesn't look as expected.
		template <class Visit>
		int walk(uintptr_t global, uintptr_t want_vtbl, uintptr_t setting_vtbl, Visit&& visit, std::string& why)
		{
			uintptr_t c = 0, vtbl = 0;
			if (!get(global, c) || !c) { why = std::format("no collection at [0x{:X}]", global); return -1; }
			if (!get(c, vtbl) || vtbl != want_vtbl)
			{
				why = std::format("collection 0x{:X} has vtable 0x{:X}, expected 0x{:X}", c, vtbl, want_vtbl);
				return -1;
			}
			uintptr_t node = c + kCollList;
			int walked = 0;
			while (node)
			{
				if (++walked > static_cast<int>(kMaxNodes)) { why = "the settings list does not end"; return -1; }
				uintptr_t data = 0, next = 0;
				if (!get(node, data) || !get(node + 4, next)) { why = std::format("node 0x{:X} is unreadable", node); return -1; }
				if (data)
				{
					uintptr_t sv = 0, name_ptr = 0;
					if (get(data, sv) && sv == setting_vtbl && get(data + kSettingName, name_ptr) && name_ptr)
						visit(read_name(name_ptr), data);
				}
				node = next;
			}
			return walked;
		}

		// Records the Settings whose names are wanted.
		int walk_wanted(Coll coll, uintptr_t global, uintptr_t want_vtbl, uintptr_t setting_vtbl,
			Found (&found)[std::size(kWanted)], std::string& why)
		{
			return walk(global, want_vtbl, setting_vtbl, [&](const std::string& name, uintptr_t data)
				{
					for (size_t i = 0; i < std::size(kWanted); i++)
					{
						if ((kWanted[i].coll != coll && kWanted[i].coll != Coll::Any) ||
							_stricmp(name.c_str(), kWanted[i].name) != 0) continue;
						if (found[i].setting && found[i].setting != data) found[i].duplicate = true;
						else found[i].setting = data;
					}
				}, why);
		}

		std::string format_value(char type, uint32_t raw)
		{
			switch (type)
			{
			case 'b': return std::to_string(raw & 0xFF);
			case 'f': { float f; std::memcpy(&f, &raw, 4); return std::format("{:g}", f); }
			default: return std::to_string(static_cast<int32_t>(raw));
			}
		}

		bool g_done = false;
	}

	void apply()
	{
		if (g_done) return;
		g_done = true;

		if (!shared::common::config::get().get_bool("GameSettings", "Apply", true))
		{
			shared::common::log_event("GameSettings", "[GameSettings] Apply=0: the game's own INI values are used");
			return;
		}
		if (!fnv_exe::known())
		{
			shared::common::log_event("GameSettings", "Unknown executable: the mod's game settings are not applied", LT::LOG_TYPE_WARN);
			return;
		}

		const auto& a = fnv_exe::addr();
		Found found[std::size(kWanted)] = {};
		std::string why;
		const int ini_nodes = walk_wanted(Coll::Ini, a.ini_settings, a.vtbl_ini_settings, a.vtbl_ini_setting, found, why);
		if (ini_nodes < 0)
		{
			shared::common::log_event("GameSettings", "Fallout.ini settings: " + why + ". Nothing applied.", LT::LOG_TYPE_ERROR);
			return;
		}
		const int renderer_nodes = walk_wanted(Coll::Renderer, a.renderer_settings, a.vtbl_renderer_settings,
			a.vtbl_renderer_setting, found, why);
		if (renderer_nodes < 0)
		{
			shared::common::log_event("GameSettings", "FalloutPrefs.ini settings: " + why + ". Nothing applied.", LT::LOG_TYPE_ERROR);
			return;
		}

		int changed = 0, same = 0, missing = 0;
		std::string details;
		for (size_t i = 0; i < std::size(kWanted); i++)
		{
			const Wanted& w = kWanted[i];
			const char type = w.name[0];
			const uintptr_t value_addr = found[i].setting + kSettingValue;
			uint32_t old_raw = 0;
			if (!found[i].setting || found[i].duplicate || !get(value_addr, old_raw))
			{
				missing++;
				shared::common::log_event("GameSettings", std::format("{}: {}, left alone", w.name,
					found[i].duplicate ? "listed twice" : "not found"), LT::LOG_TYPE_WARN);
				continue;
			}

			uint32_t new_raw = old_raw;
			size_t size = 4;
			if (type == 'b') { new_raw = (old_raw & ~0xFFu) | (w.value != 0 ? 1u : 0u); size = 1; }
			else if (type == 'f') { const float f = static_cast<float>(w.value); std::memcpy(&new_raw, &f, 4); }
			else new_raw = static_cast<uint32_t>(static_cast<int32_t>(w.value));

			if (new_raw == old_raw) { same++; continue; }
			if (!write(value_addr, &new_raw, size))
			{
				missing++;
				shared::common::log_event("GameSettings", std::format("{}: the write failed, left alone", w.name), LT::LOG_TYPE_WARN);
				continue;
			}
			changed++;
			details += std::format("{}{} {} -> {}", details.empty() ? "" : ", ", w.name, format_value(type, old_raw),
				format_value(type, new_raw));
		}

		shared::common::log_event("GameSettings", std::format("Applied the mod's game settings in memory (the INI files are "
			"not changed): {} changed, {} already set, {} left alone ({} + {} settings listed)", changed, same, missing,
			ini_nodes, renderer_nodes));
		if (!details.empty()) shared::common::log_event("GameSettings", "Changed: " + details);
	}
}
