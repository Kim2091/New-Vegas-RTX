#pragma once

namespace shared::utils
{
	const char* va(const char* fmt, ...);
	
	std::string str_to_lower(std::string input);
	std::string convert_wstring(const std::wstring& wstr);

	uint32_t data_hash32(const void* data, size_t size);
}
