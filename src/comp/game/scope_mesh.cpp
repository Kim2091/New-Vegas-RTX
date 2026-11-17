#include "std_include.hpp"
#include "scope_mesh.hpp"

#include "api_common.hpp"
#include "scene_graph.hpp"

namespace comp::game::scopes::mesh
{
	namespace
	{
		namespace sg = scene_graph;

		constexpr uint32_t OBJ_NAME_OFF = 0x08;
		constexpr size_t kMaxNodeVisits = 4096;
		constexpr uint32_t kMaxChildren = 64;
		constexpr uint32_t kMaxStripIndices = 3u * 65536u;
		constexpr uint16_t kAlphaBlend = 0x0001;       // NiAlphaProperty flags bit 0
		constexpr const char* kProjectileNode = "ProjectileNode";
		constexpr const char* kSightingNode = "##SightingNode";

		struct Pose
		{
			float rot[9];
			float pos[3];
			float scale;
		};

		bool world_pose(uintptr_t obj, Pose& out)
		{
			float p[sg::kPoseFloats];
			if (!sg::read_pose(obj, p)) return false;
			for (const float f : p)
				if (!std::isfinite(f)) return false;
			std::memcpy(out.rot, p, sizeof(out.rot));
			std::memcpy(out.pos, p + 9, sizeof(out.pos));
			out.scale = p[12];
			return out.scale > 1e-6f;
		}

		// A world point in the root's space: rot^T * (w - pos) / scale.
		void to_root(const Pose& root, const double (&w)[3], float* out)
		{
			const double d[3] = { w[0] - root.pos[0], w[1] - root.pos[1], w[2] - root.pos[2] };
			for (int i = 0; i < 3; i++)
				out[i] = static_cast<float>((root.rot[i] * d[0] + root.rot[3 + i] * d[1] + root.rot[6 + i] * d[2]) /
					root.scale);
		}

		void relative(const Pose& root, const Pose& obj, RootPose& out)
		{
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					out.rot[i * 3 + j] = root.rot[i] * obj.rot[j] + root.rot[3 + i] * obj.rot[3 + j] +
						root.rot[6 + i] * obj.rot[6 + j];
			const double w[3] = { obj.pos[0], obj.pos[1], obj.pos[2] };
			to_root(root, w, out.pos);
			out.scale = obj.scale / root.scale;
		}

		struct Visit
		{
			uintptr_t obj = 0;
			int parent = -1;
			char name[64] = {};
		};

		// Ends a begin_resolve that succeeded.
		struct Resolve
		{
			bool ok;
			explicit Resolve(uintptr_t geo) : ok(sg::begin_resolve(geo)) {}
			~Resolve() { if (ok) sg::end_resolve(); }
			Resolve(const Resolve&) = delete;
			Resolve& operator=(const Resolve&) = delete;
		};

		void emit(std::vector<uint32_t>& tris, uint32_t n, uint32_t a, uint32_t b, uint32_t c)
		{
			if (a == b || b == c || a == c || a >= n || b >= n || c >= n) return;
			tris.push_back(a);
			tris.push_back(b);
			tris.push_back(c);
		}

		// Odd triangles of a strip are wound the other way round.
		void emit_strip(std::vector<uint32_t>& tris, uint32_t n, const uint32_t* idx, size_t count)
		{
			for (size_t j = 0; j + 2 < count; j++)
			{
				if (j & 1) emit(tris, n, idx[j + 1], idx[j], idx[j + 2]);
				else emit(tris, n, idx[j], idx[j + 1], idx[j + 2]);
			}
		}

		// Positions (object space) and triangles from the CPU arrays of
		// geometry the engine has not packed: NiTriShapeData's triangle list
		// or NiTriStripsData's strips (scene_statics' cpu_mesh_data).
		bool cpu_arrays(const sg::DataRead& d, bool strips, std::vector<float>& pos, std::vector<uint32_t>& tris)
		{
			const uint32_t n = d.verts;
			if (!n || !d.pos) return false;
			pos.resize(n * 3u);
			if (!sg::read_block(d.pos, pos.data(), n * 12u)) return false;
			tris.clear();
			std::vector<uint16_t> idx;
			if (!strips)
			{
				const uint32_t count = static_cast<uint32_t>(d.tris) * 3u;
				if (!count || !d.f48) return false;
				idx.resize(count);
				if (!sg::read_block(d.f48, idx.data(), count * 2u)) return false;
				for (uint32_t i = 0; i + 2 < count; i += 3) emit(tris, n, idx[i], idx[i + 1], idx[i + 2]);
			}
			else
			{
				const uint32_t ns = d.f44 & 0xFFFFu;
				if (!ns || !d.f48 || !d.f4c) return false;
				std::vector<uint16_t> lens(ns);
				if (!sg::read_block(d.f48, lens.data(), ns * 2u)) return false;
				uint32_t total = 0;
				for (const uint16_t l : lens) total += l;
				if (!total || total > kMaxStripIndices) return false;
				idx.resize(total);
				if (!sg::read_block(d.f4c, idx.data(), total * 2u)) return false;
				std::vector<uint32_t> wide(idx.begin(), idx.end());
				uint32_t off = 0;
				for (const uint16_t l : lens)
				{
					emit_strip(tris, n, wide.data() + off, l);
					off += l;
				}
			}
			return !tris.empty();
		}

		// Positions and triangles from the packed buffers the engine's draws
		// bind (scene_statics' seed path): POSITION on stream 0, a list or a
		// strip, static buffers only (the copies refuse dynamic ones).
		bool packed_buffers(const sg::DataRead& d, std::vector<float>& pos, std::vector<uint32_t>& tris,
			const char*& reject)
		{
			sg::BufferRead b;
			if (!sg::read_buffer(d.packed, b) || !b.vb[0] || !b.ib || !b.stride[0] || !b.verts || !b.tris)
			{
				reject = "no packed buffers";
				return false;
			}
			D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
			UINT count = 0;
			if (!sg::buffer_layout(b, el, count))
			{
				reject = "no vertex layout";
				return false;
			}
			api::Attr at;
			for (UINT i = 0; i < count; i++)
			{
				const D3DVERTEXELEMENT9& e = el[i];
				if (e.Stream == 0xFF) break;
				if (e.Usage == D3DDECLUSAGE_BLENDWEIGHT || e.Usage == D3DDECLUSAGE_BLENDINDICES ||
					e.Usage == D3DDECLUSAGE_POSITIONT)
				{
					reject = "skinned or pretransformed layout";
					return false;
				}
				if (e.Stream == 0 && e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0 && !at.ok())
				{
					at.off = static_cast<int>(e.Offset);
					at.type = static_cast<int>(e.Type);
				}
			}
			const UINT stride = b.stride[0];
			if (!at.ok() || (at.type != D3DDECLTYPE_FLOAT3 && at.type != D3DDECLTYPE_FLOAT4) || !api::attr_fits(at, stride))
			{
				reject = "no FLOAT3 / FLOAT4 position on stream 0";
				return false;
			}
			const bool list = b.prim_type == D3DPT_TRIANGLELIST, strip = b.prim_type == D3DPT_TRIANGLESTRIP;
			const UINT want = list ? b.tris * 3u : strip ? b.tris + 2u : 0u;
			if (!want || want > b.index_count)
			{
				reject = "not a triangle list or strip";
				return false;
			}
			std::vector<uint8_t> vbytes;
			if (!api::copy_vertex_bytes(b.vb[0], b.base_vertex * stride, b.verts * stride, vbytes, reject)) return false;
			std::vector<uint32_t> idx;
			if (!api::copy_index_bytes(b.ib, 0, want, idx, reject)) return false;
			pos.resize(static_cast<size_t>(b.verts) * 3u);
			for (UINT i = 0; i < b.verts; i++)
			{
				const uint8_t* p = vbytes.data() + static_cast<size_t>(i) * stride + static_cast<size_t>(at.off);
				for (int k = 0; k < 3; k++) pos[static_cast<size_t>(i) * 3u + static_cast<size_t>(k)] = api::read_comp(p, at.type, k);
			}
			tris.clear();
			if (list)
				for (size_t i = 0; i + 2 < idx.size(); i += 3) emit(tris, b.verts, idx[i], idx[i + 1], idx[i + 2]);
			else emit_strip(tris, b.verts, idx.data(), idx.size());
			if (tris.empty())
			{
				reject = "no triangles";
				return false;
			}
			return true;
		}

		bool blended(const sg::GeoRead& gr)
		{
			const uintptr_t ap = gr.props[sg::fnv::kPropAlpha];
			uint16_t af = 0;
			return ap && sg::read_u16(ap + sg::fnv::ALPHA_flags, af) && (af & kAlphaBlend);
		}
	}

	bool node_name(uintptr_t obj, char (&out)[64])
	{
		out[0] = 0;
		uint32_t str = 0;
		if (!sg::read_u32(obj + OBJ_NAME_OFF, str) || str < 0x10000) return false;
		if (!sg::read_block(str, out, sizeof(out) - 1)) return false;
		out[sizeof(out) - 1] = 0;
		return true;
	}

	bool relative_pose(uintptr_t root, uintptr_t obj, RootPose& out)
	{
		Pose r, o;
		if (!world_pose(root, r) || !world_pose(obj, o)) return false;
		relative(r, o, out);
		return true;
	}

	bool collect(uintptr_t root, fit::Soup& out, Nodes& nodes, std::string& why)
	{
		sg::FaultSite site("scope fit");
		out = {};
		nodes = {};
		Pose rp;
		if (!root || !world_pose(root, rp))
		{
			why = "the weapon root's transform is not readable";
			return false;
		}
		out.unit_scale = rp.scale;
		nodes.root_scale = rp.scale;

		// The model's objects, each with its parent and name.
		std::vector<Visit> visits(1);
		visits[0].obj = root;
		node_name(root, visits[0].name);
		std::vector<int> open = { 0 };
		std::vector<int> geometry;
		uintptr_t kids[kMaxChildren];
		while (!open.empty())
		{
			const int at = open.back();
			open.pop_back();
			const uintptr_t obj = visits[static_cast<size_t>(at)].obj;
			uintptr_t parent = 0;
			uint32_t flags = 0;
			if (!sg::read_link(obj, parent, flags)) continue;
			if (at != 0 && (flags & 1u)) continue;   // app-culled: not drawn
			const sg::ClassInfo* ci = sg::class_of(obj);
			if (!ci) continue;
			if (ci->geometry)
			{
				if (ci->leaf == sg::Leaf::TriShape || ci->leaf == sg::Leaf::TriStrips) geometry.push_back(at);
				continue;
			}
			if (!ci->node || ci->excluded_subtree) continue;
			sg::NodeChildren info;
			if (!sg::read_children(obj, kids, kMaxChildren, info)) continue;
			for (uint32_t i = 0; i < info.read && visits.size() < kMaxNodeVisits; i++)
			{
				if (!kids[i]) continue;
				Visit v;
				v.obj = kids[i];
				v.parent = at;
				node_name(kids[i], v.name);
				visits.push_back(v);
				open.push_back(static_cast<int>(visits.size() - 1));
			}
		}
		std::unordered_map<std::string, int> name_count;
		for (const Visit& v : visits)
			if (v.name[0]) name_count[v.name]++;

		// The hints, in root space.
		for (const Visit& v : visits)
		{
			const bool projectile = std::strcmp(v.name, kProjectileNode) == 0;
			const bool sighting = std::strcmp(v.name, kSightingNode) == 0;
			if (!projectile && !sighting) continue;
			Pose p;
			if (!world_pose(v.obj, p)) continue;
			const double w[3] = { p.pos[0], p.pos[1], p.pos[2] };
			if (projectile && !out.hints.have_projectile)
			{
				to_root(rp, w, out.hints.projectile);
				out.hints.have_projectile = true;
			}
			if (sighting && !out.hints.have_sighting)
			{
				to_root(rp, w, out.hints.sighting);
				out.hints.have_sighting = true;
			}
		}

		std::unordered_map<int, int> anchor_of_visit;
		for (const int at : geometry)
		{
			const Visit& v = visits[static_cast<size_t>(at)];
			nodes.shapes++;
			sg::GeoRead gr;
			if (!sg::read_geo(v.obj, gr))
			{
				nodes.unreadable++;
				continue;
			}
			if (gr.skin)
			{
				nodes.skinned++;
				continue;
			}
			fit::Shape shape;
			std::vector<float> local;
			bool read = false;
			{
				const Resolve r(v.obj);
				sg::DataRead dd;
				if (r.ok && gr.data && sg::read_data(gr.data, dd))
				{
					const char* reject = nullptr;
					if (!dd.packed)
					{
						const sg::ClassInfo* ci = sg::class_of(v.obj);
						read = cpu_arrays(dd, ci && ci->leaf == sg::Leaf::TriStrips, local, shape.tris);
					}
					else read = packed_buffers(dd, local, shape.tris, reject);
				}
			}
			Pose gp;
			if (!read || !world_pose(v.obj, gp))
			{
				nodes.unreadable++;
				continue;
			}
			// Object space -> world -> root space.
			bool finite = true;
			shape.pos.resize(local.size());
			for (size_t i = 0; i + 2 < local.size() && finite; i += 3)
			{
				double w[3];
				for (int r = 0; r < 3; r++)
					w[r] = (gp.rot[r * 3] * local[i] + gp.rot[r * 3 + 1] * local[i + 1] + gp.rot[r * 3 + 2] * local[i + 2]) *
						gp.scale + gp.pos[r];
				to_root(rp, w, &shape.pos[i]);
				finite = std::isfinite(shape.pos[i]) && std::isfinite(shape.pos[i + 1]) && std::isfinite(shape.pos[i + 2]);
			}
			if (!finite)
			{
				nodes.unreadable++;
				continue;
			}
			shape.blended = blended(gr);

			// The anchor: the nearest ancestor-or-self with a unique name
			// below the root.
			int anchor = -1;
			for (int j = at; j > 0; j = visits[static_cast<size_t>(j)].parent)
			{
				const Visit& a = visits[static_cast<size_t>(j)];
				if (!a.name[0] || name_count[a.name] != 1) continue;
				const auto it = anchor_of_visit.find(j);
				if (it != anchor_of_visit.end()) { anchor = it->second; break; }
				Anchor an;
				strncpy_s(an.name, a.name, _TRUNCATE);
				if (!relative_pose(root, a.obj, an.pose)) continue;
				nodes.anchors.push_back(an);
				anchor = static_cast<int>(nodes.anchors.size() - 1);
				anchor_of_visit[j] = anchor;
				break;
			}
			nodes.shape_anchor.push_back(anchor);
			out.shapes.push_back(std::move(shape));
		}

		const uint32_t used = static_cast<uint32_t>(out.shapes.size());
		if (!nodes.shapes) why = "no geometry under the weapon root";
		else if (!used) why = nodes.skinned == nodes.shapes ? "every shape is skinned"
			: std::format("geometry not readable yet ({} shapes)", nodes.shapes);
		else if (nodes.unreadable || nodes.skinned)
			why = std::format("{} of {} shapes read ({} skinned, {} unreadable)", used, nodes.shapes, nodes.skinned,
				nodes.unreadable);
		else why = std::format("{} shapes read", used);
		return used > 0;
	}
}
