#include "common.hpp"
#include "owners.hpp"
#include "cells.hpp"
#include "decals.hpp"
#include "fog.hpp"
#include "lights.hpp"
#include "monitor.hpp"
#include "pins.hpp"
#include "queue.hpp"
#include "rtti.hpp"

namespace fnvr::owners
{
	namespace
	{
		// Nodes walked per reference and children read per node: a static
		// collection's (SCOL) root holds every placed part as a child.
		constexpr uint32_t kMaxNodes = 32768;
		constexpr uint32_t kMaxChildren = 8192;
		constexpr int kMaxChainHops = 64;
		constexpr uint32_t kNoEventFrames = 600;      // an attach with no ref 3D event this long: hooks cut
		constexpr uint32_t kMaxCellRefs = 65536;      // list nodes read per cell

		struct Owner
		{
			uintptr_t ref = 0, root = 0;
			uintptr_t cell = 0;                        // the reference's parent cell at the walk
			uintptr_t resolved_cell = 0;               // the attached cell it hangs in (owner_of)
			uint32_t resolved_serial = 0xFFFFFFFFu;    // cells::serial() of resolved_cell
			uint32_t ref_id = 0;
			uintptr_t base = 0;                        // the base form
			uint8_t ref_type = 0, base_type = 0;
			std::vector<uintptr_t> geos;
		};

		std::unordered_map<uintptr_t, Owner> g_owners;            // reference -> owner
		std::unordered_map<uintptr_t, uintptr_t> g_geo_owner;     // geometry -> reference

		// The member feed (take_members): every geometry a walk of a
		// non-actor reference found, in walk order. Off until the wrapper's
		// first call; `resend` refills it from the whole map (the first
		// call, and after an overflow dropped it).
		constexpr size_t kMaxFeed = 1u << 18;
		bool g_feed_on = false, g_feed_resend = false;
		std::vector<FnvRemixMember> g_feed;
		size_t g_feed_pos = 0;
		uint32_t g_feed_overflows = 0;

		// Since the last status line.
		uint32_t g_set3d = 0, g_unset3d = 0, g_attach = 0, g_unload = 0, g_walks = 0, g_nodes = 0, g_capped = 0,
			g_gone = 0, g_rejected = 0, g_detach_drops = 0, g_adopted = 0;
		uint32_t g_last_status = 0;
		// The invariant: ref 3D events follow cell attaches.
		uint32_t g_attach_frame = 0;
		bool g_attach_pending = false;
		bool g_events_ok = true;

		// A live reference: vtable in the image and a reference form type.
		bool read_ref(uintptr_t ref, Owner& o)
		{
			uint32_t vtbl = 0, base = 0, cell = 0, id = 0;
			uint8_t type = 0, base_type = 0;
			if (!guard::plausible(ref) || !guard::get(ref, vtbl) || !guard::in_image(vtbl) ||
				!guard::get(ref + engine::FORM_type, type) || type < engine::kFormType_REFR_first ||
				type > engine::kFormType_REFR_last)
				return false;
			guard::get(ref + engine::FORM_refID, id);
			guard::get(ref + engine::REFR_parentCell, cell);
			if (guard::get(ref + engine::REFR_baseForm, base) && guard::plausible(base))
				guard::get(base + engine::FORM_type, base_type);
			o.ref = ref;
			o.ref_type = type;
			o.base = guard::plausible(base) ? base : 0;
			o.base_type = base_type;
			o.cell = cell;
			o.ref_id = id;
			return true;
		}

		uintptr_t live_root(uintptr_t ref)
		{
			uint32_t rs = 0, root = 0;
			if (!guard::get(ref + engine::REFR_renderState, rs) || !guard::plausible(rs) ||
				!guard::get(rs + engine::RS_niNode, root))
				return 0;
			return guard::plausible(root) ? root : 0;
		}

		bool actor(const Owner& o)
		{
			return o.ref_type == engine::kFormType_ACHR || o.ref_type == engine::kFormType_ACRE;
		}

		void feed(const Owner& o)
		{
			if (!g_feed_on || actor(o) || g_feed_resend) return;
			if (g_feed.size() - g_feed_pos + o.geos.size() > kMaxFeed)
			{
				g_feed.clear();
				g_feed_pos = 0;
				g_feed_resend = true;
				g_feed_overflows++;
				return;
			}
			for (const uintptr_t g : o.geos) g_feed.push_back({ g, o.ref });
		}

		void forget_geos(Owner& o)
		{
			for (const uintptr_t g : o.geos)
				if (const auto it = g_geo_owner.find(g); it != g_geo_owner.end() && it->second == o.ref) g_geo_owner.erase(it);
			o.geos.clear();
		}

		// One walk of the reference's 3D: every geometry -> this reference,
		// every light -> the light registry, the geometry -> the decal flag and
		// the game-fog rule.
		void walk(Owner& o)
		{
			forget_geos(o);
			static std::vector<uintptr_t> s_stack, s_lights;
			static uint32_t s_children[kMaxChildren];
			s_stack.clear();
			s_lights.clear();
			s_stack.push_back(o.root);
			uint32_t nodes = 0;
			while (!s_stack.empty())
			{
				if (nodes >= kMaxNodes)
				{
					g_capped++;
					break;
				}
				const uintptr_t n = s_stack.back();
				s_stack.pop_back();
				nodes++;
				const uint32_t cls = rtti::of(n);
				if (cls & rtti::kGeometry)
				{
					o.geos.push_back(n);
					g_geo_owner[n] = o.ref;
					continue;
				}
				if (cls & rtti::kLight)
				{
					s_lights.push_back(n);
					continue;
				}
				if (!(cls & rtti::kNode)) continue;
				uint16_t count = 0;
				uint32_t data = 0;
				if (!guard::get(n + engine::NODE_childCount, count) || !guard::get(n + engine::NODE_childData, data) ||
					!count || !data)
					continue;
				if (count > kMaxChildren) g_capped++;
				const uint32_t c = (std::min)(static_cast<uint32_t>(count), kMaxChildren);
				if (!guard::read(data, s_children, c * sizeof(uint32_t))) continue;
				for (uint32_t i = 0; i < c; i++)
					if (guard::plausible(s_children[i])) s_stack.push_back(s_children[i]);
			}
			g_walks++;
			g_nodes += nodes;
			lights::set_placed(o.ref, s_lights);
			decals::on_walk(o.ref, o.base_type, o.base, o.geos);
			fog::on_walk(o.ref, o.ref_type, o.base_type, o.base, o.geos);
			feed(o);
		}

		// The reference lets go of its 3D: the wrapper hears of it (emit), the
		// root's pin goes at the next main loop.
		void drop(uintptr_t ref, bool emit)
		{
			const auto it = g_owners.find(ref);
			if (it == g_owners.end()) return;
			Owner& o = it->second;
			forget_geos(o);
			lights::on_owner_dropped(ref);
			if (emit)
			{
				queue::emit(FNVR_EV_OWNER_GONE, ref, o.root);
				g_gone++;
			}
			pins::release(o.root, ref);
			g_owners.erase(it);
		}

		void note_event()
		{
			g_attach_pending = false;
			if (!g_events_ok)
			{
				g_events_ok = true;
				log::f("OwnerMap", "ref 3D events arrive again: owner map on");
			}
		}

		// The attached cell a reference's 3D hangs in: its parent cell when that
		// is attached, else the attached cell whose root is on the chain above
		// the reference's root (persistent references).
		uintptr_t resolve_cell(Owner& o)
		{
			const uint32_t serial = cells::serial();
			if (o.resolved_serial == serial) return o.resolved_cell;
			o.resolved_serial = serial;
			o.resolved_cell = 0;
			if (cells::attached(o.cell))
			{
				o.resolved_cell = o.cell;
				return o.cell;
			}
			uintptr_t node = o.root;
			for (int hop = 0; hop < kMaxChainHops && node; hop++)
			{
				if (const uintptr_t c = cells::cell_of_root(node))
				{
					o.resolved_cell = c;
					break;
				}
				uint32_t parent = 0;
				if (!guard::get(node + engine::AV_parent, parent)) break;
				node = parent;
			}
			return o.resolved_cell;
		}
	}

	void on_set3d(uintptr_t ref, uintptr_t root)
	{
		g_set3d++;
		note_event();
		if (const auto it = g_owners.find(ref); it != g_owners.end())
		{
			if (it->second.root == root)
			{
				// The same 3D again: the extra reference goes back, the walk
				// picks up whatever changed.
				pins::adopt(root, ref);
				walk(it->second);
				return;
			}
			drop(ref, true);
		}
		Owner o;
		if (!read_ref(ref, o))
		{
			// An unreadable reference: the handler's reference goes back.
			g_rejected++;
			pins::adopt(root, ref);
			pins::release(root, ref);
			return;
		}
		pins::adopt(root, ref);
		o.root = root;
		auto& slot = g_owners[ref];
		slot = std::move(o);
		walk(slot);
	}

	void on_unset3d(uintptr_t ref)
	{
		g_unset3d++;
		drop(ref, true);
	}

	void on_attach(uintptr_t ref)
	{
		g_attach++;
		note_event();
		if (const auto it = g_owners.find(ref); it != g_owners.end())
		{
			// Script-attached models (JIP AttachModel / InsertNode) arrive with
			// no new Set3D: walk again.
			const uintptr_t root = live_root(ref);
			if (root && root != it->second.root)
			{
				drop(ref, true);
			}
			else
			{
				walk(it->second);
				return;
			}
		}
		Owner o;
		if (!read_ref(ref, o))
		{
			g_rejected++;
			return;
		}
		const uintptr_t root = live_root(ref);
		if (!root || !pins::pin(root, ref)) return;
		o.root = root;
		auto& slot = g_owners[ref];
		slot = std::move(o);
		walk(slot);
	}

	void adopt_cell(uintptr_t cell, const char* how)
	{
		// The list is read without the engine's lock (cell+0x80): every read is
		// guarded, the node count is capped, and each reference is checked as
		// an attach checks it.
		uint32_t node[2] = {};
		if (!guard::plausible(cell) || !guard::read(cell + engine::CELL_objectList, node, sizeof(node))) return;
		uint32_t nodes = 0, adopted = 0;
		for (;;)
		{
			const uintptr_t ref = node[0];
			if (guard::plausible(ref) && !g_owners.count(ref))
			{
				Owner o;
				const uintptr_t root = live_root(ref);
				if (root && read_ref(ref, o) && pins::pin(root, ref))
				{
					o.root = root;
					auto& slot = g_owners[ref];
					slot = std::move(o);
					walk(slot);
					adopted++;
				}
			}
			if (++nodes >= kMaxCellRefs || !guard::plausible(node[1]) || !guard::read(node[1], node, sizeof(node))) break;
		}
		g_adopted += adopted;
		if (adopted)
			log::f("OwnerMap", "adopted {} refs in cell 0x{:X} ({}): {} list entries, owners now {}", adopted, cell, how,
				nodes, g_owners.size());
	}

	void on_form_unload(uintptr_t form)
	{
		if (!g_owners.count(form)) return;
		g_unload++;
		drop(form, true);
	}

	void on_cell_detached(uintptr_t cell)
	{
		static std::vector<uintptr_t> s_refs;
		s_refs.clear();
		for (const auto& [ref, o] : g_owners)
			if (o.cell == cell || o.resolved_cell == cell) s_refs.push_back(ref);
		for (const uintptr_t ref : s_refs) drop(ref, false);
		g_detach_drops += static_cast<uint32_t>(s_refs.size());
	}

	void on_cell_attached(uintptr_t)
	{
		g_attach_frame = monitor::frame();
		g_attach_pending = true;
	}

	void clear(const char* why)
	{
		const size_t n = g_owners.size();
		for (auto& [ref, o] : g_owners)
		{
			lights::on_owner_dropped(ref);
			pins::release(o.root, ref);
		}
		g_owners.clear();
		g_geo_owner.clear();
		g_feed.clear();
		g_feed_pos = 0;
		if (n) log::f("OwnerMap", "{}: dropped all {} owners", why, n);
	}

	void tick(uint32_t frame)
	{
		if (g_attach_pending && frame - g_attach_frame >= kNoEventFrames && g_events_ok)
		{
			g_events_ok = false;
			log::f("OwnerMap", "invariant: a cell attached {} main-loop frames ago and no ref 3D event (Set3D / attach) "
				"arrived since -- xNVSE's hooks look cut: owner map OFF until one arrives", kNoEventFrames);
		}
		if (frame - g_last_status >= 600)
		{
			g_last_status = frame;
			// The 600-frame status is a dev diagnostic: [Logging] Verbose only.
			if (log::verbose() && (g_set3d || g_unset3d || g_attach || g_unload || g_walks))
				log::f("OwnerMap", "owners {} geometry {} | set3D +{} unset3D +{} attach +{} unload +{} | walks +{} (nodes "
					"+{}, capped {}) gone events +{} dropped at cell detach +{} adopted from cell lists +{} rejected +{} | pins "
					"live {} | member feed {} queued, overflows {}",
					g_owners.size(), g_geo_owner.size(), g_set3d, g_unset3d, g_attach, g_unload, g_walks, g_nodes, g_capped,
					g_gone, g_detach_drops, g_adopted, g_rejected, pins::live(), g_feed.size() - g_feed_pos, g_feed_overflows);
			g_set3d = g_unset3d = g_attach = g_unload = g_walks = g_nodes = g_capped = g_gone = g_rejected =
				g_detach_drops = g_adopted = 0;
		}
	}

	bool owner_of(uintptr_t geo, FnvRemixOwner& out)
	{
		const auto git = g_geo_owner.find(geo);
		if (git == g_geo_owner.end()) return false;
		const auto oit = g_owners.find(git->second);
		if (oit == g_owners.end()) return false;
		Owner& o = oit->second;
		// The chain as it is now: does it still reach the pinned root, and is
		// a node on it app-culled, from the geometry up to the root (the
		// reference's own 3D) or above it (the cell's nodes)? An unreadable
		// link: no answer.
		bool reached = false, culled = false, culled_above = false;
		uintptr_t node = geo;
		for (int hop = 0; hop < kMaxChainHops && node; hop++)
		{
			uint32_t parent = 0, flags = 0;
			if (!guard::get(node + engine::AV_parent, parent) || !guard::get(node + engine::AV_flags, flags)) return false;
			if (flags & 1u) (reached ? culled_above : culled) = true;
			if (node == o.root) reached = true;
			node = parent;
		}
		out = {};
		out.ref = o.ref;
		out.root = o.root;
		out.cell = resolve_cell(o);
		out.ref_id = o.ref_id;
		out.base_type = o.base_type;
		out.flags = 0;
		if (o.ref_type == engine::kFormType_ACHR || o.ref_type == engine::kFormType_ACRE) out.flags |= FNVR_OW_ACTOR;
		if (disabled(o.ref)) out.flags |= FNVR_OW_DISABLED;
		if (!reached) out.flags |= FNVR_OW_DETACHED;
		if (culled) out.flags |= FNVR_OW_APP_CULLED;
		if (culled_above) out.flags |= FNVR_OW_APP_CULLED_ABOVE;
		return true;
	}

	uint32_t take_members(FnvRemixMember* out, uint32_t max)
	{
		if (!g_feed_on)
		{
			g_feed_on = true;
			g_feed_resend = true;
		}
		if (g_feed_resend)
		{
			g_feed_resend = false;
			g_feed.clear();
			g_feed_pos = 0;
			for (const auto& [ref, o] : g_owners) feed(o);
			if (g_feed_resend) log::f("OwnerMap", "member feed: the owner map holds more than {} geometries", kMaxFeed);
		}
		const uint32_t n = static_cast<uint32_t>((std::min)(g_feed.size() - g_feed_pos, static_cast<size_t>(max)));
		if (n) std::memcpy(out, g_feed.data() + g_feed_pos, n * sizeof(FnvRemixMember));
		g_feed_pos += n;
		if (g_feed_pos == g_feed.size())
		{
			g_feed.clear();
			g_feed_pos = 0;
		}
		return n;
	}

	void resend_members()
	{
		g_feed_on = true;
		g_feed_resend = true;
	}

	bool events_ok() { return g_events_ok; }
	uint32_t count() { return static_cast<uint32_t>(g_owners.size()); }
	uint32_t geometry_count() { return static_cast<uint32_t>(g_geo_owner.size()); }

	bool disabled(uintptr_t ref)
	{
		uint32_t flags = 0;
		return guard::get(ref + engine::FORM_flags, flags) &&
			(flags & (engine::kFormFlag_Disabled | engine::kFormFlag_Deleted)) != 0;
	}
}
