#pragma once
#include "bridge_remix_api.h"
#include "remix/remix_c.h"
#include <string>
#include <vector>

namespace shared::common
{
	class remix_api
	{
	public:
		remix_api(const remix_api&) = delete;
		remix_api& operator=(const remix_api&) = delete;

		static remix_api& get();

		// __stdcall to match PFN_remixapi_BridgeCallback.
		static void __stdcall begin_scene_callback_internal();
		static void __stdcall end_scene_callback_internal();
		static void __stdcall present_callback_internal();

		static void initialize(
			PFN_remixapi_BridgeCallback begin_scene_callback,
			PFN_remixapi_BridgeCallback end_scene_callback,
			PFN_remixapi_BridgeCallback present_callback,
			bool is_asi = false);

		static bool is_initialized() { return get().m_initialized; }

		PFN_remixapi_BridgeCallback begin_scene_callback_external = nullptr;
		PFN_remixapi_BridgeCallback end_scene_callback_external = nullptr;
		PFN_remixapi_BridgeCallback present_callback_external = nullptr;

		remixapi_Interface m_bridge;

		// Every object the wrapper creates or destroys through m_bridge
		// (counted by wrappers around its Create* / Destroy* entries), for the
		// [Lifetime] log line. Texture bytes are the uploads' dataSize.
		struct Counts
		{
			uint64_t created = 0, destroyed = 0;
		};
		struct Lifetime
		{
			Counts mesh, material, texture, light;
			// CreateMesh calls that named a live mesh (MeshInfoRefreshGeometryEXT,
			// or the same hash again): not new meshes. A refresh that came back
			// with a handle not live is counted as a new mesh and in
			// mesh_refresh_new.
			uint64_t mesh_refreshed = 0;
			uint64_t mesh_recreated = 0;       // the same handle again without the refresh extension
			uint64_t mesh_refresh_new = 0;
			uint64_t mesh_unknown_destroys = 0;   // DestroyMesh of a handle not live here
			uint64_t texture_bytes = 0;        // live uploaded bytes
			uint64_t texture_unknown_destroys = 0;   // DestroyTexture of a handle not created here (imports)
		};
		static Lifetime lifetime();

		// The call sites that created the live meshes, most live first: the
		// caller's offset in its module, live meshes, creates, refreshes.
		struct MeshSite
		{
			std::string where;
			uint64_t live = 0, created = 0, refreshed = 0;
		};
		static std::vector<MeshSite> mesh_sites(size_t top);

	private:
		remix_api() : m_initialized(false) {}
		bool m_initialized;
	};
}