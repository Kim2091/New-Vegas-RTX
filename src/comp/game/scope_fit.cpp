#include "std_include.hpp"
#include "scope_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace comp::game::scopes::fit
{
	namespace
	{
		// ---- Constants: world game units and degrees ------------------------------

		constexpr size_t kMaxTriangles = 300000;
		constexpr double kWeld = 1e-3;                 // vertices this close are one (UV seams)
		constexpr double kPatchBendDegrees = 10.0;     // between neighbors of a flat or gently domed patch
		constexpr double kPatchTiltDegrees = 30.0;     // an opening patch's triangles from its first one
		constexpr double kGlassTiltDegrees = 40.0;     // a glass patch's triangles from the axis
		constexpr double kDomeTiltDegrees = 70.0;      // ... and a coarse dome's
		constexpr double kDomeBendDegrees = 30.0;
		constexpr double kMinOpening = 0.3;            // opening radius range
		constexpr double kMaxOpening = 4.0;
		constexpr double kRoundExtents = 0.2;          // |width - height| over the larger, for a round opening
		constexpr double kRoundRadius = 0.6;           // largest radius over the larger extent (circle 0.5, square 0.71)
		constexpr size_t kMinOpeningVerts = 6;
		constexpr double kRingFill = 0.7;              // a round patch filling less than this is a ring
		constexpr double kLoopPlanarity = 0.1;         // a loop's distance from its plane, of its radius
		constexpr double kLoopCircularity = 0.1;       // a loop's radius spread (deviation over mean)
		constexpr double kParallelDegrees = 3.0;
		constexpr double kLateralTolerance = 0.12;     // an opening's center from a line it belongs to
		constexpr double kLateralFraction = 0.15;      // ... or this fraction of its radius
		constexpr double kSnapDegrees = 0.5;           // an axis this close to a root axis is that axis
		constexpr size_t kMaxCandidates = 96;
		constexpr double kMinSpan = 3.0;
		constexpr double kBoreDistance = 1.0;          // a line this close to the projectile node is the barrel
		constexpr double kSectionStep = 0.5;
		constexpr int kMinSections = 8;
		constexpr int kMaxSections = 160;
		constexpr double kWallCos = 0.9;               // a triangle facing along the axis more than this is no wall
		constexpr int kSectors = 8;
		constexpr int kTubeSectors = 6;                // sectors with a wall for a section to be inside a tube
		constexpr double kTubeMaxRadius = 4.0;
		constexpr double kRunGap = 1.0;                // a tube's walls may break off this long
		constexpr double kEndMargin = 0.5;             // ends are looked for this far outside the tube's walls
		constexpr double kProbeOffset = 0.2;           // the extra rays' distance from the axis
		constexpr double kRecessMax = 1.5;             // glass this far inside a rim is still the end
		constexpr double kRimDepth = 1.0;              // walls this far behind the eyepiece bound its radius
		constexpr double kPlaneStep = 0.1;
		constexpr double kInsideDepth = 0.6;           // the clear aperture just inside a glass piece
		constexpr double kCurvedDegrees = 8.0;         // a patch with triangles tilted more is domed
		constexpr double kCapDepth = 1.5;              // a glass piece no deeper than this is a cap
		constexpr double kRoundFill = 0.6;
		constexpr double kRoundGlassExtents = 0.15;
		constexpr double kRectFill = 0.8;
		constexpr double kRimFloor = 0.3;              // a rim narrower than this fraction of the glass is not its rim
		constexpr double kFrontMargin = 0.15;          // the objective point past the scope's front end
		constexpr double kFrontReach = 6.0;            // the objective piece's vertices this far ahead count
		constexpr double kFrontSlab = 1.5;             // ... and its shape's vertices this far ahead
		constexpr double kObjectiveClear = 8.0;        // no geometry this far ahead of the objective point
		constexpr double kMaxPush = 2.0;               // the objective point moves at most this far past a blocker
		constexpr double kEyeRelief = 5.6;
		constexpr double kEyeGap = 0.05;
		constexpr double kMinEyeRadius = 0.3;
		constexpr double kMaxEyeRadius = 3.0;
		constexpr double kMinRatio = 0.4;              // objective radius over eyepiece radius
		constexpr double kMaxRatio = 3.0;
		constexpr double kSightingLateral = 1.0;
		constexpr double kSightingBehind = 15.0;
		constexpr double kBoreParallelDegrees = 3.0;
		constexpr double kBoreMaxDegrees = 5.0;        // an axis further off the barrel's is not a scope
		constexpr double kTubeHigh = 0.5;
		constexpr double kMarginHigh = 0.2;
		constexpr double kMarginMin = 0.05;
		constexpr double kSameScope = 0.3;             // two candidates' eyepieces this close are one scope
		constexpr double kCheckDepth = 0.5;            // the mesh check looks for the eyepiece this far either side
		constexpr double kCheckRadiusLow = 0.5;
		constexpr double kCheckRadiusHigh = 2.0;

		// Score weights.
		constexpr double kScoreTube = 0.35;
		constexpr double kScoreEye = 0.2;
		constexpr double kScoreObjective = 0.15;
		constexpr double kScoreGlass = 0.15;
		constexpr double kScoreSighting = 0.1;
		constexpr double kScoreParallel = 0.05;
		constexpr double kScoreStandoff = 0.15;       // the usable axis farthest from the bore line gets all of it

		constexpr double kPi = 3.14159265358979323846;

		double cos_deg(double deg) { return std::cos(deg * kPi / 180.0); }

		// ---- Vectors ---------------------------------------------------------------

		struct D3
		{
			double x = 0.0, y = 0.0, z = 0.0;
		};

		D3 operator+(D3 a, D3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		D3 operator-(D3 a, D3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		D3 operator*(D3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
		D3 operator-(D3 a) { return { -a.x, -a.y, -a.z }; }
		double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		D3 cross(D3 a, D3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		double length(D3 a) { return std::sqrt(dot(a, a)); }
		D3 unit(D3 a)
		{
			const double l = length(a);
			return l > 1e-12 ? a * (1.0 / l) : D3{};
		}
		D3 d3(const float* p) { return { p[0], p[1], p[2] }; }
		void put(float* out, D3 a)
		{
			out[0] = static_cast<float>(a.x);
			out[1] = static_cast<float>(a.y);
			out[2] = static_cast<float>(a.z);
		}
		double degrees_between(D3 a, D3 b)
		{
			const double c = std::clamp(dot(unit(a), unit(b)), -1.0, 1.0);
			return std::acos(c) * 180.0 / kPi;
		}

		const D3 kAxes[3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };

		// The root axis (either sign) `a` is within kSnapDegrees of, else `a`.
		D3 snap(D3 a)
		{
			const double c = cos_deg(kSnapDegrees);
			for (const D3& e : kAxes)
			{
				const double s = dot(a, e);
				if (std::fabs(s) >= c) return e * (s < 0.0 ? -1.0 : 1.0);
			}
			return a;
		}

		bool axis_aligned(D3 a)
		{
			for (const D3& e : kAxes)
				if (std::fabs(dot(a, e)) > 1.0 - 1e-9) return true;
			return false;
		}

		// A unit vector perpendicular to `n` along the root axis least aligned with it.
		D3 perpendicular(D3 n)
		{
			D3 best = kAxes[0];
			double lo = 2.0;
			for (const D3& e : kAxes)
			{
				const double c = std::fabs(dot(e, n));
				if (c < lo) { lo = c; best = e; }
			}
			return unit(cross(n, best));
		}

		// ---- The model, prepared ---------------------------------------------------

		struct Mesh
		{
			std::vector<D3> p;
			std::vector<uint32_t> tri;       // three vertex indices per triangle
			std::vector<uint32_t> weld;      // welded id per vertex
			uint32_t welded = 0;
			std::vector<D3> n;               // per triangle, unit; zero when degenerate
			std::vector<double> area;
			std::vector<uint32_t> adj_at;    // welded id -> its range in adj
			std::vector<uint32_t> adj;       // triangles per welded id
			std::vector<D3> rep;             // one position per welded id
			bool blended = false;
			uint32_t shape = 0;              // index into Soup::shapes
			size_t tris() const { return tri.size() / 3; }
			D3 v(size_t t, int c) const { return p[tri[t * 3 + static_cast<size_t>(c)]]; }
		};

		struct Model
		{
			std::vector<Mesh> m;
			double k = 1.0;                  // root units per world unit
			size_t tris = 0;
		};

		struct WeldKey
		{
			int64_t x, y, z;
			bool operator==(const WeldKey& o) const { return x == o.x && y == o.y && z == o.z; }
		};

		struct WeldHash
		{
			size_t operator()(const WeldKey& k) const
			{
				uint64_t h = static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
				h ^= static_cast<uint64_t>(k.y) + 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
				h ^= static_cast<uint64_t>(k.z) + 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
				return static_cast<size_t>(h ^ (h >> 32));
			}
		};

		bool prepare(const Soup& soup, Model& md, std::string& why)
		{
			const double scale = std::isfinite(soup.unit_scale) && soup.unit_scale > 1e-4f ? soup.unit_scale : 1.0;
			md.k = 1.0 / scale;
			md.m.clear();
			md.tris = 0;
			for (const Shape& s : soup.shapes) md.tris += s.tris.size() / 3;
			if (md.tris > kMaxTriangles)
			{
				why = std::format("{} triangles, over the fit's cap of {}", md.tris, kMaxTriangles);
				return false;
			}
			md.tris = 0;
			const double eps = kWeld * md.k;
			for (size_t si = 0; si < soup.shapes.size(); si++)
			{
				const Shape& s = soup.shapes[si];
				const size_t nv = s.pos.size() / 3;
				if (!nv || s.tris.size() < 3) continue;
				bool finite = true;
				for (const float f : s.pos) finite = finite && std::isfinite(f);
				if (!finite) continue;
				Mesh m;
				m.shape = static_cast<uint32_t>(si);
				m.blended = s.blended;
				m.p.resize(nv);
				for (size_t i = 0; i < nv; i++) m.p[i] = { s.pos[i * 3], s.pos[i * 3 + 1], s.pos[i * 3 + 2] };
				for (size_t i = 0; i + 2 < s.tris.size(); i += 3)
				{
					const uint32_t a = s.tris[i], b = s.tris[i + 1], c = s.tris[i + 2];
					if (a >= nv || b >= nv || c >= nv || a == b || b == c || a == c) continue;
					m.tri.push_back(a);
					m.tri.push_back(b);
					m.tri.push_back(c);
				}
				if (m.tri.empty()) continue;

				std::unordered_map<WeldKey, uint32_t, WeldHash> ids;
				ids.reserve(nv);
				m.weld.resize(nv);
				for (size_t i = 0; i < nv; i++)
				{
					const WeldKey key = { static_cast<int64_t>(std::llround(m.p[i].x / eps)),
						static_cast<int64_t>(std::llround(m.p[i].y / eps)), static_cast<int64_t>(std::llround(m.p[i].z / eps)) };
					const auto [it, fresh] = ids.try_emplace(key, m.welded);
					if (fresh)
					{
						m.rep.push_back(m.p[i]);
						m.welded++;
					}
					m.weld[i] = it->second;
				}

				const size_t nt = m.tris();
				m.n.resize(nt);
				m.area.resize(nt);
				std::vector<uint32_t> count(m.welded + 1u, 0u);
				for (size_t t = 0; t < nt; t++)
				{
					const D3 e = cross(m.v(t, 1) - m.v(t, 0), m.v(t, 2) - m.v(t, 0));
					const double l = length(e);
					m.area[t] = l * 0.5;
					m.n[t] = l > 1e-12 ? e * (1.0 / l) : D3{};
					for (int c = 0; c < 3; c++) count[m.weld[m.tri[t * 3 + static_cast<size_t>(c)]]]++;
				}
				m.adj_at.assign(m.welded + 1u, 0u);
				for (uint32_t w = 0; w < m.welded; w++) m.adj_at[w + 1] = m.adj_at[w] + count[w];
				m.adj.resize(m.adj_at[m.welded]);
				std::vector<uint32_t> fill(m.adj_at.begin(), m.adj_at.end() - 1);
				for (size_t t = 0; t < nt; t++)
					for (int c = 0; c < 3; c++) m.adj[fill[m.weld[m.tri[t * 3 + static_cast<size_t>(c)]]]++] = static_cast<uint32_t>(t);
				md.tris += nt;
				md.m.push_back(std::move(m));
			}
			if (md.m.empty())
			{
				why = "no triangles";
				return false;
			}
			return true;
		}

		// Breadth-first over triangles sharing a welded vertex, from `seed`,
		// taking those `accept(from, to)` allows.
		template <typename Accept>
		std::vector<uint32_t> spread(const Mesh& m, uint32_t seed, std::vector<char>& seen, Accept accept)
		{
			std::vector<uint32_t> out = { seed };
			seen[seed] = 1;
			for (size_t at = 0; at < out.size(); at++)
			{
				const uint32_t i = out[at];
				for (int c = 0; c < 3; c++)
				{
					const uint32_t w = m.weld[m.tri[static_cast<size_t>(i) * 3 + static_cast<size_t>(c)]];
					for (uint32_t a = m.adj_at[w]; a < m.adj_at[w + 1]; a++)
					{
						const uint32_t j = m.adj[a];
						if (seen[j] || !accept(i, j)) continue;
						seen[j] = 1;
						out.push_back(j);
					}
				}
			}
			return out;
		}

		// The glass around an axis hit: triangles facing along the axis within
		// kGlassTiltDegrees that bend less than kPatchBendDegrees from the
		// triangle they are reached from.
		std::vector<uint32_t> glass_patch(const Mesh& m, uint32_t seed, D3 d, double tilt_deg = kGlassTiltDegrees,
			double bend_deg = kPatchBendDegrees)
		{
			const double tilt = cos_deg(tilt_deg), bend = cos_deg(bend_deg);
			if (m.area[seed] <= 0.0 || std::fabs(dot(m.n[seed], d)) < tilt) return { seed };
			std::vector<char> seen(m.tris(), 0);
			return spread(m, seed, seen, [&](uint32_t i, uint32_t j) {
				return m.area[j] > 0.0 && std::fabs(dot(m.n[j], d)) >= tilt && std::fabs(dot(m.n[i], m.n[j])) >= bend;
			});
		}

		// The connected piece (welded) the triangle belongs to.
		std::vector<uint32_t> piece(const Mesh& m, uint32_t seed)
		{
			std::vector<char> seen(m.tris(), 0);
			return spread(m, seed, seen, [](uint32_t, uint32_t) { return true; });
		}

		// ---- Openings --------------------------------------------------------------

		struct Opening
		{
			D3 c;
			D3 n;
			double r = 0.0;          // outer radius
			double inner = 0.0;      // the clear radius (a ring's inner radius, a loop's radius)
			bool ring = false;       // a ring or a loop: open in the middle
			uint32_t mesh = 0;
		};

		// Statistics of a vertex set in a frame (origin o, normal d, in-plane u, v).
		struct Stats
		{
			double tmin = 0.0, tmax = 0.0, tmean = 0.0;
			double cu = 0.0, cv = 0.0;   // bounding-box center in the plane
			double eu = 0.0, ev = 0.0;   // extents along u and v
			double r = 0.0, inner = 0.0; // largest and smallest distance from the center
			double area = 0.0;           // projected along d
			double max_tilt = 0.0;       // degrees, the triangles' largest tilt from d
			bool round = false;
			bool rect = false;
			size_t verts = 0;
		};

		Stats stats(const Mesh& m, const std::vector<uint32_t>& tris, D3 o, D3 d, D3 u, D3 v)
		{
			Stats s;
			std::vector<uint32_t> ids;
			ids.reserve(tris.size() * 3);
			for (const uint32_t t : tris)
			{
				for (int c = 0; c < 3; c++) ids.push_back(m.tri[static_cast<size_t>(t) * 3 + static_cast<size_t>(c)]);
				s.area += m.area[t] * std::fabs(dot(m.n[t], d));
				if (m.area[t] > 0.0)
					s.max_tilt = (std::max)(s.max_tilt, degrees_between(m.n[t] * (dot(m.n[t], d) < 0.0 ? -1.0 : 1.0), d));
			}
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			s.verts = ids.size();
			if (ids.empty()) return s;
			double umin = 1e30, umax = -1e30, vmin = 1e30, vmax = -1e30, sum = 0.0;
			s.tmin = 1e30;
			s.tmax = -1e30;
			for (const uint32_t i : ids)
			{
				const D3 q = m.p[i] - o;
				const double t = dot(q, d), a = dot(q, u), b = dot(q, v);
				s.tmin = (std::min)(s.tmin, t);
				s.tmax = (std::max)(s.tmax, t);
				sum += t;
				umin = (std::min)(umin, a);
				umax = (std::max)(umax, a);
				vmin = (std::min)(vmin, b);
				vmax = (std::max)(vmax, b);
			}
			s.tmean = sum / static_cast<double>(ids.size());
			s.cu = (umin + umax) * 0.5;
			s.cv = (vmin + vmax) * 0.5;
			s.eu = umax - umin;
			s.ev = vmax - vmin;
			s.inner = 1e30;
			for (const uint32_t i : ids)
			{
				const D3 q = m.p[i] - o;
				const double r = std::hypot(dot(q, u) - s.cu, dot(q, v) - s.cv);
				s.r = (std::max)(s.r, r);
				s.inner = (std::min)(s.inner, r);
			}
			const double big = (std::max)(s.eu, s.ev);
			const double fill = s.r > 0.0 ? s.area / (kPi * s.r * s.r) : 0.0;
			s.round = big > 0.0 && fill >= kRoundFill && std::fabs(s.eu - s.ev) <= kRoundGlassExtents * big;
			s.rect = !s.round && s.eu > 0.0 && s.ev > 0.0 && s.area / (s.eu * s.ev) >= kRectFill;
			return s;
		}

		// Flat or gently domed round patches: glass discs, rings, rims.
		void patch_openings(const Model& md, uint32_t mi, std::vector<Opening>& out)
		{
			const Mesh& m = md.m[mi];
			const double tilt = cos_deg(kPatchTiltDegrees), bend = cos_deg(kPatchBendDegrees);
			std::vector<char> seen(m.tris(), 0);
			for (uint32_t t = 0; t < m.tris(); t++)
			{
				if (seen[t] || m.area[t] <= 0.0) continue;
				const D3 ns = m.n[t];
				const std::vector<uint32_t> patch = spread(m, t, seen, [&](uint32_t i, uint32_t j) {
					return m.area[j] > 0.0 && dot(m.n[j], ns) >= tilt && dot(m.n[i], m.n[j]) >= bend;
				});
				D3 mean;
				for (const uint32_t i : patch) mean = mean + m.n[i] * m.area[i];
				const D3 n = unit(mean);
				if (length(n) < 0.5) continue;
				const D3 u = perpendicular(n), v = cross(n, u);
				const Stats s = stats(m, patch, D3{}, n, u, v);
				if (s.verts < kMinOpeningVerts) continue;
				if (s.r < kMinOpening * md.k || s.r > kMaxOpening * md.k) continue;
				const double big = (std::max)(s.eu, s.ev);
				if (std::fabs(s.eu - s.ev) > kRoundExtents * big || s.r > kRoundRadius * big) continue;
				Opening o;
				o.c = n * s.tmean + u * s.cu + v * s.cv;
				o.n = n;
				o.r = s.r;
				const double fill = s.area / (kPi * s.r * s.r);
				o.ring = fill < kRingFill;
				o.inner = o.ring ? s.inner : s.r;
				o.mesh = mi;
				out.push_back(o);
			}
		}

		// Closed boundary loops (edges with one triangle) that are planar and
		// circular: the mouths of open tubes and the edges of separate discs.
		void loop_openings(const Model& md, uint32_t mi, std::vector<Opening>& out)
		{
			const Mesh& m = md.m[mi];
			std::unordered_map<uint64_t, int> edges;
			edges.reserve(m.tri.size());
			for (size_t t = 0; t < m.tris(); t++)
				for (int c = 0; c < 3; c++)
				{
					uint32_t a = m.weld[m.tri[t * 3 + static_cast<size_t>(c)]];
					uint32_t b = m.weld[m.tri[t * 3 + static_cast<size_t>((c + 1) % 3)]];
					if (a == b) continue;
					if (a > b) std::swap(a, b);
					edges[(static_cast<uint64_t>(a) << 32) | b]++;
				}
			std::vector<uint32_t> nb_count(m.welded, 0u);
			std::vector<uint32_t> nb(static_cast<size_t>(m.welded) * 2u, 0u);
			for (const auto& [key, n] : edges)
			{
				if (n != 1) continue;
				const uint32_t a = static_cast<uint32_t>(key >> 32), b = static_cast<uint32_t>(key & 0xFFFFFFFFu);
				for (const uint32_t x : { a, b })
				{
					if (nb_count[x] < 2) nb[static_cast<size_t>(x) * 2u + nb_count[x]] = x == a ? b : a;
					nb_count[x]++;
				}
			}
			std::vector<char> used(m.welded, 0);
			for (uint32_t start = 0; start < m.welded; start++)
			{
				if (used[start] || nb_count[start] != 2) continue;
				std::vector<uint32_t> loop = { start };
				used[start] = 1;
				uint32_t prev = start, at = nb[static_cast<size_t>(start) * 2u];
				bool closed = false;
				while (loop.size() <= m.welded)
				{
					if (at == start) { closed = true; break; }
					if (used[at] || nb_count[at] != 2) break;
					used[at] = 1;
					loop.push_back(at);
					const uint32_t next = nb[static_cast<size_t>(at) * 2u] == prev ? nb[static_cast<size_t>(at) * 2u + 1u]
						: nb[static_cast<size_t>(at) * 2u];
					prev = at;
					at = next;
				}
				if (!closed || loop.size() < kMinOpeningVerts) continue;
				D3 c;
				for (const uint32_t w : loop) c = c + m.rep[w];
				c = c * (1.0 / static_cast<double>(loop.size()));
				D3 newell;
				for (size_t i = 0; i < loop.size(); i++)
				{
					const D3 a = m.rep[loop[i]] - c, b = m.rep[loop[(i + 1) % loop.size()]] - c;
					newell = newell + cross(a, b);
				}
				const D3 n = unit(newell);
				if (length(n) < 0.5) continue;
				double sum = 0.0, sum2 = 0.0, off = 0.0;
				for (const uint32_t w : loop)
				{
					const D3 q = m.rep[w] - c;
					const double h = dot(q, n);
					const double r = length(q - n * h);
					sum += r;
					sum2 += r * r;
					off = (std::max)(off, std::fabs(h));
				}
				const double cnt = static_cast<double>(loop.size());
				const double mean = sum / cnt;
				const double dev = std::sqrt((std::max)(0.0, sum2 / cnt - mean * mean));
				if (mean < kMinOpening * md.k || mean > kMaxOpening * md.k) continue;
				if (off > kLoopPlanarity * mean || dev > kLoopCircularity * mean) continue;
				Opening o;
				o.c = c;
				o.n = n;
				o.r = mean;
				o.inner = mean;
				o.ring = true;
				o.mesh = mi;
				out.push_back(o);
			}
		}

		// ---- Lines -----------------------------------------------------------------

		struct Line
		{
			D3 p;
			D3 d;
			std::vector<uint32_t> members;   // indices into the openings
			double r = 0.0;
		};

		double lateral(D3 q, D3 p, D3 d)
		{
			const D3 w = q - p;
			return length(w - d * dot(w, d));
		}

		std::vector<Line> cluster(const Model& md, const std::vector<Opening>& ops)
		{
			std::vector<uint32_t> order(ops.size());
			for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
			std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return ops[a].r > ops[b].r; });
			const double par = cos_deg(kParallelDegrees);
			std::vector<Line> lines;
			for (const uint32_t i : order)
			{
				const Opening& o = ops[i];
				const double tol = (std::max)(kLateralTolerance * md.k, kLateralFraction * o.r);
				bool placed = false;
				for (Line& l : lines)
				{
					if (std::fabs(dot(l.d, o.n)) < par || lateral(o.c, l.p, l.d) > tol) continue;
					l.members.push_back(i);
					placed = true;
					break;
				}
				if (!placed)
				{
					Line l;
					l.p = o.c;
					l.d = o.n;
					l.r = o.r;
					l.members.push_back(i);
					lines.push_back(std::move(l));
				}
			}
			// The line through the members: their mean normal, their mean center.
			for (Line& l : lines)
			{
				D3 n, c;
				for (const uint32_t i : l.members)
				{
					const Opening& o = ops[i];
					n = n + o.n * ((dot(o.n, l.d) < 0.0 ? -1.0 : 1.0) * o.r * o.r);
					c = c + o.c;
				}
				l.d = snap(unit(n));
				l.p = c * (1.0 / static_cast<double>(l.members.size()));
			}
			std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
				return a.members.size() != b.members.size() ? a.members.size() > b.members.size() : a.r > b.r;
			});
			if (lines.size() > kMaxCandidates) lines.resize(kMaxCandidates);
			return lines;
		}

		// ---- Rays and sections -----------------------------------------------------

		struct Hit
		{
			double t = 0.0;
			uint32_t mesh = 0;
			uint32_t tri = 0;
		};

		std::vector<Hit> line_hits(const Model& md, D3 p, D3 d)
		{
			std::vector<Hit> out;
			for (uint32_t mi = 0; mi < md.m.size(); mi++)
			{
				const Mesh& m = md.m[mi];
				for (size_t t = 0; t < m.tris(); t++)
				{
					if (m.area[t] <= 0.0) continue;
					const D3 a = m.v(t, 0);
					const D3 e1 = m.v(t, 1) - a, e2 = m.v(t, 2) - a;
					const D3 h = cross(d, e2);
					const double det = dot(e1, h);
					if (std::fabs(det) < 1e-14) continue;
					const double f = 1.0 / det;
					const D3 s = p - a;
					const double u = f * dot(s, h);
					if (u < -1e-7 || u > 1.0 + 1e-7) continue;
					const D3 q = cross(s, e1);
					const double v = f * dot(d, q);
					if (v < -1e-7 || u + v > 1.0 + 1e-7) continue;
					out.push_back({ f * dot(e2, q), mi, static_cast<uint32_t>(t) });
				}
			}
			std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) { return a.t < b.t; });
			// A shared edge is crossed once.
			std::vector<Hit> merged;
			for (const Hit& h : out)
				if (merged.empty() || merged.back().mesh != h.mesh || std::fabs(merged.back().t - h.t) > 1e-6 * md.k)
					merged.push_back(h);
			return merged;
		}

		// Triangles left out of a profile: one mesh's, sorted.
		struct Exclude
		{
			int mesh = -1;
			std::vector<uint32_t> tris;
			bool has(uint32_t mi, uint32_t t) const
			{
				return static_cast<int>(mi) == mesh && std::binary_search(tris.begin(), tris.end(), t);
			}
		};

		// Walls crossing each plane t (sorted) across the line: the nearest
		// one's distance (the clear aperture) and the sectors (of kSectors
		// around the axis) that have a wall within kTubeMaxRadius.
		void profile(const Model& md, D3 p, D3 d, D3 u, D3 v, const std::vector<double>& planes, const Exclude* ex,
			std::vector<double>& aperture, std::vector<uint8_t>& sectors)
		{
			aperture.assign(planes.size(), 1e30);
			sectors.assign(planes.size(), 0);
			if (planes.empty()) return;
			const double reach = kTubeMaxRadius * md.k;
			for (uint32_t mi = 0; mi < md.m.size(); mi++)
			{
				const Mesh& m = md.m[mi];
				for (size_t t = 0; t < m.tris(); t++)
				{
					if (m.area[t] <= 0.0 || std::fabs(dot(m.n[t], d)) >= kWallCos) continue;
					if (ex && ex->has(mi, static_cast<uint32_t>(t))) continue;
					const D3 q[3] = { m.v(t, 0) - p, m.v(t, 1) - p, m.v(t, 2) - p };
					const double tt[3] = { dot(q[0], d), dot(q[1], d), dot(q[2], d) };
					const double lo = (std::min)({ tt[0], tt[1], tt[2] }), hi = (std::max)({ tt[0], tt[1], tt[2] });
					auto it = std::lower_bound(planes.begin(), planes.end(), lo);
					for (; it != planes.end() && *it <= hi; ++it)
					{
						const double at = *it;
						D3 pts[2];
						int np = 0;
						for (int e = 0; e < 3 && np < 2; e++)
						{
							const int f = (e + 1) % 3;
							if ((tt[e] - at) * (tt[f] - at) > 0.0 || tt[e] == tt[f]) continue;
							const double s = (at - tt[e]) / (tt[f] - tt[e]);
							pts[np++] = q[e] + (q[f] - q[e]) * s;
						}
						if (np < 2) continue;
						// Lateral offsets from the axis point on this plane.
						const D3 a = pts[0] - d * dot(pts[0], d), b = pts[1] - d * dot(pts[1], d);
						const D3 e = b - a;
						const double ee = dot(e, e);
						const double s = ee > 0.0 ? std::clamp(-dot(a, e) / ee, 0.0, 1.0) : 0.0;
						const D3 c = a + e * s;
						const size_t i = static_cast<size_t>(it - planes.begin());
						aperture[i] = (std::min)(aperture[i], length(c));
						for (int k = 0; k <= 5; k++)
						{
							const D3 x = k == 5 ? c : a + e * (k * 0.25);
							if (length(x) > reach) continue;
							const double ang = std::atan2(dot(x, v), dot(x, u));
							int sec = static_cast<int>(std::floor((ang + kPi) / (2.0 * kPi) * kSectors));
							sec = std::clamp(sec, 0, kSectors - 1);
							sectors[i] = static_cast<uint8_t>(sectors[i] | (1u << sec));
						}
					}
				}
			}
		}

		int bits(uint8_t b)
		{
			int n = 0;
			for (; b; b = static_cast<uint8_t>(b & (b - 1))) n++;
			return n;
		}

		std::vector<double> planes_between(double a, double b, double step)
		{
			std::vector<double> out;
			if (b < a || step <= 0.0) return out;
			for (double t = a; t <= b + 1e-9; t += step) out.push_back(t);
			return out;
		}

		double min_aperture(const Model& md, D3 p, D3 d, D3 u, D3 v, double a, double b, const Exclude* ex)
		{
			std::vector<double> ap;
			std::vector<uint8_t> sec;
			profile(md, p, d, u, v, planes_between(a, b, kPlaneStep * md.k), ex, ap, sec);
			double best = 1e30;
			for (const double x : ap) best = (std::min)(best, x);
			return best;
		}

		// ---- One candidate axis ----------------------------------------------------

		struct EndInfo
		{
			End src = End::None;
			double t = 0.0;              // the end's place along the axis
			double cu = 0.0, cv = 0.0;   // its center across the axis
			double r = 0.0;
			double eu = 0.0, ev = 0.0;   // extents along up and across
			bool round = false;
			bool rect = false;
			bool blended = false;
			uint32_t mesh = 0;
			double front = 0.0;          // objective: the scope's front end
		};

		struct Eval
		{
			bool usable = false;
			bool barrel = false;
			std::string why;
			D3 p, d, u, v;               // the line (d from the eyepiece toward the objective), up, right
			int hits = 0;
			EndInfo eye, obj;
			double span = 0.0;
			double distance = 0.0;       // eyepiece to objective point
			double tube = 0.0;
			double score = 0.0;
			bool sighting = false;
			bool parallel = false;
			double standoff = 0.0;       // the line's distance from the projectile node
			D3 eyepiece;
		};

		D3 principal_toward(D3 w, D3 d)
		{
			D3 best;
			double hi = -2.0;
			for (const D3& e : kAxes)
				for (const double s : { 1.0, -1.0 })
				{
					const D3 a = e * s;
					if (std::fabs(dot(a, d)) > 0.7) continue;
					const double c = dot(a, w);
					if (c > hi) { hi = c; best = a; }
				}
			return best;
		}

		// Up: the root axis nearest the direction from the bore line to the
		// scope's line, else nearest the view's up; square to the axis.
		D3 up_for(D3 p, D3 d, const Hints& h)
		{
			D3 w;
			if (h.have_projectile)
			{
				const D3 q = p - d3(h.projectile);
				w = q - d * dot(q, d);
			}
			if (length(w) < 1e-6) w = d3(h.view_up);
			D3 up = principal_toward(unit(w), d);
			if (!axis_aligned(d)) up = unit(up - d * dot(up, d));
			return up;
		}

		double member_t(const Opening& o, D3 p, D3 d) { return dot(o.c - p, d); }

		// A ring's clear radius; a ring around a pinhole (a glass with a hole at
		// its apex) is closed glass.
		double member_radius(const Opening& o) { return o.ring && o.inner >= kRimFloor * o.r ? o.inner : o.r; }

		// The eyepiece (front = false) or the objective glass around a hit.
		EndInfo surface_end(const Model& md, const Eval& e, const Hit& hit, bool front)
		{
			const Mesh& m = md.m[hit.mesh];
			EndInfo out;
			out.src = End::Surface;
			out.mesh = hit.mesh;
			out.blended = m.blended;
			std::vector<uint32_t> glass = glass_patch(m, hit.tri, e.d);
			Stats s = stats(m, glass, e.p, e.d, e.u, e.v);
			if (s.r < kMinEyeRadius * md.k)
			{
				// A patch that stops almost at once: a dome tessellated coarser
				// than the patch bends follow. Grown again with the dome's limits.
				std::vector<uint32_t> wide = glass_patch(m, hit.tri, e.d, kDomeTiltDegrees, kDomeBendDegrees);
				const Stats ws = stats(m, wide, e.p, e.d, e.u, e.v);
				if (ws.round && ws.r > s.r)
				{
					glass = std::move(wide);
					s = ws;
				}
			}
			std::vector<uint32_t> pc = piece(m, hit.tri);
			std::sort(pc.begin(), pc.end());
			const Stats ps = stats(m, pc, e.p, e.d, e.u, e.v);
			out.t = front ? s.tmean : s.tmin;
			out.r = s.r;
			out.eu = s.eu;
			out.ev = s.ev;
			out.round = s.round;
			out.rect = s.rect;
			if (s.round || s.rect)
			{
				out.cu = s.cu;
				out.cv = s.cv;
			}
			Exclude ex;
			ex.mesh = static_cast<int>(hit.mesh);
			ex.tris = pc;
			const bool cap = ps.tmax - ps.tmin <= kCapDepth * md.k && ps.round;
			if (((!front && s.max_tilt > kCurvedDegrees) || s.r < kMinEyeRadius * md.k) && cap && ps.r > s.r)
			{
				// A domed eyepiece, or a curved lens whose bends stop the patch
				// almost at once: the edge of its piece. (A front glass keeps
				// its patch: its piece is often the glass and its bezel.)
				out.r = ps.r;
				out.cu = ps.cu;
				out.cv = ps.cv;
				out.round = true;
				out.rect = false;
			}
			else if (!s.round && !s.rect)
			{
				// No usable patch: the tube's clear aperture just inside.
				const double a = front ? min_aperture(md, e.p, e.d, e.u, e.v, ps.tmin - kInsideDepth * md.k,
					ps.tmin - kEyeGap * md.k, &ex) : min_aperture(md, e.p, e.d, e.u, e.v, ps.tmax + kEyeGap * md.k,
					ps.tmax + kInsideDepth * md.k, &ex);
				if (a < kTubeMaxRadius * md.k)
				{
					out.r = a;
					out.src = End::Aperture;
					out.cu = out.cv = 0.0;
				}
			}
			if (!front)
			{
				// A narrower rim or wall right behind the glass bounds it.
				const double a = min_aperture(md, e.p, e.d, e.u, e.v, out.t - kRimDepth * md.k, out.t - 0.02 * md.k, &ex);
				if (a < out.r && a >= kRimFloor * out.r) out.r = a;
			}
			out.front = s.tmax;
			return out;
		}

		EndInfo rim_end(const Opening& o, const Eval& e)
		{
			EndInfo out;
			out.src = End::Rim;
			out.t = member_t(o, e.p, e.d);
			const D3 q = o.c - e.p;
			out.cu = dot(q, e.u);
			out.cv = dot(q, e.v);
			out.r = member_radius(o);
			out.eu = out.ev = out.r * 2.0;
			out.round = true;
			out.mesh = o.mesh;
			out.front = out.t;
			return out;
		}

		const char* end_words(const EndInfo& x, bool eyepiece);

		void evaluate(const Model& md, const Hints& h, const Line& line, const std::vector<Opening>& ops, Eval& e)
		{
			e.p = line.p;
			e.d = line.d;
			std::vector<Hit> hits = line_hits(md, e.p, e.d);
			std::vector<std::pair<double, uint32_t>> mem;
			for (const uint32_t i : line.members) mem.emplace_back(member_t(ops[i], e.p, e.d), i);
			e.hits = static_cast<int>(hits.size());
			double t0 = 1e30, t1 = -1e30;
			for (const Hit& x : hits) { t0 = (std::min)(t0, x.t); t1 = (std::max)(t1, x.t); }
			for (const auto& mm : mem) { t0 = (std::min)(t0, mm.first); t1 = (std::max)(t1, mm.first); }
			if (hits.size() + mem.size() < 2 || t1 - t0 < 0.5 * md.k)
			{
				e.why = "one surface only";
				return;
			}
			if (t1 - t0 < kMinSpan * md.k)
			{
				e.why = std::format("span {:.2f} under the minimum", (t1 - t0) / md.k);
				return;
			}
			if (h.have_projectile && lateral(d3(h.projectile), e.p, e.d) < kBoreDistance * md.k)
			{
				e.barrel = true;
				e.why = "on the bore line (the barrel)";
				return;
			}

			// The eyepiece is the end away from the muzzle (else nearer the eye).
			bool flip = false;
			if (h.have_projectile)
			{
				const double tp = dot(d3(h.projectile) - e.p, e.d);
				flip = std::fabs(t1 - tp) > std::fabs(t0 - tp);
			}
			else if (h.have_eye)
			{
				const double te = dot(d3(h.eye) - e.p, e.d);
				flip = std::fabs(t1 - te) < std::fabs(t0 - te);
			}
			else
			{
				e.why = "no projectile node or eye: the eyepiece end is unknown";
				return;
			}
			if (flip)
			{
				e.d = -e.d;
				for (Hit& x : hits) x.t = -x.t;
				std::reverse(hits.begin(), hits.end());
				for (auto& m : mem) m.first = -m.first;
				std::swap(t0, t1);
				t0 = -t0;
				t1 = -t1;
			}
			std::sort(mem.begin(), mem.end());
			e.u = up_for(e.p, e.d, h);
			e.v = cross(e.d, e.u);

			// Sections along the line: the tube's walls.
			const double step = kSectionStep * md.k;
			const int n = std::clamp(static_cast<int>(std::ceil((t1 - t0) / step)) + 1, kMinSections, kMaxSections);
			const double st = (t1 - t0) / static_cast<double>(n - 1);
			std::vector<double> planes(static_cast<size_t>(n));
			for (int i = 0; i < n; i++) planes[static_cast<size_t>(i)] = t0 + st * i;
			std::vector<double> ap;
			std::vector<uint8_t> sec;
			profile(md, e.p, e.d, e.u, e.v, planes, nullptr, ap, sec);
			int best0 = -1, best1 = -2;
			for (int i = 0; i < n;)
			{
				if (bits(sec[static_cast<size_t>(i)]) < kTubeSectors) { i++; continue; }
				// A run of walled sections bridges short gaps (a ring, a seam).
				int j = i;
				for (int k = i + 1; k < n; k++)
				{
					if (bits(sec[static_cast<size_t>(k)]) >= kTubeSectors) j = k;
					else if ((k - j) * st > kRunGap * md.k) break;
				}
				if (j - i > best1 - best0) { best0 = i; best1 = j; }
				i = j + 1;
			}
			double w0 = t0 - kEndMargin * md.k, w1 = t1 + kEndMargin * md.k;
			if (best0 >= 0 && (best1 - best0) * st >= kMinSpan * md.k)
			{
				w0 = planes[static_cast<size_t>(best0)] - kEndMargin * md.k;
				w1 = planes[static_cast<size_t>(best1)] + kEndMargin * md.k;
			}

			// The eyepiece: the rear-most surface in the tube, unless a rim is
			// further back than a recess.
			// Rays just off the axis too, so a glass with a hole at its apex
			// still counts as a surface.
			std::vector<Hit> end_hits = hits;
			for (int k = 0; k < 4; k++)
			{
				const D3 off = (k < 2 ? e.u : e.v) * ((k & 1 ? -kProbeOffset : kProbeOffset) * md.k);
				for (const Hit& x : line_hits(md, e.p + off, e.d))
					end_hits.push_back(x);
			}
			std::sort(end_hits.begin(), end_hits.end(), [](const Hit& x0, const Hit& x1) { return x0.t < x1.t; });
			const Hit* rear_hit = nullptr;
			const Hit* front_hit = nullptr;
			for (const Hit& x : end_hits)
				if (x.t >= w0 && x.t <= w1)
				{
					if (!rear_hit) rear_hit = &x;
					front_hit = &x;
				}
			const Opening* rear_mem = nullptr;
			const Opening* front_mem = nullptr;
			double rear_mem_t = 0.0, front_mem_t = 0.0;
			for (const auto& [t, i] : mem)
				if (t >= w0 && t <= w1)
				{
					if (!rear_mem) { rear_mem = &ops[i]; rear_mem_t = t; }
					front_mem = &ops[i];
					front_mem_t = t;
				}
			if (rear_hit && (!rear_mem || rear_hit->t - rear_mem_t <= kRecessMax * md.k))
				e.eye = surface_end(md, e, *rear_hit, false);
			else if (rear_mem)
				e.eye = rim_end(*rear_mem, e);
			else
			{
				e.why = "no eyepiece surface or rim";
				return;
			}
			// Rims behind the eyepiece glass bound it.
			if (e.eye.src != End::Rim)
				for (const auto& [t, i] : mem)
					if (t >= w0 && t < e.eye.t)
					{
						const double r = member_radius(ops[i]);
						if (r < e.eye.r && r >= kRimFloor * e.eye.r) e.eye.r = r;
					}

			// The objective: the front-most surface, unless a rim is further ahead
			// than a recess.
			const double min_obj = e.eye.t + kMinSpan * md.k;
			if (front_hit && front_hit->t < min_obj) front_hit = nullptr;
			if (front_mem && front_mem_t < min_obj) front_mem = nullptr;
			if (front_hit && (!front_mem || front_mem_t - front_hit->t <= kRecessMax * md.k))
				e.obj = surface_end(md, e, *front_hit, true);
			else if (front_mem)
				e.obj = rim_end(*front_mem, e);
			else
			{
				e.why = "no second surface on the axis";
				return;
			}

			// The scope's front end: the objective piece's vertices near the axis
			// (the hood), its shape's vertices just ahead, and rims just ahead.
			{
				const Mesh& m = md.m[e.obj.mesh];
				double fe = (std::max)(e.obj.front, e.obj.t);
				const double lim = 2.0 * (std::max)(e.obj.r, 0.5 * md.k);
				if (e.obj.src != End::Rim && front_hit)
				{
					const std::vector<uint32_t> pc = piece(m, front_hit->tri);
					for (const uint32_t t : pc)
						for (int c = 0; c < 3; c++)
						{
							const D3 x = m.v(t, c);
							const double tx = dot(x - e.p, e.d);
							if (tx >= e.obj.t - 0.5 * md.k && tx <= e.obj.t + kFrontReach * md.k &&
								lateral(x, e.p, e.d) <= lim)
								fe = (std::max)(fe, tx);
						}
				}
				for (const D3& x : m.p)
				{
					const double tx = dot(x - e.p, e.d);
					if (tx >= e.obj.t && tx <= e.obj.t + kFrontSlab * md.k && lateral(x, e.p, e.d) <= lim)
						fe = (std::max)(fe, tx);
				}
				for (const auto& mm : mem)
					if (mm.first >= fe && mm.first <= fe + kRecessMax * md.k) fe = mm.first;
				e.obj.front = fe;
			}
			double obj_t = e.obj.front + kFrontMargin * md.k;
			const double planned = obj_t;
			for (int pass = 0; pass < 8; pass++)
			{
				const Hit* block = nullptr;
				for (const Hit& x : hits)
					if (x.t > obj_t && x.t <= obj_t + kObjectiveClear * md.k) { block = &x; break; }
				if (!block) break;
				if (block->t + kFrontMargin * md.k - planned > kMaxPush * md.k)
				{
					e.why = std::format("geometry {:.2f} ahead of the objective point", (block->t - obj_t) / md.k);
					return;
				}
				obj_t = block->t + kFrontMargin * md.k;
			}
			e.distance = obj_t - e.eye.t;

			// Hard checks.
			const bool rect = e.eye.rect && !e.eye.round;
			const double eye_r = rect ? e.eye.eu * 0.5 : e.eye.r;
			if (eye_r < kMinEyeRadius * md.k || eye_r > kMaxEyeRadius * md.k)
			{
				e.why = std::format("eyepiece radius {:.2f} out of range ({} at {:.2f} along the axis)", eye_r / md.k,
					end_words(e.eye, true), e.eye.t / md.k);
				return;
			}
			const double ratio = e.obj.r / (std::max)(eye_r, 1e-9);
			if (ratio < kMinRatio || ratio > kMaxRatio)
			{
				e.why = std::format("objective / eyepiece radius {:.2f} out of range", ratio);
				return;
			}
			if (e.distance < kMinSpan * md.k)
			{
				e.why = std::format("span {:.2f} under the minimum", e.distance / md.k);
				return;
			}
			for (const Hit& x : hits)
				if (x.t >= e.eye.t - kEyeRelief * md.k && x.t <= e.eye.t - kEyeGap * md.k)
				{
					e.why = "eyepiece hidden from the eye point";
					return;
				}

			// Tube evidence over the scope's own span.
			int in = 0, all = 0;
			for (int i = 0; i < n; i++)
			{
				const double t = planes[static_cast<size_t>(i)];
				if (t < e.eye.t || t > e.obj.front) continue;
				all++;
				if (bits(sec[static_cast<size_t>(i)]) >= kTubeSectors) in++;
			}
			e.tube = all ? static_cast<double>(in) / all : 0.0;
			e.span = e.obj.front - e.eye.t;
			if (h.have_sighting)
			{
				const D3 s = d3(h.sighting);
				const double ts = dot(s - e.p, e.d);
				e.sighting = lateral(s, e.p, e.d) <= kSightingLateral * md.k && ts < e.eye.t &&
					e.eye.t - ts <= kSightingBehind * md.k;
			}
			const bool eye_clean = e.eye.src != End::Aperture && (e.eye.round || e.eye.rect);
			const bool obj_clean = e.obj.src != End::Aperture && e.obj.round;
			e.score = kScoreTube * e.tube + (eye_clean ? kScoreEye : 0.0) + (obj_clean ? kScoreObjective : 0.0) +
				(e.eye.blended || e.obj.blended ? kScoreGlass : 0.0) + (e.sighting ? kScoreSighting : 0.0);
			e.eyepiece = e.p + e.d * e.eye.t + e.u * e.eye.cu + e.v * e.eye.cv;
			if (h.have_projectile) e.standoff = lateral(d3(h.projectile), e.eyepiece, e.d);
			e.usable = true;
		}

		const char* end_words(const EndInfo& x, bool eyepiece)
		{
			switch (x.src)
			{
			case End::Surface:
				return x.round ? (x.blended ? "glass, round" : "surface, round")
					: x.rect ? (eyepiece ? "window, rectangular" : "surface, rectangular") : "surface, not round";
			case End::Rim: return "rim";
			case End::Aperture: return "clear aperture only";
			default: return "none";
			}
		}

		void to_candidate(const Model& md, const Eval& e, Candidate& c)
		{
			put(c.point, e.usable ? e.eyepiece : e.p);
			put(c.axis, e.d);
			c.span = static_cast<float>(e.span);
			c.tube = static_cast<float>(e.tube);
			c.score = static_cast<float>(e.score);
			c.hits = e.hits;
			c.usable = e.usable;
			c.why = e.usable ? std::format("eyepiece {} r {:.2f}, objective {} r {:.2f}, tube {:.2f}{}", end_words(e.eye, true),
				e.eye.r / md.k, end_words(e.obj, false), e.obj.r / md.k, e.tube, e.sighting ? ", sighting node behind" : "")
				: e.why;
		}
	}

	Result fit(const Soup& soup)
	{
		Result r;
		Model md;
		std::string why;
		if (!prepare(soup, md, why))
		{
			r.why = why;
			return r;
		}
		std::vector<Opening> ops;
		for (uint32_t mi = 0; mi < md.m.size(); mi++)
		{
			patch_openings(md, mi, ops);
			loop_openings(md, mi, ops);
		}
		if (ops.empty())
		{
			r.why = "no round openings in the mesh";
			return r;
		}
		const std::vector<Line> lines = cluster(md, ops);
		std::vector<Eval> evals(lines.size());
		for (size_t i = 0; i < lines.size(); i++)
		{
			evaluate(md, soup.hints, lines[i], ops, evals[i]);
		}
		// The barrel (the bore line with the most openings): a scope runs
		// along it, so an axis across it is not one.
		const Eval* barrel = nullptr;
		size_t barrel_members = 0;
		for (size_t i = 0; i < evals.size(); i++)
			if (evals[i].barrel && lines[i].members.size() > barrel_members)
			{
				barrel = &evals[i];
				barrel_members = lines[i].members.size();
			}
		if (barrel)
			for (Eval& e : evals)
			{
				if (!e.usable) continue;
				const double off = degrees_between(barrel->d * (dot(barrel->d, e.d) < 0.0 ? -1.0 : 1.0), e.d);
				if (off > kBoreMaxDegrees)
				{
					e.usable = false;
					e.why = std::format("{:.0f} degrees across the barrel", off);
				}
				else if (off <= kBoreParallelDegrees)
				{
					e.parallel = true;
					e.score += kScoreParallel;
				}
			}

		// A scope stands off the barrel; tubes near the bore (a cylinder, a gas
		// or magazine tube) do not.
		double max_standoff = 0.0;
		for (const Eval& e : evals)
			if (e.usable) max_standoff = (std::max)(max_standoff, e.standoff);
		if (max_standoff > 0.0)
			for (Eval& e : evals)
				if (e.usable) e.score += kScoreStandoff * e.standoff / max_standoff;

		std::vector<size_t> order;
		for (size_t i = 0; i < evals.size(); i++) order.push_back(i);
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
			if (evals[a].usable != evals[b].usable) return evals[a].usable;
			return evals[a].score > evals[b].score;
		});
		// Lines that measured the same scope are one candidate.
		std::vector<size_t> kept;
		for (const size_t i : order)
		{
			const Eval& e = evals[i];
			bool same = false;
			if (e.usable)
				for (const size_t j : kept)
					same = same || (length(evals[j].eyepiece - e.eyepiece) <= kSameScope * md.k &&
						degrees_between(evals[j].d, e.d) <= kParallelDegrees);
			if (same) continue;
			kept.push_back(i);
			Candidate c;
			to_candidate(md, e, c);
			c.members = static_cast<int>(lines[i].members.size());
			r.candidates.push_back(std::move(c));
		}
		const Eval* best = nullptr;
		const Eval* second = nullptr;
		int usable = 0;
		for (const size_t i : kept)
		{
			if (!evals[i].usable) continue;
			usable++;
			if (!best) best = &evals[i];
			else if (!second) second = &evals[i];
		}
		if (!best)
		{
			bool span = false, hidden = false, blocked = false;
			for (const Eval& e : evals)
			{
				span = span || e.why == "one surface only" || e.why == "no second surface on the axis" ||
					e.why.starts_with("span");
				hidden = hidden || e.why == "eyepiece hidden from the eye point";
				blocked = blocked || e.why.starts_with("geometry");
			}
			r.why = hidden ? "eyepiece hidden from the eye point on every axis" : blocked
				? "geometry ahead of the objective on every axis" : span ? "no second surface on any axis"
				: "no axis passed the checks";
			r.why += std::format(" ({} axes)", evals.size());
			return r;
		}

		const Eval& e = *best;
		const double margin = second ? e.score - second->score : 1.0;
		if (second && margin < kMarginMin)
		{
			r.why = std::format("two candidate scopes too close to call (scores {:.2f} and {:.2f})", e.score, second->score);
			return r;
		}
		std::vector<std::string> failed;
		const bool eye_measured = (e.eye.src == End::Surface || e.eye.src == End::Rim) && (e.eye.round || e.eye.rect);
		const bool obj_measured = e.obj.src == End::Surface || e.obj.src == End::Rim;
		if (!eye_measured) failed.push_back(e.eye.src == End::Aperture ? "eyepiece from the clear aperture only"
			: "eyepiece not round");
		if (!obj_measured) failed.push_back("objective from the clear aperture only");
		if (e.tube < kTubeHigh) failed.push_back(std::format("tube walls along {:.0f}% of the span", e.tube * 100.0));
		const bool weak_margin = second && margin < kMarginHigh;
		if (weak_margin) failed.push_back(std::format("margin {:.2f} over the runner-up", margin));

		if (failed.empty()) r.level = 2;
		else if (failed.size() == 1 && (!weak_margin || e.eye.blended || e.obj.blended || e.sighting)) r.level = 1;
		r.found = r.level > 0;

		const bool rect = e.eye.rect && !e.eye.round;
		put(r.eyepiece, e.eyepiece);
		put(r.axis, e.d);
		put(r.up, e.u);
		r.aperture = rect ? 1 : 0;
		r.eyepiece_radius = static_cast<float>(rect ? e.eye.eu * 0.5 : e.eye.r);
		r.aperture_aspect = rect && e.eye.eu > 0.0 ? static_cast<float>(e.eye.ev / e.eye.eu) : 1.0f;
		r.objective_distance = static_cast<float>(e.distance);
		r.objective_radius = static_cast<float>(e.obj.r);
		r.eyepiece_end = e.eye.src;
		r.objective_end = e.obj.src;
		r.eyepiece_shape = md.m[e.eye.mesh].shape;
		r.confidence = static_cast<float>(std::clamp(e.score - 0.15 * static_cast<double>(failed.size()), 0.0, 1.0));

		std::string text = std::format("eyepiece {}, objective {}, tube {:.0f}%", end_words(e.eye, true),
			end_words(e.obj, false), e.tube * 100.0);
		if (e.sighting) text += ", sighting node behind";
		text += second ? std::format(", margin {:.2f} over {} other usable axes", margin, usable - 1)
			: std::string(", no other usable axis");
		for (const std::string& f : failed) text += "; " + f;
		if (!r.found) text += "; not confident";
		r.why = text;
		return r;
	}

	Agreement agrees(const Result& fitted, const Geometry& file, float unit_scale, const Tolerances& tol)
	{
		Agreement a;
		const double w = std::isfinite(unit_scale) && unit_scale > 1e-4f ? unit_scale : 1.0;
		const D3 d = unit(d3(fitted.axis));
		const D3 fd = unit(d3(file.axis));
		if (length(d) < 0.5 || length(fd) < 0.5)
		{
			a.why = "no axis";
			return a;
		}
		const D3 diff = d3(file.eyepiece) - d3(fitted.eyepiece);
		const double axial = dot(diff, d);
		a.axial = static_cast<float>(axial * w);
		a.lateral = static_cast<float>(length(diff - d * axial) * w);
		a.radius = static_cast<float>((file.eyepiece_radius - fitted.eyepiece_radius) * w);
		a.axis_degrees = static_cast<float>(degrees_between(fd, d));
		const D3 fu = d3(file.up), gu = d3(fitted.up);
		a.up_degrees = static_cast<float>(degrees_between(fu - d * dot(fu, d), gu - d * dot(gu, d)));
		const D3 file_end = d3(file.eyepiece) + fd * file.objective_distance;
		const D3 fit_end = d3(fitted.eyepiece) + d * fitted.objective_distance;
		a.objective = static_cast<float>(dot(file_end - fit_end, d) * w);
		a.objective_radius = static_cast<float>((file.objective_radius - fitted.objective_radius) * w);

		const double fit_r = fitted.eyepiece_radius * w;
		const double r_limit = (std::max)(static_cast<double>(tol.radius), tol.radius_fraction * fit_r);
		const double dist = std::hypot(static_cast<double>(a.lateral), static_cast<double>(a.axial));
		if (a.lateral > tol.lateral || std::fabs(a.axial) > tol.axial)
			a.why = std::format("the file's eyepiece is {:.2f} from the mesh's", dist);
		else if (std::fabs(a.radius) > r_limit)
			a.why = std::format("the file's eyepiece radius is {:.2f} against the mesh's {:.2f}", file.eyepiece_radius * w, fit_r);
		else if (a.axis_degrees > tol.axis_degrees)
			a.why = std::format("the file's axis is {:.1f} degrees off the mesh's", a.axis_degrees);
		else if (a.up_degrees > tol.up_degrees)
			a.why = std::format("the file's up is {:.0f} degrees off the mesh's", a.up_degrees);
		else if (a.objective < -tol.objective_behind)
			a.why = std::format("the file's objective is {:.2f} behind the mesh's front end", -a.objective);
		else if (a.objective > tol.objective_ahead)
			a.why = std::format("the file's objective is {:.2f} past the mesh's front end", a.objective);
		else if (std::fabs(a.objective_radius) > tol.objective_radius_fraction * fitted.objective_radius * w)
			a.why = std::format("the file's objective radius is {:.2f} against the mesh's {:.2f}", file.objective_radius * w,
				fitted.objective_radius * w);
		a.ok = a.why.empty();
		return a;
	}

	Check mesh_check(const Soup& soup, const Geometry& file)
	{
		Check c;
		Model md;
		std::string why;
		if (!prepare(soup, md, why))
		{
			c.why = why;
			return c;
		}
		const D3 d = unit(d3(file.axis));
		if (length(d) < 0.5 || !(file.eyepiece_radius > 0.0f))
		{
			c.why = "the file's axis or eyepiece radius is not usable";
			return c;
		}
		const D3 p = d3(file.eyepiece);
		const double r = file.eyepiece_radius;
		const std::vector<Hit> hits = line_hits(md, p, d);
		for (const Hit& x : hits)
			// The eyepiece's own glass may reach a little behind the file's point.
			if (x.t >= -kEyeRelief * md.k && x.t <= -kCheckDepth * md.k)
			{
				c.why = "something lies between the eye point and the eyepiece";
				return c;
			}
		const D3 u = up_for(p, d, soup.hints), v = cross(d, u);
		bool opening = false;
		for (const Hit& x : hits)
		{
			if (std::fabs(x.t) > kCheckDepth * md.k) continue;
			const Mesh& m = md.m[x.mesh];
			const Stats s = stats(m, glass_patch(m, x.tri, d), p, d, u, v);
			const double gr = s.r;
			if (s.round && gr >= kCheckRadiusLow * r && gr <= kCheckRadiusHigh * r) opening = true;
		}
		if (!opening)
		{
			std::vector<double> ap;
			std::vector<uint8_t> sec;
			profile(md, p, d, u, v, { kEyeGap * md.k, 0.25 * md.k, kCheckDepth * md.k }, nullptr, ap, sec);
			for (size_t i = 0; i < ap.size() && !opening; i++)
				opening = bits(sec[i]) >= kTubeSectors && ap[i] >= kCheckRadiusLow * r && ap[i] <= kCheckRadiusHigh * r;
		}
		if (!opening)
		{
			c.why = "no walls or glass around the eyepiece at its radius";
			return c;
		}
		const double obj = file.objective_distance;
		for (const Hit& x : hits)
			if (x.t > obj && x.t <= obj + kObjectiveClear * md.k)
			{
				c.why = std::format("geometry {:.2f} ahead of the objective point", (x.t - obj) / md.k);
				return c;
			}
		c.ok = true;
		return c;
	}
}
