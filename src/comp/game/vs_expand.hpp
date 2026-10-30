#pragma once

namespace shared::common
{
	struct VsDesc;
	struct PsSlotMap;
}

namespace comp::game::api
{
	struct DrawDesc;
}

namespace comp::game::vs_expand
{
	// CPU replays of FNV vertex shaders whose output FFP cannot reproduce.
	//
	// lod_sink does this for one shader (the distant-terrain sink); this module
	// generalises the pattern to every family the VS classifier
	// (shared/common/vs_reflect.hpp) recognises. Each one runs the shader's own
	// arithmetic (checked against shader package 004's disassembly) over a
	// copy of the draw's stream-0 vertex window, and hands the draw that copy
	// instead of the game's buffer:
	//
	//   Particle  NOLIGHT017/019   u' = S.y*u + S.x, v' = S.w*v + S.z with
	//                              S = SubTexOffsets[floor(TEXCOORD1.x)]
	//                              (flipbook cell per particle)
	//   Grass     GRASS20xx/23xXXX I = InstanceData[floor(TEXCOORD1.x)]
	//                              pos = POSITION*(1 + ScaleMask*0.01*I.w) + I.xyz
	//                              (wind dropped; faded-out instances collapsed)
	//   DistLod   DISTLOD2002/2003 pos = POSITION*(0.01*I.w) + I.xyz (+ fade)
	//   DistTree  DISTTREE         P = c[ID + 2i], R = c[ID + 2i + 1]
	//                              pos = Rz(R.xy) * (POSITION*P.w) + P.xyz
	//   Leaf      STLEAF000-003    pos = POSITION + BI.w*(LB.y*Right + LB.z*Up)
	//                              LB = LeafBase[floor(BI.z)], rock/rustle = 0
	//
	// The NOLIGHT TexCoordTranform family (NoLightUv) needs no copy for its UVs:
	// the transform is affine and uniform per draw, so it becomes a stage-0
	// texture matrix (nolight_uv_matrix) instead. Its PS is
	// out = tex * COLOR * MaterialColor, which stage 0 can't express, so a draw
	// whose MaterialColor is not white (a tinted effect, or a particle system
	// the engine is fading) is drawn from a copy with MaterialColor folded into
	// COLOR (D3DCOLOR / FLOAT4 only; the game's UVs kept). For the Remix API
	// (describe_for_api) the copy also has the UV transform baked in.
	//
	// Cost model (every D3D9 call crosses the Remix bridge):
	//   hit   -> zero calls beyond SetStreamSource to bind the cached copy and
	//            SetStreamSource to put the game's buffer back. The source VB
	//            is read through a READONLY lock, which the bridge client
	//            answers from its CPU shadow without IPC.
	//   miss  -> one Lock/Unlock of the cached copy (the upload), plus a
	//            CreateVertexBuffer only when no cached allocation fits.
	// Keys include every constant register the expansion reads and a hash of
	// the source vertices (full for dynamic buffers, sampled for static ones),
	// so a recycled vertex-buffer address or a buffer the game rewrote in
	// place re-expands instead of drawing stale geometry.

	// The expander family for the current draw, or nullptr when there is none:
	// neither the engine's pass type nor the VS's constant names give one, the
	// declaration lacks what the expansion reads, or the PS has no diffuse
	// role (no albedo). The engine's pass type decides where it names the
	// family (engine_class.hpp); the draw is then read with the engine's own
	// registers for that family, whatever the bound VS declares. Cheap: two
	// pointer-keyed map hits and a few flag reads. (A recognised family the
	// declaration doesn't fit is refused by the draw capture: its positions
	// need the expansion.)
	const shared::common::VsDesc* candidate(const shared::common::PsSlotMap* psmap);

	enum class Bind
	{
		Declined,   // nothing bound; the caller draws the game's vertices
		Bound,      // expanded copy bound to stream 0 at offset 0; draw with
		            // the vertex window rebased to 0, then call unbind()
		AllCulled,  // every instance is faded out; the draw can be skipped
	};

	// NoLightUv: Declined (uncounted) unless MaterialColor needs folding; the
	// caller then draws the game's vertices under the texture matrix.
	// Builds (or reuses) the expanded copy of stream-0 vertices
	// [base + skip, base + skip + count) and binds it so that vertex lands at
	// index `skip` of the copy. Only the referenced range is read, hashed and
	// uploaded. For DrawIndexedPrimitive pass base = BaseVertexIndex,
	// skip = MinVertexIndex, count = NumVertices and draw with
	// BaseVertexIndex = 0 (indices unchanged); for DrawPrimitive pass
	// base = StartVertex, skip = 0 and draw from vertex 0.
	Bind bind_expanded(IDirect3DDevice9* dev, const shared::common::VsDesc& desc,
		const shared::common::PsSlotMap* psmap, INT base, UINT skip, UINT count);

	// Restores the game's stream-0 binding (and, after bind_smooth_normals,
	// its vertex declaration) after a Bound draw.
	void unbind(IDirect3DDevice9* dev);

	// ---- Smooth normals for a mesh drawn without a NORMAL (the Pip-Boy screen) ----
	//
	// A NoLightUv declaration carries no NORMAL, so the runtime shades every
	// triangle with its flat face normal; on a glossy low-poly surface the
	// facets show as bands in the reflections. bind_smooth_normals binds a copy
	// of the draw's stream-0 vertex window with a FLOAT3 NORMAL appended to
	// each vertex (stride + 12) and a declaration that is the game's plus that
	// element, instead of the game's buffer. It uses the same window convention
	// as bind_expanded (vertex base + skip at index skip; draw rebased to 0) and
	// is otherwise the game's vertices byte for byte (no MaterialColor fold).
	//
	// The normals: positions welded (tolerance 1e-4 x the window's bounding
	// diagonal), each vertex the normalised, area-weighted sum of the faces at
	// its welded position that lie within kSmoothCreaseDeg of the vertex's own
	// faces, so a gently curved surface reads as one and a bezel or side wall
	// meeting it at a hard edge stays separate. Every face is oriented by its
	// winding (cross(p1 - p0, p2 - p0)); a face wound the other way round is folded in
	// flipped, and each normal points the way its own largest face does.
	//
	// Built once per mesh (key: buffer, window, declaration and a hash of every
	// vertex and index byte), kept in MANAGED; a hit binds with three calls
	// (declaration, stream source) and unbind puts the game's back.
	constexpr float kSmoothCreaseDeg = 50.0f;

	struct SmoothNormalsInfo
	{
		bool built = false;        // this call built the copy (first draw of the mesh)
		UINT verts = 0;            // vertices in the window
		UINT faces = 0;            // non-degenerate triangles
		UINT welded = 0;           // distinct positions after welding
		float max_bend_deg = 0.0f; // largest angle between a vertex normal and one of its own faces
	};

	// Bound, or Declined with `reject` saying why (the caller then draws the
	// game's vertices unchanged). Static buffers only (the window is read
	// through api::read_geometry), at most 4096 vertices.
	Bind bind_smooth_normals(IDirect3DDevice9* dev, const api::DrawDesc& draw, SmoothNormalsInfo& info,
		const char*& reject);

	// ---- Remix-API description of an expansion (api_foliage.cpp, api_particles.cpp, api_unlit.cpp) ----

	struct ApiWindow
	{
		// The draw's identity: source VB, stream offset / stride, vertex
		// window, declaration and VS, not its contents or constants.
		uint64_t slot = 0;
		// What the expanded copy is a pure function of: the slot, the sampled
		// source contents and every constant the expansion reads (the same
		// inputs bind_expanded's cache keys on). 0 for AllCulled.
		uint64_t key = 0;
		IDirect3DVertexBuffer9* src = nullptr;
		UINT stride = 0;
		bool dynamic = false;             // the source VB is D3DUSAGE_DYNAMIC
		int pos_off = -1, pos_type = -1;  // POSITION in the expanded vertices
	};

	// The expansion of the draw's vertex window (every family: Grass, Leaf,
	// DistLod, DistTree, ParticleSubTex, NoLightUv), for a caller that submits
	// it through the Remix API instead of binding a copy (ParticleSubTex: the
	// flipbook cell in TEXCOORD0; NoLightUv: TexCoordTranform baked into
	// TEXCOORD0; both with MaterialColor folded into COLOR clamped to 1, as the
	// D3D9 copy).
	// Same window convention and the same checks as bind_expanded; touches no
	// device state and counts nothing (a declined draw goes through
	// bind_expanded next, which counts and logs it). Returns
	//   Bound      out filled; `expanded` (if given) receives the expanded
	//              window, count * stride bytes, vertex 0 = base + skip;
	//   AllCulled  every instance is faded out (out.slot only);
	//   Declined   reject says why.
	Bind describe_for_api(const shared::common::VsDesc& desc, const shared::common::PsSlotMap* psmap,
		INT base, UINT skip, UINT count, ApiWindow& out, std::vector<uint8_t>* expanded, const char*& reject);

	// The instances of an instanced window (Grass, DistLod, DistTree), for a
	// caller that draws one copy of the model at each instance instead of
	// the expanded copy. The expansion is affine per instance:
	//   pos' = M * POSITION, M = a 3x4 row-major matrix (Remix's
	//   remixapi_Transform layout) in the draw's model space.
	struct ApiInstances
	{
		struct Xform { float m[3][4]; };
		// Per window vertex: the instance it belongs to (an index into
		// `xforms`), -1 for an unaddressable one (the expansion collapses it).
		std::vector<int> vertex_instance;
		std::vector<Xform> xforms;
		// TEXCOORD1 (the instance index) in the source vertices, in bytes:
		// the one element that differs between the instances' copies of
		// the model.
		int tc1_off = -1, tc1_size = 0;
	};

	// Fills `out` for the draw's window when describe_for_api would return
	// Bound for it. False (reject says why) for another family, or when the
	// window can't be read. Same window convention as describe_for_api.
	bool describe_instances(const shared::common::VsDesc& desc, const shared::common::PsSlotMap* psmap,
		INT base, UINT skip, UINT count, ApiInstances& out, const char*& reject);

	// A draw the API path handled (took, or dropped as all-culled): counted
	// on the [VSExpand] status line like a bound one.
	void count_api_draw(const shared::common::VsDesc& desc, bool culled);

	// NoLightUv: the D3D texture matrix equivalent of the VS's
	//   u' = dot(TexCoordTranform[0].xyz, (u, v, 1))
	//   v' = dot(TexCoordTranform[1].xyz, (u, v, 1))
	// Returns false when the transform is the identity, so the caller can leave
	// the stage-0 transform disabled (no bridge traffic, nothing for Remix to
	// apply).
	bool nolight_uv_matrix(const shared::common::VsDesc& desc, D3DMATRIX& out);

	// One-shot "family X reached its FFP route" log line.
	void note_family_used(const shared::common::VsDesc& desc, const char* route);

	// Device reset / shutdown: release every cached buffer.
	void release_cache();

	// Once per frame: ages out entries that haven't been drawn for a while.
	void on_present();
}
