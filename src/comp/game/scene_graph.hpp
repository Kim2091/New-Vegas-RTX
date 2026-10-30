#pragma once
#include "shared/fnv_exe.hpp"

// Read-only access to FNV's scene graph: the engine layout, SEH-guarded
// reads of cells, references, nodes, geometry and their properties,
// RTTI-based class identity, and the two engine hooks the scene-graph
// features share.
//
// Every read of engine memory goes through a small function whose __try
// frame holds no C++ object; a fault counts in faults() and the read reports
// failure. Nothing here writes engine memory. Everything but the destructor
// and grass hooks runs on the game's render thread.
//
// Class identity: MSVC RTTI where the vtable has a complete object locator,
// else Gamebryo's own NiRTTI. FalloutNV.exe has no MSVC RTTI for many
// Bethesda classes: BSFadeNode's vtable 0x010A8F90 and
// BSShaderPPLightingProperty's 0x010AE0D0 have 0 / a code pointer at
// vtable-4 (no locator), and the PPL type descriptor doesn't exist at all.
// Every NiObject's vtable slot 2 is GetRTTI, `mov eax, imm32; ret` (or the
// same inside a push ebp frame), and the NiRTTI {name, parent} chain it
// returns names the class and every base (static initialisers, e.g. 0xFB94D0
// builds BSFadeNode's 0x011F9140 with parent NiNode 0x011F4428).
//
// Hooks (installed on demand by install_hooks, never at startup unless a
// feature asks, each through call_chain::hook_entry, in front of another
// mod's detour where one is found):
//   * the per-pass dispatcher 0xB994F0: records the geometry whose pass is
//     being rendered (current_geometry) and asks the pass filter whether to
//     skip the pass; render thread only (a call on any other thread passes
//     straight through, untouched). The filter sees every pass before it
//     decides (the sighting), and may read the geometry: the engine is
//     rendering it, so it is alive for the whole call;
//   * ~NiGeometry 0xA80580 (any thread): adds the dying geometry to the
//     dead set under a lock, before the object is torn down. The render
//     thread drains it (take_deaths) and retires the claim in the same step;
//     until then death_pending() answers true, so a claimed address that
//     died (or was recycled for a new object) is never skipped.
//
// Resolve / destroy handshake: between frames the render thread reads
// geometry it saw during the frame (mesh builds, hide polls). ~NiGeometry
// can run on a loader thread at that moment. The reader publishes the
// geometry (begin_resolve), then checks the dead set; the destructor marks
// it dead, then, if it is the one being read, waits (capped) until
// end_resolve. Both sides use seq_cst (a Dekker pair):
// either the reader sees the death and skips, or the destructor sees the
// reader and waits. A read never touches a freed geometry.

namespace comp::game::scene_graph
{
	// Engine offsets. U = unverified (the scene statics' seed parity line
	// reports the material fields read from them against the draws).
	namespace fnv
	{
		constexpr uint32_t TES_gridCells = 0x08;            // GridCellArray*
		constexpr uint32_t GCA_dim = 0x0C;                  // U
		constexpr uint32_t FORM_type = 0x04;                // u8
		constexpr uint32_t FORM_flags = 0x08;               // U: 0x20 deleted, 0x800 disabled
		constexpr uint32_t FORM_refID = 0x0C;               // U
		constexpr uint8_t kFormType_STAT = 0x20;
		constexpr uint32_t kFormFlag_Deleted = 0x20;
		constexpr uint32_t kFormFlag_Disabled = 0x800;
		constexpr uint32_t CELL_objectList = 0xAC;          // tList<TESObjectREFR> {data, next}
		// Not the cell root: TESObjectCELL::Create3D 0x545CF0 stores the
		// light-marker node here (a child of the Marker category node,
		// getter 0x5495D0); only LIGH refs hang under it.
		constexpr uint32_t CELL_lightMarkers = 0xB4;
		// The cell root: *(cell+0xC4) is the cell's render data, whose
		// first field is the root NiNode (Get3D 0x545CB0; stored at
		// 0x5465F4). Its children are the 9 category nodes (Actor, Marker,
		// Land, Static, Dynamic, OcclusionPlane, Portal, MultiBound,
		// Collision); the root hangs under ObjectLODRoot (TES+0x0C).
		constexpr uint32_t CELL_renderData = 0xC4;
		constexpr uint32_t REFR_baseForm = 0x20;            // U
		constexpr uint32_t REFR_pos = 0x30;                 // U
		constexpr uint32_t REFR_parentCell = 0x40;          // U
		constexpr uint32_t REFR_renderState = 0x64;         // U
		constexpr uint32_t RS_niNode = 0x14;                // U
		constexpr uint32_t OBJ_controller = 0x0C;           // U: NiObjectNET controller
		constexpr uint32_t AV_parent = 0x18;
		constexpr uint32_t AV_flags = 0x30;                 // bit 0 app-culled
		constexpr uint32_t AV_worldRot = 0x68;
		constexpr uint32_t AV_worldPos = 0x8C;
		constexpr uint32_t AV_worldScale = 0x98;
		constexpr uint32_t NODE_childData = 0xA0;
		constexpr uint32_t NODE_childCap = 0xA4;            // u16
		constexpr uint32_t NODE_childCount = 0xA6;          // u16
		constexpr uint32_t GEO_props = 0x9C;                // 7 pointers
		constexpr uint32_t GEO_data = 0xB8;
		constexpr uint32_t GEO_skin = 0xBC;
		constexpr uint32_t GEO_shader = 0xC0;
		// Property slots of GEO_props, by index.
		constexpr int kPropAlpha = 0, kPropCull = 1, kPropMaterial = 2, kPropShade = 3, kPropStencil = 4,
			kPropTexturing = 5, kPropCount = 7;
		constexpr uint32_t NGD_numVerts = 0x08;
		constexpr uint32_t NGD_id = 0x0A;
		constexpr uint32_t NGD_pos = 0x20;
		constexpr uint32_t NGD_normal = 0x24;
		constexpr uint32_t NGD_color = 0x28;
		constexpr uint32_t NGD_uv = 0x2C;
		constexpr uint32_t NGD_buffer = 0x34;               // NiGeometryBufferData* once packed
		constexpr uint32_t NGD_keepFlags = 0x38;            // Gamebryo KEEP_XYZ 1, NORM 2, COLOR 4, UV 8, INDICES 0x10, BONEDATA 0x20
		constexpr uint32_t NGD_compressFlags = 0x39;
		// NiGeometryBufferData (vtable 0x010F017C, 0x54 bytes, ctor 0xE947C0;
		// allocated by NiGeometryGroup::AddObject 0xE948F0 into NGD+0x34).
		// Field use: particle draw 0xE70140 (SetVertexDeclaration(+0x10),
		// SetStreamSource(0, chip->VB, 0, stride[0]), SetIndices(+0x34),
		// DrawIndexedPrimitive(+0x3C, +0x38, 0, +0x18, 0, +0x40)); chips
		// 0xE94E30 (unshared: VB at +8, offset 0) and 0xE94C20 (static group:
		// shared VB, +0x38 = chip offset / stride).
		constexpr uint32_t BD_flags = 0x04;
		constexpr uint32_t BD_fvf = 0x0C;                   // 0 when a declaration is used
		constexpr uint32_t BD_decl = 0x10;                  // IDirect3DVertexDeclaration9*
		constexpr uint32_t BD_vertCount = 0x18;
		constexpr uint32_t BD_streamCount = 0x20;
		constexpr uint32_t BD_strides = 0x24;               // u32[streamCount]
		constexpr uint32_t BD_chips = 0x28;                 // NiVBChip*[streamCount]
		constexpr uint32_t BD_indexCount = 0x2C;
		constexpr uint32_t BD_ib = 0x34;                    // IDirect3DIndexBuffer9*
		constexpr uint32_t BD_baseVertex = 0x38;            // DrawIndexedPrimitive BaseVertexIndex
		constexpr uint32_t BD_primType = 0x3C;              // D3DPRIMITIVETYPE (4 list, 5 strip)
		constexpr uint32_t BD_triCount = 0x40;
		constexpr uint32_t CHIP_vb = 0x08;                  // IDirect3DVertexBuffer9*
		constexpr uint32_t CHIP_offset = 0x0C;              // bytes into the (shared) VB
		constexpr uint32_t kMaxStreams = 4;
		constexpr uint32_t TBGD_numTris = 0x40;
		constexpr uint32_t TSD_indexCount = 0x44;
		constexpr uint32_t TSD_indices = 0x48;
		constexpr uint32_t TSTD_numStrips = 0x44;
		constexpr uint32_t TSTD_stripLens = 0x48;
		constexpr uint32_t TSTD_stripIdx = 0x4C;
		constexpr uint32_t SHADE_type = 0x1C;
		// BSShaderProperty's two flag words, loaded for every subclass by
		// BSShaderProperty::LoadBinary 0xBA9380 (the NIF's BSShaderFlags /
		// BSShaderFlags2; nif.xml Fallout3ShaderPropertyFlags1 / 2).
		constexpr uint32_t SHADE_flags1 = 0x20;
		constexpr uint32_t SHADE_flags2 = 0x24;
		constexpr uint32_t SF1_vertexAlpha = 1u << 3;       // Vertex_Alpha: the shader uses the vertex colour's alpha
		constexpr uint32_t SF1_decal = 1u << 26;            // Decal_Single_Pass
		constexpr uint32_t SF1_dynamicDecal = 1u << 27;     // Dynamic_Decal_Single_Pass
		constexpr uint32_t SF2_zWrite = 1u << 0;            // ZBuffer_Write (log only)
		constexpr uint32_t SF2_alphaDecal = 1u << 21;       // Alpha_Decal
		constexpr uint32_t ALPHA_flags = 0x18;              // u16; bit 0 blend, bit 9 test, bits 10-12 func (U)
		constexpr uint32_t ALPHA_threshold = 0x1A;          // U: u8
		constexpr uint32_t STENCIL_flags = 0x18;            // u16, draw mode bits 10-11 (cull per mode: ApplyStencil 0xE87EF0)
		constexpr uint32_t MAT_gloss = 0x38;                // NiMaterialProperty glossiness (the lighting PS's Toggles.z, 0xC02D90)
		constexpr uint32_t MAT_alpha = 0x3C;                // U
		// Texture Clamp Mode (Gamebryo TexClampMode, u32, default 3), read
		// by exact vtable: only these three classes have the field. The
		// BSShaderLightingProperty base has none (its +0x6C is a float 1.0),
		// so a base-class test would read garbage. PPL getter 0xB671C0
		// (vtable +0xE0, LoadBinary 0xB68ED0 reads it after the env map
		// scale); NoLighting getter 0xE902D0 (vtable +0xCC). The engine maps
		// it through the NiDX9 table 0x126F0B0 and applies it, via
		// NiD3DTextureStage::SetClampMode 0xE7DE90, to the diffuse / normal
		// stages a pass binds (not glow; some passes hard-code 0 or 3).
		// The three vtables are fnv_exe vtbl_ppl_shade, vtbl_lighting30_shade
		// (PPL layout, ctor 0xB66F50) and vtbl_no_lighting_shade.
		constexpr uint32_t PPL_clampMode = 0xD8;            // PPL and Lighting30
		constexpr uint32_t NOLIGHT_clampMode = 0x6C;        // NoLighting
		constexpr uint32_t PPL_layerCount = 0xA8;           // u16
		constexpr uint32_t PPL_texArrays = 0xAC;            // 6 x NiTexture** (texture-set loader 0xB66AD0)
		constexpr int kRoleCount = 6;                       // diffuse, normal, glow, height, env, env mask
		constexpr uint32_t TEX_rendererData = 0x24;
		constexpr uint32_t SRCTEX_fileName = 0x34;          // const char*
		constexpr uint32_t DX9TEX_d3d = 0x64;

		// Grass batches (the grass manager's 0x48-byte batch object: +0x00
		// its NiGeometry, +0x04 the BSMultiBoundNode under the grass root,
		// +0x12 u16 instance count, +0x14 float4 world positions, +0x18 the
		// per-instance owner keys, +0x3C / +0x40 cell X / Y). Every batch of
		// one grass type shares one NiGeometryData (ctor 0xBD8AE0 passes
		// template+0x08): same VB, instance data per batch. Their geometry
		// is a TallGrassTriStrips (ctor 0xC1A480) or TallGrassTriShape (ctor
		// 0xC1A660), by fnv_exe vtbl_grass_tri_strips / vtbl_grass_tri_shape.
		// fnv_exe grass_batch_append, thiscall(batch, const float4* pos, u32
		// key), ret 8: appends one instance (only caller 0xB62FC1, the grass
		// generator 0xB62DE0).
		// fnv_exe grass_batch_remove_at, thiscall(batch, u16 index), ret 4:
		// removes one instance by swap-and-shrink (only caller 0xBD8670,
		// remove-by-key, itself only called from 0xB61370).
		// fnv_exe grass_batch_destroy, thiscall(batch): detaches the geometry
		// and the multibound node and releases them (callers 0xB612CE /
		// 0xB61507 / 0xB61754 / 0xB62D2B: clear-all 0xB61250, remove-by-key,
		// per-cell 0xB61570, regenerate).
	}

	// ---- Class identity (RTTI, cached per vtable) ----------------------------------

	enum class Leaf : uint8_t { None, TriShape, TriStrips, OtherGeometry };

	enum class ClassSource : uint8_t { None, Msvc, NiRtti };

	struct ClassInfo
	{
		std::string name;             // "NiTriShape" (decoration stripped); "?" when unreadable
		ClassSource source = ClassSource::None;
		uintptr_t vtbl = 0;
		bool node = false;            // derives from NiNode
		bool geometry = false;        // derives from NiGeometry
		Leaf leaf = Leaf::None;       // exact class, for geometry
		// Subtrees not walked: BSTreeNode, NiBillboardNode, NiSwitchNode
		// (NiLODNode is one: only one child is shown) and BSRangeNode
		// (BSBlastNode / BSDebrisNode / BSDamageStage: children by range).
		bool excluded_subtree = false;
		// The same set split by what it means for a leaf under it: a tree /
		// billboard node (SpeedTree or camera-facing: never a rigid static),
		// or a node that shows one child of several (switch / LOD / range:
		// the leaf is in the world only while its branch is the active one).
		bool subtree_tree = false;
		bool subtree_switch = false;
		bool subtree_exclusive = false;   // LOD / switch node: one child branch drawn at a time (not range nodes)
		// BSShaderPPLightingProperty, or its Lighting30ShaderProperty subclass
		// (same texture arrays); not the SpeedTree / hair subclasses.
		bool shade_ppl = false;
		bool shade_water = false;     // WaterShaderProperty (and subclasses)
		bool shade_property = false;  // derives from BSShaderProperty (SHADE_flags1 / 2 are its fields)
		bool segmented = false;       // BSSegmentedTriShape (land / LOD chunks)
		bool source_texture = false;  // derives from NiSourceTexture
		bool cell = false;            // TESObjectCELL
		bool worldspace = false;      // TESWorldSpace
	};

	// Class of the object at `object` (its vtable's RTTI). Null when the
	// pointer or its vtable can't be read; name "?" (source None) when
	// neither RTTI resolves it.
	const ClassInfo* class_of(uintptr_t object);
	const ClassInfo* class_of_vtbl(uintptr_t vtbl);

	// ---- Reads ----------------------------------------------------------------

	struct AvRead
	{
		uintptr_t vtbl = 0;
		uintptr_t controller = 0;
		uintptr_t parent = 0;
		uint32_t flags = 0;
		float rot[9] = {};            // NiMatrix3, row-major
		float pos[3] = {};
		float scale = 1.0f;
		bool app_culled() const { return (flags & 1u) != 0; }
	};
	bool read_av(uintptr_t obj, AvRead& out);

	// The world transform alone: rotation (9), translation (3), scale (1),
	// NiAVObject +0x68..+0x9C in memory order.
	constexpr size_t kPoseFloats = 13;
	bool read_pose(uintptr_t obj, float (&out)[kPoseFloats]);

	// Parent (+0x18) and flags (+0x30) of an NiAVObject, one guarded read.
	bool read_link(uintptr_t obj, uintptr_t& parent, uint32_t& flags);

	struct NodeChildren
	{
		uint16_t cap = 0;
		uint16_t count = 0;           // +0xA6
		uint32_t read = 0;            // entries copied into the caller's array (nulls included)
	};
	bool read_children(uintptr_t node, uintptr_t* out, uint32_t max, NodeChildren& info);

	struct GeoRead
	{
		uintptr_t props[fnv::kPropCount] = {};
		uintptr_t data = 0;
		uintptr_t skin = 0;
		uintptr_t shader = 0;
		uintptr_t prop(int i) const { return props[i]; }
	};
	bool read_geo(uintptr_t geo, GeoRead& out);

	struct DataRead
	{
		uintptr_t vtbl = 0;
		uint16_t verts = 0;
		uint16_t id = 0;
		uintptr_t pos = 0, nrm = 0, col = 0, uv = 0;
		uintptr_t packed = 0;
		uint8_t keep = 0, compress = 0;
		uint16_t tris = 0;
		uint32_t f44 = 0;             // TriShapeData: index count; TriStripsData: strip count (u16)
		uintptr_t f48 = 0, f4c = 0;   // TriShapeData: indices; TriStripsData: lengths, indices
	};
	bool read_data(uintptr_t ngd, DataRead& out);

	// The packed D3D9 form of a geometry data (NGD+0x34 -> NiGeometryBufferData).
	// Only geometry the engine has packed has one; FNV frees the CPU UV /
	// colour (and usually normal) arrays after packing (KEEP flags), so this
	// is the only complete vertex source.
	struct BufferRead
	{
		uintptr_t bd = 0;
		uint32_t flags = 0;
		uint32_t fvf = 0;
		IDirect3DVertexDeclaration9* decl = nullptr;
		uint32_t verts = 0;
		uint32_t streams = 0;
		uint32_t stride[fnv::kMaxStreams] = {};
		IDirect3DVertexBuffer9* vb[fnv::kMaxStreams] = {};
		uint32_t chip_offset[fnv::kMaxStreams] = {};
		uint32_t index_count = 0;
		IDirect3DIndexBuffer9* ib = nullptr;
		uint32_t base_vertex = 0;
		uint32_t prim_type = 0;
		uint32_t tris = 0;
	};
	bool read_buffer(uintptr_t buffer_data, BufferRead& out);

	// The vertex layout of a packed buffer: the declaration's elements
	// (the bridge client answers GetDeclaration locally), or the FVF's
	// through D3DXDeclaratorFromFVF. Terminated by D3DDECL_END; false when
	// neither gives a layout.
	bool buffer_layout(const BufferRead& b, D3DVERTEXELEMENT9 (&out)[MAXD3DDECLLENGTH + 1], UINT& count);

	struct RefRead
	{
		uintptr_t vtbl = 0;
		uint8_t type = 0;
		uint32_t flags = 0;
		uint32_t ref_id = 0;
		uintptr_t base = 0;
		uint8_t base_type = 0;
		uintptr_t cell = 0;
		uintptr_t root = 0;           // RenderState->niNode, 0 when not loaded
		bool disabled() const { return (flags & (fnv::kFormFlag_Disabled | fnv::kFormFlag_Deleted)) != 0; }
	};
	bool read_ref(uintptr_t ref, RefRead& out);

	// Textures. The PPL role array entry [0], its D3D texture (null while
	// the engine hasn't created the renderer data; has_renderer_data tells
	// which), and its file name.
	uintptr_t ppl_texture(uintptr_t prop, int role);
	IDirect3DBaseTexture9* d3d_texture(uintptr_t nitex, bool& has_renderer_data);
	bool texture_name(uintptr_t nitex, char* out, size_t cap);

	// The shader property's Texture Clamp Mode (the raw u32, unvalidated)
	// and its exact class name (a static string), for the three classes
	// with the field (fnv_exe vtbl_ppl_shade / vtbl_lighting30_shade /
	// vtbl_no_lighting_shade). False for any other class (subclasses
	// included) or an unreadable property.
	bool read_clamp_mode(uintptr_t shade, uint32_t& mode, const char*& cls);

	// The shader property's BSShaderProperty flag words (fnv::SHADE_flags1 /
	// SHADE_flags2), when its class derives from BSShaderProperty (RTTI;
	// any subclass: PPLighting, Lighting30, NoLighting, ...). cls = its class
	// name (stable for the session). False: no property, another class, or an
	// unreadable one (out.ok stays false).
	struct ShadeFlags
	{
		bool ok = false;
		const char* cls = nullptr;
		uint32_t flags1 = 0, flags2 = 0;
	};
	bool read_shade_flags(uintptr_t shade, ShadeFlags& out);

	bool read_u8(uintptr_t addr, uint8_t& out);
	bool read_u16(uintptr_t addr, uint16_t& out);
	bool read_u32(uintptr_t addr, uint32_t& out);
	bool read_f32(uintptr_t addr, float& out);
	bool read_block(uintptr_t addr, void* out, size_t n);

	// Faults caught by the guarded reads since startup.
	uint32_t faults();

	// Fault attribution. A reader names what it is reading for its scope
	// (render thread; nests); a faulting read keeps that name and the
	// address it tried. site is null when the read had no FaultSite.
	struct FaultSite
	{
		explicit FaultSite(const char* site);
		~FaultSite();
		FaultSite(const FaultSite&) = delete;
		FaultSite& operator=(const FaultSite&) = delete;
		const char* prev;
	};
	struct FaultInfo
	{
		uint32_t count = 0;           // faults() at the time of the call
		const char* site = nullptr;   // of the last fault
		uintptr_t addr = 0;           // of the last fault
	};
	FaultInfo last_fault();

	// [addr, addr + n) is committed, readable memory right now (VirtualQuery;
	// about a microsecond). For diagnostic walks that follow pointers out of
	// lists the loader may be rewriting: a value that is really a float
	// (0x43330000 = 179.0f) is skipped without taking a fault. The guarded
	// reads stay the safety net for memory freed after the check.
	bool mapped(uintptr_t addr, size_t n);

	// ---- Hooks ------------------------------------------------------------------

	// Installs the dispatcher and destructor hooks once (call_chain::hook_entry;
	// logs and returns false when a site holds code it does not recognise).
	// Render thread, between frames.
	bool install_hooks();
	bool hooks_installed();

	// The geometry of the pass the dispatcher is rendering right now, or 0.
	uintptr_t current_geometry();

	// The type of that pass (the u16 at pass+4: the engine's pass enum,
	// BSSM_*; comp/game/engine_class.hpp), or kNoPassType outside the
	// dispatcher.
	constexpr uint16_t kNoPassType = 0xFFFF;
	uint16_t current_pass_type();

	// Pass filter: true skips the pass (the original dispatcher isn't
	// called). Null = no filter. Render thread.
	using PassFilter = bool (*)(uintptr_t geometry);
	void set_pass_filter(PassFilter filter);
	uint32_t skipped_passes_take();   // passes skipped since the last call

	// Geometries destroyed since the last call (any thread; drained on the
	// render thread). A pointer returned here must not be read again. The
	// caller retires everything it keyed by these pointers before it next
	// yields to the engine (same render-thread step), so the dead set and the
	// caller's claims never disagree while a pass can run.
	void take_deaths(std::vector<uintptr_t>& out);
	// True from the moment ~NiGeometry starts on any thread until the render
	// thread takes the death. O(1) (hash set under a shared lock).
	bool death_pending(uintptr_t geometry);

	// Render thread, around every read of a geometry outside its dispatch
	// (the handshake above). False = it already died: don't read it (the
	// next take_deaths drains it). Every true must be paired with
	// end_resolve before the next begin_resolve. Without the hooks it
	// answers true and does nothing.
	bool begin_resolve(uintptr_t geometry);
	void end_resolve();

	struct HookStats
	{
		uint64_t deaths = 0;
		uint64_t deaths_off_thread = 0;   // not on the render thread
		uint64_t passes_off_thread = 0;   // dispatcher calls not on the render thread (passed through)
		// Handshake: destructors that waited for a read of their own
		// geometry, waits that hit the cap, and reads that found their
		// geometry already dead.
		uint64_t resolve_waits = 0;
		uint64_t resolve_wait_timeouts = 0;
		uint64_t resolves_skipped_dead = 0;
	};
	HookStats hook_stats();

	// The thread that runs Present (recorded by the first call).
	void note_render_thread();

	// ---- Grass batch hooks (api_foliage.cpp) ---------------------------------------------
	//
	// Installed on demand (install_grass_hooks, call_chain::hook_entry), never
	// at startup. Any thread: they only record, under a lock, before the
	// original runs, keyed by the batch's NiGeometry (batch+0):
	//   * append 0xBD8550 / remove 0xBD85B0: the batch's instance set is
	//     about to change, so its generation moves;
	//   * destroy 0xBD8F90: the batch goes, so its generation moves and it is
	//     queued as destroyed (the ~NiGeometry hook may or may not follow:
	//     it runs only when that was the last reference).
	// Generations come from one process-wide counter, so a value is never
	// seen twice, not even for a geometry address the heap hands out again.
	bool install_grass_hooks();
	bool grass_hooks_installed();

	// The batch geometry's instance-set generation: 0 until a hook saw it.
	uint64_t grass_generation(uintptr_t geometry);

	// Batch geometries whose instance set changed / that were destroyed
	// since the last call (render thread).
	void take_grass_events(std::vector<uintptr_t>& changed, std::vector<uintptr_t>& destroyed);

	// The geometry is a grass batch's (TallGrassTriStrips / TriShape vtable).
	bool is_grass_geometry(uintptr_t geometry);

	struct GrassHookStats
	{
		uint64_t appends = 0, removes = 0, destroys = 0;
	};
	GrassHookStats grass_hook_stats();
}
