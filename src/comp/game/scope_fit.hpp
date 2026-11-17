#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The scope lens, measured from a weapon model's triangles (comp/game/scopes.hpp).
//
// Pure math: the caller hands in a triangle soup in the model root's space
// and two engine-convention node positions, and gets back where the scope's
// eyepiece and objective are. No engine reads, no logging, no settings, so
// tests/scope_fit builds this file on its own.
//
// The method: flat round patches and closed circular boundary loops
// (glass discs, tube end rims, the mouths of open tubes) are clustered into
// lines along their normals; along each line a ray finds the surfaces it
// crosses, section planes find the walls around it (tube evidence and the
// clear aperture), and the two ends of the tube are the eyepiece and the
// objective. Lines that are the barrel (they pass the projectile node),
// short, hidden from the eye point or blocked ahead of the objective are
// rejected; the rest are scored, and the best one is the scope when it is
// clearly ahead of the runner-up. Every threshold is in world game units
// (the root's world scale converts), and nothing depends on which root axis
// is forward.
namespace comp::game::scopes::fit
{
	// One shape of the model: positions (x, y, z per vertex, root space)
	// and a triangle list. `blended`: an alpha-blended material (glass).
	struct Shape
	{
		std::vector<float> pos;
		std::vector<uint32_t> tris;
		bool blended = false;
	};

	// Root-space hints. The projectile node marks the muzzle (the barrel's
	// line and the forward end); the sighting node sits behind a sight. The
	// eye and view up stand in when the model has no projectile node.
	struct Hints
	{
		bool have_projectile = false;
		float projectile[3] = {};
		bool have_sighting = false;
		float sighting[3] = {};
		bool have_eye = false;
		float eye[3] = {};
		float view_up[3] = { 0.0f, 1.0f, 0.0f };
	};

	struct Soup
	{
		std::vector<Shape> shapes;
		Hints hints;
		float unit_scale = 1.0f;   // the root's world scale: world units per root unit
	};

	// Where an end's numbers come from.
	enum class End : uint8_t
	{
		None,
		Surface,    // a surface the axis crosses, grown into its glass patch
		Rim,        // a rim or the mouth of an open tube
		Aperture,   // the clear aperture of the tube only (no usable patch)
	};

	// One axis the fit looked at, for the verbose log.
	struct Candidate
	{
		float point[3] = {};       // the eyepiece center when measured, else a point on the line
		float axis[3] = {};
		float span = 0.0f;         // eyepiece to objective end, root units
		float tube = 0.0f;         // fraction of the span with walls around the axis
		float score = 0.0f;
		int members = 0;           // openings on the line
		int hits = 0;              // surfaces the line crosses
		bool usable = false;       // passed every hard check
		std::string why;
	};

	struct Result
	{
		bool found = false;        // level 1 or 2
		float eyepiece[3] = {};
		float axis[3] = { 1.0f, 0.0f, 0.0f };
		float up[3] = { 0.0f, 1.0f, 0.0f };
		float eyepiece_radius = 0.0f;
		float objective_distance = 0.0f;
		float objective_radius = 0.0f;
		int aperture = 0;          // 0 circle, 1 rectangle (eyepiece_radius is its half height)
		float aperture_aspect = 1.0f;
		float confidence = 0.0f;   // 0..1
		int level = 0;             // 0 none, 1 medium, 2 high
		End eyepiece_end = End::None;
		End objective_end = End::None;
		uint32_t eyepiece_shape = 0;   // index into Soup::shapes
		std::string why;           // the checks passed and failed, one line
		std::vector<Candidate> candidates;
	};

	// Measures the scope. Never throws for bad input; a refusal comes back
	// with found false and `why`.
	Result fit(const Soup& soup);

	// Lens geometry as a lens file gives it, brought into root space.
	struct Geometry
	{
		float eyepiece[3] = {};
		float axis[3] = { 1.0f, 0.0f, 0.0f };
		float up[3] = { 0.0f, 1.0f, 0.0f };
		float eyepiece_radius = 0.9f;
		float objective_distance = 20.0f;
		float objective_radius = 1.0f;
	};

	// How far a file's geometry may be from the fit and still agree, world
	// units (radius: the larger of the absolute and the relative limit).
	struct Tolerances
	{
		float lateral = 0.15f;
		float axial = 0.3f;
		float radius = 0.1f;
		float radius_fraction = 0.1f;
		float axis_degrees = 1.0f;
		float up_degrees = 1.0f;
		float objective_behind = 0.1f;   // the file's objective end behind the fitted one
		float objective_ahead = 1e9f;    // ... and ahead of it
		float objective_radius_fraction = 1e9f;
	};

	struct Agreement
	{
		bool ok = false;
		float lateral = 0.0f;        // the file's eyepiece center across the fitted axis
		float axial = 0.0f;          // ... and along it (+ toward the objective)
		float radius = 0.0f;         // file minus fit
		float axis_degrees = 0.0f;
		float up_degrees = 0.0f;
		float objective = 0.0f;      // the file's objective end minus the fit's, along the fitted axis
		float objective_radius = 0.0f;   // file minus fit
		std::string why;             // the first quantity out of tolerance, or empty
	};

	// The file's geometry against a usable fit. All lengths are compared in
	// world units (`unit_scale` world units per root unit).
	Agreement agrees(const Result& fitted, const Geometry& file, float unit_scale, const Tolerances& tol = {});

	struct Check
	{
		bool ok = false;
		std::string why;
	};

	// The file's geometry on a mesh without a usable fit: walls or a glass
	// patch around the eyepiece center at about its radius, nothing between
	// the eye point and the eyepiece, and nothing right ahead of the
	// objective point.
	Check mesh_check(const Soup& soup, const Geometry& file);
}
