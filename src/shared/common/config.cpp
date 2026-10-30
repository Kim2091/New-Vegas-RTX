#include "std_include.hpp"
#include "config.hpp"

namespace shared::common
{
	config& config::get()
	{
		static config instance;
		return instance;
	}

	void config::load(const std::string& path)
	{
		ini_path_ = path;
		loaded_ = true;
		parse_all();
	}

	int config::get_int(const char* section, const char* key, int default_val) const
	{
		if (!loaded_) return default_val;
		return GetPrivateProfileIntA(section, key, default_val, ini_path_.c_str());
	}

	std::string config::get_string(const char* section, const char* key, const char* default_val) const
	{
		if (!loaded_) return default_val;
		char buf[512];
		GetPrivateProfileStringA(section, key, default_val, buf, sizeof(buf), ini_path_.c_str());
		return buf;
	}

	float config::get_float(const char* section, const char* key, float default_val) const
	{
		auto str = get_string(section, key, "");
		if (str.empty()) return default_val;
		try { return std::stof(str); }
		catch (...) { return default_val; }
	}

	bool config::get_bool(const char* section, const char* key, bool default_val) const
	{
		return get_int(section, key, default_val ? 1 : 0) != 0;
	}

	std::vector<uint32_t> config::get_hash_list(const char* section, const char* key) const
	{
		std::vector<uint32_t> out;
		const std::string str = get_string(section, key, "");
		size_t i = 0;
		while (i < str.size())
		{
			while (i < str.size() && (str[i] == ',' || str[i] == ' ' || str[i] == '\t')) i++;
			size_t j = i;
			while (j < str.size() && str[j] != ',' && str[j] != ' ' && str[j] != '\t') j++;
			if (j > i)
			{
				try { out.push_back(static_cast<uint32_t>(std::stoul(str.substr(i, j - i), nullptr, 16))); }
				catch (...) {}
			}
			i = j;
		}
		return out;
	}

	void config::parse_all()
	{
		// [Logging] first, so the lines below are written with it.
		logging.verbose = get_bool("Logging", "Verbose", false);
		logging.console = get_bool("Logging", "Console", false);
		g_log_verbose = logging.verbose;

		// [SessionReport]
		{
			auto& s = session_report;
			s.archive = get_bool("SessionReport", "Archive", true);
			s.reporter = get_bool("SessionReport", "Reporter", true);
			s.archive_keep = std::clamp(get_int("SessionReport", "ArchiveKeep", 20), 1, 1000);
			s.scrub_user_name = get_bool("SessionReport", "ScrubUserName", true);
			s.snapshots = get_bool("SessionReport", "Snapshots", true);
			s.hang_seconds = get_int("SessionReport", "HangSeconds", 15);
			if (s.hang_seconds != 0) s.hang_seconds = std::clamp(s.hang_seconds, 5, 600);
			s.mark_key = get_string("SessionReport", "MarkKey", "F8");
			s.crash_log_folder = get_string("SessionReport", "CrashLogFolder", "Crash Logs");
			s.crash_log = get_bool("SessionReport", "CrashLog", true);
		}

		// [Remix]
		remix.enabled = get_bool("Remix", "Enabled", true);
		remix.dll_name = get_string("Remix", "DLLName", "d3d9_remix.dll");

		// [Chain]
		chain.preload = get_string("Chain", "Preload", "");
		chain.postload = get_string("Chain", "Postload", "");

		// [FFP]
		ffp.enabled = get_bool("FFP", "Enabled", true);
		ffp.albedo_stage = get_int("FFP", "AlbedoStage", 0);
		if (ffp.albedo_stage < 0 || ffp.albedo_stage > 7) ffp.albedo_stage = 0;
		ffp.terrain_albedo_stage = get_int("FFP", "TerrainAlbedoStage", 0);
		if (ffp.terrain_albedo_stage < 0 || ffp.terrain_albedo_stage > 7) ffp.terrain_albedo_stage = 1;
		ffp.bi_albedo_stage = get_int("FFP", "BIAlbedoStage", 1);
		if (ffp.bi_albedo_stage < 0 || ffp.bi_albedo_stage > 7) ffp.bi_albedo_stage = 1;
		ffp.skip_fake_shadows = get_bool("FFP", "SkipFakeShadows", true);
		ffp.hide_screen_space_draws = get_bool("FFP", "HideScreenSpaceDraws", true);

		// [RemixApi]
		{
			auto& r = remix_api;
			r.terrain_weight_res = std::clamp(get_int("RemixApi", "TerrainWeightRes", 65), 5, 257);
			r.terrain_roughness = std::clamp(get_float("RemixApi", "TerrainRoughness", 0.85f), 0.0f, 1.0f);
			r.terrain_normal_gloss = get_bool("RemixApi", "TerrainNormalGloss", true);
			r.terrain_height_maps = get_bool("RemixApi", "TerrainHeightMaps", true);
			r.far_lod_roughness = std::clamp(get_float("RemixApi", "FarLodRoughness", 0.9f), 0.0f, 1.0f);
			r.far_lod_detail = get_bool("RemixApi", "FarLodDetail", true);
			r.far_lod_rebuild_frames = std::clamp(get_int("RemixApi", "FarLodRebuildFrames", 30), 0, 100000);
			r.max_builds_per_frame = std::clamp(get_int("RemixApi", "MaxBuildsPerFrame", 16), 1, 100000);
			r.evict_frames = std::clamp(get_int("RemixApi", "EvictFrames", 900), 2, 1000000);
			r.texture_evict_frames = std::clamp(get_int("RemixApi", "TextureEvictFrames", 1800), 2, 1000000);
			r.import_d3d9_textures = get_bool("RemixApi", "ImportD3D9Textures", true);
			r.keep_default_pool_textures = get_bool("RemixApi", "KeepDefaultPoolTextures", true);
			r.water_ior = std::clamp(get_float("RemixApi", "WaterIor", 1.33f), 1.0f, 3.0f);
			r.water_measurement_distance = std::clamp(get_float("RemixApi", "WaterMeasurementDistance", 256.0f), 1.0f, 1.0e6f);
			r.water_clarity = std::clamp(get_float("RemixApi", "WaterClarity", 0.8f), 0.05f, 1.0f);
			r.water_tile_size = std::max(get_float("RemixApi", "WaterTileSize", 1120.0f), 0.0f);
			r.water_normal_res = std::clamp(get_int("RemixApi", "WaterNormalRes", 1024), 64, 1024);
			r.water_normal_strength = std::clamp(get_float("RemixApi", "WaterNormalStrength", 1.0f), 0.0f, 8.0f);
			r.water_runtime_options = get_bool("RemixApi", "WaterRuntimeOptions", true);
			r.water_flow_speed = std::clamp(get_float("RemixApi", "WaterFlowSpeed", 21.0f), 0.0f, 10000.0f);
			r.water_detail_scale = std::clamp(get_float("RemixApi", "WaterDetailScale", 0.31f), 1.0f / 64.0f, 64.0f);
			r.water_detail_flow_speed = std::clamp(get_float("RemixApi", "WaterDetailFlowSpeed", 35.0f), 0.0f, 10000.0f);
			r.water_detail_lod_bias = std::clamp(get_float("RemixApi", "WaterDetailLodBias", 0.0f), 0.0f, 16.0f);
			r.water_lod_near_blend = std::clamp(get_float("RemixApi", "WaterLodNearBlend", 1.0f), 0.0f, 1.0f);
			r.underwater = get_bool("RemixApi", "Underwater", true);
			r.stable_origin = get_bool("RemixApi", "StableOrigin", true);
			r.stable_origin_grid = std::clamp(get_float("RemixApi", "StableOriginGrid", 4096.0f), 64.0f, 65536.0f);
			// At least one grid cell, or the snapped origin could already be out
			// of range and re-snap every frame.
			r.stable_origin_radius = std::clamp(get_float("RemixApi", "StableOriginRadius", 8192.0f),
				r.stable_origin_grid, 1.0e6f);
			r.publish_world_origin = get_bool("RemixApi", "PublishWorldOrigin", true);
			r.world_unit_scale = std::clamp(get_float("RemixApi", "WorldUnitScale", 1.428571f), 0.001f, 1000.0f);
			r.fallback_camera = get_bool("RemixApi", "FallbackCamera", true);
			r.retained_instances = get_bool("RemixApi", "RetainedInstances", true);
			r.retained_replay_at_frame_start = get_bool("RemixApi", "RetainedReplayAtFrameStart", true);
			r.retained_keep_culled = get_bool("RemixApi", "RetainedKeepCulled", true);
			r.retained_keep_culled_frames = std::clamp(get_int("RemixApi", "RetainedKeepCulledFrames", 36000), 2, 100000000);
			r.retained_keep_culled_cap = std::clamp(get_int("RemixApi", "RetainedKeepCulledCap", 8192), 16, 1000000);
			r.retained_lod_stale_frames = std::clamp(get_int("RemixApi", "RetainedLodStaleFrames", 10), 1, 100000);
			r.retained_lod_rule = std::clamp(get_int("RemixApi", "RetainedLodRule", 1), 0, 1);
			r.statics_ps_allow = get_hash_list("RemixApi", "StaticsPsAllow");
			r.statics_ps_deny = get_hash_list("RemixApi", "StaticsPsDeny");
			r.statics_debug_plain_material = get_bool("RemixApi", "StaticsDebugPlainMaterial", false);
			r.scene_walk_budget_us = std::clamp(get_int("RemixApi", "SceneWalkBudgetUs", 400), 50, 100000);
			r.scene_graph_statics = get_bool("RemixApi", "SceneGraphStatics", true);
			r.scene_base_types = get_hash_list("RemixApi", "SceneBaseTypes");
			if (r.scene_base_types.empty())
				r.scene_base_types = { 0x20, 0x21, 0x15, 0x16, 0x17, 0x1B, 0x1C, 0x1E, 0x22, 0x26, 0x27,
					0x18, 0x19, 0x1A, 0x1D, 0x1F, 0x28, 0x29, 0x2E, 0x2F, 0x31 };
			r.scene_claim_alpha_blend = std::clamp(get_int("RemixApi", "SceneClaimAlphaBlend", 2), 0, 2);
			r.scene_decal_category = get_bool("RemixApi", "SceneDecalCategory", true);
			r.scene_glow_grey_tint = get_bool("RemixApi", "SceneGlowGreyTint", true);
			r.scene_land_shape_statics = get_bool("RemixApi", "SceneLandShapeStatics", true);
			r.scene_streamed_relaxed = get_bool("RemixApi", "SceneStreamedRelaxed", true);
			r.scene_debug_refusal_report = std::clamp(get_int("RemixApi", "SceneDebugRefusalReport", 5), 0, 1000);
			r.scene_upload_kib_per_frame = std::clamp(get_int("RemixApi", "SceneUploadKiBPerFrame", 4096), 64, 1024 * 1024);
			r.scene_debug_draw_instance = get_bool("RemixApi", "SceneDebugDrawInstance", false);
			r.scene_debug_dump = std::clamp(get_int("RemixApi", "SceneDebugDump", 0), 0, 100000);
			// Empty or absent = off.
			if (!get_string("RemixApi", "SceneDebugTraceBaseTypes", "").empty())
				r.scene_debug_trace_base_types = get_hash_list("RemixApi", "SceneDebugTraceBaseTypes");
			r.api_glow_tint = get_bool("RemixApi", "ApiGlowTint", true);
			r.shader_bake_hair = get_bool("RemixApi", "ShaderBakeHair", true);
			r.unlit_emissive_intensity = std::clamp(get_float("RemixApi", "UnlitEmissiveIntensity", 1.0f), 0.0f, 100.0f);
			r.unlit_debug_log = get_bool("RemixApi", "UnlitDebugLog", false);
			r.view_model_camera = get_bool("RemixApi", "ViewModelCamera", true);
			r.stats_interval_frames = std::max(get_int("RemixApi", "StatsIntervalFrames", 600), 0);
		}

		// [TexturePacks]
		{
			auto& t = texture_packs;
			t.height_maps = get_bool("TexturePacks", "HeightMaps", true);
			t.height_suffixes = get_string("TexturePacks", "HeightSuffixes", "_p");
			t.max_loads_per_frame = std::clamp(get_int("TexturePacks", "MaxLoadsPerFrame", 4), 1, 256);
			t.max_load_kb_per_frame = std::clamp(get_int("TexturePacks", "MaxLoadKBPerFrame", 8192), 64, 1024 * 1024);
			t.evict_frames = std::clamp(get_int("TexturePacks", "EvictFrames", 3600), 60, 10000000);
			t.extra_archives = get_string("TexturePacks", "ExtraArchives", "");
			t.log_resolves = get_bool("TexturePacks", "LogResolves", false);
			t.stats_interval_frames = std::max(get_int("TexturePacks", "StatsIntervalFrames", 1800), 0);
		}

		// [Skinning]
		skinning.enabled = get_bool("Skinning", "Enabled", true);

		// [Lights]
		lights.enabled = get_bool("Lights", "Enabled", true);
		lights.intensity_percent = get_int("Lights", "IntensityPercent", 100);
		lights.range_mode = get_int("Lights", "RangeMode", 0);
		lights.effect_forward = get_int("Lights", "EffectLightForward", 1);
		lights.effect_third_person_forward = get_int("Lights", "EffectLightThirdPersonForward", 20);
		lights.follower_intensity_percent = get_int("Lights", "FollowerIntensityPercent", 100);

		// [Diagnostics]
		diagnostics.enabled = get_bool("Diagnostics", "Enabled", false);
		diagnostics.delay_ms = get_int("Diagnostics", "DelayMs", 300000);
		diagnostics.log_frames = get_int("Diagnostics", "LogFrames", 3);
		diagnostics.harvest_ps = get_bool("Diagnostics", "HarvestPS", false);
		diagnostics.screen_space_log = (std::max)(0, get_int("Diagnostics", "ScreenSpaceLog", 0));

		// [Tracer]
		tracer.backtrace_depth = get_int("Tracer", "BacktraceDepth", 8);
		tracer.output_dir = get_string("Tracer", "OutputDir", "captures");

		// [SunCycle] / [MoonCycle]: engine sky state driven into the Remix atmosphere.
		sun_cycle.enabled = get_bool("SunCycle", "Enabled", true);
		moon_cycle.enabled = get_bool("MoonCycle", "Enabled", true);

		// [MenuBackground]
		menu_bg.live_background = get_bool("MenuBackground", "LiveBackground", true);
		menu_bg.skip_tint = get_bool("MenuBackground", "SkipTint", true);
		menu_bg.keep_during_close = get_bool("MenuBackground", "KeepDuringClose", true);
		menu_bg.redirect_mode = std::clamp(get_int("MenuBackground", "RedirectMode", 1), 0, 2);

		// [Weather]
		weather.enabled = get_bool("Weather", "Enabled", true);
		weather.bind_presets = get_bool("Weather", "BindPresets", true);
		weather.preset_directory = get_string("Weather", "PresetDirectory", "rtx-remix/weather");
		weather.fog_far_max = (std::max)(0.0f, get_float("Weather", "FogFarMax", 20000.0f));
		weather.haze_fog_far_max = (std::max)(0.0f, get_float("Weather", "HazeFogFarMax", 60000.0f));
		weather.dust_wind_min = std::clamp(get_int("Weather", "DustWindMin", 100), 0, 256);
		weather.storm_thunder_max = std::clamp(get_int("Weather", "StormThunderMax", 200), -1, 255);
		for (int i = 0; i < WEATHER_CLASS_COUNT; i++)
		{
			std::string v = get_string("Weather", WEATHER_CLASS_NAMES[i], WEATHER_CLASS_DEFAULT_PRESETS[i]);
			const size_t a = v.find_first_not_of(" 	");
			const size_t b = v.find_last_not_of(" 	");
			weather.preset[i] = a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
		}

		// [PipBoy]
		pipboy.screen_emissive = std::clamp(get_float("PipBoy", "ScreenEmissive", 6.0f), 0.0f, 16.0f);
		pipboy.screen_glass_brightness = std::clamp(get_float("PipBoy", "ScreenGlassBrightness", 0.0f), 0.0f, 1.0f);
		pipboy.screen_glass_gloss = std::clamp(get_float("PipBoy", "ScreenGlassGloss", 10.0f), 10.0f, 1000.0f);

		// [Hair]
		hair.brightness = std::clamp(get_float("Hair", "Brightness", 1.0f), 1.0f, 4.0f);

		// [VATS]
		vats.enabled = get_bool("VATS", "Enabled", true);
		vats.sample_radius = std::clamp(get_float("VATS", "SampleRadius", 10.0f), 0.0f, 100.0f);
		vats.sample_count = std::clamp(get_int("VATS", "SampleCount", 7), 1, 16);

		// [SaveScreenshots]
		save_screenshots.enabled = get_bool("SaveScreenshots", "Enabled", true);
		save_screenshots.refresh_ms = std::clamp(get_int("SaveScreenshots", "RefreshMs", 1500), 250, 60000);

		log("Config", std::format("Loaded from: {}", ini_path_));
		log("Config", std::format("FFP={} AlbedoStage={} TerrainAlbedoStage={} BIAlbedoStage={} "
			"(every traced draw goes through the Remix API; FFP is for the Pip-Boy and the runtime's D3D9-only lists)",
			ffp.enabled ? 1 : 0, ffp.albedo_stage, ffp.terrain_albedo_stage, ffp.bi_albedo_stage));
		log("Config", std::format("RemixApi: WeightRes={} MaxBuilds/frame={} Evict={}f TexEvict={}f ImportD3D9Textures={} "
			"KeepDefaultPoolTextures={}",
			remix_api.terrain_weight_res, remix_api.max_builds_per_frame,
			remix_api.evict_frames, remix_api.texture_evict_frames, remix_api.import_d3d9_textures ? 1 : 0,
			remix_api.keep_default_pool_textures ? 1 : 0));
		log("Config", std::format("RemixApi: WorldUnitScale={:.6f} StableOrigin={} Grid={:.0f} Radius={:.0f} PublishWorldOrigin={} "
			"FallbackCamera={} RetainedInstances={} ReplayAtFrameStart={} KeepCulled={} (ttl {}f, cap {}, LOD rule {}, LOD stale {}f)",
			remix_api.world_unit_scale, remix_api.stable_origin ? 1 : 0, remix_api.stable_origin_grid, remix_api.stable_origin_radius,
			remix_api.publish_world_origin ? 1 : 0, remix_api.fallback_camera ? 1 : 0,
			remix_api.retained_instances ? 1 : 0, remix_api.retained_replay_at_frame_start ? 1 : 0,
			remix_api.retained_keep_culled ? 1 : 0, remix_api.retained_keep_culled_frames,
			remix_api.retained_keep_culled_cap, remix_api.retained_lod_rule == 1 ? "engine" : "view",
			remix_api.retained_lod_stale_frames));
		log("Config", std::format("RemixApi: statics PsAllow={} PsDeny={} StaticsDebugPlainMaterial={}",
			remix_api.statics_ps_allow.size(), remix_api.statics_ps_deny.size(),
			remix_api.statics_debug_plain_material ? 1 : 0));
		if (!skinning.enabled)
			log("Config", "[Skinning] Enabled=0: no bone capture -- skinned actors (NPCs, creatures, the first-person "
				"arms) are NOT drawn (every traced draw goes through the Remix API, which needs the bones)",
				LOG_TYPE::LOG_TYPE_WARN);
		if (remix_api.scene_graph_statics)
		{
			std::string types, trace;
			for (const uint32_t t : remix_api.scene_base_types) types += std::format("{}{:02X}", types.empty() ? "" : ",", t);
			for (const uint32_t t : remix_api.scene_debug_trace_base_types) trace += std::format("{}{:02X}", trace.empty() ? "" : ",", t);
			log("Config", std::format("RemixApi: SceneGraphStatics=1 BaseTypes={} UploadKiB/frame={} "
				"DrawInstance={} Dump={} ClaimAlphaBlend={} DecalCategory={} "
				"GlowGreyTint={} LandShapeStatics={} StreamedRelaxed={} RefusalReport={} TraceBaseTypes={}{}", types,
				remix_api.scene_upload_kib_per_frame,
				remix_api.scene_debug_draw_instance ? 1 : 0,
				remix_api.scene_debug_dump, remix_api.scene_claim_alpha_blend, remix_api.scene_decal_category ? 1 : 0,
				remix_api.scene_glow_grey_tint ? 1 : 0,
				remix_api.scene_land_shape_statics ? 1 : 0, remix_api.scene_streamed_relaxed ? 1 : 0,
				remix_api.scene_debug_refusal_report, trace.empty() ? std::string("-") : trace,
				(remix_api.retained_instances && remix_api.stable_origin) ? "" : " -- INACTIVE: needs RetainedInstances=1 and StableOrigin=1"),
				(remix_api.retained_instances && remix_api.stable_origin) ? LOG_TYPE::LOG_TYPE_DEFAULT : LOG_TYPE::LOG_TYPE_WARN);
		}
		log("Config", std::format("RemixApi: ApiGlowTint={} ShaderBakeHair={} UnlitEmissiveIntensity={:.2f} UnlitDebugLog={} "
			"ViewModelCamera={}", remix_api.api_glow_tint ? 1 : 0, remix_api.shader_bake_hair ? 1 : 0,
			remix_api.unlit_emissive_intensity, remix_api.unlit_debug_log ? 1 : 0, remix_api.view_model_camera ? 1 : 0));
		log("Config", std::format("TexturePacks: HeightMaps={} Suffixes='{}' MaxLoads/frame={} MaxLoadKB/frame={} Evict={}f ExtraArchives='{}'",
			texture_packs.height_maps ? 1 : 0, texture_packs.height_suffixes, texture_packs.max_loads_per_frame,
			texture_packs.max_load_kb_per_frame, texture_packs.evict_frames, texture_packs.extra_archives));
		log("Config", std::format("Lights={} Intensity={}% RangeMode={}",
			lights.enabled ? 1 : 0, lights.intensity_percent, lights.range_mode));
		log("Config", std::format("SessionReport: Archive={} Reporter={} ArchiveKeep={} ScrubUserName={} Snapshots={} "
			"HangSeconds={} MarkKey='{}' CrashLogFolder='{}' CrashLog={}", session_report.archive ? 1 : 0,
			session_report.reporter ? 1 : 0, session_report.archive_keep,
			session_report.scrub_user_name ? 1 : 0, session_report.snapshots ? 1 : 0, session_report.hang_seconds,
			session_report.mark_key, session_report.crash_log_folder, session_report.crash_log ? 1 : 0));
		log("Config", std::format("SunCycle={} MoonCycle={}",
			sun_cycle.enabled ? 1 : 0, moon_cycle.enabled ? 1 : 0));
		{
			std::string map;
			for (int i = 0; i < WEATHER_CLASS_COUNT; i++)
				map += std::format("{}{}={}", i ? " " : "", WEATHER_CLASS_NAMES[i], weather.preset[i].empty() ? "-" : weather.preset[i]);
			log("Config", std::format("Weather={} BindPresets={} FogFarMax={:.0f} HazeFogFarMax={:.0f} DustWindMin={} StormThunderMax={} {}",
				weather.enabled ? 1 : 0, weather.bind_presets ? 1 : 0, weather.fog_far_max, weather.haze_fog_far_max,
				weather.dust_wind_min, weather.storm_thunder_max, map));
		}
		log("Config", std::format("MenuBackground: LiveBackground={} SkipTint={} KeepDuringClose={} RedirectMode={}",
			menu_bg.live_background ? 1 : 0, menu_bg.skip_tint ? 1 : 0, menu_bg.keep_during_close ? 1 : 0,
			menu_bg.redirect_mode));
		log("Config", std::format("PipBoy: ScreenEmissive={:.2f} ScreenGlassBrightness={:.2f} ScreenGlassGloss={:.0f}",
			pipboy.screen_emissive, pipboy.screen_glass_brightness, pipboy.screen_glass_gloss));
		log("Config", std::format("Hair: Brightness={:.2f}", hair.brightness));
		log("Config", std::format("VATS: Enabled={} SampleRadius={:.1f} SampleCount={}",
			vats.enabled ? 1 : 0, vats.sample_radius, vats.sample_count));
		log("Config", std::format("SaveScreenshots: Enabled={} RefreshMs={}", save_screenshots.enabled ? 1 : 0,
			save_screenshots.refresh_ms));
		log("Config", std::format("ScreenSpace: HideScreenSpaceDraws={} ScreenSpaceLog={}",
			ffp.hide_screen_space_draws ? 1 : 0, diagnostics.screen_space_log));
	}
}
