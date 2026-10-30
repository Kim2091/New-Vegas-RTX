#include "std_include.hpp"
#include "save_thumbnail.hpp"

#include <fstream>

#include "call_chain.hpp"
#include "nvse_link.hpp"
#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/fnv_exe.hpp"

namespace comp::game::save_thumbnail
{
	namespace
	{
		// The save screenshot capture: returns a new NiPixelData (RGB24, rows
		// packed at w * 3, top row first) the caller deletes. It sets *h from
		// the screen's aspect ratio and writes a .bmp when bmp is not empty.
		using Capture_t = void*(__cdecl*)(int* w, int* h, const char* bmp);
		using NiAlloc_t = void*(__cdecl*)(size_t);
		using PixelDataCtor_t = void*(__thiscall*)(void* self, uint32_t w, uint32_t h, const void* format,
			uint32_t mips, uint32_t faces);
		constexpr size_t kPixelDataSize = 0x74;
		constexpr uint32_t kPixelDataPixels = 0x50;   // uint8_t*, the first mip of the first face

		Capture_t g_capture = nullptr;
		bool g_hooked = false;

		// Request n of this session; thumbnails asked for before g_first_valid
		// show an earlier place (a load or a cell change came after them).
		uint32_t g_requested = 0;
		uint32_t g_first_valid = 1;
		uint32_t g_cut_serial = 0;
		ULONGLONG g_last_request_ms = 0;
		std::string g_prefix;

		constexpr const char* kIfReplaced = "saves get the engine's own capture, which is blank under Remix";

		std::filesystem::path thumbnail_path()
		{
			wchar_t exe[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			return std::filesystem::path(exe).parent_path() / L"rtx-remix" / L"thumbnail.rgb";
		}

		// Box-filtered resample of the thumbnail into w x h RGB rows.
		void resample(const Thumbnail& t, uint32_t w, uint32_t h, uint8_t* out)
		{
			for (uint32_t y = 0; y < h; y++)
			{
				const uint32_t y0 = y * t.height / h;
				const uint32_t y1 = (std::max)(y0 + 1, (y + 1) * t.height / h);
				for (uint32_t x = 0; x < w; x++)
				{
					const uint32_t x0 = x * t.width / w;
					const uint32_t x1 = (std::max)(x0 + 1, (x + 1) * t.width / w);
					uint32_t sum[3] = {}, n = 0;
					for (uint32_t sy = y0; sy < y1 && sy < t.height; sy++)
						for (uint32_t sx = x0; sx < x1 && sx < t.width; sx++, n++)
							for (int c = 0; c < 3; c++)
								sum[c] += t.rgb[(static_cast<size_t>(sy) * t.width + sx) * 3 + c];
					for (int c = 0; c < 3; c++)
						out[(static_cast<size_t>(y) * w + x) * 3 + c] = static_cast<uint8_t>(n ? (sum[c] + n / 2) / n : 0);
				}
			}
		}

		void* __cdecl capture_hk(int* w, int* h, const char* bmp)
		{
			const auto& a = fnv_exe::addr();
			Thumbnail t;
			std::string why;
			if (!shared::common::config::get().save_screenshots.enabled)
				why = "[SaveScreenshots] Enabled=0";
			else if (!w || !h || *w <= 0 || *w > 4096)
				why = "the engine asked for no size";
			else if (read_thumbnail(t, why) && t.request < g_first_valid)
			{
				why = "the latest thumbnail is from before the last load or cell change";
				t.rgb.clear();
			}
			if (!why.empty() || t.rgb.empty())
			{
				shared::common::log("SaveShot", std::format("the engine's own capture is used: {}", why),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return g_capture(w, h, bmp);
			}

			// The engine's size: *h from the screen's aspect ratio, as the capture sets it.
			const uint32_t sw = *reinterpret_cast<const uint32_t*>(a.screen_width);
			const uint32_t sh = *reinterpret_cast<const uint32_t*>(a.screen_height);
			const uint32_t width = static_cast<uint32_t>(*w);
			const uint32_t height = (std::max)(1u, sw ? static_cast<uint32_t>(static_cast<float>(width) *
				(static_cast<float>(sh) / static_cast<float>(sw))) : t.height * width / t.width);
			*h = static_cast<int>(height);

			void* pd = reinterpret_cast<NiAlloc_t>(a.ni_alloc)(kPixelDataSize);
			if (!pd) return g_capture(w, h, bmp);
			reinterpret_cast<PixelDataCtor_t>(a.pixel_data_ctor)(pd, width, height,
				reinterpret_cast<const void*>(a.pixel_format_rgb24), 1, 1);
			uint8_t* pixels = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pd) + kPixelDataPixels);
			if (pixels) resample(t, width, height, pixels);
			shared::common::log("SaveShot", std::format("save picture {}x{} from thumbnail #{} ({}x{})", width, height,
				t.request, t.width, t.height), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
			return pd;
		}

		// The Print Screen capture's call: nothing is captured.
		void __cdecl screenshot_key_hk(int)
		{
		}

		void install_screenshot_key()
		{
			const fnv_exe::Code& site = fnv_exe::addr().screenshot_key_call;
			constexpr const char* what = "the main loop's call to the Print Screen capture";
			const call_chain::Site found = call_chain::inspect(site);
			call_chain::log_site("SaveShot", site, what, found);
			const auto ours = reinterpret_cast<uintptr_t>(&screenshot_key_hk);
			if (found.state == call_chain::State::Foreign || !call_chain::patch_rel32(site.addr, 0xE8, ours))
			{
				shared::common::log("SaveShot", "the game's Print Screen capture stays on (ScreenShotN.bmp next to "
					"FalloutNV.exe)", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			call_chain::track(call_chain::Kind::Call, site.addr, ours, found, "SaveShot", what,
				"Print Screen also writes the game's ScreenShotN.bmp");
			shared::common::log("SaveShot", std::format("the game's Print Screen capture is off @0x{:X} (Remix's "
				"screenshot takes the key)", site.addr), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		}

	}

	uint32_t request_thumbnail()
	{
		auto& api = shared::common::remix_api::get();
		if (!shared::common::remix_api::is_initialized() || !api.m_bridge.SetConfigVariable) return 0;
		const std::string value = g_prefix + std::to_string(++g_requested);
		api.m_bridge.SetConfigVariable("rtx.screenshot.thumbnailRequest", value.c_str());
		return g_requested;
	}

	// The runtime's thumbnail file: 'RTHB', width, height and the length of
	// the request value (u32 little-endian), the value, then RGB rows.
	bool read_thumbnail(Thumbnail& t, std::string& why)
	{
		std::ifstream file(thumbnail_path(), std::ios::binary);
		if (!file) { why = "no rtx-remix\\thumbnail.rgb"; return false; }
		uint8_t head[16] = {};
		if (!file.read(reinterpret_cast<char*>(head), sizeof(head)) || std::memcmp(head, "RTHB", 4) != 0)
		{
			why = "rtx-remix\\thumbnail.rgb has no RTHB header";
			return false;
		}
		uint32_t len = 0;
		std::memcpy(&t.width, head + 4, 4);
		std::memcpy(&t.height, head + 8, 4);
		std::memcpy(&len, head + 12, 4);
		if (!t.width || !t.height || t.width > 8192 || t.height > 8192 || len > 256)
		{
			why = "rtx-remix\\thumbnail.rgb has a bad size";
			return false;
		}
		std::string value(len, '\0');
		if (len && !file.read(value.data(), len)) { why = "rtx-remix\\thumbnail.rgb is cut short"; return false; }
		if (value.rfind(g_prefix, 0) != 0) { why = "the thumbnail is from another session"; return false; }
		t.request = static_cast<uint32_t>(std::strtoul(value.c_str() + g_prefix.size(), nullptr, 10));
		t.rgb.resize(static_cast<size_t>(t.width) * t.height * 3);
		if (!file.read(reinterpret_cast<char*>(t.rgb.data()), static_cast<std::streamsize>(t.rgb.size())))
		{
			why = "rtx-remix\\thumbnail.rgb is cut short";
			return false;
		}
		return true;
	}

	void install()
	{
		if (!fnv_exe::known()) return;
		install_screenshot_key();
		g_prefix = std::format("fnv-{}-", GetCurrentProcessId());
		if (!shared::common::config::get().save_screenshots.enabled) return;
		const fnv_exe::Code& site = fnv_exe::addr().save_screenshot_call;
		constexpr const char* what = "the save header writer's call to the save screenshot capture";
		const call_chain::Site found = call_chain::inspect(site);
		call_chain::log_site("SaveShot", site, what, found);
		if (found.state == call_chain::State::Foreign)
		{
			shared::common::log("SaveShot", "save screenshots stay the engine's: the call site cannot be hooked",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		g_capture = reinterpret_cast<Capture_t>(found.target);
		const auto ours = reinterpret_cast<uintptr_t>(&capture_hk);
		if (!call_chain::patch_rel32(site.addr, 0xE8, ours))
		{
			shared::common::log("SaveShot", std::format("save screenshots stay the engine's: 0x{:X} could not be made "
				"writable", site.addr), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		call_chain::track(call_chain::Kind::Call, site.addr, ours, found, "SaveShot", what, kIfReplaced);
		g_hooked = true;
		shared::common::log("SaveShot", std::format("save screenshot hook installed @0x{:X}: saves store the "
			"path-traced thumbnail", site.addr), shared::common::LOG_TYPE::LOG_TYPE_STATUS);
	}

	bool is_current(const Thumbnail& t)
	{
		return g_hooked && shared::common::config::get().save_screenshots.enabled && t.request >= g_first_valid;
	}

	void on_present()
	{
		if (!g_hooked) return;
		const auto& cfg = shared::common::config::get().save_screenshots;
		if (!cfg.enabled) return;

		// Only frames of the place the player is in: nothing during a load,
		// and the Pip-Boy (a 3D model in front of the camera) closed.
		if (nvse_link::linked())
		{
			const FnvRemixFacts& f = nvse_link::facts();
			if (!f.ready) return;
			if (f.cut_serial != g_cut_serial)
			{
				g_cut_serial = f.cut_serial;
				g_first_valid = g_requested + 1;
			}
			if (f.loading || !f.settled || f.pipboy != FNVR_PB_CLOSED ||
				(f.game_state != FNVR_GS_IN_GAME && f.game_state != FNVR_GS_MENU)) return;
		}

		const ULONGLONG now = GetTickCount64();
		if (g_requested >= g_first_valid && now - g_last_request_ms < static_cast<ULONGLONG>(cfg.refresh_ms)) return;
		g_last_request_ms = now;
		request_thumbnail();
	}
}
