#pragma once

// The hang dump channel between the game (session_report.cpp) and
// NewVegasRTX-Reporter.exe. The reporter writes the game's hang dump from its
// own process: MiniDumpWriteDump needs memory, and a game that hangs after
// filling its 32-bit address space has none left (in-process,
// MiniDumpWriteDump fails there with ERROR_GEN_FAILURE).
//
// The game creates the channel before it starts the reporter: one small
// shared section and two auto-reset events, named after the game's process id
// and creation time. For a dump it writes the path, bumps `request`, sets the
// request event and waits for the done event (or the reporter's exit). The
// reporter copies `request` to `done` when the dump is written or has failed.
// Nothing in the game allocates on that path once the channel exists.
namespace comp::game::hang_dump
{
	constexpr uint32_t kVersion = 1;

	struct Shared
	{
		uint32_t version;
		volatile LONG request;   // bumped by the game for each dump
		volatile LONG done;      // set to `request` by the reporter when it is finished
		uint32_t ok;             // 1: the dump was written
		uint32_t error;          // GetLastError() of the failed step otherwise
		wchar_t path[MAX_PATH];
	};

	// Thread stacks, the module list, thread times and start addresses, TEBs
	// and the PEB, and the modules unloaded so far: a few MB, small enough for
	// the session zip a player uploads.
	constexpr uint32_t kDumpType = 0x00000000 /* MiniDumpNormal */ | 0x00001000 /* MiniDumpWithThreadInfo */ |
		0x00000020 /* MiniDumpWithUnloadedModules */ | 0x00000100 /* MiniDumpWithProcessThreadData */;

	// The section's and the events' names: `what` is "map", "request" or "done".
	inline void object_name(wchar_t (&out)[96], DWORD pid, uint64_t created, const wchar_t* what)
	{
		swprintf_s(out, L"Local\\NewVegasRTX-HangDump-%lu-%llu-%s", static_cast<unsigned long>(pid),
			static_cast<unsigned long long>(created), what);
	}

	// The system's text for a Win32 error or an HRESULT that wraps one
	// (MiniDumpWriteDump reports HRESULTs such as 0x8007001F), in one line.
	inline void error_text(DWORD error, char* out, size_t out_n)
	{
		if (!out_n) return;
		out[0] = '\0';
		DWORD code = error;
		if ((code & 0xFFFF0000u) == 0x80070000u) code &= 0xFFFFu;
		const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, out,
			static_cast<DWORD>(out_n), nullptr);
		size_t len = n < out_n ? n : out_n - 1;
		out[len] = '\0';
		while (len && (out[len - 1] == '\r' || out[len - 1] == '\n' || out[len - 1] == ' ' || out[len - 1] == '.'))
			out[--len] = '\0';
		for (size_t i = 0; i < len; i++)
			if (out[i] == '\r' || out[i] == '\n') out[i] = ' ';
	}
}
