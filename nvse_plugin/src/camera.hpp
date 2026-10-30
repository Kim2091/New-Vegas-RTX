#pragma once
#include <cstdint>

// Loading, cuts and the world camera.
//
// Loading: a load message pair (PreLoadGame .. PostLoadGame), loading-screen
// presents (OnFramePresent with isLoadingScreen = 1) or the LoadingMenu
// existing. A cut requested while loading waits for the load to end, so a
// door, fast travel or a save load is one cut.
//
// The world camera is the "World" SceneGraph's NiCamera, read at the call:
// xNVSE's MainGameLoop runs before the frame's update, so the main loop
// never snapshots it.

namespace fnvr::camera
{
	void request_cut(uint32_t reason, float distance);

	void on_pre_load();
	void on_post_load(bool success);
	void on_present(bool loading_screen);
	void on_new_game();
	void on_main_menu();

	// Main loop: frame counting, load transitions, settling.
	void tick();
	// Load transitions and settling, between main loops.
	void update();

	bool loading();
	bool settled();
	bool in_world();
	uint32_t cut_serial();
	uint32_t frames_since_cut();

	// The live camera reads; the teleport rule runs here.
	struct View
	{
		bool ok = false;
		float pos[3] = {};
		float rot[9] = {};
		float fov = 0.0f;
	};
	View read_world_camera(bool check_teleport);
	// PlayerCharacter third-person flag: 1 / 0, -1 unreadable.
	int read_third_person();
}
