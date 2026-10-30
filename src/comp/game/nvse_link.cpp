#include "std_include.hpp"
#include "nvse_link.hpp"

#include "fault_report.hpp"
#include "scene_statics.hpp"

#include "shared/common/ffp_state.hpp"

#pragma comment(lib, "version.lib")


#ifndef FNVR_COMMIT
#define FNVR_COMMIT unknown
#endif
#define FNVR_STR2(x) #x
#define FNVR_STR(x) FNVR_STR2(x)

namespace comp::game::nvse_link
{
	namespace
	{
		// BeginScenes (frames) the search runs before the plugin counts as
		// absent while xNVSE is loaded. xNVSE loads its plugins before the
		// game creates its device, so the plugin is there at the first
		// BeginScene or not at all; the margin only covers a slow loader.
		// Without nvse_1_4.dll the plugin counts as absent at once.
		constexpr UINT kSearchFrames = 300;
		constexpr uint32_t kMaxEvents = 1024;
		constexpr uint32_t kXnvse648 = (6u << 24) | (4u << 16) | (8u << 4);
		// Features the plugin checks while the game runs, not only by its code-site integrity check.
		constexpr uint32_t kRuntimeChecked = FNVR_F_CAMERA | FNVR_F_MENU | FNVR_F_OWNERS | FNVR_F_LIGHTS | FNVR_F_WATER;

		enum class State : uint8_t { Searching, Linked, Absent, Refused };
		State g_state = State::Searching;
		UINT g_search_frames = 0;
		UINT g_search_frame = 0xFFFFFFFFu;
		const FnvRemixApi* g_api = nullptr;
		std::string g_problem;

		FnvRemixFacts g_facts = {};          // the last ready facts (zero before the first)
		UINT g_facts_frame = 0xFFFFFFFFu;    // the frame g_facts were read in
		bool g_ready = false;
		DWORD g_main_thread = 0;             // the thread the last ready facts came on
		uint32_t g_not_ready = 0;            // calls answered not ready after the first ready one
		bool g_logged_waiting = false;
		static constexpr uint32_t kMaxCells = 64;
		FnvRemixCell g_cells[kMaxCells];     // the last cell list read on the main thread
		uint32_t g_cell_count = 0;
		uint32_t g_last_features = 0xFFFFFFFFu;
		uint8_t g_last_game_state = 0xFF;

		FnvRemixEvent g_events[kMaxEvents];

		// Calls into the plugin and their time (call_totals). facts() is also
		// called from the loading screen's thread.
		std::atomic<uint64_t> g_calls { 0 };
		std::atomic<int64_t> g_call_ticks { 0 };

		struct CallTimer
		{
			LARGE_INTEGER start;
			CallTimer() { QueryPerformanceCounter(&start); }
			~CallTimer()
			{
				LARGE_INTEGER end;
				QueryPerformanceCounter(&end);
				g_calls.fetch_add(1, std::memory_order_relaxed);
				g_call_ticks.fetch_add(end.QuadPart - start.QuadPart, std::memory_order_relaxed);
			}
			CallTimer(const CallTimer&) = delete;
			CallTimer& operator=(const CallTimer&) = delete;
		};
		uint32_t g_cells_in = 0, g_cells_out = 0, g_refs_loaded[4] = {};
		uint32_t g_owners_gone = 0;
		UINT g_owner_log_frame = 0;

		UINT frame_now()
		{
			return shared::common::ffp_state::get().frame_count();
		}

		// xNVSE's version from nvse_1_4.dll's version resource (it reads
		// 0, major, minor, beta); empty when it isn't loaded.
		std::string xnvse_file_version(uint32_t& packed)
		{
			packed = 0;
			HMODULE m = GetModuleHandleW(L"nvse_1_4.dll");
			if (!m) return {};
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(m, path, MAX_PATH);
			DWORD handle = 0;
			const DWORD size = GetFileVersionInfoSizeW(path, &handle);
			if (!size) return "unknown version";
			std::vector<uint8_t> buf(size);
			VS_FIXEDFILEINFO* fi = nullptr;
			UINT len = 0;
			if (!GetFileVersionInfoW(path, 0, size, buf.data()) ||
				!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
				return "unknown version";
			const uint32_t major = LOWORD(fi->dwFileVersionMS), minor = HIWORD(fi->dwFileVersionLS),
				beta = LOWORD(fi->dwFileVersionLS);
			packed = (major << 24) | (minor << 16) | (beta << 4);
			return std::format("{}.{}.{}", major, minor, beta);
		}

		std::string version_name(uint32_t v)
		{
			return std::format("{}.{}.{}", v >> 24, (v >> 16) & 0xFF, (v >> 4) & 0xFFF);
		}

		std::string feature_names(uint32_t bits)
		{
			static constexpr std::pair<uint32_t, const char*> kNames[] = {
				{ FNVR_F_CELLS, "cells" }, { FNVR_F_LOADING, "loading" }, { FNVR_F_CAMERA, "camera" },
				{ FNVR_F_MENU, "menu" }, { FNVR_F_OWNERS, "owners" }, { FNVR_F_LIGHTS, "lights" },
				{ FNVR_F_DECALS, "decals" }, { FNVR_F_MENU_BG, "menu-bg" }, { FNVR_F_WATER, "water" },
				{ FNVR_F_HOOK_P1, "P1" }, { FNVR_F_HOOK_P2, "P2" }, { FNVR_F_HOOK_P3, "P3" }, { FNVR_F_HOOK_P5, "P5" },
			};
			std::string s;
			for (const auto& [bit, name] : kNames)
				if (bits & bit) s += std::format("{}{}", s.empty() ? "" : " ", name);
			return s.empty() ? std::string("none") : s;
		}

		void give_up(const std::string& why, fault_report::Fault fault)
		{
			g_problem = why;
			shared::common::log("NvseLink", why, shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			fault_report::report(fault);
		}

		void absent()
		{
			g_state = State::Absent;
			uint32_t packed = 0;
			const std::string xv = xnvse_file_version(packed);
			const char* off = "Engine features OFF: scene statics, keep-culled, cell residency, the stable origin, "
				"Pip-Boy live background.";
			if (xv.empty())
				give_up(std::format("fnv_remix_nvse.dll not loaded (xNVSE is not running). {} Run the game through xNVSE "
					"6.4.8 or newer and install Data\\NVSE\\Plugins\\fnv_remix_nvse.dll from the same build as this d3d9.dll.",
					off), fault_report::Fault::XnvseMissing);
			else if (packed && packed < kXnvse648)
				give_up(std::format("fnv_remix_nvse.dll not loaded: xNVSE {} is older than 6.4.8 and the plugin refuses it "
					"-- update xNVSE. {}", xv, off), fault_report::Fault::XnvseOld);
			else
				give_up(std::format("fnv_remix_nvse.dll not loaded (xNVSE {} present). {} Install "
					"Data\\NVSE\\Plugins\\fnv_remix_nvse.dll from the same build as this d3d9.dll.", xv, off),
					fault_report::Fault::PluginMissing);
		}

		void try_link()
		{
			HMODULE m = GetModuleHandleW(L"fnv_remix_nvse.dll");
			if (!m) return;
			const auto get = reinterpret_cast<PFN_FnvRemix_GetApi>(GetProcAddress(m, "FnvRemix_GetApi"));
			if (!get)
			{
				g_state = State::Refused;
				give_up("fnv_remix_nvse.dll is loaded but exports no FnvRemix_GetApi: a foreign or broken build. "
					"Engine features OFF.", fault_report::Fault::PluginMismatch);
				return;
			}
			const FnvRemixApi* api = get(FNV_REMIX_ABI);
			if (!api || api->abi != FNV_REMIX_ABI || !api->facts || !api->cells || !api->drain_events || !api->owner_of ||
				!api->lights || !api->take_members || !api->place || !api->load_order || !api->log_line)
			{
				g_state = State::Refused;
				give_up(std::format("fnv_remix_nvse.dll refused ABI {} (its commit is in fnv_remix_nvse.log), wrapper "
					"commit {}: rebuild both from one commit. Engine features OFF.", FNV_REMIX_ABI, FNVR_STR(FNVR_COMMIT)),
					fault_report::Fault::PluginMismatch);
				return;
			}
			g_api = api;
			g_state = State::Linked;
			g_problem.clear();
			const std::string pc = api->plugin_commit ? api->plugin_commit : "?";
			const std::string wc = FNVR_STR(FNVR_COMMIT);
			shared::common::log_event("NvseLink", std::format("plugin {}, ABI {}, xNVSE {}, features built: {}{}", pc, api->abi,
				version_name(api->xnvse_version), feature_names(api->features_built),
				pc == wc ? std::string() : std::format(" -- WARNING: wrapper commit {} differs, deploy both from one build", wc)),
				pc == wc ? shared::common::LOG_TYPE::LOG_TYPE_GREEN : shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}

		const char* game_state_name(uint8_t s)
		{
			switch (s)
			{
			case FNVR_GS_MAIN_MENU: return "main menu";
			case FNVR_GS_LOADING: return "loading";
			case FNVR_GS_IN_GAME: return "in game";
			case FNVR_GS_MENU: return "menu";
			default: return "?";
			}
		}

		const char* cut_name(uintptr_t r)
		{
			switch (r)
			{
			case FNVR_CUT_LOAD: return "load ended";
			case FNVR_CUT_NEW_GAME: return "new game";
			case FNVR_CUT_MAIN_MENU: return "main menu";
			case FNVR_CUT_WORLD_CHANGE: return "interior / worldspace change";
			case FNVR_CUT_TELEPORT: return "teleport";
			default: return "?";
			}
		}

		void note_changes(const FnvRemixFacts& f)
		{
			if (f.features_ok != g_last_features)
			{
				std::string off;
				for (uint32_t b = 0; b < 32; b++)
				{
					const uint32_t bit = 1u << b;
					if (!(g_api->features_built & bit) || (f.features_ok & bit)) continue;
					const char* why = g_api->feature_status ? g_api->feature_status(bit) : nullptr;
					off += std::format(" | {} off: {}", feature_names(bit), why && *why ? why : "?");
					// Only the code-site verdicts of the integrity check name a patch by another mod.
					if (!(bit & kRuntimeChecked) && why && std::strncmp(why, "OFF", 3) == 0)
						fault_report::report(fault_report::Fault::FeatureOff, feature_names(bit));
				}
				shared::common::log_event("NvseLink", std::format("features ok: {}{}", feature_names(f.features_ok), off),
					off.empty() ? shared::common::LOG_TYPE::LOG_TYPE_STATUS : shared::common::LOG_TYPE::LOG_TYPE_WARN);
				g_last_features = f.features_ok;
			}
			if (f.game_state != g_last_game_state)
			{
				shared::common::log_event("NvseLink", std::format("game state {} (Pip-Boy {}, cut #{}, {} settled, pins {})",
					game_state_name(f.game_state), f.pipboy_raw, f.cut_serial, f.settled ? "" : "not", f.pins));
				g_last_game_state = f.game_state;
			}
		}
	}

	void on_begin_scene()
	{
		if (g_state != State::Searching) return;
		const UINT now = frame_now();
		if (now == g_search_frame) return;
		g_search_frame = now;
		try_link();
		// Without xNVSE loaded by now the plugin cannot come.
		if (g_state == State::Searching && (!GetModuleHandleW(L"nvse_1_4.dll") || ++g_search_frames >= kSearchFrames))
			absent();
	}

	bool linked()
	{
		return g_state == State::Linked;
	}

	const FnvRemixFacts& facts()
	{
		if (g_state != State::Linked) return g_facts;
		const UINT now = frame_now();
		if (now == g_facts_frame && GetCurrentThreadId() == g_main_thread) return g_facts;
		FnvRemixFacts f;
		{
			const CallTimer t;
			g_api->facts(&f);
		}
		if (!f.ready)
		{
			if (!g_ready)
			{
				if (!g_logged_waiting)
				{
					g_logged_waiting = true;
					shared::common::log("NvseLink", "plugin linked, waiting for its startup report (xNVSE DeferredInit) -- "
						"engine features wait until then", shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
				}
			}
			else if (g_not_ready++ % 1000 == 0)
				shared::common::log("NvseLink", std::format("facts asked from thread {} (the plugin answers on thread {}): "
					"the last ready facts stand ({} such calls so far)", GetCurrentThreadId(), g_main_thread, g_not_ready),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			return g_facts;
		}
		g_facts = f;
		g_facts_frame = now;
		g_main_thread = GetCurrentThreadId();
		if (!g_ready)
		{
			g_ready = true;
			shared::common::log_event("NvseLink", std::format("plugin ready (startup report done), thread {}", g_main_thread));
		}
		note_changes(g_facts);
		return g_facts;
	}

	bool live()
	{
		if (!linked()) return false;
		facts();
		return g_ready && GetCurrentThreadId() == g_main_thread;
	}

	bool has(uint32_t features)
	{
		return linked() && (facts().features_ok & features) == features && g_ready;
	}

	uint32_t cells(FnvRemixCell* out, uint32_t max)
	{
		if (!has(FNVR_F_CELLS)) return 0;
		if (live())
		{
			const CallTimer t;
			g_cell_count = g_api->cells(g_cells, kMaxCells);
		}
		const uint32_t n = (std::min)(g_cell_count, max);
		std::memcpy(out, g_cells, n * sizeof(FnvRemixCell));
		return n;
	}

	bool owner_of(uintptr_t geo, FnvRemixOwner& out)
	{
		if (!geo || !has(FNVR_F_OWNERS) || !live()) return false;
		const CallTimer t;
		return g_api->owner_of(geo, &out);
	}

	uint32_t take_members(FnvRemixMember* out, uint32_t max)
	{
		if (!out || !max || !has(FNVR_F_OWNERS) || !live()) return 0;
		const CallTimer t;
		return g_api->take_members(out, max);
	}

	bool resend_members()
	{
		if (!has(FNVR_F_OWNERS) || !live()) return false;
		const CallTimer t;
		g_api->take_members(nullptr, 0);
		return true;
	}

	uint32_t lights(FnvRemixLight* out, uint32_t max)
	{
		if (!has(FNVR_F_LIGHTS) || !live()) return 0;
		const CallTimer t;
		return g_api->lights(out, max);
	}

	bool place(FnvRemixPlace& out)
	{
		if (!live()) return false;
		const CallTimer t;
		return g_api->place(&out);
	}

	std::string load_order()
	{
		if (!live()) return {};
		std::string list(64 * 1024, '\0');
		uint32_t n = 0;
		{
			const CallTimer t;
			n = g_api->load_order(list.data(), static_cast<uint32_t>(list.size()));
		}
		if (n >= list.size())
		{
			list.assign(n + 1, '\0');
			const CallTimer t;
			n = (std::min)(g_api->load_order(list.data(), static_cast<uint32_t>(list.size())),
				static_cast<uint32_t>(list.size() - 1));
		}
		list.resize(n);
		return list;
	}

	void plugin_log(const char* tag, const std::string& text)
	{
		if (g_state == State::Linked) g_api->log_line(tag, text.c_str());
	}

	void on_present()
	{
		if (g_state != State::Linked || !live()) return;
		uint32_t n = 0;
		{
			const CallTimer t;
			n = g_api->drain_events(g_events, kMaxEvents);
		}
		for (uint32_t i = 0; i < n; i++)
		{
			const FnvRemixEvent& e = g_events[i];
			switch (e.type)
			{
			case FNVR_EV_CELL_ATTACHED: g_cells_in++; break;
			case FNVR_EV_CELL_DETACHED: g_cells_out++; break;
			case FNVR_EV_CELL_REFS_LOADED:
				if (e.b < 4) g_refs_loaded[e.b]++;
				scene_statics::on_cell_refs_loaded(e.a);
				break;
			case FNVR_EV_LOAD_BEGIN:
				shared::common::log_event("NvseLink", "load begins");
				break;
			case FNVR_EV_LOAD_END:
				shared::common::log_event("NvseLink", "load ends");
				break;
			case FNVR_EV_CUT:
				shared::common::log_event("NvseLink", std::format("cut ({}{}), now cut #{}", cut_name(e.a),
					e.a == FNVR_CUT_TELEPORT ? std::format(", {} units", e.b) : std::string(), facts().cut_serial));
				break;
			case FNVR_EV_OWNER_GONE:
				g_owners_gone++;
				scene_statics::on_owner_gone(e.a, e.b);
				break;
			case FNVR_EV_OVERFLOW:
				shared::common::log("NvseLink", "the plugin's event queue overflowed: events lost, consumers resync from "
					"the serials", shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
				break;
			default:
				break;
			}
		}
		if (g_cells_in || g_cells_out)
		{
			const auto& f = facts();
			shared::common::log("NvseLink", std::format("cells: +{} -{} (serial {}, {}{}), refs loaded by event {} / "
				"poll {} / timeout {}", g_cells_in, g_cells_out, f.cells_serial, f.interior ? "interior" : "exterior",
				f.interior ? std::string() : std::format(" grid ({}, {}) dim {}", f.grid_cx, f.grid_cy, f.grid_dim),
				g_refs_loaded[FNVR_RL_EVENT], g_refs_loaded[FNVR_RL_POLL], g_refs_loaded[FNVR_RL_TIMEOUT]),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			g_cells_in = g_cells_out = 0;
		}
		const UINT now = frame_now();
		if (now - g_owner_log_frame >= 600)
		{
			g_owner_log_frame = now;
			const auto& f = facts();
			shared::common::log("NvseLink", std::format("owner map: {} references, {} geometries, gone +{} | pins {}",
				f.owners, f.owner_geos, g_owners_gone, f.pins), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			g_owners_gone = 0;
		}
	}

	const std::string& problem()
	{
		return g_problem;
	}

	void call_totals(uint64_t& calls, int64_t& ticks)
	{
		calls = g_calls.load(std::memory_order_relaxed);
		ticks = g_call_ticks.load(std::memory_order_relaxed);
	}
}
