#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>

// Guarded reads of engine memory. A read first checks that every page it
// touches is committed and readable (mapped(), so no access violation is
// raised for a crash handler such as NVAC's to see), then copies under SEH.
// Nothing here writes engine memory.

namespace fnvr::guard
{
	// Every page of [addr, addr + n) is committed and readable.
	bool mapped(uintptr_t addr, size_t n);

	// Copies n bytes from addr. False (out untouched or partial) when the
	// range is not mapped or the copy faulted.
	bool read(uintptr_t addr, void* out, size_t n);

	template <class T>
	bool get(uintptr_t addr, T& out)
	{
		return read(addr, &out, sizeof(T));
	}

	// The main thread's cache of checked runs (one VirtualQuery result each)
	// is dropped once per main-loop frame and after a read that faults: memory
	// freed since is not trusted past the frame it was checked in (a read in it
	// still runs under SEH).
	void new_frame();

	// The highest address this process can use: 0x7FFEFFFF normally, and close to 4 GB for a
	// large-address-aware game (the 4GB patch) on 64-bit Windows, whose heap does go above 2 GB.
	inline uintptr_t user_address_limit()
	{
		static const uintptr_t limit = [] {
			SYSTEM_INFO si = {};
			GetSystemInfo(&si);
			return reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
		}();
		return limit;
	}

	// A pointer worth reading through: user range, 4-byte aligned.
	inline bool plausible(uintptr_t p)
	{
		return p >= 0x10000 && p <= user_address_limit() && (p & 3) == 0;
	}

	// Inside FalloutNV.exe's image (vtables, code, globals).
	bool in_image(uintptr_t p);

	// Read faults since start (counted, never logged per read).
	uint32_t faults();
}
