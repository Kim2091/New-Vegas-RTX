#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "api_identity.hpp"
#include "api_rtxconf.hpp"
#include "api_material.hpp"
#include "api_textures.hpp"
#include "api_camera.hpp"
#include "api_hair_bake.hpp"
#include "scene_graph.hpp"
#include "stable_origin.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/config.hpp"

/*
 * Skinned actors through the Remix API, captured at draw time. A pass refused
 * below is not drawn (counted per reason).
 *
 * What is taken
 * -------------
 * Draws that reach the renderer's SKINNED route as a pass it would draw (the
 * lighting-only passes are already dropped there: NPC and creature bodies,
 * armour, the first-person arms, and the skinned part of FaceGen heads) and
 * pass every check below.
 *   * bridge  the bridge client has the skinning size fix: without it a
 *             skinned CreateMesh reads ~64 x V^2 bytes past the caller's
 *             arrays and kills the game, so the client module is searched
 *             once for the validation message the fix carries.
 *   * bones   the object's palette was captured (game::bone_rows /
 *             num_bones, by on_set_vs_const_f) and covers every palette slot
 *             the mesh uses.
 *   * shader  the PS classifier knows it: a Diffuse role, one BaseMap, no
 *             Decal / LOD / water sampler. FaceGen PSes too (heads,
 *             bodies, hands): their material carries the FaceGen maps
 *             (material::Desc::facegen0).
 *   * layout  stream 0 only; POSITION float3/4, NORMAL float3, TEXCOORD0
 *             float2 / half2, BLENDWEIGHT FLOAT1-4 / UBYTE4N, BLENDINDICES
 *             D3DCOLOR / UBYTE4.
 *   * state   alpha blending off or plain SrcAlpha / InvSrcAlpha; alpha test
 *             off or GREATER / GREATEREQUAL; colour writes on. FNV leaves
 *             SrcAlpha blending on for most actors; the instance carries it
 *             only while the actor actually fades: the lit SLS / SKIN PSes
 *             output BaseMap.a * AmbientColor.a (SLS2000 / 2017 / 2031
 *             disassembled from shaderpackage002), so AmbientColor.w >= 0.999
 *             means opaque and the blend is dropped (opaqueBlendDropped on
 *             the status line). A PS without AmbientColor keeps the blend.
 *   * buffers not DYNAMIC. A vertex buffer the engine rewrites (a CPU-morphed
 *             FaceGen head, hair, any buffer refilled after a mesh was built
 *             from it or written in the last kSettleWriteFrames frames) is a
 *             morph mesh: keyed by buffers and window alone and refreshed in
 *             place whenever its write generation moves.
 *   * texture the albedo in no rtx.conf list the API can't express (as the
 *             statics).
 * First-person parts carry VIEW_MODEL when [RemixApi] ViewModelCamera sent
 * the frame's view-model camera (api_camera.hpp); otherwise they are ordinary
 * world instances.
 * Shadow maps and other off-screen targets never reach the SKINNED route.
 *
 * Mesh
 * ----
 * One API mesh per skinned partition draw: keyed by the draw's geometry
 * (geo_key: buffers, window, decl layout, sampled contents), the buffers'
 * write generations and the material key, built once from the bridge
 * client's CPU shadow (READONLY locks), exactly like a draw-capture static.
 * Positions / normals are in skin (bind) space, as the vertex buffer holds
 * them. Weights and indices follow FFP indexed vertex blending with
 * D3DVBF_<nw>WEIGHTS, the route every skinned draw takes:
 *   nw = the declaration's explicit weights (FLOAT1 1, FLOAT2 2, FLOAT3 /
 *        FLOAT4 / UBYTE4N 3; ffp_state::cur_decl_num_weights);
 *   matrices nw + 1, the last weight = 1 - sum of the explicit ones;
 *   index k = byte k of BLENDINDICES in memory order (the FFP route draws the
 *        D3DCOLOR element as UBYTE4: no BGRA swizzle).
 * Sent as bonesPerVertex = 4: weights (w0 .. w_{nw-1}, 1 - sum, 0 ..) and
 * indices (i0 .. i_nw, 0 ..). The runtime reads the first three weights and
 * gives the fourth the remainder (skinning.h), which reproduces the list
 * above for every nw (for nw < 3 the remainder it computes is exactly 0).
 * Slots with a zero weight get index 0, so the mesh's highest used slot is
 * known and checked against the palette size at every draw.
 *
 * Bone space
 * ----------
 * The engine uploads bone n as three constants c[44+3n .. 44+3n+2]. The
 * skinning VS evaluates p' = sum_k w_k * (dot(c[3i_k+0], p1), dot(c[3i_k+1],
 * p1), dot(c[3i_k+2], p1)) with p1 = (p, 1): each register is one ROW of a 3x4
 * matrix applied to a COLUMN vector. game.cpp's FFP path stores the transpose
 * (D3DMATRIX _11 = c0.x, _21 = c0.y, _31 = c0.z, _41 = c0.w), which the
 * row-vector FFP pipeline applies as p1 * M: the same map. Its output feeds
 * the renderer's VIEW (no translation: FNV renders camera-relative), so the
 * map is skin space -> camera-relative world, game units: the same frame as
 * the NiDX9Renderer WORLD every other API path starts from.
 * remixapi_Transform is row-major 3x4 applied to column vectors
 * (out_r = m[r][0] x + m[r][1] y + m[r][2] z + m[r][3]; rtx_remix_api.cpp
 * tomat4 builds the column-major Matrix4 from it, and the skinning shader
 * does mul(bone, p)), so m[r][c] = c[3n+r][c] with NO transpose. No engine
 * NiTransforms are composed here: the matrices are the ones the engine's own
 * shader and the FFP route consume.
 * The frame Remix renders in is camera-relative + D (stable origin; D = 0
 * when it is off): the D3D9 route shifts each WORLDMATRIX(n) by W * T(D),
 * i.e. translation += D. With the instance transform T(A) (A = anchor, below)
 * each bone is sent as T(-A) * T(D) * B_n: linear part unchanged,
 * translation t_n + D - A. The runtime skins in object space and applies the
 * instance transform: A + (R_n p + t_n + D - A) = R_n p + t_n + D, the D3D9
 * route's position exactly. Normals take R_n in both routes. Bind pose proves
 * nothing (R_n = I there under any convention); a walking NPC does.
 *
 * Anchor: the runtime's identity hash includes the bone hash, so matching to
 * last frame falls back to the nearest instance of the same mesh. The instance translation is palette slot 0's translation (in render
 * space) rounded to 1/16 unit: per actor, near the actor's origin (a
 * skin->world bone maps the skin origin close to the actor's root), exact in
 * float, and the bones carry the small rest. Two NPCs in the same armour sit
 * at different anchors, so their motion histories don't swap.
 *
 * Per frame
 * ---------
 * One DrawInstance per taken draw, with InstanceInfoBoneTransformsEXT (the
 * palette, <= MAX_FFP_BONES) and InstanceInfoBlendEXT when the D3D9 route's
 * stage 0 would modulate by vertex colour or the draw blends.
 *
 * Held poses: the runtime refits an instance's BLAS whenever its bone hash
 * changes, and the engine's camera-relative bones change in their low bits
 * whenever only the camera moves. A pose within kHoldLinear (rotation and
 * scale entries) and kHoldTranslation (game units, render space) of the
 * pose last sent for the same mesh and NiGeometry goes out as that pose,
 * bit for bit, with its anchor. A real motion leaves the tolerance within a
 * frame or two and is sent; the held pose is at most kHoldTranslation off.
 * Nothing is retained: the runtime never keeps a skinned retained entry
 * steady, and the batched retained update can't carry bones. The D3D9 draw is skipped only
 * when DrawInstance returned success. Meshes and materials undrawn for
 * EvictFrames are destroyed.
 *
 * One primary per object (pass arbitration)
 * -----------------------------------------
 * Gamebryo draws one skinned partition in several passes a frame. Outdoors
 * that is mostly one lit pass (SLS2031 ...); indoors it is an ambient pass
 * (SLS1000: DiffuseMap, output rgb = AmbientColor), additive per-light passes
 * with no diffuse (dropped by the renderer: SKINNED_SKIP_LIGHT), and a
 * DestColor x Zero multiply by the diffuse (SLS1003; SLS1005 with Decal
 * maps). On the D3D9 route every one of those is a draw of the same
 * geometry; the runtime's instance manager matches the later ones to the
 * first draw's instance (same BLAS, same frame: "the same instance, being
 * drawn twice", mergeInstanceHeuristics) and keeps ONE instance with the
 * first draw's material. The API path does the same here: a second, coplanar
 * instance with a DestColor blend would z-fight the first and shade black.
 *
 * So every pass is attributed to an OBJECT: the draw's VB, IB, index / vertex
 * window and bone palette (two NPCs in the same armour share buffers but not
 * palettes); its PARTITION is the same without the palette (stable across
 * frames). Per object per frame:
 *   * The first pass the API takes CLAIMS the object. Every later pass of it
 *     that frame creates no RT instance and is dropped: a raster-lighting
 *     pass (additive dst=ONE, multiply DestColor / Zero x Zero / SrcColor,
 *     colour writes off), a repeat of the claiming PS + texture (folded), or
 *     any other extra pass. Path tracing replaces what those passes add, as
 *     the runtime's merge does on the D3D9 route.
 *   * A pass the API refuses is not drawn; a later pass of the object may
 *     still claim it.
 * Pass order: an extra pass may come before the pass the API would take. A
 * refused pass whose reason is the pass itself (shader, blend, alphafunc,
 * colorwrite) is counted as an extra (predicted) when the API claimed its
 * partition last frame (or earlier this frame). orphaned counts objects whose
 * predicted extras were dropped but no primary followed that frame; mixed
 * counts objects with an API instance and a refused pass in one frame.
 */

namespace comp::game::api_submit
{
	using namespace comp::game::api;
	using shared::common::PsSlotMap;
	using shared::common::PsSlotRole;

	namespace
	{
		constexpr uint64_t kSkinnedDomain = 0x534B494E4E454431ull;   // "SKINNED1"
		constexpr UINT kRetryFrames = 120;
		// A vertex buffer written this recently is still being filled (or is
		// rewritten every frame, e.g. a CPU-morphed head): a morph mesh.
		constexpr UINT kSettleWriteFrames = 30;
		constexpr uint32_t kBonesPerVertex = 4;
		// Anchor grid: 1/16 game unit (exact in float for any render-space
		// coordinate the stable origin leaves).
		constexpr double kAnchorSteps = 16.0;
		// Held poses (file comment). The translation tolerance covers the
		// rounding of a camera-relative float a few thousand units out
		// (ulp(4096) = 1/2048); 1/256 unit is about 0.06 mm.
		constexpr float kHoldLinear = 1.0e-5f;
		constexpr double kHoldTranslation = 1.0 / 256.0;
		// Held poses not sent for this many frames are forgotten.
		constexpr UINT kHoldForgetFrames = 120;

		// ---- Fallback reasons ------------------------------------------------------

		enum class Reason : uint8_t
		{
			Bridge,      // the bridge lacks the skinning size fix
			Inactive,    // API not initialised, or buffer tracking unavailable
			NoBones,     // no palette captured for this object
			Shader,
			Layout,
			Blend,       // blending other than SrcAlpha / InvSrcAlpha
			AlphaFunc,
			ColorWrite,
			Streamed,    // DYNAMIC buffer
			RtxConf,
			Texture,
			Budget,
			Build,
			Retry,
			Palette,     // the mesh uses a slot the captured palette lacks
			Bones,       // non-finite bone
			Submit,      // DrawInstance failed
			Count
		};

		constexpr const char* kReasonNames[] = {
			"bridge", "inactive", "nobones", "shader", "layout", "blend", "alphafunc", "colorwrite", "streamed",
			"rtxconf", "texture", "budget", "build", "retry", "palette", "bones", "submit",
		};
		static_assert(std::size(kReasonNames) == static_cast<size_t>(Reason::Count));

		// Refusals that describe the PASS (its shader or state), not the
		// object: another pass of the same object can still be the API's.
		bool pass_reason(Reason r)
		{
			return r == Reason::Shader || r == Reason::Blend || r == Reason::AlphaFunc || r == Reason::ColorWrite;
		}

		struct Counters
		{
			uint32_t instances = 0, instances_last = 0;        // DrawInstance calls
			uint32_t folded = 0, folded_last = 0;              // repeats of the claiming pass, dropped
			uint32_t suppressed = 0, suppressed_last = 0;      // D3D9 draws skipped (taken passes)
			uint32_t extra_skipped = 0, extra_skipped_last = 0;  // extra passes of an API object, dropped
			uint32_t predicted = 0, predicted_last = 0;        // of those: decided on last frame's claim
			uint32_t mixed = 0, mixed_last = 0;                // objects with an API instance and a refused pass
			uint64_t mixed_total = 0;                          // since start, so a rare one isn't lost
			uint32_t orphaned = 0, orphaned_last = 0;          // predicted extras without a primary that frame
			uint32_t opaque_blend = 0, opaque_blend_last = 0;  // SrcAlpha blend on, AmbientColor.a = 1: dropped
			uint32_t bones_max = 0, bones_max_last = 0;        // bones per instance, max
			uint64_t bones_sum = 0, bones_sum_last = 0;
			uint32_t first_person = 0, first_person_last = 0;  // instances drawn with the viewmodel projection
			uint32_t heads = 0, heads_last = 0;                // FaceGen instances
			uint32_t heads_refreshed = 0, heads_refreshed_last = 0;   // morph meshes re-sent (VB rewritten: heads, refilled buffers)
			uint32_t heads_refused = 0, heads_refused_last = 0;
			uint32_t blended = 0, blended_last = 0;            // instances with the draw's alpha blend
			uint32_t held = 0, held_last = 0;                  // instances sent with the held pose
			uint32_t meshes_built = 0, meshes_built_last = 0;
			uint32_t materials_built = 0, materials_built_last = 0;
			uint32_t fallback[static_cast<size_t>(Reason::Count)] = {};
			uint32_t fallback_last[static_cast<size_t>(Reason::Count)] = {};
		};
		Counters g_count;

		// ---- Caches ----------------------------------------------------------------

		struct Material
		{
			remixapi_MaterialHandle handle = nullptr;
			std::vector<uint64_t> textures;   // references held on api::textures
			uint32_t refs = 0;                // meshes using it
			UINT last_used = 0;
			bool declined = false;
			Reason why = Reason::Build;
			UINT retry_frame = 0xFFFFFFFFu;
			bool no_vertex_color = false;
			// rtx.conf lists the API expresses as instance categories.
			uint32_t categories = 0;
			bool alpha_dropped = false;       // ignoreAlphaOnTextures: no alpha state at all
		};
		std::unordered_map<uint64_t, Material> g_materials;

		struct SkinMesh
		{
			remixapi_MeshHandle mesh = nullptr;
			uint64_t material_key = 0;
			bool vertex_color = false;
			// Hair: the PS's tint baked into the vertex colours (material::hair_tint),
			// over the part above 1 that the material carries.
			bool hair = false;
			float hair_tint[3] = {};
			float hair_scale[3] = { 1.0f, 1.0f, 1.0f };
			int max_slot = 0;                 // highest palette slot any vertex uses
			UINT verts = 0, tris = 0;
			uint32_t ps_hash = 0;
			UINT last_frame = 0;
			// A morph mesh (a FaceGen head, or any rewritten VB): keyed without
			// its vertex contents, and refreshed in place (same hash,
			// RefreshGeometryEXT) whenever the game rewrites its VB (CPU-morphed
			// expressions and lip sync).
			bool morph = false;
			uint64_t hash = 0;                // the mesh's API hash (kept for refreshes)
			uint32_t vb_gen = 0;              // VB write generation the mesh holds
		};
		std::unordered_map<uint64_t, SkinMesh> g_meshes;
		std::unordered_map<uint64_t, UINT> g_retry_at;   // mesh key -> first frame to retry

		// The pose last sent per mesh and NiGeometry (held poses, file comment).
		struct HeldPose
		{
			double anchor[3] = {};
			remixapi_Transform bones[MAX_FFP_BONES];
			int count = 0;
			UINT frame = 0;
		};
		std::unordered_map<uint64_t, HeldPose> g_poses;

		bool g_logged_first = false;
		std::unordered_set<uint64_t> g_logged_decisions;   // (PS, decision) pairs logged

		// ---- Pass arbitration (see "One primary per object") -------------------

		// This frame's passes of one object (VB, IB, window, palette).
		struct ObjectFrame
		{
			bool api = false;          // the API claimed it (a DrawInstance went out)
			bool refused = false;      // a pass was refused (not drawn)
			bool predicted = false;    // an extra was dropped on last frame's claim
			uint32_t claim_ps = 0;     // the claiming pass: PS and diffuse texture
			const void* claim_tex = nullptr;
		};
		std::unordered_map<uint64_t, ObjectFrame> g_objects;   // cleared at Present
		// Partition (object without the palette) -> last frame the API
		// claimed one of its objects.
		std::unordered_map<uint64_t, UINT> g_part_claim;

		// The reason of the last refusal of evaluate().
		Reason g_last_reason = Reason::Inactive;

		// ---- Bridge capability -------------------------------------------------------

		// -1 not checked yet, 0 the client lacks the fix, 1 present.
		int g_bridge_fix = -1;

		// A bridge client with the skinning size fix sizes the skinning arrays
		// by their totals; one without it reads ~64 x V^2 bytes past them (an
		// access violation in FalloutNV.exe on the first skinned CreateMesh).
		// The fix carries a CreateMesh validation message, so its text in the
		// client image identifies a client that has it. Checked once.
		bool bridge_has_skinning_fix()
		{
			if (g_bridge_fix < 0)
			{
				static constexpr char kMarker[] = "Invalid skinning on surface";
				const bool ok = bridge_client_has(kMarker);
				g_bridge_fix = ok ? 1 : 0;
				shared::common::log("RemixApi", ok
					? "skinned: the bridge client has the skinning size fix -- skinned meshes go through the API"
					: "skinned: the bridge client LACKS the skinning size fix: a skinned "
					  "CreateMesh would crash the game, so skinned actors are not drawn -- deploy the rebuilt bridge "
					  "(client AND server)",
					ok ? shared::common::LOG_TYPE::LOG_TYPE_GREEN : shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			return g_bridge_fix == 1;
		}

		// ---- Small helpers -------------------------------------------------------

		bool fallback(Reason r, FeatureStats& st, bool head)
		{
			g_count.fallback[static_cast<size_t>(r)]++;
			if (head) g_count.heads_refused++;
			st.fallbacks++;
			st.last_reject = kReasonNames[static_cast<size_t>(r)];
			return false;
		}

		// evaluate()'s refusal: recorded, counted by the caller only when the
		// pass is really refused (not an extra of a claimed object).
		bool refused(Reason r)
		{
			g_last_reason = r;
			return false;
		}

		DWORD render_state(IDirect3DDevice9* dev, D3DRENDERSTATETYPE rs, DWORD fallback_value)
		{
			DWORD v = fallback_value;
			if (FAILED(dev->GetRenderState(rs, &v))) return fallback_value;
			return v;
		}

		// Logs each distinct (PS, decision) once, at most 512 lines: the PS, the
		// draw's blend / colour-write state and what was decided. `site`
		// identifies the decision by its inputs, so the per-pass path only
		// hashes and probes the set; `make` formats the text only for a
		// decision that is logged.
		template<typename MakeText>
		void log_decision(IDirect3DDevice9* dev, uint32_t ps, uint64_t site, MakeText&& make)
		{
			if (g_logged_decisions.size() >= 512) return;
			Hash64 h;
			h.mix(ps);
			h.mix(site);
			if (!g_logged_decisions.insert(h.get()).second) return;
			const std::string decision = make();
			shared::common::log("RemixApi", std::format("skinned: PS 0x{:08X} blend={} src={} dst={} colorWrite=0x{:X} -> {}",
				ps, render_state(dev, D3DRS_ALPHABLENDENABLE, FALSE), render_state(dev, D3DRS_SRCBLEND, D3DBLEND_ONE),
				render_state(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO), render_state(dev, D3DRS_COLORWRITEENABLE, 0xF),
				decision), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		// A pass that only adds raster lighting on top of what is already in
		// the target (the path tracer replaces it): additive, multiplicative,
		// or no colour output at all. `what` names it for the log.
		bool raster_lighting_pass(IDirect3DDevice9* dev, const char*& what)
		{
			if (!render_state(dev, D3DRS_COLORWRITEENABLE, 0xF)) { what = "no colour writes"; return true; }
			if (!render_state(dev, D3DRS_ALPHABLENDENABLE, FALSE)) { what = "opaque"; return false; }
			const DWORD src = render_state(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
			const DWORD dst = render_state(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
			if (dst == D3DBLEND_ONE) { what = "additive"; return true; }
			if ((src == D3DBLEND_DESTCOLOR || src == D3DBLEND_ZERO) && (dst == D3DBLEND_ZERO || dst == D3DBLEND_SRCCOLOR))
			{
				what = "multiply";
				return true;
			}
			what = "blended";
			return false;
		}

		// Object / partition identity of the current draw. False when it has
		// none (no palette, not indexed): such a draw can't be taken anyway.
		struct ObjectId
		{
			uint64_t part = 0, object = 0;
		};

		bool object_id(IDirect3DDevice9* dev, const DrawDesc& d, ObjectId& id)
		{
			const int nb = game::num_bones;
			if (!d.indexed || nb <= 0 || nb > MAX_FFP_BONES) return false;
			IDirect3DVertexBuffer9* vb = shared::common::ffp_state::get().stream_vb(0);
			if (!vb) return false;
			IDirect3DIndexBuffer9* ib = nullptr;
			if (FAILED(dev->GetIndices(&ib)) || !ib) return false;
			Hash64 h;
			h.mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)));
			h.mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ib)));
			ib->Release();   // compared by address only
			h.mix(static_cast<uint64_t>(static_cast<uint32_t>(d.base)));
			h.mix((static_cast<uint64_t>(d.min_vtx) << 32) | d.num_vtx);
			h.mix((static_cast<uint64_t>(d.start_index) << 32) | d.prim_count);
			id.part = h.get();
			h.add(game::bone_rows, static_cast<size_t>(nb) * sizeof(game::bone_rows[0]));
			id.object = h.get();
			return true;
		}

		uint8_t unorm8(float v)
		{
			if (!(v > 0.0f)) return 0;
			return v >= 1.0f ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f);
		}

		// ---- Layout ----------------------------------------------------------------

		struct Layout
		{
			Attr pos, nrm, uv, col, bw, bi;
			int nw = 0;                       // explicit weights (FFP D3DVBF_<nw>WEIGHTS)
		};

		bool skinned_layout(Layout& l)
		{
			const auto& ffp = shared::common::ffp_state::get();
			if (ffp.cur_decl_multi_stream() || ffp.cur_decl_has_pos_t() || !ffp.cur_decl_is_skinned()) return false;
			l.pos = find_attr(D3DDECLUSAGE_POSITION, 0);
			l.nrm = find_attr(D3DDECLUSAGE_NORMAL, 0);
			l.uv  = find_attr(D3DDECLUSAGE_TEXCOORD, 0);
			l.col = find_attr(D3DDECLUSAGE_COLOR, 0);
			l.bw  = find_attr(D3DDECLUSAGE_BLENDWEIGHT, 0);
			l.bi  = find_attr(D3DDECLUSAGE_BLENDINDICES, 0);
			if (!l.pos.ok() || (l.pos.type != D3DDECLTYPE_FLOAT3 && l.pos.type != D3DDECLTYPE_FLOAT4)) return false;
			if (!l.nrm.ok() || l.nrm.type != D3DDECLTYPE_FLOAT3) return false;
			if (!l.uv.ok() || (l.uv.type != D3DDECLTYPE_FLOAT2 && l.uv.type != D3DDECLTYPE_FLOAT16_2)) return false;
			if (!l.bw.ok() || !((l.bw.type >= D3DDECLTYPE_FLOAT1 && l.bw.type <= D3DDECLTYPE_FLOAT4) ||
				l.bw.type == D3DDECLTYPE_UBYTE4N))
				return false;
			if (!l.bi.ok() || (l.bi.type != D3DDECLTYPE_D3DCOLOR && l.bi.type != D3DDECLTYPE_UBYTE4)) return false;
			const UINT stride = ffp.stream_stride(0);
			if (!attr_fits(l.pos, stride) || !attr_fits(l.nrm, stride) || !attr_fits(l.uv, stride) ||
				!attr_fits(l.bw, stride) || !attr_fits(l.bi, stride))
				return false;
			if (l.col.ok() && !attr_fits(l.col, stride)) l.col = {};
			// Same count the FFP route programs into D3DRS_VERTEXBLEND.
			l.nw = std::clamp(ffp.cur_decl_num_weights(), 1, 3);
			return true;
		}

		// A refused skinned declaration, once per layout (at most 16): every
		// stream element's usage / type / offset, so a layout the gate doesn't
		// know is visible in remix-comp.log.
		void log_layout_once()
		{
			static std::unordered_set<uint64_t> s_logged;
			if (s_logged.size() >= 16 || !s_logged.insert(decl_hash()).second) return;
			const auto& ffp = shared::common::ffp_state::get();
			std::string els;
			for (UINT i = 0; i < ffp.cur_decl_num_elements(); i++)
			{
				const auto& e = ffp.cur_decl_elements()[i];
				els += std::format("{}s{}+{} u{}.{} t{}", els.empty() ? "" : ", ", e.Stream, e.Offset, e.Usage,
					e.UsageIndex, e.Type);
			}
			shared::common::log("RemixApi", std::format("skinned: declaration refused (layout), stride {}: {}",
				ffp.stream_stride(0), els), shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		}

		// ---- Builds ----------------------------------------------------------------

		void build_material(const material::Desc& md, uint64_t key, Material& m)
		{
			PROFILE_ZONE_N("api::skinned::build_material");
			material::Built b;
			material::build(md, key, kSkinnedDomain ^ 0x4D4154ull, b);
			m.handle = b.handle;
			m.textures = std::move(b.textures);
			m.no_vertex_color = b.no_vertex_color;
			m.categories = b.categories;
			m.alpha_dropped = b.alpha_dropped;
			switch (b.why)
			{
			case material::Decline::None:    g_count.materials_built++; return;
			case material::Decline::RtxConf: m.declined = true; m.why = Reason::RtxConf; return;
			case material::Decline::Texture: m.declined = true; m.why = Reason::Texture; return;
			case material::Decline::Build:   m.declined = true; m.why = Reason::Build; return;
			}
		}

		bool build_mesh(IDirect3DDevice9* dev, const DrawDesc& d, const Layout& L, uint64_t key,
			const Material& mat, SkinMesh& sm, const char*& reject)
		{
			PROFILE_ZONE_N("api::skinned::build_mesh");
			// sm.mesh set: a refresh of a morph mesh in place (same hash, same
			// topology), the runtime refits its BLAS and keeps its history.
			const bool refresh = sm.mesh != nullptr;
			Geometry g;
			if (!read_geometry(dev, d, g, reject)) return false;
			if (refresh && (g.count != sm.verts || g.tris.size() / 3 != sm.tris))
			{
				reject = "morph refresh changed the vertex / triangle count";
				return false;
			}

			std::vector<remixapi_HardcodedVertex> verts(g.count);
			std::vector<float> weights(static_cast<size_t>(g.count) * kBonesPerVertex, 0.0f);
			std::vector<uint32_t> indices(static_cast<size_t>(g.count) * kBonesPerVertex, 0u);
			int max_slot = 0;
			for (UINT i = 0; i < g.count; i++)
			{
				const uint8_t* p = g.verts.data() + static_cast<size_t>(i) * g.stride;
				auto& o = verts[i];
				o = {};
				for (int k = 0; k < 3; k++) o.position[k] = read_comp(p + L.pos.off, L.pos.type, k);
				if (!std::isfinite(o.position[0]) || !std::isfinite(o.position[1]) || !std::isfinite(o.position[2]))
				{
					reject = "non-finite POSITION";
					return false;
				}
				float n[3];
				for (int k = 0; k < 3; k++) n[k] = read_comp(p + L.nrm.off, L.nrm.type, k);
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (std::isfinite(len) && len > 1e-6f) { o.normal[0] = n[0] / len; o.normal[1] = n[1] / len; o.normal[2] = n[2] / len; }
				else { o.normal[0] = 0.0f; o.normal[1] = 0.0f; o.normal[2] = 1.0f; }
				o.texcoord[0] = read_comp(p + L.uv.off, L.uv.type, 0);
				o.texcoord[1] = read_comp(p + L.uv.off, L.uv.type, 1);
				if (sm.hair)
				{
					// The hair PS's factor (material::hair_color).
					const float green = L.col.type == D3DDECLTYPE_D3DCOLOR ? p[L.col.off + 1] / 255.0f
						: read_comp(p + L.col.off, L.col.type, 1);
					o.color = material::hair_color(green, sm.hair_tint, sm.hair_scale);
				}
				else if (sm.vertex_color)
				{
					if (L.col.type == D3DDECLTYPE_D3DCOLOR) std::memcpy(&o.color, p + L.col.off, sizeof(o.color));
					else o.color = D3DCOLOR_ARGB(unorm8(read_comp(p + L.col.off, L.col.type, 3)),
						unorm8(read_comp(p + L.col.off, L.col.type, 0)), unorm8(read_comp(p + L.col.off, L.col.type, 1)),
						unorm8(read_comp(p + L.col.off, L.col.type, 2)));
				}
				else o.color = 0xFFFFFFFFu;

				// FFP indexed vertex blending with nw explicit weights: nw + 1
				// matrices, the last weight the remainder; index k = byte k.
				float* w = &weights[static_cast<size_t>(i) * kBonesPerVertex];
				uint32_t* ix = &indices[static_cast<size_t>(i) * kBonesPerVertex];
				const uint8_t* bi = p + L.bi.off;
				float sum = 0.0f;
				for (int k = 0; k < L.nw; k++)
				{
					float wk = read_comp(p + L.bw.off, L.bw.type, k);
					if (!std::isfinite(wk)) { reject = "non-finite BLENDWEIGHT"; return false; }
					w[k] = wk;
					sum += wk;
				}
				w[L.nw] = 1.0f - sum;
				for (int k = 0; k <= L.nw; k++)
				{
					if (w[k] == 0.0f) continue;   // index 0: never read with any effect
					ix[k] = bi[k];
					max_slot = (std::max)(max_slot, static_cast<int>(bi[k]));
				}
			}
			if (max_slot >= MAX_FFP_BONES) { reject = "palette slot beyond the FFP bone limit"; return false; }

			remixapi_MeshInfoSurfaceTriangles surf = {};
			surf.vertices_values = verts.data();
			surf.vertices_count = static_cast<uint64_t>(verts.size());
			surf.indices_values = g.tris.data();
			surf.indices_count = static_cast<uint64_t>(g.tris.size());
			// Counts are the array totals, bonesPerVertex x vertices
			// (remix_c.h; a client with the fix checks exactly that).
			surf.skinning_hasvalue = 1;
			surf.skinning_value.bonesPerVertex = kBonesPerVertex;
			surf.skinning_value.blendWeights_values = weights.data();
			surf.skinning_value.blendWeights_count = static_cast<uint32_t>(weights.size());
			surf.skinning_value.blendIndices_values = indices.data();
			surf.skinning_value.blendIndices_count = static_cast<uint32_t>(indices.size());
			surf.material = mat.handle;

			remixapi_MeshInfoRefreshGeometryEXT rg = {};
			rg.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT;
			remixapi_MeshInfo mi = {};
			mi.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mi.pNext = refresh ? &rg : nullptr;
			mi.hash = refresh ? sm.hash : next_object_hash(key, kSkinnedDomain);
			mi.surfaces_values = &surf;
			mi.surfaces_count = 1;
			// A refresh keeps the key the mesh was created with (the bind pose).
			remixapi_AssetKeyEXT ak = {};
			if (!refresh) asset_keys::chain(mi, ak, asset_keys::of(verts, g.tris));

			auto* b = bridge();
			remixapi_MeshHandle h = nullptr;
			if (!b || b->CreateMesh(&mi, &h) != REMIXAPI_ERROR_CODE_SUCCESS || !h)
			{
				reject = refresh ? "CreateMesh (refresh) failed" : "CreateMesh failed";
				return false;
			}
			sm.mesh = h;
			sm.hash = mi.hash;
			sm.max_slot = max_slot;
			sm.verts = g.count;
			sm.tris = static_cast<UINT>(g.tris.size() / 3);
			return true;
		}

		void release_material(uint64_t key)
		{
			const auto it = g_materials.find(key);
			if (it == g_materials.end() || it->second.refs == 0) return;
			it->second.refs--;
			it->second.last_used = frame();
		}

		void destroy_mesh(SkinMesh& sm)
		{
			if (sm.mesh) if (auto* b = bridge()) b->DestroyMesh(sm.mesh);
			sm.mesh = nullptr;
			release_material(sm.material_key);
		}

		// ---- Submission --------------------------------------------------------------

		// This frame's DrawInstance of `sm` with the captured palette. False:
		// nothing reached the runtime (the pass is refused).
		bool submit(IDirect3DDevice9* dev, SkinMesh& sm, const Material& mat, bool blend, uint8_t alpha_func,
			uint8_t alpha_ref, uint32_t decal_categories, int nb, Reason& why)
		{
			auto* b = bridge();
			if (!b || !sm.mesh) { why = Reason::Inactive; return false; }

			// Render space = camera-relative + D (see the file comment).
			static const double kZero[3] = { 0.0, 0.0, 0.0 };
			const double* dshift = stable_origin::active() ? stable_origin::shift() : kZero;

			double anchor[3];
			for (int r = 0; r < 3; r++)
				anchor[r] = std::round((static_cast<double>(game::bone_rows[0][r * 4 + 3]) + dshift[r]) * kAnchorSteps) /
					kAnchorSteps;

			remixapi_Transform bones[MAX_FFP_BONES];
			for (int i = 0; i < nb; i++)
			{
				const float* c = game::bone_rows[i];
				auto& m = bones[i].matrix;
				for (int r = 0; r < 3; r++)
				{
					// Row r of the bone is register c[3i + r]: no transpose.
					m[r][0] = c[r * 4 + 0];
					m[r][1] = c[r * 4 + 1];
					m[r][2] = c[r * 4 + 2];
					m[r][3] = static_cast<float>(static_cast<double>(c[r * 4 + 3]) + dshift[r] - anchor[r]);
					if (!std::isfinite(m[r][0]) || !std::isfinite(m[r][1]) || !std::isfinite(m[r][2]) ||
						!std::isfinite(m[r][3]))
					{
						why = Reason::Bones;
						return false;
					}
				}
			}

			// The held pose, when this one is within the tolerances of it.
			Hash64 pk;
			pk.mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sm.mesh)));
			if (const uintptr_t geo = scene_graph::current_geometry()) pk.mix(static_cast<uint64_t>(geo));
			else for (int r = 0; r < 3; r++) pk.mix(static_cast<uint64_t>(static_cast<int64_t>(anchor[r] * kAnchorSteps)));
			HeldPose& hp = g_poses[pk.get()];
			bool same = hp.count == nb;
			for (int i = 0; same && i < nb; i++)
			{
				const auto& a = bones[i].matrix;
				const auto& h = hp.bones[i].matrix;
				for (int r = 0; same && r < 3; r++)
				{
					same = std::fabs(a[r][0] - h[r][0]) <= kHoldLinear && std::fabs(a[r][1] - h[r][1]) <= kHoldLinear &&
						std::fabs(a[r][2] - h[r][2]) <= kHoldLinear &&
						std::fabs((static_cast<double>(a[r][3]) + anchor[r]) - (static_cast<double>(h[r][3]) + hp.anchor[r])) <=
							kHoldTranslation;
				}
			}
			if (same)
			{
				std::memcpy(bones, hp.bones, static_cast<size_t>(nb) * sizeof(bones[0]));
				for (int r = 0; r < 3; r++) anchor[r] = hp.anchor[r];
				g_count.held++;
			}
			else
			{
				std::memcpy(hp.bones, bones, static_cast<size_t>(nb) * sizeof(bones[0]));
				for (int r = 0; r < 3; r++) hp.anchor[r] = anchor[r];
				hp.count = nb;
			}
			hp.frame = frame();

			// Stage 0 of a skinned draw: COLOR = TEXTURE * vertex colour,
			// ALPHA = TEXTURE.
			remixapi_InstanceInfoBlendEXT blend_ext;
			const bool drop_alpha = mat.alpha_dropped;
			bool use_blend_ext = false;
			if (blend && !drop_alpha)
			{
				blend_ext = material::alpha_blend(alpha_func, alpha_ref, false);
				use_blend_ext = true;
			}
			else if (sm.vertex_color)
			{
				blend_ext = material::vertex_color_blend();
				use_blend_ext = true;
			}

			remixapi_InstanceInfoBoneTransformsEXT bones_ext = {};
			bones_ext.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
			bones_ext.pNext = use_blend_ext ? &blend_ext : nullptr;
			bones_ext.boneTransforms_values = bones;
			bones_ext.boneTransforms_count = static_cast<uint32_t>(nb);

			remixapi_InstanceInfo ii = {};
			ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
			ii.pNext = &bones_ext;
			// First-person arms: VIEW_MODEL, with the frame's view-model camera
			// ([RemixApi] ViewModelCamera; 0 when off or not first-person).
			ii.categoryFlags = mat.categories | decal_categories | api_camera::view_model_categories(dev);
			ii.mesh = sm.mesh;
			// The anchor in render units. The bones stay in FNV units: the
			// instance transform scales the skinned mesh as a whole.
			const double s = stable_origin::scale();
			ii.transform = {};
			for (int r = 0; r < 3; r++)
			{
				ii.transform.matrix[r][r] = static_cast<float>(s);
				ii.transform.matrix[r][3] = static_cast<float>(anchor[r] * s);
			}
			ii.doubleSided = double_sided_for_cull(dev) ? 1 : 0;
			if (b->DrawInstance(&ii) != REMIXAPI_ERROR_CODE_SUCCESS)
			{
				why = Reason::Submit;
				return false;
			}
			return true;
		}
	}

	// ---- Evaluation --------------------------------------------------------------------

	namespace
	{
		// Every gate, the builds, and (unless `dry`) this frame's DrawInstance.
		// True: submitted (dry: everything passed, it would be submitted).
		// False: refused, g_last_reason says why; nothing is counted as a
		// fallback here, the caller decides what the pass becomes. `blend`
		// reports whether the instance carries the draw's alpha blend.
		bool evaluate(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d, bool head, bool dry, bool& blend)
		{
			auto& st = skinned_stats();
			const auto& ffp = shared::common::ffp_state::get();
			blend = false;

			if (!bridge()) return refused(Reason::Inactive);
			if (!bridge_has_skinning_fix()) return refused(Reason::Bridge);

			const int nb = game::num_bones;
			if (nb <= 0 || nb > MAX_FFP_BONES) return refused(Reason::NoBones);

			// Shader: the draws the FFP route rebinds to a single BaseMap.
			if (!psmap || !psmap->any_classified || !psmap->has(PsSlotRole::Diffuse) || psmap->base_map_count > 1 ||
				psmap->multi_layer_count || psmap->has(PsSlotRole::Decal) || psmap->has_lod_sampler ||
				psmap->has_water_sampler || psmap->is_water_surface)
				return refused(Reason::Shader);

			// Without the protocol the runtime would give the D3D9 draw its
			// albedo alone: so does the API material.
			const auto& rc = rtxconf::scalars();
			const bool normal_role = rc.protocol_enabled && psmap->has(PsSlotRole::Normal);

			Layout L;
			if (!skinned_layout(L))
			{
				log_layout_once();
				return refused(Reason::Layout);
			}

			// Render state.
			material::Desc md;
			bool decal = false;
			if (render_state(dev, D3DRS_ALPHABLENDENABLE, FALSE))
			{
				// An actor fading in / out, or hair: plain alpha blending only.
				if (render_state(dev, D3DRS_SRCBLEND, D3DBLEND_ONE) != D3DBLEND_SRCALPHA ||
					render_state(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO) != D3DBLEND_INVSRCALPHA)
					return refused(Reason::Blend);
				md.blend = true;
				// FNV leaves the blend on for opaque actors too. The lit PSes
				// output BaseMap.a * AmbientColor.a: at 1 the actor isn't fading,
				// and a blended instance would only cost tracing (file comment).
				// Hair keeps its blend: its strands are the BaseMap's alpha.
				if (psmap->ambient_color_reg != PsSlotMap::kNoReg && psmap->hair_tint_reg == PsSlotMap::kNoReg)
				{
					const float a = ffp.ps_const_data()[psmap->ambient_color_reg * 4 + 3];
					if (std::isfinite(a) && a >= 0.999f)
					{
						// A decal layer keeps its blend and goes out as a decal.
						if (material::named_decal_layer(scene_graph::current_geometry()))
							decal = true;
						else
						{
							md.blend = false;
							if (!dry) g_count.opaque_blend++;
						}
					}
				}
			}
			if (!render_state(dev, D3DRS_COLORWRITEENABLE, 0xF)) return refused(Reason::ColorWrite);
			if (render_state(dev, D3DRS_ALPHATESTENABLE, FALSE))
			{
				const DWORD func = render_state(dev, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
				if (func == D3DCMP_GREATER || func == D3DCMP_GREATEREQUAL)
				{
					md.alpha_func = static_cast<uint8_t>(func);
					md.alpha_ref = static_cast<uint8_t>(render_state(dev, D3DRS_ALPHAREF, 0) & 0xFF);
				}
				else if (func != D3DCMP_ALWAYS) return refused(Reason::AlphaFunc);
			}

			// Buffers.
			IDirect3DVertexBuffer9* vb = ffp.stream_vb(0);
			IDirect3DIndexBuffer9* ib = nullptr;
			if (!d.indexed || FAILED(dev->GetIndices(&ib)) || !ib) return refused(Reason::Layout);
			const bool tracked = identity::track_buffers(vb, ib);
			const identity::BufferInfo vbi = identity::buffer(vb);
			const identity::BufferInfo ibi = identity::buffer(ib);
			ib->Release();   // only compared by address from here on
			if (!tracked) return refused(Reason::Inactive);
			if (vbi.dynamic || ibi.dynamic) return refused(Reason::Streamed);
			// A FaceGen head's VB is rewritten whenever its expression changes,
			// and other skinned VBs are refilled now and then (hair, CPU-morphed
			// parts, a buffer still being filled): such a mesh is keyed by the
			// buffers and window alone and refreshed in place below whenever the
			// VB's write generation moves. The IB generation is in every key: a
			// rewritten IB is a new mesh.
			const bool morph = head || vbi.rewritten || vbi.since_write < kSettleWriteFrames;

			const char* reject = nullptr;
			Hash64 gk;
			if (morph)
			{
				gk.mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)));
				gk.mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ib)));
				gk.mix(static_cast<uint64_t>(static_cast<uint32_t>(d.base)));
				gk.mix((static_cast<uint64_t>(d.min_vtx) << 32) | d.num_vtx);
				gk.mix((static_cast<uint64_t>(d.start_index) << 32) | d.prim_count);
				gk.mix((static_cast<uint64_t>(ffp.stream_stride(0)) << 32) | ibi.gen);
				gk.mix(decl_hash());
				gk.mix(0x4D4F525048ull);   // "MORPH"
			}
			else
			{
				uint64_t geo = 0;
				if (!geo_key(dev, d, geo, reject)) return refused(Reason::Layout);   // also DYNAMIC buffers
				gk.mix(geo);
				gk.mix(vbi.gen);
				gk.mix(ibi.gen);
			}
			gk.mix(static_cast<uint64_t>(L.nw));
			const uint64_t geometry = gk.get();

			// Material: what the FFP route's protocol gives the same draw through D3D9
			// (apply_ps_protocol, pack height off on the skinned route).
			const float* pc = ffp.ps_const_data();
			md.diffuse = ffp.cur_texture(psmap->slot(PsSlotRole::Diffuse));
			if (!md.diffuse) return refused(Reason::Texture);
			md.normal_role = normal_role;
			const bool lit_spec = psmap->toggles_reg != PsSlotMap::kNoReg && psmap->base_map_count <= 1 &&
				rc.normal_alpha_mode == 1;
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
			if (rc.protocol_enabled && psmap->has(PsSlotRole::Glow))
			{
				md.glow = ffp.cur_texture(psmap->slot(PsSlotRole::Glow));
				if (md.glow && psmap->emittance_reg != PsSlotMap::kNoReg)
				{
					const float* e = pc + psmap->emittance_reg * 4;
					if (unorm8(e[0]) < 254 || unorm8(e[1]) < 254 || unorm8(e[2]) < 254)
					{
						// A coloured tint rides on the material only while the
						// runtime tints API emissive textures (glow_tint_ready);
						// otherwise no glow rather than a white one.
						if (!material::glow_tint_ready()) md.glow = nullptr;
						else for (int k = 0; k < 3; k++) md.glow_tint[k] = static_cast<float>(unorm8(e[k])) / 255.0f;
					}
				}
			}
			md.height = rc.protocol_enabled && psmap->has(PsSlotRole::Height)
				? ffp.cur_texture(psmap->slot(PsSlotRole::Height)) : nullptr;
			// A FaceGen head's per-NPC detail and tint (material::Desc::facegen0).
			// Without float uploads the head keeps its BaseMap alone.
			if (head && psmap->facegen0_slot != PsSlotMap::kNoSlot && psmap->facegen1_slot != PsSlotMap::kNoSlot &&
				textures::half_float_supported())
			{
				md.facegen0 = ffp.cur_texture(psmap->facegen0_slot);
				md.facegen1 = ffp.cur_texture(psmap->facegen1_slot);
				if (!md.facegen0 || !md.facegen1) md.facegen0 = md.facegen1 = nullptr;
			}
			// The albedo's own sampler stage (not stage 0): addressing and filter;
			// the geometry's own Texture Clamp Mode overrides the addressing
			// (exact shade property class; else the stage's stays).
			material::sampler_from_stage(dev, psmap->slot(PsSlotRole::Diffuse), md);
			material::clamp_from_geometry(scene_graph::current_geometry(), md);
			// Hair: a light HairTint brightens the texture up to 2x, past what a
			// vertex colour holds; the material carries max(1, 2 HairTint) and the
			// vertex colours the rest (material::hair_color).
			float hair_tint[3] = {};
			float hair_scale[3] = { 1.0f, 1.0f, 1.0f };
			const bool hair = L.col.ok() && material::hair_tint(psmap, hair_tint);
			if (hair)
			{
				material::hair_scale(hair_tint, hair_scale);
				material::hair_material_tint(hair_tint, md.albedo_tint);
			}
			if (hair && psmap->hair_layer_slot != PsSlotMap::kNoSlot)
			{
				md.hair_layer = ffp.cur_texture(psmap->hair_layer_slot);
				// [RemixApi] ShaderBakeHair: the albedo baked from the PS, once ready.
				md.hair_bake = hair_bake::albedo(psmap, ffp.last_ps(), md.diffuse, md.hair_layer);
				if (md.hair_bake) md.hair_layer = nullptr;
			}
			md.path = "skinned";
			const uint64_t mkey = material::key(md);

			const UINT now = frame();
			auto mit = g_materials.find(mkey);
			if (mit != g_materials.end() && mit->second.declined && mit->second.refs == 0 && now >= mit->second.retry_frame)
			{
				g_materials.erase(mit);
				mit = g_materials.end();
			}
			// First-person arms are never throttled: they must not wait a frame
			// for MaxBuildsPerFrame.
			const bool first_person = game::uses_viewmodel_projection();
			if (mit == g_materials.end())
			{
				if (!first_person && !take_build_budget(mkey)) return refused(Reason::Budget);
				Material m;
				build_material(md, mkey, m);
				if (m.declined)
				{
					for (const uint64_t t : m.textures) textures::release(t);
					m.textures.clear();
					if (m.handle) { if (auto* b = bridge()) b->DestroyMaterial(m.handle); m.handle = nullptr; }
					if (m.why == Reason::Texture || m.why == Reason::Build) m.retry_frame = now + kRetryFrames;
				}
				m.last_used = now;
				mit = g_materials.emplace(mkey, std::move(m)).first;
			}
			Material& mat = mit->second;
			mat.last_used = now;
			if (mat.declined) return refused(mat.why);

			// Hair: each NPC's colour is its own mesh.
			Hash64 kh;
			kh.mix(geometry);
			kh.mix(mkey);
			if (hair)
				for (const float t : hair_tint) kh.mix(static_cast<uint64_t>(t * 4096.0f) | 0x48414952ull << 32);
			const uint64_t key = kh.get();

			auto sit = g_meshes.find(key);
			if (sit == g_meshes.end())
			{
				const auto rt = g_retry_at.find(key);
				if (rt != g_retry_at.end() && now < rt->second) return refused(Reason::Retry);
				if (!first_person && !take_build_budget(key, d.num_vtx)) return refused(Reason::Budget);

				SkinMesh sm;
				sm.morph = morph;
				sm.vb_gen = vbi.gen;
				sm.material_key = mkey;
				sm.hair = hair;
				std::memcpy(sm.hair_tint, hair_tint, sizeof(hair_tint));
				std::memcpy(sm.hair_scale, hair_scale, sizeof(hair_scale));
				sm.vertex_color = hair || (L.col.ok() && !mat.no_vertex_color);
				sm.ps_hash = psmap->ps_hash;
				if (hair) material::log_hair("skinned", psmap->ps_hash, nullptr, hair_tint);
				if (!build_mesh(dev, d, L, key, mat, sm, reject))
				{
					destroy_mesh(sm);
					g_retry_at[key] = now + kRetryFrames;
					note_failure(st, reject, std::format("PS 0x{:08X}, {} verts, {} prims", psmap->ps_hash, d.num_vtx, d.prim_count));
					return refused(Reason::Build);
				}
				// A morph VB is rewritten on purpose: not a built buffer (its writes
				// would bump identity's built-write epoch every frame).
				if (!morph) identity::note_built(vb);
				identity::note_built(ib);   // compared by address only
				mat.refs++;
				g_count.meshes_built++;
				sit = g_meshes.emplace(key, std::move(sm)).first;
				st.created++;
				st.live = static_cast<uint32_t>(g_meshes.size());
				note_success(st, std::format("PS 0x{:08X}, {} verts, {} tris, {} weights + remainder, max slot {}, palette {}{}",
					psmap->ps_hash, sit->second.verts, sit->second.tris, L.nw, sit->second.max_slot, nb,
					head ? " (FaceGen head)" : ""));
			}

			SkinMesh& sm = sit->second;
			sm.last_frame = now;
			if (sm.max_slot >= nb) return refused(Reason::Palette);

			blend = md.blend;
			if (dry) return true;

			if (sm.morph && sm.vb_gen != vbi.gen)
			{
				// The game rewrote the VB since the mesh was sent: this frame's
				// vertices, same hash (BLAS refit, motion history kept).
				if (!build_mesh(dev, d, L, key, mat, sm, reject))
				{
					note_failure(st, reject, std::format("PS 0x{:08X} morph refresh, {} verts", psmap->ps_hash, d.num_vtx));
					return refused(Reason::Build);
				}
				sm.vb_gen = vbi.gen;
				g_count.heads_refreshed++;
			}

			Reason why = Reason::Submit;
			const uint32_t decal_categories = decal ? static_cast<uint32_t>(REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC) : 0u;
			if (!submit(dev, sm, mat, md.blend, md.alpha_func, md.alpha_ref, decal_categories, nb, why))
				return refused(why);

			g_count.instances++;
			st.draws++;
			g_count.bones_max = (std::max)(g_count.bones_max, static_cast<uint32_t>(nb));
			g_count.bones_sum += static_cast<uint64_t>(nb);
			if (head) g_count.heads++;
			if (md.blend) g_count.blended++;
			if (game::uses_viewmodel_projection()) g_count.first_person++;

			if (!g_logged_first)
			{
				g_logged_first = true;
				shared::common::log("RemixApi", std::format("skinned: first DrawInstance with {} bones (anchor from palette "
					"slot 0) -- mesh {} verts", nb, sm.verts), shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
			}
			return true;
		}
	}

	// ---- Entry points ----------------------------------------------------------------

	SkinnedPass try_skinned(IDirect3DDevice9* dev, const PsSlotMap* psmap, const DrawDesc& d, const char*& why)
	{
		PROFILE_ZONE_N("api::try_skinned");
		why = nullptr;
		auto& st = skinned_stats();
		const bool head = engine_class::facegen_head(psmap);
		const uint32_t ps = psmap ? psmap->ps_hash : 0u;
		const auto& ffp = shared::common::ffp_state::get();
		const void* tex = psmap ? ffp.cur_texture(psmap->slot(PsSlotRole::Diffuse)) : nullptr;
		const UINT now = frame();
		bool blend = false;

		// The pass is refused: not drawn.
		const auto refuse = [&](Reason r, ObjectFrame* o) {
			why = kReasonNames[static_cast<size_t>(r)];
			fallback(r, st, head);
			if (o) o->refused = true;
			log_decision(dev, ps, 0x100u | static_cast<uint64_t>(r),
				[&] { return std::format("refused ({})", kReasonNames[static_cast<size_t>(r)]); });
			return SkinnedPass::Refused;
		};

		// A pass of an object the API claims: no second RT instance.
		const auto extra = [&](ObjectFrame& o, bool is_predicted) {
			if (is_predicted) { o.predicted = true; g_count.predicted++; }
			st.skipped++;
			const char* what = "";
			const bool lighting = raster_lighting_pass(dev, what);
			if (!is_predicted && ps == o.claim_ps && tex == o.claim_tex && !lighting)
			{
				g_count.folded++;
				log_decision(dev, ps, 0x200u,
					[] { return std::string("extra pass, same PS and texture as the API instance: folded (dropped)"); });
				return SkinnedPass::Skip;
			}
			g_count.extra_skipped++;
			log_decision(dev, ps, 0x300u ^ (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(what)) << 8) ^ (lighting ? 1u : 0u) ^ (is_predicted ? 2u : 0u),
				[&] { return std::format("extra pass ({}{}): dropped{}", what, lighting ? ", raster lighting" : "",
					is_predicted ? " -- on last frame's API claim" : ""); });
			return SkinnedPass::Skip;
		};

		ObjectId id;
		if (!object_id(dev, d, id))
		{
			// No palette / not indexed: evaluate refuses it anyway.
			if (!evaluate(dev, psmap, d, head, true, blend)) return refuse(g_last_reason, nullptr);
			return refuse(Reason::NoBones, nullptr);
		}
		ObjectFrame& o = g_objects[id.object];

		// Claimed earlier this frame: every further pass is an extra.
		if (o.api) return extra(o, false);

		// The first pass the API takes claims the object.
		if (evaluate(dev, psmap, d, head, false, blend))
		{
			o.api = true;
			o.claim_ps = ps;
			o.claim_tex = tex;
			g_part_claim[id.part] = now;
			log_decision(dev, ps, 0x500u ^ (head ? 1u : 0u) ^ (blend ? 2u : 0u),
				[&] { return std::format("API instance{}{}", head ? " (FaceGen head)" : "",
					blend ? " (alpha-blended: fading)" : ""); });
			g_count.suppressed++;
			return SkinnedPass::Taken;
		}

		// Refused. A refusal of the pass itself, on a partition the API
		// claimed last frame (or this frame): an extra drawn before the pass
		// the API takes (dropped either way; counted as an extra).
		const Reason r = g_last_reason;
		if (pass_reason(r))
		{
			const auto pc = g_part_claim.find(id.part);
			if (pc != g_part_claim.end() && now - pc->second <= 1) return extra(o, true);
		}
		return refuse(r, &o);
	}

	void skinned_on_present()
	{
		auto& st = skinned_stats();
		const UINT keep = static_cast<UINT>(shared::common::config::get().remix_api.evict_frames);
		const UINT now = frame();

		for (auto it = g_meshes.begin(); it != g_meshes.end();)
		{
			if (now - it->second.last_frame > keep)
			{
				destroy_mesh(it->second);
				st.evicted++;
				it = g_meshes.erase(it);
			}
			else ++it;
		}
		st.live = static_cast<uint32_t>(g_meshes.size());

		for (auto it = g_materials.begin(); it != g_materials.end();)
		{
			Material& m = it->second;
			if (m.refs == 0 && now - m.last_used > keep)
			{
				if (m.handle) { if (auto* b = bridge()) b->DestroyMaterial(m.handle); }
				for (const uint64_t t : m.textures) textures::release(t);
				it = g_materials.erase(it);
			}
			else ++it;
		}

		for (auto it = g_retry_at.begin(); it != g_retry_at.end();)
		{
			if (now >= it->second) it = g_retry_at.erase(it);
			else ++it;
		}

		if ((now & 63u) == 0)
			std::erase_if(g_poses, [now](const auto& p) { return now - p.second.frame > kHoldForgetFrames; });

		// This frame's objects: the invariant, then a fresh frame.
		for (const auto& [key, o] : g_objects)
		{
			if (o.api && o.refused) { g_count.mixed++; g_count.mixed_total++; }
			if (o.predicted && !o.api && !o.refused) g_count.orphaned++;
		}
		g_objects.clear();
		for (auto it = g_part_claim.begin(); it != g_part_claim.end();)
		{
			if (now - it->second > 2) it = g_part_claim.erase(it);
			else ++it;
		}

		auto& c = g_count;
		c.instances_last = std::exchange(c.instances, 0u);
		c.folded_last = std::exchange(c.folded, 0u);
		c.suppressed_last = std::exchange(c.suppressed, 0u);
		c.extra_skipped_last = std::exchange(c.extra_skipped, 0u);
		c.predicted_last = std::exchange(c.predicted, 0u);
		c.mixed_last = std::exchange(c.mixed, 0u);
		c.orphaned_last = std::exchange(c.orphaned, 0u);
		c.opaque_blend_last = std::exchange(c.opaque_blend, 0u);
		c.bones_max_last = std::exchange(c.bones_max, 0u);
		c.bones_sum_last = std::exchange(c.bones_sum, 0ull);
		c.first_person_last = std::exchange(c.first_person, 0u);
		c.heads_last = std::exchange(c.heads, 0u);
		c.heads_refused_last = std::exchange(c.heads_refused, 0u);
		c.heads_refreshed_last = std::exchange(c.heads_refreshed, 0u);
		c.held_last = std::exchange(c.held, 0u);
		c.blended_last = std::exchange(c.blended, 0u);
		c.meshes_built_last = std::exchange(c.meshes_built, 0u);
		c.materials_built_last = std::exchange(c.materials_built, 0u);
		std::memcpy(c.fallback_last, c.fallback, sizeof(c.fallback));
		std::memset(c.fallback, 0, sizeof(c.fallback));
	}

	void skinned_on_reset()
	{
		// API objects live in the runtime and survive a device reset.
		g_retry_at.clear();
		g_objects.clear();
		g_part_claim.clear();
	}

	void skinned_drop_all()
	{
		auto& st = skinned_stats();
		for (auto& [key, sm] : g_meshes)
		{
			destroy_mesh(sm);
			st.evicted++;
		}
		g_meshes.clear();
		for (auto& [key, m] : g_materials)
		{
			if (m.handle) { if (auto* b = bridge()) b->DestroyMaterial(m.handle); }
			for (const uint64_t t : m.textures) textures::release(t);
		}
		g_materials.clear();
		g_retry_at.clear();
		g_objects.clear();
		g_part_claim.clear();
		g_poses.clear();
		st.live = 0;
	}

	std::string skinned_status()
	{
		const auto& c = g_count;
		const auto& s = skinned_stats();
		std::string reasons;
		uint32_t total = 0;
		for (size_t i = 0; i < static_cast<size_t>(Reason::Count); i++)
		{
			if (!c.fallback_last[i]) continue;
			total += c.fallback_last[i];
			reasons += std::format("{}{}={}", reasons.empty() ? "" : " ", kReasonNames[i], c.fallback_last[i]);
		}
		const double bones_avg = c.instances_last ? static_cast<double>(c.bones_sum_last) / c.instances_last : 0.0;
		return std::format("skinned: instances={} suppressed={} folded={} extraSkipped={} "
			"(predicted {}) mixed={} (total {}) orphaned={} meshes={} materials={} built={}+{}mat "
			"bones/instance max={} avg={:.1f} posesHeld={} firstPerson={} heads={} (refused {}, morph refreshes {}) blended={} "
			"opaqueBlendDropped={} refused={}{} evicted={} failed={} bridgeFix={}",
			c.instances_last, c.suppressed_last, c.folded_last, c.extra_skipped_last,
			c.predicted_last, c.mixed_last, c.mixed_total, c.orphaned_last, s.live, g_materials.size(), c.meshes_built_last,
			c.materials_built_last, c.bones_max_last, bones_avg, c.held_last, c.first_person_last, c.heads_last,
			c.heads_refused_last, c.heads_refreshed_last, c.blended_last, c.opaque_blend_last, total,
			reasons.empty() ? std::string() : std::format(" ({})", reasons),
			s.evicted, s.failed, g_bridge_fix < 0 ? "unchecked" : g_bridge_fix ? "yes" : "NO");
	}
}
