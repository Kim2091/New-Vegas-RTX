#pragma once
#include <cstdint>
#include <vector>

// Two queues.
//
// Incoming: xNVSE messages copied by the handlers, which may run on a
// loader thread (NVTF), under a lock. Nothing else happens in a handler.
// The main thread takes the whole batch and applies it in order.
//
// Outgoing: FnvRemixEvents for the wrapper, main thread only, a fixed
// capacity (kOutCapacity). An overflow drops the oldest and leaves one
// FNVR_EV_OVERFLOW at the front of the next drain.

namespace fnvr::queue
{
	struct Raw
	{
		uint32_t type;     // nvse_abi::MessageType
		uintptr_t a, b, c;
	};

	// Any thread.
	void push(const Raw& r);
	// Main thread: moves every queued message into out (cleared first).
	void take(std::vector<Raw>& out);

	constexpr uint32_t kOutCapacity = 4096;

	// Main thread.
	void emit(uint16_t type, uintptr_t a = 0, uintptr_t b = 0);
	uint32_t drain(FnvRemixEvent* out, uint32_t max);
	uint32_t pending();
}
