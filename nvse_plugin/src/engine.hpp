#pragma once
#include <cstdint>
#include "shared/fnv_exe.hpp"

// FalloutNV.exe 1.4.0.525 struct offsets the plugin uses, from static
// analysis of the executable. The plugin installs no hooks; its only writes
// are the decal flag (decals.cpp) and the fog cards' APP_CULLED bit
// (fog.cpp). The globals, vtables and functions are per executable, in the
// address table (shared/fnv_exe.hpp, fnv_exe::addr()).

namespace fnvr::engine
{
	// ---- TES / GridCellArray ----
	constexpr uint32_t TES_gridCells = 0x08;               // GridCellArray*
	constexpr uint32_t TES_interior = 0x34;                // TESObjectCELL* (the current interior)
	constexpr uint32_t GCA_centerX = 0x04;                 // i32
	constexpr uint32_t GCA_centerY = 0x08;                 // i32
	constexpr uint32_t GCA_dim = 0x0C;                     // u32
	constexpr uint32_t GCA_cells = 0x10;                   // TESObjectCELL** [dim * dim]

	// ---- TESForm / TESObjectCELL ----
	constexpr uint32_t FORM_type = 0x04;                   // u8
	constexpr uint32_t FORM_flags = 0x08;                  // 0x20 deleted, 0x800 disabled
	constexpr uint32_t FORM_refID = 0x0C;
	constexpr uint8_t kFormType_CELL = 0x39;
	constexpr uint32_t CELL_state = 0x26;                  // u8, nvse_abi::CellState
	constexpr uint32_t CELL_coords = 0x48;                 // exterior: {i32 x, i32 y}*
	// The cell's references: a list whose head node is in the cell,
	// {TESObjectREFR* data, node* next}, the next nodes {data, next}
	// (getter 0x9604F0, add 0x5AE3D0 from AddReference 0x548230, next
	// 0x726070). An exterior persistent reference is in its worldspace's
	// persistent cell and in the grid cell it stands in (0x573800).
	constexpr uint32_t CELL_objectList = 0xAC;
	constexpr uint32_t CELL_criticalQueued = 0xA0;         // u32, references still queued for load
	constexpr uint32_t CELL_queued = 0xA4;                 // u32
	constexpr uint32_t CELL_worldSpace = 0xC0;             // TESWorldSpace*, null for an interior
	constexpr uint32_t CELL_renderData = 0xC4;             // render data, whose first field is the cell's root NiNode
	// TESObjectCELL and TESWorldSpace: TESFullName at 0x18, its String {char*, u16 length, u16 capacity} at 0x1C.
	constexpr uint32_t FULLNAME_text = 0x1C;

	// ---- DataHandler / ModInfo (the plugin files) ----
	constexpr uint32_t DH_loadedModCount = 0x218;          // u32: ModList at 0x210, its count at +0x08
	constexpr uint32_t DH_loadedMods = 0x21C;              // ModInfo*[0xFF], by load index
	constexpr uint32_t MODINFO_name = 0x20;                // char[0x104], the file name

	// ---- InterfaceManager ----
	constexpr uint32_t IM_initialised = 0x000;             // u8
	constexpr uint32_t IM_menuMode = 0x00C;                // u32, 1 = game mode
	constexpr uint32_t IM_menuStack = 0x114;               // u32[10] open menu IDs, bottom first, zero-terminated (0x714F00)
	constexpr uint32_t IM_menuStackLen = 10;
	constexpr uint32_t IM_pipBoyState = 0x4BC;             // u32, 0-5 state machine (0x70EE80)

	// ---- TESObjectREFR ----
	constexpr uint8_t kFormType_REFR_first = 0x3A;         // REFR; ACHR 0x3B, ACRE 0x3C, the projectile refs after
	constexpr uint8_t kFormType_REFR = 0x3A;
	constexpr uint8_t kFormType_REFR_last = 0x45;
	constexpr uint8_t kFormType_ACHR = 0x3B;
	constexpr uint8_t kFormType_ACRE = 0x3C;
	constexpr uint32_t kFormFlag_Deleted = 0x20;
	constexpr uint32_t kFormFlag_Disabled = 0x800;
	constexpr uint32_t REFR_baseForm = 0x20;
	constexpr uint32_t REFR_pos = 0x30;                    // NiPoint3
	constexpr uint32_t REFR_parentCell = 0x40;
	constexpr uint32_t REFR_renderState = 0x64;            // its +0x14 is the 3D root
	constexpr uint32_t RS_niNode = 0x14;

	// ---- PlayerCharacter ----
	constexpr uint32_t PC_thirdPerson = 0x64A;             // u8
	// The player's active effect list (MagicTarget at +0x94, its list at
	// +0x17C): the first tList node {ActiveEffect* data, node* next}.
	constexpr uint32_t PC_effectList = 0x210;
	constexpr uint32_t LE_light = 0x48;                    // LightEffect : ActiveEffect: NiPointer<NiPointLight>

	// ---- ShadowSceneNode light lists (NiTList {head, tail, count}) ----
	constexpr uint32_t SSN_pointLights = 0xB4;             // active point lights
	constexpr uint32_t SSN_shadowLights = 0xC0;            // active shadow-casting lights
	constexpr uint32_t LIST_next = 0x00;                   // NiTListItem {next, prev, data}
	constexpr uint32_t LIST_data = 0x08;
	constexpr uint32_t SSL_light = 0xF8;                   // ShadowSceneLight -> NiLight*

	// ---- NiLight ----
	constexpr uint32_t LIGHT_dimmer = 0xC4;
	constexpr float kLightOffDimmer = 0.05f;              // below it the engine treats a light as off (0xB5D300)
	constexpr uint32_t LIGHT_ambient = 0xC8;               // NiColor
	constexpr uint32_t LIGHT_diffuse = 0xD4;               // NiColor
	constexpr uint32_t LIGHT_radius = 0xE0;                // specular.r: the radius
	constexpr uint32_t LIGHT_atten = 0xF0;                 // constant, linear, quadratic

	// ---- NiNode ----
	constexpr uint32_t NODE_childData = 0xA0;              // NiAVObject*[]
	constexpr uint32_t NODE_childCount = 0xA6;             // u16

	// ---- NiGeometry and its properties (the decal flag) ----
	constexpr uint32_t GEO_alphaProp = 0x9C;               // property slot 0: NiAlphaProperty
	constexpr uint32_t GEO_shadeProp = 0xA8;               // property slot 3: BSShaderProperty
	constexpr uint32_t GEO_propFirst = 0x9C;               // the property slots, alpha to texturing ...
	constexpr uint32_t GEO_propLast = 0xB4;                // ... and the one before the data (each checked by NiRTTI)
	constexpr uint32_t GEO_data = 0xB8;                    // NiGeometryData
	constexpr uint32_t NGD_numVerts = 0x08;                // u16
	constexpr uint32_t NGD_pos = 0x20;                     // NiPoint3[] while the CPU copy is kept
	constexpr uint32_t ALPHA_flags = 0x18;                 // u16: bit 0 blend, 1-4 src, 5-8 dst, bit 9 test
	constexpr uint32_t kAlphaFunc_One = 0, kAlphaFunc_SrcAlpha = 6, kAlphaFunc_InvSrcAlpha = 7;
	constexpr uint32_t SHADE_flags1 = 0x20;
	constexpr uint32_t SHADE_flags2 = 0x24;
	constexpr uint32_t SF1_decal = 1u << 26;               // Decal_Single_Pass
	constexpr uint32_t SF1_dynamicDecal = 1u << 27;        // Dynamic_Decal_Single_Pass
	constexpr uint32_t SF2_alphaDecal = 1u << 21;          // Alpha_Decal
	constexpr uint32_t SF2_zBufferWrite = 1u << 0;         // ZBuffer_Write
	// The shader classes the decal flag is written to go by exact vtable
	// (fnv_exe vtbl_ppl_shade, vtbl_lighting30_shade(2), vtbl_no_lighting_shade).
	constexpr uint32_t PPL_layerCount = 0xA8;              // u16
	constexpr uint32_t PPL_texArrays = 0xAC;               // NiTexture** per role; role 0 diffuse
	constexpr uint32_t SRCTEX_fileName = 0x34;             // NiSourceTexture: const char*
	constexpr uint8_t kFormType_STAT = 0x20;
	constexpr uint8_t kFormType_MSTT = 0x22;
	constexpr uint32_t STAT_modelPath = 0x34;              // TESObjectSTAT: TESModel at +0x30, its path char* at +0x04
	constexpr uint32_t MODEL_path = 0x04;                  // TESModel: the path char*

	// ---- NiObjectNET / NiTimeController (Gamebryo 2.x) ----
	constexpr uint32_t NET_controller = 0x0C;              // NiTimeController*: the first of the object's controllers
	constexpr uint32_t CTRL_target = 0x2C;                 // NiObjectNET*: the object it drives (checked against the owner)
	constexpr uint32_t CTRL_next = 0x30;                   // NiTimeController*: the next controller of the same object

	// ---- SceneGraph / NiCamera / NiAVObject ----
	constexpr uint32_t SG_camera = 0xAC;                   // NiCamera*
	constexpr uint32_t SG_fov = 0xBC;                      // float, degrees
	constexpr uint32_t AV_parent = 0x18;
	constexpr uint32_t AV_flags = 0x30;                    // bit 0 APP_CULLED
	constexpr uint32_t AV_localRot = 0x34;                 // NiMatrix3, row-major
	constexpr uint32_t AV_localPos = 0x58;
	constexpr uint32_t AV_localScale = 0x64;
	constexpr uint32_t AV_worldRot = 0x68;                 // NiMatrix3, row-major
	constexpr uint32_t AV_worldPos = 0x8C;
	constexpr uint32_t AV_worldScale = 0x98;
	constexpr uint32_t kAvFlag_CopyParentWorld = 0x200;    // the object's world transform is its parent's
}
