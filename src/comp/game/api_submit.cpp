#include "std_include.hpp"
#include "api_submit.hpp"
#include "api_textures.hpp"
#include "api_hair_bake.hpp"
#include "api_mesh.hpp"
#include "scene_statics.hpp"
#include "traced.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"

namespace comp::game::api_submit
{
	// Per-feature housekeeping, defined next to each feature's cache.
	void terrain_on_present();
	void terrain_on_reset();
	void water_on_present();
	void water_on_reset();
	void statics_on_present();
	void statics_on_reset();
	void terrain_drop_retained();
	void terrain_drop_all();
	void water_drop_retained();
	void water_drop_all();
	void statics_drop_all();
	void statics_drop_retained();
	void skinned_on_present();
	void skinned_on_reset();
	void skinned_drop_all();
	void foliage_on_present();
	void foliage_on_reset();
	void foliage_drop_all();
	bool foliage_frame_blocked();
	void foliage_end_frame();
	void unlit_on_present();
	void unlit_drop_all();
	void unlit_drop_retained();
	void particles_on_present();
	void particles_drop_all();

	void end_frame_retained()
	{
		PROFILE_ZONE_N("api_submit::end_frame_retained");
		// RetainedKeepCulled per feature this frame: with the FFP renderer off
		// (the tracer's capture) nothing reaches the API paths, so their
		// entries must not stay in the scene (they are hidden when not drawn
		// and EvictFrames destroys them). Foliage is also hidden when not
		// drawn on a frame whose draws went out as DrawInstance only
		// (transient).
		{
			// The API paths are reached from the renderer's routing only.
			const bool ffp_on = shared::common::ffp_state::get().is_enabled();
			api::terrain_stats().keep_culled = ffp_on;
			api::far_lod_stats().keep_culled = ffp_on;
			api::water_stats().keep_culled = ffp_on;
			api::statics_stats().keep_culled = ffp_on;
			api::foliage_stats().keep_culled = ffp_on && !foliage_frame_blocked();
		}
		// Grass batches whose instance set changed or that were destroyed
		// (the grass batch hooks) leave the scene with this frame's updates.
		foliage_end_frame();
		api::retained::end_frame();
	}

	void begin_frame_retained()
	{
		if (!api::retained::enabled()) return;
		api::retained::replay_for_frame(api::frame());
	}

	void on_present()
	{
		PROFILE_ZONE_N("api_submit::on_present");
		terrain_on_present();
		water_on_present();
		statics_on_present();
		skinned_on_present();
		foliage_on_present();
		unlit_on_present();
		particles_on_present();
		// Traced draws that did not go through the API, by route and reason.
		traced::on_present();
		// Draws classified by the engine and by the shader fallback, per route.
		engine_class::on_present();
		// Finished hair bakes: one upload per frame, before the upload budget resets.
		api::hair_bake::on_present();
		// After the mesh sweeps, so textures their evictions released are
		// eligible in the same pass.
		api::textures::on_present();
		api::end_frame();
	}

	void on_reset()
	{
		terrain_on_reset();
		water_on_reset();
		statics_on_reset();
		skinned_on_reset();
		foliage_on_reset();
	}

	void retained_switched_off()
	{
		PROFILE_ZONE_N("api_submit::retained_switched_off");
		terrain_drop_retained();
		water_drop_retained();
		// Grass and leaves exist only as retained instances.
		foliage_drop_all();
		// Unlit FX keep their meshes and draw with DrawInstance.
		unlit_drop_retained();
		// Draw capture's retained placements: per frame from the next draw.
		statics_drop_retained();
		scene_statics::drop_all("RetainedInstances off");
		api::retained::on_all_destroyed();
		shared::common::log("RemixApi", "RetainedInstances off: every retained instance destroyed; "
			"land / far LOD / water draw with DrawInstance",
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
	}

	void rebuild_everything()
	{
		PROFILE_ZONE_N("api_submit::rebuild_everything");
		const auto before = api::texture_stats().live;
		terrain_drop_all();
		water_drop_all();
		statics_drop_all();
		skinned_drop_all();
		foliage_drop_all();
		unlit_drop_all();
		particles_drop_all();
		scene_statics::drop_all("rebuild");
		api::retained::on_all_destroyed();
		// Nothing holds the game textures now: destroy them too, so each is
		// built again through the path configured now (upload or import).
		api::textures::purge_unreferenced();
		shared::common::log("RemixApi", std::format(
			"rebuild: every API mesh, material and retained instance destroyed, textures {} -> {}; "
			"they are built again as the game draws (MaxBuildsPerFrame per frame; not drawn meanwhile)",
			before, api::texture_stats().live),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}
}
