#pragma once

// Per-weapon scope lens data (comp/game/scopes.hpp).
//
// One file per weapon, Data\Config\NewVegasRTX\Scopes\<EditorID>.ini, so a
// mod adds or replaces a weapon's lens by shipping one file. Subfolders of
// that folder (at any depth) are patches: a weapon's file is taken whole from
// the subfolder that sorts last by its path (case-insensitive, so a "zz_"
// prefix wins), else from the folder itself (docs/scope-presets-for-mod-
// authors.md). [Lens] is the
// weapon's entry; [Lens.<slots>] is an entry for one combination of
// installed weapon mods, written as the installed slot numbers in order
// ("1", "23", "123"; "0" for none). A combination's section takes its keys
// over the [Lens] section, key by key; a key neither gives has its default.
// Values are in the first-person weapon node's local space (+X toward the
// muzzle, +Y up, game units):
//
//   EyepieceCenter    = x, y, z      the eyepiece disc's center
//   Axis              = x, y, z      optical axis, eye toward objective (1, 0, 0)
//   Up                = x, y, z      reticle up (0, 1, 0)
//   EyepieceRadius    = r            (0.9)
//   ObjectiveDistance = d            from the eyepiece along the axis (20)
//   ObjectiveRadius   = r            (1)
//   EyeRelief         = d            the eye's distance behind the eyepiece (5.6)
//   Magnification     = m            0 = the game's own zoom for the weapon (0)
//   FieldStop         = degrees      (18)
//   Reticle           = none | crosshair | duplex | dot | circledot | mildot | german | drop |
//                       stadia | frame | grid | dial   (duplex)
//   ReticleColor      = r, g, b      (0, 0, 0)
//   ReticleRadiance   = v            (0)
//   ReticleLineWidth  = w            half-width, a fraction of the field stop radius (0.004)
//   Tint              = r, g, b      (0.92, 0.92, 0.92)
//   Kind              = telescopic | reflex   (telescopic)
//   Aperture          = circle | rectangle    (circle; a rectangle is EyepieceRadius high)
//   ApertureAspect    = w / h        a rectangle's width over its height (1)
//   VisionMode        = none | night | thermal   (night on a weapon whose scope has
//                                    night vision, flags2 0x1000; none otherwise)
//   HighlightHostiles = 0 | 1        outline hostile actors in the lens (0)
//   MinMagnification  = m            the mouse wheel's zoom range; both 0 = fixed (0)
//   MaxMagnification  = m            (0)
//   ReticleAccentColor    = r, g, b  the second reticle color: mil-dots, center dots (0, 0, 0)
//   ReticleAccentRadiance = v        (0)
//   EdgeGlowColor     = r, g, b      a glow ring at the field stop (0, 0, 0)
//   EdgeGlowStrength  = v            0 = none (0)
//   EdgeShade         = 0..1         darkening toward the field stop (0)
//   EdgeTint          = r, g, b      a colored edge vignette (0, 0, 0 = none)
//   NightTint         = r, g, b      night-vision phosphor color (0, 0, 0 = the runtime's)
//   NightGain         = v            night-vision gain (0 = the runtime's)
//   Markings          = labels | rangefinder | labels,rangefinder   the reticle style's
//                                    labels, a range readout in meters (none)
//   ReticlePlane      = second | first   the reticle in the second focal plane (its size
//                                    fixed) or the first (it scales with the zoom) (second)
//   ReticleTexture    = path         the reticle image, relative to Data or absolute (none)
//   MarkingColor      = r, g, b      the labels' and rangefinder's color (not given: [Scopes]
//                                    MarkingColor; 0, 0, 0: the accent's)
//   MarkingRadiance   = v            0 = the accent's lit or ink look; above 0 lit (not given
//                                    with MarkingColor: [Scopes] MarkingRadiance)
//   MarkingOpacity    = 0..1         the markings' coverage (0 = opaque)
//   ParallaxDistance  = meters       the distance the reticle is focused at; 0 = none
//                                    (100 on telescopic scopes, 0 on reflex sights)
//   GlareStrength     = 0..1         glare on the glass ([Scopes] GlareStrength)
//   DirtStrength      = 0..1         dirt on the glass ([Scopes] DirtStrength)
//   Node              = name         the node of the first-person weapon's model the
//                                    values are in (its root when not given or not found)
//   Models            = path, ...    the first-person models (relative to meshes\) the
//                                    values were measured on; when given and the weapon's
//                                    model for its mods is none of them, the weapon keeps
//                                    the game's own scope (none: no check)
//   VisionModes       = mode, ...    the modes the vision key cycles (none and VisionMode)
//   Grain             = on | off     film grain on the lens (on)
//
// Weapons are named by EditorID. The game keeps no EditorID for a weapon at
// run time, so the names come from the loaded plugin files: their WEAP
// records, read once, mapped to run-time form IDs through the load order the
// engine monitor plugin reports. Main thread only.
namespace comp::game::scopes::data
{
	// VisionMode not given: the weapon's own (night when its scope has night vision).
	constexpr int kVisionDefault = -1;

	// Reticle shapes: none, crosshair, duplex, dot, circle-dot, mil-dot,
	// german, drop, stadia, frame, grid, dial.
	constexpr int kReticleCount = 12;

	struct Lens
	{
		float eyepiece[3] = { 0.0f, 0.0f, 0.0f };
		float axis[3] = { 1.0f, 0.0f, 0.0f };
		float up[3] = { 0.0f, 1.0f, 0.0f };
		float eyepiece_radius = 0.9f;
		float objective_distance = 20.0f;
		float objective_radius = 1.0f;
		float eye_relief = 5.6f;
		float magnification = 0.0f;
		float field_stop = 18.0f;
		int reticle = 2;                       // 0 none .. 11 dial, as kReticleCount names
		float reticle_color[3] = { 0.0f, 0.0f, 0.0f };
		float reticle_radiance = 0.0f;
		float reticle_line_width = 0.004f;
		float tint[3] = { 0.92f, 0.92f, 0.92f };
		int kind = 0;                          // 0 telescopic, 1 reflex
		int aperture = 0;                      // 0 circle, 1 rectangle, 2 binocular
		float aperture_aspect = 1.0f;
		int vision_mode = kVisionDefault;      // kVisionDefault, or 0 none, 1 night, 2 thermal
		bool highlight_hostiles = false;
		// VisionModes: the modes the vision key cycles through, in order (0 none,
		// 1 night, 2 thermal); vision_count 0 = not given.
		uint8_t vision_list[3] = {};
		int vision_count = 0;
		float min_magnification = 0.0f;
		float max_magnification = 0.0f;
		float reticle_accent_color[3] = { 0.0f, 0.0f, 0.0f };
		float reticle_accent_radiance = 0.0f;
		float edge_glow_color[3] = { 0.0f, 0.0f, 0.0f };
		float edge_glow_strength = 0.0f;
		float edge_shade = 0.0f;
		float edge_tint[3] = { 0.0f, 0.0f, 0.0f };
		float night_tint[3] = { 0.0f, 0.0f, 0.0f };
		float night_gain = 0.0f;
		char node[64] = {};                    // a node of the weapon model by name; empty: its root
		int markings = 0;                      // bit 0 labels, bit 1 rangefinder
		int reticle_plane = 0;                 // 0 second focal plane, 1 first
		bool marking_color_set = false;        // the entry gives MarkingColor
		float marking_color[3] = { 0.0f, 0.0f, 0.0f };
		float marking_radiance = 0.0f;
		float marking_opacity = 0.0f;
		char reticle_texture[128] = {};        // a texture path; empty: the procedural reticle
		float parallax_m = -1.0f;              // meters; below 0: by kind
		float glare = -1.0f;                   // below 0: [Scopes] GlareStrength
		float dirt = -1.0f;                    // below 0: [Scopes] DirtStrength
		bool grain = true;                     // Grain = off: the lens shows no film grain
		char models[768] = {};                 // Models, normalized (normalize_model), comma-separated; empty: no check
	};

	// A model path as Models compares it: lowercase, backslashes, relative to
	// meshes\ (a leading "meshes\" and "data\meshes\" dropped).
	std::string normalize_model(std::string_view path);

	// The entry lists `model` (normalized) in Models.
	bool lists_model(const Lens& lens, std::string_view model);

	// Adds `model` (normalized) to the entry's Models when missing.
	void add_model(Lens& lens, std::string_view model);

	// The file a weapon's entries come from (for the logs), or empty.
	std::wstring source(uint32_t form_id);

	// The section an entry came from.
	constexpr int kBase = -1;   // [Lens]; 0..7: [Lens.<slots>] by installed-mod mask (bits 1, 2, 4)

	// The weapon's EditorID, or null while it is unknown (the load order is
	// not readable yet, or no loaded plugin names the form).
	const std::string* editor_id(uint32_t form_id);

	// The weapon's lens for this mod mask. False when its file has neither
	// the combination's section nor [Lens]. `section` gets the most specific
	// section used.
	bool resolve(uint32_t form_id, uint8_t mods, Lens& out, int* section = nullptr);

	// The weapon has a file with at least one section (or an unsaved edit).
	bool has_any(uint32_t form_id);

	// Replaces the section's keys with `lens` in memory (the file is written
	// by save). False without an EditorID.
	bool set(uint32_t form_id, int section, const Lens& lens);

	// Writes the weapon's file. `why` gets the path or the failure.
	bool save(uint32_t form_id, std::string& why);

	// The weapon has edits not yet saved.
	bool dirty(uint32_t form_id);

	// Forgets every file read (and unsaved edits); they are read again on use.
	void reload();

	// The folder the files live in, for the logs and the menu.
	std::wstring folder();

	// "[Lens]" or "[Lens.23]".
	std::string section_name(int section);
}
