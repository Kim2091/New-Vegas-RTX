#pragma once

// Stands in for the wrapper's precompiled header when the test builds
// src/comp/game/call_chain.cpp on its own: the system headers that file
// uses, MinHook, and the log call it makes (defined in hook_chain_test.cpp).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "MinHook.h"

namespace shared::common
{
	enum class LOG_TYPE
	{
		LOG_TYPE_DEFAULT,
		LOG_TYPE_STATUS,
		LOG_TYPE_GREEN,
		LOG_TYPE_WARN,
		LOG_TYPE_ERROR,
	};

	void log(const std::string_view& module_str, const std::string_view& msg, LOG_TYPE type = LOG_TYPE::LOG_TYPE_DEFAULT,
		bool highlight = false);
}
