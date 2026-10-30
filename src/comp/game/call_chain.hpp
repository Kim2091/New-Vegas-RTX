#pragma once

#include "shared/fnv_exe.hpp"
#include "shared/hook_forms.hpp"

#include <initializer_list>

// Every place the wrapper writes engine code goes through this module, so the
// wrapper and other mods can hook the same place in either order
// (docs/hook-compat.md).
//
// Call sites (E8 rel32). A site holds the engine's own call, or a call another
// module wrote there to put its hook in front (JohnnyGuitar NVSE, for example,
// redirects Main_Render's calls to RenderWorldScene). Such a hook calls what
// it replaced, so the wrapper's hook goes in front of it the same way: the
// site calls the wrapper's code, and that code calls the target the site
// held. Anything that is not a plain call to executable code (a JMP, NOPs, a
// call into unmapped or non-executable memory) is foreign: the wrapper leaves
// the site alone.
//
// Function entries. An entry holds the engine's own prologue, or another
// module's detour: a jump into executable code (hook_forms: E9 rel32,
// FF 25 [abs], push/ret, mov eax/jmp eax). MinHook builds the wrapper's
// trampoline from whatever is there: it relocates a leading E9 to its target
// and copies the other forms whole, so the wrapper's hook runs first, then
// that module's, then the engine's. Any other bytes (a partial overwrite, a
// patch inside the prologue) are foreign: no write, and the feature stays off.
//
// The wrapper's own writes stay in standard forms: a plain E9 at a function
// entry and an E8 call at a call site. A module that patches the place after
// the wrapper and reads what is there first chains in front of the wrapper's
// hook. One that writes a fixed target skips it; on_present() notices either
// and logs it once.
namespace comp::game::call_chain
{
	enum class State : uint8_t
	{
		Vanilla,   // the engine's own code
		Chained,   // another module's hook into executable code
		Foreign,   // anything else
	};

	struct Site
	{
		State state = State::Foreign;
		uintptr_t target = 0;   // where the site leads; 0 when foreign
		std::string owner;      // the module that holds `target`
		std::string bytes;      // the site's bytes, as hex
		hook_forms::Form form = hook_forms::Form::None;
	};

	// Reads a call site. Its vanilla bytes in the address table are an E8
	// rel32 call.
	Site inspect(const fnv_exe::Code& code);

	// Reads a function entry against its vanilla bytes in the address table.
	// `target` is the function itself when vanilla.
	Site inspect_entry(const fnv_exe::Code& code);

	// Reads the entry of a function outside the address table: anything but a
	// detour counts as the function's own code.
	Site inspect_entry(uintptr_t addr);

	// The file name of the module that holds `addr`, or "allocated memory".
	std::string owner_of(uintptr_t addr);

	// True when `addr` is committed executable memory.
	bool executable(uintptr_t addr);

	// `n` bytes as hex.
	std::string hex(const uint8_t* p, size_t n);

	// Writes `op` rel32 -> `to` at `p` in memory the wrapper owns; returns
	// the byte after it.
	uint8_t* emit_rel32(uint8_t* p, uint8_t op, uintptr_t to);

	// Writes `op` rel32 -> `to` over the five bytes of engine code at `at`.
	// False when the page cannot be made writable.
	bool patch_rel32(uintptr_t at, uint8_t op, uintptr_t to);

	// Writes one byte of engine code at `at`. False when the page cannot be
	// made writable.
	bool patch_byte(uintptr_t at, uint8_t value);

	// One log line for a site that is not vanilla: DEFAULT when chained, naming
	// the previous target and its module; WARN when foreign.
	void log_site(const char* tag, const fnv_exe::Code& code, const char* what, const Site& s);

	// Detours the function at `code` through MinHook when its entry is vanilla
	// or holds another module's detour; `original` then calls whatever ran
	// there before. Logs one line when chained or foreign, and tracks the hook
	// (track) under `if_replaced`. False when foreign or MinHook fails; the
	// caller's feature stays off.
	bool hook_entry(const fnv_exe::Code& code, void* detour, void** original, const char* tag, const char* what,
		const char* if_replaced);

	// The same for a function outside the address table.
	bool hook_entry(uintptr_t addr, void* detour, void** original, const char* tag, const char* what,
		const char* if_replaced);

	// True when hook_entry would hook `code`. Logs the WARN line when it would
	// not. Callers that hook several entries all or none check each first.
	bool hookable(const fnv_exe::Code& code, const char* tag, const char* what);

	// One store a call wrapper makes: mov dword [flag], value.
	struct Store
	{
		volatile int* flag;
		int value;
	};

	// The return addresses of a call wrapper's calls in progress, one per
	// nesting level, and its saved EAX. One thread only (the engine's render
	// thread). Lives in memory the wrapper allocates next to the thunk.
	struct ReturnStack
	{
		uint32_t saved_eax;
		uint32_t depth;
		uint32_t slots[62];
	};

	// Writes at `at` a thunk for an E8 call site: the site calls it, it makes
	// the `before` stores, calls `target` with the arguments, ECX, EDX and EAX
	// the site passed, makes the `after` stores and returns to the site with
	// the target's EAX and EDX. `rs` keeps the site's return address while the
	// target runs. Returns the byte after the thunk.
	uint8_t* emit_call_wrapper(uint8_t* at, ReturnStack* rs, uintptr_t target, std::initializer_list<Store> before,
		std::initializer_list<Store> after);

	// What a tracked place holds while the wrapper's write is in place.
	enum class Kind : uint8_t
	{
		Entry,   // a jump to `ours` at a function entry
		Call,    // an E8 call to `ours`
		Slot,    // a pointer slot (a vtable entry) holding `ours`
		Byte,    // the byte `ours`
	};

	// Records a place the wrapper wrote or relies on. on_present() checks it
	// against `ours` and logs once, naming the module, when another module
	// replaced it; `if_replaced` says what that means for the feature.
	// `previous` is what the place held before.
	void track(Kind kind, uintptr_t at, uintptr_t ours, const Site& previous, const char* tag, const char* what,
		const char* if_replaced);

	// At the first Present and every 600 frames after: every tracked place is
	// checked. Nothing is written back.
	void on_present();
}
