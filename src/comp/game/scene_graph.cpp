#include "std_include.hpp"
#include "scene_graph.hpp"
#include "call_chain.hpp"

#include <atomic>

// See scene_graph.hpp. The guarded readers below keep their __try frames free
// of C++ objects (no unwinding may share a frame with SEH); everything that
// allocates happens in their callers.

namespace comp::game::scene_graph
{
	namespace
	{
		std::atomic<uint32_t> g_faults { 0 };

		// Fault attribution: the site the render thread's reader named
		// (FaultSite) and the address of the last faulting read. Plain
		// stores: written in __except blocks, read at the status line.
		const char* volatile g_site = nullptr;
		const char* volatile g_fault_site = nullptr;
		volatile uintptr_t g_fault_addr = 0;

		void note_fault(uintptr_t addr)
		{
			g_faults.fetch_add(1, std::memory_order_relaxed);
			g_fault_site = g_site;
			g_fault_addr = addr;
		}

		uintptr_t g_image_lo = 0, g_image_hi = 0;

		void init_image_range()
		{
			if (g_image_hi) return;
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			g_image_lo = base;
			g_image_hi = base + nt->OptionalHeader.SizeOfImage;
		}

		bool in_image(uintptr_t p) { return p >= g_image_lo && p < g_image_hi; }
		// Any user-mode address of a 4-byte aligned object. FNV runs large-address
		// aware (4GB patch), so the heap reaches above 2 GB.
		bool plausible(uintptr_t p) { return p >= 0x10000 && (p & 3) == 0; }

		// ---- Guarded primitives (no C++ objects in these frames) ----

		bool copy_unguarded(uintptr_t addr, void* out, size_t n)
		{
			std::memcpy(out, reinterpret_cast<const void*>(addr), n);
			return true;
		}

		bool guarded_copy(uintptr_t addr, void* out, size_t n)
		{
			__try { return copy_unguarded(addr, out, n); }
			__except (EXCEPTION_EXECUTE_HANDLER) { note_fault(addr); return false; }
		}

		// RTTI of one vtable: the class name (decoration stripped) and which of
		// the interesting bases it derives from.
		enum : uint32_t
		{
			kBaseNode = 1u << 0, kBaseGeometry = 1u << 1, kBaseTree = 1u << 2, kBaseBillboard = 1u << 3,
			kBaseLod = 1u << 4, kBaseSourceTexture = 1u << 5, kBaseSwitch = 1u << 6, kBaseRange = 1u << 7,
			kBaseWaterShader = 1u << 8, kBaseShaderProperty = 1u << 9,
		};

		// Base bit of a class name (undecorated), 0 for the uninteresting ones.
		uint32_t base_bit(const char* n)
		{
			if (std::strcmp(n, "NiNode") == 0) return kBaseNode;
			if (std::strcmp(n, "NiGeometry") == 0) return kBaseGeometry;
			if (std::strcmp(n, "BSTreeNode") == 0) return kBaseTree;
			if (std::strcmp(n, "NiBillboardNode") == 0) return kBaseBillboard;
			if (std::strcmp(n, "NiLODNode") == 0) return kBaseLod;
			if (std::strcmp(n, "NiSwitchNode") == 0) return kBaseSwitch;
			if (std::strcmp(n, "BSRangeNode") == 0) return kBaseRange;
			if (std::strcmp(n, "NiSourceTexture") == 0) return kBaseSourceTexture;
			if (std::strcmp(n, "WaterShaderProperty") == 0) return kBaseWaterShader;
			if (std::strcmp(n, "BSShaderProperty") == 0) return kBaseShaderProperty;
			return 0;
		}

		// ".?AVName@@" -> "Name" for base_bit (nested / templated names don't matter).
		uint32_t base_bit_decorated(const char* d)
		{
			if (d[0] != '.' || d[1] != '?' || d[2] != 'A') return 0;
			char n[64];
			size_t k = 0;
			for (const char* p = d + 4; *p && k + 1 < sizeof(n); p++)
			{
				if (p[0] == '@') break;
				n[k++] = *p;
			}
			n[k] = 0;
			return base_bit(n);
		}

		bool rtti_unguarded(uintptr_t vtbl, char* name, size_t cap, uint32_t& bases)
		{
			// The complete object locator must be one: in the image, signature
			// 0 and pointing at a type descriptor in the image. (vtable-4 of a
			// class compiled without RTTI is 0 or the previous vtable's last
			// code pointer.)
			const auto col = *reinterpret_cast<const uintptr_t*>(vtbl - 4);
			if (!in_image(col) || (col & 3)) return false;
			if (*reinterpret_cast<const uint32_t*>(col) != 0) return false;
			const auto td = *reinterpret_cast<const uintptr_t*>(col + 0x0C);
			const auto chd = *reinterpret_cast<const uintptr_t*>(col + 0x10);
			if (!in_image(td) || !in_image(chd)) return false;
			const char* raw = reinterpret_cast<const char*>(td + 8);
			if (raw[0] != '.' || raw[1] != '?') return false;
			size_t n = 0;
			for (const char* p = raw + 4; *p && n + 1 < cap; p++)
			{
				if (p[0] == '@' && p[1] == '@') break;
				name[n++] = *p;
			}
			name[n] = 0;

			bases = 0;
			const auto count = *reinterpret_cast<const uint32_t*>(chd + 0x08);
			const auto arr = *reinterpret_cast<const uintptr_t*>(chd + 0x0C);
			if (!in_image(arr) || count > 64) return true;
			for (uint32_t i = 0; i < count; i++)
			{
				const auto bcd = *reinterpret_cast<const uintptr_t*>(arr + 4 * i);
				if (!in_image(bcd)) continue;
				const auto btd = *reinterpret_cast<const uintptr_t*>(bcd);
				if (!in_image(btd)) continue;
				bases |= base_bit_decorated(reinterpret_cast<const char*>(btd + 8));
			}
			return true;
		}

		bool rtti_guarded(uintptr_t vtbl, char* name, size_t cap, uint32_t& bases)
		{
			__try { return rtti_unguarded(vtbl, name, cap, bases); }
			__except (EXCEPTION_EXECUTE_HANDLER) { note_fault(vtbl); return false; }
		}

		// NiRTTI* returned by a GetRTTI body, 0 when the code isn't one. The
		// two shapes FalloutNV.exe has (0xB4E1D0 BSFadeNode, 0x66B5B0
		// BSTreeNode):
		//   B8 imm32 C3                              mov eax, imm; ret
		//   55 8B EC 51 89 4D FC B8 imm32 8B E5 5D C3  (the same in a frame)
		uintptr_t get_rtti_target(const uint8_t* c)
		{
			uint32_t imm = 0;
			if (c[0] == 0xB8 && c[5] == 0xC3)
			{
				std::memcpy(&imm, c + 1, 4);
				return imm;
			}
			static constexpr uint8_t kFramed[] = { 0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC, 0xB8 };
			if (std::memcmp(c, kFramed, sizeof(kFramed)) == 0 && c[12] == 0x8B && c[13] == 0xE5 && c[14] == 0x5D &&
				c[15] == 0xC3)
			{
				std::memcpy(&imm, c + 8, 4);
				return imm;
			}
			return 0;
		}

		// Copies a class name from the image: identifier characters only, at
		// least two of them.
		bool copy_ident(uintptr_t s, char* out, size_t cap)
		{
			if (!in_image(s)) return false;
			const char* p = reinterpret_cast<const char*>(s);
			size_t n = 0;
			for (; p[n] && n + 1 < cap; n++)
			{
				const char ch = p[n];
				const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
					ch == '_' || ch == ':';
				if (!ok) return false;
				out[n] = ch;
			}
			out[n] = 0;
			return n >= 2 && p[n] == 0;
		}

		// Gamebryo RTTI of one vtable: vtable slot 2 (NiObject::GetRTTI) ->
		// NiRTTI {const char* name; const NiRTTI* parent} -> the chain.
		bool nirtti_unguarded(uintptr_t vtbl, char* name, size_t cap, uint32_t& bases)
		{
			const auto fn = *reinterpret_cast<const uintptr_t*>(vtbl + 8);
			if (!in_image(fn)) return false;
			uintptr_t rtti = get_rtti_target(reinterpret_cast<const uint8_t*>(fn));
			if (!in_image(rtti) || (rtti & 3)) return false;
			bases = 0;
			char n[96];
			for (int depth = 0; rtti && depth < 32; depth++)
			{
				if (!in_image(rtti) || (rtti & 3)) return depth > 0;
				const auto s = *reinterpret_cast<const uintptr_t*>(rtti);
				if (!copy_ident(s, n, sizeof(n))) return depth > 0;
				if (depth == 0)
				{
					std::memcpy(name, n, (std::min)(cap, sizeof(n)));
					name[cap - 1] = 0;
				}
				bases |= base_bit(n);
				rtti = *reinterpret_cast<const uintptr_t*>(rtti + 4);
			}
			return true;
		}

		bool nirtti_guarded(uintptr_t vtbl, char* name, size_t cap, uint32_t& bases)
		{
			__try { return nirtti_unguarded(vtbl, name, cap, bases); }
			__except (EXCEPTION_EXECUTE_HANDLER) { note_fault(vtbl); return false; }
		}

		uint32_t list_unguarded(uintptr_t head, uintptr_t* out, uint32_t max)
		{
			// tList: the first node is inline {data, next}; data may be null in
			// an empty list.
			uint32_t n = 0;
			uintptr_t node = head;
			for (uint32_t guard = 0; node && guard < 200000 && n < max; guard++)
			{
				const auto data = *reinterpret_cast<const uintptr_t*>(node);
				const auto next = *reinterpret_cast<const uintptr_t*>(node + 4);
				if (data) out[n++] = data;
				if (next && !plausible(next)) break;
				node = next;
			}
			return n;
		}

		uint32_t list_guarded(uintptr_t head, uintptr_t* out, uint32_t max)
		{
			__try { return list_unguarded(head, out, max); }
			__except (EXCEPTION_EXECUTE_HANDLER) { note_fault(head); return 0; }
		}

		bool name_unguarded(uintptr_t str, char* out, size_t cap)
		{
			const char* s = reinterpret_cast<const char*>(str);
			size_t n = 0;
			while (s[n] && n + 1 < cap)
			{
				out[n] = s[n];
				n++;
			}
			out[n] = 0;
			return n > 0;
		}

		bool name_guarded(uintptr_t str, char* out, size_t cap)
		{
			__try { return name_unguarded(str, out, cap); }
			__except (EXCEPTION_EXECUTE_HANDLER) { note_fault(str); out[0] = 0; return false; }
		}

		// ---- Class cache ----

		std::unordered_map<uintptr_t, ClassInfo> g_classes;

		// ---- Hooks ----

		using DispatchFn = void(__cdecl*)(void* pass, uint32_t type, uint32_t a, uint32_t b, uint32_t c);
		using DtorFn = void(__fastcall*)(void* self, void* edx);

		DispatchFn g_orig_dispatch = nullptr;
		DtorFn g_orig_dtor = nullptr;
		bool g_hooks_tried = false;
		bool g_hooks_ok = false;

		// Render-thread state (the dispatcher hook touches it only on the
		// render thread).
		uintptr_t g_current_geometry = 0;
		uint16_t g_current_pass_type = kNoPassType;
		PassFilter g_filter = nullptr;
		uint32_t g_skipped = 0;

		std::atomic<DWORD> g_render_thread { 0 };
		std::atomic<uint64_t> g_passes_off_thread { 0 };

		// The dead set: written by ~NiGeometry on any thread, read and drained
		// by the render thread, always under g_dead_lock. g_dead keeps the
		// order for take_deaths, g_dead_set answers death_pending in O(1).
		SRWLOCK g_dead_lock = SRWLOCK_INIT;
		std::vector<uintptr_t> g_dead;
		std::unordered_set<uintptr_t> g_dead_set;
		std::atomic<uint32_t> g_dead_count { 0 };   // entries in g_dead (lock-free "anything queued?")
		std::atomic<uint64_t> g_deaths { 0 };
		std::atomic<uint64_t> g_deaths_off_thread { 0 };

		// The geometry the render thread is reading between frames right now
		// (begin_resolve .. end_resolve), 0 otherwise. A destructor of exactly
		// this geometry on another thread waits for it, at most
		// kResolveWaitCapMs.
		std::atomic<uintptr_t> g_resolving { 0 };
		constexpr ULONGLONG kResolveWaitCapMs = 250;
		std::atomic<uint64_t> g_resolve_waits { 0 };
		std::atomic<uint64_t> g_resolve_wait_timeouts { 0 };
		std::atomic<uint64_t> g_resolves_skipped_dead { 0 };

		bool on_render_thread()
		{
			const DWORD rt = g_render_thread.load(std::memory_order_relaxed);
			return rt && GetCurrentThreadId() == rt;
		}

		void __cdecl dispatch_hk(void* pass, uint32_t type, uint32_t a, uint32_t b, uint32_t c)
		{
			// Only the render thread's passes are filtered or recorded: the
			// claimed set and current_geometry belong to it. (FNV renders on
			// one thread; anything else passes through untouched.)
			if (!on_render_thread())
			{
				g_passes_off_thread.fetch_add(1, std::memory_order_relaxed);
				g_orig_dispatch(pass, type, a, b, c);
				return;
			}
			// pass+0 is the NiGeometry*; the original reads it first thing.
			// The engine is rendering it, so it is alive for this call: the
			// filter may read it (its sighting comes before its verdict).
			const uintptr_t geo = pass ? *reinterpret_cast<const uintptr_t*>(pass) : 0;
			if (geo && g_filter && g_filter(geo))
			{
				g_skipped++;
				return;
			}
			// pass+4 is the pass type (the BSShaderManager pass enum); the
			// original reads it next.
			const uintptr_t prev = g_current_geometry;
			const uint16_t prev_type = g_current_pass_type;
			g_current_geometry = geo;
			g_current_pass_type = pass ? *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(pass) + 4) : kNoPassType;
			g_orig_dispatch(pass, type, a, b, c);
			g_current_geometry = prev;
			g_current_pass_type = prev_type;
		}

		// Runs before the original destructor touches anything, so the
		// geometry is in the dead set for the whole time its memory is being
		// torn down and after it is freed.
		void __fastcall dtor_hk(void* self, void* edx)
		{
			if (self)
			{
				const auto p = reinterpret_cast<uintptr_t>(self);
				AcquireSRWLockExclusive(&g_dead_lock);
				if (g_dead_set.insert(p).second) g_dead.push_back(p);
				// seq_cst: this store and the g_resolving load below pair with
				// begin_resolve's store / load in the opposite order (Dekker).
				g_dead_count.store(static_cast<uint32_t>(g_dead.size()), std::memory_order_seq_cst);
				ReleaseSRWLockExclusive(&g_dead_lock);
				g_deaths.fetch_add(1, std::memory_order_relaxed);
				if (!on_render_thread())
				{
					g_deaths_off_thread.fetch_add(1, std::memory_order_relaxed);
					// Marked dead first, then wait: a read that starts after the
					// mark sees it and skips; one that started before published
					// g_resolving first, so it is seen here. (The render thread
					// itself never waits: its own reads are sequential.)
					std::atomic_thread_fence(std::memory_order_seq_cst);
					if (g_resolving.load(std::memory_order_seq_cst) == p)
					{
						g_resolve_waits.fetch_add(1, std::memory_order_relaxed);
						const ULONGLONG t0 = GetTickCount64();
						while (g_resolving.load(std::memory_order_seq_cst) == p)
						{
							if (GetTickCount64() - t0 > kResolveWaitCapMs)
							{
								g_resolve_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
								break;
							}
							SwitchToThread();
						}
					}
				}
			}
			g_orig_dtor(self, edx);
		}

		// ---- Grass batch hooks ----

		using GrassAppendFn = uint32_t(__fastcall*)(void* batch, void* edx, void* pos, uint32_t key);
		using GrassRemoveFn = uint32_t(__fastcall*)(void* batch, void* edx, uint32_t index);
		using GrassDestroyFn = void(__fastcall*)(void* batch, void* edx);

		GrassAppendFn g_orig_grass_append = nullptr;
		GrassRemoveFn g_orig_grass_remove = nullptr;
		GrassDestroyFn g_orig_grass_destroy = nullptr;
		bool g_grass_hooks_tried = false;
		bool g_grass_hooks_ok = false;

		// Written by the hooks on any thread, drained by the render thread;
		// always under g_grass_lock.
		SRWLOCK g_grass_lock = SRWLOCK_INIT;
		uint64_t g_grass_counter = 0;
		std::unordered_map<uintptr_t, uint64_t> g_grass_gen;
		std::vector<uintptr_t> g_grass_changed;
		std::unordered_set<uintptr_t> g_grass_changed_set;
		std::vector<uintptr_t> g_grass_destroyed;
		std::atomic<uint64_t> g_grass_appends { 0 }, g_grass_removes { 0 }, g_grass_destroys { 0 };

		// The batch's NiGeometry (batch+0), 0 when unreadable. Guarded: the
		// batch is live (the engine is inside one of its methods), but the
		// read costs nothing extra.
		uintptr_t batch_geometry(void* batch)
		{
			uintptr_t geo = 0;
			if (!batch || !guarded_copy(reinterpret_cast<uintptr_t>(batch), &geo, sizeof(geo))) return 0;
			return geo;
		}

		void grass_event(void* batch, bool destroyed)
		{
			const uintptr_t geo = batch_geometry(batch);
			if (!geo) return;
			AcquireSRWLockExclusive(&g_grass_lock);
			g_grass_gen[geo] = ++g_grass_counter;
			if (destroyed) g_grass_destroyed.push_back(geo);
			else if (g_grass_changed_set.insert(geo).second) g_grass_changed.push_back(geo);
			ReleaseSRWLockExclusive(&g_grass_lock);
		}

		uint32_t __fastcall grass_append_hk(void* batch, void* edx, void* pos, uint32_t key)
		{
			g_grass_appends.fetch_add(1, std::memory_order_relaxed);
			grass_event(batch, false);
			return g_orig_grass_append(batch, edx, pos, key);
		}

		uint32_t __fastcall grass_remove_hk(void* batch, void* edx, uint32_t index)
		{
			g_grass_removes.fetch_add(1, std::memory_order_relaxed);
			grass_event(batch, false);
			return g_orig_grass_remove(batch, edx, index);
		}

		void __fastcall grass_destroy_hk(void* batch, void* edx)
		{
			// Before the original: it clears batch+0.
			g_grass_destroys.fetch_add(1, std::memory_order_relaxed);
			grass_event(batch, true);
			g_orig_grass_destroy(batch, edx);
		}

		constexpr const char* kIfReplaced = "The scene-graph features keep working only if that code continues to "
			"what it replaced.";
	}

	// ---- Class identity ----

	const ClassInfo* class_of_vtbl(uintptr_t vtbl)
	{
		init_image_range();
		if (!in_image(vtbl)) return nullptr;
		if (const auto it = g_classes.find(vtbl); it != g_classes.end()) return &it->second;

		char name[128] = {};
		uint32_t bases = 0;
		ClassInfo ci;
		ci.vtbl = vtbl;
		// MSVC RTTI first; Gamebryo's NiRTTI for the NiObjects without it.
		if (rtti_guarded(vtbl, name, sizeof(name), bases)) ci.source = ClassSource::Msvc;
		else
		{
			name[0] = 0;
			bases = 0;
			if (nirtti_guarded(vtbl, name, sizeof(name), bases)) ci.source = ClassSource::NiRtti;
		}
		if (ci.source != ClassSource::None)
		{
			ci.name = name;
			// A class is also its own base (NiRTTI's chain starts with it; the
			// MSVC hierarchy lists it first).
			bases |= base_bit(name);
			ci.node = (bases & kBaseNode) != 0;
			ci.geometry = (bases & kBaseGeometry) != 0;
			ci.excluded_subtree = (bases & (kBaseTree | kBaseBillboard | kBaseLod | kBaseSwitch | kBaseRange)) != 0;
			ci.subtree_tree = (bases & (kBaseTree | kBaseBillboard)) != 0;
			ci.subtree_switch = (bases & (kBaseLod | kBaseSwitch | kBaseRange)) != 0;
			ci.subtree_exclusive = (bases & (kBaseLod | kBaseSwitch)) != 0;
			ci.shade_water = (bases & kBaseWaterShader) != 0;
			ci.shade_property = (bases & kBaseShaderProperty) != 0;
			ci.segmented = ci.name == "BSSegmentedTriShape";
			ci.source_texture = (bases & kBaseSourceTexture) != 0;
			if (ci.geometry)
				ci.leaf = ci.name == "NiTriShape" ? Leaf::TriShape : ci.name == "NiTriStrips" ? Leaf::TriStrips
					: Leaf::OtherGeometry;
			ci.shade_ppl = ci.name == "BSShaderPPLightingProperty" || ci.name == "Lighting30ShaderProperty";
			ci.cell = ci.name == "TESObjectCELL";
			ci.worldspace = ci.name == "TESWorldSpace";
		}
		else ci.name = "?";
		return &g_classes.emplace(vtbl, std::move(ci)).first->second;
	}

	const ClassInfo* class_of(uintptr_t object)
	{
		if (!plausible(object)) return nullptr;
		uintptr_t vtbl = 0;
		if (!guarded_copy(object, &vtbl, sizeof(vtbl))) return nullptr;
		return class_of_vtbl(vtbl);
	}

	// ---- Reads ----

	bool read_block(uintptr_t addr, void* out, size_t n)
	{
		return plausible(addr) && guarded_copy(addr, out, n);
	}

	bool read_u8(uintptr_t addr, uint8_t& out) { return addr >= 0x10000 && guarded_copy(addr, &out, 1); }
	bool read_u16(uintptr_t addr, uint16_t& out) { return addr >= 0x10000 && guarded_copy(addr, &out, 2); }
	bool read_u32(uintptr_t addr, uint32_t& out) { return read_block(addr, &out, 4); }
	bool read_f32(uintptr_t addr, float& out) { return read_block(addr, &out, 4); }

	bool read_av(uintptr_t obj, AvRead& out)
	{
		uint8_t b[0x9C];
		if (!read_block(obj, b, sizeof(b))) return false;
		std::memcpy(&out.vtbl, b, 4);
		std::memcpy(&out.controller, b + fnv::OBJ_controller, 4);
		std::memcpy(&out.parent, b + fnv::AV_parent, 4);
		std::memcpy(&out.flags, b + fnv::AV_flags, 4);
		std::memcpy(out.rot, b + fnv::AV_worldRot, sizeof(out.rot));
		std::memcpy(out.pos, b + fnv::AV_worldPos, sizeof(out.pos));
		std::memcpy(&out.scale, b + fnv::AV_worldScale, 4);
		return true;
	}

	bool read_pose(uintptr_t obj, float (&out)[kPoseFloats])
	{
		static_assert(fnv::AV_worldPos == fnv::AV_worldRot + 9 * 4 && fnv::AV_worldScale == fnv::AV_worldPos + 3 * 4);
		return read_block(obj + fnv::AV_worldRot, out, sizeof(out));
	}

	bool read_link(uintptr_t obj, uintptr_t& parent, uint32_t& flags)
	{
		uint8_t b[fnv::AV_flags + 4 - fnv::AV_parent];
		if (!read_block(obj + fnv::AV_parent, b, sizeof(b))) return false;
		uint32_t pr = 0;
		std::memcpy(&pr, b, 4);
		std::memcpy(&flags, b + (fnv::AV_flags - fnv::AV_parent), 4);
		parent = pr;
		return true;
	}

	bool read_children(uintptr_t node, uintptr_t* out, uint32_t max, NodeChildren& info)
	{
		uint8_t b[0x0C];
		if (!read_block(node + fnv::NODE_childData, b, sizeof(b))) return false;
		uintptr_t data = 0;
		std::memcpy(&data, b, 4);
		std::memcpy(&info.cap, b + (fnv::NODE_childCap - fnv::NODE_childData), 2);
		std::memcpy(&info.count, b + (fnv::NODE_childCount - fnv::NODE_childData), 2);
		info.read = 0;
		const uint32_t n = (std::min)({ static_cast<uint32_t>(info.count), static_cast<uint32_t>(info.cap), max });
		if (!n) return true;
		if (!read_block(data, out, n * sizeof(uintptr_t))) return false;
		info.read = n;
		return true;
	}

	bool read_geo(uintptr_t geo, GeoRead& out)
	{
		uint8_t b[0xC4 - fnv::GEO_props];
		if (!read_block(geo + fnv::GEO_props, b, sizeof(b))) return false;
		std::memcpy(out.props, b, sizeof(out.props));
		std::memcpy(&out.data, b + (fnv::GEO_data - fnv::GEO_props), 4);
		std::memcpy(&out.skin, b + (fnv::GEO_skin - fnv::GEO_props), 4);
		std::memcpy(&out.shader, b + (fnv::GEO_shader - fnv::GEO_props), 4);
		return true;
	}

	bool read_data(uintptr_t ngd, DataRead& out)
	{
		uint8_t b[0x50];
		if (!read_block(ngd, b, sizeof(b))) return false;
		std::memcpy(&out.vtbl, b, 4);
		std::memcpy(&out.verts, b + fnv::NGD_numVerts, 2);
		std::memcpy(&out.id, b + fnv::NGD_id, 2);
		std::memcpy(&out.pos, b + fnv::NGD_pos, 4);
		std::memcpy(&out.nrm, b + fnv::NGD_normal, 4);
		std::memcpy(&out.col, b + fnv::NGD_color, 4);
		std::memcpy(&out.uv, b + fnv::NGD_uv, 4);
		std::memcpy(&out.packed, b + fnv::NGD_buffer, 4);
		out.keep = b[fnv::NGD_keepFlags];
		out.compress = b[fnv::NGD_compressFlags];
		std::memcpy(&out.tris, b + fnv::TBGD_numTris, 2);
		std::memcpy(&out.f44, b + fnv::TSD_indexCount, 4);
		std::memcpy(&out.f48, b + fnv::TSD_indices, 4);
		std::memcpy(&out.f4c, b + fnv::TSTD_stripIdx, 4);
		return true;
	}

	bool read_buffer(uintptr_t bd, BufferRead& out)
	{
		out = {};
		uint8_t b[0x54];
		if (!read_block(bd, b, sizeof(b))) return false;
		out.bd = bd;
		auto u32 = [&b](uint32_t off) { uint32_t v; std::memcpy(&v, b + off, 4); return v; };
		out.flags = u32(fnv::BD_flags);
		out.fvf = u32(fnv::BD_fvf);
		out.decl = reinterpret_cast<IDirect3DVertexDeclaration9*>(static_cast<uintptr_t>(u32(fnv::BD_decl)));
		out.verts = u32(fnv::BD_vertCount);
		out.streams = u32(fnv::BD_streamCount);
		out.index_count = u32(fnv::BD_indexCount);
		out.ib = reinterpret_cast<IDirect3DIndexBuffer9*>(static_cast<uintptr_t>(u32(fnv::BD_ib)));
		out.base_vertex = u32(fnv::BD_baseVertex);
		out.prim_type = u32(fnv::BD_primType);
		out.tris = u32(fnv::BD_triCount);
		if (!out.streams || out.streams > fnv::kMaxStreams) return false;
		uint32_t strides[fnv::kMaxStreams] = {}, chips[fnv::kMaxStreams] = {};
		if (!read_block(u32(fnv::BD_strides), strides, out.streams * 4) ||
			!read_block(u32(fnv::BD_chips), chips, out.streams * 4))
			return false;
		for (uint32_t s = 0; s < out.streams; s++)
		{
			out.stride[s] = strides[s];
			uint32_t vb = 0;
			if (!chips[s] || !read_u32(chips[s] + fnv::CHIP_vb, vb) || !read_u32(chips[s] + fnv::CHIP_offset, out.chip_offset[s]))
				return false;
			out.vb[s] = reinterpret_cast<IDirect3DVertexBuffer9*>(static_cast<uintptr_t>(vb));
		}
		return true;
	}

	namespace
	{
		// GetDeclaration of the bridge client's declaration (answered from
		// its own copy, no IPC). SEH-guarded: the pointer comes from engine
		// memory.
		bool decl_elements_unguarded(IDirect3DVertexDeclaration9* decl, D3DVERTEXELEMENT9* out, UINT& count)
		{
			UINT n = 0;
			if (FAILED(decl->GetDeclaration(nullptr, &n)) || n == 0 || n > MAXD3DDECLLENGTH + 1) return false;
			if (FAILED(decl->GetDeclaration(out, &n))) return false;
			count = n;
			return true;
		}

		bool decl_elements_guarded(IDirect3DVertexDeclaration9* decl, D3DVERTEXELEMENT9* out, UINT& count)
		{
			__try { return decl_elements_unguarded(decl, out, count); }
			__except (EXCEPTION_EXECUTE_HANDLER) { note_fault(reinterpret_cast<uintptr_t>(decl)); return false; }
		}
	}

	bool buffer_layout(const BufferRead& b, D3DVERTEXELEMENT9 (&out)[MAXD3DDECLLENGTH + 1], UINT& count)
	{
		count = 0;
		if (b.decl && plausible(reinterpret_cast<uintptr_t>(b.decl)))
		{
			if (!decl_elements_guarded(b.decl, out, count)) return false;
		}
		else if (b.fvf)
		{
			if (FAILED(D3DXDeclaratorFromFVF(b.fvf, out))) return false;
			count = 0;
			while (count <= MAXD3DDECLLENGTH && out[count].Stream != 0xFF) count++;
			count++;   // with D3DDECL_END, like GetDeclaration
		}
		else return false;
		// Normalise: the count includes the terminator.
		if (!count || out[count - 1].Stream != 0xFF) return false;
		return true;
	}

	bool read_ref(uintptr_t ref, RefRead& out)
	{
		uint8_t b[0x68];
		if (!read_block(ref, b, sizeof(b))) return false;
		std::memcpy(&out.vtbl, b, 4);
		out.type = b[fnv::FORM_type];
		std::memcpy(&out.flags, b + fnv::FORM_flags, 4);
		std::memcpy(&out.ref_id, b + fnv::FORM_refID, 4);
		std::memcpy(&out.base, b + fnv::REFR_baseForm, 4);
		std::memcpy(&out.cell, b + fnv::REFR_parentCell, 4);
		uintptr_t rs = 0;
		std::memcpy(&rs, b + fnv::REFR_renderState, 4);
		out.base_type = 0;
		if (out.base) read_u8(out.base + fnv::FORM_type, out.base_type);
		out.root = 0;
		uint32_t root = 0;
		if (rs && read_u32(rs + fnv::RS_niNode, root)) out.root = root;
		return true;
	}

	uintptr_t ppl_texture(uintptr_t prop, int role)
	{
		if (role < 0 || role >= fnv::kRoleCount) return 0;
		uint16_t layers = 0;
		uint32_t arr = 0, tex = 0;
		if (!read_u16(prop + fnv::PPL_layerCount, layers) || !layers) return 0;
		if (!read_u32(prop + fnv::PPL_texArrays + 4 * role, arr) || !arr) return 0;
		if (!read_u32(arr, tex)) return 0;
		return tex;
	}

	bool read_clamp_mode(uintptr_t shade, uint32_t& mode, const char*& cls)
	{
		uint32_t vtbl = 0;
		if (!shade || !read_u32(shade, vtbl)) return false;
		uint32_t off = 0;
		const char* name = nullptr;
		const auto& addr = fnv_exe::addr();
		if (!vtbl) return false;
		if (vtbl == addr.vtbl_ppl_shade) { off = fnv::PPL_clampMode; name = "BSShaderPPLightingProperty"; }
		else if (vtbl == addr.vtbl_lighting30_shade) { off = fnv::PPL_clampMode; name = "Lighting30ShaderProperty"; }
		else if (vtbl == addr.vtbl_no_lighting_shade) { off = fnv::NOLIGHT_clampMode; name = "BSShaderNoLightingProperty"; }
		else return false;
		uint32_t v = 0;
		if (!read_u32(shade + off, v)) return false;
		mode = v;
		cls = name;
		return true;
	}

	bool read_shade_flags(uintptr_t shade, ShadeFlags& out)
	{
		out = {};
		const ClassInfo* ci = shade ? class_of(shade) : nullptr;
		if (!ci || !ci->shade_property) return false;
		out.cls = ci->name.c_str();
		if (!read_u32(shade + fnv::SHADE_flags1, out.flags1) || !read_u32(shade + fnv::SHADE_flags2, out.flags2)) return false;
		out.ok = true;
		return true;
	}

	IDirect3DBaseTexture9* d3d_texture(uintptr_t nitex, bool& has_renderer_data)
	{
		has_renderer_data = false;
		uint32_t rd = 0, d3d = 0;
		if (!nitex || !read_u32(nitex + fnv::TEX_rendererData, rd) || !rd) return nullptr;
		has_renderer_data = true;
		if (!read_u32(rd + fnv::DX9TEX_d3d, d3d)) return nullptr;
		return reinterpret_cast<IDirect3DBaseTexture9*>(static_cast<uintptr_t>(d3d));
	}

	bool texture_name(uintptr_t nitex, char* out, size_t cap)
	{
		out[0] = 0;
		const ClassInfo* ci = class_of(nitex);
		if (!ci || !ci->source_texture) return false;
		uint32_t str = 0;
		if (!read_u32(nitex + fnv::SRCTEX_fileName, str) || str < 0x10000) return false;
		return name_guarded(str, out, cap);
	}

	uint32_t faults() { return g_faults.load(std::memory_order_relaxed); }

	FaultInfo last_fault()
	{
		FaultInfo f;
		f.count = g_faults.load(std::memory_order_relaxed);
		f.site = g_fault_site;
		f.addr = g_fault_addr;
		return f;
	}

	FaultSite::FaultSite(const char* site) : prev(g_site) { g_site = site; }
	FaultSite::~FaultSite() { g_site = prev; }

	bool mapped(uintptr_t addr, size_t n)
	{
		if (!plausible(addr) || !n) return false;
		MEMORY_BASIC_INFORMATION mbi = {};
		if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return false;
		if (mbi.State != MEM_COMMIT) return false;
		constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
			PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if (!(mbi.Protect & kReadable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
		const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		return addr + n <= end;
	}

	// ---- Hooks ----

	bool install_hooks()
	{
		if (g_hooks_tried) return g_hooks_ok;
		g_hooks_tried = true;
		note_render_thread();
		if (!fnv_exe::known())
		{
			shared::common::log("SceneGraph", "hooks SKIPPED: unknown executable. The scene-graph features stay off.",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return false;
		}
		const auto& addr = fnv_exe::addr();
		const bool dispatch_ok = call_chain::hookable(addr.render_pass_dispatch, "SceneGraph", "render-pass dispatcher");
		const bool dtor_ok = call_chain::hookable(addr.ni_geometry_dtor, "SceneGraph", "~NiGeometry");
		if (!dispatch_ok || !dtor_ok) return false;
		// Destructor first: the pass filter relies on hearing every death.
		if (!call_chain::hook_entry(addr.ni_geometry_dtor, reinterpret_cast<void*>(&dtor_hk),
				reinterpret_cast<void**>(&g_orig_dtor), "SceneGraph", "~NiGeometry", kIfReplaced))
			return false;
		if (!call_chain::hook_entry(addr.render_pass_dispatch, reinterpret_cast<void*>(&dispatch_hk),
				reinterpret_cast<void**>(&g_orig_dispatch), "SceneGraph", "render-pass dispatcher", kIfReplaced))
		{
			shared::common::log("SceneGraph", "render-pass dispatcher detour failed; the destructor hook stays (it only "
				"records)", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return false;
		}
		g_hooks_ok = true;
		shared::common::log("SceneGraph", std::format("hooked the render-pass dispatcher @0x{:X} and ~NiGeometry @0x{:X}",
			addr.render_pass_dispatch.addr, addr.ni_geometry_dtor.addr),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, false);
		return true;
	}

	bool hooks_installed() { return g_hooks_ok; }

	bool install_grass_hooks()
	{
		if (g_grass_hooks_tried) return g_grass_hooks_ok;
		g_grass_hooks_tried = true;
		if (!fnv_exe::known()) return false;
		// Vanilla prologues:
		//   append     movzx eax, word [ecx+12h]; cmp ax, [ecx+10h]
		//   remove_at  movzx eax, word [ecx+12h]; mov dx, [esp+4]
		//   destroy    push -1; push 0F2D249h; mov eax, fs:[0]  (SEH frame)
		const auto& addr = fnv_exe::addr();
		const bool a = call_chain::hookable(addr.grass_batch_append, "SceneGraph", "grass batch append");
		const bool r = call_chain::hookable(addr.grass_batch_remove_at, "SceneGraph", "grass batch remove");
		const bool d = call_chain::hookable(addr.grass_batch_destroy, "SceneGraph", "grass batch destroy");
		if (!a || !r || !d) return false;
		// Destroy first: once changes are tracked, a batch's end must never
		// be missed.
		if (!call_chain::hook_entry(addr.grass_batch_destroy, reinterpret_cast<void*>(&grass_destroy_hk),
				reinterpret_cast<void**>(&g_orig_grass_destroy), "SceneGraph", "grass batch destroy", kIfReplaced))
			return false;
		if (!call_chain::hook_entry(addr.grass_batch_append, reinterpret_cast<void*>(&grass_append_hk),
				reinterpret_cast<void**>(&g_orig_grass_append), "SceneGraph", "grass batch append", kIfReplaced) ||
			!call_chain::hook_entry(addr.grass_batch_remove_at, reinterpret_cast<void*>(&grass_remove_hk),
				reinterpret_cast<void**>(&g_orig_grass_remove), "SceneGraph", "grass batch remove", kIfReplaced))
		{
			shared::common::log("SceneGraph", "grass batch append / remove detour failed; the destroy hook stays (it only "
				"records)", shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return false;
		}
		g_grass_hooks_ok = true;
		shared::common::log("SceneGraph", std::format("hooked the grass batch append @0x{:X}, remove @0x{:X} and destroy "
			"@0x{:X}", addr.grass_batch_append.addr, addr.grass_batch_remove_at.addr, addr.grass_batch_destroy.addr),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, false);
		return true;
	}

	bool grass_hooks_installed() { return g_grass_hooks_ok; }

	uint64_t grass_generation(uintptr_t geometry)
	{
		if (!g_grass_hooks_ok) return 0;
		AcquireSRWLockShared(&g_grass_lock);
		const auto it = g_grass_gen.find(geometry);
		const uint64_t g = it != g_grass_gen.end() ? it->second : 0;
		ReleaseSRWLockShared(&g_grass_lock);
		return g;
	}

	void take_grass_events(std::vector<uintptr_t>& changed, std::vector<uintptr_t>& destroyed)
	{
		changed.clear();
		destroyed.clear();
		if (!g_grass_hooks_tried) return;
		AcquireSRWLockExclusive(&g_grass_lock);
		changed.swap(g_grass_changed);
		destroyed.swap(g_grass_destroyed);
		g_grass_changed_set.clear();
		// A destroyed batch's geometry address may belong to a new object
		// from now on: its entry goes (the counter never repeats a value, so
		// a new batch at the address still gets fresh generations).
		for (const uintptr_t geo : destroyed) g_grass_gen.erase(geo);
		ReleaseSRWLockExclusive(&g_grass_lock);
	}

	bool is_grass_geometry(uintptr_t geometry)
	{
		uint32_t vtbl = 0;
		if (!read_u32(geometry, vtbl)) return false;
		const auto& addr = fnv_exe::addr();
		return vtbl && (vtbl == addr.vtbl_grass_tri_strips || vtbl == addr.vtbl_grass_tri_shape);
	}

	GrassHookStats grass_hook_stats()
	{
		GrassHookStats s;
		s.appends = g_grass_appends.load(std::memory_order_relaxed);
		s.removes = g_grass_removes.load(std::memory_order_relaxed);
		s.destroys = g_grass_destroys.load(std::memory_order_relaxed);
		return s;
	}

	uintptr_t current_geometry() { return g_current_geometry; }

	uint16_t current_pass_type() { return g_current_pass_type; }

	void set_pass_filter(PassFilter filter) { g_filter = filter; }

	uint32_t skipped_passes_take() { return std::exchange(g_skipped, 0u); }

	void take_deaths(std::vector<uintptr_t>& out)
	{
		out.clear();
		if (!g_dead_count.load(std::memory_order_acquire)) return;
		AcquireSRWLockExclusive(&g_dead_lock);
		out.swap(g_dead);
		g_dead_set.clear();
		g_dead_count.store(0, std::memory_order_release);
		ReleaseSRWLockExclusive(&g_dead_lock);
	}

	bool death_pending(uintptr_t geometry)
	{
		if (!g_dead_count.load(std::memory_order_acquire)) return false;
		AcquireSRWLockShared(&g_dead_lock);
		const bool found = g_dead_set.count(geometry) != 0;
		ReleaseSRWLockShared(&g_dead_lock);
		return found;
	}

	bool begin_resolve(uintptr_t geometry)
	{
		if (!geometry || !g_hooks_ok) return true;
		g_resolving.store(geometry, std::memory_order_seq_cst);
		bool dead = false;
		if (g_dead_count.load(std::memory_order_seq_cst) != 0)
		{
			AcquireSRWLockShared(&g_dead_lock);
			dead = g_dead_set.count(geometry) != 0;
			ReleaseSRWLockShared(&g_dead_lock);
		}
		if (dead)
		{
			g_resolving.store(0, std::memory_order_seq_cst);
			g_resolves_skipped_dead.fetch_add(1, std::memory_order_relaxed);
		}
		return !dead;
	}

	void end_resolve()
	{
		g_resolving.store(0, std::memory_order_seq_cst);
	}

	HookStats hook_stats()
	{
		HookStats s;
		s.deaths = g_deaths.load(std::memory_order_relaxed);
		s.deaths_off_thread = g_deaths_off_thread.load(std::memory_order_relaxed);
		s.passes_off_thread = g_passes_off_thread.load(std::memory_order_relaxed);
		s.resolve_waits = g_resolve_waits.load(std::memory_order_relaxed);
		s.resolve_wait_timeouts = g_resolve_wait_timeouts.load(std::memory_order_relaxed);
		s.resolves_skipped_dead = g_resolves_skipped_dead.load(std::memory_order_relaxed);
		return s;
	}

	void note_render_thread()
	{
		DWORD expected = 0;
		g_render_thread.compare_exchange_strong(expected, GetCurrentThreadId(), std::memory_order_relaxed);
	}
}
