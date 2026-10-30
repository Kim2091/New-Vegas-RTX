#include "std_include.hpp"
#include "fault_report.hpp"

#include "api_common.hpp"
#include "nvse_link.hpp"
#include "session_archive.hpp"
#include "session_files.hpp"

#include "shared/common/remix_api.hpp"

namespace comp::game::fault_report
{
	namespace
	{
		struct Kind
		{
			const char* id;
			const char* text;   // what the player reads in the notice
		};

		// Indexed by Fault.
		constexpr Kind kKinds[] = {
			{ "xnvse-missing", "xNVSE is not running, so New Vegas RTX and most other mods cannot work, and the game can "
				"crash or render wrongly. Start the game through nvse_loader.exe, or apply the FNV 4GB Patcher to "
				"FalloutNV.exe, which makes it load xNVSE itself (xNVSE 6.4.8 or newer). In Mod Organizer 2, run the "
				"executable that does one of these." },
			{ "no-4gb", "FalloutNV.exe is not 4GB patched, so the game has only 2 GB of memory and crashes with large "
				"mod lists. Apply the FNV 4GB Patcher to FalloutNV.exe." },
			{ "xnvse-old", "xNVSE is older than 6.4.8. Update xNVSE." },
			{ "plugin-missing", "The mod's xNVSE plugin, Data\\NVSE\\Plugins\\fnv_remix_nvse.dll, did not load. Reinstall the mod." },
			{ "plugin-mismatch", "d3d9.dll and Data\\NVSE\\Plugins\\fnv_remix_nvse.dll come from different versions of the mod. "
				"Reinstall the mod so both match." },
			{ "feature-off", "Another mod changed game code this mod relies on, so these features are off:" },
			{ "unknown-exe", "FalloutNV.exe is not a version this mod knows (Steam or GOG 1.4.0.525, the 4GB patch is fine), "
				"so its game patches are off." },
			{ "d3d9ex", "The game's Direct3D device was created as D3D9Ex, so its textures cannot reach the path tracer "
				"and the world does not render." },
			{ "incompatible-dll", "A mod that does not work with this mod is loaded:" },
			{ "empty-scene", "The game world is not reaching the path tracer: nearly every draw was refused for 20 seconds." },
			{ "plugin-read-faults", "The mod's xNVSE plugin keeps failing to read game memory, which points to another mod "
				"changing the game's data." },
			{ "game-crashed", "The game crashed in the previous session:" },
		};
		static_assert(std::size(kKinds) == static_cast<size_t>(Fault::Count));

		// The mods docs/INCOMPATIBLE_MODS.md lists, by a file they load (a DLL
		// or a plugin file, compared without case) or by the ProductName in a
		// DLL's version resource.
		struct Incompatible
		{
			const char* file;
			const char* product;
			const char* mod;
		};
		constexpr Incompatible kIncompatible[] = {
			{ "NewVegasReloaded.dll", nullptr, "New Vegas Reloaded" },
			{ "NewVegasRTXHelper.dll", nullptr, "NewVegasRTXHelper" },
			{ "RealTimeMenus.dll", nullptr, "Real Time Menus" },
			{ "MobilePipBoyLight.dll", nullptr, "Mobile Pip-Boy Light" },
			{ "Fallout Shader Loader.dll", nullptr, "Shader Loader" },
			{ nullptr, "ReShade", "ReShade" },
		};

		constexpr UINT kWarmupFrames = 120;            // let the bridge finish its handshake
		constexpr DWORD kEmptySceneMs = 20000;
		constexpr uint32_t kEmptySceneRefused = 100;   // refused draws per frame
		constexpr uint32_t kReadFaults = 100;
		constexpr DWORD kCrashWindowMs = 3000;       // a crash later than this after a Present is a shutdown crash
		constexpr DWORD kArchiveWaitMs = 60000;      // the longest the crash report waits for the previous session's zip

		std::mutex g_mutex;
		std::string g_details[static_cast<size_t>(Fault::Count)];
		bool g_reported[static_cast<size_t>(Fault::Count)] = {};
		bool g_dirty = false;
		UINT g_frames = 0;
		DWORD g_empty_since = 0;
		volatile DWORD g_last_present_ms = 0;
		wchar_t g_marker[MAX_PATH] = {};
		DWORD g_first_ms = 0;
		// The previous session's crash, read from its marker and reported
		// once its zip is made.
		bool g_crash_pending = false;
		std::string g_crash_header;
		std::string g_crash_file;
		uint64_t g_crash_time = 0;
		// The zip to upload, as rtx.faultNotice.uploadFile; empty until known.
		std::string g_upload_file;

		bool reported(Fault f)
		{
			std::lock_guard lock(g_mutex);
			return g_reported[static_cast<size_t>(f)];
		}

		// FalloutNV.exe's image header, as loaded, lacks the large-address-aware flag.
		void check_address_space()
		{
			const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)) return;
			MEMORYSTATUSEX ms = {};
			ms.dwLength = sizeof(ms);
			report(Fault::NoLargeAddress, GlobalMemoryStatusEx(&ms)
				? std::format("(it can address {} MiB)", ms.ullTotalVirtual >> 20) : std::string());
		}

		// This session's marker path is kept for the crash filter, and the
		// previous session's marker is read: the exception, the crash log's
		// file name and the crash's UTC file time, one per line.
		void start()
		{
			check_address_space();
			const std::wstring path = shared::common::log_dir() + L"game-crash.marker";
			wcsncpy_s(g_marker, path.c_str(), _TRUNCATE);
			std::ifstream marker{ std::filesystem::path(path) };
			std::string time;
			if (!marker || !std::getline(marker, g_crash_header) || g_crash_header.empty()) return;
			std::getline(marker, g_crash_file);
			if (std::getline(marker, time)) g_crash_time = _strtoui64(time.c_str(), nullptr, 10);
			// A marker without a time: the crash was before this process started.
			if (!g_crash_time)
			{
				FILETIME created, exited, kernel, user;
				if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
					g_crash_time = session_files::ft64(created) - 1;
			}
			g_crash_pending = true;
		}

		// The previous session's crash, once its zip is made (or after
		// kArchiveWaitMs), with the one file to upload. The marker goes only
		// now, so a session that ends before this leaves it to the next one.
		void report_crash(DWORD now)
		{
			if (!session_archive::finished() && now - g_first_ms < kArchiveWaitMs) return;
			g_crash_pending = false;
			std::error_code ec;
			std::filesystem::remove(std::filesystem::path(g_marker), ec);

			std::string text = g_crash_header + ".";
			const std::wstring zip = session_archive::zip_for(g_crash_time);
			// The notice names the zip itself (rtx.faultNotice.uploadFile), with a
			// button that shows it in Explorer.
			if (!zip.empty())
			{
				const std::string path = session_files::utf8(zip);
				text += " Upload only the file named below; it holds that session's logs, crash log and any crash dump.";
				shared::common::log_event("Session", std::format("the file to upload for the crashed session: {}", path),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS);
				std::lock_guard lock(g_mutex);
				g_upload_file = path;
			}
			else
				text += std::format(" Upload remix-comp.prev.log{} from the log folder below.",
					g_crash_file.empty() ? std::string() : " and " + g_crash_file);
			if (session_archive::folder_unknown())
				text += " Mod Organizer 2 runs the game: if Explorer does not show the file there, look in Mod "
					"Organizer 2's Overwrite folder.";
			report(Fault::GameCrashed, text);
		}

		// In game and settled, and for kEmptySceneMs nearly every world draw
		// the API paths recognised was refused (a texture or mesh build that
		// fails for everything).
		void check_scene(DWORD now)
		{
			const FnvRemixFacts& f = nvse_link::facts();
			uint32_t drawn = 0, refused = 0;
			for (const api::FeatureStats* s : { &api::statics_stats(), &api::terrain_stats(), &api::far_lod_stats(),
				&api::skinned_stats(), &api::foliage_stats(), &api::unlit_stats(), &api::water_stats() })
			{
				drawn += s->draws_last + s->retained_live;
				refused += s->fallbacks_last;
			}
			const bool empty = nvse_link::linked() && f.ready && f.game_state == FNVR_GS_IN_GAME && f.settled && !f.loading &&
				refused >= kEmptySceneRefused && drawn * 20 < refused;
			if (!empty)
				g_empty_since = 0;
			else if (!g_empty_since)
				g_empty_since = now;
			else if (now - g_empty_since >= kEmptySceneMs)
				report(Fault::EmptyScene, std::format("{} of {} draws refused", refused, refused + drawn));
		}

		void send()
		{
			std::string lines;
			std::string upload;
			{
				std::lock_guard lock(g_mutex);
				if (!g_dirty) return;
				g_dirty = false;
				for (size_t i = 0; i < std::size(kKinds); i++)
					if (g_reported[i])
						lines += std::format("{}|{}{}{}\n", kKinds[i].id, kKinds[i].text, g_details[i].empty() ? "" : " ",
							g_details[i]);
				upload = g_upload_file;
			}
			// The runtime's config parser keeps a value as sent, but takes no
			// empty one. The file goes first, so the notice shows the problem
			// and the file in the same frame.
			auto& bridge = shared::common::remix_api::get().m_bridge;
			if (!upload.empty()) bridge.SetConfigVariable("rtx.faultNotice.uploadFile", upload.c_str());
			bridge.SetConfigVariable("rtx.faultNotice.hostReport", lines.c_str());
		}
	}

	void report(Fault fault, const std::string& detail)
	{
		const size_t i = static_cast<size_t>(fault);
		std::string clean = detail;
		std::replace_if(clean.begin(), clean.end(), [](char c) { return c == '\n' || c == '\r' || c == '|'; }, ' ');
		{
			std::lock_guard lock(g_mutex);
			if (g_reported[i] && (clean.empty() || g_details[i].find(clean) != std::string::npos)) return;
			if (!clean.empty()) g_details[i] += (g_details[i].empty() ? "" : "; ") + clean;
			g_reported[i] = true;
			g_dirty = true;
		}
		shared::common::log_event("Problem", std::format("{}{}{}", kKinds[i].text, clean.empty() ? "" : " ", clean),
			shared::common::LOG_TYPE::LOG_TYPE_WARN);
	}

	const char* check_module(const char* file, const char* product)
	{
		for (const Incompatible& m : kIncompatible)
		{
			if ((m.file && file && _stricmp(file, m.file) == 0) || (m.product && product && _stricmp(product, m.product) == 0))
			{
				report(Fault::IncompatibleDll, std::format("{} ({})", m.mod, file ? file : "?"));
				return m.mod;
			}
		}
		return nullptr;
	}

	void on_present()
	{
		const DWORD now = GetTickCount() | 1;
		g_last_present_ms = now;
		if (g_frames++ == 0)
		{
			g_first_ms = now;
			start();
		}
		if (g_crash_pending) report_crash(now);

		if (!reported(Fault::EmptyScene)) check_scene(now);
		if (!reported(Fault::PluginReadFaults) && nvse_link::linked() && nvse_link::facts().read_faults >= kReadFaults)
			report(Fault::PluginReadFaults, std::format("({} read faults)", nvse_link::facts().read_faults));

		if (g_frames >= kWarmupFrames && shared::common::remix_api::is_initialized() &&
			shared::common::remix_api::get().m_bridge.SetConfigVariable)
			send();
	}

	void on_crash(const char* header, const char* crash_file)
	{
		if (!g_marker[0] || GetTickCount() - g_last_present_ms > kCrashWindowMs) return;
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		char text[800];
		const int len = _snprintf_s(text, _TRUNCATE, "%s\n%s\n%llu\n", header, crash_file ? crash_file : "",
			session_files::ft64(now));
		if (len <= 0) return;
		const HANDLE file = CreateFileW(g_marker, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return;
		DWORD written = 0;
		WriteFile(file, text, static_cast<DWORD>(len), &written, nullptr);
		CloseHandle(file);
	}
}
