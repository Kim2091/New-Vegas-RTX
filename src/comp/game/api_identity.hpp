#pragma once

// Resource identity for the Remix-API paths (draw capture, api_statics.cpp;
// [RemixApi] SceneGraphStatics, scene_statics.cpp; the skinned, foliage and
// unlit paths).
//
// The statics cache keys a mesh by the game's vertex / index buffer pointers
// and a material by its texture pointers, and keeps both for many frames. A
// pointer alone can't be trusted for that long:
//   * a buffer can be rewritten in place (a "static" buffer the engine
//     refills, or one it streams into with DISCARD / NOOVERWRITE);
//   * a buffer or texture can be released and its address handed to a new
//     object with different contents.
//
// Buffers: the proxy never wraps IDirect3DVertexBuffer9 / IndexBuffer9 (the
// game holds the bridge client's objects directly), so it doesn't see Lock
// or Release. It does see creation: the proxy device's CreateVertexBuffer /
// CreateIndexBuffer call on_buffer_created(), which starts the address's
// record afresh (a new object: its usage, pool and size, no lock history),
// so a static buffer created at an address a streamed buffer freed does not
// inherit its `streamed` flag.
//
// track_buffers() detours the bridge client's Lock methods once (MinHook,
// vtable slot 11 of each class) and records every write lock:
//   * gen       bumped by every write lock and by every creation at the
//               address (never reset: a mesh keyed by (pointer, gen) never
//               matches a different object or different contents);
//   * streamed  a DISCARD or NOOVERWRITE lock was seen since the buffer was
//               created (sticky for the object's life; the strict rule);
//   * discards / no_overwrites / writes, first / last write frame: what
//               the relaxed rule (SceneStreamedRelaxed) looks at: a buffer
//               filled once (with whatever flags) and left alone is
//               immutable, one written again soon after is not;
//   * relocked  a write lock came within kRewriteWindow frames of the
//               previous one; reported for kRewriteHold frames after;
//   * rewritten a write lock hit a buffer a mesh had been built from, a
//               second time within kRewriteWindow frames: the buffer is
//               refilled repeatedly, so a cached copy of it can't be kept.
//               Reported for kRewriteHold frames after the last such
//               refill. A single refill of a built buffer (a recycled
//               address, a one-off update) only bumps gen.
// Any write lock of a buffer a mesh was built from also bumps
// built_write_epoch() and is listed for take_built_written(): the scene
// statics compare the claimed windows' bytes with their meshes' and keep the
// claim when the engine refilled the same bytes (scene_statics.cpp
// verify_written). Draw capture keys its meshes by content and never marks
// a buffer built.
// READONLY locks (the wrapper's own reads among them) are ignored.
//
// Textures: the proxy does see CreateTexture; on_texture_created() bumps a
// per-address generation, so a material keyed by (pointer, generation)
// never matches a different texture at a recycled address.
//
// The Lock detours are installed by the first API draw that asks for them
// (track_buffers).
// Creations are recorded from the start (a map entry per address).
//
// Thread safety: the engine may lock buffers and create textures on its
// loader threads. All state is behind one SRW lock.

namespace comp::game::api::identity
{
	struct BufferInfo
	{
		uint32_t gen = 0;
		bool streamed = false;       // DISCARD / NOOVERWRITE since creation (sticky)
		bool rewritten = false;
		bool relocked = false;       // written again within kRewriteWindow (held kRewriteHold)
		bool created = false;        // the proxy saw its creation: usage / pool / length are known
		bool dynamic = false;        // D3DUSAGE_DYNAMIC at creation
		bool recycled = false;       // created at an address another buffer had
		DWORD usage = 0;
		UINT pool = 0;
		UINT length = 0;
		uint32_t writes = 0, discards = 0, no_overwrites = 0, partial = 0;
		UINT since_write = UINT_MAX;  // frames since the last write lock (UINT_MAX: none)
		UINT since_create = UINT_MAX; // frames since creation (UINT_MAX: not seen)
	};

	// Installs the Lock detours on first use, from live objects of each
	// class (either may be null; the IB class is hooked once one is seen).
	// False while the VB detour isn't in place: the caller then declines
	// the draw.
	bool track_buffers(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib);

	// Proxy CreateVertexBuffer / CreateIndexBuffer (after success): a new
	// object at this address.
	void on_buffer_created(const void* p, DWORD usage, UINT pool, UINT length, bool index);

	// What was recorded for this buffer (all zero if neither created nor
	// write-locked since tracking began).
	BufferInfo buffer(const void* p);

	// A mesh was built from the buffer's contents at generation `gen`: a
	// later write lock counts towards `rewritten` and bumps
	// built_write_epoch(). False if the buffer was written since
	// (gen moved on): the mesh read may have seen the new contents.
	bool note_built(const void* p, uint32_t gen);
	void note_built(const void* p);   // at whatever generation it is now

	// Bumped by every write lock of a buffer a mesh was built from (and by
	// a creation at such a buffer's address). A consumer that sees it move
	// compares the generations its claims were built at with buffer().gen.
	uint32_t built_write_epoch();

	// The buffers behind those bumps since the last call (repeats possible),
	// so the consumer looks only at the claims reading them. False when more
	// piled up than are kept (nobody took them for long): `out` is then
	// empty and the consumer checks every claim.
	bool take_built_written(std::vector<const void*>& out);

	// Counters since the last call (status line).
	struct Stats
	{
		uint64_t created_vb = 0, created_ib = 0, recycled = 0, recycled_streamed = 0, created_dynamic = 0;
		uint64_t writes = 0, discards = 0, no_overwrites = 0, partial = 0, built_writes = 0;
		size_t tracked = 0;
	};
	Stats take_stats();

	// Proxy CreateTexture / CreateCubeTexture / CreateVolumeTexture.
	void on_texture_created(const void* tex);

	// Creation generation of the texture at this address (0 if never seen).
	uint32_t texture_gen(const void* tex);
}
