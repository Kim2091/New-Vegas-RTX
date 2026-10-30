#include "std_include.hpp"
#include "vats.hpp"

#include "api_common.hpp"
#include "call_chain.hpp"
#include "structs.hpp"
#include "shared/common/config.hpp"
#include "shared/fnv_exe.hpp"

namespace comp::game::vats
{
	namespace
	{
		// VATSEffect's per-limb visibility (fnv_exe vats_limb_visibility).
		// The engine's answer is false with *out = -1 while its queries are
		// pending. Its one caller, the VATS menu's limb update (0x7F3E00),
		// asks once for each pending limb of the current target, stores *out
		// as the limb's visibility when the answer is true and computes the
		// hit chance from it.
		using LimbVisibility_t = bool(__fastcall*)(void* self, void* edx, int limb, float* out);
		LimbVisibility_t g_orig_limb_visibility = nullptr;
		bool g_hooked = false;

		using LosToPoint_t = bool(__thiscall*)(void* actor, const float* point, bool eye_height, bool skip_fov);
		using EyeHeight_t = float(__thiscall*)(void* actor, bool eye_height);
		using BypassGet_t = bool(__cdecl*)();
		using BypassSet_t = void(__cdecl*)(bool);
		using Get3D_t = void*(__thiscall*)(void* ref);

		// A tList node. The list's head is a node held in place; an empty
		// list is a head with no item and no next.
		struct ListNode
		{
			void* item;
			const ListNode* next;
		};

		// VATSTargetInfo (0x24 bytes) and VATSLimbInfo (0x3C bytes).
		constexpr uint32_t TARGET_REF_OFF   = 0x00;   // TESObjectREFR*
		constexpr uint32_t TARGET_TYPE_OFF  = 0x04;   // 1 an actor's limbs, 2 the whole object
		constexpr uint32_t TARGET_LIMBS_OFF = 0x08;   // tList<VATSLimbInfo*>
		constexpr uint32_t LIMB_POS_OFF     = 0x14;   // NiPoint3, the limb's world position
		constexpr uint32_t LIMB_INDEX_OFF   = 0x20;   // BGSBodyPartData index 0..14 (14 the weapon), -1 the whole object
		constexpr int TARGET_WHOLE = 2;

		// TESObjectREFR's vtable slot that returns its 3D root, and
		// NiAVObject's world bound (NiBound*: centre xyz, radius).
		constexpr uint32_t REF_GET_3D_SLOT = 0x1D0;
		constexpr uint32_t NIAV_BOUND_OFF  = 0x20;
		constexpr uint32_t REF_POS_OFF     = 0x30;   // NiPoint3, the reference's position

		constexpr int MAX_SAMPLES = 16;
		constexpr int MAX_LIST_STEPS = 256;
		constexpr int MAX_CACHED = 32;
		constexpr DWORD LOG_INTERVAL_MS = 1000;
		constexpr float kTwoPi = 6.28318531f;

		const shared::common::config::vats_settings& cfg()
		{
			return shared::common::config::get().vats;
		}

		// What the rays aim at. The points sit on a ring of `ring` units
		// around `centre`, moved `pull` units towards the player's eye.
		struct Aim
		{
			float centre[3];
			float ring;
			float pull;
		};

		// One limb's estimate.
		struct Estimate
		{
			void* ref = nullptr;
			int limb = 0;
			int clear = 0;
			int rays = 0;
			float vis = 1.0f;
		};

		// The pick time budget's bypass as the estimate found it; restored
		// after its rays, and by the exception handler.
		bool g_bypass_prev = false;
		bool g_bypass_on = false;

		// This frame's estimates by target and limb.
		struct Cached
		{
			void* ref;
			int limb;
			float vis;
		};
		Cached g_cached[MAX_CACHED];
		int g_cached_n = 0;
		UINT g_cached_frame = 0xFFFFFFFFu;

		// This frame's estimates for the verbose line.
		Estimate g_batch[MAX_CACHED];
		int g_batch_n = 0;
		DWORD g_last_log_tick = 0;
		bool g_logged_first = false;
		uint64_t g_faults = 0;

		bool zero(const float* p)
		{
			return p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f;
		}

		// The whole object's world bound: its front towards the eye, half
		// its radius around. False without 3D.
		bool bound_aim(void* ref, Aim& aim)
		{
			const auto* vt = *reinterpret_cast<const uintptr_t* const*>(ref);
			const auto get_3d = reinterpret_cast<Get3D_t>(vt[REF_GET_3D_SLOT / 4]);
			void* root = get_3d(ref);
			if (!is_valid_ptr(root)) return false;
			const auto* b = *reinterpret_cast<const float* const*>(reinterpret_cast<uintptr_t>(root) + NIAV_BOUND_OFF);
			if (!is_valid_ptr(b) || !std::isfinite(b[3]) || b[3] <= 0.0f) return false;
			aim.centre[0] = b[0];
			aim.centre[1] = b[1];
			aim.centre[2] = b[2];
			aim.ring = 0.5f * b[3];
			aim.pull = b[3];
			return true;
		}

		// The current target's limb `limb` (the VATS target list). False
		// when there is no target, no such limb or no position.
		bool find_aim(int limb, void*& ref_out, Aim& aim)
		{
			const auto& a = fnv_exe::addr();
			void* ref = *reinterpret_cast<void* const*>(a.vats_target_ref);
			if (!is_live_object(ref)) return false;
			ref_out = ref;

			const auto* node = reinterpret_cast<const ListNode*>(a.vats_targets);
			for (int steps = 0; node && steps < MAX_LIST_STEPS; node = node->next, steps++)
			{
				const auto t = reinterpret_cast<uintptr_t>(node->item);
				if (!is_valid_ptr(node->item) || *reinterpret_cast<void* const*>(t + TARGET_REF_OFF) != ref) continue;
				if (*reinterpret_cast<const int*>(t + TARGET_TYPE_OFF) == TARGET_WHOLE || limb < 0)
					return bound_aim(ref, aim);

				const auto* ln = reinterpret_cast<const ListNode*>(t + TARGET_LIMBS_OFF);
				for (int k = 0; ln && k < MAX_LIST_STEPS; ln = ln->next, k++)
				{
					const auto l = reinterpret_cast<uintptr_t>(ln->item);
					if (!is_valid_ptr(ln->item) || *reinterpret_cast<const int*>(l + LIMB_INDEX_OFF) != limb) continue;
					const auto* p = reinterpret_cast<const float*>(l + LIMB_POS_OFF);
					// The menu fills the position after its first visibility
					// answer; until then the object's bound centre stands in.
					if (zero(p) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
					{
						if (!bound_aim(ref, aim)) return false;
						aim.pull = 0.0f;
						aim.ring = cfg().sample_radius;
						return true;
					}
					aim.centre[0] = p[0];
					aim.centre[1] = p[1];
					aim.centre[2] = p[2];
					aim.ring = cfg().sample_radius;
					aim.pull = 0.0f;
					return true;
				}
				return false;
			}
			return false;
		}

		// Rays from the player to the aim's points; e.clear of e.rays are
		// not blocked. False when nothing can be cast. No C++ objects here:
		// it runs under __try (estimate).
		bool estimate_unguarded(int limb, Estimate& e)
		{
			const auto& a = fnv_exe::addr();
			void* player = *reinterpret_cast<void* const*>(a.player);
			if (!is_live_object(player)) return false;

			Aim aim = {};
			if (!find_aim(limb, e.ref, aim)) return false;

			// The rays start at the player's eye, and the ring faces it.
			const float* pos = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(player) + REF_POS_OFF);
			const float eye_z = pos[2] + reinterpret_cast<EyeHeight_t>(a.actor_eye_height)(player, true);
			float d[3] = { aim.centre[0] - pos[0], aim.centre[1] - pos[1], aim.centre[2] - eye_z };
			const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (!std::isfinite(dist) || dist < 1.0f) return false;
			for (float& x : d) x /= dist;

			const float pull = (std::min)(aim.pull, 0.5f * dist);
			const float c[3] = { aim.centre[0] - d[0] * pull, aim.centre[1] - d[1] * pull, aim.centre[2] - d[2] * pull };
			// u = d x up, v = u x d: the ring's plane.
			float u[3] = { d[1], -d[0], 0.0f };
			float ul = std::sqrt(u[0] * u[0] + u[1] * u[1]);
			if (ul < 1.0e-3f)
			{
				u[0] = 1.0f;
				u[1] = 0.0f;
				ul = 1.0f;
			}
			u[0] /= ul;
			u[1] /= ul;
			const float v[3] = { u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0] };

			const auto los = reinterpret_cast<LosToPoint_t>(a.actor_los_to_point);
			const auto bypass_get = reinterpret_cast<BypassGet_t>(a.pick_bypass_get);
			const auto bypass_set = reinterpret_cast<BypassSet_t>(a.pick_bypass_set);
			const int n = std::clamp(cfg().sample_count, 1, MAX_SAMPLES);
			const float ring = aim.ring;

			// Picks past the frame's time budget are skipped unless the
			// bypass is on, as the crosshair pick does it.
			g_bypass_prev = bypass_get();
			g_bypass_on = true;
			bypass_set(true);
			for (int k = 0; k < n; k++)
			{
				float pt[3] = { c[0], c[1], c[2] };
				if (k > 0)
				{
					const float t = kTwoPi * static_cast<float>(k - 1) / static_cast<float>(n - 1);
					const float cu = std::cos(t) * ring, sv = std::sin(t) * ring;
					for (int i = 0; i < 3; i++) pt[i] += cu * u[i] + sv * v[i];
				}
				// Eye height, no field-of-view test.
				if (los(player, pt, true, true)) e.clear++;
				e.rays++;
			}
			bypass_set(g_bypass_prev);
			g_bypass_on = false;
			return e.rays > 0;
		}

		bool estimate(int limb, Estimate& e)
		{
			__try { return estimate_unguarded(limb, e); }
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				if (g_bypass_on)
				{
					g_bypass_on = false;
					reinterpret_cast<BypassSet_t>(fnv_exe::addr().pick_bypass_set)(g_bypass_prev);
				}
				e.clear = 0;
				e.rays = 0;
				g_faults++;
				return false;
			}
		}

		float visibility(int limb)
		{
			const UINT now = api::frame();
			if (now != g_cached_frame)
			{
				g_cached_frame = now;
				g_cached_n = 0;
			}
			void* ref = *reinterpret_cast<void* const*>(fnv_exe::addr().vats_target_ref);
			for (int i = 0; i < g_cached_n; i++)
				if (g_cached[i].ref == ref && g_cached[i].limb == limb) return g_cached[i].vis;

			Estimate e;
			e.limb = limb;
			if (estimate(limb, e)) e.vis = static_cast<float>(e.clear) / static_cast<float>(e.rays);
			else e.vis = 1.0f;

			if (g_cached_n < MAX_CACHED) g_cached[g_cached_n++] = { ref, limb, e.vis };
			if (g_batch_n < MAX_CACHED) g_batch[g_batch_n++] = e;
			if (!g_logged_first)
			{
				g_logged_first = true;
				shared::common::log("VATS", std::format("first limb visibility: limb {} {:.2f} ({} of {} rays clear{})",
					limb, e.vis, e.clear, e.rays, e.rays ? "" : "; no line of sight could be cast, so 1"));
			}
			return e.vis;
		}

		bool __fastcall limb_visibility_hk(void* self, void* edx, int limb, float* out)
		{
			if (!cfg().enabled || !out) return g_orig_limb_visibility(self, edx, limb, out);
			*out = visibility(limb);
			return true;
		}
	}

	void install()
	{
		if (!cfg().enabled)
		{
			shared::common::log("VATS", "limb visibility hook skipped ([VATS] Enabled=0): guns read 0% hit chance "
				"under Remix");
			return;
		}
		const auto& a = fnv_exe::addr();
		if (!fnv_exe::known() || !a.actor_los_to_point || !a.actor_eye_height || !a.pick_bypass_get ||
			!a.pick_bypass_set || !a.vats_targets || !a.vats_target_ref || !a.player)
		{
			shared::common::log("VATS", "limb visibility hook skipped (unknown executable): guns read 0% hit chance "
				"under Remix", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		const fnv_exe::Code& fn = a.vats_limb_visibility;
		if (!call_chain::hook_entry(fn, reinterpret_cast<void*>(&limb_visibility_hk),
			reinterpret_cast<void**>(&g_orig_limb_visibility), "VATS", "VATSEffect limb visibility",
			"VATS hit chance with guns is estimated only if that code still calls the engine's limb visibility"))
		{
			shared::common::log("VATS", "limb visibility hook skipped (the function is patched by another module): "
				"guns read 0% hit chance under Remix", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		g_hooked = true;
		shared::common::log("VATS", std::format("limb visibility hook installed @0x{:X}: line of sight, {} rays per "
			"limb on a {:.1f}-unit ring", fn.addr, cfg().sample_count, cfg().sample_radius));
	}

	bool hook_installed()
	{
		return g_hooked;
	}

	void on_frame_end()
	{
		if (!g_batch_n) return;
		const DWORD tick = GetTickCount();
		if (shared::common::log_verbose() && tick - g_last_log_tick >= LOG_INTERVAL_MS)
		{
			g_last_log_tick = tick;
			std::string limbs;
			for (int i = 0; i < g_batch_n; i++)
				limbs += std::format("{}{}={:.2f} ({}/{})", i ? ", " : "", g_batch[i].limb, g_batch[i].vis,
					g_batch[i].clear, g_batch[i].rays);
			shared::common::log("VATS", std::format("target 0x{:X}: {}{}", reinterpret_cast<uintptr_t>(g_batch[0].ref),
				limbs, g_faults ? std::format("; {} estimates faulted since startup", g_faults) : std::string()));
		}
		g_batch_n = 0;
	}
}
