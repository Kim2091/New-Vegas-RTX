#pragma once

// Path-traced scopes ([Scopes] in remix-comp.ini).
//
// The engine shows a scope as a 2D overlay: the equip paths attach the
// weapon's scope model to the HUD 3D node (fnv_exe scope_model_attach), aiming
// puts the HUD in its scoped state (scope_set_visible: the overlay shows and
// RenderWorldScene scissors the view), Actor::SetAiming hides the
// first-person weapon and keeps the ironsights aim anim off, and
// PlayerCharacter::UpdateFOV narrows the FOV to the weapon's sight FOV.
//
// A lens weapon is a gun whose scope vanilla would show (flags1 0x04
// HasScope with a scope model path, a projectile, and when flags2 0x2000
// ScopeFromMod is set an installed weapon mod with effect 14) that has a lens
// entry (scope_data.hpp) for its installed mods. Aiming one keeps the world
// and the weapon on screen, raised in its ironsights pose, and the main
// camera carries one remixapi_CameraInfoLensEXT: the entry's lens, carried
// by the first-person weapon node (biped slot 5), sent in the render frame
// of the first-person draws. Binoculars (a scope model, no projectile, no
// ammo) need no entry: aiming them shows one camera-attached lens with the
// binocular aperture (two overlapping circles), with the overlay, zoom and
// crosshair hooks as for scopes and the
// game's own cull and aim animation. Every other weapon keeps vanilla scopes.
//
// The hooks, each a pass-through for anything but a lens weapon while the
// feature is active:
//   * scope_model_attach: the scope model is not attached (the previous one
//     is still removed);
//   * scope_set_visible: the HUD does not enter its scoped state (no
//     overlay, no scissor);
//   * aim_cull_model_call: SetAiming's scope model test reads no model on
//     aim start, so the first-person weapon is not hidden;
//   * aim_scope_mod_call: SetAiming's ScopeFromMod test reads false, so a
//     mod-supplied scope gets the ironsights +3 as a built-in one does;
//   * weap_use_1st_is_anims: true for the player's lens weapon, which
//     overrides flags1 0x40 (most built-in scopes set it) for the aim pose
//     and the anims played while aiming;
//   * fov_zoom_aiming_call: UpdateFOV's zoom reads "not aiming", so the FOV
//     stays at the default;
//   * hud_sighting_call: the HUD reticle state sees a sighting node while a
//     lens weapon is aimed in first person, so it hides the crosshair (the
//     lens draws its own reticle); the center reticle tile is also kept
//     hidden every frame the lens is up, since the HUD sets it only on a
//     mode change;
//   * nv_scope_flag_call: a lens weapon's scope has no night vision for the
//     player update, so the vanilla scope IMAD stays off (the lens has its
//     own vision modes);
//   * input_poll: while a lens is up, the mouse wheel zooms it between the
//     entry's MinMagnification and MaxMagnification and the game sees no
//     wheel.
//
// Vision modes: the entry's VisionMode, else night for a weapon whose scope
// has night vision (flags2 0x1000), else none. [Scopes] VisionKey cycles
// between none and that mode while the lens is up (every mode with
// VisionAllModes).
//
// Aim alignment ([Scopes] AlignAim): the first-person camera setup
// (0x874C10, and 0x875110) puts the first-person camera at the world
// position of the node PlayerCharacter +0xE34 holds (the ##SightingNode
// SetAiming stores with bTrueIronSights) unless the player's anim state or
// the camera state rules it out (0x8A8870, 0x525430); its readers use only
// that position (+0x8C) and whether it is set. While a lens weapon is
// aimed, +0xE34 points at a wrapper-owned point at the entry's eye point
// (eyepiece center minus eyeRelief along the axis), so the eye looks down
// the scope. It is cleared once the lens is gone.
//
// Active: the hooks are installed (at startup, with PathTraced=1), the
// setting is on, [RemixApi] FallbackCamera is on (the lens rides on that
// camera) and the bridge client forwards the extension. A change applies at
// the next aim; a scope model already attached or not follows at the next
// equip.
namespace comp::game::scopes
{
	// Game-code patch time: installs the hooks when [Scopes] PathTraced=1.
	void install();

	// True when every hook is in place (Host Settings: PathTraced is live then).
	bool hooks_installed();

	// The frame's lens for the main camera, in the render frame the camera
	// goes out in: a scope's (aimed, or at the hip with [Scopes] LensAlways),
	// or the binoculars' while aimed (camera-attached, the binocular
	// aperture). False when there is none: the feature is inactive, the
	// player is in third person, or the weapon is not a lens weapon. Also
	// places the aim alignment point.
	bool lens(remixapi_CameraInfoLensEXT& out);

	// The camera went out with `view` (the device view) and `lens`: with
	// [Scopes] AimDebugLog, logs this frame's aim line, with the eye's offset
	// from the lens's exit pupil as the runtime sees it.
	void lens_sent(const D3DMATRIX& view, const remixapi_CameraInfoLensEXT& lens);

	// Present: clears the aim alignment and shows the crosshair again when no
	// lens went out this frame, reads the vision key, builds the next frame's
	// actor tags and follows the equipped weapon for the editor.
	void on_frame_end();

	// The instance tags (REMIXAPI_INSTANCE_CATEGORY_BIT_HEAT_SOURCE,
	// _HIGHLIGHT_HOSTILE) of the actor whose 3D holds `geometry`, for the
	// skinned draws. 0 unless a lens with a vision mode or hostile
	// highlighting is up. Tags are built once per frame at Present from the
	// high and middle-high process actors: living ones are heat sources,
	// hostile ones (Actor::IsInCombatWith the player 0x8BC700, or hostile on
	// the compass, PlayerCharacter +0xD48) are highlighted.
	uint32_t actor_tags(uintptr_t geometry);

	// A first-person draw is being submitted: at a draw of the held weapon's
	// model, records where the first-person pass's camera was (the lens is
	// placed relative to it from the next frame on).
	void on_first_person_draw();

	// A first-person draw's instance transform (render frame): moved with the
	// frame's weapon motion ([WeaponMotion]) while the player aims in first
	// person; unchanged otherwise.
	void motion_apply(remixapi_Transform& t);

	// The Alt+X scope editor: the equipped scoped gun's entry, edited live.
	namespace editor
	{
		struct Fields
		{
			float eyepiece[3] = {};
			float axis[3] = {};
			float up[3] = {};
			float eyepiece_radius = 0.0f;
			float objective_distance = 0.0f;
			float objective_radius = 0.0f;
			float eye_relief = 0.0f;
			float magnification = 0.0f;
			float field_stop = 0.0f;
			int reticle = 0;
			float reticle_color[3] = {};
			float reticle_radiance = 0.0f;
			float reticle_line_width = 0.0f;
			float tint[3] = {};
			int kind = 0;               // 0 telescopic, 1 reflex
			int aperture = 0;           // 0 circle, 1 rectangle
			float aperture_aspect = 1.0f;
			int vision_mode = 0;        // 0 the weapon's own, 1 none, 2 night, 3 thermal
			bool highlight_hostiles = false;
			// VisionModes: which modes the vision key cycles (none, night,
			// thermal), and the entry's order of them; none allowed = not given.
			bool vision_allowed[3] = {};
			uint8_t vision_order[3] = {};
			int vision_order_count = 0;
			float min_magnification = 0.0f;
			float max_magnification = 0.0f;
			float reticle_accent_color[3] = {};
			float reticle_accent_radiance = 0.0f;
			float edge_glow_color[3] = {};
			float edge_glow_strength = 0.0f;
			float edge_shade = 0.0f;
			float edge_tint[3] = {};
			float night_tint[3] = {};
			float night_gain = 0.0f;
			int node = 0;               // index into node_labels(): 0 the model's root
			int markings = 0;           // bit 0 labels, bit 1 rangefinder
			int reticle_plane = 0;      // 0 second focal plane, 1 first
			bool marking_color_set = false;   // the entry's own marking color, not [Scopes] MarkingColor
			float marking_color[3] = {};
			float marking_radiance = 0.0f;
			float marking_opacity = 0.0f;
			float parallax_m = -1.0f;   // below 0: by kind
			float glare = -1.0f;        // below 0: [Scopes] GlareStrength
			float dirt = -1.0f;         // below 0: [Scopes] DirtStrength
			bool grain = true;          // Grain = off: no film grain on the lens (kept, no control)
			bool combination = false;   // edit the installed mods' own section, not [Lens]
		};

		// The menu's fields; they follow the equipped weapon.
		Fields& fields();

		// A field changed in the menu: the weapon's entry takes the fields
		// (in memory; the lens follows at once).
		void changed();

		// Writes the weapon's file.
		void save();

		// Reads every lens file again (unsaved edits are dropped).
		void reload();

		// One line on what the editor edits, and a counter that moves when
		// the line changes.
		const char* status();
		uint32_t status_generation();

		// The named nodes of the equipped weapon's model for the Node choice:
		// "(root)" first. Their generation moves with the list (and with the
		// status line).
		const char* const* node_labels(uint32_t& count);
	}
}
