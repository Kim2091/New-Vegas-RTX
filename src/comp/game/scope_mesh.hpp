#pragma once
#include "scope_fit.hpp"

// The first-person weapon model's triangles, read from the scene graph for
// the scope fit (scope_fit.hpp). Read-only: every engine read goes through
// the scene_graph readers and the bridge's READONLY buffer copies. Main /
// render thread, between frames, while the root is the live first-person
// weapon node.
namespace comp::game::scopes::mesh
{
	// A pose relative to the model root: root-space point = rot * (local *
	// scale) + pos (rot row-major, mapping column vectors).
	struct RootPose
	{
		float rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		float pos[3] = {};
		float scale = 1.0f;
	};

	// A named object of the model whose name is unique in it.
	struct Anchor
	{
		char name[64] = {};
		RootPose pose;
	};

	struct Nodes
	{
		// Per soup shape: the index into anchors of its nearest ancestor-or-self
		// with a non-empty name that is unique in the model; -1: the root.
		std::vector<int> shape_anchor;
		std::vector<Anchor> anchors;
		uint32_t shapes = 0;            // geometry objects under the root
		uint32_t skinned = 0;           // left out: skinned
		uint32_t unreadable = 0;        // left out: no readable positions or triangles
		float root_scale = 1.0f;        // the root's world scale
	};

	// The model's name of `obj` (NiObjectNET +0x08) into `out`; false when
	// it has none or it cannot be read.
	bool node_name(uintptr_t obj, char (&out)[64]);

	// The triangles under `root` (app-culled and skinned shapes left out) in
	// root space into `out`, with the ProjectileNode and ##SightingNode
	// positions as hints and the root's world scale. False with `why` when
	// nothing usable was read; a partial read is described in `why` too.
	bool collect(uintptr_t root, fit::Soup& out, Nodes& nodes, std::string& why);

	// `obj`'s pose relative to `root`, from their world transforms now.
	bool relative_pose(uintptr_t root, uintptr_t obj, RootPose& out);
}
