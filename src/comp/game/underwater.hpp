#pragma once

// The runtime's underwater look (rtx.underwater.*: dimmed sun, procedural
// caustics, depth murk) driven from the engine ([RemixApi] Underwater).
//
// The runtime applies the look to primary surfaces whose primary ray
// reached them inside a medium. FNV water is a thick translucent medium
// (api_water.cpp), so a lake bed seen through the surface from above is
// inside it already; with the camera under water the ray starts inside it
// only once the camera medium is set (SetCameraMediumMaterial).
//
// Engine facts (the monitor plugin, FNVR_F_WATER): the water system's own
// "camera below the water surface" test and the height of the water at the
// camera. Each world frame, at its first world draw (after the stable
// origin's D is final):
//   * rtx.underwater.enable is True while the camera is in game with water
//     at the camera, False otherwise;
//   * rtx.underwater.surfaceHeight is that water height in render space
//     (world Z minus the frame's origin, see stable_origin.hpp), pushed when
//     it moves by more than a fraction of a unit;
//   * the camera medium is a texture-less translucent material with the
//     near water's transmittance (api_water's medium of the water in view)
//     while the engine says the camera is below the surface, cleared
//     otherwise.
// A frame with no world draw (menus, loading screens), a cut, the setting
// off or a missing fact turns all of it off.

namespace comp::game::underwater
{
	// The frame's first world draw (game::on_world_draw).
	void on_world_frame();

	// Present: turns the look off when this frame had no world frame.
	void on_present();
}
