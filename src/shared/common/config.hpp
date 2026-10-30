#pragma once

namespace shared::common
{
	// The weather classes the wrapper reports to the NUMOS sky (game/weather.*),
	// as the ini keys of [Weather] and, lower-cased, the ids fnv.<class>.
	inline constexpr int WEATHER_CLASS_COUNT = 9;
	inline constexpr const char* WEATHER_CLASS_NAMES[WEATHER_CLASS_COUNT] = {
		"Clear", "Haze", "Cloudy", "Overcast", "Fog", "Dust", "Rain", "Storm", "Snow" };
	inline constexpr const char* WEATHER_CLASS_DEFAULT_PRESETS[WEATHER_CLASS_COUNT] = {
		"Fair", "Haze", "Cloudy", "Overcast", "Fog", "Haze", "Drizzle", "Storm", "Overcast" };

	class config
	{
	public:
		static config& get();

		void load(const std::string& ini_path);
		// The ini load() read (the game folder's remix-comp.ini).
		const std::string& ini_path() const { return ini_path_; }

		int get_int(const char* section, const char* key, int default_val) const;
		std::string get_string(const char* section, const char* key, const char* default_val) const;
		float get_float(const char* section, const char* key, float default_val) const;
		bool get_bool(const char* section, const char* key, bool default_val) const;
		// Comma / space separated 32-bit hex values ("0x1234ABCD, 5678"); empty when unset.
		std::vector<uint32_t> get_hash_list(const char* section, const char* key) const;

		struct ffp_settings
		{
			bool enabled = true;
			int albedo_stage = 0;
			int terrain_albedo_stage = 0;   // for has_color && n_texcoords >= 2 (HQ terrain / multi-tile blend)
			int bi_albedo_stage = 1;        // for !skinned && has_blendindices (atlas-tile-selector decls)

			// Skip "fake shadow" overlay draws: NOLIGHTING geometry whose vertex
			// colors are all grayscale with at least one dark vertex (FNV's baked
			// planar shadow overlays). Remix traces real shadows, so these are
			// dropped.
			bool skip_fake_shadows = true;

			// Tag orthographic passthrough draws (PASS_2D: HUD, menus, Pip-Boy
			// cursor / highlight quads) InstanceCategories::Hidden. With
			// rtx.useVertexCapture the runtime otherwise turns each one drawn
			// before RTX injection into a small traced quad at the near plane
			// of the last FFP projection: a dark square floating at the player's
			// eye that shadows the Pip-Boy. Rasterisation is unchanged.
			// See comp/game/screen_space.hpp.
			bool hide_screen_space_draws = true;

			// VS constant register layout: combined WorldViewProj at c0-c3,
			// World at c8-c11.
			static constexpr int vs_reg_view_start = 0;
			static constexpr int vs_reg_view_end = 4;
			static constexpr int vs_reg_proj_start = 0;
			static constexpr int vs_reg_proj_end = 4;
			static constexpr int vs_reg_world_start = 8;
			static constexpr int vs_reg_world_end = 12;

			static constexpr int vs_reg_bone_threshold = 20;
			static constexpr int vs_regs_per_bone = 3;
			static constexpr int vs_bone_min_regs = 3;
		} ffp;

		// [RemixApi]: every ray-traced draw goes through the Remix C API
		// (CreateMesh once per unique object, DrawInstance / retained
		// instances per frame); the game's D3D9 draw is skipped. A traced
		// draw no API path can take is not drawn (docs/fnv-api-only.md).
		// Near land is a TerrainLayersEXT blend, far land LOD carries the
		// engine's sink, water is a translucent ANIMATED_WATER mesh.
		struct remix_api_settings
		{
			// Weight / tint texture edge length in texels (see api_terrain.cpp).
			int terrain_weight_res = 65;
			float terrain_roughness = 0.85f;
			// Terrain normal-map alpha is gloss (NVR convention) when the layer has
			// real alpha. Off: never (as for BC1 normals).
			bool terrain_normal_gloss = true;
			// Per-layer texture-pack height maps (_p) on API land: parallax on the
			// blended layers (needs [TexturePacks] HeightMaps and
			// rtx.fnvProtocol.heightDisplaceIn > 0). Live: land rebuilds.
			bool terrain_height_maps = true;
			float far_lod_roughness = 0.9f;
			// The detail the game's far-LOD shader adds: each block's own normal
			// map (world space, converted to the runtime's tangent space) and
			// LODLandNoise tiled 1.75 times per block over the albedo.
			bool far_lod_detail = true;
			// A far-LOD mesh whose sink constants changed is rebuilt at most
			// once per this many frames (the old mesh keeps drawing meanwhile).
			int far_lod_rebuild_frames = 30;
			// New API meshes per frame, across all features. A draw over the
			// budget is not drawn that frame (grass, leaves, unlit FX: their
			// current copy draws) and is built on a later one.
			int max_builds_per_frame = 16;
			// Frames an API mesh may go undrawn before it is destroyed.
			int evict_frames = 900;
			// Frames an unreferenced uploaded texture is kept for reuse.
			int texture_evict_frames = 1800;
			// Hand game textures to Remix by reference (remixapi_ImportD3D9Texture,
			// exported by the bridge) instead of reading their pixels and uploading
			// them with CreateTexture. Per texture, anything the import can't take
			// keeps the upload path (api_textures.cpp).
			bool import_d3d9_textures = true;
			// Plain D3DPOOL_DEFAULT textures (NVTF bUseDefaultPoolForTextures)
			// stay DEFAULT: their content is hashed from UpdateTexture's
			// SYSTEMMEM source and they reach Remix by import (tex_pool.hpp).
			// 0 creates them MANAGED, which costs a bridge-client CPU copy each.
			bool keep_default_pool_textures = true;

			float water_ior = 1.33f;
			float water_measurement_distance = 256.0f;
			// Scales the medium's transmittance colour (0..1]: lower = murkier.
			float water_clarity = 0.8f;
			// Water normal-map tile size in world units (1120 = 16 m); 0 = the
			// water VS's own TexScale.x.
			float water_tile_size = 1120.0f;
			// Ripple normal map edge (power of two, <= 1024; BC5 + mips).
			int water_normal_res = 1024;
			float water_normal_strength = 1.0f;
			// Push the runtime's rtx.translucentMaterial.animatedWater* options
			// (drift speeds, second-layer scale) derived from the values below.
			bool water_runtime_options = true;
			// Primary layer drift, world units per second (21 = 0.3 m/s).
			float water_flow_speed = 21.0f;
			// Second (swell) layer: UV scale relative to the first (0.31 = a
			// ~52 m tile at the default 16 m), its drift (world units/s) and an
			// extra LOD bias on its sample.
			float water_detail_scale = 0.31f;
			float water_detail_flow_speed = 35.0f;
			float water_detail_lod_bias = 0.0f;
			// Distant (WATER034) water's colours blended toward the near water in
			// view: 1 = one shared material, 0 = its own worldspace colours.
			float water_lod_near_blend = 1.0f;
			// The runtime's underwater look (comp/game/underwater.*):
			// rtx.underwater.enable / surfaceHeight and the camera medium from
			// the engine's water state.
			bool underwater = true;

			// Stable render origin (comp/game/stable_origin.*): hand Remix a
			// world frame anchored at O, the camera snapped to a StableOriginGrid
			// grid, instead of FNV's camera-relative one, in which every
			// transform changes whenever the camera moves. O re-snaps once the
			// camera is more than StableOriginRadius units from it. Off: the
			// camera-relative frame.
			bool stable_origin = true;
			float stable_origin_grid = 4096.0f;
			float stable_origin_radius = 8192.0f;
			// Tell the runtime O each frame it changes (remixapi_SetGameValue
			// __mcrtx.world_origin.x/y/z), so with rtx.worldOrigin.enable it
			// knows render position + O = world position: clouds and their
			// ground shadows stay in the world, and camera / instance history
			// is re-expressed on a re-snap. Only meaningful with stable_origin.
			bool publish_world_origin = true;
			// Render units per FNV world unit (comp/game/stable_origin.*):
			// every world-space value the wrapper hands the runtime is
			// multiplied by it. 1.428571 makes a render unit one centimetre
			// (70 FNV units = 1 m). rtx.conf's world-unit options are in
			// render units and must match it. Read once at startup.
			float world_unit_scale = 1.428571f;
			// Hand the runtime the game's world camera once per frame
			// (SetupCamera with the FALLBACK frame event), at the frame's first
			// world draw. The runtime uses it only when no D3D9 draw set its
			// main camera (the world went through the API: interiors whose
			// statics are all retained instances), so path tracing never stops
			// for want of a camera; a D3D9 world draw's camera always wins.
			bool fallback_camera = true;
			// Register API draws once as retained instances and replay them
			// every frame (needs StableOrigin and a bridge that forwards the
			// retained-instance calls; falls back to DrawInstance otherwise).
			bool retained_instances = true;
			// Issue the frame's DrawRetainedInstances(O) at the frame's first
			// BeginScene (after the stable origin's begin_frame, so with this
			// frame's O), ahead of the HUD draw that triggers the runtime's
			// injection (the UI boundary). Off: at Present, after the trace (then
			// rtx.retained.replayAtInjection has to correct it).
			bool retained_replay_at_frame_start = true;
			// Culling is not visibility: a retained entry the game didn't draw
			// this frame stays visible (shadows, reflections, no re-instancing
			// on every camera turn). It is hidden only when its cell leaves the
			// loaded grid / the interior changes / the camera jumps (land, near
			// water, statics), when its geometry dies (statics, ~NiGeometry),
			// or, for far LOD and distant water, when it is on screen and not
			// drawn for retained_lod_stale_frames.
			// Off: hidden the first frame it isn't drawn.
			bool retained_keep_culled = true;
			// A visible entry kept while not drawn is destroyed after this many
			// frames undrawn (the TTL backstop; hidden entries keep EvictFrames).
			int retained_keep_culled_frames = 36000;
			// Per feature (land, far LOD, water, static placements): above this
			// many entries the EvictFrames rule applies to all of them.
			int retained_keep_culled_cap = 8192;
			// Far LOD / distant water: frames in view and not drawn before the
			// block is hidden. Only the view rule
			// (retained_lod_rule 0), and engine-rule entries with no NiGeometry.
			int retained_lod_stale_frames = 10;
			// What hides a far-LOD / distant-water entry the game didn't draw:
			//   0 = view: in view (3-degree guard) and not drawn for
			//       retained_lod_stale_frames;
			//   1 = engine (default): only what the engine did to the block's
			//       NiGeometry: destroyed (~NiGeometry), drawn again under
			//       another entry (re-keyed), app-culled or detached on its
			//       parent chain. Culled / occluded blocks stay. An entry with
			//       no NiGeometry (dispatcher hook unavailable) keeps rule 0.
			int retained_lod_rule = 1;

			// Pixel-shader hashes (0xXXXXXXXX, comma-separated, as logged by
			// PSReflect) the scene-graph statics may take (empty = every lit
			// object shader) / must never take; the rest goes through draw
			// capture (comp/game/api_statics.cpp).
			std::vector<uint32_t> statics_ps_allow;
			std::vector<uint32_t> statics_ps_deny;
			// Debug: statics (scene graph and draw capture) get a plain
			// albedo-only material (no terrain-layer normal / gloss, height,
			// glow, alpha cutout or vertex colour), to tell a material problem
			// from a placement one. New meshes only.
			bool statics_debug_plain_material = false;

			// Time the scene statics' seed pass may take per frame, microseconds
			// (members of the loaded scene graph recorded without a draw).
			int scene_walk_budget_us = 400;
			// Rigid statics discovered when the engine dispatches their render
			// passes (comp/game/scene_statics.*):
			// the first base pass gives the material, the geometry's packed
			// VB / IB the mesh, and the NiGeometry becomes one retained
			// instance kept while culled; its passes are then skipped in the
			// engine. Lifetime from ~NiGeometry and the loaded cells. Needs
			// retained_instances and stable_origin. What it does not take goes
			// through draw capture (comp/game/api_statics.cpp), per draw.
			bool scene_graph_statics = true;
			// Base form types (hex, TESForm+0x04) of the references whose
			// geometry scene_graph_statics may take. Empty in the ini = the
			// default: STAT 20, SCOL 21, ACTI 15, TACT 16, TERM 17, CONT 1B,
			// DOOR 1C, LIGH 1E, MSTT 22, FLOR 26, FURN 27, ARMO 18, BOOK 19,
			// CLOT 1A, INGR 1D, MISC 1F, WEAP 28, AMMO 29, KEYM 2E, ALCH 2F,
			// NOTE 31. Actors (ACHR / ACRE) are never taken.
			std::vector<uint32_t> scene_base_types;
			// Alpha-blended geometry (NiAlphaProperty blend bit) scene_graph_statics
			// may take, with the draw's own alpha state on the instance
			// (InstanceInfoBlendEXT + useDrawCallAlphaState): 0 = none; 1 =
			// blended AND alpha-tested (cutouts with a soft edge: foliage cards,
			// fences, grates, decal edges); 2 = also blended without a test
			// (glass, plain decals). Plain alpha blending (SRCALPHA,
			// INVSRCALPHA) only; additive and other factors (effects) go through
			// draw capture.
			int scene_claim_alpha_blend = 2;
			// Decals get REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC (DYNAMIC for
			// the dynamic-decal flag or a moving draw) on every world path
			// (scene statics, draw capture, unlit; api_material decal_of): the
			// shader property's decal flags (flags1 bit 26 decal / 27 dynamic
			// decal, flags2 bit 21 alpha decal). The runtime blends a decal onto
			// the surface under it. Without the category an overlay is traced as
			// a surface of its own, and indirect rays still pass through the
			// surface under its cut-out texels. The rtx.conf decal lists apply
			// either way.
			bool scene_decal_category = true;
			// A glow map whose EmittanceColor is grey (R = G = B): black drops the
			// glow, grey scales the emissive intensity. A coloured tint rides on
			// the emissive constant (ApiGlowTint).
			bool scene_glow_grey_tint = true;
			// A colour + two-texcoord (land-shaped) draw whose geometry hangs
			// under a placed reference and whose PS is not a land shader is
			// recorded as a static instead of refused as terrain.
			bool scene_land_shape_statics = true;
			// The relaxed streamed rule: a vertex / index buffer that saw a
			// DISCARD / NOOVERWRITE write lock is still taken when it is not
			// D3DUSAGE_DYNAMIC (D3D9 ignores both flags on any other buffer),
			// was not written again within the rewrite window, and has been
			// left alone for a moment since. A claimed buffer written later
			// gives its geometry to draw capture (write generations). Off = the
			// strict rule: any such lock since creation refuses the buffer.
			bool scene_streamed_relaxed = true;
			// Diagnostics: once per session (and again when changed), the top N
			// refused geometries per reason by passes, with class, shader
			// property, alpha flags, texture, base type and PS / VS hash
			// ([SceneStatics] refused <reason> #k). 0 = off.
			int scene_debug_refusal_report = 5;
			// Mesh bytes (vertices + indices) created per frame, KiB.
			int scene_upload_kib_per_frame = 4096;
			// Debug: every visible scene instance as a per-frame DrawInstance
			// (issued at the frame's first render pass) instead of retained.
			bool scene_debug_draw_instance = false;
			// Debug: log the first N instances created. 0 = off.
			int scene_debug_dump = 0;
			// Debug: base form types (hex) whose claimed geometries log every
			// state change ([SceneTrace]: claim, refusal, deferral, move, hide /
			// show with its cause, stale buffer, unclaim with its cause, the
			// engine's render passes resuming after a gap, D3D9 drawing it after
			// an unclaim), e.g. 22 for MSTT. Empty = off (the default).
			std::vector<uint32_t> scene_debug_trace_base_types;

			// Coloured glow tints (a glow map's non-white EmittanceColor) and the
			// unlit FX's MaterialColor ride on API materials as the emissive
			// constant: pushes rtx.fnvProtocol.apiGlowTint=True to the runtime,
			// which then tints API emissive textures by it (a runtime without
			// the option leaves them untinted). Off: coloured glow is dropped
			// (statics, foliage, skinned).
			bool api_glow_tint = true;

			// Hair (the hair PS with a LayerMap) takes its albedo from the
			// game's own PS, run on the CPU with the lighting neutralised
			// (comp/game/api_hair_bake.hpp), instead of the LayerMap terrain
			// layer. Baked on a background thread; until a bake is uploaded
			// the hair keeps the LayerMap material. A bake whose probe checks
			// fail is refused and logged.
			bool shader_bake_hair = true;

			// Unlit NoLighting FX, the NOLIGHT TexCoordTranform family
			// (comp/game/api_unlit.cpp): one cached API mesh per draw geometry
			// with the UV transform baked into its texcoords and MaterialColor
			// into its vertex colours (the part above 1 as
			// MaterialInfoOpaqueAlbedoTintEXT when the bridge forwards it), drawn
			// as a retained instance (DrawInstance per frame while its UVs
			// animate: the mesh is refreshed in place, same hash).
			// Emissive intensity of the unlit material (the texture itself as
			// the glow, tinted by MaterialColor). 0 = lit, no glow.
			// Additive draws get none (the runtime already makes them emissive).
			float unlit_emissive_intensity = 1.0f;
			// One [RemixApi] "unlit draw" line per distinct draw (texture, UV
			// transform, vertex colour, MaterialColor, blend, decision), at most
			// 256 a session.
			bool unlit_debug_log = false;

			// First-person API draws (arms, weapon, unlit FX) carry the
			// VIEW_MODEL category and the frame's viewmodel projection goes to
			// the runtime as its VIEW_MODEL camera (SetupCamera). With
			// rtx.viewModel.enable off the runtime traces them as world
			// geometry. Off: plain world instances.
			bool view_model_camera = true;

			// Frames between the periodic status lines (scene, terrain,
			// foliage, retained, ...). 0 = off.
			int stats_interval_frames = 600;
		} remix_api;

		// [TexturePacks]: extra maps a texture pack ships next to a diffuse
		// that vanilla never binds (comp/game/tex_pack.*).
		struct texture_pack_settings
		{
			// Height maps: diffuse "foo.dds" -> "foo_p.dds" (loose Data\Textures
			// first, then the loaded BSAs), bound for draws whose shader has no
			// HeightMap of its own and announced through the RS 149 height
			// nibble. The runtime turns it into POM, so rtx.displacement must be
			// enabled and rtx.fnvProtocol.heightDisplaceIn (> 0) sets the depth.
			bool height_maps = true;
			// Comma-separated suffixes tried in order ("_p" = NVR / texture-pack
			// convention; add e.g. "_h" for packs that use it).
			std::string height_suffixes = "_p";
			// Height textures created per frame, and the file bytes they may
			// total (at least one is always created). A draw whose height map
			// isn't loaded yet renders without POM that frame.
			int max_loads_per_frame = 4;
			int max_load_kb_per_frame = 8192;
			// Frames a loaded height map may go unused before it is released.
			int evict_frames = 3600;
			// Extra archives (comma-separated, relative to Data\) indexed after
			// the game's own list, highest priority last.
			std::string extra_archives;
			// Log every resolved height map path (first sight only).
			bool log_resolves = false;
			// Frames between the [TexPack] status lines. 0 = off.
			int stats_interval_frames = 1800;
		} texture_packs;

		struct skinning_settings
		{
			// The game hooks that capture each skinned object's bone palette
			// (comp/game/game.cpp). The Remix-API skinned path needs them: off,
			// skinned actors are not drawn.
			bool enabled = true;
		} skinning;

		struct lights_settings
		{
			bool enabled = true;
			int intensity_percent = 100;
			int range_mode = 0;       // 0 = the light's radius, 1 = from attenuation, 2 = infinite
			int effect_forward = 1;           // the Pip-Boy light: units ahead of the eye
			int effect_third_person_forward = 20;   // the Pip-Boy light in third person: units ahead of the engine's position
			int follower_intensity_percent = 100;  // the Pip-Boy light: extra multiplier
		} lights;

		struct diagnostics_settings
		{
			bool enabled = false;
			int delay_ms = 300000;
			int log_frames = 3;
			// Logs every pixel shader's hash and draw-shape fingerprint on first
			// sight to ps_harvest.log, to identify UI and keep-diffuse shaders.
			bool harvest_ps = false;
			// [Diagnostics] ScreenSpaceLog: log the first N distinct screen-
			// space draws and small FFP draws near the camera (route, shader
			// hashes, the stage-0 texture's Remix hash and rtx.conf list,
			// transform summary). 0 = off. See comp/game/screen_space.hpp.
			int screen_space_log = 0;
		} diagnostics;

		// [Logging]: rtx-remix\logs\remix-comp.log (shared/common/log.cpp).
		struct logging_settings
		{
			// Every line is written, with the dev diagnostics gated on
			// log_verbose(). Off: those stay quiet and a repeated line kind
			// is written at most once every 5 minutes after its first 10.
			bool verbose = false;
			// A console window that shows the log as it is written.
			bool console = false;
		} logging;

		// [SessionReport]: rtx-remix\logs\session.json, the load-order and
		// config snapshots, the zip of the previous session, the hang
		// watchdog and the mark key (comp/game/session_report.*).
		struct session_report_settings
		{
			// Zip each session into rtx-remix\logs\archive: when the game quits,
			// else when its process ends (Reporter), else at the next start.
			bool archive = true;
			// NewVegasRTX-Reporter.exe waits for the game process to end and
			// zips its session then, whatever ended it.
			bool reporter = true;
			// The newest zips kept in the archive folder.
			int archive_keep = 20;
			// The zip's copies of text files say <user> for the Windows user
			// name and profile path; dumps get the name masked.
			bool scrub_user_name = true;
			// loadorder.txt, loadorder-changes.txt and the config\ copies.
			bool snapshots = true;
			// Seconds without a frame (game window in front, no load running)
			// before a hang dump is written. 0 = no watchdog.
			int hang_seconds = 15;
			// The mark key: F1-F24 or a virtual-key code. Empty or 0 = off.
			std::string mark_key = "F8";
			// Crash Logger's folder of crash logs, relative to the game folder.
			std::string crash_log_folder = "Crash Logs";
			// On an unhandled exception, rtx-remix\logs\crash-<time>.txt
			// (comp/game/crash_log.*).
			bool crash_log = true;
		} session_report;

		struct remix_settings
		{
			bool enabled = true;
			std::string dll_name = "d3d9_remix.dll";
		} remix;

		struct chain_settings
		{
			std::string preload;   // semicolon-separated DLLs/ASIs loaded before the d3d9 chain
			std::string postload;  // semicolon-separated DLLs/ASIs loaded once the game window exists
		} chain;

		struct tracer_settings
		{
			int backtrace_depth = 8;
			std::string output_dir = "captures";
		} tracer;

		struct sun_cycle_settings
		{
			bool enabled = true;
		} sun_cycle;

		struct moon_cycle_settings
		{
			bool enabled = true;
		} moon_cycle;

		// [Weather]: the game's weather reported to the NUMOS sky (game/weather.*).
		struct weather_settings
		{
			bool enabled = true;
			// Bind each class id (fnv.<class>) to its preset in the preset files at startup.
			bool bind_presets = true;
			// The runtime's rtx.weather.presetDirectory; relative paths are from the game folder.
			std::string preset_directory = "rtx-remix/weather";
			// Classification thresholds on the TESWeather record.
			float fog_far_max = 20000.0f;       // day fog far at or below this: fog (or dust in wind)
			float haze_fog_far_max = 60000.0f;  // at or below this: haze, overcast or dust
			int   dust_wind_min = 100;          // wind byte at or above this turns fog / haze into dust
			int   storm_thunder_max = 200;      // a rainy weather with thunder frequency at or below this: storm
			// NUMOS preset per class (file name without .conf); empty leaves the class unbound.
			std::string preset[WEATHER_CLASS_COUNT];
		} weather;

		// [MenuBackground]: the live, path-traced world behind the Pip-Boy
		// (game/menu_bg.*).
		struct menu_bg_settings
		{
			// Re-run the engine's menu-background capture every Pip-Boy frame
			// and route it as world geometry, instead of pasting the frozen
			// capture taken when the Pip-Boy opened.
			bool live_background = true;
			// Drop the ISMENUBG sepia/darken pass behind the live Pip-Boy.
			bool skip_tint = true;
			// Keep the live background through the Pip-Boy's closing animation
			// (the Pip-Boy states close requested / closing).
			bool keep_during_close = true;
			// 0 = route a same-sized capture target in place only,
			// 1 = in place if same-sized, else bind the backbuffer instead,
			// 2 = always bind the backbuffer instead.
			int redirect_mode = 1;
		} menu_bg;

		// [PipBoy]: the Pip-Boy screen.
		struct pipboy_settings
		{
			// Emissive intensity of the Pip-Boy screen's contents (the
			// interface render target blended onto the screen mesh): the
			// emission is contents x contents alpha x this, in the runtime's
			// emissive-intensity units. 0..16 (0..4 on a runtime without
			// rtx.worldSpaceUiEmissiveIntensity); 0 = the contents are not drawn.
			float screen_emissive = 6.0f;
			// Albedo of the screen glass under the contents (its opaque base
			// passes), as a fraction of their texture: 0..1. The glass is
			// also made glossy and non-metallic, like dark CRT glass.
			float screen_glass_brightness = 0.0f;
			// Gloss of the screen glass: a Blinn-Phong exponent, 10..1000
			// (roughness 0.64..0.21; 200 = 0.32).
			float screen_glass_gloss = 10.0f;
		} pipboy;

		// [Hair]: hair, beards and brows drawn with the hair shader.
		struct hair_settings
		{
			// Albedo multiplier over the shader's own colour, 1..4. 1 is the
			// shader's colour. FNV's overbright lighting and anisotropic sheen
			// make hair lighter in game, which a path-traced material lacks.
			// The runtime clips albedo at 1, as the game's lighting clips.
			float brightness = 1.0f;
		} hair;

		// [VATS]: the per-limb visibility VATS hit chance scales with (game/vats.*).
		struct vats_settings
		{
			// Line-of-sight rays from the player in place of the engine's
			// occlusion queries, which see no pixels of an actor the path
			// tracer draws. Off: the engine's own value (0 under Remix).
			bool enabled = true;
			// Radius in game units of the ring of sample points around a limb.
			float sample_radius = 10.0f;
			// Rays per limb: one to the limb, the rest to the ring. 1..16.
			int sample_count = 7;
		} vats;

		// [SaveScreenshots]: the picture a save stores (game/save_thumbnail.*).
		struct save_screenshot_settings
		{
			// The save takes the runtime's latest thumbnail of the path-traced
			// frame in place of the engine's own capture, which is blank
			// under Remix.
			bool enabled = true;
			// How often the runtime refreshes the thumbnail during play, in ms.
			int refresh_ms = 1500;
		} save_screenshots;

	private:
		std::string ini_path_;
		bool loaded_ = false;

		void parse_all();
	};
}
