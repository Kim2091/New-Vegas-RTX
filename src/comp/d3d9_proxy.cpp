#include "std_include.hpp"
#include "d3d9_proxy.hpp"
#include "shared/common/config.hpp"
#include "shared/globals.hpp"

namespace d3d9_proxy
{
	static HMODULE chain_module_ = nullptr;
	static std::vector<HMODULE> loaded_dlls_;

	static PFN_Direct3DCreate9      pDirect3DCreate9 = nullptr;
	static PFN_Direct3DCreate9Ex    pDirect3DCreate9Ex = nullptr;
	static PFN_D3DPERF_BeginEvent   pD3DPERF_BeginEvent = nullptr;
	static PFN_D3DPERF_EndEvent     pD3DPERF_EndEvent = nullptr;
	static PFN_D3DPERF_SetMarker    pD3DPERF_SetMarker = nullptr;
	static PFN_D3DPERF_SetRegion    pD3DPERF_SetRegion = nullptr;
	static PFN_D3DPERF_QueryRepeatFrame pD3DPERF_QueryRepeatFrame = nullptr;
	static PFN_D3DPERF_SetOptions   pD3DPERF_SetOptions = nullptr;
	static PFN_D3DPERF_GetStatus    pD3DPERF_GetStatus = nullptr;
	static FARPROC pDirect3DShaderValidatorCreate9 = nullptr;
	static FARPROC pDebugSetLevel = nullptr;
	static FARPROC pDebugSetMute = nullptr;

	static void resolve_procs(HMODULE mod)
	{
		pDirect3DCreate9                = (PFN_Direct3DCreate9)GetProcAddress(mod, "Direct3DCreate9");
		pDirect3DCreate9Ex              = (PFN_Direct3DCreate9Ex)GetProcAddress(mod, "Direct3DCreate9Ex");
		pD3DPERF_BeginEvent             = (PFN_D3DPERF_BeginEvent)GetProcAddress(mod, "D3DPERF_BeginEvent");
		pD3DPERF_EndEvent               = (PFN_D3DPERF_EndEvent)GetProcAddress(mod, "D3DPERF_EndEvent");
		pD3DPERF_SetMarker              = (PFN_D3DPERF_SetMarker)GetProcAddress(mod, "D3DPERF_SetMarker");
		pD3DPERF_SetRegion              = (PFN_D3DPERF_SetRegion)GetProcAddress(mod, "D3DPERF_SetRegion");
		pD3DPERF_QueryRepeatFrame       = (PFN_D3DPERF_QueryRepeatFrame)GetProcAddress(mod, "D3DPERF_QueryRepeatFrame");
		pD3DPERF_SetOptions             = (PFN_D3DPERF_SetOptions)GetProcAddress(mod, "D3DPERF_SetOptions");
		pD3DPERF_GetStatus              = (PFN_D3DPERF_GetStatus)GetProcAddress(mod, "D3DPERF_GetStatus");
		pDirect3DShaderValidatorCreate9 = GetProcAddress(mod, "Direct3DShaderValidatorCreate9");
		pDebugSetLevel                  = GetProcAddress(mod, "DebugSetLevel");
		pDebugSetMute                   = GetProcAddress(mod, "DebugSetMute");
	}

	static void load_dll_list(const std::string& list, const char* tag)
	{
		if (list.empty()) return;

		std::stringstream ss(list);
		std::string entry;
		while (std::getline(ss, entry, ';'))
		{
			auto start = entry.find_first_not_of(" \t");
			if (start == std::string::npos) continue;
			auto end = entry.find_last_not_of(" \t");
			entry = entry.substr(start, end - start + 1);
			if (entry.empty()) continue;

			HMODULE mod = LoadLibraryA(entry.c_str());
			if (mod)
			{
				loaded_dlls_.push_back(mod);
				shared::common::log("Chain", std::format("[{}] Loaded: {}", tag, entry));
			}
			else
			{
				shared::common::log("Chain",
					std::format("[{}] Failed to load: {} (error 0x{:X})", tag, entry, GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
		}
	}

	static bool passthrough_ = false;

	bool init_system()
	{
		passthrough_ = true;
		char sys_path[MAX_PATH];
		GetSystemDirectoryA(sys_path, MAX_PATH);
		strcat_s(sys_path, "\\d3d9.dll");
		chain_module_ = LoadLibraryA(sys_path);
		if (!chain_module_) return false;
		resolve_procs(chain_module_);
		return pDirect3DCreate9 != nullptr;
	}

	bool passthrough() { return passthrough_; }

	// The bridge client starts the renderer from .trex\NvRemixBridge.exe, or trex\ (Mod Organizer 2's Root Builder
	// skips names that start with a dot). Returns true when either exists in the game folder.
	static bool renderer_installed()
	{
		for (const char* dir : { "\\.trex\\", "\\trex\\" })
		{
			const std::string path = shared::globals::root_path + dir + "NvRemixBridge.exe";
			if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
		}
		return false;
	}

	// Tells the player in a message box, from its own thread so the game keeps loading.
	static DWORD WINAPI renderer_missing_box(LPVOID)
	{
		MessageBoxW(nullptr,
			L"New Vegas RTX cannot find its renderer: the .trex folder with NvRemixBridge.exe is missing from the game "
			L"folder.\n\nThe game starts without RTX. Reinstall the mod, and make sure the .trex folder (trex in the "
			L"Mod Organizer 2 download) is extracted too. If NvRemixBridge.exe keeps disappearing, your antivirus is "
			L"removing it: check its quarantine.",
			L"New Vegas RTX", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
		return 0;
	}

	bool init()
	{
		auto& cfg = shared::common::config::get();

		// The Remix bridge first ([Remix] DLLName, default d3d9_remix.dll).
		if (cfg.remix.enabled && !cfg.remix.dll_name.empty() && !renderer_installed())
		{
			shared::common::log("Proxy", "The renderer is missing: no .trex\\NvRemixBridge.exe or trex\\NvRemixBridge.exe in the "
				"game folder. The game starts with the system d3d9, without RTX.", shared::common::LOG_TYPE::LOG_TYPE_ERROR);
			if (const auto t = CreateThread(nullptr, 0, renderer_missing_box, nullptr, 0, nullptr); t) CloseHandle(t);
		}
		else if (cfg.remix.enabled && !cfg.remix.dll_name.empty())
		{
			chain_module_ = LoadLibraryA(cfg.remix.dll_name.c_str());
			if (chain_module_)
			{
				resolve_procs(chain_module_);
				shared::common::log("Proxy", std::format("Loaded Remix bridge: {}", cfg.remix.dll_name));
			}
			else
			{
				shared::common::log("Proxy",
					std::format("Remix enabled but could not load '{}' (error 0x{:X}). Falling back to system d3d9.",
						cfg.remix.dll_name, GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
		}

		if (!pDirect3DCreate9)
		{
			char sys_path[MAX_PATH];
			GetSystemDirectoryA(sys_path, MAX_PATH);
			strcat_s(sys_path, "\\d3d9.dll");

			chain_module_ = LoadLibraryA(sys_path);
			if (chain_module_)
			{
				resolve_procs(chain_module_);
				shared::common::log("Proxy", std::format("Loaded system d3d9: {}", sys_path));
			}
		}

		if (!pDirect3DCreate9)
		{
			shared::common::log("Proxy", "FATAL: Could not load any d3d9 implementation",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}

		shared::globals::d3d9_chain_module = chain_module_;

		return true;
	}

	void load_preload_dlls()
	{
		load_dll_list(shared::common::config::get().chain.preload, "Pre");
	}

	void load_postload_dlls()
	{
		load_dll_list(shared::common::config::get().chain.postload, "Post");
	}

	PFN_Direct3DCreate9      get_Direct3DCreate9()      { return pDirect3DCreate9; }
	PFN_Direct3DCreate9Ex    get_Direct3DCreate9Ex()    { return pDirect3DCreate9Ex; }
	PFN_D3DPERF_BeginEvent   get_D3DPERF_BeginEvent()   { return pD3DPERF_BeginEvent; }
	PFN_D3DPERF_EndEvent     get_D3DPERF_EndEvent()     { return pD3DPERF_EndEvent; }
	PFN_D3DPERF_SetMarker    get_D3DPERF_SetMarker()    { return pD3DPERF_SetMarker; }
	PFN_D3DPERF_SetRegion    get_D3DPERF_SetRegion()    { return pD3DPERF_SetRegion; }
	PFN_D3DPERF_QueryRepeatFrame get_D3DPERF_QueryRepeatFrame() { return pD3DPERF_QueryRepeatFrame; }
	PFN_D3DPERF_SetOptions   get_D3DPERF_SetOptions()   { return pD3DPERF_SetOptions; }
	PFN_D3DPERF_GetStatus    get_D3DPERF_GetStatus()    { return pD3DPERF_GetStatus; }
	FARPROC get_Direct3DShaderValidatorCreate9()         { return pDirect3DShaderValidatorCreate9; }
	FARPROC get_DebugSetLevel()                          { return pDebugSetLevel; }
	FARPROC get_DebugSetMute()                           { return pDebugSetMute; }
}

// d3d9.dll exports. Direct3DCreate9 and Direct3DCreate9Ex are defined in
// d3d9ex.cpp; everything else forwards to the real d3d9 chain.

extern "C"
{
	int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName)
	{
		auto fn = d3d9_proxy::get_D3DPERF_BeginEvent();
		return fn ? fn(col, wszName) : 0;
	}

	int WINAPI D3DPERF_EndEvent()
	{
		auto fn = d3d9_proxy::get_D3DPERF_EndEvent();
		return fn ? fn() : 0;
	}

	void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName)
	{
		auto fn = d3d9_proxy::get_D3DPERF_SetMarker();
		if (fn) fn(col, wszName);
	}

	void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName)
	{
		auto fn = d3d9_proxy::get_D3DPERF_SetRegion();
		if (fn) fn(col, wszName);
	}

	BOOL WINAPI D3DPERF_QueryRepeatFrame()
	{
		auto fn = d3d9_proxy::get_D3DPERF_QueryRepeatFrame();
		return fn ? fn() : FALSE;
	}

	void WINAPI D3DPERF_SetOptions(DWORD dwOptions)
	{
		auto fn = d3d9_proxy::get_D3DPERF_SetOptions();
		if (fn) fn(dwOptions);
	}

	DWORD WINAPI D3DPERF_GetStatus()
	{
		auto fn = d3d9_proxy::get_D3DPERF_GetStatus();
		return fn ? fn() : 0;
	}

	// Undocumented; some games import it.
	void* WINAPI Direct3DShaderValidatorCreate9()
	{
		auto fn = (void* (WINAPI*)())d3d9_proxy::get_Direct3DShaderValidatorCreate9();
		return fn ? fn() : nullptr;
	}

	void WINAPI DebugSetLevel(DWORD level)
	{
		auto fn = (void (WINAPI*)(DWORD))d3d9_proxy::get_DebugSetLevel();
		if (fn) fn(level);
	}

	void WINAPI DebugSetMute()
	{
		auto fn = (void (WINAPI*)())d3d9_proxy::get_DebugSetMute();
		if (fn) fn();
	}
}
