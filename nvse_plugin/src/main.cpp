#include "common.hpp"
#include <psapi.h>
#include "nvse_abi.h"
#include "monitor.hpp"
#include "config.hpp"
#include "queue.hpp"
#include "cells.hpp"
#include "status.hpp"
#include "camera.hpp"
#include "menu.hpp"
#include "integrity.hpp"
#include "owners.hpp"
#include "lights.hpp"
#include "pins.hpp"
#include "decals.hpp"
#include "fog.hpp"

// fnv_remix_nvse.dll: the engine monitor of the FNV Remix wrapper. It listens to xNVSE's messages, polls a
// few engine fields on the main thread and answers the wrapper through
// FnvRemix_GetApi (api.cpp). It installs no hooks and writes no engine code.
//
// Threads: the ref / cell messages may arrive on a loader thread (NVTF), so
// their handlers only copy the message into the incoming queue (and, for
// OnRefSet3D, pin the new root with the engine's interlocked increment). The
// main thread applies the queue in order (at the main loop and at every
// wrapper call) and does all other work, every pin release included.

namespace fnvr::monitor
{
	namespace
	{
		HMODULE g_self = nullptr;
		nvse_abi::PluginHandle g_handle = nvse_abi::kInvalidHandle;
		uint32_t g_xnvse = 0;
		DWORD g_main_thread = 0;
		uint32_t g_frame = 0;
		bool g_applying = false;
		std::vector<queue::Raw> g_batch;
		uint32_t g_counts[33] = {};   // messages applied, by type

		// Before the engine tears the world down: every owner and light goes,
		// and every pin is released now.
		void drop_world(const char* why)
		{
			owners::clear(why);
			lights::clear();
			pins::release_all(why);
		}

		void handle_main(uint32_t type, uintptr_t a)
		{
			using namespace nvse_abi;
			switch (type)
			{
			case kMsg_PreLoadGame:
				drop_world("PreLoadGame");
				camera::on_pre_load();
				break;
			case kMsg_PostLoadGame:
				camera::on_post_load((a & 0xFF) != 0);
				cells::resync("post load");
				break;
			case kMsg_NewGame:
				log::e("Plugin", "NewGame");
				drop_world("NewGame");
				camera::on_new_game();
				break;
			case kMsg_ExitToMainMenu:
				log::e("Plugin", "ExitToMainMenu");
				drop_world("ExitToMainMenu");
				camera::on_main_menu();
				break;
			case kMsg_OnFramePresent:
				camera::on_present(a != 0);
				break;
			default:
				break;
			}
		}

		void apply(const queue::Raw& r)
		{
			using namespace nvse_abi;
			if (r.type < std::size(g_counts)) g_counts[r.type]++;
			switch (r.type)
			{
			case kMsg_OnCellStateChange:
				cells::on_state(r.a, static_cast<uint32_t>(r.b), static_cast<uint32_t>(r.c));
				break;
			case kMsg_OnCellRefsLoaded:
				cells::on_refs_loaded(r.a);
				break;
			case kMsg_OnRefSet3D:
				owners::on_set3d(r.a, r.b);
				break;
			case kMsg_OnRefUnset3D:
				owners::on_unset3d(r.a);
				break;
			case kMsg_OnRefAttach:
				owners::on_attach(r.a);
				break;
			case kMsg_OnNonPersistentFormUnload:
				owners::on_form_unload(r.a);
				break;
			default:
				handle_main(r.type, r.a);
				break;
			}
		}

		void on_message(nvse_abi::Message* msg)
		{
			using namespace nvse_abi;
			if (!msg) return;
			const uintptr_t data = reinterpret_cast<uintptr_t>(msg->data);
			queue::Raw r = { msg->type, 0, 0, 0 };
			switch (msg->type)
			{
			case kMsg_MainGameLoop:
				if (!g_main_thread || g_main_thread != GetCurrentThreadId())
				{
					if (g_main_thread)
						log::e("Plugin", "MainGameLoop runs on thread {} (Load ran on {}): that is the main thread now",
							GetCurrentThreadId(), g_main_thread);
					g_main_thread = GetCurrentThreadId();
				}
				g_frame++;
				guard::new_frame();
				apply_queue();
				camera::tick();
				cells::tick(g_frame);
				status::tick();
				owners::tick(g_frame);
				lights::tick(g_frame);
				decals::tick(g_frame);
				fog::tick();
				pins::sweep(g_frame);
				menu::poll();
				integrity::tick(g_frame);
				return;
			// The game quits: the wrapper's session report records a quit, not a crash.
			case kMsg_ExitGame:
			case kMsg_ExitGame_Console:
				log::e("Plugin", "{}", msg->type == kMsg_ExitGame ? "ExitGame" : "ExitGame_Console");
				if (const auto fn = reinterpret_cast<PFN_FnvRemix_SessionExit>(wrapper_export("FnvRemix_SessionExit")))
					fn(msg->type == kMsg_ExitGame ? "menu" : "console");
				return;
			case kMsg_DeferredInit:
				log::e("Plugin", "DeferredInit: startup report");
				apply_queue();
				integrity::report();
				menu::poll();
				return;
			// Engine events: copied, nothing else (any thread).
			case kMsg_OnCellStateChange:
				if (msg->dataLen >= 12 && data)
				{
					const uint32_t* d = static_cast<const uint32_t*>(msg->data);
					r.a = d[0];
					r.b = d[1];
					r.c = d[2];
					queue::push(r);
				}
				return;
			case kMsg_OnCellRefsLoaded:
			case kMsg_OnRefUnset3D:
			case kMsg_OnRefAttach:
			case kMsg_OnNonPersistentFormUnload:
				r.a = data;
				queue::push(r);
				return;
			case kMsg_OnRefSet3D:
				// The new root is pinned here, before the queue, so it can't be
				// freed before the main thread walks it.
				if (msg->dataLen >= 8 && data)
				{
					const uint32_t* d = static_cast<const uint32_t*>(msg->data);
					if (!d[0] || !d[1]) return;
					r.a = d[0];
					r.b = d[1];
					pins::take_raw(r.b);
					queue::push(r);
				}
				return;
			// Load and present messages: handled at once on the main thread
			// (after the queue, to keep engine order), queued otherwise.
			case kMsg_PreLoadGame:
			case kMsg_PostLoadGame:
			case kMsg_NewGame:
			case kMsg_ExitToMainMenu:
			case kMsg_OnFramePresent:
				if (msg->type == kMsg_OnFramePresent) r.a = (data && msg->dataLen >= 4) ? *static_cast<const int*>(msg->data) : 0;
				else if (msg->type == kMsg_PostLoadGame) r.a = data & 0xFF;
				if (on_main_thread())
				{
					apply_queue();
					apply(r);
				}
				else queue::push(r);
				return;
			default:
				return;
			}
		}
	}

	void apply_queue()
	{
		if (g_applying || !on_main_thread()) return;
		g_applying = true;
		queue::take(g_batch);
		for (const auto& r : g_batch) apply(r);
		g_applying = false;
	}

	void* wrapper_export(const char* name)
	{
		HMODULE mods[512];
		DWORD need = 0;
		if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return nullptr;
		const DWORD n = (std::min)(need / static_cast<DWORD>(sizeof(HMODULE)), static_cast<DWORD>(std::size(mods)));
		for (DWORD i = 0; i < n; i++)
			if (const FARPROC p = GetProcAddress(mods[i], name)) return reinterpret_cast<void*>(p);
		return nullptr;
	}

	uint32_t frame() { return g_frame; }

	bool on_main_thread()
	{
		return g_main_thread && GetCurrentThreadId() == g_main_thread;
	}

	uint32_t xnvse_version() { return g_xnvse; }

	uint32_t features_built()
	{
		return FNVR_F_CELLS | FNVR_F_LOADING | FNVR_F_CAMERA | FNVR_F_MENU | FNVR_F_OWNERS | FNVR_F_LIGHTS |
			FNVR_F_DECALS | FNVR_F_MENU_BG | FNVR_F_WATER | FNVR_F_HOOK_P1 | FNVR_F_HOOK_P2 | FNVR_F_HOOK_P3 | FNVR_F_HOOK_P5;
	}

	bool query(const nvse_abi::NVSEInterface* nvse, nvse_abi::PluginInfo* info)
	{
		using namespace nvse_abi;
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "FnvRemixNvse";
		info->version = 1;
		log::open(g_self);
		g_xnvse = nvse->nvseVersion;
		log::e("Plugin", "fnv_remix_nvse commit {} (ABI {}), xNVSE {}.{}.{} (0x{:08X}), runtime 0x{:08X}, editor {}",
			FNVR_STR(FNVR_COMMIT), FNV_REMIX_ABI, nvse->nvseVersion >> 24, (nvse->nvseVersion >> 16) & 0xFF,
			(nvse->nvseVersion >> 4) & 0xFFF, nvse->nvseVersion, nvse->runtimeVersion, nvse->isEditor);
		if (nvse->isEditor)
		{
			log::e("Plugin", "loaded by the editor: not loading");
			return false;
		}
		if (nvse->runtimeVersion != kRuntime_1_4_0_525)
		{
			log::e("Plugin", "FalloutNV.exe runtime 0x{:08X} is not 1.4.0.525 (0x{:08X}): not loading",
				nvse->runtimeVersion, kRuntime_1_4_0_525);
			return false;
		}
		const auto& exe = fnv_exe::identity();
		if (!exe.variant)
		{
			log::e("Plugin", "UNKNOWN FalloutNV.exe (timestamp 0x{:08X}, image 0x{:X}, .rsrc 0x{:016X}, store import "
				"'{}'{}; no match on: {}): no engine addresses -- not loading", exe.timestamp, exe.size_of_image,
				exe.rsrc_fnv1a, exe.store_import, exe.steamstub ? ", SteamStub-wrapped" : "", exe.mismatch);
			return false;
		}
		log::e("Plugin", "executable: {}{}", exe.variant->name, exe.steamstub ? ", SteamStub-wrapped" : "");
		if (nvse->nvseVersion < kXnvse_6_4_8)
		{
			log::e("Plugin", "xNVSE {}.{}.{} is older than 6.4.8 (no ref / cell events): update xNVSE -- not loading",
				nvse->nvseVersion >> 24, (nvse->nvseVersion >> 16) & 0xFF, (nvse->nvseVersion >> 4) & 0xFFF);
			return false;
		}
		return true;
	}

	bool load(const nvse_abi::NVSEInterface* nvse)
	{
		using namespace nvse_abi;
		g_handle = nvse->GetPluginHandle();
		g_main_thread = GetCurrentThreadId();
		config::load();
		decals::load_config();
		fog::load_config();
		auto* msg = static_cast<MessagingInterface*>(nvse->QueryInterface(kInterface_Messaging));
		if (!msg || !msg->RegisterListener || !msg->RegisterListener(g_handle, "NVSE", &on_message))
		{
			log::e("Plugin", "the messaging interface did not take our listener: not running");
			return false;
		}
		log::e("Plugin", "loaded: listening to xNVSE (handle {}, messaging v{}), thread {}", g_handle, msg->version,
			g_main_thread);
		return true;
	}

	void set_self(HMODULE m) { g_self = m; }
}

extern "C"
{
	bool NVSEPlugin_Query(const nvse_abi::NVSEInterface* nvse, nvse_abi::PluginInfo* info)
	{
		return fnvr::monitor::query(nvse, info);
	}

	bool NVSEPlugin_Load(const nvse_abi::NVSEInterface* nvse)
	{
		return fnvr::monitor::load(nvse);
	}
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(inst);
		fnvr::monitor::set_self(inst);
	}
	return TRUE;
}
