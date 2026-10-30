#include "std_include.hpp"
#include "remix_api.hpp"
#include "bridge_remix_api.h"

namespace shared::common
{
	remix_api& remix_api::get()
	{
		static remix_api instance;
		return instance;
	}

	// Bridge callback at the device's BeginScene.
	void __stdcall remix_api::begin_scene_callback_internal()
	{
		auto& api = get();
		if (api.is_initialized())
		{
			if (api.begin_scene_callback_external) {
				api.begin_scene_callback_external();
			}
		}
	}

	void __stdcall remix_api::end_scene_callback_internal()
	{
		if (get().is_initialized() && get().end_scene_callback_external) {
			get().end_scene_callback_external();
		}
	}

	void __stdcall remix_api::present_callback_internal()
	{
		if (get().is_initialized() && get().present_callback_external) {
			get().present_callback_external();
		}
	}

	void remix_api::initialize(
		PFN_remixapi_BridgeCallback begin_scene_callback,
		PFN_remixapi_BridgeCallback end_scene_callback,
		PFN_remixapi_BridgeCallback present_callback,
		bool /*is_asi*/)
	{
		auto& instance = get();
		if (instance.m_initialized) return;

		// This DLL is d3d9.dll, so bridge_initRemixApi's GetModuleHandleA("d3d9.dll")
		// would find it instead of the Remix bridge; the proxy's chain module
		// handle reaches the bridge directly.
		HMODULE remix_module = shared::globals::d3d9_chain_module;
		if (!remix_module) return;

		auto pfn_init = (PFN_remixapi_InitializeLibrary)
			GetProcAddress(remix_module, remixapi::exported_func_name::initRemixApi);
		if (!pfn_init)
		{
			shared::common::log("RemixApi", "Remix bridge does not export remixapi_InitializeLibrary (not an RTX Remix DLL?)",
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return;
		}

		const remixapi_InitializeLibraryInfo init_info{
			REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO, nullptr,
			REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH)
		};
		remixapi_Interface iface{};
		const auto status = pfn_init(&init_info, &iface);

		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			shared::common::log("RemixApi",
				std::format("Failed to initialize the remixApi - Code: {:d}", static_cast<int>(status)),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		instance.m_bridge = iface;
		instance.begin_scene_callback_external = begin_scene_callback;
		instance.end_scene_callback_external = end_scene_callback;
		instance.present_callback_external = present_callback;

		auto pfn_callbacks = (PFN_remixapi_RegisterCallbacks)
			GetProcAddress(remix_module, remixapi::exported_func_name::registerCallbacks);
		if (pfn_callbacks)
			pfn_callbacks(begin_scene_callback_internal, end_scene_callback_internal, present_callback_internal);

		instance.m_initialized = true;
		shared::common::log("RemixApi", "Initialized RemixApi", shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}
}
