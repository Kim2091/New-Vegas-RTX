#include "std_include.hpp"
#include "sampler.hpp"

#include <algorithm>
#include <atomic>
#include <psapi.h>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace comp::game::sampler
{
	namespace
	{
		constexpr uint32_t kMaxSlots = 384;          // modules seen in a session
		constexpr uint32_t kOutside = kMaxSlots;     // index for addresses outside every module
		constexpr uint32_t kSpotCap = 4096;          // 256-byte code blocks per window
		constexpr uint32_t kSiteCap = 1024;          // call sites into system code per window
		constexpr uint32_t kProbes = 32;
		constexpr uint32_t kStackWords = 1024;       // 4 KiB of stack per sample
		// Samples held until the frame they fall in ends. A frame counted in a
		// window is at most 1 s long, about 34 samples.
		constexpr uint32_t kPendingCap = 64;
		constexpr LONGLONG kPeriodMin100ns = 300000;  // 30 ms
		constexpr LONGLONG kPeriodSpan100ns = 200000; // up to 50 ms, 25 Hz on average
		// The module list is read again after a sample outside every module, at
		// most this often, and at least every kRefreshMaxSeconds.
		constexpr double kRefreshSeconds = 5.0;
		constexpr double kRefreshMaxSeconds = 60.0;
		// A syscall stub's suspended Eip is within this many bytes of its start.
		constexpr uintptr_t kStubBytes = 0x18;

		// What the game thread was doing in system code. The first five are waits.
		enum Kind : uint8_t { kSleep, kWait, kLock, kMessages, kFile, kKernel, kSystemCode, kKinds };
		constexpr uint32_t kWaitKinds = kKernel;
		constexpr const char* kKindNames[kKinds] = { "sleep", "wait", "lock", "message wait", "file I/O", "other kernel call",
			"system code" };

		struct Slot
		{
			uintptr_t base;
			uintptr_t end;
			uintptr_t code_begin;
			uintptr_t code_end;
			bool system;
			char name[40];
		};

		struct Spot
		{
			uint32_t key;    // (slot + 1) << 24 | rva >> 8; 0 is empty
			uint32_t count;
		};

		struct Site
		{
			uint64_t key;    // (slot + 1) << 40 | kind << 32 | return address rva; 0 is empty
			uint32_t count;
			uint32_t unused;
		};

		struct Counters
		{
			uint32_t samples;
			uint32_t own[kMaxSlots + 1];                  // samples in a module's code, waits excluded
			uint32_t called_from[kKinds][kMaxSlots + 1];  // samples in system code by kind and calling module
			Spot spots[kSpotCap];
			uint32_t spots_dropped;
			Site sites[kSiteCap];
			uint32_t sites_dropped;
			int64_t pause_ticks;
			int64_t pause_max;
			uint32_t frames;
			double seconds;
		};

		struct WaitFn
		{
			uintptr_t address;
			Kind kind;
		};

		// One classified sample, waiting for its frame's verdict.
		struct Sample
		{
			uint32_t slot;
			uint32_t caller;         // kOutside when not in system code or not found
			uintptr_t eip;
			uintptr_t caller_address;
			int64_t pause;
			Kind kind;
			bool system;
		};

		struct Raw
		{
			uintptr_t eip;
			uint32_t words;
			uint32_t stack[kStackWords];
		};

		// Written by the sampler thread. Slots are appended and never change, so
		// the game thread can read the names of the first g_slot_count slots.
		Slot g_slots[kMaxSlots];
		std::atomic<uint32_t> g_slot_count{ 0 };

		// Sampler thread only.
		uint16_t g_order[kMaxSlots];  // live slots sorted by base
		uint32_t g_order_count = 0;
		uintptr_t g_min_address = 0;
		uintptr_t g_max_address = 0;
		LARGE_INTEGER g_last_refresh = {};
		WaitFn g_wait_fns[32];
		uint32_t g_wait_fn_count = 0;
		uintptr_t g_ntdll = 0;
		uintptr_t g_win32u = 0;
		Raw g_raw;

		// The window being filled and the samples of the frame in progress, under
		// g_lock (never taken while the game thread is suspended).
		SRWLOCK g_lock = SRWLOCK_INIT;
		Counters g_window;
		Sample g_pending[kPendingCap];
		uint32_t g_pending_count = 0;

		// Game thread only.
		Counters g_snapshot;
		Counters g_session;

		HANDLE g_target = nullptr;
		uintptr_t g_target_teb = 0;
		uintptr_t g_stack_base = 0;
		HMODULE g_self_ref = nullptr;
		HANDLE g_thread = nullptr;
		LARGE_INTEGER g_freq = {};
		std::atomic<bool> g_active{ false };
		std::atomic<bool> g_stop{ false };
		const char* g_off = "not started";

		bool is_system_name(const char* n)
		{
			static constexpr const char* kNames[] = { "ntdll.dll", "kernel32.dll", "kernelbase.dll", "win32u.dll", "user32.dll",
				"gdi32.dll", "gdi32full.dll", "ucrtbase.dll", "msvcrt.dll", "combase.dll", "rpcrt4.dll", "sechost.dll", "advapi32.dll" };
			for (const char* k : kNames)
				if (_stricmp(n, k) == 0) return true;
			return _strnicmp(n, "msvcr", 5) == 0 || _strnicmp(n, "msvcp", 5) == 0 || _strnicmp(n, "vcruntime", 9) == 0
				|| _strnicmp(n, "api-ms-win", 10) == 0;
		}

		// The span covering a module's executable sections, read from its headers.
		bool code_range(uintptr_t base, uintptr_t& begin, uintptr_t& end)
		{
			__try
			{
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
				const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
				begin = ~uintptr_t(0);
				end = 0;
				for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
				{
					if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
					const uintptr_t first = base + static_cast<uintptr_t>(sec->VirtualAddress);
					begin = (std::min)(begin, first);
					end = (std::max)(end, first + static_cast<uintptr_t>(sec->Misc.VirtualSize));
				}
				return end > begin;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Reads the process's module list into the slots. Runs on the sampler
		// thread while the game thread is not suspended.
		void refresh_modules()
		{
			QueryPerformanceCounter(&g_last_refresh);
			static HMODULE mods[1024];
			DWORD needed = 0;
			if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
			const uint32_t n = (std::min)(static_cast<uint32_t>(needed / sizeof(HMODULE)), 1024u);

			static bool live[kMaxSlots];
			memset(live, 0, sizeof(live));
			uint32_t count = g_slot_count.load(std::memory_order_relaxed);
			for (uint32_t i = 0; i < n; i++)
			{
				MODULEINFO mi = {};
				wchar_t wname[MAX_PATH] = {};
				if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
				if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], wname, MAX_PATH)) continue;
				char name[40] = {};
				WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name) - 1, nullptr, nullptr);
				const uintptr_t base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
				const uintptr_t end = base + mi.SizeOfImage;

				uint32_t found = kOutside;
				for (uint32_t s = 0; s < count; s++)
				{
					if (g_slots[s].base == base && g_slots[s].end == end && _stricmp(g_slots[s].name, name) == 0)
					{
						found = s;
						break;
					}
				}
				if (found == kOutside && count < kMaxSlots)
				{
					Slot& slot = g_slots[count];
					slot.base = base;
					slot.end = end;
					if (!code_range(base, slot.code_begin, slot.code_end))
					{
						slot.code_begin = base;
						slot.code_end = end;
					}
					slot.system = is_system_name(name);
					strcpy_s(slot.name, name);
					found = count++;
					g_slot_count.store(count, std::memory_order_release);
				}
				if (found != kOutside) live[found] = true;
			}

			g_order_count = 0;
			for (uint32_t s = 0; s < count; s++)
				if (live[s]) g_order[g_order_count++] = static_cast<uint16_t>(s);
			std::sort(g_order, g_order + g_order_count, [](uint16_t a, uint16_t b) { return g_slots[a].base < g_slots[b].base; });
			g_min_address = g_order_count ? g_slots[g_order[0]].base : 0;
			g_max_address = 0;
			for (uint32_t i = 0; i < g_order_count; i++) g_max_address = (std::max)(g_max_address, g_slots[g_order[i]].end);
		}

		uint32_t find_slot(uintptr_t a)
		{
			if (a < g_min_address || a >= g_max_address) return kOutside;
			uint32_t lo = 0, hi = g_order_count;
			while (lo < hi)
			{
				const uint32_t mid = (lo + hi) / 2;
				if (g_slots[g_order[mid]].base <= a) lo = mid + 1;
				else hi = mid;
			}
			if (!lo) return kOutside;
			const uint32_t s = g_order[lo - 1];
			return a < g_slots[s].end ? s : kOutside;
		}

		void resolve_wait_fns()
		{
			struct Name { const char* name; Kind kind; bool win32u; };
			static constexpr Name kFns[] = {
				{ "NtDelayExecution", kSleep, false }, { "NtYieldExecution", kSleep, false },
				{ "NtWaitForSingleObject", kWait, false }, { "NtWaitForMultipleObjects", kWait, false },
				{ "NtWaitForMultipleObjects32", kWait, false }, { "NtSignalAndWaitForSingleObject", kWait, false },
				{ "NtRemoveIoCompletion", kWait, false }, { "NtRemoveIoCompletionEx", kWait, false },
				{ "NtWaitForAlertByThreadId", kLock, false }, { "NtWaitForKeyedEvent", kLock, false },
				{ "NtReadFile", kFile, false }, { "NtWriteFile", kFile, false }, { "NtCreateFile", kFile, false },
				{ "NtOpenFile", kFile, false }, { "NtQueryAttributesFile", kFile, false }, { "NtQueryFullAttributesFile", kFile, false },
				{ "NtQueryDirectoryFile", kFile, false }, { "NtQueryDirectoryFileEx", kFile, false }, { "NtFlushBuffersFile", kFile, false },
				{ "NtQueryInformationFile", kFile, false }, { "NtSetInformationFile", kFile, false },
				{ "NtUserMsgWaitForMultipleObjectsEx", kMessages, true }, { "NtUserWaitMessage", kMessages, true },
				{ "NtUserGetMessage", kMessages, true },
			};
			const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			const HMODULE win32u = GetModuleHandleW(L"win32u.dll");
			g_ntdll = reinterpret_cast<uintptr_t>(ntdll);
			g_win32u = reinterpret_cast<uintptr_t>(win32u);
			for (const Name& f : kFns)
			{
				const HMODULE m = f.win32u ? win32u : ntdll;
				const FARPROC p = m ? GetProcAddress(m, f.name) : nullptr;
				if (p && g_wait_fn_count < std::size(g_wait_fns)) g_wait_fns[g_wait_fn_count++] = { reinterpret_cast<uintptr_t>(p), f.kind };
			}
			std::sort(g_wait_fns, g_wait_fns + g_wait_fn_count, [](const WaitFn& a, const WaitFn& b) { return a.address < b.address; });
		}

		// The instruction before `r` is a call: `r` is a return address.
		bool after_call(uintptr_t r)
		{
			__try
			{
				const auto* p = reinterpret_cast<const uint8_t*>(r);
				if (p[-5] == 0xE8) return true;                                   // call rel32
				if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;         // call reg / [reg]
				if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;         // call [reg+disp8]
				if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10) return true;         // call [sib+disp8]
				if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;         // call [disp32] / [reg+disp32]
				if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;         // call [sib+disp32]
				return false;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Eip just after a syscall stub's `call edx` (WOW64) or `call [edx]`.
		bool after_syscall(uintptr_t eip)
		{
			__try
			{
				const auto* p = reinterpret_cast<const uint8_t*>(eip);
				return p[-2] == 0xFF && (p[-1] == 0xD2 || p[-1] == 0x12);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		Kind classify(uintptr_t eip, uint32_t slot)
		{
			uint32_t lo = 0, hi = g_wait_fn_count;
			while (lo < hi)
			{
				const uint32_t mid = (lo + hi) / 2;
				if (g_wait_fns[mid].address <= eip) lo = mid + 1;
				else hi = mid;
			}
			if (lo && eip - g_wait_fns[lo - 1].address < kStubBytes) return g_wait_fns[lo - 1].kind;
			const uintptr_t base = g_slots[slot].base;
			if ((base == g_ntdll || base == g_win32u) && after_syscall(eip)) return kKernel;
			return kSystemCode;
		}

		// The first return address on the copied stack that is in a non-system module's code.
		uint32_t find_caller(const Raw& r, uintptr_t& address)
		{
			for (uint32_t i = 0; i < r.words; i++)
			{
				const uintptr_t a = r.stack[i];
				const uint32_t s = find_slot(a);
				if (s == kOutside) continue;
				const Slot& m = g_slots[s];
				if (m.system || a < m.code_begin + 8 || a >= m.code_end) continue;
				if (!after_call(a)) continue;
				address = a;
				return s;
			}
			return kOutside;
		}

		uint32_t hash32(uint64_t k)
		{
			return static_cast<uint32_t>((k ^ (k >> 29)) * 0x9E3779B97F4A7C15ull >> 32);
		}

		void add_spot(Counters& c, uint32_t key, uint32_t n)
		{
			for (uint32_t i = 0, h = hash32(key); i < kProbes; i++, h++)
			{
				Spot& e = c.spots[h & (kSpotCap - 1)];
				if (e.key == key) { e.count += n; return; }
				if (!e.key) { e.key = key; e.count = n; return; }
			}
			c.spots_dropped += n;
		}

		void add_site(Counters& c, uint64_t key, uint32_t n)
		{
			for (uint32_t i = 0, h = hash32(key); i < kProbes; i++, h++)
			{
				Site& e = c.sites[h & (kSiteCap - 1)];
				if (e.key == key) { e.count += n; return; }
				if (!e.key) { e.key = key; e.count = n; return; }
			}
			c.sites_dropped += n;
		}

		// Suspends the game thread, reads Eip and the stack above Esp, resumes
		// it. Between SuspendThread and ResumeThread nothing allocates, locks or
		// faults: the stack is read only inside [StackLimit, StackBase) from the
		// thread's TEB, which is committed memory.
		bool take(Raw& r, int64_t& pause, bool& gone)
		{
			LARGE_INTEGER t0, t1;
			QueryPerformanceCounter(&t0);
			if (SuspendThread(g_target) == static_cast<DWORD>(-1))
			{
				gone = true;
				return false;
			}
			CONTEXT ctx;
			ctx.ContextFlags = CONTEXT_CONTROL;
			const bool ok = GetThreadContext(g_target, &ctx) != 0;
			r.words = 0;
			if (ok)
			{
				r.eip = ctx.Eip;
				const uintptr_t esp = ctx.Esp;
				const auto* tib = reinterpret_cast<const volatile NT_TIB*>(g_target_teb);
				const uintptr_t limit = reinterpret_cast<uintptr_t>(tib->StackLimit);
				if (esp >= limit && esp < g_stack_base && !(esp & 3))
				{
					const uint32_t n = (std::min)(static_cast<uint32_t>((g_stack_base - esp) / 4), kStackWords);
					const auto* src = reinterpret_cast<const volatile uint32_t*>(esp);
					for (uint32_t i = 0; i < n; i++) r.stack[i] = src[i];
					r.words = n;
				}
			}
			ResumeThread(g_target);
			QueryPerformanceCounter(&t1);
			pause = t1.QuadPart - t0.QuadPart;
			return ok;
		}

		// Classifies a sample and holds it until its frame ends (frame_done).
		void count(const Raw& r, int64_t pause)
		{
			uint32_t slot = find_slot(r.eip);
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			const double since = static_cast<double>(now.QuadPart - g_last_refresh.QuadPart) / static_cast<double>(g_freq.QuadPart);
			if (since >= kRefreshMaxSeconds || (slot == kOutside && since >= kRefreshSeconds))
			{
				refresh_modules();
				slot = find_slot(r.eip);
			}
			const bool system = slot != kOutside && g_slots[slot].system;
			Kind kind = kSystemCode;
			uint32_t caller = kOutside;
			uintptr_t caller_address = 0;
			if (system)
			{
				kind = classify(r.eip, slot);
				caller = find_caller(r, caller_address);
			}

			AcquireSRWLockExclusive(&g_lock);
			if (g_pending_count < kPendingCap)
				g_pending[g_pending_count++] = { slot, caller, r.eip, caller_address, pause, kind, system };
			ReleaseSRWLockExclusive(&g_lock);
		}

		// Adds a sample to the window. Under g_lock.
		void add(Counters& c, const Sample& m)
		{
			c.samples++;
			c.pause_ticks += m.pause;
			c.pause_max = (std::max)(c.pause_max, m.pause);
			if (!m.system)
			{
				c.own[m.slot]++;
				if (m.slot != kOutside && m.slot < 255)
					add_spot(c, (m.slot + 1) << 24 | static_cast<uint32_t>((m.eip - g_slots[m.slot].base) >> 8), 1);
			}
			else
			{
				if (m.kind == kSystemCode) c.own[m.slot]++;
				c.called_from[m.kind][m.caller]++;
				if (m.caller != kOutside)
				{
					add_site(c, static_cast<uint64_t>(m.caller + 1) << 40 | static_cast<uint64_t>(m.kind) << 32
						| static_cast<uint32_t>(m.caller_address - g_slots[m.caller].base), 1);
				}
			}
		}

		DWORD WINAPI sampler_main(LPVOID)
		{
			using SetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
			if (const HMODULE kb = GetModuleHandleW(L"kernelbase.dll"))
			{
				if (const auto set = reinterpret_cast<SetDescriptionFn>(GetProcAddress(kb, "SetThreadDescription")))
					set(GetCurrentThread(), L"remix-comp sampler");
			}
			// The game thread is paused from SuspendThread to ResumeThread; this
			// priority keeps the sampler from being preempted in between.
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
			refresh_modules();

			uint32_t rng = GetTickCount() | 1;
			while (!g_stop.load(std::memory_order_relaxed))
			{
				// 30 to 50 ms apart, uneven so the samples don't lock to a frame rate.
				rng ^= rng << 13;
				rng ^= rng >> 17;
				rng ^= rng << 5;
				LARGE_INTEGER due;
				due.QuadPart = -(kPeriodMin100ns + static_cast<LONGLONG>(rng % (kPeriodSpan100ns + 1)));
				if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
				else Sleep(5);
				if (g_stop.load(std::memory_order_relaxed) || !g_active.load(std::memory_order_relaxed)) continue;

				int64_t pause = 0;
				bool gone = false;
				if (take(g_raw, pause, gone)) count(g_raw, pause);
				else if (gone) break;
			}
			if (timer) CloseHandle(timer);
			// The thread holds a reference on this DLL, so its code stays mapped until here.
			FreeLibraryAndExitThread(g_self_ref, 0);
		}

		void merge(Counters& into, const Counters& c)
		{
			into.samples += c.samples;
			into.pause_ticks += c.pause_ticks;
			into.pause_max = (std::max)(into.pause_max, c.pause_max);
			into.frames += c.frames;
			into.seconds += c.seconds;
			for (uint32_t s = 0; s <= kMaxSlots; s++) into.own[s] += c.own[s];
			for (uint32_t k = 0; k < kKinds; k++)
				for (uint32_t s = 0; s <= kMaxSlots; s++) into.called_from[k][s] += c.called_from[k][s];
			for (const Spot& e : c.spots)
				if (e.key) add_spot(into, e.key, e.count);
			for (const Site& e : c.sites)
				if (e.key) add_site(into, e.key, e.count);
			into.spots_dropped += c.spots_dropped;
			into.sites_dropped += c.sites_dropped;
		}

		const char* slot_name(uint32_t s)
		{
			return s < g_slot_count.load(std::memory_order_acquire) ? g_slots[s].name : "outside modules";
		}

		bool slot_system(uint32_t s)
		{
			return s < g_slot_count.load(std::memory_order_acquire) && g_slots[s].system;
		}

		struct Entry
		{
			uint32_t count;
			std::string name;
		};

		std::string join_top(std::vector<Entry>& v, size_t n, double total, double min_pct = 0.0)
		{
			std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.count > b.count; });
			std::string s;
			for (size_t i = 0; i < v.size() && i < n; i++)
			{
				const double pct = 100.0 * v[i].count / total;
				if (!v[i].count || pct < min_pct) break;
				s += std::format("{}{} {:.1f}%", s.empty() ? "" : ", ", v[i].name, pct);
			}
			return s.empty() ? std::string("-") : s;
		}

		uint32_t wait_samples(const Counters& c, uint32_t kind)
		{
			uint32_t n = 0;
			for (uint32_t s = 0; s <= kMaxSlots; s++) n += c.called_from[kind][s];
			return n;
		}

		std::string compact_text(const Counters& c)
		{
			if (!c.samples) return std::format("profile: no samples ({})", g_off);
			const double total = c.samples;
			// A module's share includes the system code and kernel calls it makes.
			std::vector<Entry> owners;
			for (uint32_t s = 0; s < kMaxSlots; s++)
			{
				if (slot_system(s)) continue;
				const uint32_t n = c.own[s] + c.called_from[kSystemCode][s] + c.called_from[kKernel][s];
				if (n) owners.push_back({ n, slot_name(s) });
			}
			const uint32_t unknown = c.called_from[kSystemCode][kOutside] + c.called_from[kKernel][kOutside];
			if (unknown) owners.push_back({ unknown, "system code (caller not found)" });
			if (c.own[kOutside]) owners.push_back({ c.own[kOutside], "outside modules" });

			uint32_t waiting = 0;
			std::string waits;
			for (uint32_t k = 0; k < kWaitKinds; k++)
			{
				const uint32_t n = wait_samples(c, k);
				waiting += n;
				if (100.0 * n / total < 0.5) continue;
				uint32_t top = kOutside;
				for (uint32_t s = 0; s < kMaxSlots; s++)
					if (c.called_from[k][s] > (top == kOutside ? 0u : c.called_from[k][top])) top = s;
				waits += std::format("{}{} {:.0f}% from {}", waits.empty() ? "" : ", ", kKindNames[k], 100.0 * n / total,
					top == kOutside ? "an unknown caller" : slot_name(top));
			}
			return std::format("profile of {} samples ({:.0f} Hz): {} (each with the system code it calls); waiting {:.0f}%{}{}",
				c.samples, c.seconds > 0.0 ? c.samples / c.seconds : 0.0, join_top(owners, 5, total, 0.5), 100.0 * waiting / total,
				waits.empty() ? "" : ": ", waits);
		}

		std::string detail_text(const Counters& c)
		{
			if (!c.samples) return std::format("profile: no samples ({})", g_off);
			const double total = c.samples;

			std::vector<Entry> own;
			for (uint32_t s = 0; s <= kMaxSlots; s++)
				if (c.own[s]) own.push_back({ c.own[s], slot_name(s) });

			std::vector<Entry> spots;
			for (const Spot& e : c.spots)
			{
				if (!e.key) continue;
				spots.push_back({ e.count, std::format("{}+0x{:X}", slot_name((e.key >> 24) - 1), (e.key & 0xFFFFFF) << 8) });
			}

			std::vector<Entry> sites;
			for (const Site& e : c.sites)
			{
				if (!e.key) continue;
				const uint32_t s = static_cast<uint32_t>(e.key >> 40) - 1;
				const uint32_t k = static_cast<uint32_t>(e.key >> 32) & 0xFF;
				sites.push_back({ e.count, std::format("{} {}+0x{:X}", kKindNames[(std::min)(k, kKinds - 1u)], slot_name(s),
					static_cast<uint32_t>(e.key)) });
			}

			std::string kinds;
			for (uint32_t k = 0; k < kKinds; k++)
			{
				const uint32_t n = wait_samples(c, k);
				if (n) kinds += std::format("{}{} {:.1f}%", kinds.empty() ? "" : ", ", kKindNames[k], 100.0 * n / total);
			}
			uint32_t not_found = 0;
			for (uint32_t k = 0; k < kKinds; k++) not_found += c.called_from[k][kOutside];

			const double pause_ms = static_cast<double>(c.pause_ticks) * 1000.0 / static_cast<double>(g_freq.QuadPart);
			return std::format(
				"profile of {} samples ({:.0f} Hz) | own code by module: {} | hot 256-byte blocks (module+RVA): {} | system code by kind: {}; "
				"caller not found {:.1f}% | system code by call site (kind, module+RVA of the return address): {} | suspend to resume "
				"{:.0f} us per sample (longest {:.0f} us), {:.3f} ms per frame{}",
				c.samples, c.seconds > 0.0 ? c.samples / c.seconds : 0.0, join_top(own, 15, total), join_top(spots, 15, total),
				kinds.empty() ? std::string("-") : kinds, 100.0 * not_found / total, join_top(sites, 10, total),
				pause_ms * 1000.0 / total, static_cast<double>(c.pause_max) * 1e6 / static_cast<double>(g_freq.QuadPart),
				c.frames ? pause_ms / c.frames : 0.0,
				c.spots_dropped || c.sites_dropped ? std::format(" | not bucketed: {} blocks, {} sites", c.spots_dropped, c.sites_dropped)
				: std::string());
		}
	}

	void start()
	{
		if (g_thread || g_stop.load()) return;
		QueryPerformanceFrequency(&g_freq);
		if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
			THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
		{
			g_off = "could not open the game thread";
			g_target = nullptr;
			return;
		}
		const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
		g_target_teb = reinterpret_cast<uintptr_t>(tib);
		g_stack_base = reinterpret_cast<uintptr_t>(tib->StackBase);
		resolve_wait_fns();

		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&start), &g_self_ref))
		{
			g_off = "no module reference";
			return;
		}
		g_thread = CreateThread(nullptr, 64 * 1024, sampler_main, nullptr, 0, nullptr);
		if (!g_thread)
		{
			FreeLibrary(g_self_ref);
			g_off = "could not start the sampler thread";
			return;
		}
		g_off = "the game was not in game";
	}

	void set_active(bool on)
	{
		g_active.store(on && g_thread != nullptr, std::memory_order_relaxed);
	}

	void frame_done(bool counted)
	{
		AcquireSRWLockExclusive(&g_lock);
		if (counted)
			for (uint32_t i = 0; i < g_pending_count; i++) add(g_window, g_pending[i]);
		g_pending_count = 0;
		ReleaseSRWLockExclusive(&g_lock);
	}

	void begin_window()
	{
		AcquireSRWLockExclusive(&g_lock);
		memset(&g_window, 0, sizeof(g_window));
		ReleaseSRWLockExclusive(&g_lock);
	}

	Text end_window(uint32_t frames, double seconds)
	{
		AcquireSRWLockExclusive(&g_lock);
		g_snapshot = g_window;
		memset(&g_window, 0, sizeof(g_window));
		ReleaseSRWLockExclusive(&g_lock);
		g_snapshot.frames = frames;
		g_snapshot.seconds = seconds;
		merge(g_session, g_snapshot);
		return { compact_text(g_snapshot), detail_text(g_snapshot) };
	}

	std::string session_detail()
	{
		return detail_text(g_session);
	}

	void stop()
	{
		g_stop.store(true);
		g_active.store(false);
	}
}
