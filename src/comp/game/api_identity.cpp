#include "std_include.hpp"
#include "api_identity.hpp"
#include "call_chain.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

// Resource identity for the statics path. See api_identity.hpp.

namespace comp::game::api::identity
{
	namespace
	{
		// IDirect3DResource9 layout: IUnknown (3) + GetDevice .. GetType (8),
		// then Lock. Same slot for vertex and index buffers.
		constexpr unsigned kLockSlot = 11;

		// A buffer write-locked twice within this many frames is being
		// refilled, not recycled (see api_identity.hpp).
		constexpr UINT kRewriteWindow = 300;
		// ... and is declined for this long after its last such refill (a
		// buffer refilled every frame keeps renewing it; an address that was
		// merely recycled twice in quick succession gets its turn again).
		constexpr UINT kRewriteHold = 3600;

		using LockFn = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT offset, UINT size, void** data, DWORD flags);

		struct BufferState
		{
			uint32_t gen = 0;
			bool streamed = false;
			bool rewritten = false;
			UINT rewritten_frame = 0;
			bool built = false;             // a mesh was built from the current contents
			bool built_write_seen = false;  // a built buffer was write-locked before
			UINT built_write_frame = 0;
			// Creation (proxy CreateVertexBuffer / CreateIndexBuffer).
			bool created = false;
			bool recycled = false;
			DWORD usage = 0;
			UINT pool = 0;
			UINT length = 0;
			UINT created_frame = 0;
			// Write locks since creation.
			uint32_t writes = 0, discards = 0, no_overwrites = 0, partial = 0;
			UINT last_write_frame = 0;
			bool relocked = false;
			UINT relock_frame = 0;
		};

		SRWLOCK g_lock = SRWLOCK_INIT;
		std::unordered_map<const void*, BufferState> g_buffers;
		std::unordered_map<const void*, uint32_t> g_textures;
		std::atomic<uint32_t> g_built_epoch { 0 };
		// The built buffers written since the last take_built_written.
		std::vector<const void*> g_built_written;
		bool g_built_overflow = false;
		constexpr size_t kMaxBuiltWritten = 65536;
		Stats g_stats;

		LockFn g_vb_lock = nullptr;       // original VB Lock
		LockFn g_ib_lock = nullptr;       // original IB Lock
		void*  g_vb_target = nullptr;
		void*  g_ib_target = nullptr;
		bool   g_vb_failed = false;
		bool   g_ib_failed = false;

		UINT frame_now() { return shared::common::ffp_state::get().frame_count(); }

		// The built buffer was written (or its address re-created): meshes
		// built from it are stale. Caller holds g_lock exclusively.
		void built_written(const void* p)
		{
			if (g_built_written.size() < kMaxBuiltWritten) g_built_written.push_back(p);
			else g_built_overflow = true;
			g_built_epoch.fetch_add(1, std::memory_order_release);
			g_stats.built_writes++;
		}

		// Runs on whatever thread locks the buffer.
		void note_write(const void* self, UINT offset, UINT size, DWORD flags)
		{
			// Frame counter read racily from a loader thread: only the
			// rewrite window uses it, and a frame of skew there is harmless.
			const UINT now = frame_now();
			AcquireSRWLockExclusive(&g_lock);
			auto& s = g_buffers[self];
			s.gen++;
			g_stats.writes++;
			if (flags & D3DLOCK_DISCARD) { s.discards++; g_stats.discards++; }
			if (flags & D3DLOCK_NOOVERWRITE) { s.no_overwrites++; g_stats.no_overwrites++; }
			if (flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) s.streamed = true;
			if (size && !(offset == 0 && s.created && size >= s.length)) { s.partial++; g_stats.partial++; }
			if (s.writes && now - s.last_write_frame <= kRewriteWindow)
			{
				s.relocked = true;
				s.relock_frame = now;
			}
			s.writes++;
			s.last_write_frame = now;
			if (s.built)
			{
				if (s.built_write_seen && now - s.built_write_frame <= kRewriteWindow)
				{
					s.rewritten = true;
					s.rewritten_frame = now;
				}
				s.built_write_seen = true;
				s.built_write_frame = now;
				s.built = false;
				built_written(self);
			}
			ReleaseSRWLockExclusive(&g_lock);
		}

		HRESULT STDMETHODCALLTYPE vb_lock_hk(void* self, UINT offset, UINT size, void** data, DWORD flags)
		{
			if (!(flags & D3DLOCK_READONLY)) note_write(self, offset, size, flags);
			return g_vb_lock(self, offset, size, data, flags);
		}

		HRESULT STDMETHODCALLTYPE ib_lock_hk(void* self, UINT offset, UINT size, void** data, DWORD flags)
		{
			if (!(flags & D3DLOCK_READONLY)) note_write(self, offset, size, flags);
			return g_ib_lock(self, offset, size, data, flags);
		}

		// Detours the function in slot kLockSlot of `obj`'s vtable, in front of
		// another module's detour where one is found. False on failure.
		bool detour_lock(void* obj, void* hook, LockFn& original, void*& target, const char* what)
		{
			void* fn = (*static_cast<void***>(obj))[kLockSlot];
			if (!fn) return false;
			if (!call_chain::hook_entry(reinterpret_cast<uintptr_t>(fn), hook, reinterpret_cast<void**>(&original),
				"RemixApi", what, "The statics path keeps tracking buffer writes only if that code calls what it "
				"replaced."))
			{
				shared::common::log("RemixApi", std::format(
					"statics: could not detour the bridge's {} Lock @ {} -- the statics path stays off",
					what, fn), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}
			target = fn;
			shared::common::log("RemixApi", std::format(
				"statics: tracking {} write locks (Lock detoured @ {})", what, fn),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			return true;
		}
	}

	// Every traced draw goes through an API path that keys its meshes by
	// buffer pointers + write generations and its materials by texture
	// pointers + creation generations: tracking is always on.
	bool track_buffers(IDirect3DVertexBuffer9* vb, IDirect3DIndexBuffer9* ib)
	{
		if (!g_vb_target && !g_vb_failed && vb)
			g_vb_failed = !detour_lock(vb, reinterpret_cast<void*>(&vb_lock_hk), g_vb_lock, g_vb_target, "vertex buffer");
		if (!g_ib_target && !g_ib_failed && ib)
		{
			// One class could serve both (never so in the bridge client, whose
			// VB and IB are separate LockableBuffer instantiations): the VB
			// detour already records it.
			void* fn = (*reinterpret_cast<void***>(ib))[kLockSlot];
			if (fn && fn == g_vb_target) g_ib_target = fn;
			else g_ib_failed = !detour_lock(ib, reinterpret_cast<void*>(&ib_lock_hk), g_ib_lock, g_ib_target, "index buffer");
		}
		return g_vb_target != nullptr && (!ib || g_ib_target != nullptr);
	}

	void on_buffer_created(const void* p, DWORD usage, UINT pool, UINT length, bool index)
	{
		if (!p) return;
		const UINT now = frame_now();
		AcquireSRWLockExclusive(&g_lock);
		auto [it, inserted] = g_buffers.try_emplace(p);
		BufferState& s = it->second;
		const bool recycled = !inserted;
		if (recycled)
		{
			g_stats.recycled++;
			if (s.streamed) g_stats.recycled_streamed++;
			// A mesh built from the old object is stale (its geometry is
			// normally dead already; the claim check is cheap).
			if (s.built) built_written(p);
		}
		// A new object: nothing of the old record applies. gen goes on
		// counting (mesh keys never meet the old object's).
		const uint32_t gen = s.gen + 1;
		s = BufferState{};
		s.gen = gen;
		s.created = true;
		s.recycled = recycled;
		s.usage = usage;
		s.pool = pool;
		s.length = length;
		s.created_frame = now;
		(index ? g_stats.created_ib : g_stats.created_vb)++;
		if (usage & D3DUSAGE_DYNAMIC) g_stats.created_dynamic++;
		ReleaseSRWLockExclusive(&g_lock);
	}

	BufferInfo buffer(const void* p)
	{
		BufferInfo out;
		if (!p) return out;
		const UINT now = frame_now();
		AcquireSRWLockShared(&g_lock);
		if (const auto it = g_buffers.find(p); it != g_buffers.end())
		{
			const BufferState& s = it->second;
			out.gen = s.gen;
			out.streamed = s.streamed;
			out.rewritten = s.rewritten && now - s.rewritten_frame < kRewriteHold;
			out.relocked = s.relocked && now - s.relock_frame < kRewriteHold;
			out.created = s.created;
			out.recycled = s.recycled;
			out.usage = s.usage;
			out.pool = s.pool;
			out.length = s.length;
			out.dynamic = s.created && (s.usage & D3DUSAGE_DYNAMIC) != 0;
			out.writes = s.writes;
			out.discards = s.discards;
			out.no_overwrites = s.no_overwrites;
			out.partial = s.partial;
			out.since_write = s.writes ? now - s.last_write_frame : UINT_MAX;
			out.since_create = s.created ? now - s.created_frame : UINT_MAX;
		}
		ReleaseSRWLockShared(&g_lock);
		return out;
	}

	bool note_built(const void* p, uint32_t gen)
	{
		if (!p) return true;
		AcquireSRWLockExclusive(&g_lock);
		auto& s = g_buffers[p];
		const bool same = s.gen == gen;
		if (same) s.built = true;
		ReleaseSRWLockExclusive(&g_lock);
		return same;
	}

	void note_built(const void* p)
	{
		if (!p) return;
		AcquireSRWLockExclusive(&g_lock);
		g_buffers[p].built = true;
		ReleaseSRWLockExclusive(&g_lock);
	}

	uint32_t built_write_epoch()
	{
		return g_built_epoch.load(std::memory_order_acquire);
	}

	bool take_built_written(std::vector<const void*>& out)
	{
		out.clear();
		AcquireSRWLockExclusive(&g_lock);
		const bool ok = !g_built_overflow;
		if (ok) out.swap(g_built_written);
		g_built_written.clear();
		g_built_overflow = false;
		ReleaseSRWLockExclusive(&g_lock);
		return ok;
	}

	Stats take_stats()
	{
		AcquireSRWLockExclusive(&g_lock);
		Stats s = g_stats;
		s.tracked = g_buffers.size();
		g_stats = {};
		ReleaseSRWLockExclusive(&g_lock);
		return s;
	}

	void on_texture_created(const void* tex)
	{
		if (!tex) return;
		AcquireSRWLockExclusive(&g_lock);
		g_textures[tex]++;
		ReleaseSRWLockExclusive(&g_lock);
	}

	uint32_t texture_gen(const void* tex)
	{
		if (!tex) return 0;
		AcquireSRWLockShared(&g_lock);
		const auto it = g_textures.find(tex);
		const uint32_t g = it != g_textures.end() ? it->second : 0u;
		ReleaseSRWLockShared(&g_lock);
		return g;
	}
}
