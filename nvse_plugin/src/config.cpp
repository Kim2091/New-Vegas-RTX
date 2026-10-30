#include "common.hpp"
#include "config.hpp"

namespace fnvr::config
{
	namespace
	{
		Values g_values;

		uint32_t read_u32(const std::wstring& ini, const wchar_t* section, const wchar_t* key, uint32_t def)
		{
			return GetPrivateProfileIntW(section, key, static_cast<INT>(def), ini.c_str());
		}
	}

	void load()
	{
		const std::wstring ini = log::dir() + L"fnv_remix_nvse.ini";
		Values v;
		v.monitor_period = (std::max)(30u, read_u32(ini, L"Monitor", L"Period", v.monitor_period));
		v.cut_distance = static_cast<float>((std::max)(256u, read_u32(ini, L"Monitor", L"CutDistance",
			static_cast<uint32_t>(v.cut_distance))));
		v.refs_loaded_timeout_ms = (std::max)(1000u, read_u32(ini, L"Monitor", L"RefsLoadedTimeoutMs",
			v.refs_loaded_timeout_ms));
		v.refs_loaded_poll_frames = (std::max)(1u, read_u32(ini, L"Monitor", L"RefsLoadedPollFrames",
			v.refs_loaded_poll_frames));
		v.text_diff_ranges = read_u32(ini, L"Report", L"TextDiffRanges", v.text_diff_ranges);
		v.verbose = read_u32(ini, L"Logging", L"Verbose", 0) != 0;
		g_values = v;
		log::set_verbose(v.verbose);
		log::e("Config", "Period={} CutDistance={:.0f} RefsLoadedTimeoutMs={} RefsLoadedPollFrames={} TextDiffRanges={} "
			"Verbose={}", v.monitor_period, v.cut_distance, v.refs_loaded_timeout_ms, v.refs_loaded_poll_frames,
			v.text_diff_ranges, v.verbose ? 1 : 0);
	}

	const Values& get()
	{
		return g_values;
	}
}
