#include "common.hpp"

namespace fnvr::guard
{
	namespace
	{
		constexpr size_t kRegions = 512;

		// A run of committed, readable pages, as VirtualQuery reported it.
		struct Region
		{
			uintptr_t lo, hi;
		};

		// Runs checked this frame, sorted by lo (main thread only; other
		// threads query every time). The owner checks read tens of thousands
		// of words a frame across a few heap runs: one VirtualQuery per run
		// a frame, not per read or per page.
		Region g_regions[kRegions];
		size_t g_count = 0;
		size_t g_last = 0;  // the run the previous lookup hit
		DWORD g_owner_thread = 0;

		volatile LONG g_faults = 0;
		uintptr_t g_image_lo = 0, g_image_hi = 0;

		// The index of the first run whose lo is above p.
		size_t upper(uintptr_t p)
		{
			size_t at = 0, n = g_count;
			while (n)
			{
				const size_t half = n / 2;
				if (g_regions[at + half].lo <= p)
				{
					at += half + 1;
					n -= half + 1;
				}
				else
					n = half;
			}
			return at;
		}

		// The end of the checked run holding p, or 0.
		uintptr_t cached_end(uintptr_t p)
		{
			if (!g_count) return 0;
			if (const Region& r = g_regions[g_last]; p >= r.lo && p < r.hi) return r.hi;
			const size_t at = upper(p);
			if (!at || p >= g_regions[at - 1].hi) return 0;
			g_last = at - 1;
			return g_regions[g_last].hi;
		}

		void remember(uintptr_t lo, uintptr_t hi)
		{
			if (g_count == kRegions) g_count = 0;
			const size_t at = upper(lo);
			std::memmove(&g_regions[at + 1], &g_regions[at], (g_count - at) * sizeof(Region));
			g_regions[at] = { lo, hi };
			g_count++;
			g_last = at;
		}

		// The end of the committed, readable pages from p's page on, or 0.
		uintptr_t query(uintptr_t p, uintptr_t& lo, uintptr_t& alloc_base)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi))) return 0;
			if (mbi.State != MEM_COMMIT) return 0;
			if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
			constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
				PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (!(mbi.Protect & kReadable)) return 0;
			lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			alloc_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
			const uintptr_t hi = lo + mbi.RegionSize;
			return hi > lo ? hi : ~uintptr_t(0);
		}

		// The end of the committed, readable run holding p, or 0. VirtualQuery
		// reports a run from p's page on; when the same run starts at its
		// allocation's base (a heap segment's committed part does), lo goes
		// back to that base, so later reads below p in the run hit the cache.
		uintptr_t query_run(uintptr_t p, uintptr_t& lo)
		{
			uintptr_t base = 0, base_lo = 0, unused = 0;
			const uintptr_t hi = query(p, lo, base);
			if (hi && base && base < lo && query(base, base_lo, unused) == hi) lo = base_lo;
			return hi;
		}

		bool copy(uintptr_t addr, void* out, size_t n)
		{
			__try
			{
				std::memcpy(out, reinterpret_cast<const void*>(addr), n);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				InterlockedIncrement(&g_faults);
				return false;
			}
		}
	}

	bool mapped(uintptr_t addr, size_t n)
	{
		if (!n) return true;
		if (addr < 0x10000 || addr + n < addr) return false;
		const bool cache = GetCurrentThreadId() == g_owner_thread;
		const uintptr_t end = addr + n;
		for (uintptr_t p = addr; p < end;)
		{
			uintptr_t hi = cache ? cached_end(p) : 0;
			if (!hi)
			{
				uintptr_t lo = 0, base = 0;
				hi = cache ? query_run(p, lo) : query(p, lo, base);
				if (!hi) return false;
				if (cache) remember(lo, hi);
			}
			p = hi;
		}
		return true;
	}

	bool read(uintptr_t addr, void* out, size_t n)
	{
		if (!mapped(addr, n)) return false;
		if (copy(addr, out, n)) return true;
		if (GetCurrentThreadId() == g_owner_thread) g_count = 0;
		return false;
	}

	void new_frame()
	{
		g_owner_thread = GetCurrentThreadId();
		g_count = 0;
		g_last = 0;
	}

	bool in_image(uintptr_t p)
	{
		if (!g_image_hi)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			g_image_lo = base;
			g_image_hi = base + nt->OptionalHeader.SizeOfImage;
		}
		return p >= g_image_lo && p < g_image_hi;
	}

	uint32_t faults()
	{
		return static_cast<uint32_t>(g_faults);
	}
}
