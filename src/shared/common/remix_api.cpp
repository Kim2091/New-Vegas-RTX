#include "std_include.hpp"
#include "remix_api.hpp"
#include "bridge_remix_api.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

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

	// ---- [Lifetime] counts: wrappers around the bridge's Create* / Destroy* ----

	namespace
	{
		std::mutex g_life_mutex;
		remix_api::Lifetime g_life;
		std::unordered_map<uint64_t, uint64_t> g_texture_bytes;   // live texture handle -> its upload's bytes
		remixapi_Interface g_real = {};

		template <typename H>
		uint64_t handle_key(H h)
		{
			uint64_t k = 0;
			std::memcpy(&k, &h, (std::min)(sizeof(h), sizeof(k)));
			return k;
		}

		// Meshes by the return address of the CreateMesh call that made them.
		struct Site
		{
			uint64_t live = 0, created = 0, refreshed = 0;
		};
		std::unordered_map<uintptr_t, Site> g_mesh_sites;
		std::unordered_map<uint64_t, uintptr_t> g_mesh_live;   // live mesh handle -> its creating site

		bool is_refresh(const remixapi_MeshInfo* info)
		{
			for (auto* p = info ? static_cast<const remixapi_MeshInfo*>(info->pNext) : nullptr; p;
				p = static_cast<const remixapi_MeshInfo*>(p->pNext))
				if (p->sType == REMIXAPI_STRUCT_TYPE_MESH_INFO_REFRESH_GEOMETRY_EXT) return true;
			return false;
		}

		void count_mesh(const remixapi_MeshInfo* info, remixapi_MeshHandle h, uintptr_t site)
		{
			std::lock_guard lock(g_life_mutex);
			const bool refresh = is_refresh(info);
			auto [it, fresh] = g_mesh_live.try_emplace(handle_key(h), site);
			if (!fresh)
			{
				// A live mesh again: refreshed in place (or the same hash sent
				// again without the extension).
				(refresh ? g_life.mesh_refreshed : g_life.mesh_recreated)++;
				g_mesh_sites[it->second].refreshed++;
				return;
			}
			if (refresh) g_life.mesh_refresh_new++;
			g_life.mesh.created++;
			Site& s = g_mesh_sites[site];
			s.created++;
			s.live++;
		}

		remixapi_ErrorCode REMIXAPI_PTR create_mesh_counted(const remixapi_MeshInfo* info, remixapi_MeshHandle* out)
		{
			const auto site = reinterpret_cast<uintptr_t>(_ReturnAddress());
			const remixapi_ErrorCode rc = g_real.CreateMesh(info, out);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS && out) count_mesh(info, *out, site);
			return rc;
		}

		remixapi_ErrorCode REMIXAPI_PTR create_mesh_batched_counted(const remixapi_MeshInfo* info, remixapi_MeshHandle* out)
		{
			const auto site = reinterpret_cast<uintptr_t>(_ReturnAddress());
			const remixapi_ErrorCode rc = g_real.CreateMeshBatched(info, out);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS && out) count_mesh(info, *out, site);
			return rc;
		}

		remixapi_ErrorCode REMIXAPI_PTR destroy_mesh_counted(remixapi_MeshHandle h)
		{
			const remixapi_ErrorCode rc = g_real.DestroyMesh(h);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS)
			{
				std::lock_guard lock(g_life_mutex);
				if (const auto it = g_mesh_live.find(handle_key(h)); it != g_mesh_live.end())
				{
					g_life.mesh.destroyed++;
					Site& s = g_mesh_sites[it->second];
					if (s.live) s.live--;
					g_mesh_live.erase(it);
				}
				else g_life.mesh_unknown_destroys++;
			}
			return rc;
		}

		remixapi_ErrorCode REMIXAPI_PTR create_material_counted(const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out)
		{
			const remixapi_ErrorCode rc = g_real.CreateMaterial(info, out);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS) { std::lock_guard lock(g_life_mutex); g_life.material.created++; }
			return rc;
		}

		remixapi_ErrorCode REMIXAPI_PTR destroy_material_counted(remixapi_MaterialHandle h)
		{
			const remixapi_ErrorCode rc = g_real.DestroyMaterial(h);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS) { std::lock_guard lock(g_life_mutex); g_life.material.destroyed++; }
			return rc;
		}

		remixapi_ErrorCode REMIXAPI_PTR create_texture_counted(const remixapi_TextureInfo* info, remixapi_TextureHandle* out)
		{
			const remixapi_ErrorCode rc = g_real.CreateTexture(info, out);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS && out)
			{
				std::lock_guard lock(g_life_mutex);
				const uint64_t bytes = info ? info->dataSize : 0;
				auto [it, fresh] = g_texture_bytes.try_emplace(handle_key(*out), bytes);
				if (fresh)
				{
					g_life.texture.created++;
					g_life.texture_bytes += bytes;
				}
				else
				{
					// The same handle again (REPLACE in place): its new size.
					g_life.texture_bytes += bytes - it->second;
					it->second = bytes;
				}
			}
			return rc;
		}

		remixapi_ErrorCode REMIXAPI_PTR destroy_texture_counted(remixapi_TextureHandle h)
		{
			const remixapi_ErrorCode rc = g_real.DestroyTexture(h);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS)
			{
				std::lock_guard lock(g_life_mutex);
				if (const auto it = g_texture_bytes.find(handle_key(h)); it != g_texture_bytes.end())
				{
					g_life.texture.destroyed++;
					g_life.texture_bytes -= it->second;
					g_texture_bytes.erase(it);
				}
				else g_life.texture_unknown_destroys++;
			}
			return rc;
		}

		remixapi_ErrorCode REMIXAPI_PTR create_light_counted(const remixapi_LightInfo* info, remixapi_LightHandle* out)
		{
			const remixapi_ErrorCode rc = g_real.CreateLight(info, out);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS) { std::lock_guard lock(g_life_mutex); g_life.light.created++; }
			return rc;
		}

		remixapi_ErrorCode REMIXAPI_PTR destroy_light_counted(remixapi_LightHandle h)
		{
			const remixapi_ErrorCode rc = g_real.DestroyLight(h);
			if (rc == REMIXAPI_ERROR_CODE_SUCCESS) { std::lock_guard lock(g_life_mutex); g_life.light.destroyed++; }
			return rc;
		}

		// Puts the counting wrappers in front of the entries the bridge
		// filled in (an entry left null stays null).
		void count_objects(remixapi_Interface& iface)
		{
			g_real = iface;
			if (iface.CreateMesh) iface.CreateMesh = &create_mesh_counted;
			if (iface.CreateMeshBatched) iface.CreateMeshBatched = &create_mesh_batched_counted;
			if (iface.DestroyMesh) iface.DestroyMesh = &destroy_mesh_counted;
			if (iface.CreateMaterial) iface.CreateMaterial = &create_material_counted;
			if (iface.DestroyMaterial) iface.DestroyMaterial = &destroy_material_counted;
			if (iface.CreateTexture) iface.CreateTexture = &create_texture_counted;
			if (iface.DestroyTexture) iface.DestroyTexture = &destroy_texture_counted;
			if (iface.CreateLight) iface.CreateLight = &create_light_counted;
			if (iface.DestroyLight) iface.DestroyLight = &destroy_light_counted;
		}
	}

	remix_api::Lifetime remix_api::lifetime()
	{
		std::lock_guard lock(g_life_mutex);
		return g_life;
	}

	std::vector<remix_api::MeshSite> remix_api::mesh_sites(size_t top)
	{
		std::vector<std::pair<uintptr_t, Site>> all;
		{
			std::lock_guard lock(g_life_mutex);
			all.assign(g_mesh_sites.begin(), g_mesh_sites.end());
		}
		std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
			return a.second.live != b.second.live ? a.second.live > b.second.live : a.second.created > b.second.created;
		});
		std::vector<MeshSite> out;
		for (const auto& [addr, s] : all)
		{
			if (out.size() >= top) break;
			HMODULE m = nullptr;
			char name[MAX_PATH] = {};
			std::string where;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(addr), &m) && m && GetModuleFileNameA(m, name, MAX_PATH))
			{
				const char* base = std::strrchr(name, '\\');
				where = std::format("{}+0x{:X}", base ? base + 1 : name, addr - reinterpret_cast<uintptr_t>(m));
			}
			else where = std::format("0x{:X}", addr);
			out.push_back({ where, s.live, s.created, s.refreshed });
		}
		return out;
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
		count_objects(instance.m_bridge);
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
