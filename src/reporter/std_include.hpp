#pragma once

// NewVegasRTX-Reporter.exe's common include: the Windows and standard headers
// the shared session zip code (comp/game/session_zip.*, zip_store.*) gets
// from the wrapper's std_include.hpp.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
