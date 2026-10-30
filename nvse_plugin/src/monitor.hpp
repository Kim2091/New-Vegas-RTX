#pragma once
#include <cstdint>

// The plugin's main-thread core (main.cpp): the main-loop frame count and
// the application of the incoming queue.

namespace fnvr::monitor
{
	// Applies every queued engine message, in order. Main thread only.
	void apply_queue();

	uint32_t frame();
	bool on_main_thread();
	uint32_t xnvse_version();
	// FnvRemixFeature bits this build implements.
	uint32_t features_built();

	// An export of the wrapper's d3d9.dll (FnvRemix_HookChain, ...), found
	// in whichever loaded module exports it; null when none does.
	void* wrapper_export(const char* name);
}
