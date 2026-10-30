#pragma once
#include <cstdint>

// The interface between the wrapper (d3d9.dll) and the engine monitor
// plugin (Data\NVSE\Plugins\fnv_remix_nvse.dll). Shared by both builds:
// C layout, 32-bit, no STL, no xNVSE types.
//
// Contract:
//   * every call is made on the game's main thread (FNV renders there).
//     A call from any other thread (the loading screen's) gets nothing:
//     facts() returns ready = 0, the others 0 / false;
//   * facts(), cells() and drain_events() first apply the plugin's queue
//     of engine events, so the wrapper sees them in engine order;
//   * facts() and lights() read the camera, the menu state and the lights
//     live, at the call;
//   * owner_of(geo) checks the geometry's parent chain before answering;
//     the wrapper calls it inside its begin_resolve(geo) window (or while
//     the engine renders geo), so geo can't die during the check;
//   * lights() copies values: the wrapper never reads an engine light;
//   * take_members() hands over the geometry the owner map gained (every
//     owned geometry at the first call), so the wrapper knows the loaded
//     scene graph without walking it; a geometry may come more than once;
//   * log_line() is the one call any thread may make;
//   * FnvRemix_GetApi(abi) returns null for any ABI but FNV_REMIX_ABI.

// Bumped on any change to this file. Plugin and wrapper are built from one
// commit; a different value means a mixed install.
#define FNV_REMIX_ABI 8u

enum FnvRemixFeature : uint32_t
{
	FNVR_F_CELLS   = 1u << 0,   // cell events intact (xNVSE 0x4512A0, 0x5518A5)
	FNVR_F_LOADING = 1u << 1,   // load / present / main-loop messages intact (0x7147C4, 0x87055E, 0x86B386)
	FNVR_F_CAMERA  = 1u << 2,   // world camera readable
	FNVR_F_MENU    = 1u << 3,   // InterfaceManager readable
	FNVR_F_OWNERS  = 1u << 4,   // ref 3D events intact (0x571090, 0x5710AC, 0x549787)
	FNVR_F_LIGHTS  = 1u << 5,   // light registry
	FNVR_F_DECALS  = 1u << 6,   // decal path vanilla (0xBDF6C0, 0xB9AF60, 0xB9B1A2, 0xB97E30, 0xB98320)
	FNVR_F_MENU_BG = 1u << 7,   // menu-background capture vanilla (0x86F4E3, 0x86FF99, 0x86FFA8, 0x87000F, 0x870187, 0x8722E9)
	FNVR_F_WATER   = 1u << 8,   // the water system's camera-underwater state readable
	// The wrapper's own hook sites, as the integrity monitor sees them:
	// vanilla (not installed yet), the wrapper's hook, or another module's
	// hook the wrapper chains with (FnvRemix_HookChain).
	FNVR_F_HOOK_P1 = 1u << 16,  // render-pass dispatcher 0xB994F0
	FNVR_F_HOOK_P2 = 1u << 17,  // ~NiGeometry 0xA80580
	FNVR_F_HOOK_P3 = 1u << 18,  // grass batch 0xBD8550 / 0xBD85B0 / 0xBD8F90
	FNVR_F_HOOK_P5 = 1u << 19,  // skinning 0xB992F2 / 0xB99598 / 0xB991E7
};

enum FnvRemixGameState : uint8_t { FNVR_GS_MAIN_MENU, FNVR_GS_LOADING, FNVR_GS_IN_GAME, FNVR_GS_MENU };
enum FnvRemixPipBoy : uint8_t { FNVR_PB_CLOSED, FNVR_PB_OPENING, FNVR_PB_OPEN, FNVR_PB_CLOSING };

struct FnvRemixFacts
{
	uint32_t frame;                 // the plugin's MainGameLoop count
	uint32_t features_ok;           // FnvRemixFeature bits that passed and still hold
	uint32_t cut_serial;            // +1 per cut: load, new game, main menu, interior / worldspace change, teleport
	uint32_t cells_serial;          // +1 on any attach / detach
	uint32_t frames_since_cut;
	uint8_t  game_state;            // FnvRemixGameState
	uint8_t  pipboy;                // FnvRemixPipBoy
	uint8_t  loading;               // a load is running (load messages, loading-screen presents, LoadingMenu)
	uint8_t  settled;               // since the last cut: no load running, every attached cell reported its refs loaded (or timed out)
	uint8_t  third_person;
	uint8_t  interior;              // the attached set is one interior cell
	uint8_t  pipboy_raw;            // InterfaceManager+0x4BC as read (0-5)
	uint8_t  ready;                 // 1: computed on the main thread after the startup report; 0: ignore these facts
	int16_t  grid_cx, grid_cy;      // exterior grid centre (cell coordinates)
	uint16_t grid_dim;              // 0: no grid
	uint16_t top_menu;              // the menu on top of InterfaceManager's open-menu stack (1053 BarterMenu, ...); 0: none
	float    world_cam_pos[3];
	float    world_cam_rot[9];      // row-major, the world camera's world rotate
	float    world_fov_deg;
	float    first_person_fov_deg;  // 0: not read
	uint32_t pins;                  // engine references the plugin holds (0 at the main menu)
	uint32_t owners;                // references in the owner map
	uint32_t owner_geos;            // geometries in the owner map
	float    player_pos[3];         // the player reference's position (feet); all 0 when unreadable
	uint32_t light_fixups;          // registry lights whose own world position lagged their parent's (since start)
	uint8_t  underwater;            // the engine's own "camera below the water surface" test (drives its underwater fog)
	uint8_t  water_valid;           // water_height holds the height of the water at the camera
	uint16_t read_faults;           // guarded reads that faulted since start (saturates)
	float    water_height;          // world Z of that water surface
};

struct FnvRemixCell
{
	uintptr_t cell, root;           // root: the cell's 3D root, read at the call (0: none yet)
	int32_t x, y;                   // exterior cell coordinates
	uint8_t interior, refs_loaded, pad[2];
};

enum FnvRemixEventType : uint16_t
{
	FNVR_EV_CELL_ATTACHED = 1,      // a = cell
	FNVR_EV_CELL_DETACHED,          // a = cell
	FNVR_EV_CELL_REFS_LOADED,       // a = cell, b = how (FnvRemixRefsLoadedBy)
	FNVR_EV_LOAD_BEGIN,
	FNVR_EV_LOAD_END,
	FNVR_EV_CUT,                    // a = FnvRemixCutReason, b = teleport distance in units (0 otherwise)
	FNVR_EV_OWNER_GONE,             // a = reference, b = its old 3D root (pinned until the next main loop)
	FNVR_EV_OVERFLOW,               // queue overflowed: events dropped, resync from facts / cells
};

enum FnvRemixCutReason : uint32_t
{
	FNVR_CUT_LOAD = 1,              // a load (save, door, fast travel, coc) ended
	FNVR_CUT_NEW_GAME,
	FNVR_CUT_MAIN_MENU,
	FNVR_CUT_WORLD_CHANGE,          // interior / worldspace changed without a loading screen
	FNVR_CUT_TELEPORT,              // the world camera jumped with no load
};

enum FnvRemixRefsLoadedBy : uint32_t { FNVR_RL_EVENT = 1, FNVR_RL_POLL, FNVR_RL_TIMEOUT };

struct FnvRemixEvent { uint16_t type, pad; uint32_t serial; uintptr_t a, b; };

enum FnvRemixOwnerFlags : uint32_t
{
	FNVR_OW_ACTOR = 1,              // ACHR / ACRE
	FNVR_OW_FIRST_PERSON = 2,       // reserved (the first-person root has no Set3D)
	FNVR_OW_DISABLED = 4,           // the reference is disabled or deleted
	FNVR_OW_DETACHED = 8,           // the chain no longer reaches the reference's root
	FNVR_OW_APP_CULLED = 16,        // an app-culled node in the reference's own 3D (the geometry up to the root)
	FNVR_OW_APP_CULLED_ABOVE = 32,  // an app-culled node above the reference's root (the cell's nodes)
};

struct FnvRemixOwner
{
	uintptr_t ref, root;            // the reference and its pinned 3D root
	uintptr_t cell;                 // the attached cell its 3D hangs in (0: none attached)
	uint32_t ref_id;
	uint8_t base_type, pad[3];      // the base form's type (0x20 STAT, ...)
	uint32_t flags;                 // FnvRemixOwnerFlags
};

// One geometry of the loaded scene graph and the reference whose 3D holds
// it (non-actor references only).
struct FnvRemixMember { uintptr_t geo, ref; };

enum FnvRemixLightKind : uint8_t { FNVR_LK_PLACED, FNVR_LK_EFFECT, FNVR_LK_PLAYER };

struct FnvRemixLight
{
	uintptr_t id;                   // the NiLight, pinned while listed: never reused for another light
	uintptr_t owner_ref;            // 0 if none
	float pos[3], diffuse[3], ambient[3];
	float dimmer, radius, atten[3];
	uint8_t kind, lit, listed_by_engine, pad;
	// The light's name for Remix mods and captures, the same in every session
	// (placed: its reference and base form, the reference's position and the
	// light's place in its 3D; the player's light: a constant). 0: none (effect lights).
	uint64_t asset_key;
};

// Where the player is, for the session report.
struct FnvRemixPlace
{
	char cell[80];                  // the player's cell: "'Name' (0x0011ABCD)", or its form ID alone
	char worldspace[80];            // its worldspace, the same way; empty for an interior
	int32_t cell_x, cell_y;         // exterior cell coordinates
	float pos[3];                   // the player's position (feet)
	uint8_t interior, pad[3];
};

struct FnvRemixApi
{
	uint32_t abi;                   // FNV_REMIX_ABI
	uint32_t xnvse_version;         // xNVSE's packed version (major << 24 | minor << 16 | beta << 4)
	const char* plugin_commit;      // git short hash of the plugin build
	uint32_t features_built;        // FnvRemixFeature bits this plugin implements
	const char* (*feature_status)(uint32_t feature_bit);   // the plugin's verdict line for that feature
	void     (*facts)(FnvRemixFacts* out);
	uint32_t (*cells)(FnvRemixCell* out, uint32_t max);
	uint32_t (*drain_events)(FnvRemixEvent* out, uint32_t max);
	bool     (*owner_of)(uintptr_t geo, FnvRemixOwner* out);
	uint32_t (*lights)(FnvRemixLight* out, uint32_t max);
	// Geometries the owner map gained since the last call (every owned one
	// at the first call, and again after the plugin's feed overflowed).
	// Each was alive at its walk; the wrapper reads one only inside its
	// begin_resolve window. max = 0: every owned geometry is queued again
	// for the next call (the wrapper dropped what it knew).
	uint32_t (*take_members)(FnvRemixMember* out, uint32_t max);
	// The player's cell and position. False: no player or no cell yet.
	bool     (*place)(FnvRemixPlace* out);
	// The engine's loaded plugins in load order, one "IX Name.esp" line each
	// (IX: the load index in hex), written to `out` as far as `size` allows
	// and NUL-terminated. Returns the length of the whole list without the
	// NUL; 0 before the game's data is loaded.
	uint32_t (*load_order)(char* out, uint32_t size);
	// A line in fnv_remix_nvse.log, never held back. Any thread.
	void     (*log_line)(const char* tag, const char* text);
};

typedef const FnvRemixApi* (*PFN_FnvRemix_GetApi)(uint32_t abi);

// The wrapper's hooks, for the integrity report. d3d9.dll exports
// FnvRemix_HookChain: it copies up to `max` records into `out` and returns
// how many hooks it has installed. The plugin calls it on the main thread.
struct FnvRemixHook
{
	uintptr_t site;                 // the engine address the wrapper wrote
	uintptr_t ours;                 // where the site leads while the wrapper's hook is in place
	uintptr_t previous;             // where the site led before (another module's hook, or the engine's own code)
	uint8_t chained;                // `previous` is another module's hook: the wrapper's runs first, then that one
	uint8_t pad[3];
	char previous_owner[48];        // the module that holds `previous`
};

typedef uint32_t (*PFN_FnvRemix_HookChain)(FnvRemixHook* out, uint32_t max);

// d3d9.dll also exports FnvRemix_SessionExit: the plugin calls it on the main
// thread when xNVSE reports that the game is quitting (`how`: "menu" or
// "console"), so the session report can tell a quit from a crash.
typedef void (*PFN_FnvRemix_SessionExit)(const char* how);
