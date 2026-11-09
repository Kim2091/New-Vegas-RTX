#include "std_include.hpp"
#include "stealth_field.hpp"
#include "scene_graph.hpp"
#include "stable_origin.hpp"

#include "shared/common/config.hpp"
#include "shared/fnv_exe.hpp"

// See stealth_field.hpp.

namespace comp::game::stealth_field
{
	namespace
	{
		using LT = shared::common::LOG_TYPE;

		constexpr float kTint[3] = { 0.90f, 0.95f, 1.0f };
		constexpr double kTintDistance = 48.0;   // game units

		constexpr uintptr_t kProcessManager = 0x11E0E80;
		constexpr uint32_t kPmActorData = 0x08;      // Actor*[]
		constexpr uint32_t kPmHighBegin = 0x14;      // level 0 (high process): [begin, end)
		constexpr uint32_t kPmHighEnd = 0x24;
		constexpr uint32_t kActorValueOwner = 0xA4;
		constexpr uint32_t kPlayerFirstPerson = 0x694;
		constexpr uint32_t kAvInvisibility = 48, kAvChameleon = 49;
		constexpr UINT kScanFrames = 4;
		constexpr uint32_t kActorsMax = 256;
		constexpr int kDepthMax = 64;
		constexpr uint32_t kLogMax = 64;

		std::unordered_set<uintptr_t> g_roots;   // compared by address only, never read
		UINT g_scan_frame = 0xFFFFFFFFu;
		size_t g_roots_logged = 0;
		uint32_t g_lines = 0;

		bool actor_value_unguarded(uintptr_t actor, uint32_t av, float& out)
		{
			using GetActorValue = float(__thiscall*)(void*, uint32_t);
			auto* owner = reinterpret_cast<void*>(actor + kActorValueOwner);
			const auto* vtable = *reinterpret_cast<const uintptr_t* const*>(owner);
			out = reinterpret_cast<GetActorValue>(vtable[3])(owner, av);
			return true;
		}

		// ActorValueOwner slot 3, the current value, as ChameleonEffect reads it.
		bool actor_value(uintptr_t actor, uint32_t av, float& out)
		{
			__try { return actor_value_unguarded(actor, av, out); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void note_actor(uintptr_t actor, bool player)
		{
			float chameleon = 0.0f, invisibility = 0.0f;
			if (!actor_value(actor, kAvChameleon, chameleon) || !actor_value(actor, kAvInvisibility, invisibility)) return;
			if (!(chameleon > 0.0f) && !(invisibility > 0.0f)) return;
			scene_graph::RefRead r;
			if (scene_graph::read_ref(actor, r) && r.root) g_roots.insert(r.root);
			uintptr_t first_person = 0;
			if (player && scene_graph::read_u32(actor + kPlayerFirstPerson, first_person) && first_person)
				g_roots.insert(first_person);
		}

		void scan()
		{
			const UINT now = api::frame();
			if (now - g_scan_frame < kScanFrames) return;
			g_scan_frame = now;
			g_roots.clear();

			uintptr_t player = 0;
			if (scene_graph::read_u32(fnv_exe::addr().player, player) && player) note_actor(player, true);

			uintptr_t data = 0;
			uint32_t begin = 0, end = 0;
			if (scene_graph::read_u32(kProcessManager + kPmActorData, data) && data &&
				scene_graph::read_u32(kProcessManager + kPmHighBegin, begin) &&
				scene_graph::read_u32(kProcessManager + kPmHighEnd, end))
			{
				for (uint32_t i = begin; i < end && i - begin < kActorsMax; i++)
				{
					uintptr_t actor = 0;
					if (scene_graph::read_u32(data + i * 4u, actor) && actor && actor != player) note_actor(actor, false);
				}
			}

			if (g_roots.size() != g_roots_logged)
			{
				g_roots_logged = g_roots.size();
				if (g_lines < kLogMax)
				{
					g_lines++;
					shared::common::log("StealthField", std::format("{} scene roots of actors under a stealth field", g_roots_logged),
						LT::LOG_TYPE_DEFAULT, false);
				}
			}
		}
	}

	bool covers(uintptr_t geometry)
	{
		if (!geometry || !shared::common::config::get().stealth_field.enabled) return false;
		scan();
		if (g_roots.empty()) return false;
		uintptr_t obj = geometry;
		for (int depth = 0; obj && depth < kDepthMax; depth++)
		{
			uintptr_t parent = 0;
			uint32_t flags = 0;
			if (!scene_graph::read_link(obj, parent, flags) || !parent) return false;
			if (g_roots.count(parent)) return true;
			obj = parent;
		}
		return false;
	}

	void fill_glass(remixapi_MaterialInfoTranslucentEXT& tr)
	{
		// A low IOR bends the view only near the silhouette and reflects almost
		// nothing (0.3 % head-on at 1.12): the actor is seen by the shimmer at its
		// edges, as with the engine's refraction. Solid, not thin-walled: a thin
		// wall doesn't bend the view at all.
		const float ior = shared::common::config::get().stealth_field.ior;
		tr = {};
		tr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
		tr.pNext = nullptr;
		tr.transmittanceTexture = L"";
		tr.refractiveIndex = ior;
		tr.transmittanceColor = { kTint[0], kTint[1], kTint[2] };
		tr.transmittanceMeasurementDistance = static_cast<float>(kTintDistance * stable_origin::scale());
		tr.thinWallThickness_hasvalue = 0;
		tr.thinWallThickness_value = 0.0f;
		tr.useDiffuseLayer = 0;
		if (g_lines < kLogMax)
		{
			g_lines++;
			shared::common::log("StealthField", std::format("glass material, IOR {:.2f}", ior), LT::LOG_TYPE_DEFAULT, false);
		}
	}
}
