#include "std_include.hpp"
#include "host_settings.hpp"

#include "remix/remix_c.h"
#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/common/ffp_state.hpp"
#include "comp/game/game.hpp"
#include "comp/game/api_submit.hpp"
#include "comp/game/stable_origin.hpp"
#include "comp/game/tex_pack.hpp"
#include "comp/game/vats.hpp"
#include "comp/game/game_ini.hpp"
#include "comp/game/scene_statics.hpp"
#include "comp/game/session_report.hpp"

namespace comp::host_settings
{
	namespace
	{
		// Info: a read-only line of text, registered as a one-label ENUM the
		// menu draws disabled, and registered again when the text changes.
		enum class Kind : uint8_t { Bool, Int, Float, Enum, Action, Info };

		// When a change takes effect.
		//   Live:    the value is read where it is used (every draw / frame),
		//            or a hook switches the feature over between frames.
		//   Rebuild: read when an API mesh / material is built: new ones at
		//            once, built ones after "Rebuild API meshes and textures".
		//   Restart: read once at startup; the menu only writes the ini.
		enum class Apply : uint8_t { Live, Rebuild, Restart };

		struct Setting;
		// Replaces the plain field write for a setting that has to switch a
		// feature over (teardown / bring-up); it writes the field itself.
		using Hook = void (*)(IDirect3DDevice9* dev, Setting& s, double v);

		struct Setting
		{
			const char* ui;             // menu section
			const char* sec;            // ini section; null for actions
			const char* key;
			const char* label;
			const char* tip;
			Kind  kind;
			Apply apply;
			void* field;                // bool* / int* / float*; null for actions
			double def, lo, hi, step;
			const char* const* enum_labels;
			uint32_t enum_count;
			Hook hook;
			bool user_menu = false;     // also in the user menu (REMIXAPI_HOST_SETTING_FLAG_USER_MENU)

			remixapi_HostSettingHandle handle = 0;
			double last = 0.0;          // value last applied (or registered)
		};

		constexpr UINT kWarmupFrames = 120;   // let the bridge finish its handshake
		constexpr UINT kPollFrames = 10;
		constexpr const char* kActionRebuild = "FNV.Action.RebuildApi";
		constexpr const char* kActionSaveReport = "FNV.Action.SaveReport";
		constexpr const char* kActionShowReport = "FNV.Action.ShowReport";
		constexpr const char* kInfoSavedReport = "FNV.Info.SavedReport";
		// Frames between two looks at the saved report's text.
		constexpr UINT kInfoFrames = 30;

		std::vector<Setting> s_settings;
		std::vector<remixapi_HostSettingHandle> s_handles;   // parallel to s_settings
		std::vector<double> s_values;
		bool s_started = false;
		bool s_ready = false;
		UINT s_frames = 0;
		uint64_t s_generation = 0;
		UINT s_info_frames = 0;

		// The Saved report line's text and its one label.
		std::string s_saved_text = "none yet";
		const char* s_saved_labels[1] = { "none yet" };
		uint32_t s_saved_generation = 0;

		PFN_remixapi_RegisterHostSetting       s_register = nullptr;
		PFN_remixapi_GetHostSettingsGeneration s_get_generation = nullptr;
		PFN_remixapi_GetHostSettingValue       s_get = nullptr;
		PFN_remixapi_SetHostSettingValue       s_set = nullptr;
		PFN_remixapi_ClearHostSettings         s_clear = nullptr;
		PFN_remixapi_PollHostSettings          s_poll = nullptr;   // bridge only; optional

		const char* const kLightRangeModes[] = { "Light radius", "Attenuation reaches 1/255", "Unbounded" };
		const char* const kMenuBgRedirectModes[] = {
			"In place only", "In place, else backbuffer", "Always backbuffer" };
		const char* const kLodRules[] = {
			"In view and not drawn (view)", "Engine swap / unload (engine)" };
		const char* const kSceneBlendModes[] = {
			"None (draw capture)", "Blended cutouts (blend + alpha test)", "All plain alpha blending" };

		void add(const char* ui, const char* sec, const char* key, const char* label, Kind kind, Apply apply,
			void* field, double def, double lo, double hi, double step, const char* tip, Hook hook = nullptr,
			const char* const* labels = nullptr, uint32_t count = 0)
		{
			s_settings.push_back({ ui, sec, key, label, tip, kind, apply, field, def, lo, hi, step, labels, count, hook });
		}

		void add_bool(const char* ui, const char* sec, const char* key, const char* label, Apply apply,
			bool* field, bool def, const char* tip, Hook hook = nullptr)
		{
			add(ui, sec, key, label, Kind::Bool, apply, field, def ? 1.0 : 0.0, 0.0, 1.0, 0.0, tip, hook);
		}

		void add_int(const char* ui, const char* sec, const char* key, const char* label, Apply apply,
			int* field, int def, int lo, int hi, double step, const char* tip, Hook hook = nullptr)
		{
			add(ui, sec, key, label, Kind::Int, apply, field, def, lo, hi, step, tip, hook);
		}

		void add_float(const char* ui, const char* sec, const char* key, const char* label, Apply apply,
			float* field, float def, float lo, float hi, double step, const char* tip, Hook hook = nullptr)
		{
			add(ui, sec, key, label, Kind::Float, apply, field, def, lo, hi, step, tip, hook);
		}

		void add_enum(const char* ui, const char* sec, const char* key, const char* label, Apply apply,
			int* field, int def, const char* const* labels, uint32_t count, const char* tip, Hook hook = nullptr)
		{
			add(ui, sec, key, label, Kind::Enum, apply, field, def, 0.0, static_cast<double>(count - 1), 0.0, tip,
				hook, labels, count);
		}

		void add_action(const char* ui, const char* key, const char* label, const char* tip)
		{
			add(ui, nullptr, key, label, Kind::Action, Apply::Live, nullptr, 0.0, 0.0, 0.0, 0.0, tip);
		}

		// `labels`: the one label that holds the text.
		void add_info(const char* ui, const char* key, const char* label, const char* const* labels, const char* tip)
		{
			add(ui, nullptr, key, label, Kind::Info, Apply::Live, nullptr, 0.0, 0.0, 0.0, 0.0, tip, nullptr, labels, 1);
		}

		// ---- Field access ------------------------------------------------------------

		double read_field(const Setting& s)
		{
			switch (s.kind)
			{
			case Kind::Bool:  return *static_cast<bool*>(s.field) ? 1.0 : 0.0;
			case Kind::Int:
			case Kind::Enum:  return static_cast<double>(*static_cast<int*>(s.field));
			case Kind::Float: return static_cast<double>(*static_cast<float*>(s.field));
			default:          return 0.0;
			}
		}

		void write_field(const Setting& s, double v)
		{
			switch (s.kind)
			{
			case Kind::Bool:  *static_cast<bool*>(s.field) = v != 0.0; break;
			case Kind::Int:
			case Kind::Enum:  *static_cast<int*>(s.field) = static_cast<int>(std::llround(v)); break;
			case Kind::Float: *static_cast<float*>(s.field) = static_cast<float>(v); break;
			default: break;
			}
		}

		std::string format_ini(const Setting& s, double v)
		{
			switch (s.kind)
			{
			case Kind::Bool: return v != 0.0 ? "1" : "0";
			case Kind::Int:
			case Kind::Enum: return std::format("{}", static_cast<int>(std::llround(v)));
			default:
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "%.6g", v);
				return buf;
			}
			}
		}

		Setting* find(const char* sec, const char* key)
		{
			for (auto& s : s_settings)
				if (s.sec && _stricmp(s.sec, sec) == 0 && _stricmp(s.key, key) == 0) return &s;
			return nullptr;
		}

		// ---- ini write-back ----------------------------------------------------------

		bool is_space(char c) { return c == ' ' || c == '\t'; }

		std::string_view trim(std::string_view v)
		{
			while (!v.empty() && is_space(v.front())) v.remove_prefix(1);
			while (!v.empty() && (is_space(v.back()) || v.back() == '\r' || v.back() == '\n')) v.remove_suffix(1);
			return v;
		}

		bool iequals(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
		}

		// Sets [section] key=value in the ini text and leaves every other byte
		// as it was. Like GetPrivateProfileString, the first matching key of the
		// section wins. A key the section doesn't have yet goes right after the
		// section's last key line (not after the comment banner of the section
		// that follows); a missing section is appended at the end.
		void ini_set(std::string& text, const char* section, const char* key, const std::string& value)
		{
			const char* eol = (text.empty() || text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
			bool in_section = false;     // inside any [section] block
			bool in_first = false;       // inside the first one (where a new key goes)
			bool seen_section = false;
			size_t insert_at = 0;        // just past the first block's header or last key line

			size_t pos = 0;
			while (pos < text.size())
			{
				const size_t end = text.find('\n', pos);
				const size_t next = end == std::string::npos ? text.size() : end + 1;
				const size_t content_end = end == std::string::npos ? text.size()
					: (end > pos && text[end - 1] == '\r' ? end - 1 : end);
				const std::string_view line(text.data() + pos, content_end - pos);
				const std::string_view t = trim(line);

				if (!t.empty() && t.front() == '[')
				{
					const size_t close = t.find(']');
					const std::string_view name = trim(t.substr(1, close == std::string_view::npos ? t.size() - 1 : close - 1));
					in_section = iequals(name, section);
					in_first = in_section && !seen_section;
					if (in_first)
					{
						seen_section = true;
						insert_at = next;
					}
				}
				else if (in_section && !t.empty() && t.front() != ';' && t.front() != '#')
				{
					const size_t eq = line.find('=');
					if (eq != std::string_view::npos)
					{
						if (iequals(trim(line.substr(0, eq)), key))
						{
							text.replace(pos + eq + 1, content_end - (pos + eq + 1), value);
							return;
						}
						if (in_first) insert_at = next;
					}
				}
				pos = next;
			}

			const std::string entry = std::string(key) + "=" + value + eol;
			if (seen_section)
			{
				// A header or key line on the last line without its own newline.
				if (insert_at >= text.size() && !text.empty() && text.back() != '\n')
				{
					text += eol;
					insert_at = text.size();
				}
				text.insert((std::min)(insert_at, text.size()), entry);
				return;
			}
			if (!text.empty() && text.back() != '\n') text += eol;
			text += std::string(eol) + "[" + section + "]" + eol + entry;
		}

		void save_to_ini(const Setting& s, double v)
		{
			if (!s.sec) return;
			const std::string& path = shared::common::config::get().ini_path();
			if (path.empty()) return;

			std::string text;
			{
				std::ifstream in(path, std::ios::binary);
				if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			}
			ini_set(text, s.sec, s.key, format_ini(s, v));

			const std::string tmp = path + ".tmp";
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				out.write(text.data(), static_cast<std::streamsize>(text.size()));
				if (!out)
				{
					shared::common::log("HostSettings", std::format("could not write {}", tmp),
						shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
					return;
				}
			}
			if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				shared::common::log("HostSettings", std::format("could not replace {} (error {})", path, GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				DeleteFileA(tmp.c_str());
			}
		}

		// A value the wrapper changed on its own (a dependent setting, a clamp):
		// menu and ini follow.
		void push_value(Setting& s, double v, bool save)
		{
			s.last = v;
			if (s.handle && s_set) s_set(s.handle, v);
			if (save) save_to_ini(s, v);
		}

		// ---- Hooks: features that switch over between frames -------------------------

		void hook_stable_origin(IDirect3DDevice9* dev, Setting&, double v)
		{
			auto& r = shared::common::config::get().remix_api;
			const bool on = v != 0.0;
			if (!on && r.retained_instances)
			{
				// Retained instances are placed relative to the origin: they go
				// first, and RetainedInstances with them (menu and ini too).
				game::api_submit::retained_switched_off();
				r.retained_instances = false;
				if (Setting* ri = find("RemixApi", "RetainedInstances")) push_value(*ri, 0.0, true);
				shared::common::log("HostSettings", "RetainedInstances switched off too: it needs StableOrigin",
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			game::stable_origin::set_enabled(dev, on);
		}

		void hook_retained(IDirect3DDevice9*, Setting&, double v)
		{
			auto& r = shared::common::config::get().remix_api;
			const bool on = v != 0.0;
			if (on == r.retained_instances) return;
			if (!on)
			{
				game::api_submit::retained_switched_off();
				r.retained_instances = false;
				return;
			}
			// Entries get their retained instance on their next draw.
			r.retained_instances = true;
			if (!r.stable_origin)
				shared::common::log("HostSettings", "RetainedInstances needs StableOrigin -- API draws keep DrawInstance",
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		// SceneGraphStatics: off destroys every scene instance, mesh and material
		// at once (their passes reach draw capture next frame); on starts with
		// the next tick.
		void hook_scene_statics(IDirect3DDevice9*, Setting&, double v)
		{
			auto& r = shared::common::config::get().remix_api;
			const bool on = v != 0.0;
			if (on == r.scene_graph_statics) return;
			r.scene_graph_statics = on;
			if (!on)
			{
				game::scene_statics::drop_all("SceneGraphStatics off");
				return;
			}
			if (!(r.retained_instances && r.stable_origin))
				shared::common::log("HostSettings", "SceneGraphStatics needs RetainedInstances and StableOrigin -- statics "
					"stay with draw capture", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		// A scene-statics classification rule changed: every geometry is
		// classified again (instances, meshes and materials go; statics go
		// through draw capture until they are claimed again under the new rule).
		void hook_scene_reclassify(IDirect3DDevice9*, Setting& s, double v)
		{
			const double old = read_field(s);
			write_field(s, v);
			if (old != read_field(s) && shared::common::config::get().remix_api.scene_graph_statics)
				game::scene_statics::drop_all(std::format("{} changed", s.key).c_str());
		}

		void hook_scene_draw_instance(IDirect3DDevice9*, Setting& s, double v)
		{
			write_field(s, v);
			game::scene_statics::on_draw_instance_switched();
		}

		// Radius stays at least one grid step, as the loader keeps it.
		void hook_origin_grid(IDirect3DDevice9*, Setting& s, double v)
		{
			write_field(s, v);
			auto& r = shared::common::config::get().remix_api;
			if (r.stable_origin_radius < r.stable_origin_grid)
			{
				r.stable_origin_radius = r.stable_origin_grid;
				if (Setting* rs = find("RemixApi", "StableOriginRadius")) push_value(*rs, r.stable_origin_radius, true);
			}
		}

		void hook_origin_radius(IDirect3DDevice9*, Setting& s, double v)
		{
			const double grid = shared::common::config::get().remix_api.stable_origin_grid;
			const double clamped = (std::max)(v, grid);
			write_field(s, clamped);
			if (clamped != v) push_value(s, clamped, true);
		}

		void hook_ffp_enabled(IDirect3DDevice9*, Setting& s, double v)
		{
			write_field(s, v);
			shared::common::ffp_state::get().set_enabled(v != 0.0);
		}

		// The light pass reads game.hpp's copies, taken from the ini at startup.
		void hook_lights(IDirect3DDevice9*, Setting& s, double v)
		{
			write_field(s, v);
			auto& l = shared::common::config::get().lights;
			game::lights_enabled = l.enabled;
			game::light_intensity = static_cast<float>(l.intensity_percent) / 100.0f;
			game::light_range_mode = l.range_mode;
			game::effect_light_forward = static_cast<float>(l.effect_forward);
			game::effect_light_third_person_forward = static_cast<float>(l.effect_third_person_forward);
			game::pipboy_light_intensity = static_cast<float>(l.follower_intensity_percent) / 100.0f;
		}

		// ---- The table ---------------------------------------------------------------

		void build_table()
		{
			s_settings.clear();
			auto& c = shared::common::config::get();
			auto& r = c.remix_api;
			auto& f = c.ffp;
			const Apply L = Apply::Live, B = Apply::Rebuild, S = Apply::Restart;

			// ---- [RemixApi] ----
			const char* R = "RemixApi";
			add_action(R, kActionRebuild, "Rebuild API meshes and textures",
				"Destroy every Remix API mesh, material, retained instance and texture (land, far LOD, water, "
				"statics), so each is built again from the current settings as the game draws it -- including "
				"textures, through the path ImportD3D9Textures selects now. Rebuilds are throttled by "
				"MaxBuildsPerFrame; draws not rebuilt yet are not drawn meanwhile, so things may flicker for a moment.");
			add_bool(R, "RemixApi", "StableOrigin", "Stable render origin", L, &r.stable_origin, true,
				"Hand Remix a world frame anchored at the camera snapped to a StableOriginGrid grid, re-snapped "
				"once the camera is StableOriginRadius away, instead of FNV's camera-relative frame (every "
				"transform changes as the camera moves). Rasterised output is unchanged. Switching it off also "
				"switches RetainedInstances off (it needs the origin).", hook_stable_origin);
			add_float(R, "RemixApi", "StableOriginGrid", "Stable origin grid", L, &r.stable_origin_grid,
				4096.0f, 64.0f, 65536.0f, 64.0, "Origin snap step, world units. Radius stays at least this.",
				hook_origin_grid);
			add_float(R, "RemixApi", "StableOriginRadius", "Stable origin radius", L, &r.stable_origin_radius,
				8192.0f, 64.0f, 1.0e6f, 64.0,
				"The origin re-snaps once the camera is this far from it (world units, at least one grid step).",
				hook_origin_radius);
			add_bool(R, "RemixApi", "PublishWorldOrigin", "Publish world origin", L, &r.publish_world_origin, true,
				"Tell the runtime the stable origin (game values __mcrtx.world_origin.x/y/z, sent when it changes), so "
				"render position + origin = world position. Needs StableOrigin and rtx.worldOrigin.enable = True in "
				"rtx.conf: clouds and their ground shadows then stay put in the world instead of following the "
				"player, and an origin re-snap is neither motion nor a camera cut. Off sends the origin as zero once.");
			add_bool(R, "RemixApi", "FallbackCamera", "Fallback camera", L, &r.fallback_camera, true,
				"Hand Remix the game's world camera once per frame (SetupCamera, FALLBACK). Remix uses it only on a "
				"frame where no D3D9 world draw gave it a camera (the world all went through the API), which would "
				"otherwise show a flat colour and skip path tracing. Needs a bridge that forwards SetupCamera.");
			add_bool(R, "RemixApi", "RetainedInstances", "Retained instances", L, &r.retained_instances, true,
				"Register land, far LOD and water meshes once and replay them every frame; only real changes "
				"cross the bridge. Needs StableOrigin. Off destroys every retained instance: those meshes draw with "
				"DrawInstance, scene statics go to draw capture, grass and leaves draw per frame. On: entries get "
				"their instance on their next draw. Scene statics (SceneGraphStatics, which keep objects behind the "
				"camera) start only when this is on at startup: switching it on here leaves them on draw capture "
				"until the game restarts.",
				hook_retained);
			add_bool(R, "RemixApi", "RetainedReplayAtFrameStart", "Replay retained at frame start", L,
				&r.retained_replay_at_frame_start, true,
				"Issue the frame's DrawRetainedInstances at its first BeginScene, with this frame's origin, before the "
				"HUD draw that makes Remix trace the frame. The frame's updates still go out at Present. Off = at "
				"Present, after the trace (needs rtx.retained.replayAtInjection, which is one frame late).");
			add_bool(R, "RemixApi", "RetainedKeepCulled", "Keep retained when culled", L, &r.retained_keep_culled, true,
				"A retained entry the game did not draw this frame stays visible (shadows / reflections from behind "
				"the camera, no re-instancing on camera turns). Hidden when its cell leaves the loaded grid, the "
				"interior changes or the camera jumps (land, near water, statics), when its geometry dies (statics), "
				"or, for far LOD and distant water, as RetainedLodRule says. "
				"Off = hidden the first frame it is not drawn (old behaviour).");
			add_int(R, "RemixApi", "RetainedKeepCulledFrames", "Kept-culled TTL (frames)", L,
				&r.retained_keep_culled_frames, 36000, 2, 100000000, 100,
				"A visible entry kept while not drawn is destroyed after this many frames undrawn. Hidden entries "
				"keep EvictFrames.");
			add_int(R, "RemixApi", "RetainedKeepCulledCap", "Kept-culled cap (per feature)", L,
				&r.retained_keep_culled_cap, 8192, 16, 1000000, 64,
				"Above this many entries in one feature, EvictFrames applies to all of them (memory guard).");
			add_enum(R, "RemixApi", "RetainedLodRule", "LOD hide rule", L, &r.retained_lod_rule, 1, kLodRules, 2,
				"What hides a far-LOD / distant-water block the game didn't draw. Engine (default): only what the "
				"engine did to the block's NiGeometry -- destroyed, drawn again as another entry, app-culled or "
				"detached from the scene; a block culled or occluded in view stays. View: hidden once it is in "
				"view and not drawn for RetainedLodStaleFrames (the first keep-culled rule). Blocks whose "
				"NiGeometry is unknown (no dispatcher hook) always use View.");
			add_int(R, "RemixApi", "RetainedLodStaleFrames", "LOD hidden when not drawn (frames)", L,
				&r.retained_lod_stale_frames, 10, 1, 100000, 1,
				"View rule: frames in view and not drawn before a far-LOD / distant-water block hides. Off screen "
				"the last verdict is held.");
			add_bool(R, "RemixApi", "ImportD3D9Textures", "Import D3D9 textures", L, &r.import_d3d9_textures, true,
				"Hand game textures to Remix by reference (bridge remixapi_ImportD3D9Texture) instead of "
				"uploading their pixels. Affects textures built after the change only: already-built textures keep "
				"the path they used until they are evicted, or until Rebuild API meshes and textures.");
			add_int(R, "RemixApi", "TerrainWeightRes", "Terrain weight texture size", B, &r.terrain_weight_res,
				65, 5, 257, 1, "Weight / tint texture edge per land quad, in texels. 65 puts a texel on every vertex.");
			add_float(R, "RemixApi", "TerrainRoughness", "Terrain roughness", B, &r.terrain_roughness,
				0.85f, 0.0f, 1.0f, 0.005, "Land roughness (metallic 0). FNV land has no specular.");
			add_bool(R, "RemixApi", "TerrainNormalGloss", "Normal-map alpha is gloss", B, &r.terrain_normal_gloss, true,
				"Land (and statics) normal maps with real alpha get roughness = 1 - alpha (NVR convention); "
				"the rest keep TerrainRoughness. Needs rtx.fnvProtocol.normalAlphaMode = 1.");
			add_bool(R, "RemixApi", "TerrainHeightMaps", "Terrain height maps (parallax)", L, &r.terrain_height_maps, true,
				"Each land layer's texture-pack height map (_p) goes to the terrain material: parallax on the blended "
				"layers, depth rtx.fnvProtocol.heightDisplaceIn. Needs [TexturePacks] HeightMaps. Land quads rebuild "
				"when this changes and once a height map that was still loading arrives.");
			add_float(R, "RemixApi", "FarLodRoughness", "Far LOD roughness", B, &r.far_lod_roughness,
				0.9f, 0.0f, 1.0f, 0.005, "Roughness of distant land LOD blocks.");
			add_bool(R, "RemixApi", "FarLodDetail", "Far LOD normal maps and noise", L, &r.far_lod_detail, true,
				"Distant land LOD blocks get their own normal map and the game's LOD noise over the albedo, as the "
				"game's far-LOD shader does. Blocks rebuild as they are drawn again.");
			add_int(R, "RemixApi", "FarLodRebuildFrames", "Far LOD rebuild interval (frames)", L,
				&r.far_lod_rebuild_frames, 30, 0, 100000, 1,
				"A far-LOD block whose sink moved is rebuilt at most once per this many frames.");
			add_int(R, "RemixApi", "MaxBuildsPerFrame", "Max builds per frame", L, &r.max_builds_per_frame,
				16, 1, 100000, 1,
				"New API objects per frame across all features; a draw over budget is not drawn that frame (grass, "
				"leaves and unlit FX keep drawing their current copy) and waits in a fair queue, oldest first. "
				"Meshes of up to 512 vertices have their own 64 per frame.");
			add_int(R, "RemixApi", "EvictFrames", "Mesh evict frames", L, &r.evict_frames, 900, 2, 1000000, 10,
				"Frames an API mesh may go undrawn before it is destroyed.");
			add_int(R, "RemixApi", "TextureEvictFrames", "Texture evict frames", L, &r.texture_evict_frames,
				1800, 2, 1000000, 10, "Frames an unused uploaded texture is kept for reuse before it is destroyed.");
			add_int(R, "RemixApi", "StatsIntervalFrames", "Status line interval (frames)", L,
				&r.stats_interval_frames, 600, 0, 1000000, 10, "Periodic RemixApi status line. 0 = off.");

			// ---- [RemixApi] water ----
			const char* W = "RemixApi: Water";
			add_float(W, "RemixApi", "WaterIor", "Water IOR", B, &r.water_ior, 1.33f, 1.0f, 3.0f, 0.005,
				"Refractive index of the water medium.");
			add_float(W, "RemixApi", "WaterMeasurementDistance", "Water measurement distance", B,
				&r.water_measurement_distance, 256.0f, 1.0f, 1.0e6f, 1.0,
				"World units over which light reaches the transmittance colour (256 = 3.7 m), scaled by the "
				"water colours' brightness.");
			add_float(W, "RemixApi", "WaterClarity", "Water clarity", B, &r.water_clarity, 0.8f, 0.05f, 1.0f, 0.005,
				"Scales the transmittance colour: lower = murkier.");
			add_float(W, "RemixApi", "WaterTileSize", "Ripple tile size", L, &r.water_tile_size,
				1120.0f, 0.0f, 1.0e6f, 16.0,
				"Ripple normal-map tile in world units (1120 = 16 m); 0 = the water shader's TexScale. Water "
				"meshes rebuild as they are drawn.");
			add_int(W, "RemixApi", "WaterNormalRes", "Ripple normal map size", B, &r.water_normal_res,
				1024, 64, 1024, 1, "Procedural ripple normal map edge, texels (power of two, max 1024).");
			add_float(W, "RemixApi", "WaterNormalStrength", "Ripple strength", B, &r.water_normal_strength,
				1.0f, 0.0f, 8.0f, 0.01, "Slope strength of the ripple normal map.");
			add_bool(W, "RemixApi", "WaterRuntimeOptions", "Push animated-water options", L, &r.water_runtime_options,
				true, "Push rtx.translucentMaterial.animatedWater* from the values below. Off stops pushing (the "
				"runtime keeps the last values).");
			add_float(W, "RemixApi", "WaterFlowSpeed", "Flow speed", L, &r.water_flow_speed, 21.0f, 0.0f, 10000.0f,
				0.5, "Primary ripple drift, world units/s (21 ~ 0.3 m/s).");
			add_float(W, "RemixApi", "WaterDetailScale", "Swell layer scale", L, &r.water_detail_scale,
				0.31f, 1.0f / 64.0f, 64.0f, 0.005, "Second (swell) layer's UV scale relative to the first.");
			add_float(W, "RemixApi", "WaterDetailFlowSpeed", "Swell layer flow speed", L, &r.water_detail_flow_speed,
				35.0f, 0.0f, 10000.0f, 0.5, "Second layer's drift, world units/s.");
			add_float(W, "RemixApi", "WaterDetailLodBias", "Swell layer LOD bias", L, &r.water_detail_lod_bias,
				0.0f, 0.0f, 16.0f, 0.05, "Extra blur on the second layer's sample, in mips.");
			add_float(W, "RemixApi", "WaterLodNearBlend", "Distant water follows near water", L,
				&r.water_lod_near_blend, 1.0f, 0.0f, 1.0f, 0.01,
				"Blend distant water's colours toward the near water in view: 1 = one shared material, 0 = its "
				"own colours. Distant water meshes rebuild as they are drawn.");
			add_bool(W, "RemixApi", "Underwater", "Underwater look", L, &r.underwater, true,
				"Drive the runtime's underwater look from the engine: rtx.underwater.enable and surfaceHeight from "
				"the water at the camera, and the camera medium while the camera is below the surface.");

			// ---- [RemixApi] statics ----
			const char* St = "RemixApi: Statics";
			add_bool(St, "RemixApi", "StaticsDebugPlainMaterial", "Debug: plain static materials", L,
				&r.statics_debug_plain_material, false,
				"New static meshes (scene graph and draw capture) get an albedo-only material (no terrain-layer "
				"normal, height, glow, cutout or vertex colour). Tells a material problem from a placement one.");

			// ---- [RemixApi] unlit FX, particles, first person, glow tint ----
			const char* Fx = "RemixApi: FX & first person";
			add_float(Fx, "RemixApi", "UnlitEmissiveIntensity", "Unlit FX emissive intensity", L,
				&r.unlit_emissive_intensity, 1.0f, 0.0f, 100.0f, 0.05,
				"The unlit material's texture is also its glow at this intensity, tinted by MaterialColor (a tinted one "
				"needs ApiGlowTint, else no glow). 0 = lit, no glow. Additive draws get none: the runtime "
				"already makes them emissive. New materials are built as the FX draw.");
			add_bool(Fx, "RemixApi", "UnlitDebugLog", "Unlit FX: log each distinct draw", L, &r.unlit_debug_log, false,
				"One [RemixApi] 'unlit draw' line per distinct draw: shaders, texture (D3D9 hash, rtx.conf list), UV "
				"transform, vertex colour, MaterialColor, blend / test / cull state and where it went (API, or not drawn "
				"with the reason). At most 256 a session.");
			add_bool(Fx, "RemixApi", "ViewModelCamera", "First person: VIEW_MODEL camera", L, &r.view_model_camera, true,
				"First-person API draws (arms, weapon, FX) carry the VIEW_MODEL category, and the frame's zoomed projection "
				"goes to the runtime as its VIEW_MODEL camera. With rtx.viewModel.enable the runtime perspective-corrects "
				"them; without it they are traced as world geometry, as before.");
			add_bool(Fx, "RemixApi", "ApiGlowTint", "Coloured glow tints via API", L, &r.api_glow_tint, true,
				"A glow map with a coloured EmittanceColor (and the unlit FX's MaterialColor) is tinted on the API "
				"material: pushes rtx.fnvProtocol.apiGlowTint to the runtime (needs a runtime that has it). Off: coloured "
				"glow is dropped (grey tints still scale it). Materials already built keep their tint until rebuilt.");
			add_bool(Fx, "RemixApi", "ShaderBakeHair", "Hair albedo from the game's shader", L, &r.shader_bake_hair, true,
				"Hair, beards and brows take their colour from the game's own hair shader, run on the CPU with its "
				"lighting taken out, instead of the LayerMap blend. Baked on a background thread: the hair keeps its "
				"current look until its bake is ready, then switches. A shader the bake can't separate from its lighting "
				"is logged ([HairBake]) and keeps the LayerMap blend. Hair textures loaded while this is on keep a CPU "
				"copy for the bake.");

			// ---- [RemixApi] scene graph ----
			const char* Sg = "RemixApi: Scene graph";
			add_int(Sg, "RemixApi", "SceneWalkBudgetUs", "Scene seed budget (us / frame)", L, &r.scene_walk_budget_us,
				400, 50, 100000, 50, "Time the seed pass may take per frame, in microseconds: members of the loaded "
				"scene graph (from the engine monitor's reference walks) recorded from their packed buffers and "
				"properties, so they are traced before the engine ever draws them.");
			add_bool(Sg, "RemixApi", "SceneGraphStatics", "Statics from the scene graph", L, &r.scene_graph_statics, true,
				"Rigid statics are discovered when the engine renders them (the render-pass dispatcher): "
				"the first base pass gives the material, the geometry's packed vertex / index buffer the mesh (shared "
				"by clones), and each NiGeometry becomes one retained instance that stays in the ray-traced scene while "
				"culled. From the next frame its render passes are skipped in the engine. Lifetime follows the engine "
				"(~NiGeometry, the cell leaving the loaded grid); hides come from app-cull / detach / disabled "
				"references; movers send a transform only when they move. Skinned, effect-blended, water and land "
				"geometry keep their own routes (alpha-blended statics: see the alpha-blended setting below); what it "
				"does not take goes through draw capture (one DrawInstance per draw). Needs RetainedInstances and "
				"StableOrigin. Off destroys every scene instance: statics go through draw capture from the next frame.",
				hook_scene_statics);
			add_int(Sg, "RemixApi", "SceneUploadKiBPerFrame", "Scene mesh upload (KiB / frame)", L,
				&r.scene_upload_kib_per_frame, 4096, 64, 1024 * 1024, 64,
				"Vertex + index bytes of new scene meshes per frame (at least one mesh always goes).");
			add_enum(Sg, "RemixApi", "SceneClaimAlphaBlend", "Scene statics: alpha-blended geometry", L,
				&r.scene_claim_alpha_blend, 2, kSceneBlendModes, 3,
				"Alpha-blended statics (NiAlphaProperty blend bit) taken as retained instances with the draw's own alpha "
				"state (test + blend on the instance). Blended cutouts: blend AND alpha test (foliage cards, fences, "
				"grates, decal edges). All: also blend without a test (glass, plain decals). Only plain alpha blending "
				"(SrcAlpha / InvSrcAlpha); additive and other effect blends go through draw capture. "
				"A change re-classifies every static.", hook_scene_reclassify);
			add_bool(Sg, "RemixApi", "SceneDecalCategory", "Decals -> decal category (every path)", L,
				&r.scene_decal_category, true,
				"Scene statics, draw capture and unlit FX: a draw whose shader property has a decal flag (decal, "
				"dynamic decal, alpha decal; the NVSE plugin sets it on the overlays its rules name) gets the "
				"runtime's DECAL_STATIC category (DECAL_DYNAMIC for the dynamic-decal flag or a moving draw), so "
				"Remix blends it onto the surface under it instead of tracing it as a surface of its own. [Decal] "
				"log lines say which rule tagged what. A change re-classifies every static.",
				hook_scene_reclassify);
			add_bool(Sg, "RemixApi", "SceneGlowGreyTint", "Scene statics: grey glow tints", L,
				&r.scene_glow_grey_tint, true,
				"A glow map with a grey EmittanceColor is taken: black drops the glow, grey scales the emissive "
				"intensity. Coloured tints ride on the material's emissive constant (ApiGlowTint). A change "
				"re-classifies every static.", hook_scene_reclassify);
			add_bool(Sg, "RemixApi", "SceneLandShapeStatics", "Scene statics: land-shaped placed objects", L,
				&r.scene_land_shape_statics, true,
				"A colour + two-texcoord draw (the land shape) whose geometry hangs under a placed reference and whose "
				"pixel shader is not a land shader is taken as a static instead of refused as terrain. A change "
				"re-classifies every static.", hook_scene_reclassify);
			add_bool(Sg, "RemixApi", "SceneStreamedRelaxed", "Scene statics: fill-once buffers (relaxed streamed rule)", L,
				&r.scene_streamed_relaxed, true,
				"A static whose vertex / index buffer was locked with DISCARD / NOOVERWRITE is still taken when the buffer "
				"is not DYNAMIC (D3D9 ignores both flags on any other buffer), was not written again within 300 frames, "
				"and has been left alone for 30 frames. A buffer written again later gives its statics to draw capture at "
				"once and they are re-examined. Off = the strict rule: any such lock refuses the buffer for its life "
				"(streamed.sticky). A change re-classifies every static.", hook_scene_reclassify);
			add_int(Sg, "RemixApi", "SceneDebugRefusalReport", "Debug: refused statics to report", L,
				&r.scene_debug_refusal_report, 5, 0, 1000, 1,
				"Once per session (and after each change), log the top N refused geometries of each refusal reason by "
				"render passes, with class, shader property, alpha flags, texture, base type and PS / VS hash "
				"([SceneStatics] refused ...). 0 = off.");
			add_bool(Sg, "RemixApi", "SceneDebugDrawInstance", "Debug: scene statics as DrawInstance", L,
				&r.scene_debug_draw_instance, false,
				"Every visible scene instance is drawn with a DrawInstance per frame (issued at the frame's first render "
				"pass) instead of its retained instance. Tells a retained-replay problem from the rest; expensive.",
				hook_scene_draw_instance);
			add_int(Sg, "RemixApi", "SceneDebugDump", "Debug: scene instances to log", L, &r.scene_debug_dump,
				0, 0, 100000, 1, "Log the first N scene instances created (geometry, reference, mesh, transform).");

			// ---- [FFP] ----
			const char* F = "FFP";
			add_bool(F, "FFP", "Enabled", "Draw routing", L, &f.enabled, true,
				"Route the game's draws: traced draws through the Remix API, the sky and the Pip-Boy through "
				"fixed function. Off = every draw passes through untouched (as during a tracer capture).",
				hook_ffp_enabled);
			add_int(F, "FFP", "AlbedoStage", "Albedo stage", L, &f.albedo_stage, 0, 0, 7, 1,
				"Texture stage holding the diffuse for plain meshes.");
			add_int(F, "FFP", "TerrainAlbedoStage", "Terrain albedo stage", L, &f.terrain_albedo_stage, 0, 0, 7, 1,
				"Diffuse stage for colour + 2-texcoord meshes. No effect on FNV terrain shaders (the PS classifier "
				"picks their diffuse).");
			add_int(F, "FFP", "BIAlbedoStage", "BLENDINDICES albedo stage", L, &f.bi_albedo_stage, 1, 0, 7, 1,
				"Diffuse stage for unskinned meshes with BLENDINDICES (atlas-tile selector decls).");
			add_bool(F, "FFP", "SkipFakeShadows", "Skip fake shadows", L, &f.skip_fake_shadows, true,
				"Drop NOLIGHTING grayscale overlay draws (baked planar shadows); Remix traces real ones.");
			add_bool(F, "FFP", "HideScreenSpaceDraws", "Hide screen-space draws from RT", L,
				&f.hide_screen_space_draws, true,
				"Tag orthographic UI / menu / Pip-Boy quads Hidden: still rasterised, but no longer captured as a "
				"traced quad at the camera's near plane (a floating dark square that shadows the Pip-Boy).");

			// ---- [TexturePacks] ----
			const char* T = "TexturePacks";
			const bool hook = game::tex_pack::hook_installed();
			add_bool(T, "TexturePacks", "HeightMaps", "Height maps (_p)", hook ? L : S, &c.texture_packs.height_maps,
				true, hook
				? "Bind a texture pack's foo_p.dds as the height map (POM; needs rtx.displacement and "
				  "rtx.fnvProtocol.heightDisplaceIn > 0). API statics built with one keep it until rebuilt."
				: "Bind a texture pack's foo_p.dds as the height map (POM). It was off at startup, so its texture "
				  "hook is not installed: switching it on needs a restart.");
			add_int(T, "TexturePacks", "MaxLoadsPerFrame", "Height maps created per frame", L,
				&c.texture_packs.max_loads_per_frame, 4, 1, 256, 1,
				"Objects whose height map is still loading draw without it meanwhile.");
			add_int(T, "TexturePacks", "MaxLoadKBPerFrame", "Height map KB per frame", L,
				&c.texture_packs.max_load_kb_per_frame, 8192, 64, 1024 * 1024, 64,
				"File bytes of height maps created per frame (at least one is always created).");
			add_int(T, "TexturePacks", "EvictFrames", "Height map evict frames", L, &c.texture_packs.evict_frames,
				3600, 60, 10000000, 10, "Frames a loaded height map may go unused before it is released.");
			add_bool(T, "TexturePacks", "LogResolves", "Log every height map found", L, &c.texture_packs.log_resolves,
				false, "Log each resolved height map path, not just the first.");
			add_int(T, "TexturePacks", "StatsIntervalFrames", "Status line interval (frames)", L,
				&c.texture_packs.stats_interval_frames, 1800, 0, 1000000, 10, "0 = off.");

			// ---- [MenuBackground] ----
			const char* M = "MenuBackground";
			add_bool(M, "MenuBackground", "LiveBackground", "Live background", L, &c.menu_bg.live_background, true,
				"Path-trace the world live behind the Pip-Boy (about one gameplay frame of work per Pip-Boy frame) "
				"instead of a frozen picture. Other menus always show a frozen picture of the last path-traced frame.");
			add_bool(M, "MenuBackground", "SkipTint", "Skip menu background tint", L, &c.menu_bg.skip_tint, true,
				"Drop the ISMENUBG sepia / darken pass behind the live Pip-Boy.");
			add_bool(M, "MenuBackground", "KeepDuringClose", "Keep during Pip-Boy close", L, &c.menu_bg.keep_during_close,
				true, "Keep the live background through the Pip-Boy's closing animation.");
			add_enum(M, "MenuBackground", "RedirectMode", "Capture redirect", L, &c.menu_bg.redirect_mode, 1,
				kMenuBgRedirectModes, 3,
				"How the capture pass reaches the path tracer: in place (backbuffer-sized targets only), or bound to "
				"the backbuffer instead.");

			// ---- [PipBoy] ----
			const char* P = "PipBoy";
			add_float(P, "PipBoy", "ScreenEmissive", "Screen emissive intensity", L, &c.pipboy.screen_emissive, 6.0f,
				0.0f, 16.0f, 0.1,
				"How brightly the Pip-Boy screen emits its contents (the interface it shows), in the runtime's "
				"emissive-intensity units, so it stays readable in sunlight or under a lamp. Above 4 needs a runtime "
				"with rtx.worldSpaceUiEmissiveIntensity. 0 = the contents are not drawn.");
			add_float(P, "PipBoy", "ScreenGlassBrightness", "Screen glass brightness", L,
				&c.pipboy.screen_glass_brightness, 0.0f, 0.0f, 1.0f, 0.01,
				"Albedo of the screen glass behind the contents, as a fraction of its texture. Low = dark CRT glass "
				"(glossy, non-metallic) that sun and sky can't wash out behind the text; 1 = the game's grey.");
			add_float(P, "PipBoy", "ScreenGlassGloss", "Screen glass gloss", L,
				&c.pipboy.screen_glass_gloss, 10.0f, 10.0f, 1000.0f, 10.0,
				"How glossy the screen glass is (a Blinn-Phong exponent). Higher = a sharper, mirror-like reflection "
				"of sky and sun (1000 = roughness 0.21); lower = a softer sheen (200 = 0.32, 10 = 0.64).");

			// ---- [Hair] ----
			add_float(P, "Hair", "Brightness", "Hair brightness", L, &c.hair.brightness, 1.0f, 1.0f, 4.0f, 0.05,
				"How light hair, beards and brows are, over the game's own hair colour. 1 = the textures as the shader "
				"colours them. The game's overbright lighting and hair sheen make hair lighter in game, which path "
				"tracing doesn't. Applies to hair drawn from now on.");
			s_settings.back().user_menu = true;

			// ---- [VATS] ----
			const char* V = "VATS";
			const bool vats_hook = game::vats::hook_installed();
			add_bool(V, "VATS", "Enabled", "Line-of-sight limb visibility", vats_hook ? L : S, &c.vats.enabled, true,
				vats_hook
				? "Estimate each limb's visibility for VATS hit chance with line-of-sight rays from the player. Off = "
				  "the engine's occlusion queries, which read 0% with guns under Remix."
				: "Estimate each limb's visibility for VATS hit chance with line-of-sight rays. It was off at startup, "
				  "so its hook is not installed: switching it on needs a restart.");
			add_float(V, "VATS", "SampleRadius", "Sample ring radius", L, &c.vats.sample_radius, 10.0f, 0.0f, 100.0f,
				1.0, "Radius in game units of the ring of points around a limb the rays also aim at (a head is about "
				"10 across). Whole objects use half their bounding radius.");
			add_int(V, "VATS", "SampleCount", "Rays per limb", L, &c.vats.sample_count, 7, 1, 16, 1,
				"One ray to the limb, the rest spread on the ring. Visibility is the share that is not blocked.");

			// ---- [Lights] ----
			const char* Li = "Lights";
			add_bool(Li, "Lights", "Enabled", "Point lights", L, &c.lights.enabled, true,
				"Submit the ShadowSceneNode point lights as D3D9 lights for Remix. Off disables them at once.",
				hook_lights);
			add_int(Li, "Lights", "IntensityPercent", "Intensity %", L, &c.lights.intensity_percent, 100, 0, 10000, 1,
				"Multiplier on the lights' diffuse colour.", hook_lights);
			add_enum(Li, "Lights", "RangeMode", "Range", L, &c.lights.range_mode, 0, kLightRangeModes, 3,
				"How far each light reaches: its radius, the distance at which its attenuation reaches 1/255, or no limit.",
				hook_lights);
			add_int(Li, "Lights", "EffectLightForward", "Pip-Boy light forward", L, &c.lights.effect_forward, 1, 0, 1000, 1,
				"Units ahead of the eye the Pip-Boy light is placed in first person (0 = where the engine puts it).", hook_lights);
			add_int(Li, "Lights", "EffectLightThirdPersonForward", "Pip-Boy light forward (3rd person)", L,
				&c.lights.effect_third_person_forward, 20, 0, 1000, 1,
				"Third person: units ahead of the engine's light position (on the player's body) along the view.", hook_lights);
			add_int(Li, "Lights", "FollowerIntensityPercent", "Pip-Boy light intensity %", L, &c.lights.follower_intensity_percent,
				100, 0, 100000, 10, "Extra multiplier on the Pip-Boy light.", hook_lights);

			// ---- [SunCycle] / [MoonCycle] ----
			add_bool("SunCycle", "SunCycle", "Enabled", "Drive the sun from the game", L, &c.sun_cycle.enabled, true,
				"Push the engine's sun direction to rtx.atmosphere.sun* every frame. Off = Remix defaults.");
			add_bool("MoonCycle", "MoonCycle", "Enabled", "Drive the moon from the game", L, &c.moon_cycle.enabled, true,
				"Push the engine's moon direction to rtx.atmosphere.moon0.* every frame. Off = Remix defaults.");

			// ---- [Weather] ----
			const char* Wx = "Weather";
			add_bool(Wx, "Weather", "Enabled", "Drive the sky's weather from the game", L, &c.weather.enabled, true,
				"Report the game's current and outgoing weather and its transition to rtx.weather.gameWeather*, so "
				"the NUMOS preset bound to the weather's class (or form ID) shows and blends with the game. Off = "
				"no game weather: the dev menu's preset or the settings alone.");
			add_bool(Wx, "Weather", "BindPresets", "Bind classes to presets", S, &c.weather.bind_presets, true,
				"At startup, write each class id (fnv.clear, fnv.rain, ...) into the weather.bindings of the preset "
				"[Weather] maps it to. The class -> preset table is edited in remix-comp.ini.");
			add_float(Wx, "Weather", "FogFarMax", "Fog: day fog far at most", L, &c.weather.fog_far_max, 20000.0f,
				0.0f, 1000000.0f, 500.0,
				"A weather whose day fog far distance (game units) is at most this is Fog, or Dust when windy.");
			add_float(Wx, "Weather", "HazeFogFarMax", "Haze: day fog far at most", L, &c.weather.haze_fog_far_max,
				60000.0f, 0.0f, 1000000.0f, 500.0,
				"Up to this day fog far distance a weather is Haze (Overcast when flagged cloudy, Dust when windy).");
			add_int(Wx, "Weather", "DustWindMin", "Dust: wind at least", L, &c.weather.dust_wind_min, 100, 0, 256, 1,
				"Wind speed (the record's 0..255 byte) from which a foggy or hazy weather is Dust. 256 = never.");
			add_int(Wx, "Weather", "StormThunderMax", "Storm: thunder frequency at most", L,
				&c.weather.storm_thunder_max, 200, -1, 255, 1,
				"A rainy weather whose thunder frequency byte is at most this is Storm, else Rain. Records without "
				"thunder carry 255; -1 = never Storm.");

			// ---- [Skinning] ----
			add_bool("Skinning", "Skinning", "Enabled", "Skinning", S, &c.skinning.enabled, true,
				"The engine hooks that capture each skinned object's bone palette (installed at startup). The "
				"Remix-API skinned path needs them: off, skinned actors are not drawn.");

			// ---- [Diagnostics] / [Tracer] ----
			add_bool("Diagnostics", "Diagnostics", "Enabled", "Diagnostic log", S, &c.diagnostics.enabled, false,
				"Register the diagnostics module (ffp_proxy.log).");
			add_int("Diagnostics", "Diagnostics", "DelayMs", "Delay (ms)", S, &c.diagnostics.delay_ms, 300000, 0,
				100000000, 1000, "Time before the diagnostic frames are logged.");
			add_int("Diagnostics", "Diagnostics", "LogFrames", "Frames logged", S, &c.diagnostics.log_frames, 3, 0,
				1000, 1, "Frames the diagnostics module logs.");
			add_int("Tracer", "Tracer", "BacktraceDepth", "Backtrace depth", S, &c.tracer.backtrace_depth, 8, 0, 62, 1,
				"Call-stack frames recorded per traced call. 0 = none.");

			// ---- [SessionReport] ----
			const char* Sr = "SessionReport";
			add_action(Sr, kActionSaveReport, "Save Report Now",
				"Zip this session so far (the logs, session.json, load order, config snapshot, crash logs, hang dumps "
				"and screenshots) into rtx-remix\\logs\\archive\\NewVegasRTX-<version>_<start>-snapshot-<time>.zip, "
				"without leaving the game. The zip's path shows below and in remix-comp.log.");
			add_info(Sr, kInfoSavedReport, "Saved report", s_saved_labels,
				"The zip Save Report Now wrote last, where Explorer finds it.");
			add_action(Sr, kActionShowReport, "Show Saved Report",
				"Open Explorer with the zip Save Report Now wrote last selected (the archive folder before that).");
		}

		std::string build_tooltip(const Setting& s)
		{
			std::string t = s.tip ? s.tip : "";
			const char* note = nullptr;
			if (s.apply == Apply::Rebuild)
				note = "Applies to API meshes built after the change. Press RemixApi > Rebuild API meshes and textures "
					"to apply it to everything already built.";
			else if (s.apply == Apply::Restart)
				note = "Read once at startup: the change is saved to remix-comp.ini and applies after a restart.";
			if (note)
			{
				if (!t.empty()) t += "\n\n";
				t += note;
			}
			if (s.sec)
			{
				if (!t.empty()) t += "\n";
				t += std::format("ini: [{}] {}", s.sec, s.key);
			}
			if (s.kind == Kind::Info && s.enum_count)
			{
				if (!t.empty()) t += "\n\n";
				t += s.enum_labels[0];
			}
			return t;
		}

		void apply_change(IDirect3DDevice9* dev, Setting& s, double v)
		{
			const double old = s.last;
			s.last = v;
			if (s.kind == Kind::Action)
			{
				if (std::strcmp(s.key, kActionRebuild) == 0) game::api_submit::rebuild_everything();
				else if (std::strcmp(s.key, kActionSaveReport) == 0)
				{
					if (!game::session_report::save_now())
						shared::common::log("HostSettings", "Save Report Now: a report is being saved, or another game "
							"instance owns the session files", shared::common::LOG_TYPE::LOG_TYPE_WARN);
				}
				else if (std::strcmp(s.key, kActionShowReport) == 0) game::session_report::show_saved();
				return;
			}
			if (s.kind == Kind::Info) return;
			if (s.apply != Apply::Restart)
			{
				if (s.hook) s.hook(dev, s, v);
				else write_field(s, v);
			}
			save_to_ini(s, s.last);
			shared::common::log_event("HostSettings", std::format("[{}] {}: {} -> {}{}", s.sec, s.key, format_ini(s, old),
				format_ini(s, s.last),
				s.apply == Apply::Restart ? " (saved; applies after restart)" :
				s.apply == Apply::Rebuild ? " (new API meshes; Rebuild API meshes and textures for built ones)" : ""),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}

		// Live values changed by the wrapper itself (the F4 overlay's FFP tab):
		// the menu follows; the ini is left alone, as those
		// changes never touched it.
		void follow_local_changes()
		{
			for (auto& s : s_settings)
			{
				if (!s.handle || !s.field || s.apply != Apply::Live) continue;
				const double v = read_field(s);
				if (v != s.last)
				{
					s.last = v;
					s_set(s.handle, v);
				}
			}
		}

		bool resolve()
		{
			HMODULE m = shared::globals::d3d9_chain_module;
			if (!m) return false;
			s_register = reinterpret_cast<PFN_remixapi_RegisterHostSetting>(GetProcAddress(m, "remixapi_RegisterHostSetting"));
			s_get_generation = reinterpret_cast<PFN_remixapi_GetHostSettingsGeneration>(
				GetProcAddress(m, "remixapi_GetHostSettingsGeneration"));
			s_get = reinterpret_cast<PFN_remixapi_GetHostSettingValue>(GetProcAddress(m, "remixapi_GetHostSettingValue"));
			s_set = reinterpret_cast<PFN_remixapi_SetHostSettingValue>(GetProcAddress(m, "remixapi_SetHostSettingValue"));
			s_clear = reinterpret_cast<PFN_remixapi_ClearHostSettings>(GetProcAddress(m, "remixapi_ClearHostSettings"));
			s_poll = reinterpret_cast<PFN_remixapi_PollHostSettings>(GetProcAddress(m, "remixapi_PollHostSettings"));
			if (s_register && s_get_generation && s_get && s_set && s_clear) return true;
			s_register = nullptr; s_get_generation = nullptr; s_get = nullptr; s_set = nullptr; s_clear = nullptr;
			s_poll = nullptr;
			return false;
		}

		// Values of every registered setting: one poll round trip (forced with
		// a generation it can't have), or one Get each without the poll export.
		bool read_all(uint64_t last_generation, uint64_t& generation)
		{
			if (s_poll)
			{
				if (s_poll(last_generation, &generation, s_values.data(), s_handles.data(),
					static_cast<uint32_t>(s_handles.size())) != REMIXAPI_ERROR_CODE_SUCCESS)
					return false;
				return generation != last_generation;
			}
			generation = s_get_generation();
			if (generation == last_generation) return false;
			for (size_t i = 0; i < s_handles.size(); i++)
			{
				double v = 0.0;
				s_values[i] = (s_handles[i] && s_get(s_handles[i], &v) == REMIXAPI_ERROR_CODE_SUCCESS)
					? v : std::numeric_limits<double>::quiet_NaN();
			}
			return true;
		}

		std::string setting_key(const Setting& s)
		{
			return s.sec ? std::format("FNV.{}.{}", s.sec, s.key) : std::string(s.key);
		}

		// Registers `s`, or replaces its label, tooltip and value when it is
		// registered already (same key, same handle).
		remixapi_ErrorCode register_setting(Setting& s)
		{
			const std::string tip = build_tooltip(s);
			const std::string key = setting_key(s);
			remixapi_HostSettingInfo info = {};
			info.structSize = sizeof(info);
			info.key = key.c_str();
			info.label = s.label;
			info.section = s.ui;
			info.tooltip = tip.c_str();
			info.type = s.kind == Kind::Bool ? REMIXAPI_HOST_SETTING_TYPE_BOOL
				: s.kind == Kind::Int ? REMIXAPI_HOST_SETTING_TYPE_INT
				: s.kind == Kind::Float ? REMIXAPI_HOST_SETTING_TYPE_FLOAT
				: s.kind == Kind::Enum || s.kind == Kind::Info ? REMIXAPI_HOST_SETTING_TYPE_ENUM
				: REMIXAPI_HOST_SETTING_TYPE_ACTION;
			info.flags = (s.apply == Apply::Restart ? REMIXAPI_HOST_SETTING_FLAG_REQUIRES_RESTART : 0u) |
				(s.kind == Kind::Info ? REMIXAPI_HOST_SETTING_FLAG_READ_ONLY : 0u) |
				(s.user_menu ? REMIXAPI_HOST_SETTING_FLAG_USER_MENU : 0u);
			info.minValue = s.lo;
			info.maxValue = s.hi;
			info.step = s.step;
			info.defaultValue = s.def;
			info.currentValue = s.field ? read_field(s) : 0.0;
			info.enumLabels = s.enum_labels;
			info.enumCount = s.enum_count;
			s.last = info.currentValue;
			return s_register(&info, &s.handle);
		}

		// The Saved report line follows what Save Report Now last did.
		void refresh_saved_report()
		{
			const game::session_report::Saved saved = game::session_report::saved();
			if (saved.generation == s_saved_generation) return;
			s_saved_generation = saved.generation;
			s_saved_text = saved.text;
			s_saved_labels[0] = s_saved_text.c_str();
			for (auto& s : s_settings)
				if (s.kind == Kind::Info && s.handle) register_setting(s);
		}

		void register_all()
		{
			if (!resolve())
			{
				shared::common::log("HostSettings", "the Remix bridge has no host-settings exports (older bridge?) -- "
					"remix-comp.ini settings are not in the Remix menu", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}

			build_table();
			uint32_t ok = 0, live = 0, rebuild = 0, restart = 0, actions = 0;
			for (auto& s : s_settings)
			{
				const std::string key = setting_key(s);
				const auto rc = register_setting(s);
				if (rc != REMIXAPI_ERROR_CODE_SUCCESS || !s.handle)
				{
					s.handle = 0;
					if (ok == 0)
					{
						// The first one: the runtime behind the bridge lacks the
						// exports (the bridge asked it once). Nothing else is sent.
						shared::common::log("HostSettings", std::format(
							"the Remix runtime rejected host settings (error {}; runtime without the exports?) -- "
							"remix-comp.ini settings are not in the Remix menu", static_cast<int>(rc)),
							shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
						s_settings.clear();
						return;
					}
					shared::common::log("HostSettings", std::format("register FAILED for {} (error {})", key, static_cast<int>(rc)),
						shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
					continue;
				}
				ok++;
				if (s.kind == Kind::Action) actions++;
				else if (s.kind == Kind::Info) continue;
				else if (s.apply == Apply::Live) live++;
				else if (s.apply == Apply::Rebuild) rebuild++;
				else restart++;
			}

			s_handles.resize(s_settings.size());
			s_values.assign(s_settings.size(), std::numeric_limits<double>::quiet_NaN());
			for (size_t i = 0; i < s_settings.size(); i++) s_handles[i] = s_settings[i].handle;

			// The menu clamps values outside its range: track what it shows, so
			// an unrelated change never re-applies the clamped copy. Unchanged
			// generations return no values, so ask with one it can't have.
			uint64_t generation = 0;
			if (read_all(~0ull, generation))
			{
				for (size_t i = 0; i < s_settings.size(); i++)
				{
					Setting& s = s_settings[i];
					const double shown = s_values[i];
					if (!s.handle || !s.field || std::isnan(shown) || shown == s.last) continue;
					shared::common::log("HostSettings", std::format(
						"[{}] {}={} is outside the menu range; the menu shows {} (ini value kept until changed)",
						s.sec, s.key, format_ini(s, s.last), format_ini(s, shown)),
						shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
					s.last = shown;
				}
			}
			s_generation = generation;
			s_ready = true;
			shared::common::log("HostSettings", std::format(
				"{} settings in the Remix menu (Host Settings tab): {} live, {} on API rebuild, {} after restart, "
				"{} action(s); polled every {} frames{}; ini={}",
				ok, live, rebuild, restart, actions, kPollFrames, s_poll ? "" : " (no batched poll: one Get per setting)",
				shared::common::config::get().ini_path()), shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}

		void poll(IDirect3DDevice9* dev)
		{
			uint64_t generation = s_generation;
			if (read_all(s_generation, generation))
			{
				s_generation = generation;
				for (size_t i = 0; i < s_settings.size(); i++)
				{
					Setting& s = s_settings[i];
					const double v = s_values[i];
					if (!s.handle || std::isnan(v) || v == s.last) continue;
					apply_change(dev, s, v);
				}
			}
			follow_local_changes();
		}
	}

	void on_present(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("host_settings::on_present");
		if (!s_started)
		{
			if (++s_frames < kWarmupFrames) return;
			s_started = true;
			register_all();
			s_frames = 0;
			return;
		}
		if (!s_ready) return;

		if (++s_info_frames >= kInfoFrames)
		{
			s_info_frames = 0;
			refresh_saved_report();
		}

		// The poll is a synchronous bridge round trip: it waits for the server
		// to work through every draw queued before it, a hitch on each poll
		// frame (p95 frame time in the perf log). Values only change while the
		// Remix menu is open, so poll only then, plus once on the frame it
		// closes. The bridge client answers GetUIState from its own copy of
		// the menu state, with no round trip.
		auto& bridge = shared::common::remix_api::get().m_bridge;
		const bool menu_open = bridge.GetUIState && bridge.GetUIState() != REMIXAPI_UI_STATE_NONE;
		static bool s_menu_was_open = false;
		const bool closed_now = s_menu_was_open && !menu_open;
		s_menu_was_open = menu_open;

		if (++s_frames < kPollFrames && !closed_now) return;
		s_frames = 0;
		if (menu_open || closed_now) poll(dev);
		else follow_local_changes();
	}
}
