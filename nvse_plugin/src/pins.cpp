#include "common.hpp"
#include "pins.hpp"
#include "monitor.hpp"

namespace fnvr::pins
{
	namespace
	{
		constexpr uint32_t REF_count = 0x04;

		struct Pin
		{
			uintptr_t obj = 0;
			uintptr_t tag = 0;
		};

		struct Pending
		{
			uintptr_t obj = 0;
			uint32_t after = 0;   // released at the first main loop past this frame
		};

		uint64_t key(uintptr_t obj, uintptr_t tag)
		{
			return (static_cast<uint64_t>(tag) << 32) | obj;
		}

		std::unordered_map<uint64_t, Pin> g_pins;
		std::vector<Pending> g_pending;
		volatile LONG g_raw = 0;              // taken by take_raw, not adopted yet
		uint32_t g_released = 0, g_deleted_by_us = 0, g_dropped_bad = 0;
		uint32_t g_last_check = 0;

		bool vtable_ok(uintptr_t obj)
		{
			uint32_t vtbl = 0;
			return guard::plausible(obj) && guard::get(obj, vtbl) && guard::in_image(vtbl);
		}

		// The engine's own release (fnv_exe dec_ref_count), on the main thread.
		void dec_ref(uintptr_t obj)
		{
			if (!vtable_ok(obj))
			{
				g_dropped_bad++;
				log::f("Pins", "0x{:X}: vtable no longer in the image before its release -- dropped without a release "
					"(leaked rather than freed twice)", obj);
				return;
			}
			const uint32_t before = refcount(obj);
			using DecRef = void(__thiscall*)(void*);
			reinterpret_cast<DecRef>(fnv_exe::addr().dec_ref_count)(reinterpret_cast<void*>(obj));
			g_released++;
			if (before == 1) g_deleted_by_us++;
		}
	}

	void take_raw(uintptr_t obj)
	{
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(obj + REF_count));
		InterlockedIncrement(&g_raw);
	}

	void adopt(uintptr_t obj, uintptr_t tag)
	{
		InterlockedDecrement(&g_raw);
		auto [it, fresh] = g_pins.try_emplace(key(obj, tag), Pin{ obj, tag });
		// Already pinned under this tag (the same 3D sent twice): one pin is
		// enough, the extra reference goes back.
		if (!fresh) g_pending.push_back({ obj, monitor::frame() });
	}

	bool pin(uintptr_t obj, uintptr_t tag)
	{
		if (!vtable_ok(obj) || g_pins.count(key(obj, tag))) return false;
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(obj + REF_count));
		g_pins.emplace(key(obj, tag), Pin{ obj, tag });
		return true;
	}

	void release(uintptr_t obj, uintptr_t tag)
	{
		const auto it = g_pins.find(key(obj, tag));
		if (it == g_pins.end()) return;
		g_pins.erase(it);
		g_pending.push_back({ obj, monitor::frame() });
	}

	void release_all(const char* why)
	{
		const uint32_t n = static_cast<uint32_t>(g_pins.size() + g_pending.size());
		for (const auto& p : g_pending) dec_ref(p.obj);
		g_pending.clear();
		for (const auto& [k, p] : g_pins) dec_ref(p.obj);
		g_pins.clear();
		log::f("Pins", "{}: released all {} pins ({} still taken by handlers and not applied), live now {}", why, n,
			static_cast<uint32_t>(g_raw), live());
	}

	void sweep(uint32_t frame)
	{
		size_t kept = 0;
		for (size_t i = 0; i < g_pending.size(); i++)
		{
			if (g_pending[i].after < frame) dec_ref(g_pending[i].obj);
			else g_pending[kept++] = g_pending[i];
		}
		g_pending.resize(kept);

		// The invariant, once per 300 frames: no pinned object lost its vtable.
		if (frame - g_last_check >= 300)
		{
			g_last_check = frame;
			uint32_t bad = 0;
			for (auto it = g_pins.begin(); it != g_pins.end();)
			{
				if (vtable_ok(it->second.obj)) { ++it; continue; }
				bad++;
				log::f("Pins", "invariant: pinned 0x{:X} (tag 0x{:X}) has no vtable in the image -- someone released our "
					"reference; dropped without a release", it->second.obj, it->second.tag);
				it = g_pins.erase(it);
			}
			g_dropped_bad += bad;
		}
	}

	uint32_t refcount(uintptr_t obj)
	{
		uint32_t n = 0;
		return guard::get(obj + REF_count, n) ? n : 0;
	}

	uint32_t live()
	{
		return static_cast<uint32_t>(g_pins.size() + g_pending.size()) + static_cast<uint32_t>(g_raw);
	}
}
