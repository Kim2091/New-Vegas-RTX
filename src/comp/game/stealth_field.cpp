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

		constexpr double kMeasureDistance = 48.0;   // game units; of no effect on clear glass, but must be positive

		// Shapes inside the head, by a part of their name in lower case: the
		// vanilla races' EyeLeftHuman, MouthHuman, TeethUpperHuman, Toungue (the
		// game's spelling), TongueChildM, and what replacers call theirs.
		constexpr const char* kInnerParts[] = { "eye", "mouth", "teeth", "tongue", "toungue" };
		constexpr uint32_t kObjectName = 0x08;       // NiObjectNET::m_kName

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

			const uintptr_t lists = fnv_exe::addr().process_lists;
			uintptr_t data = 0;
			uint32_t begin = 0, end = 0;
			if (lists && scene_graph::read_u32(lists + kPmActorData, data) && data &&
				scene_graph::read_u32(lists + kPmHighBegin, begin) &&
				scene_graph::read_u32(lists + kPmHighEnd, end))
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

	namespace
	{
		bool inner_part(uintptr_t geometry)
		{
			// The name is a pooled string of unknown length: a long read first,
			// a short one when that runs off its page.
			uintptr_t text = 0;
			char name[64] = {};
			if (!scene_graph::read_u32(geometry + kObjectName, text) || !text) return false;
			if (!scene_graph::read_block(text, name, sizeof(name) - 1))
			{
				std::memset(name, 0, sizeof(name));
				if (!scene_graph::read_block(text, name, 15)) return false;
			}
			for (char& c : name)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
			for (const char* inner : kInnerParts)
				if (std::strstr(name, inner)) return true;
			return false;
		}
	}

	Part part(uintptr_t geometry)
	{
		if (!geometry || !shared::common::config::get().stealth_field.enabled) return Part::None;
		scan();
		if (g_roots.empty()) return Part::None;
		uintptr_t obj = geometry;
		for (int depth = 0; obj && depth < kDepthMax; depth++)
		{
			uintptr_t parent = 0;
			uint32_t flags = 0;
			if (!scene_graph::read_link(obj, parent, flags) || !parent) return Part::None;
			if (g_roots.count(parent)) return inner_part(geometry) ? Part::Hidden : Part::Glass;
			obj = parent;
		}
		return Part::None;
	}

	void fill_glass(remixapi_MaterialInfoTranslucentEXT& tr)
	{
		// A low IOR bends the view only near the silhouette and reflects almost
		// nothing (0.06 % head-on at 1.05): the actor is seen by the shimmer at
		// its edges, as with the engine's refraction. Solid, not thin-walled: a
		// thin wall doesn't bend the view at all. Clear, with no tint: an actor is
		// many open shells, not one closed body, so a ray often stays "inside the
		// glass" long after it has left the actor, and any absorption would then
		// darken the whole view behind the figure.
		const float ior = shared::common::config::get().stealth_field.ior;
		tr = {};
		tr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
		tr.pNext = nullptr;
		tr.transmittanceTexture = L"";
		tr.refractiveIndex = ior;
		tr.transmittanceColor = { 1.0f, 1.0f, 1.0f };
		tr.transmittanceMeasurementDistance = static_cast<float>(kMeasureDistance * stable_origin::scale());
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
