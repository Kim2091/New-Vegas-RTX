#include "common.hpp"
#include "queue.hpp"

namespace fnvr::queue
{
	namespace
	{
		SRWLOCK g_lock = SRWLOCK_INIT;
		std::vector<Raw> g_in;

		FnvRemixEvent g_out[kOutCapacity];
		uint32_t g_head = 0, g_count = 0;
		uint32_t g_serial = 0;
		bool g_overflowed = false;
		uint32_t g_overflows = 0;
	}

	void push(const Raw& r)
	{
		AcquireSRWLockExclusive(&g_lock);
		g_in.push_back(r);
		ReleaseSRWLockExclusive(&g_lock);
	}

	void take(std::vector<Raw>& out)
	{
		out.clear();
		AcquireSRWLockExclusive(&g_lock);
		out.swap(g_in);
		ReleaseSRWLockExclusive(&g_lock);
	}

	void emit(uint16_t type, uintptr_t a, uintptr_t b)
	{
		if (g_count == kOutCapacity)
		{
			// Drop the oldest; the wrapper resyncs from facts and cells.
			g_head = (g_head + 1) % kOutCapacity;
			g_count--;
			if (!g_overflowed)
			{
				g_overflows++;
				log::f("Queue", "wrapper event queue full ({} events): dropping the oldest, the wrapper resyncs "
					"(overflow #{})", kOutCapacity, g_overflows);
			}
			g_overflowed = true;
		}
		FnvRemixEvent& e = g_out[(g_head + g_count) % kOutCapacity];
		e.type = type;
		e.pad = 0;
		e.serial = ++g_serial;
		e.a = a;
		e.b = b;
		g_count++;
	}

	uint32_t drain(FnvRemixEvent* out, uint32_t max)
	{
		uint32_t n = 0;
		if (g_overflowed && n < max)
		{
			out[n++] = FnvRemixEvent{ FNVR_EV_OVERFLOW, 0, ++g_serial, 0, 0 };
			g_overflowed = false;
		}
		while (g_count && n < max)
		{
			out[n++] = g_out[g_head];
			g_head = (g_head + 1) % kOutCapacity;
			g_count--;
		}
		return n;
	}

	uint32_t pending()
	{
		return g_count;
	}
}
