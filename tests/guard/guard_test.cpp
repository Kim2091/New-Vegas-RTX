// Standalone test of the plugin's guarded reads (nvse_plugin/src/guard.cpp):
// checks that unmapped, freed, guard and no-access memory is refused without
// a fault, then times a frame of scattered heap reads against one
// VirtualQuery per page. Runs on the CPU only. Built by build_test.bat; not
// part of the shipped DLLs.

#include "common.hpp"

#include <cstdio>
#include <random>
#include <set>

namespace guard = fnvr::guard;

namespace
{
	int g_failed = 0;

	void check(bool ok, const char* what)
	{
		std::printf("%s %s\n", ok ? "  ok  " : "FAILED", what);
		if (!ok) g_failed++;
	}

	double now_us()
	{
		static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return static_cast<double>(t.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
	}

	// A per-page check to time against: one VirtualQuery per page a frame,
	// remembered in a 4096-slot hash set.
	uintptr_t g_pages[4096];

	bool page_query_read(uintptr_t addr, void* out, size_t n)
	{
		for (uintptr_t page = addr & ~uintptr_t(0xFFF); page < addr + n; page += 0x1000)
		{
			const size_t h = static_cast<size_t>((static_cast<uint32_t>(page >> 12) * 2654435761u) >> 20) & 4095;
			bool hit = false;
			size_t free_at = h;
			for (size_t k = 0; k < 8; k++)
			{
				const uintptr_t p = g_pages[(h + k) & 4095];
				if (p == page) { hit = true; break; }
				if (!p) { free_at = (h + k) & 4095; break; }
			}
			if (hit) continue;
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(reinterpret_cast<const void*>(page), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
				return false;
			g_pages[free_at] = page;
		}
		std::memcpy(out, reinterpret_cast<const void*>(addr), n);
		return true;
	}

	DWORD WINAPI other_thread(void* p)
	{
		uint32_t v = 0;
		return guard::get(reinterpret_cast<uintptr_t>(p), v) && v == 0x12345678 ? 1 : 0;
	}
}

int main()
{
	std::printf("guarded reads: safety\n");
	guard::new_frame();
	uint32_t v = 0;
	check(!guard::get(0x1000, v), "a null-page address is refused");
	check(!guard::get(0xFFFF0000u, v), "an address above the user range is refused");

	SYSTEM_INFO si;
	GetSystemInfo(&si);
	auto* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	std::memset(block, 0xAB, 0x4000);
	uint64_t q = 0;
	check(guard::get(reinterpret_cast<uintptr_t>(block + 0x1FFC), q), "a read across two committed pages succeeds");
	DWORD old = 0;
	VirtualProtect(block + 0x3000, 0x1000, PAGE_NOACCESS, &old);
	guard::new_frame();
	check(guard::get(reinterpret_cast<uintptr_t>(block), v), "the readable part of a block succeeds");
	check(!guard::get(reinterpret_cast<uintptr_t>(block + 0x3000), v), "a no-access page is refused");
	check(!guard::get(reinterpret_cast<uintptr_t>(block + 0x2FFE), v), "a read running into a no-access page is refused");
	VirtualProtect(block + 0x2000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
	guard::new_frame();
	check(!guard::get(reinterpret_cast<uintptr_t>(block + 0x2000), v), "a guard page is refused");
	VirtualFree(block, 0, MEM_RELEASE);
	guard::new_frame();
	check(!guard::get(reinterpret_cast<uintptr_t>(block), v), "a block freed before the frame is refused");

	auto* live = static_cast<uint32_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	*live = 0x12345678;
	const HANDLE t = CreateThread(nullptr, 0, other_thread, live, 0, nullptr);
	WaitForSingleObject(t, INFINITE);
	DWORD code = 0;
	GetExitCodeThread(t, &code);
	CloseHandle(t);
	check(code == 1, "another thread reads without the cache");
	VirtualFree(live, 0, MEM_RELEASE);
	check(guard::faults() == 0, "no read faulted");

	// A heap of scattered small objects, like the engine's nodes and
	// references, read in a random order the way the owner checks do.
	std::printf("\nguarded reads: cost\n");
	const HANDLE heap = HeapCreate(0, 0, 0);
	constexpr int kObjects = 60000;
	std::vector<uintptr_t> objs;
	std::mt19937 rng(7);
	for (int i = 0; i < kObjects; i++)
	{
		void* o = HeapAlloc(heap, HEAP_ZERO_MEMORY, 0x40 + (rng() % 0x100));
		objs.push_back(reinterpret_cast<uintptr_t>(o));
	}
	constexpr int kReads = 40000;  // a frame's guarded reads
	std::vector<uintptr_t> order(kReads);
	for (auto& a : order) a = objs[rng() % objs.size()] + 4 * (rng() % 16);

	std::set<uintptr_t> pages, runs;  // counted outside the timing
	for (const uintptr_t a : order)
	{
		pages.insert(a & ~uintptr_t(0xFFF));
		MEMORY_BASIC_INFORMATION mbi;
		VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi));
		runs.insert(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
	}
	std::printf("  a frame: %d reads over %zu pages in %zu committed runs\n", kReads, pages.size(), runs.size());

	constexpr int kFrames = 20;
	uint32_t sink = 0;
	double t0 = now_us();
	for (int f = 0; f < kFrames; f++)
	{
		std::memset(g_pages, 0, sizeof(g_pages));
		for (const uintptr_t a : order)
			if (page_query_read(a, &v, 4)) sink += v;
	}
	const double per_page = (now_us() - t0) / kFrames;

	t0 = now_us();
	for (int f = 0; f < kFrames; f++)
	{
		guard::new_frame();
		for (const uintptr_t a : order)
			if (guard::get(a, v)) sink += v;
	}
	const double per_run = (now_us() - t0) / kFrames;

	t0 = now_us();
	for (int f = 0; f < kFrames; f++)
		for (const uintptr_t a : order)
			if (guard::get(a, v)) sink += v;
	const double warm = (now_us() - t0) / kFrames;
	std::printf("  cache already warm:  %8.1f us a frame (%.1f ns a read)\n", warm, warm * 1000.0 / kReads);

	t0 = now_us();
	for (int f = 0; f < kFrames; f++)
		for (const uintptr_t a : order)
		{
			std::memcpy(&v, reinterpret_cast<const void*>(a), 4);
			sink += v;
		}
	const double plain = (now_us() - t0) / kFrames;

	t0 = now_us();
	constexpr int kQueries = 20000;
	for (int i = 0; i < kQueries; i++)
	{
		MEMORY_BASIC_INFORMATION mbi;
		VirtualQuery(reinterpret_cast<const void*>(order[i % kReads]), &mbi, sizeof(mbi));
		sink += mbi.State;
	}
	const double query_us = (now_us() - t0) / kQueries;

	std::printf("  VirtualQuery: %.3f us a call\n", query_us);
	std::printf("  one query per page:  %8.1f us a frame (%.1f ns a read)\n", per_page, per_page * 1000.0 / kReads);
	std::printf("  one query per run:   %8.1f us a frame (%.1f ns a read)\n", per_run, per_run * 1000.0 / kReads);
	std::printf("  unguarded memcpy:    %8.1f us a frame (%.1f ns a read)\n", plain, plain * 1000.0 / kReads);
	std::printf("  (checksum %u)\n", sink & 1);
	check(per_run < per_page, "the run cache is cheaper than a query per page");
	HeapDestroy(heap);

	std::printf("\n%s\n", g_failed ? "=== guard test FAILED ===" : "=== guard test passed ===");
	return g_failed ? 1 : 0;
}
