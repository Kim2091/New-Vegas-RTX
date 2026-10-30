#include "std_include.hpp"
#include "underwater.hpp"
#include "api_submit.hpp"
#include "api_mesh.hpp"
#include "nvse_link.hpp"
#include "stable_origin.hpp"

#include "shared/common/config.hpp"
#include "shared/common/remix_api.hpp"

namespace comp::game::underwater
{
	using namespace comp::game::api;

	namespace
	{
		constexpr uint64_t kMediumDomain = 0x5557415445524D44ull;   // "UWATERMD"
		// surfaceHeight is re-pushed once the render-space height moves this
		// far (the origin re-snaps, the camera changes water body; with the
		// stable origin off, every camera move).
		constexpr float kHeightEpsilon = 0.25f;

		// What the runtime was last told; -1 / NaN: unknown (pushed next time).
		int g_enable = -1;
		float g_height = std::numeric_limits<float>::quiet_NaN();
		int g_medium = -1;                  // 1 set, 0 cleared
		remixapi_MaterialHandle g_medium_mat = nullptr;
		uint64_t g_medium_key = 0;
		bool g_was_initialized = false;

		// Engine state as last logged.
		bool g_under = false;
		bool g_have_water = false;
		float g_world_height = 0.0f;
		uint32_t g_cut_serial = 0;

		UINT g_world_frame = 0xFFFFFFFFu;
		bool g_warned_no_medium = false;

		void log(const std::string& msg)
		{
			shared::common::log("Underwater", msg, shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		}

		void push_enable(remixapi_Interface* b, bool on)
		{
			if (g_enable == (on ? 1 : 0) || !b->SetConfigVariable) return;
			if (b->SetConfigVariable("rtx.underwater.enable", on ? "True" : "False") == REMIXAPI_ERROR_CODE_SUCCESS)
				g_enable = on ? 1 : 0;
		}

		// h in FNV units, render space; the runtime gets it in render units.
		void push_height(remixapi_Interface* b, float h)
		{
			if (std::fabs(h - g_height) <= kHeightEpsilon || !b->SetConfigVariable) return;
			const std::string v = std::format("{:.2f}", h * stable_origin::scale());
			if (b->SetConfigVariable("rtx.underwater.surfaceHeight", v.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS)
				g_height = h;
		}

		uint64_t medium_key(const float color[3], float distance)
		{
			Hash64 h;
			h.mix(kMediumDomain);
			for (int k = 0; k < 3; k++) h.pod(color[k]);
			h.pod(distance);
			return h.get();
		}

		// A texture-less translucent material with this transmittance (over
		// `distance` FNV units), the water's IoR and no thin wall: what the
		// camera's rays start inside.
		remixapi_MaterialHandle medium_material(const float color[3], float distance, uint64_t key)
		{
			remixapi_MaterialInfoTranslucentEXT tr = {};
			tr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
			tr.transmittanceTexture = L"";
			tr.refractiveIndex = shared::common::config::get().remix_api.water_ior;
			tr.transmittanceColor = { color[0], color[1], color[2] };
			tr.transmittanceMeasurementDistance = static_cast<float>(distance * stable_origin::scale());
			tr.thinWallThickness_hasvalue = 0;
			tr.useDiffuseLayer = 0;
			remixapi_MaterialInfo mi;
			init_material_info(mi, next_object_hash(key, kMediumDomain), &tr);
			return create_material(mi);
		}

		void set_medium(remixapi_Interface* b, bool on)
		{
			if (!b->SetCameraMediumMaterial)
			{
				if (on && !g_warned_no_medium)
				{
					g_warned_no_medium = true;
					log("the Remix API has no SetCameraMediumMaterial (the bridge does not forward it): "
						"the camera medium is unavailable, only surfaces seen through the water get the look");
				}
				return;
			}

			if (!on)
			{
				if (g_medium == 0) return;
				remixapi_CameraMediumInfo ci = {};
				ci.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO;
				ci.medium = nullptr;
				if (b->SetCameraMediumMaterial(&ci) == REMIXAPI_ERROR_CODE_SUCCESS)
				{
					g_medium = 0;
					log("camera medium cleared");
				}
				return;
			}

			float color[3] = { 0.30f, 0.55f, 0.50f };
			float distance = shared::common::config::get().remix_api.water_measurement_distance;
			api_submit::water_near_medium(color, distance);
			const uint64_t key = medium_key(color, distance);
			if (g_medium == 1 && key == g_medium_key) return;

			remixapi_MaterialHandle old = nullptr;
			if (key != g_medium_key || !g_medium_mat)
			{
				const auto h = medium_material(color, distance, key);
				if (!h)
				{
					log("camera medium: CreateMaterial failed");
					return;
				}
				old = g_medium_mat;
				g_medium_mat = h;
				g_medium_key = key;
			}
			remixapi_CameraMediumInfo ci = {};
			ci.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO;
			ci.medium = g_medium_mat;
			const auto err = b->SetCameraMediumMaterial(&ci);
			if (err == REMIXAPI_ERROR_CODE_SUCCESS) g_medium = 1;
			log(std::format("camera medium set: transmittance ({:.2f} {:.2f} {:.2f}) over {:.0f} units -> {}",
				color[0], color[1], color[2], distance, static_cast<int>(err)));
			// The runtime copies the material into its camera-medium state.
			if (old) b->DestroyMaterial(old);
		}

		void all_off(const char* why)
		{
			if (g_under)
			{
				g_under = false;
				log(std::format("exit ({})", why));
			}
			auto* b = bridge();
			if (!b) return;
			set_medium(b, false);
			push_enable(b, false);
		}
	}

	void on_world_frame()
	{
		const UINT now = frame();
		if (g_world_frame == now) return;
		g_world_frame = now;

		// A fresh API (startup, re-initialisation) holds none of the pushed state.
		const bool init = shared::common::remix_api::is_initialized();
		if (init != g_was_initialized)
		{
			g_was_initialized = init;
			g_enable = -1;
			g_medium = -1;
			g_height = std::numeric_limits<float>::quiet_NaN();
			g_medium_mat = nullptr;
			g_medium_key = 0;
		}
		auto* b = bridge();
		if (!b) return;

		if (!shared::common::config::get().remix_api.underwater) { all_off("Underwater off"); return; }
		if (!nvse_link::has(FNVR_F_WATER)) { all_off("no engine water state"); return; }

		const FnvRemixFacts& f = nvse_link::facts();
		if (f.game_state != FNVR_GS_IN_GAME || f.loading) { all_off("not in game"); return; }
		if (f.cut_serial != g_cut_serial)
		{
			g_cut_serial = f.cut_serial;
			all_off("cut");
			return;
		}

		const bool have_water = f.water_valid && std::isfinite(f.water_height) && std::fabs(f.water_height) < 1.0e7f;
		if (have_water != g_have_water || (have_water && std::fabs(f.water_height - g_world_height) > 1.0f))
		{
			if (have_water)
				log(std::format("water at the camera: height {:.1f}", f.water_height));
			else
				log("no water at the camera");
			g_have_water = have_water;
			g_world_height = f.water_height;
		}
		if (!have_water) { all_off("no water"); return; }

		// Render space: world minus the frame's origin (the camera itself
		// while the stable origin is off).
		double base_z = 0.0;
		if (stable_origin::active()) base_z = stable_origin::origin()[2];
		else if (camera_position_ptr && std::isfinite(camera_position_ptr[2])) base_z = camera_position_ptr[2];
		else { all_off("no camera"); return; }

		const bool under = f.underwater != 0;
		if (under != g_under)
		{
			g_under = under;
			log(std::format("{}: camera z {:.1f}, water {:.1f}", under ? "enter" : "exit",
				f.world_cam_pos[2], f.water_height));
		}

		push_enable(b, true);
		push_height(b, static_cast<float>(static_cast<double>(f.water_height) - base_z));
		set_medium(b, under);
	}

	void on_present()
	{
		if (g_world_frame == frame()) return;
		if (g_enable == 1 || g_medium == 1 || g_under) all_off("no world frame");
	}
}
