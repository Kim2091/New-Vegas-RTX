#include "std_include.hpp"
#include "scene_statics.hpp"
#include "scene_graph.hpp"
#include "api_mesh.hpp"
#include "api_submit.hpp"
#include "api_material.hpp"
#include "api_identity.hpp"
#include "api_rtxconf.hpp"
#include "tex_pack.hpp"
#include "stable_origin.hpp"
#include "api_camera.hpp"
#include "menu_scene.hpp"
#include "nvse_link.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"

// See scene_statics.hpp.
//
// During the frame (render thread):
//   dispatcher  pass_filter(geo): first pass -> classify once (candidate or
//               refused); every pass -> last_seen; a claimed geometry's
//               first pass of the frame samples its pose (movers); the
//               verdict: skip the pass when the geometry is claimed, its
//               instance visible and the retained path can draw this frame.
//   renderer    on_static_draw: a candidate's base pass records the material
//               (built now, while its textures are certainly bound and
//               alive), the layout, the buffers and the draw window.
//
// At Present, tick(), before the retained end of frame:
//   1. deaths    ~NiGeometry since the last tick: its instance is destroyed,
//                its record forgotten (the address may be reused);
//      written   claimed geometry whose VB / IB was write-locked: its
//                window's bytes compared with its mesh's (verify_written):
//                kept when the same, unclaimed when they changed;
//   2. residency the attached cells (the engine monitor's) and their 3D
//                roots: an instance whose anchor cell left (or whose root
//                changed) is destroyed;
//   3. resolve   recorded candidates sighted in the last frames -> owner
//                (the engine monitor's owner map: reference, base type,
//                attached cell), scope, packed buffers, mesh (shared),
//                transform -> one CreateRetainedInstances batch -> claimed;
//   4. mover poll every claimed geometry that moved since its claim and that
//                the engine didn't render this frame: its pose, read now
//                (Havok moves it while the engine culls it);
//   5. hides     round-robin poll of claimed geometry (its owner's flags:
//                detached, app-culled, disabled, another 3D; the switch-leaf
//                not-drawn rule; the pose of one still in the world) and a
//                show for anything hidden the engine drew this frame;
//   6. movers    poses that changed (at a sighting or a poll) -> transform
//                updates;
//   7. destroys  one DestroyRetainedInstances for everything that went.
// A reference that lets go of its 3D (FNVR_EV_OWNER_GONE, at Present before
// the tick) unclaims every geometry it owned.
// Updates go out with the frame's retained updates (api_retained end_frame).

namespace comp::game::scene_statics
{
	namespace sg = scene_graph;
	using namespace comp::game::api;
	using shared::common::PsSlotMap;
	using shared::common::PsSlotRole;

	namespace
	{
		constexpr uint64_t kSceneDomain = 0x5343454E45303032ull;   // "SCENE002"
		constexpr uint32_t kCreateBatch = 2048;
		constexpr UINT kRetryFrames = 60;          // build / texture / anchor not there yet
		// SceneStreamedRelaxed: a buffer streamed into (DISCARD / NOOVERWRITE)
		// is taken once it has been left alone this long (and was not
		// written twice within the rewrite window).
		constexpr UINT kQuietFrames = 30;
		// A claimed geometry whose window was refilled with different bytes a
		// second time within this many frames is refused streamed.rewritten
		// (soft: retried after kAnchorRetryFrames). One change only unclaims
		// it (claimed again from its new contents).
		constexpr UINT kChangeWindow = 300;
		constexpr uint32_t kChangeLogs = 12;       // content changes logged per session (what changed)
		// The refusal report repeats (streamed buckets only) this often.
		constexpr UINT kReportRepeatFrames = 18000;
		constexpr UINT kActivityLogFrames = 60;    // create / destroy summary lines at most this often
		constexpr UINT kFreshFrames = 2;           // a record is resolved while its geometry was sighted this recently
		// A provisional claim whose passes reached the renderer this many
		// frames without a base pass it could record keeps its seed record
		// (its passes are skipped from then on).
		constexpr uint8_t kConfirmFrames = 30;
		constexpr uint32_t kPollPerTick = 256;     // claimed geometries whose chain is polled per tick
		constexpr int kMaxChainHops = 64;
		constexpr uint32_t kSwitchActive = 0xB0;   // NiSwitchNode: int, the active child's index
		constexpr uint8_t kMaxAnchorTries = 3;
		constexpr uint8_t kLandShapeTries = 120;   // land-shaped draws before an unplaced one counts as land
		// A resolve refusal for want of a place (noAnchor / noRef / culled)
		// is not final: the geometry becomes a candidate again this long
		// after it (a reference whose 3D event arrived late).
		constexpr UINT kAnchorRetryFrames = 600;
		constexpr float kMoveDeadband = 0.01f;     // world units
		// The translation band also covers this many float ulps of the
		// coordinate: at FNV's |x| of 65536-131072 one ulp is 0.0078, so a
		// pose the engine recomputed with 2 ulps of rounding would clear the
		// 0.01 band and go out as a move.
		constexpr float kMoveUlps = 4.0f;
		// A counted move whose translation stays under this and whose linear
		// part stays under kSmallLinear: logged as "small" (sub-visual).
		constexpr float kSmallMove = 0.5f;
		constexpr float kSmallLinear = 1.0e-3f;
		constexpr size_t kTopMovers = 3;
		constexpr float kLinearTolerance = 1.0e-4f;
		constexpr UINT kNoFrame = 0xFFFFFFFFu;

		enum class Reason : uint8_t
		{
			// At discovery (the geometry's first pass). Blend: plain alpha
			// blending without a test; BlendTest: blending with an alpha test
			// (both only while SceneClaimAlphaBlend leaves them out); BlendFx:
			// any other blend factors (additive, modulate: effects).
			Class, Terrain, Skinned, Water, Shader, Blend, BlendTest, BlendFx,
			// At the recorded draw. RtxConf: the albedo is in an rtx.conf list
			// the API can't express; RtxHash: its D3D9 hash can't be computed,
			// so the lists can't be checked. Stream*: the VB / IB can't be
			// cached (stream_reason): created D3DUSAGE_DYNAMIC; streamed into
			// and written again soon after (SceneStreamedRelaxed, retried);
			// rewritten: a claimed mesh's window was refilled with different
			// bytes twice within kChangeWindow frames (verify_written; retried
			// like a soft refusal; a refill with the same bytes never counts);
			// streamed into at all (SceneStreamedRelaxed=0, the strict rule);
			// the Lock detours aren't in place.
			Lod, DrawShader, DrawDenied, DrawVs, DrawLayout, AlphaFunc, GlowTint, Protocol, RtxConf, RtxHash,
			StreamDynamic, StreamRelock, StreamRewritten, StreamSticky, StreamUntracked,
			// At the resolve.
			// Actor: owned by an ACHR / ACRE; hair, hats, eyes, weapons move
			// with the skinned body and stay on draw capture, per frame.
			Subtree, NoAnchor, NoRef, Culled, BaseType, Actor, Buffer, Build, NoData,
			// At the hide poll. TexChurn: a claimed geometry's diffuse changed
			// twice within kTexChangeWindow frames (an animated texture);
			// retried like a soft refusal.
			TexChurn,
			Count
		};
		constexpr const char* kReasonNames[] = {
			"class", "terrain", "skinned", "water", "shader", "blend", "blendTest", "blendFx",
			"lod", "drawShader", "denied", "vs", "layout", "alphaFunc", "glowTint", "protocol", "rtxconf", "rtxHash",
			"streamed.dynamic", "streamed.relock", "streamed.rewritten", "streamed.sticky", "streamed.untracked",
			"subtree", "noAnchor", "noRef", "culled", "baseType", "actor", "buffer", "build", "nodata",
			"texChurn",
		};
		static_assert(std::size(kReasonNames) == static_cast<size_t>(Reason::Count));

		// What the geometry's NiAlphaProperty makes it (discovery).
		enum class AlphaKind : uint8_t { Opaque, Test, BlendTest, Blend };
		bool is_blend(AlphaKind k) { return k == AlphaKind::BlendTest || k == AlphaKind::Blend; }

		// What a claim carries beyond a plain opaque static (status line
		// "by[...]"): the alpha state its instance got, the categories, and
		// which rule let it in.
		enum : uint32_t
		{
			kClaimTest = 1 << 0,        // alpha test, no blend
			kClaimBlendTest = 1 << 1,   // blend + alpha test with the draw's alpha state (material::blend_test_as_cutout makes these kClaimTest)
			kClaimBlend = 1 << 2,       // blend, no test (SceneClaimAlphaBlend = 2)
			kClaimDecal = 1 << 3,       // DECAL_STATIC / DECAL_DYNAMIC (material::decal_of or rtx.decalTextures)
			kClaimRtxCat = 1 << 4,      // an rtx.conf list mapped to a category
			kClaimGlowGrey = 1 << 5,    // grey / black glow tint (SceneGlowGreyTint)
			kClaimLandShape = 1 << 6,   // a land-shaped draw of a placed object (SceneLandShapeStatics)
			kClaimNewBase = 1 << 7,     // TACT / TERM / FLOR base type
			kClaimGlowTint = 1 << 8,    // coloured glow tint on the material (ApiGlowTint)
		};
		constexpr const char* kClaimNames[] = {
			"test", "blendTest", "blend", "decal", "rtxCat", "glowGrey", "landShape", "newBase", "glowTint" };

		// Refusals that are retried after kAnchorRetryFrames (a relocked
		// buffer's hold runs out; a retry inside it is refused again; a
		// geometry whose contents changed twice is tried again once it may
		// have settled).
		bool soft_refusal(Reason why)
		{
			return why == Reason::NoAnchor || why == Reason::NoRef || why == Reason::Culled || why == Reason::StreamRelock ||
				why == Reason::StreamRewritten || why == Reason::TexChurn;
		}
		bool streamed_reason(size_t why)
		{
			return why >= static_cast<size_t>(Reason::StreamDynamic) && why <= static_cast<size_t>(Reason::StreamUntracked);
		}

		// Why a resolve (or a record) was put off for kRetryFrames.
		// Fresh: a buffer streamed into (DISCARD / NOOVERWRITE) was written
		// under kQuietFrames ago; whether it is filled once or streamed
		// shows at the next record.
		enum class Defer : uint8_t { Anchor, Material, Mesh, Create, Fresh, Count };
		constexpr const char* kDeferNames[] = { "anchor", "material", "mesh", "create", "fresh" };
		static_assert(std::size(kDeferNames) == static_cast<size_t>(Defer::Count));

		enum class State : uint8_t { Candidate, Refused, Live };

		// Where a member's seed stands (status line "members[...]").
		//   claimed   Live from its seed record (or drawn first)
		//   hidden    app-culled, disabled or detached right now, or under a
		//             switch / LOD node showing another child (retried)
		//   place     its owner or attached cell not known yet (retried)
		//   unpacked  no packed buffer and no complete CPU copy yet (retried)
		//   texture   its diffuse has no D3D texture yet (retried)
		//   layout    land-shaped (colour + two texcoords): its draw decides
		//             whether it is land or a placed object
		//   budget    the material build budget ran out (retried)
		enum class Seed : uint8_t { None, Queued, Claimed, Hidden, Place, Unpacked, Texture, Layout, Budget, Count };
		constexpr const char* kSeedNames[] = { "none", "queued", "claimed", "hidden", "place", "unpacked", "texture",
			"layout", "budget" };
		static_assert(std::size(kSeedNames) == static_cast<size_t>(Seed::Count));

		// DrawRec::clamp_mode: no engine clamp mode (the stage sampler's addressing).
		constexpr uint8_t kNoClampMode8 = 0xFF;

		// The candidate's recorded base pass.
		struct DrawRec
		{
			UINT frame = kNoFrame;            // kNoFrame = nothing recorded
			uint64_t mat_key = 0;
			IDirect3DVertexBuffer9* vb = nullptr;
			IDirect3DIndexBuffer9* ib = nullptr;
			UINT stride = 0;
			Attr pos, nrm, uv, col;
			DrawDesc d;
			bool double_sided = false;
			uint32_t claim = 0;               // kClaim* bits the draw decided (alpha state, glow)
			// Hair (PS HairTint): the shader's colour factor 1 + v0.g (2 HairTint - 1),
			// baked into the vertex colours (the API material has no tint).
			bool hair = false;
			float hair_tint[3] = {};
			// The shade property's Texture Clamp Mode (0..3) the material's
			// addressing came from (material::clamp_from_geometry);
			// kNoClampMode8: the stage sampler's. The hide poll re-reads it.
			uint8_t clamp_mode = kNoClampMode8;
			// The decal rules at this pass (material::decal_of: the shader's
			// decal flags).
			material::Decal decal;
			// The engine's diffuse at this pass (engine_diffuse): the hide poll
			// compares it with the engine's diffuse now. diffuse_known false:
			// not a PPL shade property, no check.
			bool diffuse_known = false;
			uintptr_t diffuse_shade = 0, diffuse_tex = 0;
			uintptr_t diffuse_d3d = 0;
			// Recorded from the scene graph (seed_one: the packed buffers and the
			// geometry's properties), not from a draw.
			bool from_graph = false;
			// A seed of geometry the engine has not packed yet: the mesh comes
			// from this NiGeometryData's CPU arrays (strips: NiTriStripsData).
			uintptr_t cpu_data = 0;
			bool cpu_strips = false;
		};

		struct Rec
		{
			State state = State::Candidate;
			Reason why = Reason::Count;       // State::Refused
			bool queued = false;              // in g_queue
			bool dirty = false;               // in g_dirty (pose changed)
			bool visible = true;              // as the runtime has it (Live)
			bool switch_leaf = false;         // under a switch / LOD / range node
			bool ever_recorded = false;       // a base pass was recorded at least once
			uint8_t tries = 0;                // anchor / reference not found yet
			AlphaKind alpha = AlphaKind::Opaque;   // NiAlphaProperty at discovery
			bool land_shape = false;          // a land-shaped draw found it under a reference (SceneLandShapeStatics)
			bool land_unowned = false;        // refused terrain: kLandShapeTries land-shaped draws found no owner
			uint32_t claim = 0;               // Live: kClaim* bits
			uint16_t alpha_flags = 0;         // NiAlphaProperty+0x18 at discovery (refusal report)
			uint32_t passes = 0;              // Refused: passes since the last status line (refusal report)
			uint32_t ps_hash = 0, vs_hash = 0;   // the last FFP_WORLD draw's shaders (refusal report)
			const char* list = nullptr;       // RtxConf: the rtx.conf list naming the albedo
			uint32_t categories = 0;          // Live: the instance's categoryFlags
			UINT last_seen = 0;
			UINT not_before = 0;              // no record before this frame (deferred); soft refusals: retry at
			float pose[sg::kPoseFloats] = {}; // last sampled (Live)
			DrawRec draw;
			// Live.
			uint64_t handle = 0;              // retained instance (0 in SceneDebugDrawInstance)
			uint64_t mesh_key = 0;
			// The VB / IB write generations the mesh was read at (or last
			// verified at): a claimed geometry whose buffer moved on has its
			// window compared with the mesh's bytes at the next tick
			// (check_written -> verify_written).
			uint32_t vb_gen = 0, ib_gen = 0;
			bool verify = false;              // Live, in g_verify: a buffer was written since vb_gen / ib_gen
			UINT change_frame = kNoFrame;     // the last time its claimed window was found with different bytes
			UINT tex_change_frame = kNoFrame; // the last time its claimed diffuse was found changed
			uintptr_t cell = 0, ref = 0, ref_root = 0;   // the owner (Live): its attached cell, reference, 3D root
			uintptr_t switch_node = 0;        // the nearest LOD / switch ancestor (exclusive children)
			uintptr_t switch_branch = 0;      // its child on this geometry's chain
			uint8_t base_type = 0;            // the reference's base form type (status line, trace); set at the owned resolve
			uint32_t ref_id = 0;              // the reference's form id (trace)
			uint32_t trace_moves = 0;         // moves logged by the trace
			UINT trace_d3d9_frame = kNoFrame; // the last "drawn by D3D9" trace line
			// Live: moved since its claim (in g_movers). Its pose is read at every
			// tick, not only at the engine's render passes: Havok keeps moving it
			// while the engine culls it, and a pose read only when the engine
			// renders it would leave the instance where the engine last drew it.
			bool mover = false;
			bool pose_unseen = false;         // the pending pose change was read by a tick poll, not at a render pass
			bool trace_unseen = false;        // the last traced move's source was a tick poll
			remixapi_Transform xform = {};    // absolute, as the runtime has it
			// Scene membership: the engine monitor's member feed listed it (a
			// geometry of a loaded non-actor reference's 3D).
			bool member = false;
			bool classified = false;          // classify() ran (discovery or seed)
			bool seed_queued = false;         // in g_seed
			Seed seed = Seed::None;           // what the last seed attempt found
			// Live, claimed from its seed record: its material came from its
			// properties. Its next base pass reaches the renderer once, which
			// confirms the claim or replaces it with the draw's record.
			bool provisional = false;
			uint8_t confirm_frames = 0;       // frames a provisional claim's passes reached the renderer unconfirmed
			UINT consume_frame = kNoFrame;    // the renderer drops its draws this frame (provisional / replaced)
		};

		struct Mat
		{
			remixapi_MaterialHandle handle = nullptr;
			std::vector<uint64_t> textures;   // references held on api::textures
			bool no_vertex_color = false;
			// Built with useDrawCallAlphaState: its instances carry the alpha
			// state (material::alpha_blend with this test).
			bool blend = false;
			uint8_t alpha_func = 0, alpha_ref = 0;
			uint32_t categories = 0;          // rtx.conf lists mapped to instance categories
			uint32_t refs = 0;                // meshes using it
			UINT last_used = 0;
			// Its addressing (material::Desc wrap_u / wrap_v) and the albedo's
			// D3D9 hash: the [Addressing] mesh line and the claim re-check.
			uint8_t wrap_u = 1, wrap_v = 1;
			uint64_t albedo_d3d9 = 0;
		};

		struct Mesh
		{
			remixapi_MeshHandle handle = nullptr;
			uint64_t material_key = 0;
			bool vertex_color = false;
			bool blend = false;               // its material's (Mat::blend, alpha_func, alpha_ref)
			uint8_t alpha_func = 0, alpha_ref = 0;
			float bound_c[3] = {};
			float bound_r = 0.0f;
			uint64_t content = 0;             // window_content() of the bytes it was built from
			uint32_t refs = 0;                // instances using it
			UINT last_used = 0;
			material::MeshFacts facts;        // winding, vertex alpha, reversed copy ([DecalRoute])
		};

		struct CellRec
		{
			uintptr_t root = 0;               // the cell's 3D root as the plugin last read it
			std::unordered_set<uintptr_t> live;   // claimed geometries anchored here
		};

		struct Create
		{
			uintptr_t geo = 0;
			remixapi_InstanceInfo info = {};
			bool has_ext = false;             // ext goes on info.pNext
			remixapi_InstanceInfoBlendEXT ext = {};
		};

		// The InstanceInfoBlendEXT a geometry's instance carries: the blended
		// material's alpha state (with the vertex-colour modulate), or the
		// vertex-colour modulate alone; false = none. The vertex alpha joins
		// the opacity only with the shader's Vertex_Alpha flag
		// (material::shader_alpha); `vertex_alpha_dropped` says so.
		bool instance_ext(const Mesh& m, const Rec& r, remixapi_InstanceInfoBlendEXT& out,
			bool* vertex_alpha_dropped = nullptr)
		{
			bool has = false;
			if (m.blend)
			{
				out = material::alpha_blend(m.alpha_func, m.alpha_ref, m.vertex_color);
				has = true;
			}
			else if (m.vertex_color)
			{
				out = material::vertex_color_blend();
				has = true;
			}
			const bool dropped = has && material::shader_alpha(r.draw.decal, out);
			if (vertex_alpha_dropped) *vertex_alpha_dropped = dropped;
			return has;
		}

		enum class Cause : uint8_t { Dtor, Cell, Other };

		// ---- State (render thread only) ----

		std::unordered_map<uintptr_t, Rec> g_geos;
		std::vector<uintptr_t> g_queue;                       // recorded candidates to resolve
		std::vector<uintptr_t> g_dirty;                       // claimed geometries whose pose changed
		std::unordered_set<uintptr_t> g_movers;               // claimed geometries that moved since their claim (Rec::mover)
		std::vector<uintptr_t> g_seen_hidden;                 // hidden claimed geometries the engine drew
		std::vector<uintptr_t> g_live_list;                   // round-robin order of the hide poll
		bool g_live_list_dirty = true;
		size_t g_poll_i = 0;
		std::unordered_map<uintptr_t, CellRec> g_cells;
		std::unordered_map<uint64_t, Mesh> g_meshes;
		std::unordered_map<uint64_t, Mat> g_mats;
		std::vector<uint64_t> g_doomed;                       // retained handles to destroy this tick
		uint32_t g_live = 0;                                  // retained handles alive
		uint32_t g_claimed = 0;                               // Live records (with or without a handle)
		bool g_recreate = false;                              // SceneDebugDrawInstance went off
		std::vector<uint8_t> g_base_ok;                       // SceneBaseTypes as a 256-entry table
		uint32_t g_seen_epoch = 0;                            // identity::built_write_epoch() at the last check_written
		std::vector<uintptr_t> g_verify;                      // claimed geometries whose buffer was written (compared next tick)
		// VB / IB -> the claimed geometries reading it (check_written looks
		// only at the claims of the buffers identity says were written).
		std::unordered_map<const void*, std::vector<uintptr_t>> g_buffer_claims;
		std::vector<const void*> g_written;                   // take_built_written scratch
		uint32_t g_change_logs = 0;                           // content changes logged (kChangeLogs)

		// Scene membership (the engine monitor's member feed): members to
		// seed, in feed order (g_seed_pos is the next one), and members held
		// back until a frame (hidden, place, unpacked, texture, budget).
		std::vector<uintptr_t> g_seed;
		size_t g_seed_pos = 0;
		struct SeedWait
		{
			uintptr_t geo = 0;
			UINT at = 0;
		};
		std::vector<SeedWait> g_seed_wait;
		// Members to look at again at the next tick (a build slot refused:
		// asking every tick keeps their place in the build budget's queue).
		std::vector<uintptr_t> g_seed_next;
		UINT g_seed_wait_frame = 0;                           // the last scan of g_seed_wait
		bool g_members_resend = true;                         // the record map lost its members: ask the feed for all
		UINT g_sweep_frame = 0;                               // the last membership sweep
		// The material a seed described from the geometry's properties, kept
		// while its claim is provisional: what the first base pass is
		// compared with (seed parity, by field).
		struct SeedDesc
		{
			IDirect3DBaseTexture9* diffuse = nullptr;
			IDirect3DBaseTexture9* normal = nullptr;
			IDirect3DBaseTexture9* glow = nullptr;
			IDirect3DBaseTexture9* height = nullptr;
			float gloss = 0.0f;
			uint8_t alpha_func = 0, alpha_ref = 0;
			bool blend = false, normal_role = false, gloss_alpha = false;
			uint8_t wrap_u = 1, wrap_v = 1, filter = 1;
			float glow_scale = 1.0f;
			float glow_tint[3] = { 1.0f, 1.0f, 1.0f };
		};
		std::unordered_map<uintptr_t, SeedDesc> g_seed_desc;
		// Seed parity fields (Counters::seed_field), in bit order.
		constexpr const char* kSeedFieldNames[] = { "diffuse", "normal", "glow", "height", "gloss", "alphaTest", "blend",
			"wrap", "filter", "twoSided", "window", "layout", "decal", "glowTint", "material" };
		constexpr size_t kSeedFields = std::size(kSeedFieldNames);

		// The config as the dispatcher / renderer see it, set each tick.
		bool g_active = false;
		bool g_skip_passes = false;
		bool g_draw_instance = false;
		UINT g_di_frame = kNoFrame;
		// Whether this frame's claimed geometry reaches the runtime: the
		// retained path (or SceneDebugDrawInstance's origin) is up. Decided at
		// the frame's first pass; false = every pass goes to D3D9.
		UINT g_ok_frame = kNoFrame;
		bool g_ok = false;

		struct Counters
		{
			uint64_t discovered = 0, refused_new = 0, recorded = 0, created = 0, create_failed = 0;
			uint64_t destroyed_dtor = 0, destroyed_cell = 0, destroyed_other = 0;
			uint64_t hidden = 0, shown = 0, moved = 0, deferred = 0, stale_records = 0;
			// Movers: sightings whose pose bits changed, those the deadband
			// absorbed (rounding), and counted moves that stayed sub-visual.
			uint64_t pose_changes = 0, pose_absorbed = 0, moved_small = 0;
			// Pose changes read by the tick polls (movers, hide poll) of geometry
			// the engine didn't render that frame.
			uint64_t pose_unseen = 0;
			uint64_t suppressed_passes = 0, unready_passes = 0;
			uint64_t meshes_built = 0, meshes_shared = 0, materials_built = 0, upload_bytes = 0;
			uint64_t window_mismatch = 0, buffer_mismatch = 0, polled = 0, di_draws = 0;
			uint64_t hidden_by[6] = {};       // hide poll verdicts by cause (Hide)
			// A blended candidate's pass that wasn't its plain alpha-blend base
			// pass (a light pass, blend off, other factors): not recorded.
			uint64_t draw_blend_not_base = 0;
			// Refused geometry, by reason: its render passes (dispatcher) and
			// its draws that reached the FFP_WORLD branch (on_static_draw):
			// what each bucket still costs per frame.
			uint64_t refused_passes[static_cast<size_t>(Reason::Count)] = {};
			uint64_t refused_draws[static_cast<size_t>(Reason::Count)] = {};
			// FFP_WORLD draws seen by on_static_draw: no dispatcher geometry,
			// a geometry the dispatcher never classified, a candidate held
			// back (backoff), not a base pass, no diffuse bound, no build budget.
			uint64_t draw_no_geo = 0, draw_unknown = 0, draw_backoff = 0, draw_not_base = 0, draw_no_diffuse = 0,
				draw_budget = 0;
			uint64_t resolves = 0;
			// Owner lookups (per resolve that got that far): owned and placed;
			// no owner (not in the map yet, or never under a reference); owned
			// but detached / app-culled / disabled, or its cell not attached;
			// the chain to the owner's root unreadable.
			uint64_t owner_hit = 0, owner_unknown = 0, owner_detached = 0, owner_culled = 0, owner_disabled = 0,
				owner_no_cell = 0, owner_unreadable = 0, soft_retries = 0;
			uint64_t owner_gone = 0;          // claims unclaimed by FNVR_EV_OWNER_GONE
			uint64_t deferred_by[static_cast<size_t>(Defer::Count)] = {};
			// Claimed geometries whose buffer was written, by what the compare
			// of their window with the mesh found: the same bytes (kept, no
			// rebuild), different bytes (unclaimed, claimed again from the new
			// contents), different twice within kChangeWindow (refused
			// streamed.rewritten), or no verdict yet (written during the read).
			uint64_t rewrite_same = 0, rewrite_changed = 0, rewrite_refused = 0, rewrite_retry = 0;
			uint64_t relaxed_claims = 0;      // resolves that passed only by the relaxed streamed rule
			// Scene membership: members the feed brought in (new to the record
			// map), seed attempts, seed records queued for the resolve, seeds
			// held back by reason, and what a provisional claim's first base
			// pass found: the same record (confirmed), a different one
			// (replaced, by field: SeedField), or a refusal the draw shows.
			uint64_t members_new = 0, seeds = 0, seed_records = 0, seed_confirmed = 0, seed_replaced = 0,
				seed_refused_at_draw = 0, seed_unconfirmed = 0, seed_swept = 0;
			double seed_us = 0.0;             // time in the seed pass and the member feed
			uint64_t seed_held[static_cast<size_t>(Seed::Count)] = {};
			uint64_t seed_field[16] = {};
		};
		Counters g_c;             // since the last status line
		uint64_t g_act_created = 0, g_act_destroyed = 0, g_act_cells_in = 0, g_act_cells_out = 0;
		UINT g_act_frame = 0;
		UINT g_stats_frame = 0;
		uint32_t g_dumped = 0;
		uint32_t g_faults_logged = 0;                         // sg::faults() at the last status line
		UINT g_active_frame = 0;                              // the frame the feature went active
		int g_report_done = -1;                               // SceneDebugRefusalReport the last report ran with
		UINT g_report_frame = 0;                              // the frame of the last report
		// The first report waits this long after activation (cells loaded,
		// every visible geometry classified).
		constexpr UINT kReportAfterFrames = 1800;

		// Moves per claimed geometry since the last status line (who moves:
		// an animated MSTT / ACTI moves every frame; noise would be spread).
		struct MoveStat
		{
			uint32_t moves = 0;
			float max_dt = 0.0f, max_dl = 0.0f;
		};
		std::unordered_map<uintptr_t, MoveStat> g_moves;
		bool g_logged_hooks = false, g_logged_inactive = false, g_logged_active = false;

		std::vector<uintptr_t> g_scratch_dead;

		// ---- Trace (SceneDebugTraceBaseTypes) ----
		// Every state change of the geometries whose reference has a traced base
		// form type, as [SceneTrace] lines: claims, refusals, deferrals, moves,
		// hides / shows with their cause, stale buffers, unclaims with their
		// cause, render passes resuming after a gap, D3D9 drawing the geometry
		// after an unclaim, its death. A geometry is known by its base type from
		// its first anchored resolve on (the record keeps it through unclaims).
		// Capped per status window.
		std::vector<uint8_t> g_trace_ok;                      // 256-entry table of traced base types
		uint32_t g_trace_lines = 0;                           // lines since the last status line
		constexpr uint32_t kTraceLinesPerWindow = 300;
		constexpr uint32_t kTraceMovesFull = 20;              // per geometry: every move, then every kTraceMoveEvery-th
		constexpr uint32_t kTraceMoveEvery = 60;
		constexpr UINT kTraceSightingGap = 10;                // a pass after this many frames without one is logged
		constexpr UINT kTraceD3d9Every = 120;                 // "drawn by D3D9" at most this often per geometry
		constexpr const char* kStateNames[] = { "candidate", "refused", "live" };
		std::string describe_buffer(const void* p);   // defined with the refusal report below

		bool traced(const Rec& r)
		{
			return r.base_type != 0 && g_trace_ok.size() == 256 && g_trace_ok[r.base_type] != 0;
		}

		void trace(uintptr_t geo, const Rec& r, const std::string& what)
		{
			if (g_trace_lines >= kTraceLinesPerWindow)
			{
				if (g_trace_lines == kTraceLinesPerWindow)
				{
					g_trace_lines++;
					shared::common::log("SceneTrace", std::format("{} lines this window: the rest wait for the next status line",
						kTraceLinesPerWindow), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				}
				return;
			}
			g_trace_lines++;
			const UINT now = frame();
			shared::common::log("SceneTrace", std::format(
				"f{} geo=0x{:X} base=0x{:02X} ref={:08X} {} vis={} handle={} seen={}f ago | {}",
				now, geo, r.base_type, r.ref_id, kStateNames[static_cast<size_t>(r.state)], r.visible ? 1 : 0, r.handle,
				now - r.last_seen, what), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		// ---- Helpers ----

		double now_us()
		{
			static LARGE_INTEGER f = {};
			if (!f.QuadPart) QueryPerformanceFrequency(&f);
			LARGE_INTEGER c;
			QueryPerformanceCounter(&c);
			return static_cast<double>(c.QuadPart) * 1.0e6 / static_cast<double>(f.QuadPart);
		}

		const auto& cfg() { return shared::common::config::get().remix_api; }

		uint8_t unorm8(float v)
		{
			if (!(v > 0.0f)) return 0;
			return v >= 1.0f ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f);
		}

		DWORD render_state(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD fallback)
		{
			DWORD v = fallback;
			return SUCCEEDED(dev->GetRenderState(s, &v)) ? v : fallback;
		}

		// The world pose (read_pose order) as the runtime takes it: absolute;
		// the replay subtracts the stable origin. NiMatrix3 maps column
		// vectors.
		bool pose_transform(const float (&p)[sg::kPoseFloats], remixapi_Transform& t)
		{
			const float scale = p[12];
			if (!std::isfinite(scale) || !(scale > 0.0f)) return false;
			for (int r = 0; r < 3; r++)
			{
				for (int c = 0; c < 3; c++)
				{
					t.matrix[r][c] = scale * p[r * 3 + c];
					if (!std::isfinite(t.matrix[r][c])) return false;
				}
				t.matrix[r][3] = p[9 + r];
				if (!std::isfinite(t.matrix[r][3])) return false;
			}
			return true;
		}

		// One float ulp at magnitude m (m >= 0).
		float ulp_at(float m)
		{
			if (!(m > 0.0f) || !std::isfinite(m)) return 0.0f;
			int e = 0;
			std::frexp(m, &e);   // m = f * 2^e, f in [0.5, 1)
			return std::ldexp(1.0f, e - 24);
		}

		// How far the pose moved: largest translation and linear-part deltas.
		struct PoseDelta
		{
			float dt = 0.0f, dl = 0.0f;
		};
		PoseDelta pose_delta(const remixapi_Transform& a, const remixapi_Transform& b)
		{
			PoseDelta d;
			for (int r = 0; r < 3; r++)
			{
				for (int c = 0; c < 3; c++)
					d.dl = (std::max)(d.dl, std::fabs(a.matrix[r][c] - b.matrix[r][c]));
				d.dt = (std::max)(d.dt, std::fabs(a.matrix[r][3] - b.matrix[r][3]));
			}
			return d;
		}

		// A real move: translation past the deadband (kMoveDeadband, or
		// kMoveUlps float ulps of the coordinate, whichever is larger), or the
		// linear part past float noise. Anything less is the engine's rounding
		// on an object at rest and sends nothing.
		bool moved(const remixapi_Transform& a, const remixapi_Transform& b)
		{
			for (int r = 0; r < 3; r++)
			{
				for (int c = 0; c < 3; c++)
				{
					const float tol = kLinearTolerance * (std::max)(1.0f, std::fabs(a.matrix[r][c]));
					if (!(std::fabs(a.matrix[r][c] - b.matrix[r][c]) <= tol)) return true;
				}
				const float mag = (std::max)(std::fabs(a.matrix[r][3]), std::fabs(b.matrix[r][3]));
				const float band = (std::max)(kMoveDeadband, kMoveUlps * ulp_at(mag));
				if (!(std::fabs(a.matrix[r][3] - b.matrix[r][3]) <= band)) return true;
			}
			return false;
		}

		void refuse(Rec& r, Reason why)
		{
			r.state = State::Refused;
			r.why = why;
			r.draw.frame = kNoFrame;
		}

		// NiAlphaProperty+0x18: bit 0 blend, bits 1-4 / 5-8 the source and
		// destination factors (NiAlphaProperty::AlphaFunction: 6 SRCALPHA,
		// 7 INVSRCALPHA), bit 9 test (verified; the rest follows Gamebryo's
		// layout: a misread only refuses more, as blendFx, and the recorded
		// draw's own blend state must match as well). Reason::Count = a candidate.
		Reason alpha_class(bool have, uint16_t af, AlphaKind& kind)
		{
			kind = AlphaKind::Opaque;
			if (!have) return Reason::Count;
			const bool test = ((af >> 9) & 1u) != 0;
			if (!(af & 1u))
			{
				kind = test ? AlphaKind::Test : AlphaKind::Opaque;
				return Reason::Count;
			}
			const uint32_t src = (af >> 1) & 0xFu, dst = (af >> 5) & 0xFu;
			if (src != 6 || dst != 7) return Reason::BlendFx;
			kind = test ? AlphaKind::BlendTest : AlphaKind::Blend;
			const int mode = cfg().scene_claim_alpha_blend;
			if (test ? mode >= 1 : mode >= 2) return Reason::Count;
			return test ? Reason::BlendTest : Reason::Blend;
		}

		bool read_alpha(const sg::GeoRead& gr, uint16_t& af)
		{
			af = 0;
			const uintptr_t ap = gr.props[sg::fnv::kPropAlpha];
			return ap && sg::read_u16(ap + sg::fnv::ALPHA_flags, af);
		}

		// A land shader: several BaseMap layers, or its NormalMap array at s7
		// (api_terrain's land_shader). A static's PS has one BaseMap at s0.
		bool land_like(const PsSlotMap* m)
		{
			return !m || m->base_map_count > 1 || m->multi_layer_count > 0 ||
				(m->normal_map_count > 0 && m->normal_map_reg == 7);
		}

		void defer(Rec& r, Defer why)
		{
			r.not_before = frame() + kRetryFrames;
			r.draw.frame = kNoFrame;
			g_c.deferred++;
			g_c.deferred_by[static_cast<size_t>(why)]++;
		}

		// A resolve refusal for want of a place: a candidate again after
		// kAnchorRetryFrames (pass_filter).
		void refuse_soft(Rec& r, Reason why)
		{
			refuse(r, why);
			r.tries = 0;
			r.not_before = frame() + kAnchorRetryFrames;
		}

		void refuse_stream(Rec& r, Reason why)
		{
			if (soft_refusal(why)) refuse_soft(r, why);
			else refuse(r, why);
		}

		void release_material(uint64_t key)
		{
			const auto it = g_mats.find(key);
			if (it == g_mats.end() || !it->second.refs) return;
			it->second.refs--;
			it->second.last_used = frame();
		}

		void release_mesh(uint64_t key)
		{
			const auto it = g_meshes.find(key);
			if (it == g_meshes.end() || !it->second.refs) return;
			it->second.refs--;
			it->second.last_used = frame();
		}

		// A member goes to the seed queue (seeded at the next tick's seed pass).
		void queue_seed(uintptr_t geo, Rec& r)
		{
			if (r.seed_queued) return;
			r.seed_queued = true;
			r.seed = Seed::Queued;
			g_seed.push_back(geo);
		}

		// A member held back: looked at again from `frames` frames on.
		void hold_seed(uintptr_t geo, Rec& r, Seed why, UINT frames)
		{
			r.seed = why;
			g_c.seed_held[static_cast<size_t>(why)]++;
			if (frames <= 1) g_seed_next.push_back(geo);
			else g_seed_wait.push_back({ geo, frame() + frames });
		}

		// A claimed geometry's instance goes (its handle joins this tick's
		// destroy batch); its passes reach the renderer again from the next
		// frame. The record stays unless the caller erases it. A member
		// unclaimed for what it holds (contents, clamp mode, diffuse) is
		// seeded again from the scene graph at the next tick, so it is not
		// left waiting for a draw.
		void unclaim(uintptr_t geo, Rec& r, Cause cause)
		{
			if (r.state != State::Live) return;
			r.provisional = false;
			g_seed_desc.erase(geo);
			if (traced(r))
				trace(geo, r, std::format("unclaimed: cause={}",
					cause == Cause::Dtor ? "dtor (~NiGeometry)" : cause == Cause::Cell ? "cell left / root changed"
						: "other (contents changed, drop)"));
			if (r.handle)
			{
				g_doomed.push_back(r.handle);
				if (g_live) g_live--;
				r.handle = 0;
			}
			if (g_claimed) g_claimed--;
			for (const void* p : { static_cast<const void*>(r.draw.vb), static_cast<const void*>(r.draw.ib) })
			{
				const auto bc = g_buffer_claims.find(p);
				if (bc == g_buffer_claims.end()) continue;
				auto& v = bc->second;
				if (const auto at = std::find(v.begin(), v.end(), geo); at != v.end())
				{
					*at = v.back();
					v.pop_back();
				}
				if (v.empty()) g_buffer_claims.erase(bc);
			}
			release_mesh(r.mesh_key);
			if (const auto c = g_cells.find(r.cell); c != g_cells.end()) c->second.live.erase(geo);
			r.state = State::Candidate;
			r.draw.frame = kNoFrame;
			r.mesh_key = 0;
			r.tries = 0;
			r.verify = false;
			r.mover = false;
			r.pose_unseen = false;
			g_movers.erase(geo);
			g_live_list_dirty = true;
			switch (cause)
			{
			case Cause::Dtor: g_c.destroyed_dtor++; break;
			case Cause::Cell: g_c.destroyed_cell++; break;
			default:          g_c.destroyed_other++; break;
			}
			g_act_destroyed++;
			r.seed = Seed::None;
			if (cause == Cause::Other && r.member) queue_seed(geo, r);
		}

		void set_visible(Rec& r, bool visible)
		{
			if (r.visible == visible) return;
			r.visible = visible;
			(visible ? g_c.shown : g_c.hidden)++;
			if (!r.handle) return;
			remixapi_RetainedInstanceUpdate u = {};
			u.handle = r.handle;
			u.flags = REMIXAPI_RETAINED_UPDATE_VISIBILITY;
			u.visible = visible ? 1 : 0;
			retained::update_external(u);
		}

		// ---- Buffer identity ----

		// What identity recorded, with the usage filled in from the buffer's
		// own description when the proxy didn't see its creation.
		identity::BufferInfo buffer_info(IDirect3DVertexBuffer9* vb)
		{
			identity::BufferInfo i = identity::buffer(vb);
			if (vb && !i.created)
			{
				D3DVERTEXBUFFER_DESC d = {};
				if (SUCCEEDED(vb->GetDesc(&d))) { i.usage = d.Usage; i.pool = d.Pool; i.length = d.Size; i.dynamic = (d.Usage & D3DUSAGE_DYNAMIC) != 0; }
			}
			return i;
		}

		identity::BufferInfo buffer_info(IDirect3DIndexBuffer9* ib)
		{
			identity::BufferInfo i = identity::buffer(ib);
			if (ib && !i.created)
			{
				D3DINDEXBUFFER_DESC d = {};
				if (SUCCEEDED(ib->GetDesc(&d))) { i.usage = d.Usage; i.pool = d.Pool; i.length = d.Size; i.dynamic = (d.Usage & D3DUSAGE_DYNAMIC) != 0; }
			}
			return i;
		}

		// Why the recorded buffers can't be cached (Reason::Count: they can).
		//   * DYNAMIC usage: refilled at will, and the bridge's READONLY lock
		//     of one may not even return the data.
		//   * identity's `rewritten` (a buffer written again after any path
		//     built a mesh from it) is not a reason: it says nothing about the
		//     bytes, and a buffer the engine refills with the same bytes would
		//     be refused for good and vanish whenever the engine culls it. A
		//     claimed window that really changes is caught by verify_written
		//     instead (streamed.rewritten there).
		//   * streamed into (a DISCARD / NOOVERWRITE write lock since the
		//     buffer was created): the strict rule (SceneStreamedRelaxed=0)
		//     refuses it. D3D9 honours both flags only on DYNAMIC buffers,
		//     so on any other buffer they say nothing by themselves; the
		//     relaxed rule refuses only a buffer written again within the
		//     rewrite window (relock; retried once the hold runs out) and
		//     defers one written moments ago (`fresh`: filled once, or the
		//     first of a stream; the next record tells).
		// `relaxed`: cacheable only by the relaxed rule. A claimed buffer that
		// is written after all is caught by the write generations
		// (check_written) and its window compared at the next tick
		// (verify_written).
		Reason stream_reason(const identity::BufferInfo& vb, const identity::BufferInfo& ib, bool& fresh, bool& relaxed)
		{
			fresh = false;
			relaxed = false;
			if (vb.dynamic || ib.dynamic) return Reason::StreamDynamic;
			if (!vb.streamed && !ib.streamed) return Reason::Count;
			if (!cfg().scene_streamed_relaxed) return Reason::StreamSticky;
			for (const identity::BufferInfo* b : { &vb, &ib })
			{
				if (!b->streamed) continue;
				if (b->relocked) return Reason::StreamRelock;
				if (b->since_write < kQuietFrames) fresh = true;
			}
			relaxed = !fresh;
			return Reason::Count;
		}

		// ---- Dispatcher (render thread) ----

		void draw_instances_now();

		// First pass of a geometry: what it is decides once whether it can be
		// a candidate. The engine is rendering it: it is alive.
		// Owned by an actor (the engine monitor's owner map): hair, hats, eyes
		// and weapons move with the skinned body, so they stay on draw capture.
		bool actor_owned(uintptr_t geo)
		{
			FnvRemixOwner o;
			return nvse_link::owner_of(geo, o) && (o.flags & FNVR_OW_ACTOR);
		}

		// What the geometry is decides once whether it can be a candidate:
		// at its first render pass (discover) or its first seed. The caller
		// keeps it alive (the engine renders it, or begin_resolve).
		void classify(uintptr_t geo, Rec& r)
		{
			r.classified = true;
			auto fail = [&](Reason why) { refuse(r, why); g_c.refused_new++; };
			const sg::ClassInfo* ci = sg::class_of(geo);
			sg::GeoRead gr;
			if (!ci || !ci->geometry) fail(Reason::Class);
			else if (ci->segmented) fail(Reason::Terrain);
			else if (ci->leaf != sg::Leaf::TriShape && ci->leaf != sg::Leaf::TriStrips) fail(Reason::Class);
			else if (!sg::read_geo(geo, gr)) fail(Reason::NoData);
			else if (gr.skin) fail(Reason::Skinned);
			else if (actor_owned(geo)) fail(Reason::Actor);
			else
			{
				const uintptr_t shade = gr.props[sg::fnv::kPropShade];
				const sg::ClassInfo* sci = shade ? sg::class_of(shade) : nullptr;
				uint16_t af = 0;
				const bool have_af = read_alpha(gr, af);
				r.alpha_flags = af;
				AlphaKind ak = AlphaKind::Opaque;
				const Reason ar = alpha_class(have_af, af, ak);
				if (sci && sci->shade_water) fail(Reason::Water);
				else if (!sci || !sci->shade_ppl) fail(Reason::Shader);
				else if (ar != Reason::Count) fail(ar);
				else
				{
					r.alpha = ak;
					g_c.discovered++;
				}
			}
		}

		void discover(uintptr_t geo, UINT now)
		{
			Rec r;
			r.last_seen = now;
			classify(geo, r);
			g_geos.emplace(geo, r);
		}

		// A buffer some mesh was built from was written (identity's epoch
		// moved): every claimed geometry whose VB / IB generation moved on
		// since its mesh was read (or last verified) is queued for the next
		// tick, which compares its window's bytes with the mesh's
		// (verify_written). Its instance stays meanwhile: the engine refills
		// most of these buffers with the very same bytes, and between frames
		// the write is complete.
		void mark_written(uintptr_t geo, Rec& r)
		{
			if (r.state != State::Live || r.verify) return;
			if (identity::buffer(r.draw.vb).gen == r.vb_gen && identity::buffer(r.draw.ib).gen == r.ib_gen) return;
			r.verify = true;
			g_verify.push_back(geo);
			if (traced(r))
				trace(geo, r, std::format("written: a buffer the mesh was read from was written (vb gen {} -> {}, ib gen {} -> {}); "
					"its window is compared at the next tick", r.vb_gen, identity::buffer(r.draw.vb).gen, r.ib_gen,
					identity::buffer(r.draw.ib).gen));
		}

		void check_written()
		{
			const uint32_t e = identity::built_write_epoch();
			if (e == g_seen_epoch) return;
			g_seen_epoch = e;
			if (!identity::take_built_written(g_written))
			{
				for (auto& [geo, r] : g_geos) mark_written(geo, r);
				return;
			}
			for (const void* p : g_written)
			{
				const auto bc = g_buffer_claims.find(p);
				if (bc == g_buffer_claims.end()) continue;
				for (const uintptr_t geo : bc->second)
					if (const auto it = g_geos.find(geo); it != g_geos.end()) mark_written(geo, it->second);
			}
		}

		bool pass_filter(uintptr_t geo)
		{
			const UINT now = frame();
			if (g_draw_instance && now != g_di_frame)
			{
				// The frame's first render pass: every visible instance is
				// drawn now, before the HUD triggers the RTX injection.
				g_di_frame = now;
				draw_instances_now();
			}
			if (now != g_ok_frame)
			{
				g_ok_frame = now;
				g_ok = g_draw_instance ? stable_origin::active() : retained::available();
			}
			// A geometry destroyed on another thread since the last tick: its
			// address may already be a new object's. Neither is touched until
			// the drain.
			if (sg::death_pending(geo)) return false;
			const sg::FaultSite site("dispatch");
			const auto it = g_geos.find(geo);
			if (it == g_geos.end())
			{
				discover(geo, now);
				return false;
			}
			Rec& r = it->second;
			// A member the seed has not looked at yet.
			if (!r.classified) classify(geo, r);
			const bool first_today = r.last_seen != now;
			const UINT since_seen = now - r.last_seen;
			r.last_seen = now;
			if (r.state == State::Refused && soft_refusal(r.why) && now >= r.not_before)
			{
				// Its place may be known now: recorded and resolved again.
				r.state = State::Candidate;
				r.why = Reason::Count;
				r.tries = 0;
				r.not_before = 0;
				g_c.soft_retries++;
			}
			if (r.state == State::Refused)
			{
				g_c.refused_passes[static_cast<size_t>(r.why)]++;
				if (r.passes != UINT32_MAX) r.passes++;
			}
			if (r.state != State::Live) return false;
			check_written();
			if (first_today)
			{
				// The engine stopped rendering it for a while (culled, faded, or not
				// in the world): when its passes resume.
				if (since_seen > kTraceSightingGap && traced(r))
					trace(geo, r, std::format("render pass again after {} frames without one (the engine culled or did not draw it)", since_seen));
				// Sighting of a claimed geometry: its pose, compared bitwise.
				float p[sg::kPoseFloats];
				if (sg::read_pose(geo, p) && std::memcmp(p, r.pose, sizeof(p)) != 0)
				{
					std::memcpy(r.pose, p, sizeof(p));
					if (!r.dirty)
					{
						r.dirty = true;
						g_dirty.push_back(geo);
					}
				}
				// Hidden, yet the engine renders it: it is in the world.
				if (!r.visible) g_seen_hidden.push_back(geo);
			}
			// A provisional claim's passes reach the renderer, which records
			// its base pass (confirming or replacing the claim) and drops every
			// draw of it this frame: its instance draws it.
			if (r.provisional && r.visible && r.handle && g_ok && now >= r.not_before &&
				!menu_scene::geometry_in_menu_scene(geo))
			{
				if (first_today && ++r.confirm_frames > kConfirmFrames)
				{
					r.provisional = false;
					g_seed_desc.erase(geo);
					g_c.seed_unconfirmed++;
				}
				else
				{
					r.consume_frame = now;
					return false;
				}
			}
			if (!g_skip_passes || !r.visible || (!r.handle && !g_draw_instance)) return false;
			if (!g_ok)
			{
				g_c.unready_passes++;
				return false;
			}
			// A menu scene node's geometry is never suppressed: its D3D9 draw
			// goes to the screen (menu_scene.hpp) and sets no camera.
			if (menu_scene::geometry_in_menu_scene(geo)) return false;
			// No D3D9 draw follows this pass: the frame's world camera goes
			// out here when no draw sent it yet.
			api_camera::on_claimed_static_pass();
			g_c.suppressed_passes++;
			return true;
		}

		void draw_instances_now()
		{
			auto* b = bridge();
			if (!b || !stable_origin::active()) return;
			const double* o = stable_origin::origin();
			remixapi_InstanceInfoBlendEXT blend = {};
			for (const auto& [geo, r] : g_geos)
			{
				if (r.state != State::Live || !r.visible) continue;
				const auto mit = g_meshes.find(r.mesh_key);
				if (mit == g_meshes.end() || !mit->second.handle) continue;
				remixapi_InstanceInfo ii = {};
				ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
				ii.pNext = instance_ext(mit->second, r, blend) ? &blend : nullptr;
				ii.categoryFlags = r.categories;
				ii.mesh = mit->second.handle;
				ii.transform = r.xform;
				for (int k = 0; k < 3; k++)
					ii.transform.matrix[k][3] = static_cast<float>(static_cast<double>(r.xform.matrix[k][3]) - o[k]);
				ii.transform = stable_origin::to_render(ii.transform);
				ii.doubleSided = r.draw.double_sided ? 1 : 0;
				if (b->DrawInstance(&ii) == REMIXAPI_ERROR_CODE_SUCCESS) g_c.di_draws++;
			}
		}

		// ---- Residency: the attached cells and their roots ----

		void leave_cell(CellRec& cr, Cause cause)
		{
			// Copy: unclaim edits cr.live.
			const std::vector<uintptr_t> live(cr.live.begin(), cr.live.end());
			for (const uintptr_t geo : live)
				if (const auto it = g_geos.find(geo); it != g_geos.end()) unclaim(geo, it->second, cause);
			cr.live.clear();
		}

		// The engine monitor's attached cells, with their roots as the plugin
		// reads them now (indoors the exterior grid is detached: its statics
		// leave the scene with it, and are claimed again on the way out).
		void residency()
		{
			const sg::FaultSite site("residency");
			static std::vector<uintptr_t> now_cells;
			static std::vector<uintptr_t> now_roots;
			static FnvRemixCell s_cells[64];
			now_cells.clear();
			now_roots.clear();
			const uint32_t n = nvse_link::cells(s_cells, static_cast<uint32_t>(std::size(s_cells)));
			for (uint32_t i = 0; i < n; i++)
			{
				now_cells.push_back(s_cells[i].cell);
				now_roots.push_back(s_cells[i].root);
			}
			for (auto it = g_cells.begin(); it != g_cells.end();)
			{
				if (std::find(now_cells.begin(), now_cells.end(), it->first) != now_cells.end()) { ++it; continue; }
				leave_cell(it->second, Cause::Cell);
				it = g_cells.erase(it);
				g_act_cells_out++;
			}
			for (size_t k = 0; k < now_cells.size(); k++)
			{
				const uintptr_t cell = now_cells[k];
				auto [it, fresh] = g_cells.try_emplace(cell);
				if (fresh) g_act_cells_in++;
				CellRec& cr = it->second;
				const uintptr_t root = now_roots[k];
				if (root == cr.root) continue;
				// The cell's 3D was rebuilt (or dropped): whatever hung under
				// the old root is not this cell's any more.
				if (cr.root)
				{
					const std::vector<uintptr_t> live(cr.live.begin(), cr.live.end());
					for (const uintptr_t geo : live)
						if (const auto g = g_geos.find(geo); g != g_geos.end()) unclaim(geo, g->second, Cause::Cell);
				}
				cr.root = root;
			}
		}

		// ---- Resolve: recorded candidate -> owner, mesh, instance ----

		enum class Outcome : uint8_t { Created, Declined, Deferred, OutOfBudget };

		// The recorded window, copied from the recorded (packed) buffers.
		// `colours` gets each vertex's colour as the buffer holds it.
		bool build_mesh_data(const DrawRec& dr, bool vertex_color, std::vector<remixapi_HardcodedVertex>& verts,
			std::vector<uint32_t>& indices, std::vector<uint32_t>& colours, const char*& reject)
		{
			const DrawDesc& d = dr.d;
			const INT first = d.base + static_cast<INT>(d.min_vtx);
			if (first < 0 || !d.num_vtx || !dr.stride) { reject = "empty vertex window"; return false; }
			static std::vector<uint8_t> vbytes;
			if (!copy_vertex_bytes(dr.vb, static_cast<UINT>(first) * dr.stride, d.num_vtx * dr.stride, vbytes, reject))
				return false;
			const UINT count = d.type == D3DPT_TRIANGLELIST ? d.prim_count * 3 : d.prim_count ? d.prim_count + 2 : 0;
			static std::vector<uint32_t> stream;
			if (!count || !copy_index_bytes(dr.ib, d.start_index, count, stream, reject)) return false;

			asset_keys::read_colours(vbytes.data(), dr.stride, d.num_vtx, dr.col, colours);
			verts.resize(d.num_vtx);
			for (UINT i = 0; i < d.num_vtx; i++)
			{
				const uint8_t* p = vbytes.data() + static_cast<size_t>(i) * dr.stride;
				auto& o = verts[i];
				o = {};
				for (int k = 0; k < 3; k++) o.position[k] = read_comp(p + dr.pos.off, dr.pos.type, k);
				if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2]))
				{
					reject = "non-finite POSITION";
					return false;
				}
				float n[3];
				for (int k = 0; k < 3; k++) n[k] = read_comp(p + dr.nrm.off, dr.nrm.type, k);
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (std::isfinite(len) && len > 1e-6f) { o.normal[0] = n[0] / len; o.normal[1] = n[1] / len; o.normal[2] = n[2] / len; }
				else { o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f; }
				o.texcoord[0] = read_comp(p + dr.uv.off, dr.uv.type, 0);
				o.texcoord[1] = read_comp(p + dr.uv.off, dr.uv.type, 1);
				if (dr.hair)
				{
					// The hair PS's factor (material::hair_color).
					const float g = dr.col.type == D3DDECLTYPE_D3DCOLOR ? p[dr.col.off + 1] / 255.0f
						: read_comp(p + dr.col.off, dr.col.type, 1);
					o.color = material::hair_color(g, dr.hair_tint);
				}
				else if (vertex_color)
				{
					// D3DCOLOR's bytes (B, G, R, A) are the runtime's B8G8R8A8.
					if (dr.col.type == D3DDECLTYPE_D3DCOLOR) std::memcpy(&o.color, p + dr.col.off, sizeof(o.color));
					else o.color = D3DCOLOR_ARGB(unorm8(read_comp(p + dr.col.off, dr.col.type, 3)),
						unorm8(read_comp(p + dr.col.off, dr.col.type, 0)), unorm8(read_comp(p + dr.col.off, dr.col.type, 1)),
						unorm8(read_comp(p + dr.col.off, dr.col.type, 2)));
				}
				else o.color = 0xFFFFFFFFu;
			}

			// Indices address (base + index); the window starts at base + min.
			for (uint32_t& v : stream)
			{
				if (v < d.min_vtx || v - d.min_vtx >= d.num_vtx) { reject = "index outside the vertex window"; return false; }
				v -= d.min_vtx;
			}
			indices.clear();
			auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
				if (a == b || b == c || a == c) return;   // degenerate (strip stitching)
				indices.push_back(a);
				indices.push_back(b);
				indices.push_back(c);
			};
			if (d.type == D3DPT_TRIANGLELIST)
				for (size_t i = 0; i + 2 < stream.size(); i += 3) emit(stream[i], stream[i + 1], stream[i + 2]);
			else
				// Odd triangles of a strip are wound the other way round.
				for (size_t i = 0; i + 2 < stream.size(); i++)
				{
					if (i & 1) emit(stream[i + 1], stream[i], stream[i + 2]);
					else       emit(stream[i], stream[i + 1], stream[i + 2]);
				}
			if (indices.empty()) { reject = "no non-degenerate triangles"; return false; }
			return true;
		}

		// A seed's mesh from its NiGeometryData's CPU arrays (positions,
		// normals, UV set 0, NiColorA colours; NiTriShapeData triangles or
		// NiTriStripsData strips), for geometry the engine has not packed yet
		// (packing frees the UV / colour arrays, so only unpacked geometry has
		// them all). Called inside begin_resolve. `colours` gets each vertex's
		// colour as D3DCOLOR.
		bool cpu_mesh_data(const sg::DataRead& d, bool strips, bool vertex_color,
			std::vector<remixapi_HardcodedVertex>& verts, std::vector<uint32_t>& indices, std::vector<uint32_t>& colours,
			const char*& reject)
		{
			const uint32_t n = d.verts;
			if (!n || !d.pos || !d.nrm || !d.uv) { reject = "incomplete CPU arrays"; return false; }
			static std::vector<float> pos, nrm, uv, col;
			pos.resize(n * 3u);
			nrm.resize(n * 3u);
			uv.resize(n * 2u);
			if (!sg::read_block(d.pos, pos.data(), n * 12u) || !sg::read_block(d.nrm, nrm.data(), n * 12u) ||
				!sg::read_block(d.uv, uv.data(), n * 8u))
			{
				reject = "CPU arrays unreadable";
				return false;
			}
			const bool read_col = d.col != 0;
			bool col_read = false;
			if (read_col)
			{
				col.resize(n * 4u);
				col_read = sg::read_block(d.col, col.data(), n * 16u);
				if (!col_read && vertex_color) { reject = "CPU colours unreadable"; return false; }
			}
			const bool have_col = vertex_color && col_read;
			colours.assign(col_read ? n : 0u, 0u);
			verts.resize(n);
			for (uint32_t i = 0; i < n; i++)
			{
				auto& o = verts[i];
				o = {};
				for (uint32_t k = 0; k < 3; k++) o.position[k] = pos[i * 3u + k];
				if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2]))
				{
					reject = "non-finite position";
					return false;
				}
				const float* nv = &nrm[i * 3u];
				const float len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
				if (std::isfinite(len) && len > 1e-6f) { o.normal[0] = nv[0] / len; o.normal[1] = nv[1] / len; o.normal[2] = nv[2] / len; }
				else { o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f; }
				o.texcoord[0] = uv[i * 2u];
				o.texcoord[1] = uv[i * 2u + 1u];
				// NiColorA (r, g, b, a) as the runtime's B8G8R8A8.
				if (col_read)
				{
					const float* c = &col[i * 4u];
					colours[i] = D3DCOLOR_ARGB(unorm8(c[3]), unorm8(c[0]), unorm8(c[1]), unorm8(c[2]));
				}
				o.color = have_col ? colours[i] : 0xFFFFFFFFu;
			}
			indices.clear();
			auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
				if (a == b || b == c || a == c || a >= n || b >= n || c >= n) return;
				indices.push_back(a);
				indices.push_back(b);
				indices.push_back(c);
			};
			static std::vector<uint16_t> idx, lens;
			if (!strips)
			{
				const uint32_t count = static_cast<uint32_t>(d.tris) * 3u;
				if (!count || !d.f48) { reject = "no triangle list"; return false; }
				idx.resize(count);
				if (!sg::read_block(d.f48, idx.data(), count * 2u)) { reject = "triangle list unreadable"; return false; }
				for (uint32_t i = 0; i + 2 < count; i += 3) emit(idx[i], idx[i + 1], idx[i + 2]);
			}
			else
			{
				const uint32_t ns = d.f44 & 0xFFFFu;
				if (!ns || !d.f48 || !d.f4c) { reject = "no strips"; return false; }
				lens.resize(ns);
				if (!sg::read_block(d.f48, lens.data(), ns * 2u)) { reject = "strip lengths unreadable"; return false; }
				uint32_t total = 0;
				for (const uint16_t l : lens) total += l;
				if (!total || total > 3u * 65536u) { reject = "strip lengths out of range"; return false; }
				idx.resize(total);
				if (!sg::read_block(d.f4c, idx.data(), total * 2u)) { reject = "strip indices unreadable"; return false; }
				uint32_t off = 0;
				for (const uint16_t l : lens)
				{
					// Odd triangles of a strip are wound the other way round.
					for (uint32_t j = 0; j + 2 < l; j++)
					{
						const uint32_t a = idx[off + j], b = idx[off + j + 1], c = idx[off + j + 2];
						if (j & 1) emit(b, a, c);
						else emit(a, b, c);
					}
					off += l;
				}
			}
			if (indices.empty()) { reject = "no non-degenerate triangles"; return false; }
			return true;
		}

		// Hash of every byte of the recorded window as the buffers hold it now:
		// the vertex window and the index range build_mesh_data reads. A mesh
		// is keyed and verified by it, so a write lock that refills the same
		// bytes changes nothing.
		bool window_content(const DrawRec& dr, uint64_t& out)
		{
			const DrawDesc& d = dr.d;
			const INT first = d.base + static_cast<INT>(d.min_vtx);
			if (first < 0 || !d.num_vtx || !dr.stride) return false;
			static std::vector<uint8_t> vbytes;
			static std::vector<uint32_t> stream;
			const char* reject = nullptr;
			if (!copy_vertex_bytes(dr.vb, static_cast<UINT>(first) * dr.stride, d.num_vtx * dr.stride, vbytes, reject))
				return false;
			const UINT count = d.type == D3DPT_TRIANGLELIST ? d.prim_count * 3 : d.prim_count ? d.prim_count + 2 : 0;
			if (!count || !copy_index_bytes(dr.ib, d.start_index, count, stream, reject)) return false;
			Hash64 h;
			h.add(vbytes.data(), vbytes.size());
			h.add(stream.data(), stream.size() * sizeof(uint32_t));
			out = h.get();
			return true;
		}

		void bound_of(const std::vector<remixapi_HardcodedVertex>& verts, Mesh& m)
		{
			float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (const auto& v : verts)
				for (int k = 0; k < 3; k++)
				{
					lo[k] = (std::min)(lo[k], v.position[k]);
					hi[k] = (std::max)(hi[k], v.position[k]);
				}
			float r2 = 0.0f;
			for (int k = 0; k < 3; k++)
			{
				m.bound_c[k] = 0.5f * (lo[k] + hi[k]);
				const float h = 0.5f * (hi[k] - lo[k]);
				r2 += h * h;
			}
			m.bound_r = std::sqrt(r2);
		}

		// Ends the handshake on every way out of resolve (a second end_resolve
		// after an explicit one only stores 0 again).
		struct ResolveGuard
		{
			~ResolveGuard() { sg::end_resolve(); }
		};

		// Where a geometry is: the engine monitor's owner (the reference, its
		// pinned 3D root, base type, the attached cell, the flags), and what
		// kind of node is on the chain from the geometry up to that root (tree
		// / billboard, switch / LOD / range: the reference's own subtree
		// decides). Called inside begin_resolve, or while the engine renders
		// the geometry.
		struct Place
		{
			bool known = false;                  // the owner map has it
			FnvRemixOwner o = {};
			bool tree = false, switch_like = false, unreadable = false;
			uintptr_t switch_node = 0;           // the nearest LOD / switch ancestor inside the reference
			uintptr_t switch_branch = 0;         // its child on the chain
			// A switch / LOD ancestor's active child (NiSwitchNode+0xB0) is not
			// the branch on the chain: the engine shows another variant.
			bool switched_off = false;
		};

		// NiSwitchNode (and its NiLODNode subclass) shows only children[+0xB0]
		// (-1: none; OnVisible 0xA93EF0; NiLODNode::OnVisible 0xA99140 writes
		// the index from the camera distance at each cull, so off screen it
		// keeps the level it was last seen at). Unreadable: shown.
		bool switch_shows(uintptr_t node, uintptr_t branch)
		{
			int32_t active = 0;
			uint32_t data = 0;
			uint16_t count = 0;
			if (!sg::read_block(node + kSwitchActive, &active, sizeof(active)) ||
				!sg::read_u32(node + sg::fnv::NODE_childData, data) || !sg::read_u16(node + sg::fnv::NODE_childCount, count))
				return true;
			if (active < 0 || active >= count) return false;
			uint32_t child = 0;
			if (!sg::read_u32(data + 4u * static_cast<uint32_t>(active), child)) return true;
			return child == branch;
		}

		Place place_of(uintptr_t geo)
		{
			const sg::FaultSite site("owner");
			Place p;
			if (!nvse_link::owner_of(geo, p.o)) return p;
			p.known = true;
			uintptr_t node = geo, prev = 0;
			for (int hop = 0; hop < kMaxChainHops && node; hop++)
			{
				uintptr_t parent = 0;
				uint32_t flags = 0;
				if (!sg::read_link(node, parent, flags)) { p.unreadable = true; break; }
				if (hop > 0)
					if (const sg::ClassInfo* ci = sg::class_of(node))
					{
						p.tree = p.tree || ci->subtree_tree;
						if (ci->subtree_exclusive)
						{
							if (!p.switch_node) { p.switch_node = node; p.switch_branch = prev; }
							if (!switch_shows(node, prev)) p.switched_off = true;
						}
						p.switch_like = p.switch_like || ci->subtree_switch;
					}
				if (node == p.o.root) break;
				prev = node;
				node = parent;
			}
			return p;
		}

		bool placed(const Place& p)
		{
			return p.known && !p.unreadable && p.o.cell && g_cells.count(p.o.cell) &&
				!(p.o.flags & (FNVR_OW_DETACHED | FNVR_OW_APP_CULLED | FNVR_OW_APP_CULLED_ABOVE | FNVR_OW_DISABLED));
		}

		Outcome resolve(uintptr_t geo, Rec& r, std::vector<Create>& creates, uint64_t& upload_bytes, uint64_t upload_budget)
		{
			// The handshake: from here to the end of this function a
			// destructor of `geo` on another thread waits for this read.
			if (!sg::begin_resolve(geo)) return Outcome::Declined;   // died: drained next tick
			ResolveGuard guard;
			// The trace, for a geometry known by its base type (a claim before).
			auto tr = [&](const std::string& what) { if (traced(r)) trace(geo, r, what); };

			sg::GeoRead gr;
			if (!sg::read_geo(geo, gr)) { tr("refused nodata (geometry unreadable)"); refuse(r, Reason::NoData); return Outcome::Declined; }
			if (gr.skin) { tr("refused skinned"); refuse(r, Reason::Skinned); return Outcome::Declined; }
			{
				// The alpha property as it is now (discovery saw it once).
				uint16_t af = 0;
				const bool have_af = read_alpha(gr, af);
				AlphaKind ak = AlphaKind::Opaque;
				const Reason ar = alpha_class(have_af, af, ak);
				if (ar != Reason::Count) { tr(std::format("refused {} (alpha property)", kReasonNames[static_cast<size_t>(ar)])); refuse(r, ar); return Outcome::Declined; }
				if (is_blend(ak) != is_blend(r.alpha))
				{
					// It started or stopped blending since the record: the
					// recorded pass's alpha state is stale; record it again.
					tr("deferred: its alpha blending changed since the record");
					r.alpha = ak;
					r.draw.frame = kNoFrame;
					return Outcome::Deferred;
				}
			}

			// Scope: a non-actor reference of an allowed base type, in an
			// attached cell, not under a tree / billboard node. Geometry an
			// actor owns moves with its skinned body: draw capture keeps it,
			// drawn per frame with the body.
			g_c.resolves++;
			const Place pl = place_of(geo);
			if (pl.known && (pl.o.flags & FNVR_OW_ACTOR)) { tr("refused actor"); refuse(r, Reason::Actor); return Outcome::Declined; }
			if (pl.known && pl.tree) { refuse(r, Reason::Subtree); return Outcome::Declined; }
			if (!pl.known) g_c.owner_unknown++;
			else if (pl.unreadable) g_c.owner_unreadable++;
			else if (pl.o.flags & FNVR_OW_DETACHED) g_c.owner_detached++;
			else if (pl.o.flags & (FNVR_OW_APP_CULLED | FNVR_OW_APP_CULLED_ABOVE)) g_c.owner_culled++;
			else if (pl.o.flags & FNVR_OW_DISABLED) g_c.owner_disabled++;
			else if (!pl.o.cell || !g_cells.count(pl.o.cell)) g_c.owner_no_cell++;
			else g_c.owner_hit++;
			if (!placed(pl))
			{
				// Not placed yet (its reference's 3D event not applied yet, a
				// chain detached for a moment, a cell still attaching): later.
				tr(std::format("resolve: no place yet (owner {} ref 0x{:X} cell 0x{:X} flags 0x{:X} unreadable {} tries {})",
					pl.known ? 1 : 0, pl.o.ref, pl.o.cell, pl.o.flags, pl.unreadable ? 1 : 0, r.tries));
				if (++r.tries >= kMaxAnchorTries)
				{
					refuse_soft(r, !pl.known ? Reason::NoRef : !pl.o.cell || !g_cells.count(pl.o.cell) ? Reason::NoAnchor
						: Reason::Culled);
					return Outcome::Declined;
				}
				defer(r, Defer::Anchor);
				return Outcome::Deferred;
			}
			// Known by its base type from here on (status line, trace).
			r.base_type = pl.o.base_type;
			r.ref_id = pl.o.ref_id;
			if (!g_base_ok[pl.o.base_type]) { tr("refused baseType"); refuse(r, Reason::BaseType); return Outcome::Declined; }

			// The packed buffers: the geometry data's NiGeometryBufferData must
			// name the VB / IB the recorded pass bound: the mesh comes from
			// exactly the engine's packed copy of this geometry.
			// A seed of unpacked geometry reads its CPU arrays (still unpacked).
			const bool cpu = r.draw.cpu_data != 0;
			sg::DataRead d;
			sg::BufferRead b;
			identity::BufferInfo vbi, ibi;
			bool relaxed = false;
			if (cpu)
			{
				if (!gr.data || gr.data != r.draw.cpu_data || !sg::read_data(gr.data, d))
				{
					tr("refused buffer: its geometry data changed since the seed");
					refuse(r, Reason::Buffer);
					return Outcome::Declined;
				}
				if (d.packed)
				{
					// Packed since the seed (the UV / colour arrays may be gone):
					// seeded again from the packed buffers.
					tr("deferred: packed since its seed");
					r.draw.frame = kNoFrame;
					return Outcome::Deferred;
				}
			}
			else
			{
				if (!gr.data || !sg::read_data(gr.data, d) || !d.packed || !sg::read_buffer(d.packed, b))
				{
					tr("refused buffer: no packed buffer data");
					refuse(r, Reason::Buffer);
					return Outcome::Declined;
				}
				if (b.vb[0] != r.draw.vb || b.ib != r.draw.ib || (b.stride[0] && b.stride[0] != r.draw.stride))
				{
					g_c.buffer_mismatch++;
					tr(std::format("refused buffer: the packed buffers are not the recorded pass's (vb 0x{:X} vs 0x{:X}, ib 0x{:X} vs 0x{:X})",
						reinterpret_cast<uintptr_t>(b.vb[0]), reinterpret_cast<uintptr_t>(r.draw.vb),
						reinterpret_cast<uintptr_t>(b.ib), reinterpret_cast<uintptr_t>(r.draw.ib)));
					refuse(r, Reason::Buffer);
					return Outcome::Declined;
				}
				// Informational: the buffer data's window against the recorded draw.
				if (b.base_vertex != static_cast<uint32_t>(r.draw.d.base) || b.verts != r.draw.d.num_vtx ||
					b.tris != r.draw.d.prim_count)
					g_c.window_mismatch++;
				// The buffers as they are now (the draw checked them too).
				vbi = buffer_info(r.draw.vb);
				ibi = buffer_info(r.draw.ib);
				bool fresh = false;
				if (const Reason sr = stream_reason(vbi, ibi, fresh, relaxed); sr != Reason::Count)
				{
					tr(std::format("refused {} (vb {} | ib {})", kReasonNames[static_cast<size_t>(sr)], describe_buffer(r.draw.vb), describe_buffer(r.draw.ib)));
					refuse_stream(r, sr);
					return Outcome::Declined;
				}
				if (fresh) { tr("deferred fresh: a buffer was streamed into moments ago"); defer(r, Defer::Fresh); return Outcome::Deferred; }
			}

			// Material (built at the draw).
			const auto mit = g_mats.find(r.draw.mat_key);
			if (mit == g_mats.end() || !mit->second.handle)
			{
				tr("deferred: its material was evicted meanwhile, recorded again at the next draw");
				r.draw.frame = kNoFrame;   // evicted meanwhile: recorded again at the next draw
				return Outcome::Deferred;
			}
			Mat& mt = mit->second;
			const bool vertex_color = r.draw.hair || (r.draw.col.ok() && !mt.no_vertex_color);
			// A two-sided decal: its mesh carries a reversed copy of every
			// triangle (material::append_reversed).
			const bool reversed_copy = material::is_decal(mt.categories | r.draw.decal.categories) && r.draw.double_sided;

			// Transform: the live world pose, absolute.
			remixapi_Transform xf = {};
			if (!sg::read_pose(geo, r.pose) || !pose_transform(r.pose, xf)) { tr("refused nodata (pose unreadable or degenerate)"); refuse(r, Reason::NoData); return Outcome::Declined; }

			// Mesh, shared by every geometry with the same data, buffers,
			// window, bytes and material (clones of one NIF). The bytes (not
			// the write generations) tell a recycled buffer address or a
			// refill from the old contents: a refill with the same bytes
			// shares the mesh it had.
			uint64_t content = 0;
			static std::vector<remixapi_HardcodedVertex> verts;
			static std::vector<uint32_t> indices;
			static std::vector<uint32_t> colours;
			if (cpu)
			{
				const char* reject = nullptr;
				if (!cpu_mesh_data(d, r.draw.cpu_strips, vertex_color, verts, indices, colours, reject))
				{
					tr(std::format("refused build ({})", reject ? reject : "?"));
					refuse(r, Reason::Build);
					return Outcome::Declined;
				}
				Hash64 ch;
				ch.add(verts.data(), verts.size() * sizeof(remixapi_HardcodedVertex));
				ch.add(indices.data(), indices.size() * sizeof(uint32_t));
				content = ch.get();
			}
			else if (!window_content(r.draw, content))
			{
				tr("refused build (the recorded window could not be read)");
				refuse(r, Reason::Build);
				return Outcome::Declined;
			}
			Hash64 kh;
			kh.mix(gr.data);
			kh.mix(d.id);
			kh.mix(d.verts);
			kh.ptr(r.draw.vb);
			kh.ptr(r.draw.ib);
			kh.mix(content);
			kh.mix(r.draw.stride);
			kh.mix(static_cast<uint64_t>(r.draw.d.type) | (static_cast<uint64_t>(r.draw.d.prim_count) << 8));
			kh.mix(static_cast<uint64_t>(static_cast<uint32_t>(r.draw.d.base)) | (static_cast<uint64_t>(r.draw.d.min_vtx) << 32));
			kh.mix(static_cast<uint64_t>(r.draw.d.num_vtx) | (static_cast<uint64_t>(r.draw.d.start_index) << 32));
			for (const Attr* at : { &r.draw.pos, &r.draw.nrm, &r.draw.uv, &r.draw.col })
				kh.mix(static_cast<uint64_t>(static_cast<uint32_t>(at->off)) | (static_cast<uint64_t>(static_cast<uint32_t>(at->type)) << 32));
			kh.mix(r.draw.mat_key);
			kh.mix(vertex_color ? 1u : 0u);
			kh.mix(reversed_copy ? 0x5245564552534544ull : 0ull);   // "REVERSED"
			if (r.draw.hair)
				for (const float t : r.draw.hair_tint) kh.mix(static_cast<uint64_t>(unorm8(t)) | 0x48414952ull << 8);
			const uint64_t key = kh.get();
			auto msh = g_meshes.find(key);
			if (msh == g_meshes.end())
			{
				const uint64_t bytes = static_cast<uint64_t>(r.draw.d.num_vtx) * sizeof(remixapi_HardcodedVertex) +
					static_cast<uint64_t>(r.draw.d.prim_count + 2) * 3ull * sizeof(uint32_t);
				if (upload_bytes && upload_bytes + bytes > upload_budget) return Outcome::OutOfBudget;
				if (!take_build_budget(key, r.draw.d.num_vtx)) return Outcome::OutOfBudget;
				const char* reject = nullptr;
				if (!cpu && !build_mesh_data(r.draw, vertex_color, verts, indices, colours, reject))
				{
					if (g_c.meshes_built == 0 && reject)
						shared::common::log("SceneStatics", std::format("mesh build failed ({}): geo=0x{:X} {} verts, {} prims",
							reject, geo, r.draw.d.num_vtx, r.draw.d.prim_count), shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
					tr(std::format("refused build ({})", reject ? reject : "?"));
					refuse(r, Reason::Build);
					return Outcome::Declined;
				}
				// Everything of the geometry is copied: the handshake ends
				// before the bridge call, which can block on a full channel
				// (a destructor waiting on this read must not wait for that).
				sg::end_resolve();
				Mesh m;
				bound_of(verts, m);
				// The asset key names the geometry before the reversed copy.
				const uint64_t asset_key = asset_keys::for_static(asset_keys::of(verts, indices), mt.albedo_d3d9, indices,
					[&](std::vector<uint32_t>& out) { out = colours; });
				m.facts = material::mesh_facts(verts, indices, vertex_color);
				if (reversed_copy)
				{
					material::append_reversed(indices);
					m.facts.reversed_copy = true;
				}
				m.handle = create_mesh(next_object_hash(key, kSceneDomain), verts, indices, mt.handle, asset_key);
				if (!m.handle) { tr("deferred mesh: CreateMesh failed"); defer(r, Defer::Mesh); return Outcome::Deferred; }
				material::log_mesh_addressing("scene", geo, mt.albedo_d3d9, mt.wrap_u, mt.wrap_v, verts);
				// Copied at these generations. A write since (a loader thread)
				// may have mixed old and new contents into the copy: the mesh
				// is kept (its key names the old generation, so nothing shares
				// it; evicted unused) and the geometry is recorded again.
				const bool vb_same = cpu || identity::note_built(r.draw.vb, vbi.gen);
				const bool ib_same = cpu || identity::note_built(r.draw.ib, ibi.gen);
				m.material_key = r.draw.mat_key;
				m.content = content;
				m.vertex_color = vertex_color;
				m.blend = mt.blend;
				m.alpha_func = mt.alpha_func;
				m.alpha_ref = mt.alpha_ref;
				m.last_used = frame();
				mt.refs++;
				mt.last_used = frame();
				msh = g_meshes.emplace(key, m).first;
				upload_bytes += verts.size() * sizeof(remixapi_HardcodedVertex) + indices.size() * sizeof(uint32_t);
				g_c.meshes_built++;
				if (!vb_same || !ib_same) { tr("deferred mesh: a buffer was written during the copy"); defer(r, Defer::Mesh); return Outcome::Deferred; }
			}
			else
			{
				g_c.meshes_shared++;
				// The same bytes, maybe refilled since that mesh was built (the
				// refill cleared the buffer's built mark): marked again at the
				// generation the bytes were read at, so a later write is
				// checked (check_written). Written meanwhile: recorded again.
				if (!cpu && (!identity::note_built(r.draw.vb, vbi.gen) || !identity::note_built(r.draw.ib, ibi.gen)))
				{
					tr("deferred mesh: a buffer was written while its bytes were compared");
					defer(r, Defer::Mesh);
					return Outcome::Deferred;
				}
			}
			Mesh& mesh = msh->second;

			r.mesh_key = key;
			r.vb_gen = vbi.gen;
			r.ib_gen = ibi.gen;
			r.verify = false;
			if (relaxed) g_c.relaxed_claims++;
			r.cell = pl.o.cell;
			r.ref = pl.o.ref;
			r.ref_root = pl.o.root;
			r.switch_leaf = pl.switch_like;
			r.switch_node = pl.switch_node;
			r.switch_branch = pl.switch_branch;
			r.xform = xf;
			r.visible = true;
			r.tries = 0;
			mesh.refs++;
			mesh.last_used = frame();

			// Categories: the rtx.conf lists the material mapped, and the decal
			// rules of the recorded pass (material::decal_of; SceneDecalCategory).
			r.categories = mt.categories | r.draw.decal.categories;
			r.claim = r.draw.claim;
			if (!mt.blend) r.claim &= static_cast<uint32_t>(~(kClaimBlendTest | kClaimBlend));   // ignoreAlphaOnTextures
			if (!mt.blend && !mt.alpha_func) r.claim &= static_cast<uint32_t>(~kClaimTest);
			if (mt.categories) r.claim |= kClaimRtxCat;
			if (r.categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC))
				r.claim |= kClaimDecal;
			if (r.land_shape) r.claim |= kClaimLandShape;
			if (r.base_type == 0x16 || r.base_type == 0x17 || r.base_type == 0x26) r.claim |= kClaimNewBase;

			Create c;
			c.geo = geo;
			c.info = {};
			c.info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
			c.info.pNext = nullptr;
			c.info.categoryFlags = r.categories;
			c.info.mesh = mesh.handle;
			c.info.transform = xf;
			c.info.doubleSided = r.draw.double_sided ? 1 : 0;
			bool vertex_alpha_dropped = false;
			c.has_ext = instance_ext(mesh, r, c.ext, &vertex_alpha_dropped);
			if (material::is_decal(r.categories))
			{
				// What the decal goes out as ([DecalRoute], once per geometry and mesh).
				material::DecalRoute dr;
				dr.path = "scene";
				dr.object = geo;
				dr.albedo_d3d9 = mt.albedo_d3d9;
				dr.mesh_key = key;
				dr.categories = r.categories;
				dr.instance = "retained";
				dr.draw_alpha = mt.blend;
				dr.alpha_func = mt.alpha_func;
				dr.alpha_ref = mt.alpha_ref;
				dr.ext = c.has_ext ? &c.ext : nullptr;
				dr.vertex_alpha_dropped = vertex_alpha_dropped;
				dr.double_sided = r.draw.double_sided;
				dr.mesh = mesh.facts;
				material::log_decal_route(dr);
			}
			creates.push_back(c);
			return Outcome::Created;
		}

		void note_claimed(uintptr_t geo, Rec& r)
		{
			r.state = State::Live;
			r.dirty = false;
			g_claimed++;
			for (const void* p : { static_cast<const void*>(r.draw.vb), static_cast<const void*>(r.draw.ib) })
				if (p) g_buffer_claims[p].push_back(geo);
			if (auto c = g_cells.find(r.cell); c != g_cells.end()) c->second.live.insert(geo);
			g_live_list_dirty = true;
			g_c.created++;
			g_act_created++;
			if (g_dumped < static_cast<uint32_t>(cfg().scene_debug_dump))
			{
				g_dumped++;
				const auto& t = r.xform.matrix;
				shared::common::log("SceneStatics", std::format(
					"instance #{} geo=0x{:X} ref {:08X} (base type 0x{:02X}) cell 0x{:X} mesh=0x{:016X} handle={} "
					"abs=({:.1f}, {:.1f}, {:.1f}) row0=({:.3f}, {:.3f}, {:.3f}) ds={} switchLeaf={} window {}v/{}p",
					g_dumped, geo, r.ref_id, r.base_type, r.cell, r.mesh_key, r.handle,
					t[0][3], t[1][3], t[2][3], t[0][0], t[0][1], t[0][2], r.draw.double_sided ? 1 : 0, r.switch_leaf ? 1 : 0,
					r.draw.d.num_vtx, r.draw.d.prim_count), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			}
		}

		// One CreateRetainedInstances for the batch; each created geometry is
		// claimed now, so its passes are skipped from the next frame on (the
		// bridge is a FIFO: the create is ahead of that frame's replay).
		void submit(std::vector<Create>& creates)
		{
			if (creates.empty()) return;
			if (g_draw_instance)
			{
				// Drawn per frame from the first render pass on.
				for (const Create& c : creates)
					if (const auto it = g_geos.find(c.geo); it != g_geos.end())
					{
						it->second.handle = 0;
						it->second.seed = Seed::Claimed;
						note_claimed(c.geo, it->second);
					}
				return;
			}
			static std::vector<remixapi_InstanceInfo> infos;
			static std::vector<remixapi_InstanceInfoBlendEXT> blends;
			static std::vector<uint64_t> handles;
			infos.resize(creates.size());
			blends.resize(creates.size());
			handles.assign(creates.size(), 0);
			for (size_t i = 0; i < creates.size(); i++)
			{
				infos[i] = creates[i].info;
				blends[i] = creates[i].ext;
				infos[i].pNext = creates[i].has_ext ? &blends[i] : nullptr;
			}
			retained::create_external(infos.data(), static_cast<uint32_t>(infos.size()), handles.data());
			for (size_t i = 0; i < creates.size(); i++)
			{
				const auto it = g_geos.find(creates[i].geo);
				if (it == g_geos.end()) continue;
				Rec& r = it->second;
				if (!handles[i])
				{
					// Nothing reached the runtime: the geometry keeps D3D9.
					if (traced(r)) trace(creates[i].geo, r, "create failed: no handle came back, deferred (D3D9 keeps drawing it)");
					release_mesh(r.mesh_key);
					r.mesh_key = 0;
					defer(r, Defer::Create);
					g_c.create_failed++;
					continue;
				}
				r.handle = handles[i];
				g_live++;
				r.provisional = r.draw.from_graph;
				r.confirm_frames = 0;
				r.seed = Seed::Claimed;
				if (!r.provisional) g_seed_desc.erase(creates[i].geo);
				note_claimed(creates[i].geo, r);
				if (traced(r))
				{
					const auto& t = r.xform.matrix;
					trace(creates[i].geo, r, std::format("claimed: mesh=0x{:016X} cell=0x{:X} ref=0x{:X} refRoot=0x{:X} "
						"switchNode=0x{:X} branch=0x{:X} switchLeaf={} claim=0x{:X} cat=0x{:X} ds={} ext={} pos=({:.1f}, {:.1f}, {:.1f}) "
						"window {}v/{}p", r.mesh_key, r.cell, r.ref, r.ref_root, r.switch_node, r.switch_branch,
						r.switch_leaf ? 1 : 0, r.claim, r.categories, r.draw.double_sided ? 1 : 0, creates[i].has_ext ? 1 : 0,
						t[0][3], t[1][3], t[2][3], r.draw.d.num_vtx, r.draw.d.prim_count));
				}
			}
		}

		void resolve_queue()
		{
			if (g_queue.empty()) return;
			const sg::FaultSite site("resolve");
			const UINT now = frame();
			const uint64_t budget = static_cast<uint64_t>(cfg().scene_upload_kib_per_frame) * 1024ull;
			uint64_t upload = 0;
			std::vector<Create> creates;
			size_t i = 0;
			for (; i < g_queue.size() && creates.size() < kCreateBatch; i++)
			{
				const uintptr_t geo = g_queue[i];
				const auto it = g_geos.find(geo);
				if (it == g_geos.end()) continue;   // died
				Rec& r = it->second;
				if (r.state != State::Candidate || r.draw.frame == kNoFrame) { r.queued = false; continue; }
				// Sighted (and recorded) recently, so the record describes the
				// geometry as it is; an older one is recorded again next time.
				// A seed record was read from the scene graph itself (its
				// buffers and place are checked again below).
				if (!r.draw.from_graph && (now - r.last_seen > kFreshFrames || now - r.draw.frame > kFreshFrames))
				{
					if (traced(r))
						trace(geo, r, std::format("record too old (seen {}f ago, recorded {}f ago): recorded again at its next draw",
							now - r.last_seen, now - r.draw.frame));
					r.queued = false;
					r.draw.frame = kNoFrame;
					g_c.stale_records++;
					continue;
				}
				const Outcome o = resolve(geo, r, creates, upload, budget);
				if (o == Outcome::OutOfBudget) break;
				// A member's record put off (anchor, mesh, fresh buffer): seeded
				// again when the deferral runs out, not left for a draw.
				if (o == Outcome::Deferred && r.member && r.draw.frame == kNoFrame)
					hold_seed(geo, r, Seed::Budget, r.not_before > now ? r.not_before - now : 1u);
				r.queued = false;
			}
			g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<ptrdiff_t>(i));
			g_c.upload_bytes += upload;
			submit(creates);
		}

		void recreate_all()
		{
			// SceneDebugDrawInstance went off: every claimed geometry needs its
			// retained instance again (its passes reach D3D9 until it has one).
			g_recreate = false;
			std::vector<Create> creates;
			for (auto& [geo, r] : g_geos)
			{
				if (r.state != State::Live || r.handle) continue;
				const auto mit = g_meshes.find(r.mesh_key);
				if (mit == g_meshes.end()) continue;
				Create c;
				c.geo = geo;
				c.info = {};
				c.info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
				c.info.mesh = mit->second.handle;
				c.info.transform = r.xform;
				c.info.doubleSided = r.draw.double_sided ? 1 : 0;
				c.info.categoryFlags = r.categories;
				c.has_ext = instance_ext(mit->second, r, c.ext);
				creates.push_back(c);
			}
			static std::vector<remixapi_InstanceInfo> infos;
			static std::vector<remixapi_InstanceInfoBlendEXT> blends;
			static std::vector<uint64_t> handles;
			for (size_t i = 0; i < creates.size(); i += kCreateBatch)
			{
				const size_t n = (std::min)(creates.size() - i, static_cast<size_t>(kCreateBatch));
				infos.resize(n);
				blends.resize(n);
				handles.assign(n, 0);
				for (size_t k = 0; k < n; k++)
				{
					infos[k] = creates[i + k].info;
					blends[k] = creates[i + k].ext;
					infos[k].pNext = creates[i + k].has_ext ? &blends[k] : nullptr;
				}
				retained::create_external(infos.data(), static_cast<uint32_t>(n), handles.data());
				for (size_t k = 0; k < n; k++)
				{
					const auto it = g_geos.find(creates[i + k].geo);
					if (it == g_geos.end() || !handles[k]) continue;
					Rec& r = it->second;
					r.handle = handles[k];
					g_live++;
					if (!r.visible)
					{
						// Created visible: hide it again.
						r.visible = true;
						set_visible(r, false);
						g_c.hidden--;   // not a new hide
					}
				}
			}
		}

		// ---- Movers and hides ----

		void movers()
		{
			for (const uintptr_t geo : g_dirty)
			{
				const auto it = g_geos.find(geo);
				if (it == g_geos.end()) continue;
				Rec& r = it->second;
				r.dirty = false;
				const bool unseen = std::exchange(r.pose_unseen, false);
				if (r.state != State::Live) continue;
				remixapi_Transform xf = {};
				if (!pose_transform(r.pose, xf)) continue;
				g_c.pose_changes++;
				if (unseen) g_c.pose_unseen++;
				if (!moved(xf, r.xform))
				{
					g_c.pose_absorbed++;
					continue;
				}
				const PoseDelta pd = pose_delta(xf, r.xform);
				if (pd.dt < kSmallMove && pd.dl < kSmallLinear) g_c.moved_small++;
				{
					MoveStat& ms = g_moves[geo];
					ms.moves++;
					ms.max_dt = (std::max)(ms.max_dt, pd.dt);
					ms.max_dl = (std::max)(ms.max_dl, pd.dl);
				}
				r.xform = xf;
				g_c.moved++;
				if (!r.mover)
				{
					r.mover = true;
					g_movers.insert(geo);
				}
				if (traced(r))
				{
					r.trace_moves++;
					// Also logged: a move whose source (render pass / tick poll) is
					// not the last one's, i.e. when the engine stops and resumes
					// rendering a mover. The reference's own position (Havok's)
					// goes next to the geometry's.
					const bool switched = unseen != r.trace_unseen;
					r.trace_unseen = unseen;
					if (r.trace_moves <= kTraceMovesFull || r.trace_moves % kTraceMoveEvery == 0 || switched)
					{
						float rp[3] = {};
						const bool have_rp = r.ref && sg::read_block(r.ref + sg::fnv::REFR_pos, rp, sizeof(rp));
						trace(geo, r, std::format("moved #{}: dt={:.3f} dl={:.4f} pos=({:.1f}, {:.1f}, {:.1f}) refPos={} {}{}",
							r.trace_moves, pd.dt, pd.dl, xf.matrix[0][3], xf.matrix[1][3], xf.matrix[2][3],
							have_rp ? std::format("({:.1f}, {:.1f}, {:.1f})", rp[0], rp[1], rp[2]) : std::string("?"),
							unseen ? "read at the tick (the engine did not render it this frame)" : "read at its render pass",
							r.handle ? "" : " (no handle: nothing sent)"));
					}
				}
				if (!r.handle) continue;
				remixapi_RetainedInstanceUpdate u = {};
				u.handle = r.handle;
				u.flags = REMIXAPI_RETAINED_UPDATE_TRANSFORM;
				u.transform = xf;
				retained::update_external(u);
			}
			g_dirty.clear();
		}

		// Engine state that removes a claimed geometry from the world while it
		// lives, read from engine data only (never from whether the engine drew
		// it): an app-culled node in its reference's 3D (scripts, NiVisController,
		// BSRangeNode stages, light on / off nodes) or above it (the cell's
		// nodes: an exterior buffered behind an interior, the console's cell
		// toggles), a chain that no longer reaches its reference's root, its
		// reference disabled / deleted or given another 3D, a switch / LOD
		// ancestor showing another child. The engine's per-frame culling
		// (frustum, BSPortalGraph rooms, BSFadeNode fade) sets none of these
		// outside its own cull pass. Called inside begin_resolve.
		enum class Hide : uint8_t { AppCulled, CellCulled, Detached, Disabled, RootSwap, SwitchOff, Count };
		constexpr const char* kHideNames[] = { "appCulled", "cellCulled", "detached", "disabled", "rootSwap", "switchOff" };

		bool engine_visible(uintptr_t geo, const Rec& r, UINT, Hide& why)
		{
			if (!nvse_link::has(FNVR_F_OWNERS) || !nvse_link::live()) return r.visible;   // no owner answer here: hold
			FnvRemixOwner o;
			if (!nvse_link::owner_of(geo, o)) { why = Hide::Detached; return false; }
			if (o.ref != r.ref || o.root != r.ref_root) { why = Hide::RootSwap; return false; }
			if (o.flags & FNVR_OW_APP_CULLED) { why = Hide::AppCulled; return false; }
			if (o.flags & FNVR_OW_APP_CULLED_ABOVE) { why = Hide::CellCulled; return false; }
			if (o.flags & FNVR_OW_DETACHED) { why = Hide::Detached; return false; }
			if (o.flags & FNVR_OW_DISABLED) { why = Hide::Disabled; return false; }
			if (r.switch_node)
			{
				// Every switch / LOD ancestor up to the reference's root.
				uintptr_t node = geo, prev = 0;
				for (int hop = 0; hop < kMaxChainHops && node; hop++)
				{
					uintptr_t parent = 0;
					uint32_t flags = 0;
					if (!sg::read_link(node, parent, flags)) break;
					if (hop > 0)
						if (const sg::ClassInfo* ci = sg::class_of(node); ci && ci->subtree_exclusive && !switch_shows(node, prev))
						{
							why = Hide::SwitchOff;
							return false;
						}
					if (node == r.ref_root) break;
					prev = node;
					node = parent;
				}
			}
			return true;
		}

		// Called inside begin_resolve, for a claimed geometry the engine didn't
		// render this frame and whose engine state keeps it in the world: its
		// pose as the scene graph has it now, queued as a move when it changed.
		// Havok moves a physics object (dust bunnies, knocked clutter) whether
		// or not the engine renders it; the instance follows it while culled.
		void sample_unseen(uintptr_t geo, Rec& r)
		{
			float p[sg::kPoseFloats];
			if (r.dirty || !sg::read_pose(geo, p) || std::memcmp(p, r.pose, sizeof(p)) == 0) return;
			std::memcpy(r.pose, p, sizeof(p));
			r.pose_unseen = true;
			r.dirty = true;
			g_dirty.push_back(geo);
		}

		// Every tick, every mover the engine didn't render this frame (its
		// render passes sampled the rest): the pose the instance must have.
		void poll_movers()
		{
			if (g_movers.empty()) return;
			const sg::FaultSite site("moverPoll");
			const UINT now = frame();
			for (const uintptr_t geo : g_movers)
			{
				const auto it = g_geos.find(geo);
				if (it == g_geos.end()) continue;
				Rec& r = it->second;
				if (r.state != State::Live || r.last_seen == now || r.dirty) continue;
				if (!sg::begin_resolve(geo)) continue;   // died: drained next tick
				Hide why = Hide::Count;
				if (engine_visible(geo, r, now, why)) sample_unseen(geo, r);
				sg::end_resolve();
			}
		}

		// Called inside begin_resolve, for a claimed geometry whose material
		// took the engine's clamp mode: the shade property's mode now (any
		// value, `mode_now`), and whether its wrap pair differs from the claimed
		// material's. An unreadable property, another class or a mode above 3
		// holds the claim.
		constexpr uint32_t kClampChangeLogs = 20;

		bool clamp_mode_changed(uintptr_t geo, const Rec& r, uint32_t& mode_now)
		{
			if (r.draw.clamp_mode == kNoClampMode8) return false;
			const auto mit = g_mats.find(r.draw.mat_key);
			if (mit == g_mats.end()) return false;
			material::Desc probe;
			const bool engine = material::clamp_from_geometry(geo, probe);
			mode_now = probe.clamp_raw;
			return engine && (probe.wrap_u != mit->second.wrap_u || probe.wrap_v != mit->second.wrap_v);
		}

		// A claimed geometry's diffuse as the engine has it: its shade
		// property (PPL or Lighting30 only), the property's role-0 texture and
		// that texture's D3D texture (0 while it has none). The script command
		// SwapTexture changes these on the same geometry, whose claimed passes
		// the engine no longer draws.
		// A second change within this many frames is an animated texture
		// (wider than one round of the hide poll in a large exterior).
		constexpr UINT kTexChangeWindow = kChangeWindow;
		constexpr uint32_t kTexChangeLogs = 20;

		struct EngineDiffuse
		{
			uintptr_t shade = 0, tex = 0, d3d = 0;
			bool operator==(const EngineDiffuse&) const = default;
		};

		bool engine_diffuse(uintptr_t geo, EngineDiffuse& out)
		{
			sg::GeoRead gr;
			if (!sg::read_geo(geo, gr)) return false;
			const uintptr_t shade = gr.props[sg::fnv::kPropShade];
			const sg::ClassInfo* ci = shade ? sg::class_of(shade) : nullptr;
			if (!ci || !ci->shade_ppl) return false;
			const uintptr_t tex = sg::ppl_texture(shade, 0);
			if (!tex) return false;
			bool has_rd = false;
			out.shade = shade;
			out.tex = tex;
			out.d3d = reinterpret_cast<uintptr_t>(sg::d3d_texture(tex, has_rd));
			return true;
		}

		// Called inside begin_resolve: whether the engine's diffuse differs
		// from the one recorded at the claimed pass. An unreadable property
		// holds the claim.
		bool diffuse_changed(uintptr_t geo, const Rec& r, EngineDiffuse& now_d)
		{
			if (!r.draw.diffuse_known) return false;
			if (!engine_diffuse(geo, now_d)) return false;
			const EngineDiffuse was{ r.draw.diffuse_shade, r.draw.diffuse_tex, r.draw.diffuse_d3d };
			return !(now_d == was);
		}

		void poll_hides()
		{
			const UINT now = frame();
			// Drawn by the engine this frame: in the world, whatever else says.
			for (const uintptr_t geo : g_seen_hidden)
				if (const auto it = g_geos.find(geo); it != g_geos.end() && it->second.state == State::Live)
				{
					if (!it->second.visible && traced(it->second)) trace(geo, it->second, "shown: the engine rendered it this frame");
					set_visible(it->second, true);
				}
			g_seen_hidden.clear();

			if (g_live_list_dirty)
			{
				g_live_list.clear();
				for (const auto& [geo, r] : g_geos)
					if (r.state == State::Live) g_live_list.push_back(geo);
				g_live_list_dirty = false;
			}
			if (g_live_list.empty()) return;
			const sg::FaultSite site("hidePoll");
			const size_t n =(std::min)(g_live_list.size(), static_cast<size_t>(kPollPerTick));
			for (size_t k = 0; k < n; k++)
			{
				const uintptr_t geo = g_live_list[g_poll_i++ % g_live_list.size()];
				const auto it = g_geos.find(geo);
				if (it == g_geos.end() || it->second.state != State::Live) continue;
				Rec& r = it->second;
				// Drawn this frame: visible, but still gets the clamp-mode
				// re-check.
				const bool poll = r.last_seen != now;
				if (!poll && r.draw.clamp_mode == kNoClampMode8 && !r.draw.diffuse_known) continue;
				if (!sg::begin_resolve(geo)) continue;   // died: drained next tick
				uint32_t clamp_now = material::kNoClampMode;
				const bool clamp_changed = clamp_mode_changed(geo, r, clamp_now);
				EngineDiffuse tex_now;
				const bool tex_changed = !clamp_changed && diffuse_changed(geo, r, tex_now);
				char tex_name[128] = {};
				if (tex_changed) sg::texture_name(tex_now.tex, tex_name, sizeof(tex_name));
				Hide why = Hide::Count;
				bool vis = r.visible;
				if (poll && !clamp_changed && !tex_changed)
				{
					vis = engine_visible(geo, r, now, why);
					// Pushed while the engine didn't render it (clutter knocked
					// over off screen): its first move is caught here, then it
					// is a mover.
					if (vis) sample_unseen(geo, r);
				}
				sg::end_resolve();
				if (clamp_changed)
				{
					// The material's addressing no longer matches the engine's:
					// unclaimed, so its next base pass records and builds it
					// with the new mode.
					static uint32_t s_clamp_logged = 0;
					if (s_clamp_logged < kClampChangeLogs)
					{
						s_clamp_logged++;
						shared::common::log("Addressing", std::format("clamp changed #{} geo=0x{:X} base 0x{:02X} ref {:08X}: "
							"mode {} -> {} -- unclaimed, rebuilt at its next draw{}", s_clamp_logged, geo, r.base_type, r.ref_id,
							r.draw.clamp_mode, clamp_now, s_clamp_logged == kClampChangeLogs ? " (last clamp-change line)" : ""),
							shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
					}
					if (traced(r)) trace(geo, r, std::format("clamp mode {} -> {}: unclaimed", r.draw.clamp_mode, clamp_now));
					unclaim(geo, r, Cause::Other);
					continue;
				}
				if (tex_changed)
				{
					// The engine's diffuse is no longer the claimed material's
					// (SwapTexture): unclaimed, so its next base pass records and
					// builds it with the new texture. After a second change within
					// kTexChangeWindow it stays on per-frame draws for a while.
					const bool repeat = r.tex_change_frame != kNoFrame && now - r.tex_change_frame <= kTexChangeWindow;
					static uint32_t s_tex_logged = 0;
					if (s_tex_logged < kTexChangeLogs)
					{
						s_tex_logged++;
						shared::common::log("SceneStatics", std::format("diffuse changed #{} geo=0x{:X} base 0x{:02X} ref {:08X}: "
							"texture 0x{:X} -> 0x{:X} '{}' (shade 0x{:X} -> 0x{:X}) -- {}{}", s_tex_logged, geo, r.base_type,
							r.ref_id, r.draw.diffuse_tex, tex_now.tex, tex_name[0] ? tex_name : "?", r.draw.diffuse_shade, tex_now.shade,
							repeat ? "again: refused texChurn" : "unclaimed, rebuilt at its next draw",
							s_tex_logged == kTexChangeLogs ? " (last diffuse-change line)" : ""),
							shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
					}
					if (traced(r))
						trace(geo, r, std::format("diffuse changed to '{}': {}", tex_name[0] ? tex_name : "?",
							repeat ? "twice within the window, refused texChurn" : "unclaimed"));
					r.tex_change_frame = now;
					unclaim(geo, r, Cause::Other);
					if (repeat) refuse_soft(r, Reason::TexChurn);
					continue;
				}
				if (!poll) continue;
				g_c.polled++;
				if (vis && !r.visible && traced(r)) trace(geo, r, "shown: the engine state no longer hides it");
				if (!vis && r.visible && why != Hide::Count)
				{
					g_c.hidden_by[static_cast<size_t>(why)]++;
					if (traced(r))
						trace(geo, r, std::format("hidden: {} switchNode=0x{:X} branch=0x{:X}",
							kHideNames[static_cast<size_t>(why)], r.switch_node, r.switch_branch));
					// The first hides of a session: what was hidden and why.
					static uint32_t s_hide_logged = 0;
					if (s_hide_logged < 40)
					{
						s_hide_logged++;
						shared::common::log("SceneStatics", std::format("hide #{} geo=0x{:X} base 0x{:02X} {} switchLeaf={} "
							"lastSeen {}f ago", s_hide_logged, geo, r.base_type, kHideNames[static_cast<size_t>(why)],
							r.switch_leaf ? 1 : 0, now - r.last_seen), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
					}
				}
				set_visible(r, vis);
			}
		}

		// ---- Scene membership: seeds ----
		//
		// Every geometry of a loaded non-actor reference reaches the record map
		// through the engine monitor's member feed (take_members, at each tick)
		// and is seeded once: classified, placed, and recorded from the scene
		// graph alone (its packed buffers and its properties) so the resolve
		// claims it whether or not the engine ever draws it. A member the seed
		// can't record yet is held back and looked at again (hold_seed); one
		// under a switch / LOD / range node is left to the engine's draws.

		constexpr uint32_t kMembersPerTake = 4096;
		constexpr uint32_t kMembersPerTick = 65536;
		constexpr UINT kHoldHiddenFrames = 120;    // app-culled / disabled / detached
		constexpr UINT kHoldPlaceFrames = 300;     // owner or attached cell not known yet
		constexpr UINT kHoldDataFrames = 120;      // not packed, no texture yet, build budget
		constexpr UINT kSeedWaitScanFrames = 30;
		constexpr UINT kSweepFrames = 600;         // the membership sweep (members neither claimed nor queued)

		// Gamebryo NiAlphaProperty test functions (flags bits 10-12) the
		// renderer takes: GREATER 4 and GREATEREQUAL 6; ALWAYS 0 = no test.
		constexpr uint32_t kNiTestAlways = 0, kNiTestGreater = 4, kNiTestGreaterEqual = 6;
		// NiStencilProperty draw mode (flags bits 10-11). ApplyStencil
		// (0xE87EF0, cull table NiDX9RenderState+0xD4 filled at 0xE883DB)
		// sets D3DCULL_CW for 0 (CCW or both, also a geometry without the
		// property) and 1 (CCW), D3DCULL_CCW for 2 (CW), D3DCULL_NONE for 3.
		constexpr uint32_t kNiDrawCw = 2;
		// BSShaderProperty flags1: FaceGen (no glow pass), parallax (height map).
		constexpr uint32_t kSf1FaceGen = 1u << 10, kSf1Parallax = 1u << 11;
		// NiMaterialProperty: emissive NiColor, the external-emittance colour
		// (NiColor*, set on flags1 bit 29 geometry), and the lighting shader's
		// gloss without a material.
		constexpr uint32_t kMatEmissive = 0x28, kMatEmitOverride = 0x34;
		constexpr float kNoMaterialGloss = 30.0f;

		void take_members()
		{
			static FnvRemixMember s_members[kMembersPerTake];
			const double t0 = now_us();
			uint32_t total = 0;
			for (;;)
			{
				const uint32_t n = nvse_link::take_members(s_members, kMembersPerTake);
				for (uint32_t i = 0; i < n; i++)
				{
					const uintptr_t geo = s_members[i].geo;
					if (!geo) continue;
					auto [it, fresh] = g_geos.try_emplace(geo);
					Rec& r = it->second;
					if (fresh) g_c.members_new++;
					r.member = true;
					if (r.state != State::Live) queue_seed(geo, r);
				}
				total += n;
				if (n < kMembersPerTake || total >= kMembersPerTick) break;
			}
			g_c.seed_us += now_us() - t0;
		}

		// The vertex attributes of a packed buffer's layout (stream 0), as a
		// draw's record takes them from the bound declaration.
		struct SeedLayout
		{
			Attr pos, nrm, uv, col;
			uint32_t texcoords = 0;
			bool skin = false, pretransformed = false, other_stream = false;
		};

		bool seed_layout(const sg::BufferRead& b, SeedLayout& out)
		{
			D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
			UINT n = 0;
			if (!sg::buffer_layout(b, el, n)) return false;
			for (UINT i = 0; i < n; i++)
			{
				const D3DVERTEXELEMENT9& e = el[i];
				if (e.Stream == 0xFF) break;
				if (e.Stream != 0)
				{
					out.other_stream = true;
					continue;
				}
				Attr a;
				a.off = static_cast<int>(e.Offset);
				a.type = static_cast<int>(e.Type);
				switch (e.Usage)
				{
				case D3DDECLUSAGE_POSITION: if (e.UsageIndex == 0 && !out.pos.ok()) out.pos = a; break;
				case D3DDECLUSAGE_NORMAL:   if (e.UsageIndex == 0 && !out.nrm.ok()) out.nrm = a; break;
				case D3DDECLUSAGE_TEXCOORD:
					out.texcoords++;
					if (e.UsageIndex == 0 && !out.uv.ok()) out.uv = a;
					break;
				case D3DDECLUSAGE_COLOR:    if (e.UsageIndex == 0 && !out.col.ok()) out.col = a; break;
				case D3DDECLUSAGE_BLENDWEIGHT:
				case D3DDECLUSAGE_BLENDINDICES: out.skin = true; break;
				case D3DDECLUSAGE_POSITIONT: out.pretransformed = true; break;
				default: break;
				}
			}
			return true;
		}

		// The class name of a property, or "" (RTTI, cached per vtable).
		bool property_is(uintptr_t prop, const char* name)
		{
			const sg::ClassInfo* ci = prop ? sg::class_of(prop) : nullptr;
			return ci && ci->name == name;
		}

		enum class SeedOutcome : uint8_t { Recorded, Held, Refused, Left };

		// The material a draw's base pass would describe, from the geometry's
		// properties, as the engine's lighting pass binds them (FalloutNV.exe
		// ShadowLightShader SetupTextures 0xB7D200, pass build 0xBDF790,
		// constants 0xC02D90 / 0xC03180):
		//   * textures: PPL role arrays 0 diffuse, 1 normal, 2 glow (used when
		//     set, unless FaceGen), 3 height (used with flags1 bit 11 parallax),
		//     else the texture pack's height map;
		//   * gloss (Toggles.z): NiMaterialProperty+0x38, 30 without one;
		//   * glow tint (EmittanceColor): the external-emittance colour
		//     (material+0x34) or the emissive colour (+0x28);
		//   * alpha test: NiAlphaProperty bits 9-12 and +0x1A, applied as-is;
		//   * two-sided: unless NiStencilProperty draw mode 2 (D3DCULL_CCW);
		//   * addressing: the engine clamp mode.
		// Called inside begin_resolve.
		SeedOutcome seed_material(uintptr_t geo, Rec& r, const sg::GeoRead& gr, uint16_t af, DrawRec& dr,
			material::Desc& md, Seed& held)
		{
			const uintptr_t shade = gr.props[sg::fnv::kPropShade];
			bool rd = false;
			const uintptr_t t0 = sg::ppl_texture(shade, 0);
			md.diffuse = t0 ? sg::d3d_texture(t0, rd) : nullptr;
			if (!md.diffuse)
			{
				held = Seed::Texture;
				return SeedOutcome::Held;
			}
			if (const char* only = rtxconf::d3d9_only(md.diffuse))
			{
				r.list = only;
				refuse(r, Reason::RtxConf);
				return SeedOutcome::Refused;
			}
			const auto& rc = rtxconf::scalars();
			const uintptr_t t1 = sg::ppl_texture(shade, 1);
			md.normal = t1 ? sg::d3d_texture(t1, rd) : nullptr;
			md.normal_role = md.normal != nullptr;
			if (!rc.protocol_enabled || (md.normal_role && rc.normal_alpha_mode != 1))
			{
				refuse(r, Reason::Protocol);
				return SeedOutcome::Refused;
			}
			const uintptr_t mp = gr.props[sg::fnv::kPropMaterial];
			const bool have_mat = property_is(mp, "NiMaterialProperty");
			if (md.normal_role)
			{
				md.gloss_alpha = true;
				float g = kNoMaterialGloss;
				if (have_mat && (!sg::read_f32(mp + sg::fnv::MAT_gloss, g) || !std::isfinite(g))) g = 0.0f;
				if (g > 0.0f) md.gloss = (std::min)(g, 4096.0f);
			}
			uint32_t flags1 = 0;
			sg::read_u32(shade + sg::fnv::SHADE_flags1, flags1);
			if (flags1 & kSf1Parallax)
				if (const uintptr_t t3 = sg::ppl_texture(shade, 3)) md.height = sg::d3d_texture(t3, rd);
			if (!md.height) md.height = tex_pack::height_for(md.diffuse);
			if (!(flags1 & kSf1FaceGen))
				if (const uintptr_t t2 = sg::ppl_texture(shade, 2)) md.glow = sg::d3d_texture(t2, rd);
			if (md.glow && have_mat)
			{
				// The same tint rules as a draw's EmittanceColor.
				float e[3] = { 1.0f, 1.0f, 1.0f };
				uint32_t ext = 0;
				if (!(sg::read_u32(mp + kMatEmitOverride, ext) && ext && sg::read_block(ext, e, sizeof(e))))
					sg::read_block(mp + kMatEmissive, e, sizeof(e));
				const uint8_t c[3] = { unorm8(e[0]), unorm8(e[1]), unorm8(e[2]) };
				if (c[0] < 254 || c[1] < 254 || c[2] < 254)
				{
					const uint8_t hi = (std::max)({ c[0], c[1], c[2] }), lo = (std::min)({ c[0], c[1], c[2] });
					if (cfg().scene_glow_grey_tint && hi - lo <= 1)
					{
						if (hi == 0) md.glow = nullptr;
						else md.glow_scale = (static_cast<float>(c[0]) + c[1] + c[2]) / (3.0f * 255.0f);
						dr.claim |= kClaimGlowGrey;
					}
					else if (material::glow_tint_ready())
					{
						for (int k = 0; k < 3; k++) md.glow_tint[k] = static_cast<float>(c[k]) / 255.0f;
						dr.claim |= kClaimGlowTint;
					}
					else if (material::glow_tint_pending())
					{
						held = Seed::Texture;   // the runtime's tint option is not settled yet
						return SeedOutcome::Held;
					}
					else
					{
						refuse(r, Reason::GlowTint);
						return SeedOutcome::Refused;
					}
				}
			}
			if (af & (1u << 9))
			{
				const uint32_t fn = (af >> 10) & 7u;
				uint8_t thr = 0;
				sg::read_u8(gr.props[sg::fnv::kPropAlpha] + sg::fnv::ALPHA_threshold, thr);
				if (fn == kNiTestGreater || fn == kNiTestGreaterEqual)
				{
					md.alpha_func = static_cast<uint8_t>(fn == kNiTestGreater ? D3DCMP_GREATER : D3DCMP_GREATEREQUAL);
					md.alpha_ref = thr;
				}
				else if (fn != kNiTestAlways)
				{
					refuse(r, Reason::AlphaFunc);
					return SeedOutcome::Refused;
				}
			}
			md.blend = is_blend(r.alpha);
			material::blend_test_as_cutout(md);
			dr.claim |= md.blend ? (md.alpha_func ? kClaimBlendTest : kClaimBlend) : (md.alpha_func ? kClaimTest : 0u);
			if (material::clamp_from_geometry(geo, md)) dr.clamp_mode = static_cast<uint8_t>(md.clamp_raw);
			if (EngineDiffuse ed; engine_diffuse(geo, ed))
			{
				dr.diffuse_known = true;
				dr.diffuse_shade = ed.shade;
				dr.diffuse_tex = ed.tex;
				dr.diffuse_d3d = ed.d3d;
			}
			{
				// As double_sided_for_cull rules for the draw: only the engine's
				// D3DCULL_CCW (draw mode CW) goes out single-sided. Its default
				// D3DCULL_CW keeps counter-clockwise front faces, which a
				// single-sided API mesh (clockwise front faces) would cull.
				const uintptr_t sp = gr.props[sg::fnv::kPropStencil];
				uint16_t sf = 0;
				dr.double_sided = !(property_is(sp, "NiStencilProperty") && sg::read_u16(sp + sg::fnv::STENCIL_flags, sf) &&
					((sf >> 10) & 3u) == kNiDrawCw);
			}
			md.path = "seed";
			md.plain = cfg().statics_debug_plain_material;
			const uint64_t mkey = material::key(md);
			auto mit = g_mats.find(mkey);
			if (mit == g_mats.end())
			{
				if (!take_build_budget(mkey))
				{
					held = Seed::Budget;
					return SeedOutcome::Held;
				}
				material::Built b;
				material::build(md, mkey, kSceneDomain ^ 0x4D4154ull, b);
				if (b.why != material::Decline::None)
				{
					for (const uint64_t t : b.textures) textures::release(t);
					if (b.handle) { if (auto* br = bridge()) br->DestroyMaterial(b.handle); }
					if (b.why == material::Decline::RtxConf)
					{
						r.list = b.list;
						refuse(r, b.list ? Reason::RtxConf : Reason::RtxHash);
						return SeedOutcome::Refused;
					}
					held = Seed::Texture;
					return SeedOutcome::Held;
				}
				Mat m;
				m.handle = b.handle;
				m.textures = std::move(b.textures);
				m.no_vertex_color = b.no_vertex_color;
				m.blend = md.blend && !b.alpha_dropped;
				m.alpha_func = static_cast<uint8_t>(b.alpha_dropped ? 0 : md.alpha_func);
				m.alpha_ref = static_cast<uint8_t>(b.alpha_dropped ? 0 : md.alpha_ref);
				m.categories = b.categories;
				m.last_used = frame();
				m.wrap_u = md.wrap_u;
				m.wrap_v = md.wrap_v;
				m.albedo_d3d9 = b.albedo_d3d9;
				mit = g_mats.emplace(mkey, std::move(m)).first;
				g_c.materials_built++;
			}
			mit->second.last_used = frame();
			dr.decal = material::decal_of(geo, nullptr, mit->second.categories, false);
			dr.mat_key = mkey;
			return SeedOutcome::Recorded;
		}

		// One member: classified, placed, and recorded from the scene graph,
		// then queued for this tick's resolve, or held back / refused / left
		// to the engine's draws.
		SeedOutcome seed_one(uintptr_t geo, Rec& r)
		{
			const UINT now = frame();
			if (r.state == State::Live)
			{
				r.seed = Seed::Claimed;
				return SeedOutcome::Left;
			}
			if (r.state == State::Refused)
			{
				if (!soft_refusal(r.why)) return SeedOutcome::Refused;
				if (now < r.not_before)
				{
					hold_seed(geo, r, r.seed == Seed::None || r.seed == Seed::Queued ? Seed::Place : r.seed, r.not_before - now);
					return SeedOutcome::Held;
				}
				r.state = State::Candidate;
				r.why = Reason::Count;
				r.tries = 0;
				r.not_before = 0;
				g_c.soft_retries++;
			}
			// A draw recorded it: this tick's resolve takes that record.
			if (r.draw.frame != kNoFrame) return SeedOutcome::Left;
			if (now < r.not_before)
			{
				hold_seed(geo, r, Seed::Budget, r.not_before - now);
				return SeedOutcome::Held;
			}
			if (sg::death_pending(geo) || !sg::begin_resolve(geo)) return SeedOutcome::Left;   // died: drained next tick
			ResolveGuard guard;
			const sg::FaultSite site("seed");
			g_c.seeds++;
			if (!r.classified) classify(geo, r);
			if (r.state == State::Refused) return SeedOutcome::Refused;

			sg::GeoRead gr;
			if (!sg::read_geo(geo, gr)) { refuse(r, Reason::NoData); return SeedOutcome::Refused; }
			uint16_t af = 0;
			const bool have_af = read_alpha(gr, af);
			AlphaKind ak = AlphaKind::Opaque;
			if (const Reason ar = alpha_class(have_af, af, ak); ar != Reason::Count) { refuse(r, ar); return SeedOutcome::Refused; }
			r.alpha = ak;

			// Place: the owner, the attached cell, what the chain passes.
			const Place pl = place_of(geo);
			if (!pl.known)
			{
				// The owner map let go of it (its reference's 3D or cell went):
				// a new walk brings it back through the feed.
				r.member = false;
				r.seed = Seed::None;
				return SeedOutcome::Left;
			}
			if (pl.unreadable)
			{
				hold_seed(geo, r, Seed::Place, kHoldPlaceFrames);
				return SeedOutcome::Held;
			}
			if (pl.o.flags & FNVR_OW_ACTOR) { refuse(r, Reason::Actor); return SeedOutcome::Refused; }
			if (pl.tree) { refuse(r, Reason::Subtree); return SeedOutcome::Refused; }
			r.base_type = pl.o.base_type;
			r.ref_id = pl.o.ref_id;
			if (pl.switched_off || (pl.o.flags & (FNVR_OW_APP_CULLED | FNVR_OW_APP_CULLED_ABOVE | FNVR_OW_DISABLED |
				FNVR_OW_DETACHED)))
			{
				hold_seed(geo, r, Seed::Hidden, kHoldHiddenFrames);
				return SeedOutcome::Held;
			}
			if (!pl.o.cell || !g_cells.count(pl.o.cell))
			{
				hold_seed(geo, r, Seed::Place, kHoldPlaceFrames);
				return SeedOutcome::Held;
			}
			if (!g_base_ok[pl.o.base_type]) { refuse(r, Reason::BaseType); return SeedOutcome::Refused; }

			// Mesh source: the engine's packed buffers (the very VB / IB its
			// draws bind) and their declaration; before the engine packed it,
			// its CPU arrays (packing frees the UVs and colours).
			sg::DataRead dd;
			if (!gr.data || !sg::read_data(gr.data, dd)) { refuse(r, Reason::NoData); return SeedOutcome::Refused; }
			DrawRec dr;
			dr.from_graph = true;
			if (!dd.packed)
			{
				const sg::ClassInfo* ci = sg::class_of(geo);
				const bool strips = ci && ci->leaf == sg::Leaf::TriStrips;
				if (!dd.verts || !dd.pos || !dd.nrm || !dd.uv || !dd.tris || !dd.f48 || (strips && !dd.f4c))
				{
					hold_seed(geo, r, Seed::Unpacked, kHoldDataFrames);
					return SeedOutcome::Held;
				}
				dr.cpu_data = gr.data;
				dr.cpu_strips = strips;
				// The arrays' element types (positions, normals, UV set 0, NiColorA).
				dr.pos = Attr{ 0, D3DDECLTYPE_FLOAT3 };
				dr.nrm = Attr{ 0, D3DDECLTYPE_FLOAT3 };
				dr.uv = Attr{ 0, D3DDECLTYPE_FLOAT2 };
				dr.col = dd.col ? Attr{ 0, D3DDECLTYPE_FLOAT4 } : Attr{};
				dr.d.type = strips ? D3DPT_TRIANGLESTRIP : D3DPT_TRIANGLELIST;
				dr.d.indexed = true;
				dr.d.num_vtx = dd.verts;
				dr.d.prim_count = dd.tris;
			}
			else
			{
				sg::BufferRead b;
				if (!sg::read_buffer(dd.packed, b) || !b.vb[0] || !b.ib || !b.stride[0])
				{
					hold_seed(geo, r, Seed::Unpacked, kHoldDataFrames);
					return SeedOutcome::Held;
				}
				SeedLayout sl;
				if (!seed_layout(b, sl))
				{
					hold_seed(geo, r, Seed::Unpacked, kHoldDataFrames);
					return SeedOutcome::Held;
				}
				// Land-shaped (colour + two texcoords): a draw decides whether it
				// is land or a placed object (SceneLandShapeStatics).
				if (sl.col.ok() && sl.texcoords >= 2)
				{
					r.seed = Seed::Layout;
					return SeedOutcome::Left;
				}
				const UINT stride = b.stride[0];
				const bool list = b.prim_type == D3DPT_TRIANGLELIST, strip = b.prim_type == D3DPT_TRIANGLESTRIP;
				const UINT want = list ? b.tris * 3u : b.tris ? b.tris + 2u : 0u;
				if (b.streams != 1 || sl.other_stream || sl.skin || sl.pretransformed || (!list && !strip) || !b.verts ||
					!b.tris || want > b.index_count || !sl.pos.ok() ||
					(sl.pos.type != D3DDECLTYPE_FLOAT3 && sl.pos.type != D3DDECLTYPE_FLOAT4) || !sl.nrm.ok() ||
					sl.nrm.type != D3DDECLTYPE_FLOAT3 || !sl.uv.ok() ||
					(sl.uv.type != D3DDECLTYPE_FLOAT2 && sl.uv.type != D3DDECLTYPE_FLOAT16_2) || !attr_fits(sl.pos, stride) ||
					!attr_fits(sl.nrm, stride) || !attr_fits(sl.uv, stride))
				{
					r.seed = Seed::Layout;
					refuse(r, Reason::DrawLayout);
					return SeedOutcome::Refused;
				}
				dr.vb = b.vb[0];
				dr.ib = b.ib;
				dr.stride = stride;
				dr.pos = sl.pos;
				dr.nrm = sl.nrm;
				dr.uv = sl.uv;
				dr.col = sl.col.ok() && attr_fits(sl.col, stride) ? sl.col : Attr{};
				dr.d.type = static_cast<D3DPRIMITIVETYPE>(b.prim_type);
				dr.d.indexed = true;
				dr.d.base = static_cast<INT>(b.base_vertex);
				dr.d.min_vtx = 0;
				dr.d.num_vtx = b.verts;
				dr.d.start_index = 0;
				dr.d.prim_count = b.tris;
				r.draw.vb = dr.vb;   // for the refusal report
				r.draw.ib = dr.ib;
				if (!identity::track_buffers(dr.vb, dr.ib)) { refuse(r, Reason::StreamUntracked); return SeedOutcome::Refused; }
				bool fresh = false, relaxed = false;
				const Reason sr = stream_reason(buffer_info(dr.vb), buffer_info(dr.ib), fresh, relaxed);
				if (sr != Reason::Count) { refuse_stream(r, sr); return SeedOutcome::Refused; }
				if (fresh)
				{
					hold_seed(geo, r, Seed::Unpacked, kQuietFrames);
					return SeedOutcome::Held;
				}
			}

			material::Desc md;
			Seed held = Seed::None;
			const SeedOutcome mo = seed_material(geo, r, gr, af, dr, md, held);
			if (mo == SeedOutcome::Held)
			{
				hold_seed(geo, r, held, held == Seed::Budget ? 1u : kHoldDataFrames);
				return mo;
			}
			if (mo != SeedOutcome::Recorded) return mo;

			SeedDesc sd;
			sd.diffuse = md.diffuse;
			sd.normal = md.normal;
			sd.glow = md.glow;
			sd.height = md.height;
			sd.gloss = md.gloss;
			sd.alpha_func = md.alpha_func;
			sd.alpha_ref = md.alpha_ref;
			sd.blend = md.blend;
			sd.normal_role = md.normal_role;
			sd.gloss_alpha = md.gloss_alpha;
			sd.wrap_u = md.wrap_u;
			sd.wrap_v = md.wrap_v;
			sd.filter = md.filter;
			sd.glow_scale = md.glow_scale;
			std::memcpy(sd.glow_tint, md.glow_tint, sizeof(sd.glow_tint));
			g_seed_desc[geo] = sd;

			dr.frame = now;
			r.draw = dr;
			r.seed = Seed::Queued;
			g_c.seed_records++;
			if (!r.queued)
			{
				r.queued = true;
				g_queue.push_back(geo);
			}
			if (traced(r)) trace(geo, r, std::format("seeded from the scene graph: window {}v/{}p mat 0x{:016X}", dr.d.num_vtx,
				dr.d.prim_count, dr.mat_key));
			return SeedOutcome::Recorded;
		}

		// The seed pass of a tick: members held back whose time came are
		// queued again, then queued members are seeded until the time budget
		// ([RemixApi] SceneWalkBudgetUs) or the create batch runs out. Every
		// kSweepFrames, members neither claimed, queued, held nor left to the
		// draws are queued again (a record dropped on the way).
		void seed_tick()
		{
			const UINT now = frame();
			if (!g_seed_next.empty())
			{
				static std::vector<uintptr_t> s_next;
				s_next.clear();
				s_next.swap(g_seed_next);
				for (const uintptr_t geo : s_next)
					if (const auto it = g_geos.find(geo); it != g_geos.end() && it->second.member &&
						it->second.state != State::Live)
						queue_seed(geo, it->second);
			}
			if (now - g_seed_wait_frame >= kSeedWaitScanFrames && !g_seed_wait.empty())
			{
				g_seed_wait_frame = now;
				size_t keep = 0;
				for (size_t i = 0; i < g_seed_wait.size(); i++)
				{
					const SeedWait w = g_seed_wait[i];
					if (now < w.at) { g_seed_wait[keep++] = w; continue; }
					if (const auto it = g_geos.find(w.geo); it != g_geos.end() && it->second.member &&
						it->second.state != State::Live)
						queue_seed(w.geo, it->second);
				}
				g_seed_wait.resize(keep);
			}
			if (now - g_sweep_frame >= kSweepFrames)
			{
				g_sweep_frame = now;
				for (auto& [geo, r] : g_geos)
				{
					if (!r.member || r.state == State::Live || r.seed_queued || r.queued) continue;
					if (r.seed == Seed::Layout) continue;
					if (r.state == State::Refused && !(soft_refusal(r.why) && now >= r.not_before)) continue;
					// Held: its wait entry queues it again.
					if (r.seed == Seed::Hidden || r.seed == Seed::Place || r.seed == Seed::Unpacked ||
						r.seed == Seed::Texture || r.seed == Seed::Budget)
						continue;
					queue_seed(geo, r);
					g_c.seed_swept++;
				}
			}
			if (g_seed_pos >= g_seed.size()) return;
			const sg::FaultSite site("seed");
			const double t0 = now_us();
			struct Spent
			{
				double t0;
				~Spent() { g_c.seed_us += now_us() - t0; }
			} spent{ t0 };
			const double budget = static_cast<double>((std::max)(50, cfg().scene_walk_budget_us));
			uint32_t done = 0;
			while (g_seed_pos < g_seed.size() && g_queue.size() < kCreateBatch)
			{
				const uintptr_t geo = g_seed[g_seed_pos++];
				const auto it = g_geos.find(geo);
				if (it == g_geos.end()) continue;   // died
				Rec& r = it->second;
				if (!r.seed_queued) continue;
				r.seed_queued = false;
				if (r.seed == Seed::Queued) r.seed = Seed::None;
				seed_one(geo, r);
				if ((++done & 7u) == 0 && now_us() - t0 > budget) break;
			}
			if (g_seed_pos >= g_seed.size())
			{
				g_seed.clear();
				g_seed_pos = 0;
			}
			else if (g_seed_pos > 4096)
			{
				g_seed.erase(g_seed.begin(), g_seed.begin() + static_cast<ptrdiff_t>(g_seed_pos));
				g_seed_pos = 0;
			}
		}

		void flush_destroys()
		{
			if (g_doomed.empty()) return;
			retained::destroy_external(g_doomed.data(), static_cast<uint32_t>(g_doomed.size()));
			g_doomed.clear();
		}

		// Claimed geometries whose buffer was written since the mesh was read
		// (check_written): the window's bytes now against the mesh's.
		//   * the same: the engine refilled it unchanged; the claim stays
		//     (generations adopted, the buffers marked built again);
		//   * different: unclaimed; a candidate again, claimed from its new
		//     contents (a geometry re-packed into the same window);
		//   * different again within kChangeWindow: its contents really move;
		//     refused streamed.rewritten (soft), draw capture takes it.
		// A buffer written while it was read gets no verdict: compared again
		// at the next tick.
		void verify_written()
		{
			check_written();
			if (g_verify.empty()) return;
			const UINT now = frame();
			static std::vector<uintptr_t> again;
			again.clear();
			for (const uintptr_t geo : g_verify)
			{
				const auto it = g_geos.find(geo);
				if (it == g_geos.end()) continue;
				Rec& r = it->second;
				if (r.state != State::Live || !r.verify) continue;
				const identity::BufferInfo vbi = identity::buffer(r.draw.vb);
				const identity::BufferInfo ibi = identity::buffer(r.draw.ib);
				const auto mit = g_meshes.find(r.mesh_key);
				uint64_t content = 0;
				const bool readable = mit != g_meshes.end() && window_content(r.draw, content);
				if (readable && content == mit->second.content)
				{
					if (!identity::note_built(r.draw.vb, vbi.gen) || !identity::note_built(r.draw.ib, ibi.gen))
					{
						g_c.rewrite_retry++;
						again.push_back(geo);
						continue;
					}
					r.verify = false;
					r.vb_gen = vbi.gen;
					r.ib_gen = ibi.gen;
					g_c.rewrite_same++;
					continue;
				}
				const bool repeat = r.change_frame != kNoFrame && now - r.change_frame <= kChangeWindow;
				if (g_change_logs < kChangeLogs)
				{
					g_change_logs++;
					shared::common::log("SceneStatics", std::format(
						"contents changed #{} geo=0x{:X} base 0x{:02X} ref {:08X}: window {}v/{}p {} (mesh 0x{:016X} vs now 0x{:016X}){} "
						"| vb {} | ib {}", g_change_logs, geo, r.base_type, r.ref_id, r.draw.d.num_vtx, r.draw.d.prim_count,
						readable ? "differs" : "unreadable", mit != g_meshes.end() ? mit->second.content : 0ull, content,
						repeat ? " -- again: refused streamed.rewritten" : " -- claimed again from the new contents",
						describe_buffer(r.draw.vb), describe_buffer(r.draw.ib)),
						shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				}
				if (traced(r))
					trace(geo, r, std::format("contents changed ({}){}", readable ? "different bytes" : "window unreadable",
						repeat ? ": twice within the change window, refused streamed.rewritten" : ": unclaimed, claimed again"));
				r.change_frame = now;
				unclaim(geo, r, Cause::Other);
				g_c.rewrite_changed++;
				if (repeat)
				{
					refuse_soft(r, Reason::StreamRewritten);
					g_c.rewrite_refused++;
				}
			}
			g_verify.swap(again);
		}

		void handle_deaths()
		{
			sg::take_deaths(g_scratch_dead);
			// Far LOD / distant water linked to a dead geometry
			// (RetainedLodRule = engine) leave the scene too, and grass
			// batches / leaf placements of it are destroyed.
			retained::on_engine_deaths(g_scratch_dead);
			if (!g_scratch_dead.empty()) api_submit::foliage_on_deaths(g_scratch_dead);
			if (!g_scratch_dead.empty()) api_submit::particles_on_deaths(g_scratch_dead);
			if (!g_scratch_dead.empty()) api_submit::statics_on_deaths(g_scratch_dead);
			for (const uintptr_t geo : g_scratch_dead)
			{
				const auto it = g_geos.find(geo);
				if (it == g_geos.end()) continue;
				if (it->second.state != State::Live && traced(it->second)) trace(geo, it->second, "~NiGeometry (not claimed at the time)");
				unclaim(geo, it->second, Cause::Dtor);
				g_seed_desc.erase(geo);
				// The address may be a new object's from now on.
				g_geos.erase(it);
				g_moves.erase(geo);
				g_movers.erase(geo);
			}
		}

		void evict()
		{
			const UINT now = frame();
			const UINT keep = static_cast<UINT>(cfg().evict_frames);
			auto* b = bridge();
			for (auto it = g_meshes.begin(); it != g_meshes.end();)
			{
				if (it->second.refs || now - it->second.last_used <= keep) { ++it; continue; }
				if (b && it->second.handle) b->DestroyMesh(it->second.handle);
				release_material(it->second.material_key);
				it = g_meshes.erase(it);
			}
			for (auto it = g_mats.begin(); it != g_mats.end();)
			{
				if (it->second.refs || now - it->second.last_used <= keep) { ++it; continue; }
				if (b && it->second.handle) b->DestroyMaterial(it->second.handle);
				for (const uint64_t t : it->second.textures) textures::release(t);
				it = g_mats.erase(it);
			}
		}

		void log_activity(bool force)
		{
			const UINT now = frame();
			if (!(g_act_created || g_act_destroyed || g_act_cells_in || g_act_cells_out)) return;
			if (!force && now - g_act_frame < kActivityLogFrames) return;
			g_act_frame = now;
			shared::common::log("SceneStatics", std::format(
				"cells +{} -{} ({} loaded) | instances created {} destroyed {} ({} claimed, {} meshes, {} materials)",
				g_act_cells_in, g_act_cells_out, g_cells.size(), g_act_created, g_act_destroyed, g_claimed,
				g_meshes.size(), g_mats.size()),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			g_act_created = g_act_destroyed = g_act_cells_in = g_act_cells_out = 0;
		}

		// The geometries that moved most since the last status line: class,
		// base form type, moves, largest translation / linear step, and
		// whether a controller sits on its chain (an animated object), e.g.
		// ", top [0xGEO NiTriShape MSTT(22) n=600 dt=0.000 dl=0.0120 ctrl]".
		std::string top_movers()
		{
			if (g_moves.empty()) return {};
			std::vector<std::pair<uintptr_t, MoveStat>> v(g_moves.begin(), g_moves.end());
			const size_t n = (std::min)(kTopMovers, v.size());
			std::partial_sort(v.begin(), v.begin() + n, v.end(),
				[](const auto& a, const auto& b) { return a.second.moves > b.second.moves; });
			std::string s = ", top";
			for (size_t i = 0; i < n; i++)
			{
				const uintptr_t geo = v[i].first;
				const MoveStat& ms = v[i].second;
				const auto it = g_geos.find(geo);
				const uint8_t bt = it != g_geos.end() ? it->second.base_type : 0;
				std::string cls = "?";
				bool ctrl = false;
				if (it != g_geos.end() && !sg::death_pending(geo) && sg::begin_resolve(geo))
				{
					if (const sg::ClassInfo* ci = sg::class_of(geo)) cls = ci->name;
					// A controller on the geometry or an ancestor up to its
					// reference's root: the engine animates it.
					uintptr_t node = geo;
					for (int hop = 0; hop < 16 && node && !ctrl; hop++)
					{
						sg::AvRead av;
						if (!sg::read_av(node, av)) break;
						ctrl = av.controller != 0;
						if (node == it->second.ref_root) break;
						node = av.parent;
					}
					sg::end_resolve();
				}
				s += std::format(" [0x{:X} {} base={:02X} n={} dt={:.3f} dl={:.4f}{}]", geo, cls, bt, ms.moves,
					ms.max_dt, ms.max_dl, ctrl ? " ctrl" : "");
			}
			return s;
		}

		// What identity recorded for a buffer (by address only: it may be
		// gone). `new` = created at an address no buffer had before.
		std::string describe_buffer(const void* p)
		{
			if (!p) return "none";
			const identity::BufferInfo i = identity::buffer(p);
			auto frames = [](UINT f) { return f == UINT_MAX ? std::string("-") : std::format("{}", f); };
			return std::format("0x{:X} {} usage=0x{:X} pool={} len={} gen={} writes={} (discard {} noOverwrite {} partial {}) "
				"lastWrite={}f ago created={}f ago{}{}", reinterpret_cast<uintptr_t>(p),
				i.created ? (i.recycled ? "recycled" : "new") : "unseenCreate", i.usage, i.pool, i.length, i.gen, i.writes,
				i.discards, i.no_overwrites, i.partial, frames(i.since_write), frames(i.since_create),
				i.relocked ? " relocked" : "", i.rewritten ? " rewritten" : "");
		}

		// One refused geometry for the report, read under the handshake: its
		// class, shade property (class, type, flags1 / flags2), alpha flags and
		// threshold, diffuse (PPL role 0) file name, the reference and base
		// type its chain passes, the last draw's PS / VS and the rtx.conf list;
		// for a streamed.* refusal, what identity recorded of its VB / IB.
		std::string describe_refused(uintptr_t geo, const Rec& r)
		{
			if (sg::death_pending(geo) || !sg::begin_resolve(geo)) return "died";
			ResolveGuard guard;
			const sg::FaultSite site("report");
			std::string s;
			const sg::ClassInfo* ci = sg::class_of(geo);
			s += ci ? ci->name : std::string("?");
			sg::GeoRead gr;
			if (sg::read_geo(geo, gr))
			{
				if (const uintptr_t shade = gr.props[sg::fnv::kPropShade])
				{
					const sg::ClassInfo* sci = sg::class_of(shade);
					uint32_t type = 0, f1 = 0, f2 = 0;
					sg::read_u32(shade + sg::fnv::SHADE_type, type);
					sg::read_u32(shade + sg::fnv::SHADE_flags1, f1);
					sg::read_u32(shade + sg::fnv::SHADE_flags2, f2);
					s += std::format(" | shade {} type={} f1=0x{:08X} f2=0x{:08X}", sci ? sci->name : std::string("?"),
						type, f1, f2);
					if (sci && sci->shade_ppl)
					{
						char name[128] = {};
						if (const uintptr_t tex = sg::ppl_texture(shade, 0); tex && sg::texture_name(tex, name, sizeof(name)))
							s += std::format(" tex '{}'", name);
					}
				}
				else s += " | no shade property";
				uint16_t af = 0;
				uint8_t thr = 0;
				if (read_alpha(gr, af))
				{
					sg::read_u8(gr.props[sg::fnv::kPropAlpha] + sg::fnv::ALPHA_threshold, thr);
					s += std::format(" | alpha 0x{:04X} (blend {} src {} dst {} test {} func {}) thr {}", af, af & 1u,
						(af >> 1) & 0xFu, (af >> 5) & 0xFu, (af >> 9) & 1u, (af >> 10) & 7u, thr);
				}
				s += std::format(" | skin {}", gr.skin ? "yes" : "no");
			}
			const Place pl = place_of(geo);
			if (pl.known)
				s += std::format(" | ref {:08X} base 0x{:02X} flags 0x{:X}{}", pl.o.ref_id, pl.o.base_type, pl.o.flags,
					pl.o.cell ? "" : ", no attached cell");
			else s += " | no owner (not under a loaded reference)";
			if (r.ps_hash || r.vs_hash) s += std::format(" | ps 0x{:08X} vs 0x{:08X}", r.ps_hash, r.vs_hash);
			if (r.list) s += std::format(" | list {}", r.list);
			if (streamed_reason(static_cast<size_t>(r.why)))
				s += std::format(" | vb {} | ib {}", describe_buffer(r.draw.vb), describe_buffer(r.draw.ib));
			return s;
		}

		// SceneDebugRefusalReport: once (again after the value changes), the top
		// N refused geometries of each reason by passes in the last window.
		void refusal_report(UINT window)
		{
			const int n = cfg().scene_debug_refusal_report;
			if (n <= 0 || frame() - g_active_frame < kReportAfterFrames) return;
			// After the first report, the streamed buckets again every
			// kReportRepeatFrames (they grow with cell loads, long after the
			// first report).
			const bool repeat = n == g_report_done;
			if (repeat && frame() - g_report_frame < kReportRepeatFrames) return;
			g_report_done = n;
			g_report_frame = frame();
			std::vector<std::pair<uint32_t, uintptr_t>> by[static_cast<size_t>(Reason::Count)];
			for (const auto& [geo, r] : g_geos)
				if (r.state == State::Refused) by[static_cast<size_t>(r.why)].emplace_back(r.passes, geo);
			for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
			{
				auto& v = by[i];
				if (v.empty() || (repeat && !streamed_reason(i))) continue;
				const size_t k = (std::min)(v.size(), static_cast<size_t>(n));
				std::partial_sort(v.begin(), v.begin() + k, v.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
				uint64_t total = 0;
				for (const auto& e : v) total += e.first;
				shared::common::log("SceneStatics", std::format("refused {}: {} geometries, {:.1f} passes / frame; top {}:",
					kReasonNames[i], v.size(), static_cast<double>(total) / window, k),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				for (size_t j = 0; j < k; j++)
				{
					const auto it = g_geos.find(v[j].second);
					if (it == g_geos.end()) continue;
					shared::common::log("SceneStatics", std::format("refused {} #{}: geo=0x{:X} {:.2f} passes / frame | {}",
						kReasonNames[i], j + 1, v[j].second, static_cast<double>(v[j].first) / window,
						describe_refused(v[j].second, it->second)), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				}
			}
		}

		// Scene membership against the traced scene, once per status line:
		// every geometry of a loaded non-actor reference (the member feed),
		// how many are traced, and why the rest are not.
		void log_members(UINT window)
		{
			uint32_t total = 0, live = 0, provisional = 0, hidden = 0, outside = 0;
			uint32_t by_seed[static_cast<size_t>(Seed::Count)] = {};
			uint32_t refused[static_cast<size_t>(Reason::Count)] = {};
			for (const auto& [geo, r] : g_geos)
			{
				if (!r.member)
				{
					if (r.state == State::Live) outside++;
					continue;
				}
				total++;
				switch (r.state)
				{
				case State::Live:
					live++;
					provisional += r.provisional ? 1 : 0;
					hidden += r.visible ? 0 : 1;
					break;
				case State::Refused: refused[static_cast<size_t>(r.why)]++; break;
				default: by_seed[static_cast<size_t>(r.seed == Seed::Claimed ? Seed::None : r.seed)]++; break;
				}
			}
			std::string waiting;
			for (const Seed s : { Seed::None, Seed::Queued, Seed::Hidden, Seed::Place, Seed::Unpacked, Seed::Texture, Seed::Budget })
				if (by_seed[static_cast<size_t>(s)])
					waiting += std::format("{}{} {}", waiting.empty() ? "" : " ", s == Seed::None ? "unseeded" : kSeedNames[static_cast<size_t>(s)],
						by_seed[static_cast<size_t>(s)]);
			std::string reasons;
			for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
				if (refused[i]) reasons += std::format("{}{} {}", reasons.empty() ? "" : " ", kReasonNames[i], refused[i]);
			std::string held;
			for (size_t i = 0; i < static_cast<size_t>(Seed::Count); i++)
				if (g_c.seed_held[i]) held += std::format("{}{} +{}", held.empty() ? "" : " ", kSeedNames[i], g_c.seed_held[i]);
			std::string fields;
			for (size_t b = 0; b < kSeedFields; b++)
				if (g_c.seed_field[b]) fields += std::format("{}{} {}", fields.empty() ? "" : " ", kSeedFieldNames[b], g_c.seed_field[b]);
			shared::common::log("RemixApi", std::format(
				"scene members: {} geometries of loaded references (feed +{}) | traced {} (provisional {}, hidden by the game {}) "
				"| not traced: waiting[{}] engine draws decide[landShape {}] refused[{}] | seeds +{} recorded +{} "
				"held[{}] swept +{} | first base pass of a provisional claim: confirmed +{} replaced +{} [{}] refused +{} "
				"unconfirmed +{} | traced from draws outside the membership {} | seed pass {:.1f} us / frame",
				total, g_c.members_new, live, provisional, hidden, waiting.empty() ? std::string("-") : waiting,
				by_seed[static_cast<size_t>(Seed::Layout)],
				reasons.empty() ? std::string("-") : reasons, g_c.seeds, g_c.seed_records, held.empty() ? std::string("-") : held,
				g_c.seed_swept, g_c.seed_confirmed, g_c.seed_replaced, fields.empty() ? std::string("-") : fields,
				g_c.seed_refused_at_draw, g_c.seed_unconfirmed, outside, g_c.seed_us / window),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		}

		void log_status()
		{
			const int interval = cfg().stats_interval_frames;
			const UINT now = frame();
			if (interval <= 0 || now - g_stats_frame < static_cast<UINT>(interval)) return;
			const UINT window = (std::max)(1u, now - g_stats_frame);
			g_stats_frame = now;
			uint32_t refused[static_cast<size_t>(Reason::Count)] = {};
			// Claimed geometry by what it carries (kClaim* bits; opaque = no
			// alpha test or blend).
			uint32_t by_claim[std::size(kClaimNames)] = {};
			uint32_t opaque = 0;
			// Candidates at the status line (after this tick's resolves, so a
			// record is normally consumed already): never recorded, held back
			// after a deferral, part-way through the anchor tries, recorded
			// and still queued.
			uint32_t candidates = 0, unrecorded = 0, backoff = 0, anchor_tries = 0, queued = 0;
			uint32_t hidden = 0, switch_leaves = 0;
			for (const auto& [geo, r] : g_geos)
			{
				switch (r.state)
				{
				case State::Refused: refused[static_cast<size_t>(r.why)]++; break;
				case State::Candidate:
					candidates++;
					if (r.draw.frame != kNoFrame) queued++;
					else if (now < r.not_before) backoff++;
					else if (!r.ever_recorded) unrecorded++;
					if (r.tries) anchor_tries++;
					break;
				case State::Live:
					hidden += r.visible ? 0 : 1;
					switch_leaves += r.switch_leaf ? 1 : 0;
					if (!(r.claim & (kClaimTest | kClaimBlendTest | kClaimBlend))) opaque++;
					for (size_t b = 0; b < std::size(kClaimNames); b++)
						if (r.claim & (1u << b)) by_claim[b]++;
					break;
				}
			}
			std::string reasons;
			for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
				if (refused[i]) reasons += std::format("{}{}={}", reasons.empty() ? "" : " ", kReasonNames[i], refused[i]);
			std::string claims = std::format("opaque={}", opaque);
			for (size_t b = 0; b < std::size(kClaimNames); b++) claims += std::format(" {}={}", kClaimNames[b], by_claim[b]);
			// What the refused geometry still costs per frame: its render passes
			// (every pass reaches D3D9 unless another route takes the draw) and
			// its draws in the FFP_WORLD branch.
			std::string costs;
			for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
				if (g_c.refused_passes[i] || g_c.refused_draws[i])
					costs += std::format("{}{}={:.1f}/{:.1f}", costs.empty() ? "" : " ", kReasonNames[i],
						static_cast<double>(g_c.refused_passes[i]) / window,
						static_cast<double>(g_c.refused_draws[i]) / window);
			std::string defers;
			for (size_t i = 0; i < static_cast<size_t>(Defer::Count); i++)
				if (g_c.deferred_by[i]) defers += std::format("{}{}={}", defers.empty() ? "" : " ", kDeferNames[i], g_c.deferred_by[i]);
			const auto hs = sg::hook_stats();
			const sg::FaultInfo fi = sg::last_fault();
			std::string faults = std::format("{}", fi.count);
			if (fi.count != g_faults_logged)
				faults += std::format(" (+{}, last in {} at 0x{:X})", fi.count - g_faults_logged, fi.site ? fi.site : "?", fi.addr);
			g_faults_logged = fi.count;
			shared::common::log("RemixApi", std::format(
				"scene: discovered=+{} (candidates {}: unrecorded {}, backoff {}, ownerTries {}, queued {}) claimed={} "
				"(+{}, failed {}) by[{}] instances={} (hidden {}, switch leaves {}) meshes={} (built +{}, shared +{}) "
				"mats={} (+{}) | draws: recorded +{} noGeo +{} unknown +{} backoff +{} notBase +{} blendNotBase +{} "
				"noDiffuse +{} budget +{} "
				"| resolves +{} owner[hit {} unknown {} detached {} culled {} disabled {} noCell {} unreadable {}] gone +{} "
				"retried +{} | suppressed passes={} notReady={} | destroyed dtor={} cell={} other={} | moved={} (poses {}, "
				"unseen {}, movers {}, absorbed {}, small {}, geos {}{}) hidden={} "
				"shown={} polled={} deferred={} [{}] stale={} | refused[{}] (new +{}) | cells={} ownerMap={}/{} pins={} "
				"uploadKiB={} bufferMismatch={} windowMismatch={} | handshake waits={} "
				"timeouts={} skippedDead={} | faults={}{}",
				g_c.discovered, candidates, unrecorded, backoff, anchor_tries, queued, g_claimed, g_c.created,
				g_c.create_failed, claims, g_live, hidden, switch_leaves,
				g_meshes.size(), g_c.meshes_built, g_c.meshes_shared, g_mats.size(), g_c.materials_built,
				g_c.recorded, g_c.draw_no_geo, g_c.draw_unknown, g_c.draw_backoff, g_c.draw_not_base,
				g_c.draw_blend_not_base, g_c.draw_no_diffuse,
				g_c.draw_budget, g_c.resolves, g_c.owner_hit, g_c.owner_unknown, g_c.owner_detached, g_c.owner_culled,
				g_c.owner_disabled, g_c.owner_no_cell, g_c.owner_unreadable, g_c.owner_gone, g_c.soft_retries,
				g_c.suppressed_passes,
				g_c.unready_passes, g_c.destroyed_dtor, g_c.destroyed_cell, g_c.destroyed_other,
				g_c.moved, g_c.pose_changes, g_c.pose_unseen, g_movers.size(), g_c.pose_absorbed, g_c.moved_small, g_moves.size(), top_movers(),
				g_c.hidden, g_c.shown, g_c.polled, g_c.deferred, defers.empty() ? std::string("-") : defers,
				g_c.stale_records, reasons.empty() ? std::string("-") : reasons, g_c.refused_new,
				g_cells.size(), nvse_link::facts().owners, nvse_link::facts().owner_geos, nvse_link::facts().pins,
				g_c.upload_bytes >> 10, g_c.buffer_mismatch, g_c.window_mismatch,
				hs.resolve_waits, hs.resolve_wait_timeouts, hs.resolves_skipped_dead, faults,
				(g_draw_instance ? std::format(" drawInstance={}", g_c.di_draws) : std::string()) +
					std::format(" hiddenBy[appCulled {} cellCulled {} detached {} disabled {} rootSwap {} switchOff {}]",
						g_c.hidden_by[0], g_c.hidden_by[1], g_c.hidden_by[2], g_c.hidden_by[3], g_c.hidden_by[4],
						g_c.hidden_by[5])),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			// Buffer identity since the last line: creations (recycled = at an
			// address another buffer had; streamed = that one had been streamed
			// into), write locks by flag, and writes to buffers a mesh was
			// built from.
			const identity::Stats bs = identity::take_stats();
			shared::common::log("RemixApi", std::format(
				"scene refused, per frame over {} frames (passes / FFP_WORLD draws): {} | streamed rule {} "
				"(relaxedClaims +{}) | claimed windows written: same bytes +{} changed +{} (refused rewritten +{}) retry +{} "
				"| buffers[tracked={} created vb +{} ib +{} (dynamic +{}) recycled +{} "
				"(streamed +{}) | writes +{} discard +{} noOverwrite +{} partial +{} | builtWrites +{}]", window,
				costs.empty() ? std::string("-") : costs, cfg().scene_streamed_relaxed ? "relaxed" : "strict",
				g_c.relaxed_claims, g_c.rewrite_same, g_c.rewrite_changed, g_c.rewrite_refused, g_c.rewrite_retry, bs.tracked, bs.created_vb, bs.created_ib, bs.created_dynamic,
				bs.recycled, bs.recycled_streamed, bs.writes, bs.discards, bs.no_overwrites, bs.partial, bs.built_writes),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
			log_members(window);
			refusal_report(window);
			for (auto& [geo, r] : g_geos) r.passes = 0;
			g_c = {};
			g_moves.clear();
			g_trace_lines = 0;
		}

		// A candidate being drawn right now (the renderer's current pass).
		Rec* drawn_candidate()
		{
			if (!g_active) return nullptr;
			const uintptr_t geo = sg::current_geometry();
			if (!geo) return nullptr;
			const auto it = g_geos.find(geo);
			if (it == g_geos.end() || it->second.state != State::Candidate) return nullptr;
			return &it->second;
		}

		void clear_all_state()
		{
			g_meshes.clear();
			g_mats.clear();
			g_geos.clear();
			g_moves.clear();
			g_queue.clear();
			g_dirty.clear();
			g_movers.clear();
			g_seen_hidden.clear();
			g_live_list.clear();
			g_live_list_dirty = true;
			g_poll_i = 0;
			g_cells.clear();
			g_doomed.clear();
			g_verify.clear();
			g_buffer_claims.clear();
			g_seed.clear();
			g_seed_pos = 0;
			g_seed_wait.clear();
			g_seed_next.clear();
			g_seed_desc.clear();
			g_members_resend = true;
			g_seen_epoch = identity::built_write_epoch();
			g_live = 0;
			g_claimed = 0;
			g_recreate = false;
			g_active = false;
			g_skip_passes = false;
			g_draw_instance = false;
			g_ok_frame = kNoFrame;
			g_logged_active = false;
			g_report_done = -1;   // classified again: report again
		}
	}

	bool enabled()
	{
		return cfg().scene_graph_statics;
	}

	bool owns_statics()
	{
		const auto& r = cfg();
		return r.scene_graph_statics && r.retained_instances && r.stable_origin &&
			nvse_link::has(FNVR_F_CELLS | FNVR_F_OWNERS);
	}

	void tick()
	{
		// A present on another thread (the loading screen's): the plugin
		// doesn't answer there; the next main-thread tick does the work.
		if (nvse_link::linked() && !nvse_link::live()) return;
		if (!owns_statics())
		{
			// Deaths are drained whenever the hooks are in (the foliage, the
			// LOD engine rule or the menu scene may have installed them), so
			// the queue never grows.
			if (sg::hooks_installed())
			{
				sg::take_deaths(g_scratch_dead);
				retained::on_engine_deaths(g_scratch_dead);
				if (!g_scratch_dead.empty()) api_submit::foliage_on_deaths(g_scratch_dead);
				if (!g_scratch_dead.empty()) api_submit::particles_on_deaths(g_scratch_dead);
				if (!g_scratch_dead.empty()) api_submit::statics_on_deaths(g_scratch_dead);
			}
			if (g_active || !g_geos.empty() || !g_meshes.empty() || !g_mats.empty()) drop_all("SceneGraphStatics inactive");
			if (enabled() && !g_logged_inactive && (nvse_link::linked() || !nvse_link::problem().empty()))
			{
				g_logged_inactive = true;
				shared::common::log("SceneStatics", nvse_link::has(FNVR_F_CELLS | FNVR_F_OWNERS)
					? "SceneGraphStatics needs RetainedInstances=1 and StableOrigin=1 -- statics go through draw capture"
					: "SceneGraphStatics needs the engine monitor plugin's cell events and owner map (see [NvseLink]) -- "
					  "statics go through draw capture", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			return;
		}
		if (!sg::install_hooks())
		{
			if (!g_logged_hooks)
			{
				g_logged_hooks = true;
				shared::common::log("SceneStatics", "SceneGraphStatics: the scene-graph hooks could not be installed (see "
					"above) -- statics go through draw capture", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			return;
		}
		const auto& r = cfg();
		if (!g_logged_active)
		{
			g_logged_active = true;
			g_active_frame = frame();
			shared::common::log("SceneStatics", "SceneGraphStatics active: statics are discovered at the render-pass "
				"dispatcher and kept while culled; what it does not take goes through draw capture",
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		sg::note_render_thread();
		sg::set_pass_filter(&pass_filter);
		g_active = true;
		if (g_base_ok.size() != 256)
		{
			g_base_ok.assign(256, 0);
			for (const uint32_t t : r.scene_base_types)
				if (t < 256) g_base_ok[t] = 1;
		}
		if (g_trace_ok.size() != 256)
		{
			g_trace_ok.assign(256, 0);
			for (const uint32_t t : r.scene_debug_trace_base_types)
				if (t < 256) g_trace_ok[t] = 1;
		}
		g_draw_instance = r.scene_debug_draw_instance;
		g_skip_passes = true;
		sg::skipped_passes_take();   // counted by the filter itself (suppressed_passes)

		const sg::FaultSite site("tick");
		// Members first: a member that died since its walk is in the dead
		// set, and the drain below forgets it.
		if (g_members_resend && nvse_link::resend_members()) g_members_resend = false;
		take_members();
		handle_deaths();
		verify_written();
		residency();

		// Resolves wait while the retained path can't take instances, and
		// until the engine monitor reports the cut settled (a load, a door,
		// fast travel: cells are being rebuilt).
		const bool settled = nvse_link::facts().settled != 0;
		const bool can_create = g_draw_instance ? stable_origin::active() : retained::available();
		if (can_create && settled)
		{
			if (g_recreate && !g_draw_instance) recreate_all();
			seed_tick();
			resolve_queue();
		}
		// The polls read the poses of what the engine didn't render; movers
		// sends every changed pose (render pass or poll) this tick.
		poll_movers();
		poll_hides();
		movers();
		flush_destroys();
		retained::set_external_live(g_live);
		if ((frame() & 63u) == 0)
		{
			evict();
		}
		log_activity(false);
		log_status();
	}

	namespace
	{
		// A candidate's pass in the renderer (FFP_WORLD shape, the
		// dispatcher's current geometry): recorded when it is the base pass
		// the statics path can take (material built, layout, buffers,
		// window) and queued for the resolve, refused for what the draw
		// shows, or left for a later pass. `md` is the material description
		// the pass gave (when it got that far).
		void record_pass(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d, uintptr_t geo, Rec& rec,
			material::Desc& md)
		{
			const UINT now = frame();
			const sg::FaultSite site("draw");

			// Only a base pass says what the geometry is: additive light passes
			// (blended) and depth-only passes are the engine's lighting. A blended
			// candidate (SceneClaimAlphaBlend) is recorded from its plain
			// alpha-blend pass (SrcAlpha / InvSrcAlpha, as its NiAlphaProperty
			// says), with or without depth write; any other pass of it (blend
			// off, an additive light pass) is not its base pass.
			const bool blended = render_state(dev, D3DRS_ALPHABLENDENABLE, FALSE) != 0;
			if (!render_state(dev, D3DRS_ZENABLE, D3DZB_TRUE) || !render_state(dev, D3DRS_COLORWRITEENABLE, 0xF) ||
				game::uses_viewmodel_projection())
			{
				g_c.draw_not_base++;
				return;
			}
			if (is_blend(rec.alpha))
			{
				if (!blended || render_state(dev, D3DRS_SRCBLEND, D3DBLEND_ONE) != D3DBLEND_SRCALPHA ||
					render_state(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO) != D3DBLEND_INVSRCALPHA ||
					render_state(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD) != D3DBLENDOP_ADD)
				{
					g_c.draw_blend_not_base++;
					return;
				}
			}
			else if (blended || !render_state(dev, D3DRS_ZWRITEENABLE, TRUE))
			{
				g_c.draw_not_base++;
				return;
			}
			const auto& r = cfg();
			const auto& ffp = shared::common::ffp_state::get();
			if (psmap) rec.ps_hash = psmap->ps_hash;

			// Shader: a lit object shader with a diffuse and one BaseMap, nothing
			// layered on top. A PS without Toggles is taken too: the protocol
			// gives it no gloss.
			if (!psmap || !psmap->any_classified || !psmap->has(PsSlotRole::Diffuse) || psmap->base_map_count > 1 ||
				psmap->multi_layer_count || psmap->has(PsSlotRole::Decal) || psmap->has_lod_sampler ||
				psmap->has_water_sampler || psmap->is_water_surface || psmap->facegen0_slot != PsSlotMap::kNoSlot ||
				psmap->facegen1_slot != PsSlotMap::kNoSlot)
				return refuse(rec, Reason::DrawShader);
			{
				const auto& deny = r.statics_ps_deny;
				const auto& allow = r.statics_ps_allow;
				if (std::find(deny.begin(), deny.end(), psmap->ps_hash) != deny.end() ||
					(!allow.empty() && std::find(allow.begin(), allow.end(), psmap->ps_hash) == allow.end()))
					return refuse(rec, Reason::DrawDenied);
			}
			const auto* vs = shared::common::g_vs_classifier.classify(ffp.last_vs());
			if (vs) rec.vs_hash = vs->vs_hash;
			if (!vs || vs->family != shared::common::VsFamily::None || game::is_no_lighting()) return refuse(rec, Reason::DrawVs);
			const auto& rc = rtxconf::scalars();
			const bool normal_role = psmap->has(PsSlotRole::Normal);
			// normalAlphaMode 0 (alpha = specular mask) has no API equivalent.
			if (!rc.protocol_enabled || (normal_role && rc.normal_alpha_mode != 1)) return refuse(rec, Reason::Protocol);

			// Layout: stream 0 only; POSITION float3/4, NORMAL float3, TEXCOORD0
			// float2 / half2; an indexed list or strip at stream offset 0.
			DrawRec dr;
			if (ffp.cur_decl_multi_stream() || ffp.cur_decl_is_skinned() || ffp.cur_decl_has_blendindices() ||
				ffp.cur_decl_has_pos_t() || !d.indexed || (d.type != D3DPT_TRIANGLELIST && d.type != D3DPT_TRIANGLESTRIP))
				return refuse(rec, Reason::DrawLayout);
			for (UINT i = 0; i < ffp.cur_decl_num_elements(); i++)
				if (ffp.cur_decl_elements()[i].Usage == D3DDECLUSAGE_BLENDWEIGHT) return refuse(rec, Reason::DrawLayout);
			dr.pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			dr.nrm = find_attr(D3DDECLUSAGE_NORMAL, 0);
			dr.uv = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			dr.col = find_attr(D3DDECLUSAGE_COLOR, 0);
			dr.stride = ffp.stream_stride(0);
			dr.vb = ffp.stream_vb(0);
			if (!dr.vb || !dr.stride || ffp.stream_offset(0) != 0 ||
				!dr.pos.ok() || (dr.pos.type != D3DDECLTYPE_FLOAT3 && dr.pos.type != D3DDECLTYPE_FLOAT4) ||
				!dr.nrm.ok() || dr.nrm.type != D3DDECLTYPE_FLOAT3 ||
				!dr.uv.ok() || (dr.uv.type != D3DDECLTYPE_FLOAT2 && dr.uv.type != D3DDECLTYPE_FLOAT16_2) ||
				!attr_fits(dr.pos, dr.stride) || !attr_fits(dr.nrm, dr.stride) || !attr_fits(dr.uv, dr.stride))
				return refuse(rec, Reason::DrawLayout);
			if (dr.col.ok() && !attr_fits(dr.col, dr.stride)) dr.col = {};
			{
				IDirect3DIndexBuffer9* ib = nullptr;
				if (FAILED(dev->GetIndices(&ib)) || !ib) return refuse(rec, Reason::DrawLayout);
				ib->Release();   // compared by address and read between frames; the geometry keeps it alive
				dr.ib = ib;
			}
			dr.d = d;
			dr.double_sided = double_sided_for_cull(dev);

			// Alpha: a test only as GREATER / GREATEREQUAL, with ALPHAREF as the
			// cutoff against the diffuse's alpha. A blended candidate's pass with
			// a test is a cutout (material::blend_test_as_cutout); one without a
			// test takes its blend onto the instance (material::alpha_blend).
			md.blend = blended;
			if (render_state(dev, D3DRS_ALPHATESTENABLE, FALSE))
			{
				const DWORD func = render_state(dev, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
				if (func == D3DCMP_GREATER || func == D3DCMP_GREATEREQUAL)
				{
					md.alpha_func = static_cast<uint8_t>(func);
					md.alpha_ref = static_cast<uint8_t>(render_state(dev, D3DRS_ALPHAREF, 0) & 0xFF);
				}
				else if (func != D3DCMP_ALWAYS) return refuse(rec, Reason::AlphaFunc);
			}
			material::blend_test_as_cutout(md);
			dr.claim = md.blend ? (md.alpha_func ? kClaimBlendTest : kClaimBlend) : (md.alpha_func ? kClaimTest : 0u);

			// Buffers the engine streams into or refills can't be cached
			// (stream_reason); one streamed into moments ago is looked at again
			// after the deferral.
			rec.draw.vb = dr.vb;   // for the refusal report (compared by address only)
			rec.draw.ib = dr.ib;
			if (!identity::track_buffers(dr.vb, dr.ib)) return refuse(rec, Reason::StreamUntracked);
			{
				bool fresh = false, relaxed = false;
				const Reason sr = stream_reason(buffer_info(dr.vb), buffer_info(dr.ib), fresh, relaxed);
				if (sr != Reason::Count) return refuse_stream(rec, sr);
				if (fresh) return defer(rec, Defer::Fresh);
			}

			// Material description: what apply_ps_protocol would put in RS 149 /
			// 169 / 177 and the texture slots the runtime would read.
			const float* pc = ffp.ps_const_data();
			md.diffuse = ffp.cur_texture(psmap->slot(PsSlotRole::Diffuse));
			if (!md.diffuse)
			{
				g_c.draw_no_diffuse++;   // nothing bound this time: the next draw
				return;
			}
			// An albedo in a list the runtime reads only on D3D9 draws
			// (rtx.lightConverter: the effect light of a bulb / neon sign) is
			// never claimed: its draws keep the D3D9 route (renderer.cpp,
			// rtxconf::d3d9_only). Refused before any build slot is taken.
			if (const char* only = rtxconf::d3d9_only(md.diffuse))
			{
				rec.list = only;
				return refuse(rec, Reason::RtxConf);
			}
			const bool lit_spec = psmap->toggles_reg != PsSlotMap::kNoReg && psmap->base_map_count <= 1;
			md.normal_role = normal_role;
			if (normal_role)
			{
				md.normal = ffp.cur_texture(psmap->slot(PsSlotRole::Normal));
				md.gloss_alpha = lit_spec;
				if (lit_spec)
				{
					const float g = pc[psmap->toggles_reg * 4 + 2];
					if (std::isfinite(g) && g > 0.0f) md.gloss = (std::min)(g, 4096.0f);
				}
			}
			if (psmap->has(PsSlotRole::Glow))
			{
				md.glow = ffp.cur_texture(psmap->slot(PsSlotRole::Glow));
				if (md.glow && psmap->emittance_reg != PsSlotMap::kNoReg)
				{
					// The D3D9 route tints the glow texture by EmittanceColor
					// (OPAQUE_SURFACE_MATERIAL_FLAG_EMISSIVE_TEXTURE_TINTED). White
					// needs nothing; grey is the same as scaling the intensity, black
					// as no glow (SceneGlowGreyTint); a colour goes on the material's
					// emissive constant, which the runtime applies with
					// rtx.fnvProtocol.apiGlowTint ([RemixApi] ApiGlowTint: waits for
					// the push to settle; off keeps D3D9).
					const float* e = pc + psmap->emittance_reg * 4;
					const uint8_t c[3] = { unorm8(e[0]), unorm8(e[1]), unorm8(e[2]) };
					if (c[0] < 254 || c[1] < 254 || c[2] < 254)
					{
						const uint8_t hi = (std::max)({ c[0], c[1], c[2] }), lo = (std::min)({ c[0], c[1], c[2] });
						if (r.scene_glow_grey_tint && hi - lo <= 1)
						{
							if (hi == 0) md.glow = nullptr;
							else md.glow_scale = (static_cast<float>(c[0]) + c[1] + c[2]) / (3.0f * 255.0f);
							dr.claim |= kClaimGlowGrey;
						}
						else if (material::glow_tint_ready())
						{
							for (int k = 0; k < 3; k++) md.glow_tint[k] = static_cast<float>(c[k]) / 255.0f;
							dr.claim |= kClaimGlowTint;
						}
						else if (material::glow_tint_pending())
						{
							defer(rec, Defer::Material);
							return;
						}
						else return refuse(rec, Reason::GlowTint);
					}
				}
			}
			md.height = psmap->has(PsSlotRole::Height)
				? ffp.cur_texture(psmap->slot(PsSlotRole::Height))
				: tex_pack::height_for(md.diffuse);   // nullptr when packs are off
			// The albedo's own sampler stage (not stage 0): addressing and filter;
			// the geometry's own Texture Clamp Mode (its shade property, read by
			// exact class) overrides the addressing. The wrap pair it gives is in
			// the material key.
			material::sampler_from_stage(dev, psmap->slot(PsSlotRole::Diffuse), md);
			if (material::clamp_from_geometry(geo, md)) dr.clamp_mode = static_cast<uint8_t>(md.clamp_raw);
			if (EngineDiffuse ed; engine_diffuse(geo, ed))
			{
				dr.diffuse_known = true;
				dr.diffuse_shade = ed.shade;
				dr.diffuse_tex = ed.tex;
				dr.diffuse_d3d = ed.d3d;
			}
			md.path = "scene";
			md.plain = r.statics_debug_plain_material;
			if (dr.col.ok() && material::hair_tint(psmap, dr.hair_tint))
			{
				dr.hair = true;
				material::log_hair("scene", psmap->ps_hash, nullptr, dr.hair_tint);
			}
			const uint64_t mkey = material::key(md);

			// Built now, while the pass has its textures bound (the draw path's
			// guarantee that they are alive); shared by every mesh that uses it.
			auto mit = g_mats.find(mkey);
			if (mit == g_mats.end())
			{
				if (!take_build_budget(mkey))
				{
					g_c.draw_budget++;   // the next draw
					return;
				}
				material::Built b;
				material::build(md, mkey, kSceneDomain ^ 0x4D4154ull, b);
				if (b.why != material::Decline::None)
				{
					for (const uint64_t t : b.textures) textures::release(t);
					if (b.handle) { if (auto* br = bridge()) br->DestroyMaterial(b.handle); }
					if (b.why == material::Decline::RtxConf)
					{
						rec.list = b.list;
						return refuse(rec, b.list ? Reason::RtxConf : Reason::RtxHash);
					}
					defer(rec, Defer::Material);
					return;
				}
				Mat m;
				m.handle = b.handle;
				m.textures = std::move(b.textures);
				m.no_vertex_color = b.no_vertex_color;
				m.blend = md.blend && !b.alpha_dropped;
				m.alpha_func = static_cast<uint8_t>(b.alpha_dropped ? 0 : md.alpha_func);
				m.alpha_ref = static_cast<uint8_t>(b.alpha_dropped ? 0 : md.alpha_ref);
				m.categories = b.categories;
				m.last_used = now;
				m.wrap_u = md.wrap_u;
				m.wrap_v = md.wrap_v;
				m.albedo_d3d9 = b.albedo_d3d9;
				mit = g_mats.emplace(mkey, std::move(m)).first;
				g_c.materials_built++;
			}
			mit->second.last_used = now;

			// Decal: the shader's decal flags (material::decal_of); logged once per
			// geometry when blended or tagged.
			dr.decal = material::decal_of(geo, dev, mit->second.categories, false);
			material::log_decal("scene", geo, mit->second.albedo_d3d9, dr.decal);

			dr.mat_key = mkey;
			dr.frame = now;
			rec.draw = dr;
			rec.ever_recorded = true;
			g_c.recorded++;
			if (!rec.queued)
			{
				rec.queued = true;
				g_queue.push_back(geo);
			}
		}

		// Which seed parity fields differ between a provisional claim's seed
		// record (and its SeedDesc) and the draw's record of its first base
		// pass (kSeedFieldNames bits); 0 = the same claim.
		uint32_t seed_diff(uintptr_t geo, const DrawRec& was, const DrawRec& now_rec, const material::Desc& md)
		{
			uint32_t bits = 0;
			auto set = [&](size_t b) { bits |= 1u << b; };
			if (const auto it = g_seed_desc.find(geo); it != g_seed_desc.end())
			{
				const SeedDesc& sd = it->second;
				if (sd.diffuse != md.diffuse) set(0);
				if (sd.normal != md.normal || sd.normal_role != md.normal_role || sd.gloss_alpha != md.gloss_alpha) set(1);
				if (sd.glow != md.glow) set(2);
				if (sd.height != md.height) set(3);
				if (sd.gloss != md.gloss) set(4);
				if (sd.alpha_func != md.alpha_func || sd.alpha_ref != md.alpha_ref) set(5);
				if (sd.blend != md.blend) set(6);
				if (sd.wrap_u != md.wrap_u || sd.wrap_v != md.wrap_v) set(7);
				if (sd.filter != md.filter) set(8);
				if (sd.glow_scale != md.glow_scale || std::memcmp(sd.glow_tint, md.glow_tint, sizeof(sd.glow_tint)) != 0) set(13);
			}
			if (was.double_sided != now_rec.double_sided) set(9);
			// A seed from the CPU arrays has no buffers: its mesh is the same
			// geometry's, so its window and layout are not compared.
			auto same_attr = [](const Attr& a, const Attr& b) { return a.off == b.off && a.type == b.type; };
			if (!was.cpu_data && (was.vb != now_rec.vb || was.ib != now_rec.ib || was.stride != now_rec.stride ||
				was.d.type != now_rec.d.type || was.d.base != now_rec.d.base || was.d.min_vtx != now_rec.d.min_vtx ||
				was.d.num_vtx != now_rec.d.num_vtx || was.d.start_index != now_rec.d.start_index ||
				was.d.prim_count != now_rec.d.prim_count))
				set(10);
			if (!was.cpu_data && (!same_attr(was.pos, now_rec.pos) || !same_attr(was.nrm, now_rec.nrm) ||
				!same_attr(was.uv, now_rec.uv) || !same_attr(was.col, now_rec.col)))
				set(11);
			if (was.cpu_data && was.col.ok() != now_rec.col.ok()) set(11);
			if (was.decal.categories != now_rec.decal.categories) set(12);
			// Anything else the claim carries (the material key's other
			// scalars, the claim bits, the clamp mode, the hair tint).
			if ((was.mat_key != now_rec.mat_key && !bits) || was.claim != now_rec.claim ||
				was.clamp_mode != now_rec.clamp_mode || was.hair != now_rec.hair)
				set(14);
			return bits;
		}

		// A provisional claim's base pass: the draw's record against the
		// seed's. The same: confirmed (its passes are skipped from now on).
		// Different: the claim is replaced by the draw's record in this tick's
		// resolve (one destroy and one create in the same tick). A refusal
		// the draw shows (shader, layout, alpha function, rtx.conf):
		// unclaimed and refused. Its draws this frame are dropped either way:
		// the instance still draws it until the tick.
		void confirm_pass(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d, uintptr_t geo, Rec& rec)
		{
			const UINT now = frame();
			Rec probe = rec;
			probe.state = State::Candidate;
			probe.why = Reason::Count;
			probe.not_before = 0;
			probe.queued = true;   // the probe never queues itself
			probe.draw = {};
			material::Desc md;
			record_pass(dev, psmap, d, geo, probe, md);
			rec.ps_hash = probe.ps_hash;
			rec.vs_hash = probe.vs_hash;
			if (probe.state == State::Refused)
			{
				g_c.seed_refused_at_draw++;
				if (traced(rec))
					trace(geo, rec, std::format("provisional claim refused at its first base pass: {}",
						kReasonNames[static_cast<size_t>(probe.why)]));
				unclaim(geo, rec, Cause::Other);
				rec.seed_queued = false;
				rec.seed = Seed::None;
				rec.state = State::Refused;
				rec.why = probe.why;
				rec.not_before = probe.not_before;
				rec.list = probe.list;
				return;
			}
			if (probe.draw.frame != now)
			{
				// Not its base pass, or held back (a fresh buffer, a material not
				// built yet): the claim stands until a later pass.
				if (probe.not_before > now) rec.not_before = probe.not_before;
				return;
			}
			const uint32_t bits = seed_diff(geo, rec.draw, probe.draw, md);
			if (!bits)
			{
				rec.provisional = false;
				g_seed_desc.erase(geo);
				g_c.seed_confirmed++;
				return;
			}
			for (size_t b = 0; b < kSeedFields; b++)
				if (bits & (1u << b)) g_c.seed_field[b]++;
			g_c.seed_replaced++;
			if (traced(rec))
			{
				std::string f;
				for (size_t b = 0; b < kSeedFields; b++)
					if (bits & (1u << b)) f += std::format("{}{}", f.empty() ? "" : " ", kSeedFieldNames[b]);
				trace(geo, rec, std::format("provisional claim replaced by its first base pass ({})", f));
			}
			unclaim(geo, rec, Cause::Other);
			rec.seed_queued = false;
			rec.seed = Seed::None;
			rec.draw = probe.draw;
			rec.ever_recorded = true;
			rec.consume_frame = now;
			if (!rec.queued)
			{
				rec.queued = true;
				g_queue.push_back(geo);
			}
		}
	}

	bool on_static_draw(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d)
	{
		if (!g_active) return false;
		// A world draw to the backbuffer: this frame's camera for the retained
		// entries' on-screen tests (cheap after the frame's first call).
		retained::note_world_view();
		const uintptr_t geo = sg::current_geometry();
		if (!geo)
		{
			g_c.draw_no_geo++;   // an FFP_WORLD draw outside the dispatcher
			return false;
		}
		const auto git = g_geos.find(geo);
		if (git == g_geos.end())
		{
			g_c.draw_unknown++;   // dispatched but never classified (a death pending, the filter off)
			return false;
		}
		Rec& rec = git->second;
		const UINT now = frame();
		// A traced geometry the renderer draws (not claimed, or claimed and
		// unclaimed): whether the engine still renders it, and under which
		// refusal.
		if (rec.state != State::Live && traced(rec) &&
			(rec.trace_d3d9_frame == kNoFrame || now - rec.trace_d3d9_frame >= kTraceD3d9Every))
		{
			rec.trace_d3d9_frame = now;
			trace(geo, rec, std::format("reached the renderer (draw capture){}", rec.state == State::Refused
				? std::format(", refused {}", kReasonNames[static_cast<size_t>(rec.why)]) : std::string()));
		}
		if (rec.state == State::Refused)
		{
			// What the bucket still sends through FFP_WORLD (and its PS, for
			// the refusal report).
			g_c.refused_draws[static_cast<size_t>(rec.why)]++;
			if (psmap) rec.ps_hash = psmap->ps_hash;
			return rec.consume_frame == now;
		}
		if (rec.state == State::Live)
		{
			if (rec.provisional && rec.consume_frame == now)
			{
				const sg::FaultSite site("confirm");
				confirm_pass(dev, psmap, d, geo, rec);
			}
			return rec.consume_frame == now;
		}
		if (rec.state != State::Candidate || rec.draw.frame == now) return rec.consume_frame == now;
		if (now < rec.not_before)
		{
			g_c.draw_backoff++;
			return rec.consume_frame == now;
		}
		material::Desc md;
		record_pass(dev, psmap, d, geo, rec, md);
		return rec.consume_frame == now;
	}

	bool on_shaped_draw(ShapedDraw kind, const PsSlotMap* psmap)
	{
		Rec* rp = drawn_candidate();
		if (!rp) return false;
		switch (kind)
		{
		case ShapedDraw::Terrain:
		{
			// Colour + two texcoords is land's layout, but a placed object can
			// have it too. Land quads (TESObjectLAND) aren't references: a land
			// shader, or a geometry no reference owns, is land.
			if (rp->land_shape) return true;
			if (cfg().scene_land_shape_statics && !land_like(psmap))
			{
				const sg::FaultSite site("landShape");
				FnvRemixOwner o;
				if (nvse_link::owner_of(sg::current_geometry(), o))
				{
					rp->land_shape = true;
					rp->tries = 0;
					return true;
				}
				// Its reference's 3D event may not be applied yet: a later draw
				// decides (at most kLandShapeTries draws, then it counts as land).
				if (++rp->tries < kLandShapeTries) return false;
				rp->tries = 0;
				refuse(*rp, Reason::Terrain);
				rp->land_unowned = true;
				return false;
			}
			refuse(*rp, Reason::Terrain);
			return false;
		}
		case ShapedDraw::BlendIndices: refuse(*rp, Reason::DrawLayout); return false;
		case ShapedDraw::FarLod:       refuse(*rp, Reason::Lod); return false;
		}
		return false;
	}

	bool claimed(uintptr_t geo)
	{
		if (!g_active || !geo) return false;
		const auto it = g_geos.find(geo);
		return it != g_geos.end() && it->second.state == State::Live;
	}

	bool capture_may_retain(uintptr_t geo)
	{
		if (!g_active || !geo) return false;
		const auto it = g_geos.find(geo);
		if (it == g_geos.end() || it->second.state == State::Live) return false;
		if (sg::death_pending(geo) || !sg::begin_resolve(geo)) return false;
		ResolveGuard guard;
		const sg::FaultSite site("captureRetain");
		const Place pl = place_of(geo);
		return placed(pl) && !pl.tree && !pl.switch_like && !(pl.o.flags & FNVR_OW_ACTOR);
	}

	void on_owner_gone(uintptr_t ref, uintptr_t root)
	{
		if (!g_active) return;
		static std::vector<uintptr_t> s_gone;
		s_gone.clear();
		for (const auto& [geo, r] : g_geos)
			if (r.state == State::Live && r.ref == ref && r.ref_root == root) s_gone.push_back(geo);
		for (const uintptr_t geo : s_gone)
			if (const auto it = g_geos.find(geo); it != g_geos.end())
			{
				if (traced(it->second)) trace(geo, it->second, "unclaimed: its reference let go of its 3D (owner gone)");
				unclaim(geo, it->second, Cause::Other);
			}
		g_c.owner_gone += s_gone.size();
	}

	void on_cell_refs_loaded(uintptr_t cell)
	{
		if (!g_active) return;
		uint32_t n = 0;
		for (auto& [geo, r] : g_geos)
		{
			if (r.state != State::Refused || r.why != Reason::Terrain || !r.land_unowned) continue;
			r.state = State::Candidate;
			r.why = Reason::Count;
			r.tries = 0;
			r.not_before = 0;
			r.land_unowned = false;
			n++;
		}
		if (n)
			shared::common::log("SceneStatics", std::format("cell 0x{:X} has its references loaded: {} land-shaped "
				"geometries refused as terrain for want of an owner are candidates again", cell, n),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	bool instance_draws_now()
	{
		if (!g_active || !g_claimed) return false;
		const uintptr_t geo = sg::current_geometry();
		if (!geo) return false;
		const auto it = g_geos.find(geo);
		return it != g_geos.end() && it->second.state == State::Live && it->second.visible && it->second.handle &&
			it->second.consume_frame == frame();
	}

	void drop_all(const char* why)
	{
		// No more sightings or verdicts: every pass goes to D3D9.
		sg::set_pass_filter(nullptr);
		std::vector<uint64_t> handles;
		handles.reserve(g_live + g_doomed.size());
		handles.insert(handles.end(), g_doomed.begin(), g_doomed.end());
		for (const auto& [geo, r] : g_geos)
			if (r.state == State::Live && r.handle) handles.push_back(r.handle);
		const uint32_t count = g_claimed;
		if (!handles.empty()) retained::destroy_external(handles.data(), static_cast<uint32_t>(handles.size()));
		if (auto* b = bridge())
		{
			for (auto& [key, m] : g_meshes)
				if (m.handle) b->DestroyMesh(m.handle);
			for (auto& [key, m] : g_mats)
				if (m.handle) b->DestroyMaterial(m.handle);
		}
		for (auto& [key, m] : g_mats)
			for (const uint64_t t : m.textures) textures::release(t);
		clear_all_state();
		retained::set_external_live(0);
		if (count)
			shared::common::log("SceneStatics", std::format("{}: {} scene instance(s) destroyed with their meshes and "
				"materials; statics go through draw capture from the next frame", why ? why : "drop", count),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
	}

	void on_draw_instance_switched()
	{
		if (!g_claimed) return;
		if (cfg().scene_debug_draw_instance)
		{
			// Retained instances go; the claims stay, and the next frame's
			// first render pass draws them.
			std::vector<uint64_t> handles;
			for (auto& [geo, r] : g_geos)
				if (r.state == State::Live && r.handle) { handles.push_back(r.handle); r.handle = 0; }
			if (!handles.empty()) retained::destroy_external(handles.data(), static_cast<uint32_t>(handles.size()));
			g_live = 0;
			retained::set_external_live(0);
			g_draw_instance = true;
		}
		else
		{
			// Nothing draws them until their retained instances exist: their
			// passes reach D3D9 meanwhile (no handle, no skip); the next tick
			// re-creates them.
			g_draw_instance = false;
			g_recreate = true;
		}
	}
}
