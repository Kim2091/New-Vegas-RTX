// Standalone test of comp/game/call_chain: hooks dummy functions that
// already carry another module's detour (E9, FF 25, push/ret, mov/jmp) and
// checks that the wrapper's hook runs first, then the other module's, then
// the original. Also checks a hook added after the wrapper's, foreign
// prologues that must be left alone, the call-site wrapper thunk (nested
// calls, callee-cleaned arguments, a chained target), the Present checks
// and the record list the engine monitor reads. Built by build_test.bat;
// not part of the shipped DLLs.

#include "std_include.hpp"
#include "comp/game/call_chain.hpp"
#include "shared/fnv_remix_plugin_api.h"

#include <cstdio>

extern "C" uint32_t FnvRemix_HookChain(FnvRemixHook* out, uint32_t max);

namespace cc = comp::game::call_chain;

// ---- Log capture -------------------------------------------------------------

namespace
{
	std::vector<std::string> g_log;
}

namespace shared::common
{
	void log(const std::string_view& module_str, const std::string_view& msg, LOG_TYPE type, bool)
	{
		const char* kind = type == LOG_TYPE::LOG_TYPE_WARN ? "WARN" : type == LOG_TYPE::LOG_TYPE_DEFAULT ? "INFO" : "OTHER";
		g_log.push_back(std::format("[{}] {}: {}", kind, module_str, msg));
		std::printf("    log %s\n", g_log.back().c_str());
	}
}

namespace
{
	int g_failures = 0;
	std::string g_order;

	void expect(bool ok, const std::string& what)
	{
		std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
		if (!ok) ++g_failures;
	}

	size_t log_lines_with(const char* a, const char* b = "")
	{
		size_t n = 0;
		for (const auto& l : g_log)
			if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) ++n;
		return n;
	}

	extern "C" void __cdecl record(int c)
	{
		g_order += static_cast<char>(c);
	}

	// ---- Executable memory ---------------------------------------------------

	uint8_t* g_page = nullptr;
	size_t g_used = 0;

	uint8_t* alloc_code(size_t n)
	{
		if (!g_page)
			g_page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		uint8_t* p = g_page + g_used;
		g_used = (g_used + n + 15) & ~size_t{ 15 };
		return p;
	}

	void put32(uint8_t*& p, uint32_t v)
	{
		std::memcpy(p, &v, 4);
		p += 4;
	}

	uint32_t addr32(const void* p)
	{
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
	}

	using Fn = int(__cdecl*)(int);

	// A stand-in engine function: int f(int x) { record('O'); return x + 1000; }.
	// Its first 7 bytes are three whole instructions (mov eax, 0 ; nop ; nop),
	// so a detour of 5, 6 or 7 bytes ends on an instruction boundary.
	uint8_t* make_original(fnv_exe::Code& code)
	{
		uint8_t* f = alloc_code(48);
		uint8_t* p = f;
		*p++ = 0xB8; put32(p, 0);                   // mov eax, 0
		*p++ = 0x90;                                // nop
		*p++ = 0x90;                                // nop
		*p++ = 0x68; put32(p, 'O');                 // push 'O'
		*p++ = 0xB8; put32(p, addr32(reinterpret_cast<void*>(&record)));
		*p++ = 0xFF; *p++ = 0xD0;                   // call eax
		*p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;      // add esp, 4
		*p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;   // mov eax, [esp+4]
		*p++ = 0x05; put32(p, 1000);                // add eax, 1000
		*p++ = 0xC3;                                // ret
		code = {};
		code.addr = reinterpret_cast<uintptr_t>(f);
		code.len = 7;
		std::memcpy(code.bytes, f, 7);
		return f;
	}

	// ---- Another module's hooks ----------------------------------------------

	template <int N>
	struct Theirs
	{
		static inline Fn original = nullptr;
		static int __cdecl hook(int x)
		{
			record('T');
			return original(x) + 100;
		}
	};

	enum class Form { E9, FF25, PushRet, MovJmp };

	// Writes another module's detour of the given form at `f`, with a
	// trampoline that runs the instructions it covered and jumps back.
	template <int N>
	void install_theirs(uint8_t* f, Form form)
	{
		const size_t covered = form == Form::E9 ? 5 : form == Form::MovJmp ? 7 : 6;
		uint8_t* t = alloc_code(32);
		std::memcpy(t, f, covered);
		cc::emit_rel32(t + covered, 0xE9, reinterpret_cast<uintptr_t>(f + covered));
		Theirs<N>::original = reinterpret_cast<Fn>(t);

		static uint32_t slot = 0;
		slot = addr32(reinterpret_cast<void*>(&Theirs<N>::hook));
		uint8_t* p = f;
		switch (form)
		{
		case Form::E9: cc::emit_rel32(p, 0xE9, reinterpret_cast<uintptr_t>(&Theirs<N>::hook)); break;
		case Form::FF25: *p++ = 0xFF; *p++ = 0x25; put32(p, addr32(&slot)); break;
		case Form::PushRet: *p++ = 0x68; put32(p, slot); *p++ = 0xC3; break;
		case Form::MovJmp: *p++ = 0xB8; put32(p, slot); *p++ = 0xFF; *p++ = 0xE0; break;
		}
		FlushInstructionCache(GetCurrentProcess(), f, 16);
	}

	// ---- The wrapper's hooks -------------------------------------------------

	template <int N>
	struct Ours
	{
		static inline Fn original = nullptr;
		static int __cdecl hook(int x)
		{
			record('W');
			return original(x) + 10;
		}
	};

	int call(uint8_t* f, int x)
	{
		g_order.clear();
		return reinterpret_cast<Fn>(f)(x);
	}

	template <int N>
	void chained_entry(Form form, const char* name)
	{
		std::printf("\n%s at the entry, then the wrapper's hook\n", name);
		fnv_exe::Code code;
		uint8_t* f = make_original(code);
		install_theirs<N>(f, form);
		const int alone = call(f, 1);
		expect(alone == 1101 && g_order == "TO", std::format("before: their hook alone: {} = {}", g_order, alone));
		const cc::Site s = cc::inspect_entry(code);
		expect(s.state == cc::State::Chained && s.target == reinterpret_cast<uintptr_t>(&Theirs<N>::hook),
			std::format("inspect_entry: chained to their hook ({})", hook_forms::name(s.form)));
		const size_t before = log_lines_with("[INFO] Test", "another mod hooked it");
		const bool ok = cc::hook_entry(code, reinterpret_cast<void*>(&Ours<N>::hook),
			reinterpret_cast<void**>(&Ours<N>::original), "Test", name, "test consequence");
		expect(ok, "hook_entry installed");
		expect(log_lines_with("[INFO] Test", "another mod hooked it") == before + 1, "one INFO line naming the module");
		const int r = call(f, 1);
		expect(r == 1111 && g_order == "WTO", std::format("order ours -> theirs -> original: {} = {}", g_order, r));
		expect(f[0] == 0xE9, "the entry holds a plain E9 to the wrapper's hook");
	}
}

int main()
{
	if (MH_Initialize() != MH_OK)
	{
		std::printf("MH_Initialize failed\n");
		return 2;
	}

	// 1. Vanilla entry: hooked directly, with no log line.
	{
		std::printf("\nvanilla entry\n");
		fnv_exe::Code code;
		uint8_t* f = make_original(code);
		expect(cc::inspect_entry(code).state == cc::State::Vanilla, "inspect_entry: vanilla");
		const size_t lines = g_log.size();
		expect(cc::hook_entry(code, reinterpret_cast<void*>(&Ours<0>::hook), reinterpret_cast<void**>(&Ours<0>::original),
			"Test", "vanilla", "test consequence"), "hook_entry installed");
		expect(g_log.size() == lines, "no log line for a vanilla entry");
		const int r = call(f, 1);
		expect(r == 1011 && g_order == "WO", std::format("order ours -> original: {} = {}", g_order, r));
	}

	// 2. Another module's detour first, in each recognised form.
	chained_entry<1>(Form::E9, "E9 rel32 detour");
	chained_entry<2>(Form::FF25, "FF 25 [abs] detour");
	chained_entry<3>(Form::PushRet, "push/ret detour");
	chained_entry<4>(Form::MovJmp, "mov eax/jmp eax detour");

	// 3. A module that hooks after the wrapper and reads what is there.
	uint8_t* after_fn = nullptr;
	{
		std::printf("\nthe wrapper's hook, then another module's after it\n");
		fnv_exe::Code code;
		after_fn = make_original(code);
		expect(cc::hook_entry(code, reinterpret_cast<void*>(&Ours<5>::hook), reinterpret_cast<void**>(&Ours<5>::original),
			"Test", "hooked after the wrapper", "test consequence"), "hook_entry installed");
		// The later module relocates the E9 it finds, as MinHook does.
		const hook_forms::Decoded d = hook_forms::decode(after_fn, 5, reinterpret_cast<uintptr_t>(after_fn), nullptr);
		expect(d.form == hook_forms::Form::JmpRel32 && d.target == reinterpret_cast<uintptr_t>(&Ours<5>::hook),
			"a later module finds a plain E9 to the wrapper's hook");
		uint8_t* t = alloc_code(16);
		cc::emit_rel32(t, 0xE9, d.target);
		Theirs<5>::original = reinterpret_cast<Fn>(t);
		cc::emit_rel32(after_fn, 0xE9, reinterpret_cast<uintptr_t>(&Theirs<5>::hook));
		const int r = call(after_fn, 1);
		expect(r == 1111 && g_order == "TWO", std::format("order theirs -> ours -> original: {} = {}", g_order, r));
	}

	// 4. Foreign entries: no write, feature off.
	{
		std::printf("\nforeign entries\n");
		struct Case { const char* name; uint8_t bytes[7]; };
		const Case cases[] = {
			{ "short jmp", { 0xEB, 0xFE, 0x90, 0x90, 0x90, 0x90, 0x90 } },
			{ "E9 into unmapped memory", { 0xE9, 0x00, 0x00, 0x00, 0x80, 0x90, 0x90 } },
			{ "a patch inside the prologue", { 0xB8, 0x00, 0x00, 0xCC, 0x00, 0x90, 0x90 } },
			{ "push/ret into unmapped memory", { 0x68, 0x10, 0x00, 0x00, 0x00, 0xC3, 0x90 } },
		};
		for (const Case& c : cases)
		{
			fnv_exe::Code code;
			uint8_t* f = make_original(code);
			std::memcpy(f, c.bytes, sizeof(c.bytes));
			uint8_t before[16];
			std::memcpy(before, f, sizeof(before));
			const size_t warns = log_lines_with("[WARN] Test", "leaves it alone");
			void* orig = nullptr;
			const bool ok = cc::hook_entry(code, reinterpret_cast<void*>(&Ours<6>::hook), &orig, "Test", c.name, "x");
			expect(!ok && std::memcmp(before, f, sizeof(before)) == 0 &&
				log_lines_with("[WARN] Test", "leaves it alone") == warns + 1,
				std::format("{}: left alone with one WARN line", c.name));
		}
	}

	// 5. The call-site wrapper thunk.
	{
		std::printf("\ncall-site wrapper thunk\n");
		static volatile int flag = 0, after_flag = 0;
		static int seen_flag[8] = {};
		static int depth_seen = 0;
		static uint8_t* caller = nullptr;

		// The callee records 'C' with the flag it sees and recurses once.
		struct Callee
		{
			static int __cdecl run(int x)
			{
				record('C');
				seen_flag[depth_seen++ & 7] = flag;
				if (x > 1) return reinterpret_cast<Fn>(caller)(x - 1) * 10 + x;
				return x;
			}
			static int __stdcall run_std(int a, int b)
			{
				record('S');
				seen_flag[depth_seen++ & 7] = flag;
				return a * 100 + b;
			}
		};

		// caller(x): push x ; call <site> ; add esp, 4 ; ret
		caller = alloc_code(32);
		uint8_t* p = caller;
		*p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04;   // push dword [esp+4]
		uint8_t* site = p;
		p = cc::emit_rel32(p, 0xE8, reinterpret_cast<uintptr_t>(&Callee::run));
		*p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;               // add esp, 4
		*p++ = 0xC3;

		fnv_exe::Code code = {};
		code.addr = reinterpret_cast<uintptr_t>(site);
		code.len = 5;
		std::memcpy(code.bytes, site, 5);
		const cc::Site s = cc::inspect(code);
		expect(s.state == cc::State::Vanilla && s.target == reinterpret_cast<uintptr_t>(&Callee::run), "inspect: vanilla call");

		uint8_t* thunk = alloc_code(256);
		auto* rs = reinterpret_cast<cc::ReturnStack*>(alloc_code(sizeof(cc::ReturnStack)));
		*rs = {};
		cc::emit_call_wrapper(thunk, rs, s.target, { { &flag, 1 } },
			{ { &flag, 0 }, { &after_flag, 7 } });
		expect(cc::patch_rel32(code.addr, 0xE8, reinterpret_cast<uintptr_t>(thunk)), "site redirected to the thunk");
		cc::track(cc::Kind::Call, code.addr, reinterpret_cast<uintptr_t>(thunk), s, "Test", "call site", "test consequence");

		depth_seen = 0;
		const int r = call(caller, 3);
		expect(r == 123 && g_order == "CCC", std::format("nested calls through the thunk: {} = {}", g_order, r));
		expect(seen_flag[0] == 1 && seen_flag[1] == 1 && seen_flag[2] == 1 && flag == 0 && after_flag == 7,
			"the flag is up inside every call and down after");
		expect(rs->depth == 0, "the return stack is empty after");

		// A callee that pops its own arguments.
		uint8_t* caller2 = alloc_code(32);
		p = caller2;
		*p++ = 0x6A; *p++ = 0x05;                            // push 5
		*p++ = 0x6A; *p++ = 0x07;                            // push 7
		uint8_t* site2 = p;
		p = cc::emit_rel32(p, 0xE8, reinterpret_cast<uintptr_t>(&Callee::run_std));
		*p++ = 0xC3;
		auto* rs2 = reinterpret_cast<cc::ReturnStack*>(alloc_code(sizeof(cc::ReturnStack)));
		*rs2 = {};
		uint8_t* thunk2 = alloc_code(256);
		cc::emit_call_wrapper(thunk2, rs2, reinterpret_cast<uintptr_t>(&Callee::run_std),
			{ { &flag, 1 } }, { { &flag, 0 } });
		cc::patch_rel32(reinterpret_cast<uintptr_t>(site2), 0xE8, reinterpret_cast<uintptr_t>(thunk2));
		depth_seen = 0;
		g_order.clear();
		const int r2 = reinterpret_cast<int(__cdecl*)()>(caller2)();
		expect(r2 == 705 && g_order == "S" && seen_flag[0] == 1 && flag == 0,
			std::format("stdcall callee through the thunk: {} = {}", g_order, r2));

		// A call site another module already redirected: the thunk calls its code.
		struct TheirCall
		{
			static int __cdecl run(int x)
			{
				record('T');
				return Callee::run(x) + 5000;
			}
		};
		uint8_t* caller3 = alloc_code(32);
		p = caller3;
		*p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04;
		uint8_t* site3 = p;
		p = cc::emit_rel32(p, 0xE8, reinterpret_cast<uintptr_t>(&Callee::run));
		*p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
		*p++ = 0xC3;
		fnv_exe::Code code3 = {};
		code3.addr = reinterpret_cast<uintptr_t>(site3);
		code3.len = 5;
		std::memcpy(code3.bytes, site3, 5);
		cc::patch_rel32(code3.addr, 0xE8, reinterpret_cast<uintptr_t>(&TheirCall::run));
		const cc::Site s3 = cc::inspect(code3);
		expect(s3.state == cc::State::Chained, "inspect: chained call");
		auto* rs3 = reinterpret_cast<cc::ReturnStack*>(alloc_code(sizeof(cc::ReturnStack)));
		*rs3 = {};
		uint8_t* thunk3 = alloc_code(256);
		cc::emit_call_wrapper(thunk3, rs3, s3.target, { { &flag, 1 } }, { { &flag, 0 } });
		cc::patch_rel32(code3.addr, 0xE8, reinterpret_cast<uintptr_t>(thunk3));
		depth_seen = 0;
		const int r3 = call(caller3, 1);
		expect(r3 == 5001 && g_order == "TC" && seen_flag[0] == 1,
			std::format("chained call site: ours -> theirs -> original: {} = {}", g_order, r3));
	}

	// 6. Present checks: a slot and a byte the wrapper relies on, then
	//    changes made after the wrapper.
	{
		std::printf("\nPresent checks\n");
		static uintptr_t vtable[4] = {};
		vtable[1] = reinterpret_cast<uintptr_t>(&Theirs<1>::hook);
		cc::Site prev;
		prev.state = cc::State::Vanilla;
		cc::track(cc::Kind::Slot, reinterpret_cast<uintptr_t>(&vtable[1]), vtable[1], prev, "Test", "vtable slot", "slot consequence");
		uint8_t* byte = alloc_code(4);
		*byte = 0xEB;
		cc::track(cc::Kind::Byte, reinterpret_cast<uintptr_t>(byte), 0xEB, prev, "Test", "jz byte", "byte consequence");

		const size_t lines = g_log.size();
		cc::on_present();   // first Present: the hook added after the wrapper in test 3 is noticed
		expect(log_lines_with("[INFO] Test", "patched it after the wrapper") == 1 &&
			log_lines_with("hooked after the wrapper", "hook_chain_test.exe") == 1,
			"first Present: the entry hooked after the wrapper is logged once, naming the module");
		const size_t after_first = g_log.size();
		vtable[1] = reinterpret_cast<uintptr_t>(&Theirs<2>::hook);
		*byte = 0x74;
		for (int i = 0; i < 599; ++i) cc::on_present();
		expect(g_log.size() == after_first, "no check between intervals");
		cc::on_present();
		expect(log_lines_with("vtable slot", "patched it after the wrapper") == 1, "slot replaced: logged once, naming the module");
		expect(log_lines_with("[WARN] Test", "jz byte") == 1, "byte changed: one WARN line");
		for (int i = 0; i < 1200; ++i) cc::on_present();
		expect(g_log.size() == lines + 3, "each change logged once, never again");
	}

	// 7. The records the engine monitor reads.
	{
		std::printf("\nFnvRemix_HookChain\n");
		FnvRemixHook recs[64];
		const uint32_t n = FnvRemix_HookChain(recs, 64);
		uint32_t chained = 0;
		for (uint32_t i = 0; i < n && i < 64; ++i)
		{
			std::printf("    0x%08X ours 0x%08X previous 0x%08X chained %u (%s)\n", static_cast<unsigned>(recs[i].site),
				static_cast<unsigned>(recs[i].ours), static_cast<unsigned>(recs[i].previous), recs[i].chained,
				recs[i].previous_owner);
			chained += recs[i].chained;
		}
		expect(n == 8 && chained == 4, std::format("8 code sites listed (slot left out), 4 chained: {} / {}", n, chained));
	}

	std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
	return g_failures ? 1 : 0;
}
