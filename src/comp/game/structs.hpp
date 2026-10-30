#pragma once
#include "shared/fnv_exe.hpp"

namespace comp::game
{
	// NiDX9Renderer: the D3D matrices of the draw in progress.
	constexpr unsigned int RENDERER_WORLD_OFF = 0x940;
	constexpr unsigned int RENDERER_VIEW_OFF  = 0x980;
	constexpr unsigned int RENDERER_PROJ_OFF  = 0x9C0;

	// The material of the draw in progress: the renderer's property state,
	// that state's shade-property slot, and the shade property's type.
	constexpr unsigned int RENDERER_PROPERTY_STATE_OFF = 0x0C;
	constexpr unsigned int PROPERTY_STATE_SHADE_OFF    = 0x0C;
	constexpr unsigned int SHADE_PROPERTY_TYPE_OFF     = 0x1C;

	// Shade property types.
	constexpr unsigned int KPROP_SKY         = 0x0D;

	constexpr int MAX_EXTRACTED_LIGHTS   = 128;
	constexpr int MAX_FFP_BONES          = 48;

	// FNV sky chain.
	//
	// Pointer chain to the rendered moon body:
	//   TES (fnv_exe tes) → manager (singleton)
	//   +0x68      → sky_state
	//   +0x30      → masser_config (0x7c-byte struct, vtable 0x0104ED28)
	//   +0x10      → moon billboard NiAVObject (vtable 0x0109D454)
	//   +0x18 of billboard → orbit-root parent NiNode (vtable 0x0109B5AC)
	//
	// NiAVObject layout (FNV-specific, verified live):
	//   +0x18:        m_pkParent (NiNode*)
	//   +0x34..+0x67: m_kLocal  (NiTransform: 9 rot + 3 translate + 1 scale)
	//   +0x68..+0x9B: m_kWorld  (composed)
	//     +0x68..+0x8B: m_kWorld.m_Rotate    (3x3 row-major)
	//     +0x8C..+0x97: m_kWorld.m_Translate (NiPoint3, X/Y/Z world units)
	//     +0x98:        m_kWorld.m_fScale
	//
	// The orbit root sits at the camera/sky-anchor world position; the
	// billboard sits 512 game units away along its local +Y axis (the
	// orbital arm direction). The vector from parent.T to billboard.T,
	// normalized, is the world-space direction from the camera to the moon,
	// which gives elevation and rotation with no calibration.
	//
	// Bethesda axis convention: Z = up, X = east, Y = north. Azimuth is
	// measured clockwise from north (atan2(x, y)).
	inline constexpr uint32_t  WM_SKY_OFF          = 0x68;
	inline constexpr uint32_t  SKY_SUN_OFF         = 0x28;
	inline constexpr uint32_t  SKY_MASSER_OFF      = 0x30;
	inline constexpr uint32_t  MASSER_BILLBOARD_OFF = 0x10;
	// sun+0x10 / sun+0x14 are the sun's glare and disc geometry, not the arc
	// nodes (see the sun cycle notes below); the sun's direction is read from
	// SUN_ROOT_OFF.
	inline constexpr uint32_t  SUN_BILLBOARD_OFF   = 0x14;
	inline constexpr uint32_t  NIAV_PARENT_OFF     = 0x18;
	inline constexpr uint32_t  NIAV_KWORLD_T_OFF   = 0x8C;

	// ---- Sun day/night cycle (FalloutNV.exe 1.4.0.525) ----
	//
	// Sun::Update @0x641830 (its asserts carry "...\Fallout Shared\Sky\Sun.cpp",
	// line 0x12C) computes the sun's arc position analytically and writes it as the
	// local translate of the sun root node:
	//
	//   H = *(float*)(sky + 0xEC)                        Sky::GetGameHour @0x966A20
	//   A = (climT[1]+climT[0])/2 - fSunAlphaTransTime*0.5    sun at horizon, east
	//   B = (climT[3]+climT[2])/2 + fSunAlphaTransTime*0.5    sun at horizon, west
	//
	//   day    A<H<B :  t = (H-A)/(B-A)           s = 1 - 2t      (+1 -> -1)
	//   night  H>=B  :  t = (H-B)/(24-(B-A))      s = 2t - 1      (-1 -> +1)
	//   night  H<=A  :  t = (24-B+H)/(24-(B-A))   s = 2t - 1
	//
	//   X = fSunXExtreme * s        (fSunXExtreme = -400)
	//   Y = fSunYExtreme            (25, constant)
	//   Z = |fSunXExtreme| - |X|
	//
	// The "f" applied to X for the height term is fabs: the helper at 0x408860 tail-
	// calls 0x00EC6CDE, whose normal path (0x00EC6D77) masks the sign bit off the
	// high dword. The position is then handed to NiAVObject::SetLocalTranslate
	// @0x440460 (which writes 3 floats at node+0x58) on *(void**)(sun+0x04), the
	// disc root, and again on the glare root at sun+0x0C.
	//
	// Z is a function of |X| only. X sweeps -400 -> +400 across the day and
	// +400 -> -400 across the night, so Z traces the same above-horizon tent on
	// both halves. The engine never moves the sun below the horizon; it app-culls
	// the disc root at night (SetAppCulled on sun+0x04 @0x641C41). Mirroring the
	// node verbatim gives a sun that bounces off the horizon at A and B and stays
	// up all night. get_sun_orientation keeps the engine arc for the lit half and
	// mirrors Z for the dark half, which continues the engine's single-plane model
	// and is exact at dawn and dusk.
	//
	// sun+0x10 / sun+0x14 are not the arc nodes. They are child NiGeometry the
	// engine only queries for material fades (GetProperty(3) compared against
	// KPROP_SKY = 0x0D, @0x641EC8 / @0x641FC2).
	inline constexpr uint32_t  SKY_GAME_HOUR_OFF   = 0xEC;  // float, Sky::GetGameHour
	inline constexpr uint32_t  SUN_ROOT_OFF        = 0x04;  // NiPointer<NiNode>, positioned + culled
	inline constexpr uint32_t  NIAV_KLOCAL_T_OFF   = 0x58;  // m_kLocal.m_Translate

	// Cached climate transition hours, 4 contiguous floats: sunrise begin, sunrise
	// end, sunset begin, sunset end. Written by the Sky::GetClimateTime* accessors
	// @0x595EA0 / 0x595F50 / 0x595FC0 / 0x596030, which pull a raw byte off the
	// climate record and divide by 6.0 (Bethesda stores these in 10-minute units).
	// Sun::Update refreshes all four every frame, so these are never stale in-world.
	// The four floats are fnv_exe climate_hours. fnv_exe sun_alpha_trans_time is
	// the value of the setting fSunAlphaTransTime, at its Setting object +4
	// (Setting::GetValuePtr @0x403E20 returns this+4). Default 2.0.

	// The TES singleton the sky hangs off, or null when there is none yet or the
	// executable is unknown.
	inline void* world_manager()
	{
		const uintptr_t a = fnv_exe::addr().tes;
		return a ? *reinterpret_cast<void**>(a) : nullptr;
	}

	// True when `p` looks like a live engine object: non-null, aligned, and its
	// first dword (the vtable) points into FalloutNV.exe's image.
	//
	// The sky chain below is torn down and rebuilt on every worldspace change
	// (fast travel, door transitions, loading a save). A pointer read between the
	// free and the rebuild lands on freed heap whose first dword is the
	// allocator's free-list link (a heap address, never an image address), so
	// this rejects the stale object before anything is read through it. The
	// SEH wrappers in game.cpp catch whatever slips past.
	inline bool is_live_object(const void* p)
	{
		static uintptr_t s_lo = 0, s_hi = 0;
		if (!s_hi)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			s_lo = base;
			s_hi = base + nt->OptionalHeader.SizeOfImage;
		}

		const auto addr = reinterpret_cast<uintptr_t>(p);
		if (addr < 0x10000 || (addr & 3)) return false;
		const auto vtbl = *reinterpret_cast<const uintptr_t*>(addr);
		return vtbl >= s_lo && vtbl < s_hi;
	}

	// The highest address this process can use: 0x7FFEFFFF normally, and close to 4 GB for a
	// large-address-aware game (the 4GB patch) on 64-bit Windows, whose heap does go above 2 GB.
	inline uintptr_t user_address_limit()
	{
		static const uintptr_t limit = [] {
			SYSTEM_INFO si = {};
			GetSystemInfo(&si);
			return reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
		}();
		return limit;
	}

	// Plain pointer sanity for links in the sky chain that are not vtable-bearing
	// engine objects (the world-manager singleton, Sky, Sun), which
	// is_live_object would reject: non-null, aligned, user-mode. The SEH
	// wrappers in game.cpp catch a stale pointer that passes.
	inline bool is_valid_ptr(const void* p)
	{
		const auto addr = reinterpret_cast<uintptr_t>(p);
		return addr >= 0x10000 && addr <= user_address_limit() && !(addr & 3);
	}

	// The two hours at which the engine's sun arc crosses the horizon, matching
	// Sun::Update's own A/B bounds. False when the climate cache looks unpopulated
	// (main menu, or before the first sky tick).
	inline bool get_sun_horizon_hours(float& a_out, float& b_out)
	{
		const auto& addr = fnv_exe::addr();
		if (!addr.climate_hours || !addr.sun_alpha_trans_time) return false;
		const float* t = reinterpret_cast<const float*>(addr.climate_hours);
		for (int i = 0; i < 4; ++i)
			if (!(t[i] >= 0.0f) || t[i] > 24.0f) return false;

		const float sunrise = (t[1] + t[0]) * 0.5f;
		const float sunset  = (t[3] + t[2]) * 0.5f;
		if (!(sunset > sunrise)) return false;

		const float half = *reinterpret_cast<const float*>(addr.sun_alpha_trans_time) * 0.5f;
		a_out = sunrise - half;
		b_out = sunset  + half;
		return b_out > a_out;
	}

	// Reads the moon's rendered direction from the live scene graph and
	// converts to (elevation, rotation) in degrees. Returns false when any
	// pointer in the chain is null (main menu, interior cell without sky,
	// or sky tick hasn't run yet).
	inline bool get_masser_orientation(float& elev_deg_out, float& rot_deg_out)
	{
		auto* mgr = world_manager();
		if (!is_valid_ptr(mgr)) return false;
		auto* sky = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(mgr) + WM_SKY_OFF);
		if (!is_valid_ptr(sky)) return false;
		auto* masser = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(sky) + SKY_MASSER_OFF);
		if (!is_valid_ptr(masser)) return false;
		auto* billboard = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(masser) + MASSER_BILLBOARD_OFF);
		if (!is_live_object(billboard)) return false;
		auto* parent = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(billboard) + NIAV_PARENT_OFF);
		if (!is_live_object(parent)) return false;

		auto* moon_T   = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(billboard) + NIAV_KWORLD_T_OFF);
		auto* parent_T = reinterpret_cast<float*>(
			reinterpret_cast<uintptr_t>(parent) + NIAV_KWORLD_T_OFF);

		float dx = moon_T[0] - parent_T[0];
		float dy = moon_T[1] - parent_T[1];
		float dz = moon_T[2] - parent_T[2];
		float len = sqrtf(dx*dx + dy*dy + dz*dz);
		if (len < 1.0f) return false;
		dx /= len; dy /= len; dz /= len;

		constexpr float RAD_TO_DEG = 57.2957795f;
		elev_deg_out = asinf(dz) * RAD_TO_DEG;
		float rot = atan2f(dx, dy) * RAD_TO_DEG;
		if (rot < 0.0f) rot += 360.0f;
		rot_deg_out = rot;
		return true;
	}

	// Reads the sun's rendered direction and converts it to (elevation, rotation) in
	// degrees, mirroring the arc below the horizon across the dark half of the cycle.
	// See the block comment above SKY_GAME_HOUR_OFF for the derivation.
	// hour_out / night_out are for the caller's log line.
	inline bool get_sun_orientation(float& elev_deg_out, float& rot_deg_out,
	                                float* hour_out = nullptr, bool* night_out = nullptr)
	{
		auto* mgr = world_manager();
		if (!is_valid_ptr(mgr)) return false;
		auto* sky = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(mgr) + WM_SKY_OFF);
		if (!is_valid_ptr(sky)) return false;
		auto* sun = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(sky) + SKY_SUN_OFF);
		if (!is_valid_ptr(sun)) return false;

		// sun+0x04 is a NiPointer<NiNode>; NiPointer::operator* @0x559450 is a plain
		// single deref. This is the node Sun::Update positions.
		auto* root = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(sun) + SUN_ROOT_OFF);
		if (!is_live_object(root)) return false;

		// The world-space arm (root vs its parent) includes any rotation carried
		// by the sky root, so it matches the rendered sun. Without a parent, the
		// local translate Sun::Update wrote, which is correct even before this
		// frame's scene-graph update.
		float x, y, z;
		auto* parent = *reinterpret_cast<void**>(
			reinterpret_cast<uintptr_t>(root) + NIAV_PARENT_OFF);
		if (is_live_object(parent))
		{
			auto* root_T = reinterpret_cast<const float*>(
				reinterpret_cast<uintptr_t>(root) + NIAV_KWORLD_T_OFF);
			auto* par_T = reinterpret_cast<const float*>(
				reinterpret_cast<uintptr_t>(parent) + NIAV_KWORLD_T_OFF);
			x = root_T[0] - par_T[0];
			y = root_T[1] - par_T[1];
			z = root_T[2] - par_T[2];
		}
		else
		{
			auto* local_T = reinterpret_cast<const float*>(
				reinterpret_cast<uintptr_t>(root) + NIAV_KLOCAL_T_OFF);
			x = local_T[0]; y = local_T[1]; z = local_T[2];
		}

		const float hour = *reinterpret_cast<const float*>(
			reinterpret_cast<uintptr_t>(sky) + SKY_GAME_HOUR_OFF);
		if (!(hour >= 0.0f && hour <= 24.0f)) return false;  // also rejects NaN
		if (hour_out) *hour_out = hour;

		// Dark half: the engine retraces the daytime arc, so drive the sun under the
		// horizon instead of letting it climb back up.
		float a, b;
		const bool night = get_sun_horizon_hours(a, b) && (hour <= a || hour >= b);
		if (night) z = -z;
		if (night_out) *night_out = night;

		float len = sqrtf(x*x + y*y + z*z);
		if (len < 1.0f) return false;
		x /= len; y /= len; z /= len;

		constexpr float RAD_TO_DEG = 57.2957795f;
		elev_deg_out = asinf(z) * RAD_TO_DEG;
		float rot = atan2f(x, y) * RAD_TO_DEG;
		if (rot < 0.0f) rot += 360.0f;
		rot_deg_out = rot;
		return true;
	}

	// ---- Weather (FalloutNV.exe 1.4.0.525) ----
	//
	// Sky::UpdateWeather @0x63D1D0 picks the weather and runs the transition:
	// when a new weather starts it copies +0x10 to +0x14 and stores the new one
	// at +0x10, then each update sets +0xF4 = elapsed game hours / the incoming
	// weather's transition time, and clears +0x14 once that passes 1.0. With no
	// outgoing weather +0xF4 is 1.0. Sky::ForceWeather @0x63D0E0 (console `fw`)
	// writes +0x10, clears +0x14 and stores 1.0: a cut with no blend. `sw`
	// writes the default (+0x18) or override (+0x1C) weather, which then plays
	// out as a normal transition.
	inline constexpr uint32_t  SKY_WEATHER_CUR_OFF     = 0x10;   // TESWeather*, the current (incoming) weather
	inline constexpr uint32_t  SKY_WEATHER_OUT_OFF     = 0x14;   // TESWeather*, the outgoing weather; null outside a transition
	inline constexpr uint32_t  SKY_WEATHER_PERCENT_OFF = 0xF4;   // float 0..1, 1 = fully the current weather

	// TESWeather (0x36C bytes; TESWeather::LoadForm @0x580990). The main vtable
	// (fnv_exe vtbl_tes_weather) identifies a live weather form.
	inline constexpr uint32_t  TESFORM_FORMID_OFF      = 0x0C;
	inline constexpr uint32_t  WEATHER_DATA_OFF        = 0xE0;   // DATA, 15 bytes
	inline constexpr uint32_t  WEATHER_FOG_OFF         = 0xF0;   // FNAM, float[6]

	// One weather's classification inputs, copied out of the form. TESWeather
	// keeps no editor ID at runtime (its GetEditorID returns ""), so the form
	// ID is the only name it has.
	struct WeatherRecord
	{
		uint32_t form_id;
		uint8_t  wind;             // DATA[0], 0..255
		uint8_t  thunder_freq;     // DATA[10]; 255 in every vanilla record without thunder
		uint8_t  classification;   // DATA[11]: 1 pleasant, 2 cloudy, 4 rainy, 8 snow
		float    fog_day_far;      // FNAM[1], game units
		float    fog_night_far;    // FNAM[3]
	};

	inline bool read_weather_record(const void* weather, WeatherRecord& out)
	{
		if (!is_live_object(weather)) return false;
		if (*reinterpret_cast<const uintptr_t*>(weather) != fnv_exe::addr().vtbl_tes_weather) return false;
		const auto base = reinterpret_cast<uintptr_t>(weather);
		const auto* data = reinterpret_cast<const uint8_t*>(base + WEATHER_DATA_OFF);
		const auto* fog = reinterpret_cast<const float*>(base + WEATHER_FOG_OFF);
		out.form_id        = *reinterpret_cast<const uint32_t*>(base + TESFORM_FORMID_OFF);
		out.wind           = data[0];
		out.thunder_freq   = data[10];
		out.classification = data[11];
		out.fog_day_far    = fog[1];
		out.fog_night_far  = fog[3];
		return true;
	}

	// The sky's current and outgoing weather and the transition between them.
	struct WeatherState
	{
		bool          has_current;
		bool          has_outgoing;
		WeatherRecord current;
		WeatherRecord outgoing;
		float         percent;     // 0..1, 1 = fully the current weather
	};

	// False when there is no sky. A sky without a current weather (interiors)
	// reads back with has_current false.
	inline bool get_weather_state(WeatherState& out)
	{
		auto* mgr = world_manager();
		if (!is_valid_ptr(mgr)) return false;
		auto* sky = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(mgr) + WM_SKY_OFF);
		if (!is_valid_ptr(sky)) return false;
		const auto s = reinterpret_cast<uintptr_t>(sky);
		const void* cur = *reinterpret_cast<void* const*>(s + SKY_WEATHER_CUR_OFF);
		const void* old = *reinterpret_cast<void* const*>(s + SKY_WEATHER_OUT_OFF);
		out.has_current = cur && read_weather_record(cur, out.current);
		out.has_outgoing = out.has_current && old && old != cur && read_weather_record(old, out.outgoing);
		float pct = *reinterpret_cast<const float*>(s + SKY_WEATHER_PERCENT_OFF);
		if (!(pct >= 0.0f)) pct = 0.0f;   // also rejects NaN
		out.percent = pct > 1.0f ? 1.0f : pct;
		return true;
	}
}
