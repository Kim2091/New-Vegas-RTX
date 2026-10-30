#pragma once
#include <cstdint>
#include <format>
#include <string_view>

// The plugin's own log, rtx-remix\logs\fnv_remix_nvse.log in the game folder,
// beside the runtime's and the wrapper's logs. One line per call:
// "[time] [Tag] text". Safe from any thread; every line is flushed.
// - The previous session's file is kept as fnv_remix_nvse.prev.log.
// - The first line names the folder the file really is in, which a mod
//   manager's virtual file system can move out of the game folder.
// - A line kind (the tag and the text's first two words, without digits)
//   is written for its first 10 lines, then at most once every 5 minutes
//   with a count of the lines held back. event() lines are never held
//   back; [Logging] Verbose=1 writes every line.
// - Past 20 MB nothing more is written but one line that says so.

namespace fnvr::log
{
	void open(HMODULE self);
	void line(std::string_view tag, std::string_view text);
	// A line the repeat limit never holds back: the startup report, state
	// changes and lines their caller already rate-limits.
	void event(std::string_view tag, std::string_view text);

	template <class... Args>
	void f(std::string_view tag, std::format_string<Args...> fmt, Args&&... args)
	{
		line(tag, std::format(fmt, std::forward<Args>(args)...));
	}

	template <class... Args>
	void e(std::string_view tag, std::format_string<Args...> fmt, Args&&... args)
	{
		event(tag, std::format(fmt, std::forward<Args>(args)...));
	}

	// [Logging] Verbose: every line is written, and the dev diagnostics
	// gated on verbose() as well.
	void set_verbose(bool on);
	bool verbose();

	// Lines the repeat limit held back since start.
	uint64_t held_back();

	// The directory of this DLL, with a trailing backslash.
	const std::wstring& dir();
}
