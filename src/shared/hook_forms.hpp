#pragma once

#include <cstdint>
#include <cstring>

// The instruction forms a hook leaves at the start of a function or at a
// call site, decoded from the bytes there. Shared by the wrapper's hook
// installer (comp/game/call_chain) and the engine monitor's integrity report,
// so both read a site the same way.
namespace hook_forms
{
	enum class Form : uint8_t
	{
		None,          // none of the forms below
		CallRel32,     // E8 rel32
		JmpRel32,      // E9 rel32
		JmpIndirect,   // FF 25 [abs32]
		PushRet,       // 68 imm32 ; C3
		MovEaxJmp,     // B8 imm32 ; FF E0
	};

	struct Decoded
	{
		Form form = Form::None;
		uintptr_t target = 0;   // where control goes; 0 when it cannot be read
		uint8_t len = 0;        // the bytes the form covers
	};

	// Reads the dword at an address; false when it is not readable.
	using ReadU32 = bool (*)(uintptr_t addr, uint32_t& out);

	// Decodes the form at `at` from `p`, a copy of at least `n` of its bytes.
	// An indirect jump reads its pointer through `read`.
	inline Decoded decode(const uint8_t* p, size_t n, uintptr_t at, ReadU32 read)
	{
		Decoded d;
		uint32_t v = 0;
		if (n >= 5 && (p[0] == 0xE8 || p[0] == 0xE9))
		{
			int32_t rel = 0;
			std::memcpy(&rel, p + 1, 4);
			d.form = p[0] == 0xE8 ? Form::CallRel32 : Form::JmpRel32;
			d.target = at + 5 + static_cast<uintptr_t>(rel);
			d.len = 5;
		}
		else if (n >= 6 && p[0] == 0xFF && p[1] == 0x25)
		{
			std::memcpy(&v, p + 2, 4);
			uint32_t dst = 0;
			d.form = Form::JmpIndirect;
			d.target = read && read(v, dst) ? dst : 0;
			d.len = 6;
		}
		else if (n >= 6 && p[0] == 0x68 && p[5] == 0xC3)
		{
			std::memcpy(&v, p + 1, 4);
			d.form = Form::PushRet;
			d.target = v;
			d.len = 6;
		}
		else if (n >= 7 && p[0] == 0xB8 && p[5] == 0xFF && p[6] == 0xE0)
		{
			std::memcpy(&v, p + 1, 4);
			d.form = Form::MovEaxJmp;
			d.target = v;
			d.len = 7;
		}
		return d;
	}

	// A form that replaces a function's start with a jump to other code.
	inline bool is_detour(Form f)
	{
		return f == Form::JmpRel32 || f == Form::JmpIndirect || f == Form::PushRet || f == Form::MovEaxJmp;
	}

	inline const char* name(Form f)
	{
		switch (f)
		{
		case Form::CallRel32: return "call rel32";
		case Form::JmpRel32: return "jmp rel32";
		case Form::JmpIndirect: return "jmp [abs]";
		case Form::PushRet: return "push/ret";
		case Form::MovEaxJmp: return "mov eax/jmp eax";
		default: return "none";
		}
	}
}
