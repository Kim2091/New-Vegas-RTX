#pragma once
#include <cstdint>

// fnv_remix_nvse.ini next to the DLL. Missing keys keep their defaults.

namespace fnvr::config
{
	struct Values
	{
		uint32_t monitor_period = 300;          // [Monitor] Period: frames between two integrity re-hashes
		float cut_distance = 2048.0f;           // [Monitor] CutDistance: a world-camera jump this far with no load is a cut
		uint32_t refs_loaded_timeout_ms = 20000;// [Monitor] RefsLoadedTimeoutMs: an attached cell counts as loaded after this
		uint32_t refs_loaded_poll_frames = 30;  // [Monitor] RefsLoadedPollFrames: queue counters at zero this long = loaded
		uint32_t text_diff_ranges = 200;        // [Report] TextDiffRanges: .text ranges listed in the startup report
		bool verbose = false;                   // [Logging] Verbose: every log line, and the dev diagnostics
	};

	void load();
	const Values& get();
}
