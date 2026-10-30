#include "common.hpp"
#include "mods.hpp"

namespace fnvr::mods
{
	uint32_t load_order(char* out, uint32_t size)
	{
		uint32_t dh = 0, count = 0;
		if (!guard::get(fnv_exe::addr().data_handler, dh) || !guard::plausible(dh) ||
			!guard::get(dh + engine::DH_loadedModCount, count) || count == 0 || count > 0xFF)
			return 0;

		std::string list;
		list.reserve(count * 32);
		for (uint32_t i = 0; i < count; i++)
		{
			uint32_t info = 0;
			char name[0x104] = {};
			if (!guard::get(dh + engine::DH_loadedMods + i * 4, info) || !guard::plausible(info) ||
				!guard::read(info + engine::MODINFO_name, name, sizeof(name) - 1))
			{
				list += std::format("{:02X} (unreadable)\n", i);
				continue;
			}
			// The name is a C string in a fixed buffer; anything past its NUL is left over.
			size_t len = 0;
			while (len < sizeof(name) - 1 && name[len]) len++;
			list += std::format("{:02X} {}\n", i, std::string_view(name, len));
		}

		if (out && size)
		{
			const size_t n = (std::min)(list.size(), static_cast<size_t>(size - 1));
			std::memcpy(out, list.data(), n);
			out[n] = '\0';
		}
		return static_cast<uint32_t>(list.size());
	}
}
