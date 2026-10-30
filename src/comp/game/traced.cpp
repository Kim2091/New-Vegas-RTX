#include "std_include.hpp"
#include "traced.hpp"
#include "api_common.hpp"

#include "shared/common/config.hpp"

namespace comp::game::traced
{
	namespace
	{
		// Few distinct (route, reason) pairs exist: a linear table keyed by
		// pointer is cheaper than hashing strings on the draw path.
		struct Bucket
		{
			const char* route = nullptr;
			const char* reason = nullptr;
			bool d3d9 = false;
			uint64_t count = 0;
		};
		constexpr size_t kMaxBuckets = 64;
		Bucket g_buckets[kMaxBuckets];
		size_t g_used = 0;
		uint64_t g_overflow = 0;
		UINT g_frames = 0;

		void note(const char* route, const char* reason, bool d3d9)
		{
			if (!route) route = "?";
			if (!reason) reason = "?";
			for (size_t i = 0; i < g_used; i++)
			{
				Bucket& b = g_buckets[i];
				if (b.route == route && b.reason == reason && b.d3d9 == d3d9)
				{
					b.count++;
					return;
				}
			}
			if (g_used == kMaxBuckets)
			{
				g_overflow++;
				return;
			}
			g_buckets[g_used++] = Bucket{ route, reason, d3d9, 1 };

			// First time this pair is seen: once per session per pair.
			static std::unordered_set<uint64_t> s_logged;
			const uint64_t id = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(route)) << 32) ^
				static_cast<uint64_t>(reinterpret_cast<uintptr_t>(reason)) ^ (d3d9 ? 1ull : 0ull);
			if (!s_logged.insert(id).second) return;
			if (d3d9)
				shared::common::log("RemixApi", std::format(
					"traced->D3D9: {} drawn through D3D9 ({}) -- an allowed exception ({})", route, reason,
					std::strcmp(reason, "pipboy") == 0 ? "the Pip-Boy's own draws keep their D3D9 routes"
					: std::strncmp(reason, "rtx.", 4) == 0 ? "the runtime acts on that list only for D3D9 draws"
					: "no API form"),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			else
				shared::common::log("RemixApi", std::format(
					"traced->D3D9: {} NOT drawn ({}) -- no Remix-API path took it", route, reason),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
		}
	}

	void note_dropped(const char* route, const char* reason) { note(route, reason, false); }
	void note_d3d9(const char* route, const char* reason) { note(route, reason, true); }

	void on_present()
	{
		g_frames++;
		const int interval = shared::common::config::get().remix_api.stats_interval_frames;
		if (interval <= 0 || g_frames < static_cast<UINT>(interval)) return;

		const double frames = static_cast<double>(g_frames);
		uint64_t d3d9 = 0, dropped = 0;
		std::string d3d9_reasons, dropped_reasons;
		for (size_t i = 0; i < g_used; i++)
		{
			const Bucket& b = g_buckets[i];
			if (!b.count) continue;
			(b.d3d9 ? d3d9 : dropped) += b.count;
			std::string& out = b.d3d9 ? d3d9_reasons : dropped_reasons;
			out += std::format("{}{}:{}={:.1f}", out.empty() ? "" : " ", b.route, b.reason, b.count / frames);
		}
		shared::common::log("RemixApi", std::format(
			"traced->D3D9 over {} frames: d3d9={:.1f}/frame [{}] | dropped={:.1f}/frame [{}]{} | {}",
			g_frames, d3d9 / frames, d3d9_reasons.empty() ? std::string("-") : d3d9_reasons,
			dropped / frames, dropped_reasons.empty() ? std::string("-") : dropped_reasons,
			g_overflow ? std::format(" (+{} uncounted: bucket table full)", g_overflow) : std::string(),
			api::budget_status(frames)),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);

		for (size_t i = 0; i < g_used; i++) g_buckets[i].count = 0;
		g_overflow = 0;
		g_frames = 0;
	}
}
