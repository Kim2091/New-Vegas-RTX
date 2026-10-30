#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "shared/fnv_remix_plugin_api.h"

#include "log.hpp"
#include "guard.hpp"
#include "engine.hpp"

// The git commit of this build (build_plugin.bat passes it).
#ifndef FNVR_COMMIT
#define FNVR_COMMIT unknown
#endif
#define FNVR_STR2(x) #x
#define FNVR_STR(x) FNVR_STR2(x)
