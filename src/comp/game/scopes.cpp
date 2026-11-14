#include "std_include.hpp"
#include "scopes.hpp"
#include "scope_data.hpp"

#include "api_camera.hpp"
#include "nvse_link.hpp"
#include "api_common.hpp"
#include "call_chain.hpp"
#include "scene_graph.hpp"
#include "session_files.hpp"
#include "stable_origin.hpp"
#include "structs.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/fnv_exe.hpp"

namespace comp::game::scopes
{
	namespace
	{
		// The message a bridge client that forwards remixapi_CameraInfoLensEXT
		// with its accent, edge and night-tint fields carries in its read-only
		// data. The image is searched, not the log, so a message written only
		// at first use is found before the first lens is sent.
		constexpr const char* kLensMarker = "Forwarding CameraInfoLensEXT";
		constexpr const char* kLensMarkerFields = "(markings)";
		constexpr const char* kLensMarkerTexture = "(reticle texture)";
		constexpr const char* kLensMarkerMarkingColor = "(marking color)";
		constexpr const char* kLensMarkerGlass = "(parallax, glass)";

		// The lens's id: one lens, always the player's scope.
		constexpr uint64_t kLensId = 0x4E56534350453031ull;   // "NVSCPE01"

		// Engine layout.
		constexpr uint32_t ACTOR_PROCESS_OFF      = 0x68;    // BaseProcess*
		constexpr uint32_t PROCESS_WEAPON_SLOT    = 0x148;   // vtable slot: the equipped weapon's inventory entry
		constexpr uint32_t ENTRY_EXTRA_LIST_OFF   = 0x00;    // tList<ExtraDataList>*: its first node's data
		constexpr uint32_t ENTRY_FORM_OFF         = 0x08;    // the inventory entry's form
		constexpr uint32_t FORM_TYPE_OFF          = 0x04;    // u8
		constexpr uint32_t FORM_ID_OFF            = 0x0C;
		constexpr uint8_t  FORM_TYPE_WEAP         = 0x28;
		constexpr uint32_t WEAP_FLAGS1_OFF        = 0x100;   // u8
		constexpr uint8_t  WEAP_HAS_SCOPE         = 0x04;
		constexpr uint32_t WEAP_SIGHT_FOV_OFF     = 0x110;   // float
		constexpr uint32_t WEAP_PROJECTILE_OFF    = 0x118;   // BGSProjectile*
		constexpr uint32_t WEAP_AMMO_OFF          = 0xA8;    // BGSAmmoForm (+0xA4, its ctor call 0x51D839) +4: the ammo form
		constexpr uint32_t WEAP_FLAGS2_OFF        = 0x12C;   // u32
		constexpr uint32_t WEAP_SCOPE_FROM_MOD    = 0x2000;
		constexpr uint32_t WEAP_SCOPE_NIGHT_VISION = 0x1000;
		constexpr uint32_t WEAP_SCOPE_MODEL_OFF   = 0x1E8;   // TESModel
		// TESObjectSTAT* [8], the first-person models (WEAP's save 0x521A10):
		// WNAM, WNM1, WNM2, WNM4 (slot 3), WNM3 (1+2), WNM6 (2+3), WNM5 (1+3),
		// WNM7; kFpModelIndex maps an installed-mod mask (1, 2, 4) to its slot.
		constexpr uint32_t WEAP_FP_MODELS_OFF     = 0x250;
		constexpr int kFpModelIndex[8] = { 0, 1, 2, 4, 3, 6, 5, 7 };
		constexpr uint32_t STAT_MODEL_PATH_OFF    = 0x34;    // TESModelTextureSwap (+0x30): its path's chars
		constexpr uint8_t  MOD_EFFECT_ZOOM        = 14;
		constexpr uint8_t  kModSlotBits[]         = { 1, 2, 4 };
		constexpr uint32_t PC_THIRD_PERSON_OFF    = 0x64A;   // u8
		constexpr uint32_t PC_FIRST_PERSON_BIPED  = 0x68C;   // the first-person biped (PlayerCharacter::GetBipedAnims 0x950B00)
		constexpr uint32_t PC_SIGHTING_NODE_OFF   = 0xE34;   // NiAVObject* (SetSightingNode 0x8BBBF0)
		constexpr uint32_t BIPED_WEAPON_ITEM_OFF  = 0x7C;    // slot 5's item (slot 0 at +0x2C, 0x10 apart: item, model, object)
		constexpr uint32_t BIPED_WEAPON_NODE_OFF  = 0x84;    // slot 5's object, the weapon model's root node
		constexpr uint32_t AV_WORLD_POS_OFF       = 0x8C;
		constexpr uint32_t SETTING_VALUE_OFF      = 0x04;
		constexpr uint32_t HUD_VISIBILITY_OFF     = 0x1BC;   // u32 mask (0x771700)
		constexpr uint32_t HUD_RETICLE_TILE_OFF   = 0x12C;   // the center reticle tile, visibility bit 0x40
		constexpr uint32_t HUD_RETICLE_BIT        = 0x40;
		constexpr uint32_t TILE_VISIBLE           = 0xFA3;   // the trait 0x771700 sets from the mask
		constexpr uint32_t INPUT_MOUSE_WHEEL_OFF  = 0x1B2C;  // OSInputGlobals: this poll's DIMOUSESTATE lZ
		constexpr uint32_t PL_OBJECTS_OFF         = 0x08;    // ProcessLists: MobileObject** (the objects array's data)
		constexpr uint32_t PL_CAPACITY_OFF        = 0x0C;    // u16, the array's capacity
		constexpr uint32_t PL_BEGIN_OFF           = 0x14;    // u32[4], first index per process level (high, middle-high, ...)
		constexpr uint32_t PL_END_OFF             = 0x24;    // u32[4], end index per level
		constexpr uint32_t ACTOR_LIFE_STATE_OFF   = 0x108;   // u32: 1 dying, 2 dead
		constexpr uint32_t PC_COMPASS_TARGETS_OFF = 0xD48;   // tList<{Actor*, u8 isHostile, u8}>*, the compass's targets (0x966F20)
		constexpr uint32_t REF_RENDER_STATE_OFF   = 0x64;    // TESObjectREFR: its render state; +0x14 the 3D root
		constexpr uint32_t RENDER_STATE_NODE_OFF  = 0x14;
		constexpr uint8_t  FORM_TYPE_CHARACTER    = 0x3B;
		constexpr uint8_t  FORM_TYPE_CREATURE     = 0x3C;
		constexpr int      kMaxTagged             = 512;
		constexpr int      kMaxListSteps          = 512;
		constexpr int      kMaxParentDepth        = 64;
		constexpr uint32_t kTagHeat = static_cast<uint32_t>(REMIXAPI_INSTANCE_CATEGORY_BIT_HEAT_SOURCE);
		constexpr uint32_t kTagHostile = static_cast<uint32_t>(REMIXAPI_INSTANCE_CATEGORY_BIT_HIGHLIGHT_HOSTILE);
		constexpr double kZoomStepPerNotch = 1.1;   // magnification factor per wheel notch (120 units)

		// UpdateFOV zooms only to a target above this.
		constexpr float kNoZoomFov = 5.0f;
		constexpr float kFallbackWorldFov = 75.0f;

		using GetEntry_t = void*(__thiscall*)(void* process);
		using IsAiming_t = bool(__thiscall*)(void* actor);
		using ModelPathLength_t = uint32_t(__thiscall*)(void* model);
		using ItemModEffect_t = bool(__thiscall*)(void* entry, uint32_t effect, float* value1);
		using XlistWeaponMod_t = bool(__thiscall*)(void* xlist, uint32_t bit);
		using Block_t = bool(__thiscall*)(void* self);
		using InCombatWith_t = bool(__thiscall*)(void* actor, void* target);
		using TileGet_t = float(__thiscall*)(void* tile, uint32_t trait);
		using TileSet_t = void(__thiscall*)(void* tile, uint32_t trait, int value);
		using SoundByName_t = void*(__thiscall*)(void* mgr, void* handle, const char* edid, uint32_t flags);
		using SoundPlay_t = bool(__thiscall*)(void* handle, uint32_t arg);
		using SetPov_t = bool(__thiscall*)(void* pc, bool first_person);
		using XInputGetState_t = DWORD(WINAPI*)(DWORD user, void* state);

		// Hooked engine code. Thiscall targets are called as fastcall with
		// an unused EDX; none of them takes stack arguments.
		using Attach_t = void(__cdecl*)(void* model);
		using SetVisible_t = void(__cdecl*)(int on);
		using ModelTest_t = uint32_t(__fastcall*)(void* model, void* edx);
		using WeapTest_t = bool(__fastcall*)(void* weap, void* edx);
		using ActorTest_t = bool(__fastcall*)(void* actor, void* edx);
		using ActorNode_t = void*(__fastcall*)(void* actor, void* edx);
		using Poll_t = void(__fastcall*)(void* osig, void* edx);

		Attach_t g_orig_attach = nullptr;
		SetVisible_t g_orig_set_visible = nullptr;
		WeapTest_t g_orig_use_is = nullptr;
		ModelTest_t g_cull_target = nullptr;
		WeapTest_t g_scope_mod_target = nullptr;
		ActorTest_t g_zoom_target = nullptr;
		ActorNode_t g_hud_sighting_target = nullptr;
		WeapTest_t g_nv_flag_target = nullptr;
		Poll_t g_orig_poll = nullptr;
		ActorTest_t g_sight_block_a_target = nullptr;
		ActorTest_t g_sight_block_b_target = nullptr;

		bool g_hooked = false;
		int g_supported = -1;           // -1 not decided yet
		bool g_texture_supported = false; // the bridge forwards referenceMagnification and reticleTexture
		bool g_marking_color_supported = false; // the bridge forwards markingColor, markingRadiance, markingOpacity
		bool g_glass_supported = false; // the bridge forwards parallaxDistance, glareStrength, dirtStrength
		bool g_pushed_enable = false;
		uint32_t g_logged_form = 0;
		uint8_t g_logged_mods = 0xFF;
		uint64_t g_faults = 0;

		// The aim alignment point: the first-person camera setup reads only
		// its world position (+0x8C), as it does a ##SightingNode's.
		alignas(16) uint8_t g_sight[0xA0] = {};
		UINT g_sight_frame = 0xFFFFFFFFu;
		// The alignment's eye offset in the main camera's frame, as it follows
		// the entry's eye point, and when it last moved (QueryPerformanceCounter).
		double g_follow[3] = {};
		LONGLONG g_follow_ticks = 0;

		// The aim-in, from the first aimed frame. It follows the angle
		// between the scope's axis and the view: the weight w goes from 0 at
		// kAimFarDegrees to 1 at kAimNearDegrees (smoothstep, at most
		// kAimWeightRate per second), and the eye goes from the camera (where
		// the game puts it) to the eye point by w, so the gun comes up as the
		// game animates it and the eye slides onto the eyepiece as it lines
		// up. The angle the raise settles at (it holds still for
		// kAimSettleSeconds, or kAimInMax has passed) counts as lined up. The
		// lens itself is the same throughout; the aim-only features (vision,
		// highlighting, rangefinder, peripheral blur, the shot) start once w
		// is 1 at a settled angle (locked).
		struct AimIn
		{
			bool active = false;        // the aim-in runs (not locked yet)
			LONGLONG start = 0;         // QueryPerformanceCounter at the aim's first frame
			LONGLONG ticks = 0;         // at the last update
			double w = 0.0;
			double angle = 0.0;         // degrees, this frame
			double last_angle = -1.0;
			double still = 0.0;         // seconds the angle has held still
			bool settled = false;
		};
		AimIn g_aim_in;
		constexpr double kAimFarDegrees = 9.0;
		constexpr double kAimNearDegrees = 1.25;
		constexpr double kAimWeightRate = 5.0;      // w per second at most
		constexpr double kAimSettleRate = 3.0;      // degrees per second the axis turns at most when settled
		constexpr double kAimSettleSeconds = 0.08;
		constexpr double kAimSettleMin = 0.1;       // seconds into the aim before it can settle
		constexpr double kAimInMax = 1.0;

		double seconds_since(LONGLONG t)
		{
			LARGE_INTEGER now = {}, freq = {};
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			if (freq.QuadPart <= 0) return 1e3;
			return static_cast<double>(now.QuadPart - t) / static_cast<double>(freq.QuadPart);
		}

		double aim_in_elapsed()
		{
			return seconds_since(g_aim_in.start);
		}

		void aim_in_begin()
		{
			LARGE_INTEGER now = {};
			QueryPerformanceCounter(&now);
			g_aim_in = {};
			g_aim_in.active = true;
			g_aim_in.start = now.QuadPart;
			g_aim_in.ticks = now.QuadPart;
		}

		// This frame's aim-in step at `angle` (degrees between the scope's
		// axis and the view); the eye's weight w.
		double aim_in_update(double angle)
		{
			AimIn& a = g_aim_in;
			a.angle = angle;
			if (!a.active) return 1.0;
			LARGE_INTEGER now = {};
			QueryPerformanceCounter(&now);
			const double dt = std::clamp(seconds_since(a.ticks), 0.0, 0.1);
			a.ticks = now.QuadPart;
			const double elapsed = aim_in_elapsed();
			const double rate = (a.last_angle >= 0.0 && dt > 1e-4) ? std::fabs(angle - a.last_angle) / dt : 1e9;
			a.last_angle = angle;
			if (elapsed >= kAimSettleMin && angle < kAimFarDegrees * 1.5 && rate < kAimSettleRate) a.still += dt;
			else a.still = 0.0;
			if (a.still >= kAimSettleSeconds || elapsed >= kAimInMax) a.settled = true;
			double target = 1.0;
			if (!a.settled)
			{
				const double r = std::clamp((kAimFarDegrees - angle) / (kAimFarDegrees - kAimNearDegrees), 0.0, 1.0);
				target = r * r * (3.0 - 2.0 * r);
			}
			const double step = kAimWeightRate * dt;
			a.w = std::clamp(target, a.w - step, a.w + step);
			if (a.settled && a.w >= 1.0)
			{
				a.w = 1.0;
				a.active = false;
			}
			return a.w;
		}

		// The frame the last lens went out, and that lens's state across
		// frames: the weapon it is for, its zoom (the mouse wheel moves it),
		// its vision mode (the vision key cycles it), the wheel's notches
		// since the last lens.
		UINT g_lens_frame = 0xFFFFFFFFu;
		// The frames the last lens went out aimed, and aimed and locked (the
		// aim-in done): input and the crosshair follow the first, the aim-only
		// features the second.
		UINT g_lens_aimed_frame = 0xFFFFFFFFu;
		UINT g_lens_locked_frame = 0xFFFFFFFFu;
		uint32_t g_state_form = 0;
		uint8_t g_state_mods = 0xFF;
		float g_zoom = 1.0f;
		int g_vision = 0;
		int g_vision_default = 0;
		// The modes the vision key cycles for the lens up (lens()).
		uint8_t g_vision_list[3] = { 0, 0, 0 };
		int g_vision_count = 0;
		bool g_vision_key_down = false;
		long g_wheel = 0;
		// The wheel / d-pad notches since the last lens that go to the
		// night-vision gain (with the brightness modifier held), and each
		// weapon's gain factor for the session.
		long g_gain_wheel = 0;
		std::unordered_map<uint32_t, float> g_gain;
		constexpr double kGainStepPerNotch = 1.25;
		constexpr float kGainMin = 0.25f, kGainMax = 4.0f;
		bool g_tags_wanted = false;
		bool g_reticle_hidden = false;
		bool g_reticle_logged = false;
		bool g_pad_vision = false;          // the gamepad's vision combo went down
		WORD g_pad_prev[4] = {};
		bool g_forced_first_person = false;
		float g_blur_pushed = -1.0f;

		// XInputGetState as the game imports it, and the import slot.
		XInputGetState_t g_orig_xinput = nullptr;
		uintptr_t g_xinput_slot = 0;

		constexpr WORD kPadUp = 0x0001, kPadDown = 0x0002;
		constexpr uint32_t XINPUT_BUTTONS_OFF = 0x04;   // XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD wButtons
		constexpr const char* kSoundZoom = "UIMenuPrevNext";
		constexpr const char* kSoundVisionOn = "UIPipBoyLightOn";
		constexpr const char* kSoundVisionOff = "UIPipBoyLightOff";
		constexpr uint32_t kSoundFlags = 0x121;          // as the game's own UI sounds (0x717280, 0x9675EB)
		constexpr float kVariableZoomHigh = 3.0f;   // VariableZoomAll: up to three times the default magnification

		const shared::common::config::scopes_settings& cfg()
		{
			return shared::common::config::get().scopes;
		}

		void log(const std::string& msg, shared::common::LOG_TYPE type = shared::common::LOG_TYPE::LOG_TYPE_DEFAULT,
			bool notice = false)
		{
			shared::common::log("Scopes", msg, type, notice);
		}

		// The bridge client forwards the lens. Decided once the client and the
		// Remix API are up.
		bool supported()
		{
			if (g_supported >= 0) return g_supported == 1;
			if (!shared::globals::d3d9_chain_module || !shared::common::remix_api::is_initialized()) return false;
			const bool found = api::bridge_client_has_line(kLensMarker, kLensMarkerFields);
			const bool ok = found && shared::common::remix_api::get().m_bridge.SetupCamera;
			g_supported = ok ? 1 : 0;
			g_texture_supported = ok && api::bridge_client_has_line(kLensMarker, kLensMarkerTexture);
			g_marking_color_supported = ok && api::bridge_client_has_line(kLensMarker, kLensMarkerMarkingColor);
			g_glass_supported = ok && api::bridge_client_has_line(kLensMarker, kLensMarkerGlass);
			if (ok)
				log(g_texture_supported
					? "the bridge client forwards reticle textures and first-focal-plane reticles"
					: "the bridge client does not forward reticle textures (older runtime): entries keep their drawn "
					  "reticle in the second focal plane");
			log(ok
				? std::format("the bridge client forwards CameraInfoLensEXT with its markings field -- weapons with a "
					"lens entry aim through a path-traced lens (entries in {})", session_files::utf8(data::folder()))
				: "the bridge client does NOT forward CameraInfoLensEXT with its markings field (older runtime) -- "
				  "scopes are the game's own",
				ok ? shared::common::LOG_TYPE::LOG_TYPE_STATUS : shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
			return ok;
		}

		bool active()
		{
			return g_hooked && cfg().path_traced && shared::common::config::get().remix_api.fallback_camera &&
				supported();
		}

		// ---- Engine reads (no C++ objects: they run under __try) ----------------

		bool is_weapon(const void* form)
		{
			return is_live_object(form) &&
				*reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(form) + FORM_TYPE_OFF) == FORM_TYPE_WEAP;
		}

		// A gun with a scope: HasScope, a scope model path and a projectile.
		// Weapons without a projectile are not guns (optic_unguarded).
		bool scoped_gun_unguarded(const uint8_t* weap)
		{
			if (!is_weapon(weap)) return false;
			if (!(weap[WEAP_FLAGS1_OFF] & WEAP_HAS_SCOPE)) return false;
			if (!*reinterpret_cast<void* const*>(weap + WEAP_PROJECTILE_OFF)) return false;
			const auto len = reinterpret_cast<ModelPathLength_t>(fnv_exe::addr().model_path_length);
			return len(const_cast<uint8_t*>(weap + WEAP_SCOPE_MODEL_OFF)) > 0;
		}

		// An optic: a scope model and no projectile and no ammo (binoculars).
		// A camera has ammo (NVCamera's film), so it is not one; a weapon with
		// a projectile is a gun.
		bool optic_unguarded(const uint8_t* weap)
		{
			if (!is_weapon(weap)) return false;
			if (!(weap[WEAP_FLAGS1_OFF] & WEAP_HAS_SCOPE)) return false;
			if (*reinterpret_cast<void* const*>(weap + WEAP_PROJECTILE_OFF)) return false;
			if (*reinterpret_cast<void* const*>(weap + WEAP_AMMO_OFF)) return false;
			const auto len = reinterpret_cast<ModelPathLength_t>(fnv_exe::addr().model_path_length);
			return len(const_cast<uint8_t*>(weap + WEAP_SCOPE_MODEL_OFF)) > 0;
		}

		bool optic(const void* weap)
		{
			__try { return optic_unguarded(static_cast<const uint8_t*>(weap)); }
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
		}

		bool scoped_gun(const void* weap, uint32_t& form_id)
		{
			__try
			{
				const auto* w = static_cast<const uint8_t*>(weap);
				if (!scoped_gun_unguarded(w)) return false;
				form_id = *reinterpret_cast<const uint32_t*>(w + FORM_ID_OFF);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
		}

		// The player and the weapon they hold.
		struct Held
		{
			uint8_t* player = nullptr;
			const uint8_t* weap = nullptr;   // a scoped gun or an optic, else null
			const uint8_t* equipped = nullptr;   // the equipped weapon (any WEAP), else null
			bool optic = false;              // binoculars (optic_unguarded): two camera-attached lenses
			uint32_t form_id = 0;
			uint8_t mods = 0;                // installed weapon mod slots (bits 1, 2, 4)
			bool shown = false;              // vanilla would show its scope
			bool aiming = false;
			bool third_person = false;
			bool night_vision = false;       // flags2 0x1000 ScopeHasNightVision
			float fov = 0.0f;                // the vanilla zoom target: |sightFOV| minus an installed zoom mod's value1
		};

		bool held_unguarded(Held& h)
		{
			const auto& a = fnv_exe::addr();
			auto* player = *reinterpret_cast<uint8_t* const*>(a.player);
			if (!is_live_object(player)) return false;
			h.player = player;
			h.third_person = player[PC_THIRD_PERSON_OFF] != 0;
			h.aiming = reinterpret_cast<IsAiming_t>(a.actor_is_aiming)(player);

			void* process = *reinterpret_cast<void* const*>(player + ACTOR_PROCESS_OFF);
			if (!is_live_object(process)) return true;
			const auto* vt = *reinterpret_cast<const uintptr_t* const*>(process);
			void* entry = reinterpret_cast<GetEntry_t>(vt[PROCESS_WEAPON_SLOT / 4])(process);
			if (!is_valid_ptr(entry)) return true;
			const auto* weap = *reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(entry) + ENTRY_FORM_OFF);
			if (is_weapon(weap)) h.equipped = weap;
			const bool gun = scoped_gun_unguarded(weap);
			if (!gun && !optic_unguarded(weap)) return true;
			h.optic = !gun;
			h.weap = weap;
			h.form_id = *reinterpret_cast<const uint32_t*>(weap + FORM_ID_OFF);

			void* list = *reinterpret_cast<void* const*>(static_cast<uint8_t*>(entry) + ENTRY_EXTRA_LIST_OFF);
			void* xlist = is_valid_ptr(list) ? *reinterpret_cast<void* const*>(list) : nullptr;
			if (is_valid_ptr(xlist))
				for (const uint8_t bit : kModSlotBits)
					if (reinterpret_cast<XlistWeaponMod_t>(a.xlist_weapon_mod)(xlist, bit))
						h.mods = static_cast<uint8_t>(h.mods | bit);

			// UpdateFOV subtracts an installed zoom mod's value1 whatever
			// supplies the scope; the scope itself needs that mod when it
			// comes from one.
			float value1 = 0.0f;
			const bool zoom_mod = reinterpret_cast<ItemModEffect_t>(a.item_mod_effect)(entry, MOD_EFFECT_ZOOM, &value1);
			const uint32_t flags2 = *reinterpret_cast<const uint32_t*>(weap + WEAP_FLAGS2_OFF);
			h.shown = !(flags2 & WEAP_SCOPE_FROM_MOD) || zoom_mod;
			h.night_vision = (flags2 & WEAP_SCOPE_NIGHT_VISION) != 0;
			h.fov = std::fabs(*reinterpret_cast<const float*>(weap + WEAP_SIGHT_FOV_OFF)) - (zoom_mod ? value1 : 0.0f);
			return true;
		}

		bool held(Held& h)
		{
			__try { return held_unguarded(h); }
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; h = {}; return false; }
		}

		// The first-person weapon node's world transform: rotation (row-major,
		// maps column vectors), translation, scale.
		struct Pose
		{
			float rot[9];
			float pos[3];
			float scale;
		};

		// The first-person weapon model's root: biped slot 5's object while
		// its item is the held weapon. 0 otherwise.
		uintptr_t weapon_root(const Held& h)
		{
			__try
			{
				const auto* bip = *reinterpret_cast<const uint8_t* const*>(h.player + PC_FIRST_PERSON_BIPED);
				if (!is_valid_ptr(bip)) return 0;
				if (*reinterpret_cast<const void* const*>(bip + BIPED_WEAPON_ITEM_OFF) != h.weap) return 0;
				void* obj = *reinterpret_cast<void* const*>(bip + BIPED_WEAPON_NODE_OFF);
				return is_live_object(obj) ? reinterpret_cast<uintptr_t>(obj) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return 0; }
		}

		// A node of the weapon model by name (NiObjectNET +0x08), found once per
		// model root and name; 0 when the model has none.
		constexpr uint32_t OBJ_NAME_OFF = 0x08;
		constexpr int kMaxNodeVisits = 4096;
		uintptr_t g_named_root = 0;
		char g_named_name[64] = {};
		uintptr_t g_named_node = 0;
		bool g_named_logged = false;

		bool node_name(uintptr_t obj, char (&out)[64])
		{
			out[0] = 0;
			uint32_t str = 0;
			if (!scene_graph::read_u32(obj + OBJ_NAME_OFF, str) || str < 0x10000) return false;
			if (!scene_graph::read_block(str, out, sizeof(out) - 1)) return false;
			out[sizeof(out) - 1] = 0;
			return true;
		}

		uintptr_t named_node(uintptr_t root, const char* name)
		{
			if (root == g_named_root && std::strcmp(name, g_named_name) == 0) return g_named_node;
			g_named_root = root;
			strncpy_s(g_named_name, name, _TRUNCATE);
			g_named_node = 0;
			std::vector<uintptr_t> open = { root };
			uintptr_t kids[64];
			for (int visits = 0; !open.empty() && visits < kMaxNodeVisits; visits++)
			{
				const uintptr_t obj = open.back();
				open.pop_back();
				char nm[64];
				if (node_name(obj, nm) && std::strcmp(nm, name) == 0) { g_named_node = obj; break; }
				const scene_graph::ClassInfo* ci = scene_graph::class_of(obj);
				if (!ci || !ci->node) continue;
				scene_graph::NodeChildren info;
				if (!scene_graph::read_children(obj, kids, 64, info)) continue;
				for (uint32_t i = 0; i < info.read; i++)
					if (kids[i]) open.push_back(kids[i]);
			}
			if (!g_named_logged || !g_named_node)
			{
				g_named_logged = true;
				log(g_named_node ? std::format("lens on the weapon model's node '{}'", name)
					: std::format("the weapon model has no node '{}': the lens uses its root", name),
					g_named_node ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
			return g_named_node;
		}

		// The world transform of the node the entry's values are in: its Node,
		// else the weapon model's root.
		bool weapon_pose(const Held& h, const char* node_label, Pose& out)
		{
			uintptr_t node = weapon_root(h);
			if (!node) return false;
			if (node_label && node_label[0])
				if (const uintptr_t named = named_node(node, node_label)) node = named;
			float p[scene_graph::kPoseFloats];
			if (!scene_graph::read_pose(node, p)) return false;
			std::memcpy(out.rot, p, sizeof(out.rot));
			std::memcpy(out.pos, p + 9, sizeof(out.pos));
			out.scale = p[12];
			for (const float f : p)
				if (!std::isfinite(f)) return false;
			return out.scale > 1e-4f;
		}

		// Points PlayerCharacter +0xE34 at the alignment point, or clears it
		// when it holds that point. Returns the world position the
		// first-person camera takes this frame: the node +0xE34 holds unless
		// the engine's own tests rule it out, else null (the main camera).
		const float* set_sighting(uint8_t* player, bool ours)
		{
			const auto& a = fnv_exe::addr();
			__try
			{
				auto* slot = reinterpret_cast<uint8_t**>(player + PC_SIGHTING_NODE_OFF);
				if (ours) *slot = g_sight;
				else if (*slot == g_sight) *slot = nullptr;
				const uint8_t* node = *slot;
				if (!node) return nullptr;
				if (reinterpret_cast<Block_t>(a.sighting_anim_block)(player)) return nullptr;
				if (reinterpret_cast<Block_t>(a.sighting_cam_block)(reinterpret_cast<void*>(a.camera_state))) return nullptr;
				return reinterpret_cast<const float*>(node + AV_WORLD_POS_OFF);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return nullptr; }
		}

		// ---- Lens weapons --------------------------------------------------------

		// The held weapon's lens for its installed mods.
		// Binoculars: no preset; the look of the vanilla scopes, no reticle,
		// a wide field. Their places come from binocular_place.
		constexpr double kBinocularEyeReliefCm = 2.0;
		constexpr double kBinocularObjectiveCm = 10.0;
		constexpr double kBinocularObjectiveRadiusCm = 2.5;
		constexpr double kBinocularFill = 0.42;     // each barrel's radius, of half the screen height
		constexpr float kBinocularAspect = 0.85f;   // each barrel's center from the view axis, in barrel radii
		constexpr float kBinocularFieldStop = 25.0f;
		constexpr double kCmPerUnit = 1.428571;     // FNV units are about 1.43 cm
		constexpr double kParallaxDefaultM = 100.0; // telescopic scopes without ParallaxDistance
		constexpr int kAimView = 1;                 // [Scopes] AimMode view; 0 realistic

		// The shot hook is in (AimMode realistic needs it).
		bool g_shot_hooked = false;

		// [Scopes] AimMode, view when the shot hook is not in.
		int aim_mode()
		{
			return g_shot_hooked ? cfg().aim_mode : kAimView;
		}


		// The model path of the weapon's first-person model at `index` into
		// `buf`; false when there is none.
		bool fp_model_unguarded(const uint8_t* weap, int index, char* buf, size_t n)
		{
			const auto* stat = *reinterpret_cast<const uint8_t* const*>(weap + WEAP_FP_MODELS_OFF + 4u * static_cast<uint32_t>(index));
			if (!is_valid_ptr(stat)) return false;
			const char* p = *reinterpret_cast<const char* const*>(stat + STAT_MODEL_PATH_OFF);
			if (!is_valid_ptr(p)) return false;
			size_t i = 0;
			for (; i + 1 < n && p[i]; i++) buf[i] = p[i];
			buf[i] = 0;
			return i > 0;
		}

		bool fp_model(const uint8_t* weap, int index, char* buf, size_t n)
		{
			__try { return fp_model_unguarded(weap, index, buf, n); }
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
		}

		// The first-person model the game draws the weapon with for `mods`
		// (normalized): that combination's, else the weapon's own. 0 when
		// neither can be read.
		int fp_models(const uint8_t* weap, uint8_t mods, std::string out[2])
		{
			if (!weap) return 0;
			char buf[260];
			const int combo = kFpModelIndex[mods & 7];
			if ((combo && fp_model(weap, combo, buf, sizeof(buf))) || fp_model(weap, 0, buf, sizeof(buf)))
			{
				// Only a model path counts; anything else reads as unknown (no check).
				out[0] = data::normalize_model(buf);
				const std::string& m = out[0];
				return m.size() > 4 && m.compare(m.size() - 4, 4, ".nif") == 0 ? 1 : 0;
			}
			return 0;
		}

		std::string mods_text(uint8_t mods);

		// Weapons (form ID, mods) whose entry was turned down for its Models,
		// logged once each.
		std::unordered_set<uint64_t> g_model_logged;

		bool held_lens(const Held& h, data::Lens* lens = nullptr, int* section = nullptr)
		{
			if (!h.weap || !h.shown) return false;
			if (h.optic)
			{
				if (lens)
				{
					*lens = data::Lens{};
					lens->reticle = 0;
					lens->aperture = static_cast<int>(REMIXAPI_CAMERA_LENS_APERTURE_BINOCULAR);
					lens->aperture_aspect = kBinocularAspect;
					lens->field_stop = kBinocularFieldStop;
					lens->edge_shade = 0.3f;
				}
				if (section) *section = data::kBase;
				return true;
			}
			data::Lens l;
			if (!data::resolve(h.form_id, h.mods, l, section)) return false;
			// The entry's Models: a weapon drawn with another first-person model
			// (a mesh replacer's record edit) keeps the game's own scope.
			if (l.models[0])
			{
				std::string drawn[2];
				const int n = fp_models(h.weap, h.mods, drawn);
				bool listed = n == 0;
				for (int i = 0; i < n && !listed; i++) listed = data::lists_model(l, drawn[i]);
				if (!listed)
				{
					const uint64_t key = static_cast<uint64_t>(h.form_id) | static_cast<uint64_t>(h.mods) << 32;
					if (g_model_logged.insert(key).second)
					{
						const std::string* edid = data::editor_id(h.form_id);
						log(std::format("lens for {} (mods {}) not used: its first-person model {} is not among the "
							"entry's Models ({}) in {}; the game's own scope stays", edid ? *edid : std::format("{:08X}",
							h.form_id), mods_text(h.mods), drawn[0], l.models, session_files::utf8(data::source(h.form_id))),
							shared::common::LOG_TYPE::LOG_TYPE_WARN);
					}
					return false;
				}
			}
			if (lens) *lens = l;
			return true;
		}

		// A lens weapon: the player's, with its installed mods; any other
		// scoped gun by whether its file has an entry at all.
		bool lens_weapon(const void* weap)
		{
			Held h;
			if (held(h) && h.weap && h.weap == weap) return held_lens(h);
			uint32_t form_id = 0;
			return (scoped_gun(weap, form_id) && data::has_any(form_id)) || optic(weap);
		}

		// A lens weapon that is a gun (the ironsights and cull hooks leave
		// optics to the game).
		bool gun_lens_weapon(const void* weap)
		{
			return lens_weapon(weap) && !optic(weap);
		}

		bool player_lens()
		{
			Held h;
			return held(h) && held_lens(h);
		}

		float default_world_fov()
		{
			const uintptr_t setting = fnv_exe::addr().default_world_fov;
			if (!setting) return kFallbackWorldFov;
			const float v = *reinterpret_cast<const float*>(setting + SETTING_VALUE_OFF);
			return (std::isfinite(v) && v > 1.0f && v < 179.0f) ? v : kFallbackWorldFov;
		}

		// ---- Hooks ---------------------------------------------------------------

		void __cdecl attach_hk(void* model)
		{
			// Both equip paths pass the weapon's own scope model (weapon +0x1E8).
			if (model && active() && lens_weapon(static_cast<uint8_t*>(model) - WEAP_SCOPE_MODEL_OFF))
				return g_orig_attach(nullptr);
			g_orig_attach(model);
		}

		void __cdecl set_visible_hk(int on)
		{
			if ((on & 0xFF) && active() && player_lens()) on = 0;
			g_orig_set_visible(on);
		}

		// SetAiming has set the aiming state when it asks; on aim end the
		// engine's own uncull runs.
		uint32_t __fastcall aim_cull_model_hk(void* model, void* edx)
		{
			if (model && active())
			{
				Held h;
				if (held(h) && h.aiming && !h.optic && h.weap == static_cast<uint8_t*>(model) - WEAP_SCOPE_MODEL_OFF &&
					held_lens(h))
					return 0;
			}
			return g_cull_target(model, edx);
		}

		bool __fastcall aim_scope_mod_hk(void* weap, void* edx)
		{
			if (active() && gun_lens_weapon(weap)) return false;
			return g_scope_mod_target(weap, edx);
		}

		bool __fastcall use_1st_is_hk(void* weap, void* edx)
		{
			const bool own = g_orig_use_is(weap, edx);
			if (own || !active()) return own;
			Held h;
			return (held(h) && !h.optic && h.weap == weap && held_lens(h)) ? true : own;
		}

		bool __fastcall zoom_aiming_hk(void* actor, void* edx)
		{
			const bool aiming = g_zoom_target(actor, edx);
			if (!aiming || !active()) return aiming;
			return player_lens() ? false : aiming;
		}

		// The HUD's reticle state asks for the sighting node only to hide the
		// center reticle (visibility bit 0x40) when one is set. It is asked
		// again on every HUD mode change, SetAiming's aim start and end among
		// them, and at aim start the aiming state is already set. While a
		// lens weapon is aimed in first person the answer is a node (the
		// alignment point, never read here), so the crosshair hides with or
		// without AlignAim.
		void* __fastcall hud_sighting_hk(void* actor, void* edx)
		{
			void* node = g_hud_sighting_target(actor, edx);
			if (node || !active()) return node;
			Held h;
			return (held(h) && h.aiming && !h.third_person && held_lens(h)) ? static_cast<void*>(g_sight) : nullptr;
		}

		// The player update applies the scope night-vision IMAD while aiming a
		// weapon whose scope has night vision: a lens weapon's lens does it.
		bool __fastcall nv_flag_hk(void* weap, void* edx)
		{
			if (active() && lens_weapon(weap)) return false;
			return g_nv_flag_target(weap, edx);
		}

		// The lens went out this frame or the one before.
		bool lens_recent()
		{
			const UINT now = api::frame();
			return g_lens_frame == now || g_lens_frame == now - 1;
		}

		// The lens went out aimed this frame or the one before.
		bool lens_aimed_recent()
		{
			const UINT now = api::frame();
			return g_lens_aimed_frame == now || g_lens_aimed_frame == now - 1;
		}

		// [Scopes] LensAlways: the held weapon's lens also goes out at the hip,
		// while the game is in play with no menu (VATS among them) and the
		// Pip-Boy closed. Without the NVSE link's facts, only while aimed.
		bool hip_lens_allowed()
		{
			if (!cfg().lens_always) return false;
			const FnvRemixFacts& f = nvse_link::facts();
			return f.ready && !f.loading && f.game_state == FNVR_GS_IN_GAME && f.pipboy == FNVR_PB_CLOSED &&
				f.top_menu == 0;
		}

		// The first-person camera setup skips the sighting node while the
		// player's process reports state 9 or 0xE..0x11 (0x8A8870, process
		// vtable +0x3E4, the sit / sleep state the camera code also tests for
		// furniture). While a lens weapon is aimed in first person the
		// alignment point is used anyway; the camera state's own block
		// (0x525430) still applies.
		bool lens_aim_now()
		{
			if (!lens_recent() || !active() || !cfg().align_aim) return false;
			Held h;
			return held(h) && h.aiming && !h.third_person && !h.optic && held_lens(h);
		}

		bool __fastcall sight_block_a_hk(void* pc, void* edx)
		{
			const bool block = g_sight_block_a_target(pc, edx);
			return block && !lens_aim_now();
		}

		bool __fastcall sight_block_b_hk(void* pc, void* edx)
		{
			const bool block = g_sight_block_b_target(pc, edx);
			return block && !lens_aim_now();
		}

		// While a lens is up, d-pad up / down zoom it and the vision buttons
		// switch its vision mode; the game sees none of those buttons then.
		DWORD WINAPI xinput_hk(DWORD user, void* state)
		{
			const DWORD rc = g_orig_xinput(user, state);
			if (rc != ERROR_SUCCESS || !state || user >= 4) return rc;
			__try
			{
				auto* buttons = reinterpret_cast<WORD*>(static_cast<uint8_t*>(state) + XINPUT_BUTTONS_OFF);
				const WORD now = *buttons;
				const WORD prev = g_pad_prev[user];
				g_pad_prev[user] = now;
				if (!lens_aimed_recent() || !active()) return rc;
				const WORD vision = static_cast<WORD>(cfg().gamepad_vision_buttons);
				const WORD pressed = static_cast<WORD>(now & ~prev);
				const WORD bright = static_cast<WORD>(cfg().gamepad_brightness_buttons);
				long& notches = (bright && (now & bright) == bright) ? g_gain_wheel : g_wheel;
				if (pressed & kPadUp) notches += 120;
				if (pressed & kPadDown) notches -= 120;
				if (vision && (now & vision) == vision && (prev & vision) != vision) g_pad_vision = true;
				*buttons = static_cast<WORD>(now & ~(kPadUp | kPadDown | vision));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
			return rc;
		}

		// Points the game's XInputGetState import at xinput_hk. The slot is
		// the one in FalloutNV.exe's import table that holds the address
		// XINPUT1_3.dll exports as ordinal 2. False when the game imports no
		// XInput or the slot cannot be written.
		bool hook_xinput()
		{
			const HMODULE xinput = GetModuleHandleA("xinput1_3.dll");
			const auto want = xinput ? reinterpret_cast<uintptr_t>(GetProcAddress(xinput, MAKEINTRESOURCEA(2))) : 0;
			if (!want) return false;
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
			const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!dir.VirtualAddress) return false;
			for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++)
			{
				if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "xinput1_3.dll") != 0) continue;
				for (auto* t = reinterpret_cast<uintptr_t*>(base + d->FirstThunk); *t; t++)
				{
					if (*t != want) continue;
					DWORD old = 0;
					if (!VirtualProtect(t, sizeof(*t), PAGE_READWRITE, &old)) return false;
					g_orig_xinput = reinterpret_cast<XInputGetState_t>(*t);
					*t = reinterpret_cast<uintptr_t>(&xinput_hk);
					VirtualProtect(t, sizeof(*t), old, &old);
					g_xinput_slot = reinterpret_cast<uintptr_t>(t);
					return true;
				}
			}
			return false;
		}

		// One of the game's sounds by editor ID, as its UI sounds play.
		void play_sound(const char* edid)
		{
			if (!cfg().sounds) return;
			const auto& a = fnv_exe::addr();
			__try
			{
				void* mgr = *reinterpret_cast<void* const*>(a.audio_manager);
				if (!is_live_object(mgr)) return;
				alignas(8) uint8_t handle[16] = {};
				*reinterpret_cast<uint32_t*>(handle) = 0xFFFFFFFFu;
				reinterpret_cast<SoundByName_t>(a.sound_by_name)(mgr, handle, edid, kSoundFlags);
				reinterpret_cast<SoundPlay_t>(a.sound_play)(handle, 0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
		}

		// The engine's first / third person switch, as the POV key uses it.
		void set_first_person(uint8_t* player, bool first_person)
		{
			__try { reinterpret_cast<SetPov_t>(fnv_exe::addr().player_set_pov)(player, first_person); }
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
		}

		// While a lens is up, the mouse wheel zooms it: this poll's wheel delta
		// is taken and the game sees none.
		void __fastcall poll_hk(void* osig, void* edx)
		{
			g_orig_poll(osig, edx);
			if (!osig || !lens_aimed_recent() || !active()) return;
			const int bright_vk = cfg().brightness_key;
			const bool bright = bright_vk > 0 && bright_vk < 0xFF && (GetAsyncKeyState(bright_vk) & 0x8000) != 0;
			__try
			{
				auto* b = static_cast<uint8_t*>(osig);
				auto* wheel = reinterpret_cast<long*>(b + INPUT_MOUSE_WHEEL_OFF);
				(bright ? g_gain_wheel : g_wheel) += *wheel;
				*wheel = 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
		}

		constexpr const char* kIfReplaced = "path-traced scopes stay consistent only while that code still calls the "
			"wrapper's hook";

		bool patch_call(const fnv_exe::Code& site, const call_chain::Site& found, uintptr_t ours, const char* what)
		{
			if (!call_chain::patch_rel32(site.addr, 0xE8, ours))
			{
				log(std::format("0x{:X} could not be made writable ({})", site.addr, what),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}
			call_chain::track(call_chain::Kind::Call, site.addr, ours, found, "Scopes", what, kIfReplaced);
			return true;
		}

		// ---- Weapon space -> world -> render frame --------------------------------

		struct V3
		{
			double x, y, z;
		};

		V3 v3(const float* p) { return { p[0], p[1], p[2] }; }
		V3 add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		V3 sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		V3 mul(V3 a, double k) { return { a.x * k, a.y * k, a.z * k }; }
		double len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

		V3 rotate(const Pose& p, V3 v)
		{
			const float* r = p.rot;
			return { r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z,
				r[6] * v.x + r[7] * v.y + r[8] * v.z };
		}

		V3 to_world(const Pose& p, V3 v)
		{
			return add(rotate(p, mul(v, p.scale)), v3(p.pos));
		}

		// The transpose of the pose's rotation: world directions into its frame.
		V3 unrotate(const Pose& p, V3 v)
		{
			const float* r = p.rot;
			return { r[0] * v.x + r[3] * v.y + r[6] * v.z, r[1] * v.x + r[4] * v.y + r[7] * v.z,
				r[2] * v.x + r[5] * v.y + r[8] * v.z };
		}

		// The main camera node's world pose (fnv_exe main_camera), the frame the
		// head moves the first-person view in.
		bool main_camera_pose(Pose& out)
		{
			uintptr_t node = 0;
			__try
			{
				void* obj = *reinterpret_cast<void* const*>(fnv_exe::addr().main_camera);
				if (!is_live_object(obj)) return false;
				node = reinterpret_cast<uintptr_t>(obj);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
			float p[scene_graph::kPoseFloats];
			if (!scene_graph::read_pose(node, p)) return false;
			for (const float f : p)
				if (!std::isfinite(f)) return false;
			std::memcpy(out.rot, p, sizeof(out.rot));
			std::memcpy(out.pos, p + 9, sizeof(out.pos));
			out.scale = p[12];
			return true;
		}

		// [Scopes] AimDebugLog: this frame's alignment values (camera frame, game
		// units), logged for the first kAimDebugSeconds of each aim.
		struct AimDebug
		{
			bool aligned = false;
			double weight = 1.0;                // the eye's weight toward the eye point (aim-in)
			bool pending = false;               // an aimed lens went out: lens_sent logs the line
			V3 base = { 0.0, 0.0, 0.0 };        // the posed eye point from the camera
			V3 eye_error = { 0.0, 0.0, 0.0 };   // the eye minus the moved eye point
			V3 follow = { 0.0, 0.0, 0.0 };      // the followed motion offset
			V3 last_cam = { 0.0, 0.0, 0.0 };
			bool have_cam = false;
		};
		AimDebug g_aim_dbg;
		constexpr double kAimDebugSeconds = 2.0;

		// The alignment point this frame, from the eye point of the gun as the
		// game poses it (`eye`) and as the weapon motion moves it (`moved`).
		// The eye sits on the posed eye point (the raise animation and the
		// game's own poses carry eye and gun alike), and follows the weapon
		// motion's offset with time constant AlignSmoothing (0: at once), so
		// sway and recoil move the scope against the eye. Offsets are kept in
		// the main camera's frame, so walking and turning carry both alike.
		// AlignStrength times the aim-in's weight scales the offset from the
		// camera: 0 is the camera (the weapon's ironsights pose), 1 the eye
		// point. Without the camera node, the moved eye point.
		V3 alignment_point(V3 eye, V3 moved, bool first, double weight)
		{
			Pose cam;
			if (!main_camera_pose(cam)) return moved;
			const V3 cam_pos = v3(cam.pos);
			const V3 base = unrotate(cam, sub(eye, cam_pos));
			const V3 target = unrotate(cam, sub(moved, eye));

			LARGE_INTEGER now = {}, freq = {};
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			double dt = 0.0;
			if (!first && freq.QuadPart > 0)
				dt = std::clamp(static_cast<double>(now.QuadPart - g_follow_ticks) / static_cast<double>(freq.QuadPart),
					0.0, 0.25);
			g_follow_ticks = now.QuadPart;

			// Through the aim-in the motion offset is followed at once (the
			// motion fades in only after it).
			if (first || g_aim_in.active)
			{
				g_follow[0] = target.x;
				g_follow[1] = target.y;
				g_follow[2] = target.z;
			}

			const double tau = cfg().align_smoothing;
			const double follow = tau > 0.0 ? 1.0 - std::exp(-dt / tau) : 1.0;
			g_follow[0] += (target.x - g_follow[0]) * follow;
			g_follow[1] += (target.y - g_follow[1]) * follow;
			g_follow[2] += (target.z - g_follow[2]) * follow;

			const double strength = std::clamp(static_cast<double>(cfg().align_strength), 0.0, 1.0) *
				std::clamp(weight, 0.0, 1.0);
			const V3 offset = { base.x + g_follow[0], base.y + g_follow[1], base.z + g_follow[2] };
			g_aim_dbg.weight = weight;
			g_aim_dbg.eye_error = { g_follow[0] - target.x, g_follow[1] - target.y, g_follow[2] - target.z };
			g_aim_dbg.follow = { g_follow[0], g_follow[1], g_follow[2] };
			g_aim_dbg.base = base;
			g_aim_dbg.aligned = true;
			return add(cam_pos, rotate(cam, mul(offset, strength)));
		}

		bool unit(V3& v)
		{
			const double l = len(v);
			if (!(l > 1e-6)) return false;
			v = mul(v, 1.0 / l);
			return true;
		}

		remixapi_Float3D f3(V3 v)
		{
			return { static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) };
		}

		// ---- Weapon motion ([WeaponMotion]) -------------------------------------------

		// A small rigid motion of the first-person view model against the camera:
		// breathing sway, the lag behind turns, the walk / run bob and the recoil
		// of the player's own shots. It is applied where the first-person draws
		// go out (motion_apply, to their instance transforms) and to the lens,
		// the alignment eye point and the realistic shot, so gun, lens and shot
		// move together; the game's animations and meshes are not touched.
		// Motion is the rotation R about a pivot below and ahead of the eye
		// plus a translation, both in world axes relative to the camera, game
		// units.
		constexpr double kMotionPivotDown = 12.0;      // game units below the eye
		constexpr double kMotionPivotForward = 10.0;   // and ahead of it: the hands
		constexpr double kBreathDegrees = 0.30;        // the breathing figure eight's half width at the default
		constexpr double kBreathHz = 0.22;
		constexpr double kInertiaSeconds = 0.045;      // the lag per unit of turn rate (degrees per degree/s)
		constexpr double kInertiaMaxDegrees = 1.2;
		constexpr double kBobDegrees = 0.35;           // at a run
		constexpr double kBobAimFactor = 0.4;          // while aiming
		constexpr double kRecoilDegreesBase = 0.5;     // the kick, plus kRecoilPerDamage per point of damage
		constexpr double kRecoilPerDamage = 0.025;
		constexpr double kRecoilMaxDegrees = 2.5;
		constexpr double kRecoilBackPerDegree = 0.35;  // game units the gun comes back per degree of kick
		constexpr double kMaxMotionDegrees = 3.0;
		constexpr double kRunSpeed = 300.0;            // game units per second counted as running
		constexpr double kMotionFadeDelay = 0.25;      // seconds into an aim before the motion fades in
		constexpr double kMotionFadeTime = 0.35;
		constexpr uint32_t ACTOR_AV_OWNER_OFF = 0xA4;  // ActorValueOwner: vtable +4 base, +0xC current (float, code)
		constexpr uint32_t AV_AGILITY = 10, AV_ACTION_POINTS = 12, AV_LEFT_ARM = 27, AV_RIGHT_ARM = 28;
		constexpr uint32_t ATTACK_DAMAGE_OFF = 0xA0;   // TESAttackDamageForm (+0x9C) +4: u16 damage
		constexpr uint32_t REF_POS_OFF = 0x30;

		using AvGet_t = float(__thiscall*)(void* owner, uint32_t code);

		// A critically damped spring: x follows the target with natural
		// frequency omega (rad/s), without overshoot. The step is the exact
		// solution over dt, so it is stable at any frame time.
		struct Spring
		{
			double x = 0.0, v = 0.0;
			void step(double target, double dt, double omega)
			{
				const double x0 = x - target;
				const double b = v + omega * x0;
				const double e = std::exp(-omega * dt);
				x = target + (x0 + b * dt) * e;
				v = (v - omega * b * dt) * e;
			}
			bool finite() const { return std::isfinite(x) && std::isfinite(v); }
		};
		constexpr double kLagOmega = 9.5;    // the turn lag's spring (rad/s)
		constexpr double kKickOmega = 11.8;  // the recoil's spring (rad/s)
		constexpr double kMaxShift = 2.0;    // game units the recoil pushes the gun back at most

		struct Motion
		{
			bool active = false;           // applied this frame
			UINT frame = 0xFFFFFFFFu;
			double rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // R, world axes, row-major (column vectors)
			// Pivot and shift are relative to the main camera node, world axes;
			// main_offset is that node minus the camera the first-person draws
			// go out from (the alignment point), so the draws, the lens, the eye
			// and the shot all turn about the same point.
			V3 pivot = { 0.0, 0.0, 0.0 };
			V3 shift = { 0.0, 0.0, 0.0 };
			V3 main_offset = { 0.0, 0.0, 0.0 };
			// State.
			LONGLONG ticks = 0;
			double breath_phase = 0.0, bob_phase = 0.0, fatigue = 0.0;
			Spring lag_yaw, lag_pitch, kick_pitch, kick_yaw;
			V3 last_forward = { 0.0, 0.0, 1.0 };
			V3 last_pos = { 0.0, 0.0, 0.0 };
			bool have_last = false;
			double aim_time = 0.0;         // seconds aimed; the motion fades in after the aim-in
			double yaw_deg = 0.0, pitch_deg = 0.0;   // this frame's motion angles
			int pending_shots = 0;
			float shot_damage = 0.0f;
		};
		Motion g_motion;

		const shared::common::config::weapon_motion_settings& mcfg()
		{
			return shared::common::config::get().weapon_motion;
		}

		// The player's actor values (current, and base for the action points);
		// false when they cannot be read.
		bool player_condition(uint8_t* player, float& ap_ratio, float& arms, float& agility)
		{
			__try
			{
				auto* owner = player + ACTOR_AV_OWNER_OFF;
				const auto* vt = *reinterpret_cast<const uintptr_t* const*>(owner);
				const auto base = reinterpret_cast<AvGet_t>(vt[1]);
				const auto current = reinterpret_cast<AvGet_t>(vt[3]);
				const float ap = current(owner, AV_ACTION_POINTS), ap_base = base(owner, AV_ACTION_POINTS);
				ap_ratio = ap_base > 1.0f ? ap / ap_base : 1.0f;
				arms = (std::min)(current(owner, AV_LEFT_ARM), current(owner, AV_RIGHT_ARM)) / 100.0f;
				agility = current(owner, AV_AGILITY);
				return std::isfinite(ap_ratio) && std::isfinite(arms) && std::isfinite(agility);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
		}

		bool player_position(uint8_t* player, V3& out)
		{
			__try
			{
				const auto* p = reinterpret_cast<const float*>(player + REF_POS_OFF);
				out = { p[0], p[1], p[2] };
				return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
		}

		uint16_t weapon_damage(const uint8_t* weap)
		{
			__try { return weap ? *reinterpret_cast<const uint16_t*>(weap + ATTACK_DAMAGE_OFF) : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return 0; }
		}

		// The player's own shot in first person (from the fire function's
		// pitch call): the next update kicks.
		void motion_shot(void* pc, const Held& h)
		{
			if (!mcfg().enabled || !h.player || h.player != pc || h.third_person) return;
			// One kick per frame: a shotgun's pellets are one shot.
			g_motion.pending_shots = 1;
			g_motion.shot_damage = static_cast<float>(weapon_damage(h.equipped));
		}

		// Once per frame, at the first world draw (before the lens and the
		// first-person draws).
		void motion_update(const Held& h, bool scope)
		{
			Motion& m = g_motion;
			const UINT now = api::frame();
			if (m.frame == now) return;
			m.frame = now;
			m.main_offset = { 0.0, 0.0, 0.0 };
			LARGE_INTEGER t = {}, freq = {};
			QueryPerformanceCounter(&t);
			QueryPerformanceFrequency(&freq);
			const double dt = (m.ticks && freq.QuadPart > 0)
				? std::clamp(static_cast<double>(t.QuadPart - m.ticks) / static_cast<double>(freq.QuadPart), 0.0, 0.1)
				: 0.0;
			m.ticks = t.QuadPart;

			const auto& c = mcfg();
			const float* view = game::get_renderer_matrix(RENDERER_VIEW_OFF);
			const bool aiming = c.enabled && h.player && h.aiming && !h.third_person && !h.optic &&
				(scope || c.ironsights);
			V3 right = { 1.0, 0.0, 0.0 }, up = { 0.0, 1.0, 0.0 }, forward = { 0.0, 0.0, 1.0 };
			if (view)
			{
				right = { view[0], view[4], view[8] };
				up = { view[1], view[5], view[9] };
				forward = { view[2], view[6], view[10] };
			}
			if (!view || !unit(right) || !unit(up) || !unit(forward) || !h.player)
			{
				m.active = false;
				m.have_last = false;
				return;
			}

			// Each aim starts at rest: no spring carries a step into it, and the
			// motion fades in only after the aim-in (a lens's angle-driven one,
			// else the first kMotionFadeDelay).
			if (!aiming || (scope && g_aim_in.active)) m.aim_time = 0.0;
			else
			{
				if (m.aim_time == 0.0)
				{
					m.lag_yaw = {};
					m.lag_pitch = {};
					m.kick_pitch = {};
					m.kick_yaw = {};
					m.pending_shots = 0;
				}
				m.aim_time += dt;
			}
			const double fade = std::clamp((m.aim_time - kMotionFadeDelay) / kMotionFadeTime, 0.0, 1.0);
			const double weight = fade * fade * (3.0 - 2.0 * fade);

			// The turn rate and the player's speed since the last frame.
			double yaw_rate = 0.0, pitch_rate = 0.0, speed = 0.0;
			V3 pos = { 0.0, 0.0, 0.0 };
			const bool have_pos = player_position(h.player, pos);
			if (m.have_last && dt > 1e-4)
			{
				yaw_rate = std::atan2(m.last_forward.x * forward.y - m.last_forward.y * forward.x,
					m.last_forward.x * forward.x + m.last_forward.y * forward.y) / dt;
				pitch_rate = (std::asin(std::clamp(forward.z, -1.0, 1.0)) -
					std::asin(std::clamp(m.last_forward.z, -1.0, 1.0))) / dt;
				if (have_pos)
				{
					const double dx = pos.x - m.last_pos.x, dy = pos.y - m.last_pos.y;
					speed = std::sqrt(dx * dx + dy * dy) / dt;
				}
			}
			m.last_forward = forward;
			if (have_pos) m.last_pos = pos;
			m.have_last = have_pos;
			if (speed > 5000.0) speed = 0.0;   // a teleport or a load

			// Running tires the arms; it wears off over seconds.
			if (speed > kRunSpeed) m.fatigue = (std::min)(1.0, m.fatigue + dt * 0.25);
			else m.fatigue = (std::max)(0.0, m.fatigue - dt * 0.12);

			// Breathing: a slow figure eight, larger when tired, low on action
			// points, with crippled arms or low Agility; smaller on ironsights.
			double breath = kBreathDegrees * c.breathing * (scope ? 1.0 : 0.6) * (1.0 + 1.5 * m.fatigue);
			float ap_ratio = 1.0f, arms = 1.0f, agility = 5.0f;
			if (player_condition(h.player, ap_ratio, arms, agility))
			{
				breath *= 1.0 + 0.8 * (1.0 - std::clamp(static_cast<double>(ap_ratio), 0.0, 1.0));
				breath *= 1.0 + 1.0 * (1.0 - std::clamp(static_cast<double>(arms), 0.0, 1.0));
				breath *= std::clamp(1.0 + (5.0 - static_cast<double>(agility)) * 0.06, 0.7, 1.3);
			}
			constexpr double kTwoPi = 6.28318530717959;
			m.breath_phase = std::fmod(m.breath_phase + dt * kBreathHz * kTwoPi, kTwoPi * 2.0);
			double yaw = breath * std::sin(m.breath_phase);
			double pitch = breath * 0.5 * std::sin(2.0 * m.breath_phase);

			// The lag behind turns: a spring toward the opposite of the turn.
			constexpr double kDeg = 57.2957795130823;
			const double lag_max = kInertiaMaxDegrees * c.inertia;
			const double yaw_target = std::clamp(-yaw_rate * kDeg * kInertiaSeconds * c.inertia, -lag_max, lag_max);
			const double pitch_target = std::clamp(-pitch_rate * kDeg * kInertiaSeconds * c.inertia, -lag_max, lag_max);
			m.lag_yaw.step(aiming ? yaw_target : 0.0, dt, kLagOmega);
			m.lag_pitch.step(aiming ? pitch_target : 0.0, dt, kLagOmega);
			yaw += m.lag_yaw.x;
			pitch += m.lag_pitch.x;

			// The bob: a step rhythm that grows with speed, smaller while aiming.
			const double pace = (std::min)(speed, 400.0) / 400.0;
			m.bob_phase = std::fmod(m.bob_phase + dt * (1.4 + 1.2 * pace) * kTwoPi, kTwoPi * 2.0);
			const double bob = kBobDegrees * c.bob * pace * kBobAimFactor;
			yaw += bob * std::sin(m.bob_phase);
			pitch += bob * 0.6 * std::fabs(std::sin(m.bob_phase)) - bob * 0.3;

			// Recoil: each shot kicks up (and a little sideways), the spring
			// brings it back.
			if (m.pending_shots > 0 && aiming)
			{
				const double kick = (std::min)(kRecoilMaxDegrees, kRecoilDegreesBase + kRecoilPerDamage * m.shot_damage) *
					c.recoil;
				m.kick_pitch.v += kick * 18.0 * m.pending_shots;
				m.kick_yaw.v += kick * 4.0 * (std::sin(m.breath_phase * 7.0) > 0.0 ? 1.0 : -1.0) * m.pending_shots;
			}
			m.pending_shots = 0;
			m.kick_pitch.step(0.0, dt, kKickOmega);
			m.kick_yaw.step(0.0, dt, kKickOmega);
			// A spring that is not finite (a bad frame time or input) starts
			// over at rest.
			for (Spring* spring : { &m.lag_yaw, &m.lag_pitch, &m.kick_pitch, &m.kick_yaw })
				if (!spring->finite()) *spring = {};
			yaw += m.kick_yaw.x;
			pitch += m.kick_pitch.x;
			if (!std::isfinite(yaw) || !std::isfinite(pitch))
			{
				yaw = 0.0;
				pitch = 0.0;
			}

			if (!aiming)
			{
				m.active = false;
				return;
			}
			yaw = std::clamp(yaw * weight, -kMaxMotionDegrees, kMaxMotionDegrees) / kDeg;
			pitch = std::clamp(pitch * weight, -kMaxMotionDegrees, kMaxMotionDegrees) / kDeg;
			m.yaw_deg = yaw * kDeg;
			m.pitch_deg = pitch * kDeg;

			// R = C * Ry(yaw) * Rx(-pitch) * C^T in the camera's axes (x right, y
			// up, z forward); a positive pitch raises the muzzle.
			const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
			const double local[9] = { cy, sy * sp, sy * cp, 0.0, cp, -sp, -sy, cy * sp, cy * cp };
			const V3 axes[3] = { right, up, forward };
			const auto comp = [&](const V3& v, int j) { return j == 0 ? v.x : j == 1 ? v.y : v.z; };
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
				{
					double sum = 0.0;
					for (int a = 0; a < 3; a++)
						for (int b = 0; b < 3; b++) sum += comp(axes[a], i) * local[a * 3 + b] * comp(axes[b], j);
					m.rot[i * 3 + j] = sum;
				}
			m.pivot = add(mul(up, -kMotionPivotDown), mul(forward, kMotionPivotForward));
			m.shift = mul(forward, std::clamp(-m.kick_pitch.x * kRecoilBackPerDegree * weight, -kMaxShift, kMaxShift));
			m.active = true;
		}

		// R * v.
		V3 motion_turn(V3 v)
		{
			const double* r = g_motion.rot;
			return { r[0] * v.x + r[1] * v.y + r[2] * v.z, r[3] * v.x + r[4] * v.y + r[5] * v.z,
				r[6] * v.x + r[7] * v.y + r[8] * v.z };
		}

		// A point relative to the main camera node (game units, world axes)
		// moved with the view model.
		V3 motion_point(V3 rel)
		{
			if (!g_motion.active) return rel;
			return add(add(motion_turn(sub(rel, g_motion.pivot)), g_motion.pivot), g_motion.shift);
		}

		V3 motion_turn_if_active(V3 v)
		{
			return g_motion.active ? motion_turn(v) : v;
		}

		// One [Scopes] AimDebugLog line for this frame, during the first
		// kAimDebugSeconds of an aim: the aim-in phase, the eye's offset from
		// the moved eye point and the followed motion offset (main camera
		// frame), the motion's angles and translation, the magnification and
		// the main camera's move since the last frame (game units).
		void aim_debug_line(float magnification, double axial, double lateral)
		{
			const double t = aim_in_elapsed();
			Pose cam;
			const bool have_cam = main_camera_pose(cam);
			V3 cam_delta = { 0.0, 0.0, 0.0 };
			if (have_cam)
			{
				if (g_aim_dbg.have_cam) cam_delta = sub(v3(cam.pos), g_aim_dbg.last_cam);
				g_aim_dbg.last_cam = v3(cam.pos);
			}
			g_aim_dbg.have_cam = have_cam;
			if (t > kAimDebugSeconds) return;
			const Motion& m = g_motion;
			const V3& e = g_aim_dbg.eye_error;
			const V3& f = g_aim_dbg.follow;
			const V3& b = g_aim_dbg.base;
			// Every line of the aim's first seconds: past the repeat limit.
			shared::common::log_event("Scopes", std::format("aim t={:.3f} phase={} angle={:.2f} w={:.3f} "
				"eye-point=({:+.3f},{:+.3f},{:+.3f}) eye-err=({:+.3f},{:+.3f},{:+.3f}) follow=({:+.3f},{:+.3f},{:+.3f}) "
				"motion={} yaw={:+.3f} pitch={:+.3f} shift=({:+.3f},{:+.3f},{:+.3f}) mag={:.3f} "
				"cam-delta=({:+.3f},{:+.3f},{:+.3f}) pupil-axial={:+.3f} pupil-lateral={:.3f}", t, !g_aim_dbg.aligned ? "unaligned" : g_aim_in.active ?
				(g_aim_in.settled ? "aim-in settled" : "aim-in") : "locked", g_aim_in.angle, g_aim_dbg.weight, b.x, b.y,
				b.z, e.x, e.y, e.z, f.x, f.y, f.z, m.active ? 1 : 0, m.yaw_deg, m.pitch_deg, m.shift.x, m.shift.y,
				m.shift.z, magnification, cam_delta.x, cam_delta.y, cam_delta.z, axial, lateral),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
		}

		// A point in the render frame (the first-person camera at s * D, the
		// main camera node at s * (D + main_offset)) moved with the view model.
		V3 motion_render_point(V3 p)
		{
			if (!g_motion.active) return p;
			static const double kNoShift[3] = { 0.0, 0.0, 0.0 };
			const double* d = stable_origin::active() ? stable_origin::shift() : kNoShift;
			const double s = stable_origin::scale();
			const V3& o = g_motion.main_offset;
			const V3 cam = { (d[0] + o.x) * s, (d[1] + o.y) * s, (d[2] + o.z) * s };
			return add(cam, mul(motion_point(mul(sub(p, cam), 1.0 / s)), s));
		}

		// ---- AimMode realistic: the shot follows the scope -------------------------
		// The last lens's scope axis and the view's forward
		// (world axes), and the scope's objective from the main camera node.
		struct Shot
		{
			V3 axis = { 0.0, 0.0, 1.0 };
			V3 view = { 0.0, 0.0, 1.0 };
			V3 from_main = { 0.0, 0.0, 0.0 };
			UINT frame = 0xFFFFFFFFu;
		};
		Shot g_shot;


		// The fire function 0x523150, for the player in first person, takes the
		// heading (actor vtable +0x2BC, stored at its [ebp-0x20]) and asks for
		// the pitch (0x931D70, this call); with a scope aimed it has set the
		// shot's origin at the camera ([ebp-0x44], 0x52377C). While a lens
		// weapon is aimed through its lens, the pitch and the heading turn by
		// the scope axis's angle from the view and the origin moves to the
		// scope's objective; the game's spread applies after. Not in VATS (its
		// menu, 1056, on top).
		using Pitch_t = float(__fastcall*)(void* actor, void* edx);
		Pitch_t g_pitch_target = nullptr;
		float g_shot_pitch = 0.0f;
		float g_heading_sign = 1.0f;
		float g_pitch_sign = -1.0f;
		bool g_shot_logged = false;
		constexpr uint32_t FIRE_HEADING_OFF = 0x20;   // below the fire function's EBP
		constexpr uint32_t FIRE_ORIGIN_OFF = 0x44;
		constexpr uint16_t kVatsMenu = 1056;
		constexpr double kPi = 3.14159265358979;

		double wrap_angle(double a)
		{
			while (a > kPi) a -= 2.0 * kPi;
			while (a < -kPi) a += 2.0 * kPi;
			return a;
		}

		bool read_fire_frame(uintptr_t frame, float& heading)
		{
			__try { heading = *reinterpret_cast<const float*>(frame - FIRE_HEADING_OFF); return std::isfinite(heading); }
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return false; }
		}

		void write_fire_frame(uintptr_t frame, float heading, const float (&origin)[3])
		{
			__try
			{
				*reinterpret_cast<float*>(frame - FIRE_HEADING_OFF) = heading;
				std::memcpy(reinterpret_cast<void*>(frame - FIRE_ORIGIN_OFF), origin, sizeof(origin));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; }
		}

		// True with g_shot_pitch set when the shot follows the scope; the
		// heading and origin are written into the fire function's frame.
		bool __cdecl shot_override(void* pc, uintptr_t frame)
		{
			Held h;
			const bool have = held(h);
			if (have) motion_shot(pc, h);
			if (!g_hooked || aim_mode() == kAimView || !active()) return false;
			const UINT now = api::frame();
			if (g_shot.frame != now && g_shot.frame != now - 1) return false;
			const FnvRemixFacts& facts = nvse_link::facts();
			if (facts.ready && facts.top_menu == kVatsMenu) return false;
			if (!have || h.player != pc || !h.aiming || h.third_person || h.optic || !held_lens(h)) return false;
			float heading = 0.0f;
			if (!read_fire_frame(frame, heading)) return false;
			const float pitch = g_pitch_target(pc, nullptr);
			if (!std::isfinite(pitch)) return false;

			// The engine's angle conventions, read from the view: the heading
			// matches atan2(x, y) or its negative, the pitch has the elevation's
			// sign or the opposite.
			const V3& v = g_shot.view;
			const V3& a = g_shot.axis;
			const double yaw_v = std::atan2(v.x, v.y), yaw_a = std::atan2(a.x, a.y);
			const double elev_v = std::asin(std::clamp(v.z, -1.0, 1.0)), elev_a = std::asin(std::clamp(a.z, -1.0, 1.0));
			const double same = std::fabs(wrap_angle(yaw_v - heading)), opposite = std::fabs(wrap_angle(yaw_v + heading));
			if (std::fabs(same - opposite) > 0.2) g_heading_sign = same < opposite ? 1.0f : -1.0f;
			if (std::fabs(elev_v) > 0.05 && std::fabs(pitch) > 0.05) g_pitch_sign = elev_v * pitch < 0.0 ? -1.0f : 1.0f;

			const float new_heading = static_cast<float>(heading + g_heading_sign * wrap_angle(yaw_a - yaw_v));
			g_shot_pitch = static_cast<float>(pitch + g_pitch_sign * (elev_a - elev_v));
			Pose mcam = {};
			if (!main_camera_pose(mcam)) return false;
			const V3 o = add(v3(mcam.pos), g_shot.from_main);
			const float origin[3] = { static_cast<float>(o.x), static_cast<float>(o.y), static_cast<float>(o.z) };
			write_fire_frame(frame, new_heading, origin);
			if (!g_shot_logged)
			{
				g_shot_logged = true;
				log(std::format("realistic aim: the shot follows the scope ({:.2f} deg off the view; heading sign {:+.0f}, "
					"pitch sign {:+.0f})", std::acos(std::clamp(v.x * a.x + v.y * a.y + v.z * a.z, -1.0, 1.0)) / kPi * 180.0,
					g_heading_sign, g_pitch_sign));
			}
			return true;
		}

		// At the fire function's pitch call: the caller's EBP is its frame.
		__declspec(naked) void shot_pitch_thunk()
		{
			__asm
			{
				push ecx
				push ebp
				push ecx
				call shot_override
				add esp, 8
				pop ecx
				test al, al
				jz vanilla
				fld dword ptr [g_shot_pitch]
				ret
			vanilla:
				jmp dword ptr [g_pitch_target]
			}
		}

		// ---- Binoculars --------------------------------------------------------------

		// The binoculars' lens in the render frame, attached to this frame's
		// world camera (renderer VIEW and PROJECTION; the camera sits at s * D)
		// on the view axis: the eyepiece eye relief ahead of the eye, its two
		// barrels (the binocular aperture) kBinocularFill of half the screen
		// height in radius. Fills the entry's lengths in render units.
		bool binocular_place(const double* d, double s, data::Lens& l, V3& eyepiece, V3& axis, V3& up)
		{
			const float* view = game::get_renderer_matrix(RENDERER_VIEW_OFF);
			const float* proj = game::get_renderer_matrix(RENDERER_PROJ_OFF);
			if (!view || !proj || !(proj[5] > 1e-4f)) return false;
			up = { view[1], view[5], view[9] };
			axis = { view[2], view[6], view[10] };
			if (!unit(up) || !unit(axis)) return false;
			const double per_cm = s / kCmPerUnit;
			const double tan_y = 1.0 / static_cast<double>(proj[5]);
			const double relief = kBinocularEyeReliefCm * per_cm;
			eyepiece = add(V3{ d[0] * s, d[1] * s, d[2] * s }, mul(axis, relief));
			l.eye_relief = static_cast<float>(relief);
			l.eyepiece_radius = static_cast<float>(relief * tan_y * kBinocularFill);
			l.objective_distance = static_cast<float>(kBinocularObjectiveCm * per_cm);
			l.objective_radius = static_cast<float>(kBinocularObjectiveRadiusCm * per_cm);
			return true;
		}

		// ---- Reticle textures --------------------------------------------------------

		// An entry's ReticleTexture as the absolute path the runtime loads: as
		// written when absolute, else under the game's Data folder. Null when
		// the file is not there, is not a .dds or its path is not ASCII. One lookup per path for the session; the
		// strings stay put, so the runtime can read them during SetupCamera.
		std::unordered_map<std::string, std::wstring> g_reticle_paths;

		const wchar_t* reticle_texture_path(const char* entry)
		{
			if (!entry || !entry[0]) return nullptr;
			auto [it, fresh] = g_reticle_paths.try_emplace(entry);
			if (fresh)
			{
				std::wstring w = session_files::wide(entry);
				for (wchar_t& c : w)
					if (c == L'/') c = L'\\';
				const bool absolute = (w.size() > 2 && w[1] == L':') || w.rfind(L"\\\\", 0) == 0;
				if (!absolute) w = session_files::game_dir() + L"Data\\" + w;
				// The runtime loads DDS files only, from an ASCII path.
				const bool dds = w.size() > 4 && _wcsicmp(w.c_str() + w.size() - 4, L".dds") == 0;
				const bool ascii = std::all_of(w.begin(), w.end(), [](wchar_t c) { return c > 0 && c < 0x80; });
				const DWORD attr = GetFileAttributesW(w.c_str());
				const bool found = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
				const std::string text = session_files::utf8(w);
				if (found && dds && ascii) it->second = std::move(w);
				log(!found ? std::format("reticle texture '{}' not found: the entry's drawn reticle is used", entry)
					: !dds ? std::format("reticle texture '{}' is not a .dds file: the entry's drawn reticle is used", entry)
					: !ascii ? std::format("reticle texture path '{}' is not plain ASCII: the entry's drawn reticle is used",
						text)
					: std::format("reticle texture '{}': {}", entry, text),
					it->second.empty() ? shared::common::LOG_TYPE::LOG_TYPE_WARN : shared::common::LOG_TYPE::LOG_TYPE_DEFAULT);
			}
			return it->second.empty() ? nullptr : it->second.c_str();
		}

		// ---- The crosshair ---------------------------------------------------------

		// 0x771700 sets the center reticle tile's visibility from its mask
		// only when the HUD's mode changes; while a lens is up the tile is
		// kept hidden every frame, after the frame's HUD update.
		void* reticle_tile()
		{
			const uintptr_t hud_ptr = fnv_exe::addr().hud_main_menu;
			__try
			{
				const auto* hud = *reinterpret_cast<const uint8_t* const*>(hud_ptr);
				if (!is_live_object(hud)) return nullptr;
				void* tile = *reinterpret_cast<void* const*>(hud + HUD_RETICLE_TILE_OFF);
				return is_live_object(tile) ? tile : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return nullptr; }
		}

		// The tile's visible trait, or -1 when it cannot be read; sets it to
		// `value` when `set` and it differs.
		float reticle_visible(void* tile, bool set, int value)
		{
			const auto& a = fnv_exe::addr();
			__try
			{
				const float v = reinterpret_cast<TileGet_t>(a.tile_get_value)(tile, TILE_VISIBLE);
				if (set && v != static_cast<float>(value))
					reinterpret_cast<TileSet_t>(a.tile_set_value)(tile, TILE_VISIBLE, value);
				return v;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return -1.0f; }
		}

		void hide_reticle()
		{
			void* tile = reticle_tile();
			if (!tile) return;
			const float was = reticle_visible(tile, true, 0);
			g_reticle_hidden = true;
			if (!g_reticle_logged)
			{
				g_reticle_logged = true;
				log(std::format("crosshair hidden while the lens is up (its visible trait was {:.0f})", was));
			}
		}

		// The tile back to what the HUD's mask says.
		void show_reticle()
		{
			if (!g_reticle_hidden) return;
			g_reticle_hidden = false;
			void* tile = reticle_tile();
			if (!tile) return;
			uint32_t mask = 0;
			__try
			{
				const auto* hud = *reinterpret_cast<const uint8_t* const*>(fnv_exe::addr().hud_main_menu);
				mask = *reinterpret_cast<const uint32_t*>(hud + HUD_VISIBILITY_OFF);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return; }
			reticle_visible(tile, true, (mask & HUD_RETICLE_BIT) ? 1 : 0);
		}

		// ---- Vision modes ----------------------------------------------------------

		// The vision key cycles the lens's vision mode while the lens is up:
		// none and the weapon's own mode, or every mode with VisionAllModes.
		void vision_key(bool up)
		{
			const int vk = cfg().vision_key;
			if (vk <= 0 || vk > 0xFE) return;
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0 && GetForegroundWindow() == shared::globals::main_window;
			const bool pressed = (down && !g_vision_key_down) || g_pad_vision;
			g_vision_key_down = down;
			g_pad_vision = false;
			if (!pressed || !up) return;
			const int was = g_vision;
			// The next of the weapon's modes (the first when the current one is
			// not among them).
			int at = -1;
			for (int i = 0; i < g_vision_count; i++)
				if (g_vision_list[i] == g_vision) at = i;
			if (g_vision_count > 0) g_vision = g_vision_list[(at + 1) % g_vision_count];
			if (g_vision == was) return;
			play_sound(g_vision ? kSoundVisionOn : kSoundVisionOff);
			static constexpr const char* kNames[] = { "none", "night", "thermal" };
			log(std::format("vision mode: {}", kNames[std::clamp(g_vision, 0, 2)]));
		}

		// Aiming a lens weapon in third person switches to first person; the
		// end of that aim switches back.
		void force_first_person()
		{
			Held h;
			if (!held(h) || !h.player) return;
			if (g_forced_first_person)
			{
				if (h.aiming) return;
				g_forced_first_person = false;
				if (!h.third_person) set_first_person(h.player, false);
				return;
			}
			if (!cfg().force_first_person || !active() || !h.aiming || !h.third_person || !held_lens(h)) return;
			g_forced_first_person = true;
			set_first_person(h.player, true);
			log("third-person aim with a lens weapon: first person for the aim");
		}

		// rtx.lens.peripheralBlur follows [Scopes] PeripheralBlur while a lens
		// is aimed and locked, and is 0 otherwise (the lens at the hip).
		void push_blur()
		{
			const float blur = g_lens_locked_frame == api::frame() - 1 ? cfg().peripheral_blur : 0.0f;
			if (blur == g_blur_pushed || !active()) return;
			auto& bridge = shared::common::remix_api::get().m_bridge;
			if (!bridge.SetConfigVariable) return;
			const std::string v = std::format("{:.3f}", blur);
			if (bridge.SetConfigVariable("rtx.lens.peripheralBlur", v.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS)
				g_blur_pushed = blur;
		}

		// ---- Actor tags --------------------------------------------------------------

		// One actor's 3D root and its instance tags.
		struct Tagged
		{
			uintptr_t root;
			uint32_t tags;
		};
		Tagged g_tagged[kMaxTagged];
		std::unordered_map<uintptr_t, uint32_t> g_tags;
		std::unordered_map<uintptr_t, uint32_t> g_tag_cache;
		UINT g_tag_cache_frame = 0xFFFFFFFFu;
		uint32_t g_tags_logged = 0xFFFFFFFFu;

		// The high and middle-high process actors: living ones are heat
		// sources, hostile ones (in combat with the player, or hostile on
		// the compass) are highlighted. Read-only: no AI state changes.
		int collect_tags_unguarded(uint8_t* player)
		{
			const auto& a = fnv_exe::addr();
			const auto pl = a.process_lists;
			auto* const* objects = *reinterpret_cast<uint8_t* const* const*>(pl + PL_OBJECTS_OFF);
			const uint32_t capacity = *reinterpret_cast<const uint16_t*>(pl + PL_CAPACITY_OFF);
			if (!is_valid_ptr(objects)) return 0;
			const auto in_combat = reinterpret_cast<InCombatWith_t>(a.actor_in_combat_with);

			// The compass's hostile targets.
			const void* hostile[kMaxTagged];
			int hostiles = 0;
			const auto* node = *reinterpret_cast<const uintptr_t* const*>(player + PC_COMPASS_TARGETS_OFF);
			for (int steps = 0; is_valid_ptr(node) && steps < kMaxListSteps && hostiles < kMaxTagged; steps++)
			{
				const auto* target = reinterpret_cast<const uint8_t*>(node[0]);
				if (is_valid_ptr(target) && target[4]) hostile[hostiles++] = *reinterpret_cast<void* const*>(target);
				node = reinterpret_cast<const uintptr_t*>(node[1]);
			}

			int n = 0;
			for (int level = 0; level < 2; level++)
			{
				const uint32_t begin = *reinterpret_cast<const uint32_t*>(pl + PL_BEGIN_OFF + level * 4);
				const uint32_t end = *reinterpret_cast<const uint32_t*>(pl + PL_END_OFF + level * 4);
				if (begin > end || end > capacity) continue;
				for (uint32_t i = begin; i < end && n < kMaxTagged; i++)
				{
					uint8_t* actor = objects[i];
					if (!is_live_object(actor) || actor == player) continue;
					if (actor[FORM_TYPE_OFF] != FORM_TYPE_CHARACTER && actor[FORM_TYPE_OFF] != FORM_TYPE_CREATURE) continue;
					const uint32_t life = *reinterpret_cast<const uint32_t*>(actor + ACTOR_LIFE_STATE_OFF);
					const bool dead = life == 1 || life == 2;
					// The 3D root read from the render state, no virtual call: an
					// actor being unloaded has none and is skipped.
					const auto* rs = *reinterpret_cast<const uint8_t* const*>(actor + REF_RENDER_STATE_OFF);
					if (!is_valid_ptr(rs)) continue;
					void* root = *reinterpret_cast<void* const*>(rs + RENDER_STATE_NODE_OFF);
					if (!is_live_object(root)) continue;
					uint32_t tags = dead ? 0u : kTagHeat;
					if (!dead)
					{
						bool is_hostile = in_combat(actor, player);
						for (int k = 0; !is_hostile && k < hostiles; k++) is_hostile = hostile[k] == actor;
						if (is_hostile) tags |= kTagHostile;
					}
					if (tags) g_tagged[n++] = { reinterpret_cast<uintptr_t>(root), tags };
				}
			}
			return n;
		}

		int collect_tags(uint8_t* player)
		{
			__try { return collect_tags_unguarded(player); }
			__except (EXCEPTION_EXECUTE_HANDLER) { g_faults++; return 0; }
		}

		bool g_tags_unready_logged = false;

		// Once per frame at Present, while a lens with a vision mode or
		// hostile highlighting is up; the next frame's actor draws read it.
		void build_tags()
		{
			g_tags.clear();
			if (!g_tags_wanted || !active()) return;
			// Not while cells load or unload: the process lists' actors are
			// being torn down and rebuilt then.
			const FnvRemixFacts& facts = nvse_link::facts();
			if (!facts.ready && !g_tags_unready_logged)
			{
				g_tags_unready_logged = true;
				log("the NVSE link's facts are not ready: thermal heat and hostile highlighting show no actors until "
					"they are", shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
			if (!facts.ready || facts.loading || !facts.settled) return;
			Held h;
			if (!held(h) || !h.player) return;
			const int n = collect_tags(h.player);
			uint32_t hostiles = 0;
			for (int i = 0; i < n; i++)
			{
				g_tags[g_tagged[i].root] = g_tagged[i].tags;
				if (g_tagged[i].tags & kTagHostile) hostiles++;
			}
			const uint32_t summary = static_cast<uint32_t>(n) << 16 | hostiles;
			if (summary != g_tags_logged && shared::common::log_verbose())
			{
				g_tags_logged = summary;
				log(std::format("actor tags: {} living actors, {} hostile", n, hostiles));
			}
		}

		// ---- The first-person camera, measured --------------------------------------

		// The first-person draws go out at the renderer's WORLD: the object's
		// world position minus the camera the first-person pass renders from,
		// in world axes. At a draw of the held weapon's model that camera is
		// the geometry's world position minus WORLD's translation. The lens's
		// node is kept relative to that camera, in the camera's own axes (the
		// renderer VIEW of that pass), so the next lens rebuilds it with that
		// frame's camera: a turn moves lens and gun alike, and only the gun's
		// motion against the view (sway, recoil) is a frame old.
		struct FirstPersonPose
		{
			double pos[3];   // the node's position from the first-person camera, camera axes
			double rot[9];   // the node's rotation in camera axes, row-major (column vectors)
			double eye[3];   // the entry's eye point from the main camera node, camera axes
			float scale;
			uint32_t form_id;
			UINT frame = 0xFFFFFFFFu;
		};
		FirstPersonPose g_fp;
		uint32_t g_fp_logged_form = 0;

		// The camera-to-world rotation of a D3D VIEW (row vectors: view =
		// world * V), as a column-vector matrix: C[i][j] = V[i][j].
		double cam_axis(const float* view, int i, int j)
		{
			return view[i * 4 + j];
		}

		void measure_first_person_camera()
		{
			const UINT now = api::frame();
			if (g_fp.frame == now || (!lens_recent() && !cfg().lens_always)) return;
			const uintptr_t geo = scene_graph::current_geometry();
			if (!geo) return;
			Held h;
			data::Lens l;
			if (!held(h) || !h.weap || h.optic || !held_lens(h, &l)) return;
			const uintptr_t root = weapon_root(h);
			if (!root) return;
			bool under = false;
			uintptr_t obj = geo;
			for (int depth = 0; obj && depth < kMaxParentDepth; ++depth)
			{
				if (obj == root) { under = true; break; }
				uintptr_t parent = 0;
				uint32_t flags = 0;
				if (!scene_graph::read_link(obj, parent, flags)) break;
				obj = parent;
			}
			if (!under) return;
			float p[scene_graph::kPoseFloats];
			const float* w = api::world_matrix();
			const float* view = game::get_renderer_matrix(RENDERER_VIEW_OFF);
			Pose node;
			if (!w || !view || !scene_graph::read_pose(geo, p) || !weapon_pose(h, l.node, node)) return;
			Pose mcam;
			V3 axis_local = v3(l.axis);
			if (!main_camera_pose(mcam) || !unit(axis_local)) return;
			const V3 fp = { p[9] - w[12], p[10] - w[13], p[11] - w[14] };
			const V3 o = sub(v3(node.pos), fp);
			const V3 e = sub(to_world(node, sub(v3(l.eyepiece), mul(axis_local, l.eye_relief))), v3(mcam.pos));
			// Camera axes: C^T * o, C^T * e, C^T * R.
			for (int j = 0; j < 3; j++)
			{
				g_fp.pos[j] = cam_axis(view, 0, j) * o.x + cam_axis(view, 1, j) * o.y + cam_axis(view, 2, j) * o.z;
				g_fp.eye[j] = cam_axis(view, 0, j) * e.x + cam_axis(view, 1, j) * e.y + cam_axis(view, 2, j) * e.z;
			}
			for (int i = 0; i < 3; i++)
				for (int c = 0; c < 3; c++)
					g_fp.rot[i * 3 + c] = cam_axis(view, 0, i) * node.rot[c] + cam_axis(view, 1, i) * node.rot[3 + c] +
						cam_axis(view, 2, i) * node.rot[6 + c];
			g_fp.scale = node.scale;
			g_fp.form_id = h.form_id;
			g_fp.frame = now;

			// Once per weapon: whether the first-person pass rendered from the
			// alignment point (the engine used it), for the log.
			if (h.form_id != g_fp_logged_form && cfg().align_aim)
			{
				g_fp_logged_form = h.form_id;
				float at[3];
				std::memcpy(at, g_sight + AV_WORLD_POS_OFF, sizeof(at));
				const double gap = len(sub(fp, v3(at)));
				log(gap < 0.05
					? std::string("first-person camera at the alignment point")
					: std::format("first-person camera {:.2f} units from the alignment point (the engine did not use it); "
						"the lens follows the camera it used", gap),
					gap < 0.05 ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
		}

		// The lens's node relative to this frame's first-person camera, in
		// world axes: the last measurement, turned with this frame's VIEW.
		// False without a measurement of this weapon from this frame or the
		// last.
		bool first_person_pose(const Held& h, Pose& out, V3& eye_from_main)
		{
			const UINT now = api::frame();
			if ((g_fp.frame != now && g_fp.frame != now - 1) || g_fp.form_id != h.form_id) return false;
			const float* view = game::get_renderer_matrix(RENDERER_VIEW_OFF);
			if (!view) return false;
			double e[3] = {};
			for (int i = 0; i < 3; i++)
			{
				out.pos[i] = static_cast<float>(cam_axis(view, i, 0) * g_fp.pos[0] + cam_axis(view, i, 1) * g_fp.pos[1] +
					cam_axis(view, i, 2) * g_fp.pos[2]);
				e[i] = cam_axis(view, i, 0) * g_fp.eye[0] + cam_axis(view, i, 1) * g_fp.eye[1] +
					cam_axis(view, i, 2) * g_fp.eye[2];
				for (int c = 0; c < 3; c++)
					out.rot[i * 3 + c] = static_cast<float>(cam_axis(view, i, 0) * g_fp.rot[c] +
						cam_axis(view, i, 1) * g_fp.rot[3 + c] + cam_axis(view, i, 2) * g_fp.rot[6 + c]);
			}
			out.scale = g_fp.scale;
			eye_from_main = { e[0], e[1], e[2] };
			return true;
		}

		// ---- The editor's state --------------------------------------------------

		editor::Fields g_fields;
		// The entry's ReticleTexture, kept through edits (the menu has no text field).
		char g_ed_texture[128] = {};
		// The entry's Models, kept through edits.
		char g_ed_models[768] = {};
		uint32_t g_ed_form = 0;
		uint8_t g_ed_mods = 0;
		bool g_ed_target = false;
		bool g_ed_resync = true;
		std::string g_status = "no scoped weapon equipped";
		uint32_t g_status_gen = 0;

		void set_status(std::string s)
		{
			if (s == g_status) return;
			g_status = std::move(s);
			g_status_gen++;
		}

		void fields_from(const data::Lens& l, bool combination)
		{
			auto& f = g_fields;
			std::memcpy(f.eyepiece, l.eyepiece, sizeof(f.eyepiece));
			std::memcpy(f.axis, l.axis, sizeof(f.axis));
			std::memcpy(f.up, l.up, sizeof(f.up));
			f.eyepiece_radius = l.eyepiece_radius;
			f.objective_distance = l.objective_distance;
			f.objective_radius = l.objective_radius;
			f.eye_relief = l.eye_relief;
			f.magnification = l.magnification;
			f.field_stop = l.field_stop;
			f.reticle = l.reticle;
			std::memcpy(f.reticle_color, l.reticle_color, sizeof(f.reticle_color));
			f.reticle_radiance = l.reticle_radiance;
			f.reticle_line_width = l.reticle_line_width;
			std::memcpy(f.tint, l.tint, sizeof(f.tint));
			f.kind = l.kind;
			f.aperture = l.aperture;
			f.aperture_aspect = l.aperture_aspect;
			f.vision_mode = l.vision_mode + 1;
			f.highlight_hostiles = l.highlight_hostiles;
			for (bool& a : f.vision_allowed) a = false;
			f.vision_order_count = l.vision_count;
			for (int i = 0; i < l.vision_count; i++)
			{
				f.vision_order[i] = l.vision_list[i];
				f.vision_allowed[std::clamp(static_cast<int>(l.vision_list[i]), 0, 2)] = true;
			}
			f.min_magnification = l.min_magnification;
			f.max_magnification = l.max_magnification;
			std::memcpy(f.reticle_accent_color, l.reticle_accent_color, sizeof(f.reticle_accent_color));
			f.reticle_accent_radiance = l.reticle_accent_radiance;
			std::memcpy(f.edge_glow_color, l.edge_glow_color, sizeof(f.edge_glow_color));
			f.edge_glow_strength = l.edge_glow_strength;
			f.edge_shade = l.edge_shade;
			std::memcpy(f.edge_tint, l.edge_tint, sizeof(f.edge_tint));
			std::memcpy(f.night_tint, l.night_tint, sizeof(f.night_tint));
			f.night_gain = l.night_gain;
			f.markings = l.markings & 3;
			f.reticle_plane = l.reticle_plane;
			f.marking_color_set = l.marking_color_set;
			std::memcpy(f.marking_color, l.marking_color, sizeof(f.marking_color));
			f.marking_radiance = l.marking_radiance;
			f.marking_opacity = l.marking_opacity;
			f.parallax_m = l.parallax_m;
			f.glare = l.glare;
			f.dirt = l.dirt;
			f.grain = l.grain;
			strncpy_s(g_ed_texture, l.reticle_texture, _TRUNCATE);
			strncpy_s(g_ed_models, l.models, _TRUNCATE);
			f.combination = combination;
		}

		// The equipped weapon model's named nodes, for the editor's Node choice.
		constexpr uint32_t kMaxNodeLabels = 48;
		std::vector<std::string> g_node_names = { "(root)" };
		const char* g_node_labels[kMaxNodeLabels] = { "(root)" };
		uint32_t g_node_count = 1;

		data::Lens lens_from_fields()
		{
			const auto& f = g_fields;
			data::Lens l;
			if (f.node > 0 && static_cast<uint32_t>(f.node) < g_node_count)
				strncpy_s(l.node, g_node_names[static_cast<size_t>(f.node)].c_str(), _TRUNCATE);
			std::memcpy(l.eyepiece, f.eyepiece, sizeof(l.eyepiece));
			std::memcpy(l.axis, f.axis, sizeof(l.axis));
			std::memcpy(l.up, f.up, sizeof(l.up));
			l.eyepiece_radius = f.eyepiece_radius;
			l.objective_distance = f.objective_distance;
			l.objective_radius = f.objective_radius;
			l.eye_relief = f.eye_relief;
			l.magnification = f.magnification;
			l.field_stop = f.field_stop;
			l.reticle = std::clamp(f.reticle, 0, data::kReticleCount - 1);
			std::memcpy(l.reticle_accent_color, f.reticle_accent_color, sizeof(l.reticle_accent_color));
			l.reticle_accent_radiance = f.reticle_accent_radiance;
			std::memcpy(l.edge_glow_color, f.edge_glow_color, sizeof(l.edge_glow_color));
			l.edge_glow_strength = f.edge_glow_strength;
			l.edge_shade = f.edge_shade;
			std::memcpy(l.edge_tint, f.edge_tint, sizeof(l.edge_tint));
			std::memcpy(l.night_tint, f.night_tint, sizeof(l.night_tint));
			l.night_gain = f.night_gain;
			l.markings = std::clamp(f.markings, 0, 3);
			l.reticle_plane = std::clamp(f.reticle_plane, 0, 1);
			l.marking_color_set = f.marking_color_set;
			std::memcpy(l.marking_color, f.marking_color, sizeof(l.marking_color));
			l.marking_radiance = f.marking_radiance;
			l.marking_opacity = std::clamp(f.marking_opacity, 0.0f, 1.0f);
			l.parallax_m = f.parallax_m;
			l.glare = f.glare;
			l.dirt = f.dirt;
			l.grain = f.grain;
			strncpy_s(l.reticle_texture, g_ed_texture, _TRUNCATE);
			// Models keeps the entry's and adds the model the weapon is drawn
			// with now.
			strncpy_s(l.models, g_ed_models, _TRUNCATE);
			{
				Held h;
				if (held(h) && h.form_id == g_ed_form && h.weap)
				{
					std::string drawn[2];
					if (fp_models(h.weap, f.combination ? g_ed_mods : h.mods, drawn) > 0) data::add_model(l, drawn[0]);
				}
			}
			l.kind = std::clamp(f.kind, 0, 1);
			l.aperture = std::clamp(f.aperture, 0, 2);
			l.aperture_aspect = f.aperture_aspect;
			l.vision_mode = std::clamp(f.vision_mode, 0, 3) - 1;
			l.highlight_hostiles = f.highlight_hostiles;
			// The entry's order for the modes still allowed, then the newly
			// allowed ones in none, night, thermal order.
			l.vision_count = 0;
			for (int i = 0; i < f.vision_order_count && i < 3; i++)
			{
				const int m = std::clamp(static_cast<int>(f.vision_order[i]), 0, 2);
				if (f.vision_allowed[m]) l.vision_list[l.vision_count++] = static_cast<uint8_t>(m);
			}
			for (int m = 0; m < 3; m++)
			{
				bool listed = false;
				for (int i = 0; i < l.vision_count; i++) listed = listed || l.vision_list[i] == m;
				if (f.vision_allowed[m] && !listed && l.vision_count < 3) l.vision_list[l.vision_count++] = static_cast<uint8_t>(m);
			}
			l.min_magnification = f.min_magnification;
			l.max_magnification = f.max_magnification;
			std::memcpy(l.reticle_color, f.reticle_color, sizeof(l.reticle_color));
			l.reticle_radiance = f.reticle_radiance;
			l.reticle_line_width = f.reticle_line_width;
			std::memcpy(l.tint, f.tint, sizeof(l.tint));
			return l;
		}

		std::string mods_text(uint8_t mods)
		{
			std::string s;
			if (mods & 1) s += '1';
			if (mods & 2) s += '2';
			if (mods & 4) s += '3';
			return s.empty() ? "none" : s;
		}

		void update_status()
		{
			if (!g_ed_target)
			{
				set_status("no scoped weapon equipped");
				return;
			}
			const std::string* edid = data::editor_id(g_ed_form);
			if (!edid)
			{
				set_status(std::format("weapon {:08X} has no EditorID (the engine monitor plugin reads them from the "
					"plugin files)", g_ed_form));
				return;
			}
			data::Lens l;
			int section = data::kBase;
			if (!data::resolve(g_ed_form, g_ed_mods, l, &section))
			{
				set_status(std::format("{} (mods {}): no lens entry; changing a value creates one", *edid,
					mods_text(g_ed_mods)));
				return;
			}
			set_status(std::format("{} (mods {}): {}{}", *edid, mods_text(g_ed_mods), data::section_name(section),
				data::dirty(g_ed_form) ? ", not saved" : ""));
		}

		// The editor follows the equipped scoped gun and its installed mods.

		void list_nodes(uintptr_t root, const char* keep)
		{
			g_node_names.assign(1, "(root)");
			std::vector<uintptr_t> open;
			if (root) open.push_back(root);
			uintptr_t kids[64];
			for (int visits = 0; !open.empty() && visits < kMaxNodeVisits && g_node_names.size() < kMaxNodeLabels;
				visits++)
			{
				const uintptr_t obj = open.back();
				open.pop_back();
				const scene_graph::ClassInfo* ci = scene_graph::class_of(obj);
				if (!ci || !ci->node) continue;
				char nm[64];
				if (obj != root && node_name(obj, nm) && nm[0]) g_node_names.emplace_back(nm);
				scene_graph::NodeChildren info;
				if (!scene_graph::read_children(obj, kids, 64, info)) continue;
				for (uint32_t i = 0; i < info.read; i++)
					if (kids[i]) open.push_back(kids[i]);
			}
			// An entry's node the model does not have stays choosable.
			if (keep && keep[0] && std::find(g_node_names.begin(), g_node_names.end(), keep) == g_node_names.end() &&
				g_node_names.size() < kMaxNodeLabels)
				g_node_names.emplace_back(keep);
			g_node_count = static_cast<uint32_t>(g_node_names.size());
			for (uint32_t i = 0; i < g_node_count; i++) g_node_labels[i] = g_node_names[i].c_str();
			g_status_gen++;
		}

		int node_index(const char* name)
		{
			if (!name || !name[0]) return 0;
			for (uint32_t i = 1; i < g_node_count; i++)
				if (g_node_names[i] == name) return static_cast<int>(i);
			return 0;
		}

		void editor_follow()
		{
			Held h;
			const bool target = held(h) && h.weap && h.shown && !h.optic;
			if (target == g_ed_target && (!target || (h.form_id == g_ed_form && h.mods == g_ed_mods)) && !g_ed_resync)
				return;
			g_ed_target = target;
			g_ed_form = target ? h.form_id : 0;
			g_ed_mods = target ? h.mods : 0;
			g_ed_resync = false;
			data::Lens l;
			int section = data::kBase;
			if (target && data::resolve(g_ed_form, g_ed_mods, l, &section)) fields_from(l, section != data::kBase);
			else fields_from(data::Lens{}, false);
			list_nodes(target ? weapon_root(h) : 0, l.node);
			g_fields.node = node_index(l.node);
			update_status();
		}
	}

	void install()
	{
		if (!cfg().path_traced)
		{
			log("hooks skipped ([Scopes] PathTraced=0): scopes are the game's own");
			return;
		}
		const auto& a = fnv_exe::addr();
		if (!fnv_exe::known() || !a.player || !a.actor_is_aiming || !a.model_path_length || !a.item_mod_effect ||
			!a.xlist_weapon_mod || !a.sighting_anim_block || !a.sighting_cam_block || !a.camera_state ||
			!a.default_world_fov)
		{
			log("hooks skipped (unknown executable): scopes are the game's own",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}

		// The core hooks all or none: a partial set would leave, say, the
		// overlay off with the zoom still on. The others each carry one
		// feature, which stays off when another module holds its site in a way
		// the wrapper cannot chain.
		constexpr const char* kAttach = "HUDMainMenu scope model attach";
		constexpr const char* kVisible = "HUDMainMenu scoped state";
		constexpr const char* kUseIs = "TESObjectWEAP first-person ironsights anims";
		constexpr const char* kCull = "SetAiming's scope model test before the first-person cull";
		constexpr const char* kMod = "SetAiming's ScopeFromMod test before the ironsights anim";
		constexpr const char* kZoom = "UpdateFOV's aiming test before the ironsights zoom";
		constexpr const char* kHud = "the HUD reticle state's sighting node test before it hides the crosshair";
		constexpr const char* kNv = "the player update's ScopeHasNightVision test before the scope night-vision IMAD";
		constexpr const char* kPoll = "OSInputGlobals::Poll (the mouse wheel)";
		constexpr const char* kXInput = "the game's XInputGetState import (the gamepad)";
		constexpr const char* kSightA = "the first-person camera setup's sit / sleep test before the sighting node (0x874C10)";
		constexpr const char* kSightB = "the same test in 0x875110";
		constexpr const char* kShot = "the fire function's pitch call for the player's first-person shot";
		const bool entries = call_chain::hookable(a.scope_model_attach, "Scopes", kAttach) &&
			call_chain::hookable(a.scope_set_visible, "Scopes", kVisible) &&
			call_chain::hookable(a.weap_use_1st_is_anims, "Scopes", kUseIs);
		const bool poll_ok = call_chain::hookable(a.input_poll, "Scopes", kPoll);
		const call_chain::Site cull = call_chain::inspect(a.aim_cull_model_call);
		const call_chain::Site mod = call_chain::inspect(a.aim_scope_mod_call);
		const call_chain::Site zoom = call_chain::inspect(a.fov_zoom_aiming_call);
		const call_chain::Site hud = call_chain::inspect(a.hud_sighting_call);
		const call_chain::Site nv = call_chain::inspect(a.nv_scope_flag_call);
		const call_chain::Site sight_a = call_chain::inspect(a.fp_sight_block_call_a);
		const call_chain::Site sight_b = call_chain::inspect(a.fp_sight_block_call_b);
		const call_chain::Site shot = call_chain::inspect(a.shot_pitch_call);
		call_chain::log_site("Scopes", a.aim_cull_model_call, kCull, cull);
		call_chain::log_site("Scopes", a.aim_scope_mod_call, kMod, mod);
		call_chain::log_site("Scopes", a.fov_zoom_aiming_call, kZoom, zoom);
		call_chain::log_site("Scopes", a.hud_sighting_call, kHud, hud);
		call_chain::log_site("Scopes", a.nv_scope_flag_call, kNv, nv);
		call_chain::log_site("Scopes", a.fp_sight_block_call_a, kSightA, sight_a);
		call_chain::log_site("Scopes", a.fp_sight_block_call_b, kSightB, sight_b);
		call_chain::log_site("Scopes", a.shot_pitch_call, kShot, shot);
		if (!entries || cull.state == call_chain::State::Foreign || mod.state == call_chain::State::Foreign ||
			zoom.state == call_chain::State::Foreign)
		{
			log("hooks skipped (the code is patched by another module): scopes are the game's own",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		const auto usable = [](const call_chain::Site& site, const char* feature) {
			if (site.state != call_chain::State::Foreign) return true;
			log(std::format("{} stays off: another module holds its code (see above)", feature),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return false;
		};
		const bool hud_ok = usable(hud, "the crosshair's hiding at aim start (the per-frame hiding stays)");
		const bool nv_ok = usable(nv, "keeping the game's scope night-vision effect off under a lens");
		const bool sight_ok = usable(sight_a, "the eye alignment while sitting or in furniture") &&
			usable(sight_b, "the eye alignment while sitting or in furniture");
		const bool shot_ok = usable(shot, "AimMode realistic (shots stay on the view; the lens follows the view)");
		if (!poll_ok)
			log("the mouse-wheel zoom stays off: another module holds OSInputGlobals::Poll (see above)",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		g_cull_target = reinterpret_cast<ModelTest_t>(cull.target);
		g_scope_mod_target = reinterpret_cast<WeapTest_t>(mod.target);
		g_zoom_target = reinterpret_cast<ActorTest_t>(zoom.target);
		g_hud_sighting_target = reinterpret_cast<ActorNode_t>(hud.target);
		g_nv_flag_target = reinterpret_cast<WeapTest_t>(nv.target);
		g_sight_block_a_target = reinterpret_cast<ActorTest_t>(sight_a.target);
		g_sight_block_b_target = reinterpret_cast<ActorTest_t>(sight_b.target);
		g_pitch_target = reinterpret_cast<Pitch_t>(shot.target);

		// g_hooked stays false until every write is in, so a failure leaves
		// each hook passing through.
		if (!call_chain::hook_entry(a.scope_model_attach, reinterpret_cast<void*>(&attach_hk),
				reinterpret_cast<void**>(&g_orig_attach), "Scopes", kAttach, kIfReplaced) ||
			!call_chain::hook_entry(a.scope_set_visible, reinterpret_cast<void*>(&set_visible_hk),
				reinterpret_cast<void**>(&g_orig_set_visible), "Scopes", kVisible, kIfReplaced) ||
			!call_chain::hook_entry(a.weap_use_1st_is_anims, reinterpret_cast<void*>(&use_1st_is_hk),
				reinterpret_cast<void**>(&g_orig_use_is), "Scopes", kUseIs, kIfReplaced) ||
			!patch_call(a.aim_cull_model_call, cull, reinterpret_cast<uintptr_t>(&aim_cull_model_hk), kCull) ||
			!patch_call(a.aim_scope_mod_call, mod, reinterpret_cast<uintptr_t>(&aim_scope_mod_hk), kMod) ||
			!patch_call(a.fov_zoom_aiming_call, zoom, reinterpret_cast<uintptr_t>(&zoom_aiming_hk), kZoom))
		{
			log("hooks incomplete: scopes are the game's own", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		// The optional hooks: a write that fails leaves only its feature off.
		if (poll_ok && !call_chain::hook_entry(a.input_poll, reinterpret_cast<void*>(&poll_hk),
				reinterpret_cast<void**>(&g_orig_poll), "Scopes", kPoll, kIfReplaced))
			log("the mouse-wheel zoom stays off (OSInputGlobals::Poll not hooked)", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		if (hud_ok) patch_call(a.hud_sighting_call, hud, reinterpret_cast<uintptr_t>(&hud_sighting_hk), kHud);
		if (nv_ok) patch_call(a.nv_scope_flag_call, nv, reinterpret_cast<uintptr_t>(&nv_flag_hk), kNv);
		if (sight_ok && patch_call(a.fp_sight_block_call_a, sight_a, reinterpret_cast<uintptr_t>(&sight_block_a_hk), kSightA))
			patch_call(a.fp_sight_block_call_b, sight_b, reinterpret_cast<uintptr_t>(&sight_block_b_hk), kSightB);
		g_shot_hooked = shot_ok && patch_call(a.shot_pitch_call, shot, reinterpret_cast<uintptr_t>(&shot_pitch_thunk), kShot);
		g_hooked = true;
		if (hook_xinput())
		{
			call_chain::Site prev;
			prev.state = call_chain::State::Vanilla;
			prev.target = reinterpret_cast<uintptr_t>(g_orig_xinput);
			call_chain::track(call_chain::Kind::Slot, g_xinput_slot, reinterpret_cast<uintptr_t>(&xinput_hk), prev,
				"Scopes", kXInput, "the gamepad zooms the lens only while that slot still calls the wrapper");
		}
		else log("no gamepad hook: the game imports no XInputGetState (xinput1_3.dll ordinal 2), so the gamepad does "
			"not zoom the lens");
		log(std::format("hooks installed (scope attach @0x{:X}, scoped state @0x{:X}, ironsights anims @0x{:X}, "
			"SetAiming @0x{:X} / @0x{:X}, UpdateFOV @0x{:X}, HUD reticle @0x{:X}, scope night vision @0x{:X}, input "
			"poll @0x{:X}); active once the bridge forwards CameraInfoLensEXT", a.scope_model_attach.addr,
			a.scope_set_visible.addr, a.weap_use_1st_is_anims.addr, a.aim_cull_model_call.addr,
			a.aim_scope_mod_call.addr, a.fov_zoom_aiming_call.addr, a.hud_sighting_call.addr,
			a.nv_scope_flag_call.addr, a.input_poll.addr),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS);
	}

	bool hooks_installed()
	{
		return g_hooked;
	}

	bool lens(remixapi_CameraInfoLensEXT& out)
	{
		Held h;
		data::Lens l;
		const bool have = held(h);
		if (!have) h = Held{};
		const bool lens_held = have && held_lens(h, &l);
		// The weapon motion runs for every first-person aim, scope or not.
		motion_update(h, lens_held && active());
		if (!active()) return false;
		if (!have || h.third_person || !lens_held) return false;
		// Aimed, or at the hip with LensAlways (scopes only: binoculars are
		// a lens only while aimed).
		const bool aimed = h.aiming;
		if (!aimed && (h.optic || !hip_lens_allowed())) return false;
		if (aimed && !lens_aimed_recent()) aim_in_begin();
		g_aim_dbg.aligned = false;
		g_aim_dbg.weight = 1.0;

		static const double kZero[3] = { 0.0, 0.0, 0.0 };
		const double* d = stable_origin::active() ? stable_origin::shift() : kZero;
		const double s = stable_origin::scale();

		// The lens's place in the render frame of the first-person draws, and
		// render units per entry unit.
		V3 lens_eyepiece = { 0.0, 0.0, 0.0 }, lens_axis = { 0.0, 0.0, 1.0 }, lens_up = { 0.0, 1.0, 0.0 };
		double k = 1.0;
		const float* sight = nullptr;
		if (h.optic)
		{
			// Binoculars: one camera-attached lens with the binocular aperture
			// (binocular_place); no alignment point, no aim-in.
			set_sighting(h.player, false);
			g_aim_in.active = false;
			if (!binocular_place(d, s, l, lens_eyepiece, lens_axis, lens_up)) return false;
		}
		else
		{
			Pose pose;
			if (!weapon_pose(h, l.node, pose)) return false;

			// The entry in world space (game units).
			V3 axis_local = v3(l.axis);
			if (!unit(axis_local)) return false;
			V3 eye = to_world(pose, sub(v3(l.eyepiece), mul(axis_local, l.eye_relief)));

			// The last first-person draw's pose of the node, relative to its
			// camera and turned with this frame's VIEW; with it the eye point too
			// comes from the main camera node and this frame's turn.
			Pose rel;
			V3 eye_from_main = { 0.0, 0.0, 0.0 };
			Pose main_now = {};
			const bool measured = first_person_pose(h, rel, eye_from_main) && main_camera_pose(main_now);
			// At the hip the lens needs the weapon drawn in first person (a
			// measurement from this frame or the last).
			if (!aimed && !measured) return false;
			if (measured) eye = add(v3(main_now.pos), eye_from_main);
			// The aim-in's weight, from the angle between the scope's axis (as
			// the game poses the gun) and the view.
			double aim_weight = aimed ? 1.0 : 0.0;
			if (const float* view_now = aimed ? game::get_renderer_matrix(RENDERER_VIEW_OFF) : nullptr)
			{
				V3 scope_dir = rotate(measured ? rel : pose, v3(l.axis));
				V3 view_dir = { view_now[2], view_now[6], view_now[10] };
				if (unit(scope_dir) && unit(view_dir))
				{
					const double c = std::clamp(scope_dir.x * view_dir.x + scope_dir.y * view_dir.y +
						scope_dir.z * view_dir.z, -1.0, 1.0);
					aim_weight = aim_in_update(std::acos(c) * 57.2957795130823);
					// AimMode view sends the lens along the view (by the weight):
					// the eye point is taken along that same axis, so the eye sits
					// on the lens's exit pupil at whatever angle the scope settles.
					if (aim_mode() == kAimView)
					{
						V3 sent = add(mul(scope_dir, 1.0 - aim_weight), mul(view_dir, aim_weight));
						if (unit(sent))
							eye = add(eye, mul(sub(scope_dir, sent), l.eye_relief * (measured ? rel.scale : pose.scale)));
					}
				}
			}
			// The weapon motion moves the eye point with the gun; the alignment
			// takes the posed eye point and the moved one apart.
			V3 eye_moved = eye;
			if (g_motion.active)
			{
				Pose mc = {};
				if (measured ? (mc = main_now, true) : main_camera_pose(mc))
					eye_moved = add(v3(mc.pos), motion_point(sub(eye, v3(mc.pos))));
			}

			// The first-person camera this frame: at the alignment point, at a
			// sighting node the engine set, or at the main camera. The first-person
			// draws go out relative to it, with D, so the lens does too.
			const bool align = cfg().align_aim && aimed;
			if (align)
			{
				const UINT frame = api::frame();
				const bool first = g_sight_frame != frame && g_sight_frame != frame - 1;
				const V3 at = alignment_point(eye, eye_moved, first, aim_weight);
				const float e[3] = { static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(at.z) };
				std::memcpy(g_sight + AV_WORLD_POS_OFF, e, sizeof(e));
				g_sight_frame = frame;
			}
			sight = set_sighting(h.player, align);
			const float* main_cam = game::camera_position();
			if (!sight && !main_cam) return false;
			const V3 fp_cam = v3(sight ? sight : main_cam);
			{
				Pose mref = {};
				if (measured ? (mref = main_now, true) : main_camera_pose(mref))
					g_motion.main_offset = sub(v3(mref.pos), fp_cam);
			}

			// The lens in the render frame of the first-person draws: s * (point -
			// first-person camera + D). With a measurement, from the node's pose
			// relative to the camera the first-person pass used (so turning keeps
			// lens and gun together); on an aim's first frame, from this frame's
			// world pose and the predicted camera.
			const Pose& place = measured ? rel : pose;
			const V3 origin = measured ? V3{ 0.0, 0.0, 0.0 } : fp_cam;
			const auto render = [&](V3 p) { return mul(add(sub(p, origin), V3{ d[0], d[1], d[2] }), s); };
			lens_axis = rotate(place, v3(l.axis));
			lens_up = rotate(place, v3(l.up));
			// A player's shot in first person flies along the player's view
			// angles (0x5237B0: heading from actor vtable +0x2BC, pitch
			// 0x931D70), from the camera while a scope is aimed (0x52376A), not
			// along the gun. AimMode view: the lens's axis turns from the
			// scope's onto the view's forward by the aim-in's weight, so once
			// aimed the magnified center and the reticle mark where the shot
			// goes (at the hip it is the scope's own). AimMode realistic: the
			// lens follows the scope, and once locked the shot is turned onto
			// the scope's axis from the scope's objective (shot_override), so
			// the reticle marks where it goes.
			if (const float* view = aimed ? game::get_renderer_matrix(RENDERER_VIEW_OFF) : nullptr)
			{
				V3 aim = { view[2], view[6], view[10] };
				V3 aim_up = { view[1], view[5], view[9] };
				if (unit(aim) && unit(aim_up))
				{
					if (aim_mode() == kAimView && unit(lens_axis))
					{
						lens_axis = add(mul(lens_axis, 1.0 - aim_weight), mul(aim, aim_weight));
						lens_up = add(mul(lens_up, 1.0 - aim_weight), mul(aim_up, aim_weight));
					}
					else if (!g_aim_in.active)
					{
						V3 scope_axis = motion_turn_if_active(lens_axis);
						const V3 objective_local = add(v3(l.eyepiece), mul(axis_local, l.objective_distance));
						const V3 objective = measured ? add(fp_cam, to_world(rel, objective_local))
							: to_world(pose, objective_local);
						Pose mcam = {};
						if (unit(scope_axis) && (measured ? (mcam = main_now, true) : main_camera_pose(mcam)))
						{
							g_shot.axis = scope_axis;
							g_shot.view = aim;
							g_shot.from_main = motion_point(sub(objective, v3(mcam.pos)));
							g_shot.frame = api::frame();
						}
					}
				}
			}
			// The weapon motion turns the lens with the gun (in AimMode view the
			// lens keeps looking along the view).
			if (aim_mode() != kAimView)
			{
				lens_axis = motion_turn_if_active(lens_axis);
				lens_up = motion_turn_if_active(lens_up);
			}
			if (!unit(lens_axis) || !unit(lens_up)) return false;
			lens_eyepiece = motion_render_point(render(to_world(place, v3(l.eyepiece))));
			k = s * place.scale;   // weapon-space lengths to render units
		}

		// The game's own zoom for the weapon, unless the entry sets one.
		constexpr float kDegToRad = 0.0174532925f;
		const float world_fov = default_world_fov();
		float magnification = l.magnification;
		if (!(magnification > 0.0f))
		{
			magnification = 1.0f;
			if (h.fov > kNoZoomFov && h.fov < world_fov)
				magnification = std::tan(0.5f * world_fov * kDegToRad) / std::tan(0.5f * h.fov * kDegToRad);
		}
		if (!std::isfinite(magnification) || magnification < 1.0f) magnification = 1.0f;

		// The wheel's zoom range: the entry's, else fixed. A new weapon, new
		// mods or a new aim starts at the default magnification and the
		// weapon's own vision mode.
		float zoom_lo = magnification, zoom_hi = magnification;
		if (l.max_magnification > 0.0f)
		{
			zoom_hi = (std::max)(1.0f, l.max_magnification);
			zoom_lo = l.min_magnification > 0.0f ? (std::max)(1.0f, l.min_magnification) : magnification;
			if (zoom_lo > zoom_hi) std::swap(zoom_lo, zoom_hi);
		}
		else if (cfg().variable_zoom_all && l.kind == 0)
		{
			zoom_lo = magnification;
			zoom_hi = magnification * kVariableZoomHigh;
		}
		const int vision_default = l.vision_mode >= 0 ? std::clamp(l.vision_mode, 0, 2) : (h.night_vision ? 1 : 0);
		if (!lens_recent() || h.form_id != g_state_form || h.mods != g_state_mods)
		{
			g_state_form = h.form_id;
			g_state_mods = h.mods;
			g_zoom = std::clamp(magnification, zoom_lo, zoom_hi);
			g_vision = vision_default;
			g_wheel = 0;
		}
		g_vision_default = vision_default;
		// The modes the vision key cycles: all three with VisionAllModes, else
		// the entry's VisionModes (with its starting mode first when the list
		// leaves it out), else none and the starting mode.
		g_vision_count = 0;
		if (cfg().vision_all_modes)
			for (uint8_t m = 0; m < 3; m++) g_vision_list[g_vision_count++] = m;
		else if (l.vision_count > 0)
		{
			bool has_default = false;
			for (int i = 0; i < l.vision_count; i++) has_default = has_default || l.vision_list[i] == vision_default;
			if (!has_default) g_vision_list[g_vision_count++] = static_cast<uint8_t>(vision_default);
			for (int i = 0; i < l.vision_count && g_vision_count < 3; i++) g_vision_list[g_vision_count++] = l.vision_list[i];
		}
		else
		{
			g_vision_list[g_vision_count++] = 0;
			if (vision_default != 0) g_vision_list[g_vision_count++] = static_cast<uint8_t>(vision_default);
		}
		const float zoom_was = g_zoom;
		// In night vision the brightness notches set the gain (thermal has
		// none); otherwise they zoom like the others.
		if (g_gain_wheel && g_vision != 1) g_wheel += g_gain_wheel;
		else if (g_gain_wheel)
		{
			float& gain = g_gain.try_emplace(h.form_id, 1.0f).first->second;
			const float was = gain;
			gain = std::clamp(static_cast<float>(gain * std::pow(kGainStepPerNotch, static_cast<double>(g_gain_wheel) /
				120.0)), kGainMin, kGainMax);
			if (gain != was) play_sound(kSoundZoom);
		}
		g_gain_wheel = 0;
		if (g_wheel)
		{
			g_zoom = static_cast<float>(g_zoom * std::pow(kZoomStepPerNotch, static_cast<double>(g_wheel) / 120.0));
			g_wheel = 0;
		}
		g_zoom = std::clamp(std::isfinite(g_zoom) ? g_zoom : magnification, zoom_lo, zoom_hi);
		if (std::fabs(g_zoom - zoom_was) > 1e-4f) play_sound(kSoundZoom);
		// The aim-only features follow `locked`: the aim-in is done.
		const bool locked = aimed && !g_aim_in.active;
		g_lens_frame = api::frame();
		if (aimed) g_lens_aimed_frame = g_lens_frame;
		if (locked) g_lens_locked_frame = g_lens_frame;
		g_tags_wanted = locked && (g_vision != 0 || l.highlight_hostiles);
		// The crosshair hides only while aimed (at the hip it is the game's).
		if (aimed) hide_reticle();
		else show_reticle();

		remixapi_CameraInfoLensEXT& lo = out;
		lo = {};
		lo.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_LENS_EXT;
		lo.pNext = nullptr;
		lo.id = kLensId;
		lo.eyepieceCenter = f3(lens_eyepiece);
		lo.axis = f3(lens_axis);
		lo.up = f3(lens_up);
		lo.eyepieceRadius = static_cast<float>(l.eyepiece_radius * k);
		lo.objectiveDistance = static_cast<float>(l.objective_distance * k);
		lo.objectiveRadius = static_cast<float>(l.objective_radius * k);
		lo.magnification = g_zoom;
		g_aim_dbg.pending = cfg().aim_debug_log && aimed;
		lo.fieldStopDegrees = l.field_stop;
		lo.eyeRelief = static_cast<float>(l.eye_relief * k);
		lo.tint = { l.tint[0], l.tint[1], l.tint[2] };
		lo.reticle = static_cast<uint32_t>(std::clamp(l.reticle, 0, data::kReticleCount - 1));
		lo.reticleColor = { l.reticle_color[0], l.reticle_color[1], l.reticle_color[2] };
		lo.reticleRadiance = l.reticle_radiance;
		lo.reticleLineWidth = l.reticle_line_width;
		lo.kind = static_cast<uint32_t>(std::clamp(l.kind, 0, 1));
		lo.aperture = static_cast<uint32_t>(std::clamp(l.aperture, 0, 2));
		lo.apertureAspect = l.aperture_aspect > 0.0f ? l.aperture_aspect : 1.0f;
		lo.visionMode = locked ? static_cast<uint32_t>(std::clamp(g_vision, 0, 2)) : 0u;
		lo.lensFlags = locked && l.highlight_hostiles ? REMIXAPI_CAMERA_LENS_FLAG_HIGHLIGHT_HOSTILES : 0u;
		// Binoculars show only their eyepieces; the body around them is black.
		if (h.optic) lo.lensFlags |= REMIXAPI_CAMERA_LENS_FLAG_OCCLUDE_OUTSIDE;
		lo.minMagnification = zoom_hi > zoom_lo ? zoom_lo : 0.0f;
		lo.reticleAccentColor = { l.reticle_accent_color[0], l.reticle_accent_color[1], l.reticle_accent_color[2] };
		lo.reticleAccentRadiance = l.reticle_accent_radiance;
		lo.edgeGlowColor = { l.edge_glow_color[0], l.edge_glow_color[1], l.edge_glow_color[2] };
		lo.edgeGlowStrength = l.edge_glow_strength;
		lo.edgeShade = std::clamp(l.edge_shade, 0.0f, 1.0f);
		lo.edgeTint = { l.edge_tint[0], l.edge_tint[1], l.edge_tint[2] };
		lo.nightTint = { l.night_tint[0], l.night_tint[1], l.night_tint[2] };
		{
			const auto git = g_gain.find(h.form_id);
			const float factor = git != g_gain.end() ? git->second : 1.0f;
			lo.nightGain = factor == 1.0f ? l.night_gain : (l.night_gain > 0.0f ? l.night_gain : 1.0f) * factor;
		}
		// The labels are printed on the reticle; the rangefinder measures only
		// while locked.
		lo.markings = static_cast<uint32_t>(l.markings & 1) |
			(locked ? static_cast<uint32_t>(l.markings & 2) | (cfg().rangefinder_all ? 2u : 0u) : 0u);
		if (g_texture_supported)
		{
			// First focal plane: the reticle keeps its size at the default
			// magnification and grows with the zoom from there.
			if (l.reticle_plane == 1)
			{
				lo.lensFlags |= REMIXAPI_CAMERA_LENS_FLAG_FIRST_FOCAL_PLANE;
				lo.referenceMagnification = magnification;
			}
			if (const wchar_t* tex = reticle_texture_path(l.reticle_texture))
			{
				lo.reticle = REMIXAPI_CAMERA_LENS_RETICLE_TEXTURE;
				lo.reticleTexture = tex;
			}
		}
		if (g_marking_color_supported)
		{
			// Markings take the entry's color, else [Scopes] MarkingColor.
			const float* mc = l.marking_color_set ? l.marking_color : cfg().marking_color;
			lo.markingColor = { mc[0], mc[1], mc[2] };
			lo.markingRadiance = l.marking_color_set ? l.marking_radiance : cfg().marking_radiance;
			lo.markingOpacity = std::clamp(l.marking_opacity, 0.0f, 1.0f);
		}
		if (g_glass_supported)
		{
			// Parallax: the entry's distance, else 100 m on telescopic scopes
			// and none on reflex sights; meters to render units.
			const double meters = l.parallax_m >= 0.0f ? l.parallax_m : (l.kind == 0 ? kParallaxDefaultM : 0.0);
			lo.parallaxDistance = cfg().parallax ? static_cast<float>(meters * 100.0 * s / kCmPerUnit) : 0.0f;
			lo.glareStrength = cfg().glass_effects ? std::clamp(l.glare >= 0.0f ? l.glare : cfg().glare_strength, 0.0f,
				1.0f) : 0.0f;
			lo.dirtStrength = cfg().glass_effects ? std::clamp(l.dirt >= 0.0f ? l.dirt : cfg().dirt_strength, 0.0f, 1.0f)
				: 0.0f;
			// Grain = off: no film grain on the lens (runtimes without the
			// bit ignore it).
			if (!l.grain) lo.lensFlags |= REMIXAPI_CAMERA_LENS_FLAG_NO_GRAIN;
		}

		auto& bridge = shared::common::remix_api::get().m_bridge;
		if (!g_pushed_enable && bridge.SetConfigVariable)
			g_pushed_enable = bridge.SetConfigVariable("rtx.lens.enable", "True") == REMIXAPI_ERROR_CODE_SUCCESS;

		if (h.form_id != g_logged_form || h.mods != g_logged_mods)
		{
			g_logged_form = h.form_id;
			g_logged_mods = h.mods;
			const std::string* edid = data::editor_id(h.form_id);
			log(std::format("lens for {} (mods {}): magnification {:.2f}, eyepiece radius {:.3f}, first-person camera at "
				"{}{}", edid ? *edid : std::format("{:08X}", h.form_id), mods_text(h.mods), magnification,
				lo.eyepieceRadius, h.optic ? "the main camera (binoculars)" : sight ?
				(cfg().align_aim ? "the eye point" : "the engine's sighting node") : "the main camera",
				g_faults ? std::format("; {} reads faulted since startup", g_faults) : std::string()));
		}
		return true;
	}

	void on_frame_end()
	{
		if (!g_hooked) return;
		// The frame counter has already moved on to the next frame here.
		if (g_sight_frame != api::frame() - 1)
		{
			Held h;
			if (held(h)) set_sighting(h.player, false);
		}
		const UINT last = api::frame() - 1;
		if (g_lens_frame != last) show_reticle();
		if (g_lens_locked_frame != last) g_tags_wanted = false;
		vision_key(g_lens_aimed_frame == last);
		force_first_person();
		push_blur();
		build_tags();
		editor_follow();
	}

	void lens_sent(const D3DMATRIX& view, const remixapi_CameraInfoLensEXT& lens)
	{
		if (!g_aim_dbg.pending) return;
		g_aim_dbg.pending = false;
		// The camera's position from the view it went out with (row vectors:
		// p * R + t = 0), and the eye's offset from the lens's exit-pupil
		// center (eyepieceCenter - axis * eyeRelief), along the axis (+ toward
		// the eyepiece) and across it, in game units.
		const V3 t = { view.m[3][0], view.m[3][1], view.m[3][2] };
		V3 cam = { 0.0, 0.0, 0.0 };
		cam.x = -(t.x * view.m[0][0] + t.y * view.m[0][1] + t.z * view.m[0][2]);
		cam.y = -(t.x * view.m[1][0] + t.y * view.m[1][1] + t.z * view.m[1][2]);
		cam.z = -(t.x * view.m[2][0] + t.y * view.m[2][1] + t.z * view.m[2][2]);
		V3 axis = { lens.axis.x, lens.axis.y, lens.axis.z };
		double axial = 0.0, lateral = 0.0;
		if (unit(axis))
		{
			const V3 pupil = sub(V3{ lens.eyepieceCenter.x, lens.eyepieceCenter.y, lens.eyepieceCenter.z },
				mul(axis, lens.eyeRelief));
			const V3 off = mul(sub(cam, pupil), 1.0 / stable_origin::scale());
			axial = off.x * axis.x + off.y * axis.y + off.z * axis.z;
			lateral = len(sub(off, mul(axis, axial)));
		}
		aim_debug_line(lens.magnification, axial, lateral);
	}

	void motion_apply(remixapi_Transform& t)
	{
		const Motion& m = g_motion;
		if (!m.active || m.frame != api::frame()) return;
		remixapi_Transform out = t;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				out.matrix[r][c] = static_cast<float>(m.rot[r * 3 + 0] * t.matrix[0][c] + m.rot[r * 3 + 1] * t.matrix[1][c] +
					m.rot[r * 3 + 2] * t.matrix[2][c]);
		const V3 moved = motion_render_point(V3{ t.matrix[0][3], t.matrix[1][3], t.matrix[2][3] });
		out.matrix[0][3] = static_cast<float>(moved.x);
		out.matrix[1][3] = static_cast<float>(moved.y);
		out.matrix[2][3] = static_cast<float>(moved.z);
		t = out;
	}

	void on_first_person_draw()
	{
		if (g_hooked) measure_first_person_camera();
	}

	uint32_t actor_tags(uintptr_t geometry)
	{
		if (!geometry || g_tags.empty()) return 0;
		const UINT now = api::frame();
		if (now != g_tag_cache_frame)
		{
			g_tag_cache_frame = now;
			g_tag_cache.clear();
		}
		if (const auto it = g_tag_cache.find(geometry); it != g_tag_cache.end()) return it->second;
		uint32_t tags = 0;
		uintptr_t obj = geometry;
		for (int depth = 0; obj && depth < kMaxParentDepth; ++depth)
		{
			if (const auto it = g_tags.find(obj); it != g_tags.end()) { tags = it->second; break; }
			uintptr_t parent = 0;
			uint32_t flags = 0;
			if (!scene_graph::read_link(obj, parent, flags)) break;
			obj = parent;
		}
		g_tag_cache[geometry] = tags;
		return tags;
	}

	namespace editor
	{
		Fields& fields()
		{
			return g_fields;
		}

		void changed()
		{
			if (!g_ed_target) return;
			const int section = g_fields.combination ? g_ed_mods : data::kBase;
			if (!data::set(g_ed_form, section, lens_from_fields())) return;
			update_status();
		}

		void save()
		{
			if (!g_ed_target)
			{
				set_status("no scoped weapon equipped: nothing to save");
				return;
			}
			std::string why;
			if (data::save(g_ed_form, why)) update_status();
			else set_status("not saved: " + why);
		}

		void reload()
		{
			data::reload();
			g_ed_resync = true;
		}

		const char* status()
		{
			return g_status.c_str();
		}

		uint32_t status_generation()
		{
			return g_status_gen;
		}

		const char* const* node_labels(uint32_t& count)
		{
			count = g_node_count;
			return g_node_labels;
		}
	}
}
