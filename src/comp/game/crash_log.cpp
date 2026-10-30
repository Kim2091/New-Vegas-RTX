#include "std_include.hpp"
#include "crash_log.hpp"

#include <cstdarg>
#include <psapi.h>
#pragma warning(push)
#pragma warning(disable: 4091)
#include <dbghelp.h>
#pragma warning(pop)

#include "shared/common/config.hpp"
#include "shared/fnv_exe.hpp"
#include "shared/fnv_remix_plugin_api.h"

#ifndef FNVR_COMMIT
#define FNVR_COMMIT unknown
#endif
#ifndef FNVR_RELEASE
#define FNVR_RELEASE dev
#endif
#define FNVR_STR2(x) #x
#define FNVR_STR(x) FNVR_STR2(x)

namespace comp::game::crash_log
{
	namespace
	{
		constexpr DWORD kWaitMs = 20000;           // the crashing thread's wait for the writer
		constexpr LONG kMaxLogs = 5;               // per session (a handler may resume the game)
		constexpr uint32_t kScanSlots = 512;       // stack slots from ESP (2 KiB)
		constexpr uint32_t kMaxHits = 96;          // stack slots listed
		constexpr uint32_t kMaxFrames = 64;
		constexpr size_t kBufSize = 64u << 10;
		constexpr size_t kMaxModules = 512;
		constexpr size_t kNameLen = 96;
		constexpr size_t kTextLen = 512;

		// FalloutNV.exe 1.4.0.525 layouts (the plugin's engine.hpp): TESForm
		// {vtable, u8 type, u32 flags at +8, u32 form ID at +0xC};
		// TESFullName {vtable, String {char*, u16 length, u16 capacity}};
		// TESObjectREFR: base form at +0x20, parent cell at +0x40. Gamebryo's
		// NiObjectNET keeps its name (a char*) at +8.
		constexpr uint32_t kFormType = 0x04;
		constexpr uint32_t kFormFlags = 0x08;
		constexpr uint32_t kFormId = 0x0C;
		constexpr uint32_t kFormFlagDeleted = 0x20;
		constexpr uint32_t kFormFlagDisabled = 0x800;
		constexpr uint32_t kFullNameString = 0x04;
		constexpr uint32_t kRefrBaseForm = 0x20;
		constexpr uint32_t kRefrParentCell = 0x40;
		constexpr uint32_t kNiObjectNetName = 0x08;

		constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
			PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

		// ---- Set up at startup ----

		using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

		struct Dbghelp
		{
			decltype(&SymInitializeW) init = nullptr;
			decltype(&SymCleanup) cleanup = nullptr;
			decltype(&SymGetOptions) get_options = nullptr;
			decltype(&SymSetOptions) set_options = nullptr;
			decltype(&StackWalk64) walk = nullptr;
			decltype(&SymFunctionTableAccess64) table_access = nullptr;
			decltype(&SymGetModuleBase64) module_base = nullptr;
			decltype(&SymFromAddr) from_addr = nullptr;
			decltype(&SymGetLineFromAddr64) line_from_addr = nullptr;
			decltype(&SymGetModuleInfo64) module_info = nullptr;

			bool ok() const
			{
				return init && cleanup && get_options && set_options && walk && table_access && module_base;
			}
		};

		bool g_enabled = false;
		HANDLE g_request = nullptr;                 // auto-reset: describe the crash in g_ep
		HANDLE g_done = nullptr;                    // auto-reset: described
		HANDLE g_worker = nullptr;
		HANDLE g_process = nullptr;                 // this process, a handle dbghelp's session gets alone
		Dbghelp g_dbg;
		GetThreadDescriptionFn g_thread_description = nullptr;
		wchar_t g_dir[MAX_PATH] = {};
		wchar_t g_windir[MAX_PATH] = {};
		char g_game[160] = {};
		volatile DWORD g_render_tid = 0;

		// ---- Load order and place, kept current while the game runs ----

		char g_plugins[256][kNameLen] = {};
		bool g_plugin_known[256] = {};
		volatile LONG g_plugin_count = 0;
		FnvRemixPlace g_place = {};
		volatile LONG g_place_valid = 0;
		volatile ULONGLONG g_place_tick = 0;

		// ---- The crash being described ----

		volatile LONG g_busy = 0;
		volatile LONG g_logs = 0;
		volatile LONG g_finished = 0;
		volatile LONG g_opened = 0;
		EXCEPTION_POINTERS* g_ep = nullptr;
		EXCEPTION_RECORD g_rec = {};
		CONTEXT g_ctx = {};
		bool g_have_ctx = false;
		DWORD g_tid = 0;
		HANDLE g_thread = nullptr;
		uintptr_t g_stack_lo = 0, g_stack_hi = 0;
		char g_name[64] = {};
		HANDLE g_file = INVALID_HANDLE_VALUE;
		char* g_buf = nullptr;
		size_t g_len = 0;

		struct Module
		{
			uintptr_t base, end;
			char name[64];
			wchar_t path[MAX_PATH];
		};
		Module g_mods[kMaxModules];
		size_t g_mod_count = 0;
		HMODULE g_handles[kMaxModules];

		// The plugins whose forms were found: the order found and how often.
		struct Tally
		{
			uint16_t rank;
			uint16_t forms;
		};
		Tally g_tally[256] = {};
		uint16_t g_next_rank = 0;

		const char* const kRegNames[] = { "EAX", "EBX", "ECX", "EDX", "ESI", "EDI", "EBP", "ESP", "EIP" };
		char g_reg_text[std::size(kRegNames)][kTextLen];

		struct Hit
		{
			uint32_t offset;                        // from ESP
			uint32_t value;
			char text[kTextLen];
		};
		Hit g_hits[kMaxHits];
		uint32_t g_hit_count = 0;
		uint32_t g_scanned = 0;
		uint32_t g_seen[kScanSlots];

		wchar_t g_sympath[16384];
		alignas(8) uint8_t g_symbol[sizeof(SYMBOL_INFO) + 256];
		IMAGEHLP_MODULE64 g_modinfo;
		CONTEXT g_walk_ctx;

		// ---- Output: a buffer written to the file section by section ----

		void flush()
		{
			if (g_file != INVALID_HANDLE_VALUE && g_len)
			{
				DWORD written = 0;
				WriteFile(g_file, g_buf, static_cast<DWORD>(g_len), &written, nullptr);
			}
			g_len = 0;
		}

		void out(_Printf_format_string_ const char* fmt, ...)
		{
			if (!g_buf) return;
			for (int attempt = 0; attempt < 2; attempt++)
			{
				va_list ap;
				va_start(ap, fmt);
				const int n = _vsnprintf_s(g_buf + g_len, kBufSize - g_len, _TRUNCATE, fmt, ap);
				va_end(ap);
				if (n >= 0)
				{
					g_len += static_cast<size_t>(n);
					return;
				}
				if (g_len == 0)
				{
					// One line longer than the buffer: cut.
					g_len = strnlen(g_buf, kBufSize - 1);
					return;
				}
				flush();
			}
		}

		// Appends to a fixed buffer; cuts what does not fit.
		void append(char* dst, size_t n, _Printf_format_string_ const char* fmt, ...)
		{
			const size_t used = strnlen(dst, n);
			if (used + 1 >= n) return;
			va_list ap;
			va_start(ap, fmt);
			_vsnprintf_s(dst + used, n - used, _TRUNCATE, fmt, ap);
			va_end(ap);
		}

		// ---- Guarded reads ----

		bool readable(uintptr_t p, size_t n)
		{
			if (p < 0x10000 || n == 0 || p + n < p) return false;
			const uintptr_t end = p + n;
			while (p < end)
			{
				MEMORY_BASIC_INFORMATION m = {};
				if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &m, sizeof(m))) return false;
				if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(m.Protect & kReadable)) return false;
				const uintptr_t next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
				if (next <= p) return false;
				p = next;
			}
			return true;
		}

		bool read_raw(uintptr_t p, void* out_data, size_t n)
		{
			if (!readable(p, n)) return false;
			__try
			{
				std::memcpy(out_data, reinterpret_cast<const void*>(p), n);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		template <class T>
		bool rd(uintptr_t p, T& v)
		{
			return read_raw(p, &v, sizeof(T));
		}

		bool is_code(uintptr_t p)
		{
			MEMORY_BASIC_INFORMATION m = {};
			return VirtualQuery(reinterpret_cast<LPCVOID>(p), &m, sizeof(m)) && m.State == MEM_COMMIT &&
				m.Type == MEM_IMAGE && (m.Protect & kExecutable);
		}

		// A C string at `p`, at most n - 1 characters; anything unprintable is '?'.
		bool read_cstr(uintptr_t p, char* dst, size_t n, size_t min_len)
		{
			dst[0] = '\0';
			if (n < 2) return false;
			size_t got = n - 1;
			if (!read_raw(p, dst, got))
			{
				got = 16 < n - 1 ? 16 : n - 1;
				if (!read_raw(p, dst, got)) return false;
			}
			size_t len = 0;
			while (len < got && dst[len]) len++;
			dst[len] = '\0';
			if (len < min_len) return false;
			for (size_t i = 0; i < len; i++)
				if (static_cast<unsigned char>(dst[i]) < 0x20 || static_cast<unsigned char>(dst[i]) == 0x7F) dst[i] = '?';
			return true;
		}

		// A plain text: printable ASCII only, NUL-terminated within the read.
		bool read_text(uintptr_t p, char* dst, size_t n)
		{
			char tmp[72] = {};
			const size_t want = (std::min)(sizeof(tmp) - 1, n - 1);
			if (!read_raw(p, tmp, want) && !read_raw(p, tmp, 8)) return false;
			size_t len = 0;
			while (len < want && tmp[len] >= 0x20 && tmp[len] < 0x7F) len++;
			if (len < 4 || (len < want && tmp[len] != '\0')) return false;
			std::memcpy(dst, tmp, len);
			dst[len] = '\0';
			return true;
		}

		// ---- Modules ----

		void set_module_name(Module& m)
		{
			const wchar_t* slash = wcsrchr(m.path, L'\\');
			const wchar_t* base = slash ? slash + 1 : m.path;
			if (!WideCharToMultiByte(CP_UTF8, 0, base, -1, m.name, static_cast<int>(sizeof(m.name)), nullptr, nullptr))
				strcpy_s(m.name, "?");
			m.name[sizeof(m.name) - 1] = '\0';
		}

		// EnumProcessModules and its siblings read the loader's list without
		// taking the loader lock, which the crashing thread may hold.
		void load_modules()
		{
			g_mod_count = 0;
			DWORD need = 0;
			if (!EnumProcessModules(GetCurrentProcess(), g_handles, static_cast<DWORD>(sizeof(g_handles)), &need)) return;
			const size_t n = (std::min)(static_cast<size_t>(need / sizeof(HMODULE)), kMaxModules);
			for (size_t i = 0; i < n; i++)
			{
				MODULEINFO mi = {};
				if (!GetModuleInformation(GetCurrentProcess(), g_handles[i], &mi, sizeof(mi))) continue;
				Module& m = g_mods[g_mod_count];
				m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
				m.end = m.base + mi.SizeOfImage;
				if (!GetModuleFileNameExW(GetCurrentProcess(), g_handles[i], m.path, MAX_PATH)) m.path[0] = L'\0';
				m.path[MAX_PATH - 1] = L'\0';
				set_module_name(m);
				g_mod_count++;
			}
		}

		// The module that holds `p`. An image the list does not have (or
		// before the list is read) is added from VirtualQuery, its name from
		// the mapped file.
		const Module* module_at(uintptr_t p)
		{
			for (size_t i = 0; i < g_mod_count; i++)
				if (p >= g_mods[i].base && p < g_mods[i].end) return &g_mods[i];
			MEMORY_BASIC_INFORMATION m = {};
			if (p < 0x10000 || !VirtualQuery(reinterpret_cast<LPCVOID>(p), &m, sizeof(m)) || m.Type != MEM_IMAGE ||
				!m.AllocationBase || g_mod_count >= kMaxModules)
				return nullptr;
			Module& mod = g_mods[g_mod_count];
			mod.base = reinterpret_cast<uintptr_t>(m.AllocationBase);
			mod.end = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
			IMAGE_DOS_HEADER dos = {};
			IMAGE_NT_HEADERS32 nt = {};
			if (rd(mod.base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && rd(mod.base + dos.e_lfanew, nt) &&
				nt.Signature == IMAGE_NT_SIGNATURE)
				mod.end = mod.base + nt.OptionalHeader.SizeOfImage;
			if (!GetMappedFileNameW(GetCurrentProcess(), m.AllocationBase, mod.path, MAX_PATH)) mod.path[0] = L'\0';
			mod.path[MAX_PATH - 1] = L'\0';
			set_module_name(mod);
			if (p < mod.base || p >= mod.end) return nullptr;
			return &g_mods[g_mod_count++];
		}

		// "FalloutNV.exe+0x5A1234", or the bare address outside every module.
		void location(uintptr_t p, char* dst, size_t n)
		{
			if (const Module* m = module_at(p)) _snprintf_s(dst, n, _TRUNCATE, "%s+0x%IX", m->name, p - m->base);
			else _snprintf_s(dst, n, _TRUNCATE, "0x%08IX", p);
		}

		// ---- MSVC RTTI ----

		// ".?AVTESObjectREFR@@" -> "TESObjectREFR", ".?AVfoo@bar@@" -> "bar::foo".
		// Templates and other decorated names keep their decoration.
		void demangle(const char* raw, char* dst, size_t n)
		{
			if (!(raw[0] == '.' && raw[1] == '?' && raw[2] == 'A' && (raw[3] == 'V' || raw[3] == 'U')) || raw[4] == '?')
			{
				strncpy_s(dst, n, raw, _TRUNCATE);
				return;
			}
			const char* parts[8];
			size_t lens[8];
			size_t count = 0;
			const char* s = raw + 4;
			while (*s && count < std::size(parts))
			{
				const char* start = s;
				while (*s && *s != '@') s++;
				parts[count] = start;
				lens[count] = static_cast<size_t>(s - start);
				count++;
				if (*s != '@') break;
				s++;
				if (*s == '@' || *s == '\0') break;
			}
			dst[0] = '\0';
			for (size_t i = count; i-- > 0;)
			{
				const size_t used = strnlen(dst, n);
				_snprintf_s(dst + used, n - used, _TRUNCATE, "%s%.*s", i + 1 == count ? "" : "::", static_cast<int>(lens[i]),
					parts[i]);
			}
		}

		// The decorated name in a TypeDescriptor {vtable, spare, name}.
		bool type_name(uintptr_t td, char (&raw)[kNameLen])
		{
			raw[0] = '\0';
			if (!module_at(td) || !read_cstr(td + 8, raw, kNameLen, 4)) return false;
			return raw[0] == '.' && raw[1] == '?' && raw[2] == 'A';
		}

		// The type is `name` ("TESForm@@"), class or struct.
		bool type_is(const char* raw, const char* name)
		{
			return raw[0] == '.' && raw[1] == '?' && raw[2] == 'A' && (raw[3] == 'V' || raw[3] == 'U') &&
				std::strcmp(raw + 4, name) == 0;
		}

		// The complete-object locator just before a vtable:
		// {signature 0, offset, constructor displacement, TypeDescriptor*, ClassHierarchyDescriptor*}.
		struct Col
		{
			uint32_t signature, offset, cd_offset, type, hierarchy;
		};

		bool col_of_vtable(uintptr_t vt, Col& c)
		{
			if ((vt & 3) || !module_at(vt)) return false;
			uint32_t at = 0;
			if (!rd(vt - 4, at) || (at & 3) || !module_at(at)) return false;
			return rd(at, c) && c.signature == 0 && module_at(c.type) && module_at(c.hierarchy);
		}

		struct Object
		{
			uintptr_t complete;                     // the whole object (the pointer minus the locator's offset)
			uintptr_t hierarchy;
			char raw[kNameLen];
			char name[kNameLen];
		};

		bool object_at(uintptr_t p, Object& o)
		{
			if (p < 0x10000 || (p & 3)) return false;
			uint32_t vt = 0;
			Col c = {};
			if (!rd(p, vt) || !col_of_vtable(vt, c) || !type_name(c.type, o.raw)) return false;
			o.complete = p - c.offset;
			o.hierarchy = c.hierarchy;
			demangle(o.raw, o.name, sizeof(o.name));
			return true;
		}

		// Where base class `name` ("TESForm@@") sits in the object, from the
		// ClassHierarchyDescriptor {signature, attributes, count, BaseClassDescriptor**};
		// a BaseClassDescriptor is {TypeDescriptor*, contained bases, mdisp, pdisp, vdisp, ...}.
		bool base_at(const Object& o, const char* name, uintptr_t& at)
		{
			uint32_t h[4] = {};
			if (!rd(o.hierarchy, h)) return false;
			const uint32_t count = (std::min)(h[2], 64u);
			for (uint32_t i = 0; i < count; i++)
			{
				uint32_t bcd = 0;
				uint32_t b[4] = {};
				char raw[kNameLen];
				if (!rd(h[3] + 4 * i, bcd) || !rd(bcd, b) || !type_name(b[0], raw) || !type_is(raw, name)) continue;
				if (static_cast<int32_t>(b[3]) != -1) return false;   // a virtual base: not followed
				at = o.complete + static_cast<int32_t>(b[2]);
				return true;
			}
			return false;
		}

		// ---- Forms ----

		void plugin_label(uint32_t id, char* dst, size_t n)
		{
			const uint32_t index = id >> 24;
			if (index == 0xFF) _snprintf_s(dst, n, _TRUNCATE, "created in game (0xFF, kept in the save)");
			else if (g_plugin_known[index]) _snprintf_s(dst, n, _TRUNCATE, "%s", g_plugins[index]);
			else if (g_plugin_count) _snprintf_s(dst, n, _TRUNCATE, "plugin 0x%02X, not in the load order", index);
			else _snprintf_s(dst, n, _TRUNCATE, "plugin 0x%02X (load order not read)", index);
		}

		void tally(uint32_t id)
		{
			Tally& t = g_tally[id >> 24];
			if (!t.rank) t.rank = ++g_next_rank;
			if (t.forms < 0xFFFF) t.forms++;
		}

		struct Form
		{
			uintptr_t at;
			uint32_t id;
			uint32_t flags;
			uint8_t type;
			bool ref;
			char name[72];
		};

		bool form_of(const Object& o, Form& f)
		{
			uintptr_t at = 0;
			uint8_t head[16] = {};
			if (!base_at(o, "TESForm@@", at) || !read_raw(at, head, sizeof(head))) return false;
			f.at = at;
			f.type = head[kFormType];
			std::memcpy(&f.flags, head + kFormFlags, 4);
			std::memcpy(&f.id, head + kFormId, 4);
			f.name[0] = '\0';
			uintptr_t full = 0;
			if (base_at(o, "TESFullName@@", full))
			{
				uint32_t text = 0;
				uint16_t len = 0;
				if (rd(full + kFullNameString, text) && rd(full + kFullNameString + 4, len) && text && len)
				{
					const size_t want = (std::min)(static_cast<size_t>(len) + 1, sizeof(f.name));
					if (!read_cstr(text, f.name, want, 1)) f.name[0] = '\0';
				}
			}
			uintptr_t refr = 0;
			f.ref = base_at(o, "TESObjectREFR@@", refr);
			return true;
		}

		// `TESNPC 0x0001234A "Name" [Mod.esp]`, for a pointer to a form.
		void form_brief(uintptr_t p, char* dst, size_t n)
		{
			Object o;
			Form f;
			if (!p)
			{
				_snprintf_s(dst, n, _TRUNCATE, "none");
				return;
			}
			if (!object_at(p, o) || !form_of(o, f))
			{
				_snprintf_s(dst, n, _TRUNCATE, "0x%08IX (not a readable form)", p);
				return;
			}
			tally(f.id);
			char plugin[kNameLen];
			plugin_label(f.id, plugin, sizeof(plugin));
			_snprintf_s(dst, n, _TRUNCATE, "%s 0x%08X%s%s%s [%s]", o.name, f.id, f.name[0] ? " \"" : "", f.name,
				f.name[0] ? "\"" : "", plugin);
		}

		void describe_object(const Object& o, uintptr_t p, char* dst, size_t n)
		{
			Form f;
			if (form_of(o, f))
			{
				tally(f.id);
				char plugin[kNameLen];
				plugin_label(f.id, plugin, sizeof(plugin));
				_snprintf_s(dst, n, _TRUNCATE, "%s form 0x%08X%s%s%s [%s] (form type 0x%02X%s%s)", o.name, f.id,
					f.name[0] ? " \"" : "", f.name, f.name[0] ? "\"" : "", plugin, static_cast<unsigned>(f.type),
					(f.flags & kFormFlagDeleted) ? ", deleted" : "", (f.flags & kFormFlagDisabled) ? ", disabled" : "");
				if (f.ref)
				{
					uint32_t base = 0, cell = 0;
					char text[200];
					if (rd(f.at + kRefrBaseForm, base))
					{
						form_brief(base, text, sizeof(text));
						append(dst, n, " | base: %s", text);
					}
					if (rd(f.at + kRefrParentCell, cell) && cell)
					{
						form_brief(cell, text, sizeof(text));
						append(dst, n, " | cell: %s", text);
					}
				}
			}
			else
			{
				_snprintf_s(dst, n, _TRUNCATE, "%s", o.name);
				uintptr_t net = 0;
				uint32_t name_ptr = 0;
				char name[64];
				if (base_at(o, "NiObjectNET@@", net) && rd(net + kNiObjectNetName, name_ptr) && name_ptr &&
					read_cstr(name_ptr, name, sizeof(name), 1))
					append(dst, n, " \"%s\"", name);
			}
			if (p != o.complete) append(dst, n, " (0x%IX into it)", p - o.complete);
		}

		// What a register or stack value points at. True when it is worth
		// listing from the stack: an object, a form, a vtable or code.
		bool describe(uintptr_t v, char* dst, size_t n, bool full)
		{
			dst[0] = '\0';
			if (v < 0x10000) return false;
			Object o;
			if (const Module* m = module_at(v))
			{
				const uintptr_t off = v - m->base;
				if (is_code(v))
				{
					_snprintf_s(dst, n, _TRUNCATE, "%s+0x%IX (code)", m->name, off);
					return true;
				}
				Col c = {};
				char raw[kNameLen], name[kNameLen];
				if (col_of_vtable(v, c) && type_name(c.type, raw))
				{
					demangle(raw, name, sizeof(name));
					_snprintf_s(dst, n, _TRUNCATE, "%s+0x%IX (vtable of %s)", m->name, off, name);
					return true;
				}
				if (object_at(v, o))
				{
					char text[kTextLen];
					describe_object(o, v, text, sizeof(text));
					_snprintf_s(dst, n, _TRUNCATE, "%s+0x%IX (static object: %s)", m->name, off, text);
					return true;
				}
				_snprintf_s(dst, n, _TRUNCATE, "%s+0x%IX (data)", m->name, off);
				return false;
			}
			if (object_at(v, o))
			{
				describe_object(o, v, dst, n);
				return true;
			}
			if (!full) return false;
			if (v >= g_stack_lo && v < g_stack_hi && g_have_ctx)
			{
				if (v >= g_ctx.Esp) _snprintf_s(dst, n, _TRUNCATE, "the stack (ESP+0x%IX)", static_cast<uintptr_t>(v - g_ctx.Esp));
				else _snprintf_s(dst, n, _TRUNCATE, "the stack (ESP-0x%IX)", static_cast<uintptr_t>(g_ctx.Esp - v));
				return false;
			}
			char text[64];
			if (read_text(v, text, sizeof(text)))
			{
				_snprintf_s(dst, n, _TRUNCATE, "text \"%s\"", text);
				return false;
			}
			MEMORY_BASIC_INFORMATION m = {};
			if (!VirtualQuery(reinterpret_cast<LPCVOID>(v), &m, sizeof(m)) || m.State == MEM_FREE)
				_snprintf_s(dst, n, _TRUNCATE, "free memory");
			else if (m.State == MEM_RESERVE)
				_snprintf_s(dst, n, _TRUNCATE, "reserved memory (not committed)");
			else if (!readable(v, 4))
				_snprintf_s(dst, n, _TRUNCATE, "unreadable memory (protection 0x%lX)", m.Protect);
			return false;
		}

		// ---- Sections ----

		const char* exception_name(DWORD code)
		{
			switch (code)
			{
			case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
			case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
			case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
			case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
			case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
			case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
			case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
			case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
			case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
			case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
			case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
			case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
			case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
			case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
			case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
			case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
			case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
			case EXCEPTION_GUARD_PAGE:               return "EXCEPTION_GUARD_PAGE";
			case EXCEPTION_INVALID_HANDLE:           return "EXCEPTION_INVALID_HANDLE";
			case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
			case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
			case 0xC0000017:                         return "STATUS_NO_MEMORY";
			case 0xC0000374:                         return "STATUS_HEAP_CORRUPTION";
			case 0xC0000409:                         return "STATUS_STACK_BUFFER_OVERRUN";
			case 0x40000015:                         return "STATUS_FATAL_APP_EXIT (abort)";
			case 0xE06D7363:                         return "C++ exception";
			default:                                 return "unknown exception";
			}
		}

		// The registers and the stack, annotated. Plugins are counted in the
		// order a form turns up: ECX (the object a member function runs on)
		// first, then the other registers, then the stack from its top.
		void analyse()
		{
			g_hit_count = 0;
			g_scanned = 0;
			for (auto& r : g_reg_text) r[0] = '\0';
			if (!g_have_ctx) return;
			const DWORD regs[] = { g_ctx.Eax, g_ctx.Ebx, g_ctx.Ecx, g_ctx.Edx, g_ctx.Esi, g_ctx.Edi, g_ctx.Ebp, g_ctx.Esp, g_ctx.Eip };
			static constexpr size_t kOrder[] = { 2, 0, 3, 4, 5, 1, 6, 8, 7 };
			for (const size_t i : kOrder) describe(regs[i], g_reg_text[i], kTextLen, true);

			const uintptr_t esp = g_ctx.Esp;
			if (esp < g_stack_lo || esp >= g_stack_hi) return;
			for (uint32_t slot = 0; slot < kScanSlots && g_hit_count < kMaxHits; slot++)
			{
				const uintptr_t at = esp + slot * 4;
				if (at + 4 > g_stack_hi) break;
				uint32_t v = 0;
				if (!rd(at, v)) break;
				g_scanned = slot + 1;
				bool seen = false;
				for (uint32_t k = 0; k < slot && !seen; k++) seen = g_seen[k] == v;
				g_seen[slot] = v;
				if (seen) continue;
				Hit& h = g_hits[g_hit_count];
				if (!describe(v, h.text, sizeof(h.text), false)) continue;
				h.offset = slot * 4;
				h.value = v;
				g_hit_count++;
			}
		}

		void section_summary()
		{
			char where[kNameLen + 32];
			location(reinterpret_cast<uintptr_t>(g_rec.ExceptionAddress), where, sizeof(where));
			out("Crashed in: %s (%s)\n", where, exception_name(g_rec.ExceptionCode));

			out("Plugins involved: the plugins whose forms are in the registers and on the stack, in the order they turn up\n"
				"(the game's own FalloutNV.esm and DLC .esm files are in most crashes; look at the others first):\n");
			uint32_t listed = 0;
			for (uint16_t rank = 1; rank <= g_next_rank; rank++)
				for (uint32_t i = 0; i < 256; i++)
				{
					if (g_tally[i].rank != rank) continue;
					char plugin[kNameLen];
					plugin_label(i << 24, plugin, sizeof(plugin));
					out("  %u. %s: %u form%s\n", ++listed, plugin, static_cast<unsigned>(g_tally[i].forms), g_tally[i].forms == 1 ? "" : "s");
				}
			if (!listed) out("  none: no game form in the registers or on the stack.\n");
		}

		void section_exception()
		{
			out("\n[Exception]\n");
			char where[kNameLen + 32];
			const uintptr_t addr = reinterpret_cast<uintptr_t>(g_rec.ExceptionAddress);
			location(addr, where, sizeof(where));
			out("  Code:    0x%08lX %s%s\n", g_rec.ExceptionCode, exception_name(g_rec.ExceptionCode),
				(g_rec.ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? " (noncontinuable)" : "");
			out("  Address: 0x%08IX %s\n", addr, where);
			if ((g_rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || g_rec.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
				g_rec.NumberParameters >= 2)
			{
				const ULONG_PTR kind = g_rec.ExceptionInformation[0];
				const uintptr_t target = g_rec.ExceptionInformation[1];
				const char* verb = kind == 0 ? "reading" : kind == 1 ? "writing" : kind == 8 ? "executing (DEP)" : "touching";
				char text[kTextLen];
				if (target < 0x10000) _snprintf_s(text, _TRUNCATE, "a null pointer plus 0x%IX", target);
				else describe(target, text, sizeof(text), true);
				out("  Access:  %s 0x%08IX%s%s\n", verb, target, text[0] ? ", " : "", text);
			}
			if (g_rec.ExceptionCode == 0xE06D7363)
			{
				char cxx[kTextLen];
				describe_cxx_exception(&g_rec, cxx, sizeof(cxx));
				out("  C++:     %s\n", cxx);
			}

			char name[128] = {};
			if (g_thread_description && g_thread)
			{
				PWSTR w = nullptr;
				if (SUCCEEDED(g_thread_description(g_thread, &w)) && w)
				{
					if (w[0]) WideCharToMultiByte(CP_UTF8, 0, w, -1, name, static_cast<int>(sizeof(name) - 1), nullptr, nullptr);
					LocalFree(w);
				}
			}
			out("  Thread:  %lu%s%s%s%s\n", g_tid, name[0] ? " \"" : "", name, name[0] ? "\"" : "",
				g_tid == g_render_tid ? ", the game's main thread (it renders the frames)" : "");
			if (g_have_ctx)
				out("  Stack:   ESP 0x%08lX in 0x%08IX-0x%08IX (%Iu KiB used)\n", g_ctx.Esp, g_stack_lo, g_stack_hi,
					g_ctx.Esp >= g_stack_lo && g_ctx.Esp < g_stack_hi ? static_cast<uintptr_t>((g_stack_hi - g_ctx.Esp) >> 10) : uintptr_t(0));
		}

		void section_registers()
		{
			out("\n[Registers]\n");
			if (!g_have_ctx)
			{
				out("  no context record\n");
				return;
			}
			const DWORD regs[] = { g_ctx.Eax, g_ctx.Ebx, g_ctx.Ecx, g_ctx.Edx, g_ctx.Esi, g_ctx.Edi, g_ctx.Ebp, g_ctx.Esp, g_ctx.Eip };
			for (size_t i = 0; i < std::size(regs); i++)
				out("  %s 0x%08lX  %s\n", kRegNames[i], regs[i], g_reg_text[i]);
			out("  EFLAGS 0x%08lX\n", g_ctx.EFlags);
		}

		// The return addresses along the EBP chain (dbghelp missing).
		void ebp_chain()
		{
			char where[kNameLen + 32];
			location(g_ctx.Eip, where, sizeof(where));
			out("  #00 0x%08lX %s\n", g_ctx.Eip, where);
			uintptr_t ebp = g_ctx.Ebp;
			for (int i = 1; i < static_cast<int>(kMaxFrames) && ebp >= g_stack_lo && ebp + 8 <= g_stack_hi && !(ebp & 3); i++)
			{
				uint32_t frame[2] = {};
				if (!rd(ebp, frame) || !is_code(frame[1])) break;
				location(frame[1], where, sizeof(where));
				out("  #%02d 0x%08X %s\n", i, frame[1], where);
				if (frame[0] <= ebp) break;
				ebp = frame[0];
			}
		}

		// The folders of the loaded modules (Windows' own left out): PDBs
		// next to their module are found, and no symbol server is asked.
		void build_symbol_path()
		{
			g_sympath[0] = L'\0';
			size_t used = 0;
			const size_t windir_len = wcslen(g_windir);
			for (size_t i = 0; i < g_mod_count; i++)
			{
				const wchar_t* path = g_mods[i].path;
				const wchar_t* slash = wcsrchr(path, L'\\');
				if (!slash || path[0] == L'\\') continue;
				const size_t len = static_cast<size_t>(slash - path);
				if (windir_len && len >= windir_len && _wcsnicmp(path, g_windir, windir_len) == 0) continue;
				bool dup = false;
				for (size_t k = 0; k < i && !dup; k++)
				{
					const wchar_t* other = g_mods[k].path;
					const wchar_t* other_slash = wcsrchr(other, L'\\');
					dup = other_slash && static_cast<size_t>(other_slash - other) == len && _wcsnicmp(other, path, len) == 0;
				}
				if (dup || used + len + 2 >= std::size(g_sympath)) continue;
				if (used) g_sympath[used++] = L';';
				wmemcpy(g_sympath + used, path, len);
				used += len;
				g_sympath[used] = L'\0';
			}
		}

		void frame_line(uint32_t index, DWORD64 pc, bool symbols)
		{
			char where[kNameLen + 32];
			location(static_cast<uintptr_t>(pc), where, sizeof(where));
			char sym[400] = {};
			if (symbols && g_dbg.from_addr)
			{
				// A return address is just past its call: look up the call.
				const DWORD64 at = index ? pc - 1 : pc;
				auto* s = reinterpret_cast<SYMBOL_INFO*>(g_symbol);
				std::memset(g_symbol, 0, sizeof(g_symbol));
				s->SizeOfStruct = sizeof(SYMBOL_INFO);
				s->MaxNameLen = 255;
				DWORD64 disp = 0;
				if (g_dbg.from_addr(g_process, at, &disp, s))
				{
					std::memset(&g_modinfo, 0, sizeof(g_modinfo));
					g_modinfo.SizeOfStruct = sizeof(g_modinfo);
					const bool exported = g_dbg.module_info && g_dbg.module_info(g_process, at, &g_modinfo) &&
						g_modinfo.SymType == SymExport;
					// The nearest export of a module without a PDB is often another function.
					if (!exported || disp < 0x1000)
						_snprintf_s(sym, _TRUNCATE, " %s+0x%llX%s", s->Name, disp, exported ? " (nearest export)" : "");
				}
				IMAGEHLP_LINE64 line = {};
				line.SizeOfStruct = sizeof(line);
				DWORD line_disp = 0;
				if (g_dbg.line_from_addr && g_dbg.line_from_addr(g_process, at, &line_disp, &line) && line.FileName)
					append(sym, sizeof(sym), " (%s:%lu)", line.FileName, line.LineNumber);
			}
			out("  #%02u 0x%08llX %s%s\n", index, pc, where, sym);
		}

		void section_call_stack()
		{
			if (!g_have_ctx)
			{
				out("\n[Call stack]\n  no context record\n");
				return;
			}
			if (!g_dbg.ok() || !g_process)
			{
				out("\n[Call stack] along the EBP chain (dbghelp.dll did not load); frames without EBP are skipped\n");
				ebp_chain();
				return;
			}
			const DWORD old_options = g_dbg.get_options();
			g_dbg.set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS |
				SYMOPT_LOAD_LINES);
			build_symbol_path();
			const bool symbols = g_dbg.init(g_process, g_sympath[0] ? g_sympath : nullptr, TRUE) != FALSE;
			if (!symbols)
			{
				g_dbg.set_options(old_options);
				out("\n[Call stack] along the EBP chain (dbghelp's SymInitialize failed, error %lu)\n", GetLastError());
				ebp_chain();
				return;
			}

			DWORD64 pcs[kMaxFrames];
			uint32_t count = 0;
			STACKFRAME64 sf = {};
			sf.AddrPC.Offset = g_ctx.Eip;
			sf.AddrPC.Mode = AddrModeFlat;
			sf.AddrFrame.Offset = g_ctx.Ebp;
			sf.AddrFrame.Mode = AddrModeFlat;
			sf.AddrStack.Offset = g_ctx.Esp;
			sf.AddrStack.Mode = AddrModeFlat;
			g_walk_ctx = g_ctx;
			DWORD64 last_frame = 0;
			while (count < kMaxFrames &&
				g_dbg.walk(IMAGE_FILE_MACHINE_I386, g_process, g_thread ? g_thread : GetCurrentThread(), &sf, &g_walk_ctx,
					nullptr, g_dbg.table_access, g_dbg.module_base, nullptr))
			{
				if (!sf.AddrPC.Offset) break;
				if (count && sf.AddrFrame.Offset == last_frame && pcs[count - 1] == sf.AddrPC.Offset) break;
				last_frame = sf.AddrFrame.Offset;
				pcs[count++] = sf.AddrPC.Offset;
			}

			out("\n[Call stack] %u frames (dbghelp StackWalk64; function names where a PDB is next to the module)\n", count);
			char seen[16][64];
			uint32_t seen_count = 0;
			out("  Modules on it:");
			for (uint32_t i = 0; i < count; i++)
			{
				const Module* m = module_at(static_cast<uintptr_t>(pcs[i]));
				const char* name = m ? m->name : "?";
				bool dup = false;
				for (uint32_t k = 0; k < seen_count && !dup; k++) dup = _stricmp(seen[k], name) == 0;
				if (dup || seen_count >= std::size(seen)) continue;
				strcpy_s(seen[seen_count++], name);
				out("%s %s", seen_count == 1 ? "" : ",", name);
			}
			out("\n");
			flush();
			for (uint32_t i = 0; i < count; i++) frame_line(i, pcs[i], symbols);

			g_dbg.cleanup(g_process);
			g_dbg.set_options(old_options);
		}

		void section_stack()
		{
			out("\n[Stack] objects, forms, vtables and code addresses in the top %u slots from ESP (%u read), each value once\n",
				kScanSlots, g_scanned);
			for (uint32_t i = 0; i < g_hit_count; i++)
				out("  ESP+0x%03X 0x%08X  %s\n", g_hits[i].offset, g_hits[i].value, g_hits[i].text);
			if (!g_hit_count) out("  none\n");
		}

		void section_session()
		{
			out("\n[Session]\n");
			out("  New Vegas RTX: release %s, commit %s\n", FNVR_STR(FNVR_RELEASE), FNVR_STR(FNVR_COMMIT));
			out("  Game: %s\n", g_game);
			FILETIME created, exited, kernel, user, now;
			GetSystemTimeAsFileTime(&now);
			if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
			{
				const uint64_t c = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
				const uint64_t t = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
				if (t > c) out("  Running for: %llu s\n", (t - c) / 10000000ull);
			}
			if (g_place_valid)
			{
				const FnvRemixPlace p = g_place;
				const ULONGLONG ago = (GetTickCount64() - g_place_tick) / 1000;
				if (p.interior)
					out("  Place: %.80s (interior), position %.0f %.0f %.0f, read %llu s before the crash\n", p.cell, p.pos[0],
						p.pos[1], p.pos[2], ago);
				else
					out("  Place: %.80s (%d, %d) in %.80s, position %.0f %.0f %.0f, read %llu s before the crash\n", p.cell,
						p.cell_x, p.cell_y, p.worldspace, p.pos[0], p.pos[1], p.pos[2], ago);
			}
			else out("  Place: unknown (no engine monitor plugin, or no player in a cell yet)\n");

			MEMORYSTATUSEX ms = {};
			ms.dwLength = sizeof(ms);
			if (GlobalMemoryStatusEx(&ms))
			{
				SYSTEM_INFO si = {};
				GetSystemInfo(&si);
				uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
				const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
				size_t largest = 0;
				MEMORY_BASIC_INFORMATION mbi = {};
				while (p < end && VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
				{
					if (mbi.State == MEM_FREE) largest = (std::max)(largest, static_cast<size_t>(mbi.RegionSize));
					const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
					if (next <= p) break;
					p = next;
				}
				out("  Address space: %llu of %llu MiB used, largest free block %Iu MiB\n",
					(ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20, ms.ullTotalVirtual >> 20, largest >> 20);
			}
			out("  remix-comp.log ends with the wrapper's last 64 log lines; loadorder.txt (in the session zip) has every\n"
				"  NVSE plugin and DLL with its version and path.\n");
		}

		void section_load_order()
		{
			uint32_t known = 0;
			for (const bool k : g_plugin_known) known += k ? 1u : 0u;
			if (!known)
			{
				out("\n[Load order] not read (the engine monitor plugin is not running, or the game's data was not loaded yet)\n");
				return;
			}
			out("\n[Load order] %u plugins, the engine's list\n", known);
			for (uint32_t i = 0; i < 256; i++)
				if (g_plugin_known[i]) out("  %02X %s\n", i, g_plugins[i]);
		}

		// The file version in the module's version resource (VS_FIXEDFILEINFO).
		bool file_version(uintptr_t base, char* dst, size_t n)
		{
			__try
			{
				const HMODULE m = reinterpret_cast<HMODULE>(base);
				const HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16));
				const HGLOBAL g = r ? LoadResource(m, r) : nullptr;
				const auto* p = g ? static_cast<const uint8_t*>(LockResource(g)) : nullptr;
				const DWORD size = r ? SizeofResource(m, r) : 0;
				if (!p || size < sizeof(VS_FIXEDFILEINFO)) return false;
				for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size && at < 256; at += 4)
				{
					const auto* fi = reinterpret_cast<const VS_FIXEDFILEINFO*>(p + at);
					if (fi->dwSignature != 0xFEEF04BD) continue;
					_snprintf_s(dst, n, _TRUNCATE, "%u.%u.%u.%u", static_cast<unsigned>(HIWORD(fi->dwFileVersionMS)),
						static_cast<unsigned>(LOWORD(fi->dwFileVersionMS)), static_cast<unsigned>(HIWORD(fi->dwFileVersionLS)),
						static_cast<unsigned>(LOWORD(fi->dwFileVersionLS)));
					return true;
				}
				return false;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void section_modules()
		{
			out("\n[Modules] %Iu loaded (base-end, name, file version, path)\n", g_mod_count);
			for (size_t i = 0; i < g_mod_count; i++)
			{
				const Module& m = g_mods[i];
				char version[48];
				if (!file_version(m.base, version, sizeof(version))) strcpy_s(version, "-");
				char path[MAX_PATH * 3];
				if (!WideCharToMultiByte(CP_UTF8, 0, m.path, -1, path, static_cast<int>(sizeof(path)), nullptr, nullptr))
					path[0] = '\0';
				out("  0x%08IX-0x%08IX %s %s %s\n", m.base, m.end, m.name, version, path);
				if (g_len > kBufSize / 2) flush();
			}
		}

		// ---- The writer ----

		using Section = void (*)();

		void run(Section s)
		{
			__try
			{
				s();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				out("\n  (this part stopped at exception 0x%08lX inside the crash log writer)\n", GetExceptionCode());
			}
			flush();
		}

		void copy_exception()
		{
			__try
			{
				g_rec = *g_ep->ExceptionRecord;
				g_have_ctx = g_ep->ContextRecord != nullptr;
				if (g_have_ctx) g_ctx = *g_ep->ContextRecord;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_have_ctx = false;
			}
		}

		void write_log()
		{
			copy_exception();
			wchar_t path[MAX_PATH + 64];
			_snwprintf_s(path, _TRUNCATE, L"%s%S", g_dir, g_name);
			g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (g_file == INVALID_HANDLE_VALUE) return;
			InterlockedExchange(&g_opened, 1);
			g_len = 0;
			std::memset(g_tally, 0, sizeof(g_tally));
			g_next_rank = 0;

			SYSTEMTIME t;
			GetLocalTime(&t);
			out("New Vegas RTX crash log, %04u-%02u-%02u %02u:%02u:%02u\n", static_cast<unsigned>(t.wYear),
				static_cast<unsigned>(t.wMonth), static_cast<unsigned>(t.wDay), static_cast<unsigned>(t.wHour),
				static_cast<unsigned>(t.wMinute), static_cast<unsigned>(t.wSecond));
			out("Written by the New Vegas RTX wrapper (d3d9.dll) when the game crashed; the crash handlers installed\n"
				"before it (Crash Logger and others) run after it. Send this file, or the session's zip from\n"
				"rtx-remix\\logs\\archive (made at the next start), with the report.\n\n");
			flush();

			run(load_modules);
			run(analyse);
			run(section_summary);
			run(section_exception);
			run(section_registers);
			run(section_call_stack);
			run(section_stack);
			run(section_session);
			run(section_load_order);
			run(section_modules);
			out("\n[End]\n");
			flush();
			FlushFileBuffers(g_file);
			CloseHandle(g_file);
			g_file = INVALID_HANDLE_VALUE;
		}

		void write_guarded()
		{
			__try
			{
				write_log();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
			InterlockedExchange(&g_finished, 1);
		}

		DWORD WINAPI writer_thread(LPVOID)
		{
			// Loaded now: loading a DLL at the crash could wait on the loader lock the crashing thread holds.
			if (const HMODULE h = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
			{
				g_dbg.init = reinterpret_cast<decltype(g_dbg.init)>(GetProcAddress(h, "SymInitializeW"));
				g_dbg.cleanup = reinterpret_cast<decltype(g_dbg.cleanup)>(GetProcAddress(h, "SymCleanup"));
				g_dbg.get_options = reinterpret_cast<decltype(g_dbg.get_options)>(GetProcAddress(h, "SymGetOptions"));
				g_dbg.set_options = reinterpret_cast<decltype(g_dbg.set_options)>(GetProcAddress(h, "SymSetOptions"));
				g_dbg.walk = reinterpret_cast<decltype(g_dbg.walk)>(GetProcAddress(h, "StackWalk64"));
				g_dbg.table_access = reinterpret_cast<decltype(g_dbg.table_access)>(GetProcAddress(h, "SymFunctionTableAccess64"));
				g_dbg.module_base = reinterpret_cast<decltype(g_dbg.module_base)>(GetProcAddress(h, "SymGetModuleBase64"));
				g_dbg.from_addr = reinterpret_cast<decltype(g_dbg.from_addr)>(GetProcAddress(h, "SymFromAddr"));
				g_dbg.line_from_addr = reinterpret_cast<decltype(g_dbg.line_from_addr)>(GetProcAddress(h, "SymGetLineFromAddr64"));
				g_dbg.module_info = reinterpret_cast<decltype(g_dbg.module_info)>(GetProcAddress(h, "SymGetModuleInfo64"));
			}
			for (;;)
			{
				if (WaitForSingleObject(g_request, INFINITE) != WAIT_OBJECT_0) return 0;
				write_guarded();
				SetEvent(g_done);
			}
		}
	}

	void describe_cxx_exception(const EXCEPTION_RECORD* er, char* out_text, size_t out_n)
	{
		// MSVC C++ exception (32-bit): ExceptionInformation holds the magic
		// number, the thrown object and its ThrowInfo, all as plain pointers.
		struct CxxTypeDescriptor { const void* vftable; const void* spare; char name[1]; };
		struct CxxCatchableType { unsigned properties; const CxxTypeDescriptor* type; int mdisp, pdisp, vdisp, size; const void* copy; };
		struct CxxCatchableTypeArray { int count; const CxxCatchableType* types[1]; };
		struct CxxThrowInfo { unsigned attributes; const void* unwind; const void* forward_compat; const CxxCatchableTypeArray* types; };

		__try
		{
			if (er->NumberParameters < 3 || er->ExceptionInformation[0] < 0x19930520 ||
				er->ExceptionInformation[0] > 0x19930522)
			{
				_snprintf_s(out_text, out_n, _TRUNCATE, "C++ exception with an unknown layout");
				return;
			}
			const auto* object = reinterpret_cast<const uint8_t*>(er->ExceptionInformation[1]);
			const auto* info = reinterpret_cast<const CxxThrowInfo*>(er->ExceptionInformation[2]);
			if (!info)
			{
				_snprintf_s(out_text, out_n, _TRUNCATE, "C++ rethrow with no exception in flight (a bare 'throw;'), "
					"which ends the process");
				return;
			}
			const CxxCatchableTypeArray* types = info->types;
			const char* name = types && types->count > 0 && types->types[0] && types->types[0]->type
				? types->types[0]->type->name : "?";
			bool is_std_exception = false;
			for (int i = 0; types && i < types->count && i < 16; i++)
				if (types->types[i] && types->types[i]->type &&
					strcmp(types->types[i]->type->name, ".?AVexception@std@@") == 0)
					is_std_exception = true;
			char where[kNameLen + 32];
			location(reinterpret_cast<uintptr_t>(info), where, sizeof(where));
			_snprintf_s(out_text, out_n, _TRUNCATE, "C++ exception %.120s thrown by %s", name, where);
			// MSVC std::exception: the vftable, then the what() string.
			if (is_std_exception && object)
			{
				const char* what = *reinterpret_cast<const char* const*>(object + sizeof(void*));
				if (what) append(out_text, out_n, ", what() \"%.160s\"", what);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			_snprintf_s(out_text, out_n, _TRUNCATE, "C++ exception (its type could not be read)");
		}
	}

	void start()
	{
		g_enabled = shared::common::config::get().session_report.crash_log;
		if (!g_enabled) return;
		g_buf = static_cast<char*>(VirtualAlloc(nullptr, kBufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!g_buf || !g_request || !g_done)
		{
			g_enabled = false;
			return;
		}
		wcsncpy_s(g_dir, shared::common::log_dir().c_str(), _TRUNCATE);
		GetWindowsDirectoryW(g_windir, MAX_PATH);
		DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &g_process, 0, FALSE,
			DUPLICATE_SAME_ACCESS);
		if (const HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
			g_thread_description = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(k32, "GetThreadDescription"));
		const fnv_exe::Identity& id = fnv_exe::identity();
		if (id.variant) _snprintf_s(g_game, _TRUNCATE, "%s", id.variant->name);
		else _snprintf_s(g_game, _TRUNCATE, "FalloutNV.exe of no known version (%s)", id.mismatch);
		g_worker = CreateThread(nullptr, 0, writer_thread, nullptr, 0, nullptr);
	}

	void note_render_thread()
	{
		g_render_tid = GetCurrentThreadId();
	}

	void set_load_order(const std::string& lines)
	{
		size_t at = 0;
		LONG count = 0;
		while (at < lines.size())
		{
			size_t eol = lines.find('\n', at);
			if (eol == std::string::npos) eol = lines.size();
			const std::string_view line(lines.data() + at, eol - at);
			at = eol + 1;
			if (line.size() < 4 || line[2] != ' ') continue;
			char hex[3] = { line[0], line[1], '\0' };
			char* end = nullptr;
			const unsigned long index = strtoul(hex, &end, 16);
			if (!end || *end || index > 0xFE) continue;
			std::string_view name = line.substr(3);
			if (!name.empty() && name.back() == '\r') name.remove_suffix(1);
			_snprintf_s(g_plugins[index], _TRUNCATE, "%.*s", static_cast<int>(name.size()), name.data());
			g_plugin_known[index] = true;
			count++;
		}
		InterlockedExchange(&g_plugin_count, count);
	}

	void note_place(const FnvRemixPlace& place)
	{
		g_place = place;
		g_place_tick = GetTickCount64();
		InterlockedExchange(&g_place_valid, 1);
	}

	const char* on_crash(EXCEPTION_POINTERS* ep)
	{
		if (!g_enabled || !ep || !ep->ExceptionRecord || g_logs >= kMaxLogs) return nullptr;
		if (InterlockedExchange(&g_busy, 1) != 0) return nullptr;
		const LONG n = InterlockedIncrement(&g_logs);

		g_ep = ep;
		g_tid = GetCurrentThreadId();
		const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
		g_stack_lo = reinterpret_cast<uintptr_t>(tib->StackLimit);
		g_stack_hi = reinterpret_cast<uintptr_t>(tib->StackBase);
		g_thread = nullptr;
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread, 0, FALSE,
			DUPLICATE_SAME_ACCESS);
		SYSTEMTIME t;
		GetLocalTime(&t);
		_snprintf_s(g_name, _TRUNCATE, "crash-%04u-%02u-%02u_%02u-%02u-%02u", static_cast<unsigned>(t.wYear),
			static_cast<unsigned>(t.wMonth), static_cast<unsigned>(t.wDay), static_cast<unsigned>(t.wHour),
			static_cast<unsigned>(t.wMinute), static_cast<unsigned>(t.wSecond));
		// A second log in one session (a handler resumed the game after the first) gets a number.
		if (n > 1) append(g_name, sizeof(g_name), "-%ld", n);
		append(g_name, sizeof(g_name), ".txt");
		InterlockedExchange(&g_finished, 0);
		InterlockedExchange(&g_opened, 0);

		// The writer thread does the work; when it is gone (the process is
		// exiting), this thread does.
		bool here = !g_worker || WaitForSingleObject(g_worker, 0) == WAIT_OBJECT_0;
		if (!here)
		{
			SetEvent(g_request);
			const HANDLE handles[] = { g_done, g_worker };
			const DWORD r = WaitForMultipleObjects(2, handles, FALSE, kWaitMs);
			here = r == WAIT_OBJECT_0 + 1 && !g_finished && g_file == INVALID_HANDLE_VALUE;
		}
		if (here) write_guarded();
		return g_opened ? g_name : nullptr;
	}

	void after_chain(LONG verdict, const void* previous_filter)
	{
		// Only the thread whose crash was written; and not while the writer
		// still runs (it timed out): it owns the file and the state.
		if (!g_enabled || !g_busy || g_tid != GetCurrentThreadId() || !g_finished) return;
		if (g_thread)
		{
			CloseHandle(g_thread);
			g_thread = nullptr;
		}
		if (verdict != EXCEPTION_CONTINUE_EXECUTION || !g_opened) return;

		wchar_t path[MAX_PATH + 64];
		_snwprintf_s(path, _TRUNCATE, L"%s%S", g_dir, g_name);
		const HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (f != INVALID_HANDLE_VALUE)
		{
			char where[kNameLen + 32];
			location(reinterpret_cast<uintptr_t>(previous_filter), where, sizeof(where));
			char line[256];
			const int len = _snprintf_s(line, _TRUNCATE, "\nThe crash handler at %s resumed the game after this exception; "
				"the game kept running.\n", where);
			DWORD written = 0;
			if (len > 0) WriteFile(f, line, static_cast<DWORD>(len), &written, nullptr);
			CloseHandle(f);
		}
		// The next crash gets its own log.
		InterlockedExchange(&g_busy, 0);
	}
}
