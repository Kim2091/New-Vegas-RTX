// Standalone test of comp/game/sampler: the main thread spins in its own
// code, sleeps and waits on an event for a few seconds while the sampler
// samples it, then the test checks the shares and prints the profile lines.
// Runs on the CPU only. Built by build_test.bat; not part of the shipped DLLs.

#include "std_include.hpp"
#include "comp/game/sampler.hpp"

#include <cstdio>

namespace sampler = comp::game::sampler;

namespace
{
	volatile uint32_t g_sink = 0;

	__declspec(noinline) void spin(double ms)
	{
		LARGE_INTEGER f, t0, t;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t0);
		do
		{
			for (int i = 0; i < 1000; i++) g_sink = g_sink * 1664525u + 1013904223u;
			QueryPerformanceCounter(&t);
		} while (static_cast<double>(t.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart) < ms);
	}

	double share(const std::string& s, const std::string& key)
	{
		const size_t at = s.find(key);
		return at == std::string::npos ? 0.0 : atof(s.c_str() + at + key.size());
	}
}

int main()
{
	const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	sampler::start();
	sampler::set_active(true);
	sampler::begin_window();

	// Per 16 ms "frame": 8 ms in own code, 4 ms Sleep, 4 ms event wait.
	LARGE_INTEGER f, t0, t1;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	uint32_t frames = 0;
	for (; frames < 500; frames++)
	{
		spin(8.0);
		Sleep(4);
		WaitForSingleObject(event, 4);
		sampler::frame_done(true);
	}
	QueryPerformanceCounter(&t1);
	// A 5 s stall outside the counted frames: its samples are dropped (kept,
	// they would put the rate near 40 Hz).
	Sleep(5000);
	sampler::frame_done(false);
	const double seconds = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
	const sampler::Text text = sampler::end_window(frames, seconds);
	sampler::stop();

	printf("[Perf] ... | %s\n\n[PerfSlow] ... | game thread %s\n\n", text.compact.c_str(), text.detail.c_str());
	printf("spin() is at sampler_test.exe+0x%X\n\n", static_cast<unsigned>(reinterpret_cast<uintptr_t>(&spin)
		- reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));

	int failures = 0;
	auto check = [&](bool ok, const char* what)
		{
			printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
			failures += ok ? 0 : 1;
		};
	const std::string& c = text.compact;
	check(share(c, "profile of ") >= 120.0, "at least 120 samples");
	check(share(c, "samples (") >= 18.0 && share(c, "samples (") <= 32.0, "rate near 25 Hz over the counted time");
	check(share(c, "sampler_test.exe ") > 15.0, "own code share over 15% (Sleep and the wait round up to the timer tick)");
	check(share(c, "sleep ") > 10.0, "sleep share over 10%");
	check(share(c, ", wait ") > 10.0 || share(c, ": wait ") > 10.0, "wait share over 10%");
	check(c.find("sleep") != std::string::npos && c.find("from sampler_test.exe") != std::string::npos, "waits attributed to the caller");
	check(text.detail.find("sleep sampler_test.exe+0x") != std::string::npos, "sleep call site found");
	check(sampler::session_detail().find("profile of ") != std::string::npos, "session totals");
	return failures;
}
