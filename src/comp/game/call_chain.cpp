#include "std_include.hpp"
#include "call_chain.hpp"
#include "shared/fnv_remix_plugin_api.h"

// See call_chain.hpp.

namespace comp::game::call_chain
{
	namespace
	{
		constexpr UINT kCheckWindow = 600;

		uintptr_t rel32_target(const uint8_t* p)
		{
			int32_t rel = 0;
			std::memcpy(&rel, p + 1, sizeof(rel));
			return reinterpret_cast<uintptr_t>(p) + 5 + static_cast<uintptr_t>(rel);
		}

		// True when `n` bytes at `addr` are committed, readable memory in one region.
		bool readable(uintptr_t addr, size_t n)
		{
			MEMORY_BASIC_INFORMATION mbi = {};
			if (!addr || !VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi))) return false;
			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
			constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
				PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (!(mbi.Protect & kRead)) return false;
			return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		}

		bool read_u32(uintptr_t addr, uint32_t& out)
		{
			if (!readable(addr, 4)) return false;
			std::memcpy(&out, reinterpret_cast<const void*>(addr), 4);
			return true;
		}

		// Copies up to `cap` bytes from `addr`, fewer at the end of a region.
		size_t copy(uintptr_t addr, uint8_t* out, size_t cap)
		{
			size_t n = cap;
			while (n && !readable(addr, n)) --n;
			if (n) std::memcpy(out, reinterpret_cast<const void*>(addr), n);
			return n;
		}

		// A function entry that is not the engine's own code: another
		// module's detour into executable code, or foreign.
		void classify_detour(Site& s, const uint8_t* p, size_t n, uintptr_t at)
		{
			const hook_forms::Decoded d = hook_forms::decode(p, n, at, &read_u32);
			s.form = d.form;
			if (!hook_forms::is_detour(d.form) || !executable(d.target)) return;
			s.state = State::Chained;
			s.target = d.target;
			s.owner = owner_of(d.target);
		}

		void log_entry(const char* tag, uintptr_t at, const char* what, const Site& s, const fnv_exe::Code* code)
		{
			const std::string engine = code ? std::format(" instead of the engine's [{}]", hex(code->bytes, code->len))
				: std::string();
			if (s.state == State::Chained)
				shared::common::log(tag, std::format("0x{:X} ({}) starts with a {} to 0x{:X} in {} [{}]{}: another mod "
					"hooked it; the wrapper's hook goes in front and continues into that mod's code.", at, what,
					hook_forms::name(s.form), s.target, s.owner, s.bytes, engine),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			else if (s.state == State::Foreign)
				shared::common::log(tag, std::format("0x{:X} ({}) holds [{}], expected {}or another mod's jump into "
					"executable code -- another mod rewrote it, so the wrapper leaves it alone.", at, what, s.bytes,
					code ? std::format("the engine's [{}] ", hex(code->bytes, code->len)) : std::string()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		bool hook_entry_at(uintptr_t at, const fnv_exe::Code* code, void* detour, void** original, const char* tag,
			const char* what, const char* if_replaced)
		{
			const Site s = code ? inspect_entry(*code) : inspect_entry(at);
			log_entry(tag, at, what, s, code);
			if (s.state == State::Foreign) return false;
			auto* target = reinterpret_cast<LPVOID>(at);
			MH_STATUS st = MH_CreateHook(target, detour, original);
			if (st == MH_OK) st = MH_EnableHook(target);
			if (st != MH_OK)
			{
				shared::common::log(tag, std::format("0x{:X} ({}): MinHook could not hook it ({}) -- the wrapper leaves "
					"it alone.", at, what, MH_StatusToString(st)), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}
			track(Kind::Entry, at, reinterpret_cast<uintptr_t>(detour), s, tag, what, if_replaced);
			return true;
		}

		struct Tracked
		{
			Kind kind;
			uintptr_t at;
			uintptr_t ours;
			uintptr_t previous;
			bool chained;
			std::string previous_owner;
			const char* tag;
			const char* what;
			const char* if_replaced;
			bool reported;
		};

		// Written by the installers (the window thread, the render thread),
		// read by on_present and the plugin's report.
		SRWLOCK g_lock = SRWLOCK_INIT;
		std::vector<Tracked> g_tracked;
		UINT g_window = 0;

		// Where a tracked place leads now, when that is executable code; 0 otherwise.
		uintptr_t leads_to(const Tracked& t, const uint8_t* p, size_t n)
		{
			uintptr_t to = 0;
			if (t.kind == Kind::Slot && n >= 4) std::memcpy(&to, p, 4);
			else if (t.kind == Kind::Entry || t.kind == Kind::Call)
			{
				const hook_forms::Decoded d = hook_forms::decode(p, n, t.at, &read_u32);
				if (t.kind == Kind::Entry ? hook_forms::is_detour(d.form) : d.form == hook_forms::Form::CallRel32)
					to = d.target;
			}
			return to && executable(to) ? to : 0;
		}

		void check(Tracked& t)
		{
			if (t.reported) return;
			uint8_t p[8] = {};
			const size_t n = copy(t.at, p, t.kind == Kind::Byte ? 1 : sizeof(p));
			if (!n) return;
			const uintptr_t now = leads_to(t, p, n);
			const bool ours = t.kind == Kind::Byte ? p[0] == static_cast<uint8_t>(t.ours) : now == t.ours;
			if (ours) return;
			t.reported = true;
			const size_t shown = t.kind == Kind::Byte ? 1 : t.kind == Kind::Slot ? 4 : 5;
			if (now)
				shared::common::log(t.tag, std::format("0x{:X} ({}) now leads to 0x{:X} in {} [{}]: another module "
					"patched it after the wrapper. {}", t.at, t.what, now, owner_of(now), hex(p, (std::min)(n, shown)),
					t.if_replaced), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
			else
				shared::common::log(t.tag, std::format("0x{:X} ({}) no longer holds the wrapper's {}: it holds [{}]. {}",
					t.at, t.what, t.kind == Kind::Slot ? "pointer" : t.kind == Kind::Byte ? "byte" : "hook",
					hex(p, (std::min)(n, shown)), t.if_replaced), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}
	}

	bool executable(uintptr_t addr)
	{
		MEMORY_BASIC_INFORMATION mbi = {};
		if (!addr || !VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi))) return false;
		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
		return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
	}

	std::string hex(const uint8_t* p, size_t n)
	{
		std::string s;
		for (size_t i = 0; i < n; i++) s += std::format("{}{:02X}", i ? " " : "", p[i]);
		return s;
	}

	std::string owner_of(uintptr_t addr)
	{
		HMODULE m = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(addr), &m) || !m)
			return "allocated memory";
		char path[MAX_PATH] = {};
		const DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
		const std::string p(path, n);
		const size_t slash = p.find_last_of("\\/");
		return slash == std::string::npos ? p : p.substr(slash + 1);
	}

	Site inspect(const fnv_exe::Code& code)
	{
		Site s;
		if (!code.addr || !readable(code.addr, 5)) return s;
		const auto* p = reinterpret_cast<const uint8_t*>(code.addr);
		s.bytes = hex(p, 5);
		if (code.len != 5 || code.bytes[0] != 0xE8) return s;
		if (p[0] == 0xE8) s.form = hook_forms::Form::CallRel32;
		if (std::memcmp(p, code.bytes, 5) == 0)
		{
			s.state = State::Vanilla;
			s.target = rel32_target(p);
			s.owner = owner_of(s.target);
			return s;
		}
		if (p[0] != 0xE8) return s;
		const uintptr_t target = rel32_target(p);
		if (!executable(target)) return s;
		s.state = State::Chained;
		s.target = target;
		s.owner = owner_of(target);
		return s;
	}

	Site inspect_entry(const fnv_exe::Code& code)
	{
		Site s;
		if (!code.addr || !code.len) return s;
		uint8_t p[sizeof(code.bytes)] = {};
		const size_t n = copy(code.addr, p, sizeof(p));
		if (n < code.len) return s;
		s.bytes = hex(p, code.len);
		if (std::memcmp(p, code.bytes, code.len) == 0)
		{
			s.state = State::Vanilla;
			s.target = code.addr;
			s.owner = owner_of(code.addr);
			return s;
		}
		classify_detour(s, p, n, code.addr);
		return s;
	}

	Site inspect_entry(uintptr_t addr)
	{
		Site s;
		uint8_t p[16] = {};
		const size_t n = copy(addr, p, sizeof(p));
		if (n < 5) return s;
		s.bytes = hex(p, (std::min)(n, size_t{ 8 }));
		const hook_forms::Decoded d = hook_forms::decode(p, n, addr, &read_u32);
		if (!hook_forms::is_detour(d.form))
		{
			s.state = State::Vanilla;
			s.target = addr;
			s.owner = owner_of(addr);
			return s;
		}
		classify_detour(s, p, n, addr);
		return s;
	}

	uint8_t* emit_rel32(uint8_t* p, uint8_t op, uintptr_t to)
	{
		*p++ = op;
		const int32_t rel = static_cast<int32_t>(to - reinterpret_cast<uintptr_t>(p + 4));
		std::memcpy(p, &rel, sizeof(rel));
		return p + 4;
	}

	bool patch_rel32(uintptr_t at, uint8_t op, uintptr_t to)
	{
		auto* p = reinterpret_cast<uint8_t*>(at);
		DWORD old = 0;
		if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
		emit_rel32(p, op, to);
		VirtualProtect(p, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
		return true;
	}

	bool patch_byte(uintptr_t at, uint8_t value)
	{
		auto* p = reinterpret_cast<uint8_t*>(at);
		DWORD old = 0;
		if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
		*p = value;
		VirtualProtect(p, 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 1);
		return true;
	}

	void log_site(const char* tag, const fnv_exe::Code& code, const char* what, const Site& s)
	{
		if (s.state == State::Chained)
			shared::common::log(tag, std::format("0x{:X} ({}) already calls 0x{:X} in {} [{}] instead of the engine's "
				"[{}]: another mod hooked it; the wrapper's hook goes in front and calls that mod's code.",
				code.addr, what, s.target, s.owner, s.bytes, hex(code.bytes, code.len)),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		else if (s.state == State::Foreign)
			shared::common::log(tag, std::format("0x{:X} ({}) holds [{}], expected the engine's call [{}] or another "
				"mod's call into executable code -- another mod rewrote it, so the wrapper leaves it alone.",
				code.addr, what, s.bytes, hex(code.bytes, code.len)), shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
	}

	bool hook_entry(const fnv_exe::Code& code, void* detour, void** original, const char* tag, const char* what,
		const char* if_replaced)
	{
		return code.addr && hook_entry_at(code.addr, &code, detour, original, tag, what, if_replaced);
	}

	bool hookable(const fnv_exe::Code& code, const char* tag, const char* what)
	{
		const Site s = inspect_entry(code);
		if (s.state == State::Foreign) log_entry(tag, code.addr, what, s, &code);
		return s.state != State::Foreign;
	}

	bool hook_entry(uintptr_t addr, void* detour, void** original, const char* tag, const char* what,
		const char* if_replaced)
	{
		return addr && hook_entry_at(addr, nullptr, detour, original, tag, what, if_replaced);
	}

	uint8_t* emit_call_wrapper(uint8_t* at, ReturnStack* rs, uintptr_t target, std::initializer_list<Store> before,
		std::initializer_list<Store> after)
	{
		const auto addr32 = [](const volatile void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); };
		const auto put32 = [&](uint32_t v) { std::memcpy(at, &v, 4); at += 4; };
		const uint32_t saved = addr32(&rs->saved_eax), depth = addr32(&rs->depth), slots = addr32(rs->slots);
		const auto stores = [&](std::initializer_list<Store> list)
		{
			for (const Store& s : list)
			{
				*at++ = 0xC7; *at++ = 0x05;   // mov dword [flag], value
				put32(addr32(s.flag));
				put32(static_cast<uint32_t>(s.value));
			}
		};

		// The site's return address moves from the stack to slots[depth], so
		// the target finds its arguments where the site left them.
		*at++ = 0xA3; put32(saved);                             // mov [saved_eax], eax
		*at++ = 0xA1; put32(depth);                             // mov eax, [depth]
		*at++ = 0x8F; *at++ = 0x04; *at++ = 0x85; put32(slots); // pop dword [slots + eax*4]
		*at++ = 0xFF; *at++ = 0x05; put32(depth);               // inc dword [depth]
		*at++ = 0xA1; put32(saved);                             // mov eax, [saved_eax]
		stores(before);
		at = emit_rel32(at, 0xE8, target);                      // call target
		stores(after);
		*at++ = 0xA3; put32(saved);                             // mov [saved_eax], eax
		*at++ = 0xFF; *at++ = 0x0D; put32(depth);               // dec dword [depth]
		*at++ = 0xA1; put32(depth);                             // mov eax, [depth]
		*at++ = 0xFF; *at++ = 0x34; *at++ = 0x85; put32(slots); // push dword [slots + eax*4]
		*at++ = 0xA1; put32(saved);                             // mov eax, [saved_eax]
		*at++ = 0xC3;                                           // ret
		return at;
	}

	void track(Kind kind, uintptr_t at, uintptr_t ours, const Site& previous, const char* tag, const char* what,
		const char* if_replaced)
	{
		AcquireSRWLockExclusive(&g_lock);
		g_tracked.push_back({ kind, at, ours, previous.target, previous.state == State::Chained, previous.owner, tag,
			what, if_replaced, false });
		ReleaseSRWLockExclusive(&g_lock);
	}

	void on_present()
	{
		if (g_window == 0)
		{
			AcquireSRWLockExclusive(&g_lock);
			for (Tracked& t : g_tracked) check(t);
			ReleaseSRWLockExclusive(&g_lock);
		}
		if (++g_window >= kCheckWindow) g_window = 0;
	}
}

// The wrapper's hooks for the engine monitor's integrity report
// (shared/fnv_remix_plugin_api.h). Pointer slots are not code sites and are
// left out.
extern "C" uint32_t FnvRemix_HookChain(FnvRemixHook* out, uint32_t max)
{
	using namespace comp::game::call_chain;
	AcquireSRWLockShared(&g_lock);
	uint32_t count = 0;
	for (const Tracked& t : g_tracked)
	{
		if (t.kind == Kind::Slot) continue;
		if (out && count < max)
		{
			FnvRemixHook& h = out[count];
			h = {};
			h.site = t.at;
			h.ours = t.ours;
			h.previous = t.previous;
			h.chained = t.chained ? 1 : 0;
			strncpy_s(h.previous_owner, t.previous_owner.c_str(), _TRUNCATE);
		}
		++count;
	}
	ReleaseSRWLockShared(&g_lock);
	return count;
}
