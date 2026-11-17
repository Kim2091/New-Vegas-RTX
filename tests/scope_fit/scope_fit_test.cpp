// Standalone test of comp/game/scope_fit.cpp. Runs on the CPU only. Built by
// build_test.bat; not part of the shipped DLLs.
//
// Without arguments: synthetic meshes built here (scope tubes, glass discs,
// rims, domes, windows, barrels), each fitted and checked against the
// numbers it was built with, or against the expected refusal.
//
// With --soups <dir> --lenses <dir>: every soup file in <dir> (written by
// path-traced-scopes/tools/dump_soups.py from the game's meshes; never
// committed) is fitted, and the fit is compared with every lens file
// section whose Models names that soup's model. One row per pair, then a
// summary. Exit code: the number of hard failures (a usable fit with its
// eyepiece more than 1.0 from a file's that does not say Fit = file, a soup
// marked "expect none" that fits, or fewer than 90% of the pairs from
// "Confidence: high" files usable and agreeing).

#include "std_include.hpp"
#include "comp/game/scope_fit.hpp"

namespace fit = comp::game::scopes::fit;

namespace
{
	int g_failed = 0;

	void check(bool ok, const std::string& what)
	{
		std::printf("%s %s\n", ok ? "  ok  " : "FAILED", what.c_str());
		if (!ok) g_failed++;
	}

	constexpr double kPi = 3.14159265358979323846;

	// ---- Mesh builders (canonical frame: +X toward the muzzle, +Y up) ----------

	struct Builder
	{
		fit::Shape s;
		uint32_t add(double x, double y, double z)
		{
			s.pos.push_back(static_cast<float>(x));
			s.pos.push_back(static_cast<float>(y));
			s.pos.push_back(static_cast<float>(z));
			return static_cast<uint32_t>(s.pos.size() / 3 - 1);
		}
		void tri(uint32_t a, uint32_t b, uint32_t c)
		{
			s.tris.push_back(a);
			s.tris.push_back(b);
			s.tris.push_back(c);
		}
		void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
		{
			tri(a, b, c);
			tri(a, c, d);
		}
	};

	double ring_y(double cy, double r, int i, int seg) { return cy + r * std::cos(2.0 * kPi * i / seg); }
	double ring_z(double cz, double r, int i, int seg) { return cz + r * std::sin(2.0 * kPi * i / seg); }

	// An open, zero-thickness tube along X.
	fit::Shape tube(double x0, double x1, double cy, double cz, double r, int seg = 32)
	{
		Builder b;
		for (int i = 0; i < seg; i++)
		{
			b.add(x0, ring_y(cy, r, i, seg), ring_z(cz, r, i, seg));
			b.add(x1, ring_y(cy, r, i, seg), ring_z(cz, r, i, seg));
		}
		for (int i = 0; i < seg; i++)
		{
			const uint32_t a = static_cast<uint32_t>(i * 2), n = static_cast<uint32_t>(((i + 1) % seg) * 2);
			b.quad(a, n, n + 1, a + 1);
		}
		return b.s;
	}

	// A flat disc facing along X (a fan around its center).
	fit::Shape disc(double x, double cy, double cz, double r, bool blended, int seg = 32)
	{
		Builder b;
		const uint32_t c = b.add(x, cy, cz);
		for (int i = 0; i < seg; i++) b.add(x, ring_y(cy, r, i, seg), ring_z(cz, r, i, seg));
		for (int i = 0; i < seg; i++)
			b.tri(c, c + 1 + static_cast<uint32_t>(i), c + 1 + static_cast<uint32_t>((i + 1) % seg));
		b.s.blended = blended;
		return b.s;
	}

	// A flat ring facing along X.
	fit::Shape ring(double x, double cy, double cz, double rin, double rout, int seg = 32)
	{
		Builder b;
		for (int i = 0; i < seg; i++)
		{
			b.add(x, ring_y(cy, rin, i, seg), ring_z(cz, rin, i, seg));
			b.add(x, ring_y(cy, rout, i, seg), ring_z(cz, rout, i, seg));
		}
		for (int i = 0; i < seg; i++)
		{
			const uint32_t a = static_cast<uint32_t>(i * 2), n = static_cast<uint32_t>(((i + 1) % seg) * 2);
			b.quad(a, n, n + 1, a + 1);
		}
		return b.s;
	}

	// A spherical cap: its edge circle (radius r) at x_edge, its apex `h`
	// toward -X.
	fit::Shape dome(double x_edge, double cy, double cz, double r, double h, bool blended, int rings = 12, int seg = 32)
	{
		Builder b;
		const double R = (r * r + h * h) / (2.0 * h);
		const double top = std::asin(r / R);
		const uint32_t apex = b.add(x_edge - h, cy, cz);
		for (int k = 1; k <= rings; k++)
		{
			const double a = top * k / rings;
			const double rr = R * std::sin(a), x = x_edge - h + R * (1.0 - std::cos(a));
			for (int i = 0; i < seg; i++) b.add(x, ring_y(cy, rr, i, seg), ring_z(cz, rr, i, seg));
		}
		for (int i = 0; i < seg; i++)
			b.tri(apex, apex + 1 + static_cast<uint32_t>(i), apex + 1 + static_cast<uint32_t>((i + 1) % seg));
		for (int k = 1; k < rings; k++)
			for (int i = 0; i < seg; i++)
			{
				const uint32_t a = apex + 1 + static_cast<uint32_t>((k - 1) * seg + i);
				const uint32_t n = apex + 1 + static_cast<uint32_t>((k - 1) * seg + (i + 1) % seg);
				b.quad(a, a + static_cast<uint32_t>(seg), n + static_cast<uint32_t>(seg), n);
			}
		b.s.blended = blended;
		return b.s;
	}

	// A flat rectangle facing along X: h tall (Y), w wide (Z).
	fit::Shape pane(double x, double cy, double cz, double h, double w, bool blended)
	{
		Builder b;
		const uint32_t a = b.add(x, cy - h / 2, cz - w / 2), c = b.add(x, cy + h / 2, cz - w / 2);
		const uint32_t d = b.add(x, cy + h / 2, cz + w / 2), e = b.add(x, cy - h / 2, cz + w / 2);
		b.quad(a, c, d, e);
		b.s.blended = blended;
		return b.s;
	}

	// The four walls of a box along X, open at both ends.
	fit::Shape box(double x0, double x1, double cy, double cz, double h, double w)
	{
		Builder b;
		const double ys[4] = { cy - h / 2, cy + h / 2, cy + h / 2, cy - h / 2 };
		const double zs[4] = { cz - w / 2, cz - w / 2, cz + w / 2, cz + w / 2 };
		for (int i = 0; i < 4; i++)
		{
			b.add(x0, ys[i], zs[i]);
			b.add(x1, ys[i], zs[i]);
		}
		for (uint32_t i = 0; i < 4; i++)
		{
			const uint32_t a = i * 2, n = ((i + 1) % 4) * 2;
			b.quad(a, n, n + 1, a + 1);
		}
		return b.s;
	}

	// ---- Synthetic weapons ---------------------------------------------------------

	struct Scope
	{
		double x0 = 0.0, x1 = 12.0, cy = 10.0, cz = 0.0, r = 1.0;
		double eye_x = 0.2, eye_r = 0.95, obj_x = 11.8, obj_r = 0.95;
	};

	void add_scope(fit::Soup& s, const Scope& sc, bool glass = true)
	{
		s.shapes.push_back(tube(sc.x0, sc.x1, sc.cy, sc.cz, sc.r));
		if (!glass) return;
		s.shapes.push_back(disc(sc.eye_x, sc.cy, sc.cz, sc.eye_r, true));
		s.shapes.push_back(disc(sc.obj_x, sc.cy, sc.cz, sc.obj_r, true));
	}

	void add_barrel(fit::Soup& s, double y = 5.0)
	{
		s.shapes.push_back(tube(-5.0, 30.0, y, 0.0, 0.4, 16));
		s.hints.have_projectile = true;
		s.hints.projectile[0] = 30.0f;
		s.hints.projectile[1] = static_cast<float>(y);
		s.hints.projectile[2] = 0.0f;
	}

	// Rotates (rows of r, mapping column vectors), scales and moves every
	// point of the soup and its hints.
	void transform(fit::Soup& s, const double (&r)[9], double scale, const double (&t)[3])
	{
		auto map = [&](float* p) {
			const double x = p[0], y = p[1], z = p[2];
			for (int i = 0; i < 3; i++)
				p[i] = static_cast<float>((r[i * 3] * x + r[i * 3 + 1] * y + r[i * 3 + 2] * z) * scale + t[i]);
		};
		for (fit::Shape& sh : s.shapes)
			for (size_t i = 0; i + 2 < sh.pos.size(); i += 3) map(&sh.pos[i]);
		map(s.hints.projectile);
		map(s.hints.sighting);
	}

	struct Expect
	{
		double eye[3];
		double axis[3];
		double up[3];
		double eye_r;
		double distance;
		double obj_r;
		int aperture = 0;
		double aspect = 1.0;
	};

	bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

	bool near3(const float* a, const double* b, double tol)
	{
		return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
	}

	std::string v3(const float* a) { return std::format("({:.3f}, {:.3f}, {:.3f})", a[0], a[1], a[2]); }

	void expect_fit(const char* name, const fit::Soup& s, const Expect& e, double tol = 0.03, int level = 2)
	{
		const fit::Result r = fit::fit(s);
		check(r.found && r.level >= level, std::format("{}: found at level {} (got level {}: {})", name, level, r.level, r.why));
		if (!r.found) return;
		check(near3(r.eyepiece, e.eye, tol), std::format("{}: eyepiece {} (want {:.3f}, {:.3f}, {:.3f})", name,
			v3(r.eyepiece), e.eye[0], e.eye[1], e.eye[2]));
		check(near3(r.axis, e.axis, 1e-4), std::format("{}: axis {}", name, v3(r.axis)));
		check(near3(r.up, e.up, 1e-4), std::format("{}: up {}", name, v3(r.up)));
		check(near(r.eyepiece_radius, e.eye_r, tol), std::format("{}: eyepiece radius {:.3f} (want {:.3f})", name,
			r.eyepiece_radius, e.eye_r));
		check(near(r.objective_distance, e.distance, tol), std::format("{}: objective distance {:.3f} (want {:.3f})", name,
			r.objective_distance, e.distance));
		check(near(r.objective_radius, e.obj_r, tol), std::format("{}: objective radius {:.3f} (want {:.3f})", name,
			r.objective_radius, e.obj_r));
		check(r.aperture == e.aperture && near(r.aperture_aspect, e.aspect, 0.02), std::format("{}: aperture {} aspect "
			"{:.3f} (want {} {:.3f})", name, r.aperture, r.aperture_aspect, e.aperture, e.aspect));
	}

	void expect_none(const char* name, const fit::Soup& s, const char* words = nullptr)
	{
		const fit::Result r = fit::fit(s);
		check(!r.found, std::format("{}: refused ({})", name, r.why));
		if (words) check(r.why.find(words) != std::string::npos, std::format("{}: says '{}'", name, words));
	}

	fit::Soup main_scope()
	{
		fit::Soup s;
		add_scope(s, Scope{});
		add_barrel(s);
		return s;
	}

	fit::Geometry geometry(const Expect& e)
	{
		fit::Geometry g;
		for (int i = 0; i < 3; i++)
		{
			g.eyepiece[i] = static_cast<float>(e.eye[i]);
			g.axis[i] = static_cast<float>(e.axis[i]);
			g.up[i] = static_cast<float>(e.up[i]);
		}
		g.eyepiece_radius = static_cast<float>(e.eye_r);
		g.objective_distance = static_cast<float>(e.distance);
		g.objective_radius = static_cast<float>(e.obj_r);
		return g;
	}

	void synthetic()
	{
		const Expect base = { { 0.2, 10.0, 0.0 }, { 1, 0, 0 }, { 0, 1, 0 }, 0.95, 12.15 - 0.2, 0.95 };
		expect_fit("scope above a longer barrel", main_scope(), base);

		{
			fit::Soup s;
			add_scope(s, Scope{}, false);
			add_barrel(s);
			expect_fit("open tube, no glass", s, { { 0.0, 10.0, 0.0 }, { 1, 0, 0 }, { 0, 1, 0 }, 1.0, 12.15, 1.0 });
		}
		{
			fit::Soup s;
			s.shapes.push_back(tube(0.0, 12.0, 10.0, 0.0, 1.0));
			s.shapes.push_back(ring(0.0, 10.0, 0.0, 0.7, 1.0));
			s.shapes.push_back(disc(0.4, 10.0, 0.0, 1.0, true));
			s.shapes.push_back(disc(11.8, 10.0, 0.0, 0.95, true));
			add_barrel(s);
			expect_fit("glass recessed behind a narrower rim", s, { { 0.4, 10.0, 0.0 }, { 1, 0, 0 }, { 0, 1, 0 }, 0.7,
				12.15 - 0.4, 0.95 });
		}
		{
			fit::Soup s;
			s.shapes.push_back(tube(0.0, 12.0, 10.0, 0.0, 1.0));
			s.shapes.push_back(dome(0.0, 10.0, 0.0, 1.0, 0.6, true));
			s.shapes.push_back(disc(11.8, 10.0, 0.0, 0.95, true));
			add_barrel(s);
			expect_fit("domed eyepiece glass", s, { { -0.6, 10.0, 0.0 }, { 1, 0, 0 }, { 0, 1, 0 }, 1.0, 12.15 + 0.6, 0.95 });
		}
		{
			fit::Soup s;
			s.shapes.push_back(box(1.0, 17.0, 14.9, 0.0, 2.0, 4.2));
			s.shapes.push_back(pane(1.3, 14.9, 0.0, 1.9, 4.1, true));
			s.shapes.push_back(disc(16.8, 14.9, 0.0, 0.9, true));
			add_barrel(s, 10.0);
			expect_fit("rectangular rear window, round front glass", s, { { 1.3, 14.9, 0.0 }, { 1, 0, 0 }, { 0, 1, 0 },
				0.95, 16.95 - 1.3, 0.9, 1, 4.1 / 1.9 });
		}
		{
			// -Y forward, +X up: x' = y, y' = -x.
			fit::Soup s = main_scope();
			const double r[9] = { 0, 1, 0, -1, 0, 0, 0, 0, 1 };
			const double t[3] = { 0, 0, 0 };
			transform(s, r, 1.0, t);
			expect_fit("the same scope, -Y forward and +X up", s, { { 10.0, -0.2, 0.0 }, { 0, -1, 0 }, { 1, 0, 0 }, 0.95,
				11.95, 0.95 });
		}
		{
			fit::Soup s;
			Scope sc;
			sc.x0 = 2.0;
			sc.x1 = 18.0;
			sc.cy = 12.5;
			sc.cz = 0.3;
			sc.r = 1.3;
			sc.eye_x = 2.3;
			sc.eye_r = 1.2;
			sc.obj_x = 17.6;
			sc.obj_r = 1.25;
			add_scope(s, sc);
			add_barrel(s);
			expect_fit("a moved, larger and longer scope (a replacer)", s, { { 2.3, 12.5, 0.3 }, { 1, 0, 0 }, { 0, 1, 0 },
				1.2, 18.15 - 2.3, 1.25 });
		}
		{
			// Root scale 2: the mesh is half as large in root units.
			fit::Soup s = main_scope();
			const double r[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
			const double t[3] = { 0, 0, 0 };
			transform(s, r, 0.5, t);
			s.unit_scale = 2.0f;
			expect_fit("unit scale 2", s, { { 0.1, 5.0, 0.0 }, { 1, 0, 0 }, { 0, 1, 0 }, 0.475, 11.95 * 0.5, 0.475 }, 0.02);
		}
		{
			fit::Soup s;
			add_scope(s, Scope{});
			s.shapes.push_back(pane(13.5, 10.0, 0.0, 3.0, 3.0, false));
			add_barrel(s);
			expect_fit("a plate ahead of the objective", s, { { 0.2, 10.0, 0.0 }, { 1, 0, 0 }, { 0, 1, 0 }, 0.95,
				13.65 - 0.2, 0.95 });
		}
		{
			fit::Soup s;
			add_barrel(s);
			expect_none("barrel only", s);
		}
		{
			fit::Soup s;
			s.shapes.push_back(disc(68.0, 3.0, 0.0, 1.0, true));
			add_barrel(s);
			expect_none("one glass disc only (a camera lens)", s, "no second surface");
		}
		{
			fit::Soup s;
			Scope a, b;
			a.cz = 2.5;
			b.cz = -2.5;
			add_scope(s, a);
			add_scope(s, b);
			add_barrel(s);
			expect_none("two parallel scopes of equal merit", s, "too close to call");
		}
		{
			fit::Soup s = main_scope();
			s.hints.have_projectile = false;
			expect_none("no projectile node or eye", s);
			s.hints.have_eye = true;
			s.hints.eye[0] = -6.0f;
			s.hints.eye[1] = 10.0f;
			expect_fit("no projectile node, the eye behind", s, base, 0.03, 1);
		}

		// agrees and mesh_check.
		{
			const fit::Soup s = main_scope();
			const fit::Result r = fit::fit(s);
			fit::Tolerances tol;
			tol.objective_behind = 0.5f;
			tol.objective_ahead = 0.5f;
			tol.objective_radius_fraction = 0.15f;
			const fit::Agreement same = fit::agrees(r, geometry(base), 1.0f, tol);
			check(same.ok, std::format("agrees: the built geometry ({})", same.why));
			Expect shifted = base;
			shifted.eye[1] += 1.31;
			const fit::Agreement off = fit::agrees(r, geometry(shifted), 1.0f, tol);
			check(!off.ok && off.why.find("1.31") != std::string::npos, std::format("agrees: shifted 1.31 ({})", off.why));
			Expect buried = base;
			buried.distance = 9.0;
			const fit::Agreement short_obj = fit::agrees(r, geometry(buried), 1.0f, tol);
			check(!short_obj.ok && short_obj.why.find("behind") != std::string::npos,
				std::format("agrees: objective buried ({})", short_obj.why));

			const fit::Check good = fit::mesh_check(s, geometry(base));
			check(good.ok, std::format("mesh check: the built geometry ({})", good.why));
			const fit::Check moved = fit::mesh_check(s, geometry(shifted));
			check(!moved.ok, std::format("mesh check: shifted 1.31 ({})", moved.why));
			const fit::Check inside = fit::mesh_check(s, geometry(buried));
			check(!inside.ok, std::format("mesh check: objective buried ({})", inside.why));
		}
	}

	// ---- Soups and lens files ----------------------------------------------------

	struct Xform
	{
		double r[9];
		double t[3];
		double s;
	};

	struct SoupFile
	{
		std::string model;
		fit::Soup soup;
		std::map<std::string, Xform> nodes;
		bool expect_none = false;
	};

	std::string lower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	std::string trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
		while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
		return s.substr(a, b - a);
	}

	// model <path> / scale <s> / node <name> x y z / xform <r00..r22 tx ty tz s> <name> /
	// expect none / shape <hidden> <skinned> <blended> <verts> <tris> <name>, then the
	// vertex lines (x y z) and the triangle lines (a b c). Hidden and skinned shapes are
	// left out, as the engine-side collector leaves them out.
	bool read_soup(const std::filesystem::path& path, SoupFile& out)
	{
		std::ifstream in(path);
		if (!in) return false;
		std::string line;
		while (std::getline(in, line))
		{
			std::istringstream ls(line);
			std::string word;
			ls >> word;
			if (word == "model") out.model = lower(trim(line.substr(6)));
			else if (word == "scale") ls >> out.soup.unit_scale;
			else if (word == "expect") out.expect_none = true;
			else if (word == "node")
			{
				std::string name;
				float p[3] = {};
				ls >> name >> p[0] >> p[1] >> p[2];
				if (name == "ProjectileNode")
				{
					out.soup.hints.have_projectile = true;
					std::memcpy(out.soup.hints.projectile, p, sizeof(p));
				}
				else if (name == "##SightingNode")
				{
					out.soup.hints.have_sighting = true;
					std::memcpy(out.soup.hints.sighting, p, sizeof(p));
				}
			}
			else if (word == "xform")
			{
				Xform x = {};
				for (double& v : x.r) ls >> v;
				for (double& v : x.t) ls >> v;
				ls >> x.s;
				std::string name;
				std::getline(ls, name);
				out.nodes[trim(name)] = x;
			}
			else if (word == "shape")
			{
				int hidden = 0, skinned = 0, blended = 0;
				size_t nv = 0, nt = 0;
				ls >> hidden >> skinned >> blended >> nv >> nt;
				fit::Shape sh;
				sh.blended = blended != 0;
				sh.pos.resize(nv * 3);
				sh.tris.resize(nt * 3);
				for (size_t i = 0; i < nv * 3; i++) in >> sh.pos[i];
				for (size_t i = 0; i < nt * 3; i++) in >> sh.tris[i];
				std::getline(in, line);
				if (!hidden && !skinned) out.soup.shapes.push_back(std::move(sh));
			}
		}
		return !out.model.empty();
	}

	using Keys = std::map<std::string, std::string>;   // lowercase key -> value

	struct LensFile
	{
		std::string name;
		std::string confidence;
		std::map<std::string, Keys> sections;   // "lens", "lens.23", ...
	};

	bool read_lens(const std::filesystem::path& path, LensFile& out)
	{
		std::ifstream in(path);
		if (!in) return false;
		out.name = path.filename().string();
		std::string line, section;
		while (std::getline(in, line))
		{
			line = trim(line);
			if (line.empty()) continue;
			if (line[0] == ';')
			{
				const size_t at = line.find("Confidence:");
				if (at != std::string::npos)
				{
					out.confidence = trim(line.substr(at + 11));
					while (!out.confidence.empty() && out.confidence.back() == '.') out.confidence.pop_back();
				}
				continue;
			}
			if (line[0] == '[')
			{
				section = lower(trim(line.substr(1, line.find(']') - 1)));
				out.sections[section];
				continue;
			}
			const size_t eq = line.find('=');
			if (eq == std::string::npos || section.empty()) continue;
			out.sections[section][lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
		}
		return true;
	}

	void floats(const std::string& v, float* out, int n)
	{
		std::string s = v;
		for (char& c : s)
			if (c == ',') c = ' ';
		std::istringstream ls(s);
		for (int i = 0; i < n; i++) ls >> out[i];
	}

	std::vector<std::string> models(const std::string& v)
	{
		std::vector<std::string> out;
		size_t at = 0;
		while (at <= v.size())
		{
			const size_t end = (std::min)(v.find(',', at), v.size());
			std::string m = lower(trim(v.substr(at, end - at)));
			for (char& c : m)
				if (c == '/') c = '\\';
			if (!m.empty()) out.push_back(m);
			at = end + 1;
		}
		return out;
	}

	// The section's geometry in root space (through its Node's rest transform).
	bool file_geometry(const Keys& k, const SoupFile& sf, fit::Geometry& g, std::string& why)
	{
		const auto get = [&](const char* key) -> const std::string* {
			const auto it = k.find(key);
			return it == k.end() ? nullptr : &it->second;
		};
		if (!get("eyepiececenter"))
		{
			why = "no geometry in the file";
			return false;
		}
		floats(*get("eyepiececenter"), g.eyepiece, 3);
		if (get("axis")) floats(*get("axis"), g.axis, 3);
		if (get("up")) floats(*get("up"), g.up, 3);
		if (get("eyepieceradius")) floats(*get("eyepieceradius"), &g.eyepiece_radius, 1);
		if (get("objectivedistance")) floats(*get("objectivedistance"), &g.objective_distance, 1);
		if (get("objectiveradius")) floats(*get("objectiveradius"), &g.objective_radius, 1);
		const std::string* node = get("node");
		if (!node || node->empty()) return true;
		const auto it = sf.nodes.find(*node);
		if (it == sf.nodes.end())
		{
			why = "the file's node is not in the soup";
			return false;
		}
		const Xform& x = it->second;
		auto rot = [&](const float* v, float* o) {
			const double a = v[0], b = v[1], c = v[2];
			for (int i = 0; i < 3; i++) o[i] = static_cast<float>(x.r[i * 3] * a + x.r[i * 3 + 1] * b + x.r[i * 3 + 2] * c);
		};
		float e[3], ax[3], up[3];
		rot(g.eyepiece, e);
		rot(g.axis, ax);
		rot(g.up, up);
		for (int i = 0; i < 3; i++)
		{
			g.eyepiece[i] = static_cast<float>(e[i] * x.s + x.t[i]);
			g.axis[i] = ax[i];
			g.up[i] = up[i];
		}
		g.eyepiece_radius = static_cast<float>(g.eyepiece_radius * x.s);
		g.objective_distance = static_cast<float>(g.objective_distance * x.s);
		g.objective_radius = static_cast<float>(g.objective_radius * x.s);
		return true;
	}

	bool g_candidates = false;

	bool verbose_candidates() { return g_candidates; }

	const char* level_name(int level) { return level == 2 ? "high" : level == 1 ? "medium" : "none"; }

	int compare(const std::filesystem::path& soups, const std::filesystem::path& lenses)
	{
		std::map<std::string, SoupFile> by_model;
		std::error_code ec;
		for (const auto& f : std::filesystem::directory_iterator(soups, ec))
		{
			if (f.path().extension() != ".soup") continue;
			SoupFile sf;
			if (read_soup(f.path(), sf)) by_model[sf.model] = std::move(sf);
		}
		std::vector<LensFile> files;
		for (const auto& f : std::filesystem::directory_iterator(lenses, ec))
		{
			if (f.path().extension() != ".ini") continue;
			LensFile lf;
			if (read_lens(f.path(), lf)) files.push_back(std::move(lf));
		}
		std::sort(files.begin(), files.end(), [](const LensFile& a, const LensFile& b) { return a.name < b.name; });
		std::printf("%zu soups, %zu lens files\n", by_model.size(), files.size());

		fit::Tolerances tol;
		tol.objective_behind = 0.5f;
		tol.objective_ahead = 0.5f;
		tol.objective_radius_fraction = 0.15f;

		std::map<std::string, fit::Result> fits;
		auto fitted = [&](const SoupFile& sf) -> const fit::Result& {
			auto it = fits.find(sf.model);
			if (it == fits.end()) it = fits.emplace(sf.model, fit::fit(sf.soup)).first;
			return it->second;
		};

		int high_pairs = 0, high_agree = 0, pairs = 0, agree_all = 0, usable_all = 0, hard = 0, missing = 0;
		std::set<std::string> seen;
		std::printf("%-44s %-10s %-62s %-6s %-5s %7s %7s %6s %5s %4s %6s %5s  %s\n", "file", "section", "model", "level",
			"conf", "lateral", "axial", "radius", "axis", "up", "obj", "agree", "why");
		for (const LensFile& lf : files)
		{
			const auto base_it = lf.sections.find("lens");
			for (const auto& [name, own] : lf.sections)
			{
				Keys k = base_it != lf.sections.end() ? base_it->second : Keys{};
				for (const auto& [key, value] : own) k[key] = value;
				const auto mit = k.find("models");
				if (mit == k.end()) continue;
				const auto fit_it = k.find("fit");
				const bool fit_file = fit_it != k.end() && lower(fit_it->second) == "file";
				for (const std::string& model : models(mit->second))
				{
					seen.insert(model);
					const auto sit = by_model.find(model);
					if (sit == by_model.end())
					{
						std::printf("%-44s %-10s %-62s (no soup)\n", lf.name.c_str(), name.c_str(), model.c_str());
						missing++;
						continue;
					}
					const SoupFile& sf = sit->second;
					const fit::Result& r = fitted(sf);
					pairs++;
					const bool high = lf.confidence == "high";
					if (high) high_pairs++;
					fit::Geometry g;
					std::string why;
					if (!file_geometry(k, sf, g, why))
					{
						std::printf("%-44s %-10s %-62s %-6s %.2f  (%s)\n", lf.name.c_str(), name.c_str(), model.c_str(),
							level_name(r.level), r.confidence, why.c_str());
						continue;
					}
					if (!r.found)
					{
						const fit::Check mc = fit::mesh_check(sf.soup, g);
						std::printf("%-44s %-10s %-62s %-6s %.2f  %s; mesh check %s%s\n", lf.name.c_str(), name.c_str(),
							model.c_str(), level_name(r.level), r.confidence, r.why.c_str(), mc.ok ? "passes" : "fails: ",
							mc.ok ? "" : mc.why.c_str());
						continue;
					}
					usable_all++;
					const fit::Agreement a = fit::agrees(r, g, sf.soup.unit_scale, tol);
					if (a.ok) agree_all++;
					if (high && a.ok) high_agree++;
					const double off = std::hypot(static_cast<double>(a.lateral), static_cast<double>(a.axial));
					const bool far_off = off > 1.0 && !fit_file;
					if (far_off) hard++;
					// With Fit = file the file's geometry is used when it passes the
					// mesh check.
					std::string kept;
					if (fit_file)
					{
						const fit::Check mc = fit::mesh_check(sf.soup, g);
						kept = mc.ok ? "; Fit = file: mesh check passes, the file's geometry is used"
							: "; Fit = file: mesh check fails (" + mc.why + "), the fit is used";
					}
					std::printf("%-44s %-10s %-62s %-6s %.2f %7.3f %7.3f %6.3f %5.2f %4.0f %6.2f %5s  %s%s%s%s\n",
						lf.name.c_str(), name.c_str(), model.c_str(), level_name(r.level), r.confidence, a.lateral, a.axial,
						a.radius, a.axis_degrees, a.up_degrees, a.objective, a.ok ? "yes" : "NO", a.ok ? "" : a.why.c_str(),
						a.ok ? "" : "; ", far_off ? "FAR OFF (hard failure)" : r.why.c_str(), kept.c_str());
				}
			}
		}
		for (const auto& [model, sf] : by_model)
		{
			if (seen.count(model)) continue;
			const fit::Result& r = fitted(sf);
			const bool bad = sf.expect_none && r.found;
			if (bad) hard++;
			std::printf("%-44s %-10s %-62s %-6s %.2f  %s%s\n", "(no lens file)", "", model.c_str(), level_name(r.level),
				r.confidence, r.why.c_str(), bad ? "; FITS BUT SHOULD NOT (hard failure)" : sf.expect_none ? "; expected none" : "");
		}
		const double share = high_pairs ? 100.0 * high_agree / high_pairs : 0.0;
		std::printf("\nsummary: %d pairs (%d without a soup); %d usable, %d agree\n", pairs, missing, usable_all, agree_all);
		std::printf("Confidence: high pairs usable and agreeing: %d of %d (%.1f%%, need 90%%)\n", high_agree, high_pairs, share);
		std::printf("hard failures: %d\n", hard);
		if (verbose_candidates())
			for (const auto& [model, r] : fits)
			{
				std::printf("\n%s\n", model.c_str());
				for (const fit::Candidate& c : r.candidates)
					std::printf("  %s p (%.2f, %.2f, %.2f) axis (%.3f, %.3f, %.3f) span %.2f tube %.2f score %.2f members %d "
						"hits %d: %s\n", c.usable ? "usable " : "refused", c.point[0], c.point[1], c.point[2], c.axis[0],
						c.axis[1], c.axis[2], c.span, c.tube, c.score, c.members, c.hits, c.why.c_str());
			}
		return hard + (share < 90.0 ? 1 : 0);
	}

}

int main(int argc, char** argv)
{
	std::string soups, lenses;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--soups" && i + 1 < argc) soups = argv[++i];
		else if (a == "--lenses" && i + 1 < argc) lenses = argv[++i];
		else if (a == "--candidates") g_candidates = true;
	}
	if (!soups.empty() && !lenses.empty()) return compare(soups, lenses);

	synthetic();
	std::printf("%s\n", g_failed ? "=== FAILURES ===" : "all ok");
	return g_failed;
}
