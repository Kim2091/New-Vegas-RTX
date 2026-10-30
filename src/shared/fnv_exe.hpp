#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <iterator>

// FalloutNV.exe engine addresses, one row per supported executable. Shared by
// the wrapper (d3d9.dll) and the engine monitor plugin (fnv_remix_nvse.dll).
//
// The running executable picks its row once (identity()): the PE timestamp,
// the layout of the seven linker sections, an FNV-1a hash of .rsrc and the
// store DLL it imports. All four are read from the mapped image and none of
// them is encrypted by SteamStub, so the check works from DllMain onwards on
// every row, including the Steam executable before its entry point decrypts
// .text.
//
// An executable that matches no row gets no addresses: addr() is all zero,
// known() is false, and every engine feature stays off. Nothing guesses.
//
// The GOG and Steam 1.4.0.525 executables share one code layout: the Steam
// .text, decrypted, is byte-identical to the GOG .text, .data is identical,
// and .rdata differs only in the store DLL's import name. Both rows point at
// the same Addresses. The rows differ in steamstub: the Steam .text on disk is
// encrypted, so a disk comparison of .text means nothing there.

namespace fnv_exe
{
	// A code site and the bytes the executable holds there before anything
	// patches it.
	struct Code
	{
		uintptr_t addr;
		uint8_t len;
		uint8_t bytes[16];
	};

	struct Addresses
	{
		// ---- The wrapper's hook sites ----
		Code render_pass_dispatch;     // P1: per-pass render dispatcher (detour)
		Code ni_geometry_dtor;         // P2: ~NiGeometry (detour)
		Code grass_batch_append;       // P3: thiscall(batch, const float4*, u32), ret 8 (detour)
		Code grass_batch_remove_at;    // P3: thiscall(batch, u16), ret 4 (detour)
		Code grass_batch_destroy;      // P3: thiscall(batch) (detour)
		Code skin_path_jz;             // P5: jz +0x58 over the skinned path, written as jmp
		// The call sites below hold E8 rel32 calls; the hooks call what the
		// site calls when they are installed (comp/game/call_chain.hpp).
		Code skin_render_call;         // P5: call to the skinned render batch, redirected to a stub
		Code skin_reset_call;          // P5: call to the per-object bone setup, redirected to a stub
		Code set_d3d_texture;          // P4: NiDX9SourceTextureData::SetD3DTexture (detour)
		Code ui_boundary_call;         // UI boundary: Main_Render's call to the HUD's 3D render 0x709B40, before Interface_Render (thunk)
		Code world_scene_call_a;       // UI boundary: Main_Render's call to RenderWorldScene 0x8706B0, gameplay branch (thunk)
		Code world_scene_call_c;       // UI boundary: Main_Render's call to RenderWorldScene 0x8706B0, Pip-Boy branch (thunk)
		Code pipboy_screen_call;       // menu background: 0x871290's call to the Pip-Boy screen render 0x871A50 (guard)
		Code d3d9ex_failed_mark;       // NiDX9Renderer::CreateDirect3D9 0xE69460: `or esi, -1` after a failed Direct3DCreate9Ex, written as `or esi, 0`
		Code texture_from_file;        // NiDX9SourceTextureData's D3DX file load: thiscall(this, const char* path, renderer, prefs, BSFile*), ret 0x10 (detour)
		Code facegen_image_texture;    // BSFaceGenImage -> NiSourceTexture (EGT face maps built in memory, created and uploaded inside): thiscall(this, float, float, float, NiPixelFormat*), ret 0x10 (detour)
		Code vats_limb_visibility;     // VATSEffect's per-limb visibility from its occlusion queries: thiscall(this, int limb, float* out), ret 8 (detour)
		Code save_screenshot_call;     // the save header writer 0x84D4B0's call to the save screenshot capture 0x878F60: NiPixelData* cdecl(int* w, int* h, const char* bmp)
		Code screenshot_key_call;      // Main_OnIdle's call to the Print Screen capture 0x878860 (ScreenShotN.bmp from the back buffer): cdecl(int)

		// ---- Engine functions ----
		uintptr_t dec_ref_count;       // NiRefObject::DecRefCount
		uintptr_t actor_los_to_point;  // Actor's line of sight to a point: thiscall(actor, NiPoint3*, bool eye height, bool skip FOV), ret 0xC
		uintptr_t actor_eye_height;    // the height above an actor's position its line of sight starts at: float thiscall(actor, bool eye height), ret 4
		uintptr_t pick_bypass_get;     // the pick time budget's bypass flag: bool cdecl()
		uintptr_t pick_bypass_set;     // sets it: void cdecl(bool)
		uintptr_t ni_alloc;            // NiAlloc: void* cdecl(size_t)
		uintptr_t pixel_data_ctor;     // NiPixelData's constructor: thiscall(this, u32 w, u32 h, const NiPixelFormat*, u32 mips, u32 faces), 0x74 bytes

		// ---- xNVSE's event hook sites ----
		Code xnvse_cell_state;         // cell SetState (OnCellStateChange)
		Code xnvse_refs_loaded;        // all refs loaded (OnCellRefsLoaded)
		Code xnvse_set_3d;             // ref Set3D (OnRefSet3D)
		Code xnvse_unset_3d;           // ref Unset3D (OnRefUnset3D)
		Code xnvse_ref_attach;         // ref attach (OnRefAttach)
		Code xnvse_main_present;       // main present (OnFramePresent 0)
		Code xnvse_loading_present;    // loading present (OnFramePresent 1)
		Code xnvse_main_loop;          // main loop (MainGameLoop)

		// ---- The decal path ----
		Code decal_pass_builder;
		Code accum_draw_loop;
		Code accum_decal_test;
		Code zwrite_state;             // ZWRITE render-state wrapper
		Code depthbias_state;          // DEPTHBIAS render-state wrapper

		// ---- The menu-background capture ----
		Code menu_capture_call;        // call to the capture
		Code pipboy_request_test;
		Code pipboy_request_clear;
		Code pipboy_capture_call;      // call to the capture
		Code live_bg_test;             // live / frozen background test
		Code bg_ready_mark;            // the capture marks the background ready

		// ---- Globals ----
		uintptr_t tes;                 // TES*; its +0x68 is the Sky
		uintptr_t world_scene_graph;   // SceneGraph* "World"
		uintptr_t interface_manager;   // InterfaceManager*
		uintptr_t loading_menu;        // LoadingMenu*, non-null while it exists
		uintptr_t player;              // PlayerCharacter*
		uintptr_t shadow_scene_node;   // ShadowSceneNode*
		uintptr_t renderer;            // NiDX9Renderer*
		uintptr_t camera_position;     // NiPoint3, the renderer's camera position
		uintptr_t climate_hours;       // float[4], the climate's sunrise / sunset begin / end hours
		uintptr_t sun_alpha_trans_time;// float, fSunAlphaTransTime's value
		uintptr_t menu_capture_request;// u8, the Pip-Boy capture request
		uintptr_t menu_bg_captured;    // u8, the menu background is captured
		uintptr_t menu_bg_rt;          // BSRenderedTexture*, the menu background capture (render target type 6)
		uintptr_t water_underwater;    // u8, the camera is below the water surface (TESWaterSystem)
		uintptr_t water_in_placeable;  // u8, the camera is inside placeable water
		uintptr_t water_placeable_height; // float, that placeable water's surface Z
		uintptr_t water_system_on;     // u8, ToggleWaterSystem
		uintptr_t water_setting;       // u8, the water setting is on
		uintptr_t render_camera;       // NiCamera*, the camera of the scene being drawn (BSShader DrawScene)
		uintptr_t hud_3d_node;         // NiNode*, HUDMainMenu's 3D element (drawn every frame under the menu object scene node)
		uintptr_t interface_lock;      // CRITICAL_SECTION, the interface's lock (tiles, menus)
		uintptr_t data_handler;        // DataHandler*, the loaded plugin files and forms
		uintptr_t vats_targets;        // tList<VATSTargetInfo*>, the VATS menu's targets
		uintptr_t vats_target_ref;     // TESObjectREFR*, the VATS menu's current target
		uintptr_t screen_width;        // u32, iSize W's value
		uintptr_t screen_height;       // u32, iSize H's value
		uintptr_t pixel_format_rgb24;  // NiPixelFormat, RGB24
		uintptr_t moon_phase;          // u32, both moons' phase index (0 full, 2 last quarter, 4 new, 6 first quarter), set by Sky_Update 0x63AC70 once per day

		// ---- Vtables ----
		uintptr_t vtbl_ppl_shade;            // BSShaderPPLightingProperty
		uintptr_t vtbl_lighting30_shade;     // Lighting30ShaderProperty
		uintptr_t vtbl_lighting30_shade2;    // Lighting30ShaderProperty, second variant
		uintptr_t vtbl_no_lighting_shade;    // BSShaderNoLightingProperty
		uintptr_t vtbl_grass_tri_strips;     // TallGrassTriStrips
		uintptr_t vtbl_grass_tri_shape;      // TallGrassTriShape
		uintptr_t vtbl_tes_weather;          // TESWeather
		uintptr_t vtbl_source_texture_data;  // NiDX9SourceTextureData; slot +0xB0 is set_d3d_texture
		uintptr_t vtbl_player_character;     // PlayerCharacter
		uintptr_t vtbl_light_effect;         // LightEffect
		uintptr_t vtbl_ni_point_light;       // NiPointLight
		uintptr_t vtbl_ni_camera;            // NiCamera

		// ---- INI setting collections ----
		// Each holds a list of its Settings at +0x10C. The game reads Fallout.ini into the
		// first and FalloutPrefs.ini into the second before its first Direct3DCreate9.
		uintptr_t ini_settings;              // INISettingCollection* (Fallout.ini)
		uintptr_t renderer_settings;         // RendererSettingCollection* (FalloutPrefs.ini)
		uintptr_t vtbl_ini_settings;         // INISettingCollection
		uintptr_t vtbl_renderer_settings;    // RendererSettingCollection
		uintptr_t vtbl_ini_setting;          // SettingT<INISettingCollection>
		uintptr_t vtbl_renderer_setting;     // SettingT<RendererSettingCollection>
	};

	// FalloutNV.exe 1.4.0.525 (GOG and Steam).
	inline constexpr Addresses k1_4_0_525 = {
		// Hook sites
		{ 0x00B994F0, 10, { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x0F, 0xB7, 0x46, 0x04, 0x57 } },
		{ 0x00A80580, 10, { 0x53, 0x8B, 0x1D, 0xC4, 0xF1, 0xFD, 0x00, 0x56, 0x8B, 0xF1 } },
		{ 0x00BD8550, 8,  { 0x0F, 0xB7, 0x41, 0x12, 0x66, 0x3B, 0x41, 0x10 } },
		{ 0x00BD85B0, 9,  { 0x0F, 0xB7, 0x41, 0x12, 0x66, 0x8B, 0x54, 0x24, 0x04 } },
		{ 0x00BD8F90, 13, { 0x6A, 0xFF, 0x68, 0x49, 0xD2, 0xF2, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 } },
		{ 0x00B992F2, 2,  { 0x74, 0x58 } },
		{ 0x00B99598, 5,  { 0xE8, 0x73, 0xFB, 0xFF, 0xFF } },
		{ 0x00B991E7, 5,  { 0xE8, 0x64, 0x42, 0x8A, 0xFF } },
		{ 0x00E68530, 16, { 0x83, 0xEC, 0x64, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x74, 0x8B, 0x06, 0x57, 0x8B, 0xD9, 0x8B } },
		{ 0x00870403, 5,  { 0xE8, 0x38, 0x97, 0xE9, 0xFF } },
		{ 0x00870244, 5,  { 0xE8, 0x67, 0x04, 0x00, 0x00 } },
		{ 0x008702A9, 5,  { 0xE8, 0x02, 0x04, 0x00, 0x00 } },
		{ 0x008719F8, 5,  { 0xE8, 0x53, 0x00, 0x00, 0x00 } },
		{ 0x00E694A7, 3,  { 0x83, 0xCE, 0xFF } },
		{ 0x00E68A80, 6,  { 0x81, 0xEC, 0x50, 0x05, 0x00, 0x00 } },
		{ 0x0064CEB0, 6,  { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08 } },
		{ 0x00800A00, 6,  { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C } },
		{ 0x0084D6B2, 5,  { 0xE8, 0xA9, 0xB8, 0x02, 0x00 } },
		{ 0x00870545, 5,  { 0xE8, 0x16, 0x83, 0x00, 0x00 } },
		// Engine functions
		0x00401970, 0x0088C240, 0x008854D0, 0x00888960, 0x00C66230, 0x00AA13E0, 0x00A7C190,
		// xNVSE event hook sites
		{ 0x004512A0, 5, { 0x55, 0x8B, 0xEC, 0x51, 0x89 } },
		{ 0x005518A5, 5, { 0xE8, 0x26, 0x47, 0xED, 0xFF } },
		{ 0x00571090, 5, { 0xE8, 0xEB, 0xB0, 0x00, 0x00 } },
		{ 0x005710AC, 5, { 0xE8, 0x4F, 0xB2, 0x00, 0x00 } },
		{ 0x00549787, 5, { 0xE8, 0x54, 0x83, 0xFF, 0xFF } },
		{ 0x0087055E, 5, { 0xE8, 0xCD, 0xB1, 0x2F, 0x00 } },
		{ 0x007147C4, 5, { 0xE8, 0x67, 0x6F, 0x45, 0x00 } },
		{ 0x0086B386, 5, { 0xE8, 0x25, 0x11, 0xBD, 0xFF } },
		// Decal path
		{ 0x00BDF6C0, 5, { 0x56, 0x8B, 0xF1, 0xF7, 0x46 } },
		{ 0x00B9AF60, 5, { 0x83, 0xEC, 0x1C, 0x8B, 0x44 } },
		{ 0x00B9B1A2, 5, { 0xA9, 0x00, 0x00, 0x00, 0x04 } },
		{ 0x00B97E30, 5, { 0x56, 0x8B, 0x35, 0x68, 0xFA } },
		{ 0x00B98320, 5, { 0xD9, 0x44, 0x24, 0x04, 0x56 } },
		// Menu-background capture
		{ 0x0086F4E3, 5, { 0xE8, 0xD8, 0x28, 0x00, 0x00 } },
		{ 0x0086FF99, 5, { 0x0F, 0xB6, 0x05, 0x0A, 0x89 } },
		{ 0x0086FFA8, 5, { 0xC6, 0x05, 0x0A, 0x89, 0x1D } },
		{ 0x0087000F, 5, { 0xE8, 0xAC, 0x1D, 0x00, 0x00 } },
		{ 0x00870187, 5, { 0x0F, 0xB6, 0x15, 0x29, 0xEA } },
		{ 0x008722E9, 5, { 0xC6, 0x05, 0x29, 0xEA, 0x1D } },
		// Globals
		0x011DEA10, 0x011DEB7C, 0x011D8A80, 0x011DA0C0, 0x011DEA3C, 0x011F91C8,
		0x011C73B4, 0x011F8E9C, 0x011CA9E8, 0x011CCEA8, 0x011D890A, 0x011DEA29, 0x011DED3C,
		0x011C7A59, 0x011C7A58, 0x011C7A5C, 0x01189624, 0x011C7ADC, 0x011F917C, 0x011D9770,
		0x011F3330, 0x011C3F2C, 0x011DB150, 0x011F21CC, 0x0118947C, 0x01189480, 0x011AA2E8, 0x011CCA80,
		// Vtables
		0x010AE0D0, 0x010B9910, 0x010BCB60, 0x010AE670, 0x010BCC90, 0x010BCDC8,
		0x0103168C, 0x010ED37C, 0x0108AA3C, 0x0107A2CC, 0x0109DD0C, 0x0109CB9C,
		// INI setting collections
		0x011F96A0, 0x011F35A4, 0x0101772C, 0x01018508, 0x01017720, 0x010184FC,
	};

	// One linker section as the image maps it.
	struct Section
	{
		char name[8];
		uint32_t rva;
		uint32_t virtual_size;
	};

	// The 1.4.0.525 link: identical in the GOG and Steam executables. The
	// Steam one adds SteamStub's .bind after these.
	inline constexpr Section k1_4_0_525_sections[] = {
		{ ".text",  0x00001000, 0x00BDD58B },
		{ ".rdata", 0x00BDF000, 0x001A304C },
		{ ".data",  0x00D83000, 0x000EEA9C },
		{ ".tls",   0x00E72000, 0x000002C5 },
		{ "CONST",  0x00E73000, 0x0000001F },
		{ ".rsrc",  0x00E74000, 0x000CBA88 },
		{ ".reloc", 0x00F40000, 0x000C89E4 },
	};

	struct Variant
	{
		const char* name;
		uint32_t timestamp;            // IMAGE_FILE_HEADER::TimeDateStamp
		const Section* sections;
		uint32_t section_count;
		uint64_t rsrc_fnv1a;           // FNV-1a 64 of .rsrc (virtual size)
		const char* store_import;      // lower case
		const Addresses* addr;
	};

	inline constexpr Variant kVariants[] = {
		{ "FalloutNV.exe 1.4.0.525 (GOG)", 0x4E0D50ED, k1_4_0_525_sections,
			static_cast<uint32_t>(std::size(k1_4_0_525_sections)), 0xE86070488DAC3BFFull, "galaxywrp.dll", &k1_4_0_525 },
		{ "FalloutNV.exe 1.4.0.525 (Steam)", 0x4E0D50ED, k1_4_0_525_sections,
			static_cast<uint32_t>(std::size(k1_4_0_525_sections)), 0xE86070488DAC3BFFull, "steam_api.dll", &k1_4_0_525 },
	};

	// What the running executable is.
	struct Identity
	{
		const Variant* variant = nullptr;  // null: no row matches
		bool steamstub = false;            // SteamStub wraps the entry point: .text on disk is encrypted
		uint32_t timestamp = 0;
		uint32_t size_of_image = 0;
		uint32_t entry_point = 0;          // RVA
		uint64_t rsrc_fnv1a = 0;
		char store_import[32] = {};        // the store DLL it imports, lower case; empty if none
		char mismatch[96] = {};            // why no row matched
	};

	namespace detail
	{
		inline bool name_is(const BYTE (&name)[IMAGE_SIZEOF_SHORT_NAME], const char* want)
		{
			char n[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
			std::memcpy(n, name, IMAGE_SIZEOF_SHORT_NAME);
			return std::strcmp(n, want) == 0;
		}

		inline void lower_copy(char* out, size_t cap, const char* in)
		{
			size_t i = 0;
			for (; i + 1 < cap && in[i]; i++)
				out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? static_cast<char>(in[i] - 'A' + 'a') : in[i];
			out[i] = 0;
		}

		inline void set_mismatch(Identity& id, const char* why)
		{
			strncpy_s(id.mismatch, why, _TRUNCATE);
		}

		// `base`: a mapped image of the executable (its module handle).
		inline Identity detect(uintptr_t base)
		{
			Identity id;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
			{
				set_mismatch(id, "no DOS header");
				return id;
			}
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
			{
				set_mismatch(id, "no PE32 header");
				return id;
			}
			id.timestamp = nt->FileHeader.TimeDateStamp;
			id.size_of_image = nt->OptionalHeader.SizeOfImage;
			id.entry_point = nt->OptionalHeader.AddressOfEntryPoint;

			const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
			const WORD nsec = nt->FileHeader.NumberOfSections;
			for (WORD i = 0; i < nsec; i++)
			{
				const IMAGE_SECTION_HEADER& s = sec[i];
				if (name_is(s.Name, ".bind") && id.entry_point >= s.VirtualAddress &&
					id.entry_point < s.VirtualAddress + s.Misc.VirtualSize)
					id.steamstub = true;
				if (name_is(s.Name, ".rsrc") && s.VirtualAddress + s.Misc.VirtualSize <= id.size_of_image)
				{
					uint64_t h = 0xCBF29CE484222325ull;
					const auto* p = reinterpret_cast<const uint8_t*>(base + s.VirtualAddress);
					for (DWORD k = 0; k < s.Misc.VirtualSize; k++) h = (h ^ p[k]) * 0x100000001B3ull;
					id.rsrc_fnv1a = h;
				}
			}

			const IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (imp.VirtualAddress && imp.VirtualAddress + imp.Size <= id.size_of_image)
			{
				for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp.VirtualAddress);
					d->Name && d->Name < id.size_of_image; d++)
				{
					char dll[32] = {};
					lower_copy(dll, sizeof(dll), reinterpret_cast<const char*>(base + d->Name));
					for (const Variant& v : kVariants)
						if (std::strcmp(dll, v.store_import) == 0)
							std::memcpy(id.store_import, dll, sizeof(dll));
				}
			}

			for (const Variant& v : kVariants)
			{
				if (id.timestamp != v.timestamp) { set_mismatch(id, "PE timestamp"); continue; }
				if (nsec < v.section_count) { set_mismatch(id, "section count"); continue; }
				bool layout = true;
				for (uint32_t i = 0; i < v.section_count && layout; i++)
					layout = name_is(sec[i].Name, v.sections[i].name) && sec[i].VirtualAddress == v.sections[i].rva &&
						sec[i].Misc.VirtualSize == v.sections[i].virtual_size;
				if (!layout) { set_mismatch(id, "section layout"); continue; }
				if (id.rsrc_fnv1a != v.rsrc_fnv1a) { set_mismatch(id, ".rsrc hash"); continue; }
				if (std::strcmp(id.store_import, v.store_import) != 0) { set_mismatch(id, "store DLL import"); continue; }
				id.variant = &v;
				id.mismatch[0] = 0;
				break;
			}
			return id;
		}
	}

	// The running executable, detected on the first call.
	inline const Identity& identity()
	{
		static const Identity id = detail::detect(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
		return id;
	}

	// A matching row exists: the engine addresses are known.
	inline bool known()
	{
		return identity().variant != nullptr;
	}

	// The running executable's addresses; all zero when it is unknown.
	inline const Addresses& addr()
	{
		static constexpr Addresses kNone = {};
		const Variant* v = identity().variant;
		return v ? *v->addr : kNone;
	}
}
