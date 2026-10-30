#include "std_include.hpp"
#include "shared/globals.hpp"

#define VA_BUFFER_COUNT		64
#define VA_BUFFER_SIZE		65536

namespace shared::utils
{
	const char* va(const char* fmt, ...)
	{
		static char g_vaBuffer[VA_BUFFER_COUNT][VA_BUFFER_SIZE];
		static int g_vaNextBufferIndex = 0;

		va_list ap;
		va_start(ap, fmt);
		char* dest = g_vaBuffer[g_vaNextBufferIndex];
		vsnprintf(g_vaBuffer[g_vaNextBufferIndex], VA_BUFFER_SIZE, fmt, ap);
		g_vaNextBufferIndex = (g_vaNextBufferIndex + 1) % VA_BUFFER_COUNT;
		va_end(ap);
		return dest;
	}

	std::string str_to_lower(std::string input)
	{
		std::ranges::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return input;
	}

	std::string convert_wstring(const std::wstring& wstr)
	{
		std::string result;
		result.reserve(wstr.size());

		for (const auto& chr : wstr) {
			result.push_back(static_cast<char>(chr));
		}

		return result;
	}

	// FNV-1a.
	uint32_t data_hash32(const void* data, const size_t size)
	{
		constexpr uint32_t FNV_prime = 16777619u;
		constexpr uint32_t offset_basis = 2166136261u;
		uint32_t hash = offset_basis;
		const uint8_t* bytes = static_cast<const uint8_t*>(data);

		for (size_t i = 0; i < size; ++i) 
		{
			hash ^= bytes[i];
			hash *= FNV_prime;
		}

		return hash;
	}
}
