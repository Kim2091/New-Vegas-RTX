#pragma once

// A D3D9 ps_2_0 / ps_2_x / ps_3_0 bytecode interpreter for offline bakes of a
// pixel shader's output (shader_bake.hpp). CPU only: no D3D9 calls, so it
// runs on any thread.
//
//   * parse(): token stream -> instructions, def / defi / defb constants,
//     input and sampler declarations, and the CTAB (constant table) names.
//   * Exec: runs the program on one pixel; texture reads go through a
//     Sampler.
//   * specialize(): for a flow-free program, classifies each instruction as
//     dead, uniform (the same result for every texel of a bake) or varying,
//     so a bake runs only the varying instructions per texel and replays the
//     uniform ones from one reference run.

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace comp::game::ps_interp
{
	using Vec = std::array<float, 4>;

	enum Reg : uint8_t
	{
		REG_TEMP = 0, REG_INPUT = 1, REG_CONST = 2, REG_TEXTURE = 3, REG_CONSTINT = 7, REG_COLOROUT = 8, REG_DEPTHOUT = 9,
		REG_SAMPLER = 10, REG_CONSTBOOL = 14, REG_LOOP = 15, REG_MISC = 17, REG_PREDICATE = 19,
	};

	enum Op : uint16_t
	{
		OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7, OP_DP3 = 8,
		OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11, OP_SLT = 12, OP_SGE = 13, OP_EXP = 14, OP_LOG = 15, OP_LIT = 16,
		OP_DST = 17, OP_LRP = 18, OP_FRC = 19, OP_M4x4 = 20, OP_M4x3 = 21, OP_M3x4 = 22, OP_M3x3 = 23, OP_M3x2 = 24,
		OP_CALL = 25, OP_CALLNZ = 26, OP_LOOP = 27, OP_RET = 28, OP_ENDLOOP = 29, OP_LABEL = 30, OP_DCL = 31,
		OP_POW = 32, OP_CRS = 33, OP_SGN = 34, OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37, OP_REP = 38, OP_ENDREP = 39,
		OP_IF = 40, OP_IFC = 41, OP_ELSE = 42, OP_ENDIF = 43, OP_BREAK = 44, OP_BREAKC = 45, OP_MOVA = 46,
		OP_DEFB = 47, OP_DEFI = 48, OP_TEXKILL = 65, OP_TEX = 66, OP_CND = 80, OP_DEF = 81, OP_CMP = 88,
		OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94, OP_TEXLDL = 95, OP_BREAKP = 96,
		OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
	};

	inline const char* op_name(uint16_t op, uint8_t ctrl)
	{
		switch (op)
		{
		case OP_MOV: return "mov"; case OP_ADD: return "add"; case OP_SUB: return "sub"; case OP_MAD: return "mad";
		case OP_MUL: return "mul"; case OP_RCP: return "rcp"; case OP_RSQ: return "rsq"; case OP_DP3: return "dp3";
		case OP_DP4: return "dp4"; case OP_MIN: return "min"; case OP_MAX: return "max"; case OP_SLT: return "slt";
		case OP_SGE: return "sge"; case OP_EXP: return "exp"; case OP_LOG: return "log"; case OP_LRP: return "lrp";
		case OP_FRC: return "frc"; case OP_POW: return "pow"; case OP_CRS: return "crs"; case OP_ABS: return "abs";
		case OP_NRM: return "nrm"; case OP_SINCOS: return "sincos"; case OP_REP: return "rep"; case OP_ENDREP: return "endrep";
		case OP_IF: return "if"; case OP_IFC: return "ifc"; case OP_ELSE: return "else"; case OP_ENDIF: return "endif";
		case OP_BREAK: return "break"; case OP_BREAKC: return "breakc"; case OP_TEXKILL: return "texkill";
		case OP_TEX: return ctrl == 1 ? "texldp" : ctrl == 2 ? "texldb" : "texld";
		case OP_CND: return "cnd"; case OP_CMP: return "cmp"; case OP_DP2ADD: return "dp2add"; case OP_DSX: return "dsx";
		case OP_DSY: return "dsy"; case OP_TEXLDD: return "texldd"; case OP_SETP: return "setp"; case OP_TEXLDL: return "texldl";
		case OP_LOOP: return "loop"; case OP_ENDLOOP: return "endloop"; case OP_M4x4: return "m4x4"; case OP_M4x3: return "m4x3";
		case OP_M3x4: return "m3x4"; case OP_M3x3: return "m3x3"; case OP_M3x2: return "m3x2"; case OP_BREAKP: return "breakp";
		default: return "?";
		}
	}

	struct Src
	{
		uint8_t type = 0;
		uint16_t idx = 0;
		uint8_t swz[4] = { 0, 1, 2, 3 };
		uint8_t mod = 0;
		bool rel = false;
		uint8_t rel_type = 0;
		uint16_t rel_idx = 0;
		uint8_t rel_comp = 0;
	};

	struct Dst
	{
		uint8_t type = 0;
		uint16_t idx = 0;
		uint8_t mask = 0xF;
		bool sat = false;
		int8_t shift = 0;
	};

	struct Inst
	{
		uint16_t op = 0;
		uint8_t ctrl = 0;
		bool has_dst = false;
		Dst dst;
		Src src[4];
		int nsrc = 0;
		bool predicated = false;
		Src pred;
		int jump = -1;   // the matching else / endif / endrep / endloop, or the opener
	};

	struct InputDecl
	{
		uint8_t type = 0;
		uint16_t idx = 0;
		uint8_t usage = 0;
		uint8_t usage_index = 0;
		uint8_t mask = 0;
	};

	// One CTAB entry. regset: 0 bool, 1 int4, 2 float4, 3 sampler.
	struct CtabEntry
	{
		std::string name;
		uint16_t regset = 0;
		uint16_t reg = 0;
		uint16_t count = 0;
	};

	struct Program
	{
		int major = 0, minor = 0;
		std::vector<Inst> code;
		std::vector<std::pair<int, std::array<float, 4>>> defs;
		std::vector<std::pair<int, std::array<int, 4>>> defis;
		std::vector<std::pair<int, bool>> defbs;
		std::vector<InputDecl> inputs;
		std::vector<int> samplers;
		std::vector<CtabEntry> ctab;
		bool flow = false;
		std::string error;

		const CtabEntry* find(const char* n) const
		{
			for (const auto& e : ctab)
				if (e.name == n) return &e;
			return nullptr;
		}
	};

	inline uint8_t reg_type(uint32_t t)
	{
		return static_cast<uint8_t>(((t >> 28) & 7) | ((t >> 8) & 0x18));
	}

	inline bool op_has_dst(uint16_t op)
	{
		switch (op)
		{
		case OP_NOP: case OP_CALL: case OP_CALLNZ: case OP_LOOP: case OP_RET: case OP_ENDLOOP: case OP_LABEL:
		case OP_REP: case OP_ENDREP: case OP_IF: case OP_IFC: case OP_ELSE: case OP_ENDIF: case OP_BREAK:
		case OP_BREAKC: case OP_BREAKP: case OP_TEXKILL:
			return false;
		default:
			return true;
		}
	}

	inline Program parse(const uint8_t* bytes, size_t size)
	{
		Program p;
		const size_t n = size / 4;
		std::vector<uint32_t> tk(n);
		if (n) std::memcpy(tk.data(), bytes, n * 4);
		if (n < 2 || (tk[0] >> 16) != 0xFFFF) { p.error = "not a pixel shader"; return p; }
		p.major = static_cast<int>((tk[0] >> 8) & 0xFF);
		p.minor = static_cast<int>(tk[0] & 0xFF);
		std::vector<int> stack;
		bool overrun = false;
		auto token = [&](size_t k) -> uint32_t {
			if (k < n) return tk[k];
			overrun = true;
			return 0;
		};
		auto rd_src = [&](size_t& k) {
			Src s;
			const uint32_t v = token(k++);
			s.type = reg_type(v);
			s.idx = static_cast<uint16_t>(v & 0x7FF);
			for (int c = 0; c < 4; c++) s.swz[c] = static_cast<uint8_t>((v >> (16 + 2 * c)) & 3);
			s.mod = static_cast<uint8_t>((v >> 24) & 0xF);
			if (v & (1u << 13))
			{
				s.rel = true;
				if (p.major >= 3)
				{
					const uint32_t r = token(k++);
					s.rel_type = reg_type(r);
					s.rel_idx = static_cast<uint16_t>(r & 0x7FF);
					s.rel_comp = static_cast<uint8_t>((r >> 16) & 3);
				}
				else s.rel_type = REG_LOOP;
			}
			return s;
		};
		auto rd_dst = [&](size_t& k) {
			Dst d;
			const uint32_t v = token(k++);
			d.type = reg_type(v);
			d.idx = static_cast<uint16_t>(v & 0x7FF);
			d.mask = static_cast<uint8_t>((v >> 16) & 0xF);
			d.sat = ((v >> 20) & 1) != 0;
			const int sh = static_cast<int>((v >> 24) & 0xF);
			d.shift = static_cast<int8_t>(sh > 7 ? sh - 16 : sh);
			return d;
		};
		size_t i = 1;
		while (i < n && !overrun)
		{
			const uint32_t t = tk[i];
			const uint16_t op = static_cast<uint16_t>(t & 0xFFFF);
			if (op == OP_END) break;
			if (op == OP_COMMENT)
			{
				const uint32_t len = (t >> 16) & 0x7FFF;
				// The CTAB: a header of 7 dwords, then 20-byte entries.
				if (len >= 8 && i + 1 + len <= n && tk[i + 1] == 0x42415443u)
				{
					const uint8_t* base = bytes + (i + 2) * 4;
					const size_t blob = static_cast<size_t>(len - 1) * 4;
					uint32_t hdr[7];
					std::memcpy(hdr, base, 28);
					const uint32_t cnum = hdr[3], cinfo = hdr[4];
					for (uint32_t c = 0; c < cnum; c++)
					{
						const size_t at = static_cast<size_t>(cinfo) + 20ull * c;
						if (at + 20 > blob) break;
						const uint8_t* e = base + at;
						uint32_t nm;
						uint16_t regset, reg, cnt;
						std::memcpy(&nm, e, 4);
						std::memcpy(&regset, e + 4, 2);
						std::memcpy(&reg, e + 6, 2);
						std::memcpy(&cnt, e + 8, 2);
						if (nm >= blob) continue;
						const char* name = reinterpret_cast<const char*>(base + nm);
						p.ctab.push_back({ std::string(name, strnlen(name, blob - nm)), regset, reg, cnt });
					}
				}
				i += len + 1;
				continue;
			}
			const uint32_t len = (t >> 24) & 0xF;
			size_t k = i + 1;
			const size_t end = i + 1 + len;
			if (op == OP_DCL)
			{
				const uint32_t u = token(k++);
				const Dst d = rd_dst(k);
				if (d.type == REG_SAMPLER) p.samplers.push_back(d.idx);
				else
					p.inputs.push_back({ d.type, d.idx, static_cast<uint8_t>(u & 0x1F), static_cast<uint8_t>((u >> 16) & 0xF),
						d.mask });
			}
			else if (op == OP_DEF)
			{
				const Dst d = rd_dst(k);
				std::array<float, 4> v{};
				if (k + 4 <= n) std::memcpy(v.data(), &tk[k], 16);
				else overrun = true;
				p.defs.push_back({ d.idx, v });
			}
			else if (op == OP_DEFI)
			{
				const Dst d = rd_dst(k);
				std::array<int, 4> v{};
				if (k + 4 <= n) std::memcpy(v.data(), &tk[k], 16);
				else overrun = true;
				p.defis.push_back({ d.idx, v });
			}
			else if (op == OP_DEFB)
			{
				const Dst d = rd_dst(k);
				p.defbs.push_back({ d.idx, token(k) != 0 });
			}
			else
			{
				Inst in;
				in.op = op;
				in.ctrl = static_cast<uint8_t>((t >> 16) & 0xFF);
				in.predicated = (t & (1u << 28)) != 0;
				if (op == OP_TEXKILL)
				{
					const Dst d = rd_dst(k);
					in.src[0].type = d.type;
					in.src[0].idx = d.idx;
					in.nsrc = 1;
					in.dst = d;   // mask = the components tested
				}
				else
				{
					in.has_dst = op_has_dst(op);
					if (in.has_dst) in.dst = rd_dst(k);
					if (in.predicated) in.pred = rd_src(k);
					while (k < end && in.nsrc < 4) in.src[in.nsrc++] = rd_src(k);
				}
				const int pc = static_cast<int>(p.code.size());
				switch (op)
				{
				case OP_IF: case OP_IFC: case OP_REP: case OP_LOOP:
					stack.push_back(pc);
					p.flow = true;
					break;
				case OP_ELSE:
					if (!stack.empty()) { p.code[stack.back()].jump = pc; stack.back() = pc; }
					break;
				case OP_ENDIF: case OP_ENDREP: case OP_ENDLOOP:
					if (!stack.empty()) { p.code[stack.back()].jump = pc; in.jump = stack.back(); stack.pop_back(); }
					break;
				case OP_CALL: case OP_CALLNZ: case OP_RET: case OP_LABEL:
					p.error = "subroutines not supported";
					break;
				default:
					break;
				}
				p.code.push_back(in);
			}
			i = end;
		}
		if (overrun && p.error.empty()) p.error = "truncated bytecode";
		return p;
	}

	// Texture reads: stage, coordinate (after projection), lod bias / explicit
	// lod, mode (0 implicit, 1 bias, 2 explicit lod).
	struct Sampler
	{
		virtual Vec sample(int stage, const Vec& coord, float lod_arg, int mode) = 0;
	protected:
		~Sampler() = default;
	};

	struct State
	{
		Vec r[32] = {};
		Vec c[256] = {};
		std::array<int, 4> i[16] = {};
		bool b[16] = {};
		Vec v[12] = {};
		Vec t[10] = {};
		Vec oc[4] = {};
		Vec p = {};
		Vec misc[2] = {};
		int aL = 0;
		bool killed = false;
	};

	inline float rcp_(float x) { return x == 0.0f ? FLT_MAX : 1.0f / x; }
	inline float rsq_(float x) { x = std::fabs(x); return x == 0.0f ? FLT_MAX : 1.0f / std::sqrt(x); }
	inline float sat_(float x) { return !(x > 0.0f) ? 0.0f : x > 1.0f ? 1.0f : x; }   // NaN -> 0

	struct Exec
	{
		const Program& prog;
		Sampler* sampler = nullptr;
		uint64_t executed = 0;   // instructions run by run()

		explicit Exec(const Program& p) : prog(p) {}

		Vec* reg(State& s, uint8_t type, int idx)
		{
			switch (type)
			{
			case REG_TEMP: return &s.r[idx & 31];
			case REG_INPUT: return &s.v[idx % 12];
			case REG_CONST: return &s.c[idx & 255];
			case REG_TEXTURE: return &s.t[idx % 10];
			case REG_COLOROUT: return &s.oc[idx & 3];
			case REG_PREDICATE: return &s.p;
			case REG_MISC: return &s.misc[idx & 1];
			default: return nullptr;
			}
		}

		Vec read(State& s, const Src& src)
		{
			Vec base{};
			int idx = src.idx;
			if (src.rel) idx += s.aL;
			if (src.type == REG_CONSTINT)
			{
				const auto& iv = s.i[idx & 15];
				base = { static_cast<float>(iv[0]), static_cast<float>(iv[1]), static_cast<float>(iv[2]), static_cast<float>(iv[3]) };
			}
			else if (src.type == REG_CONSTBOOL)
			{
				const float f = s.b[idx & 15] ? 1.0f : 0.0f;
				base = { f, f, f, f };
			}
			else if (src.type == REG_LOOP)
			{
				const float f = static_cast<float>(s.aL);
				base = { f, f, f, f };
			}
			else if (Vec* r = reg(s, src.type, idx)) base = *r;
			Vec o;
			for (int k = 0; k < 4; k++) o[k] = base[src.swz[k]];
			for (float& x : o)
			{
				switch (src.mod)
				{
				case 1: x = -x; break;
				case 2: x = x - 0.5f; break;
				case 3: x = -(x - 0.5f); break;
				case 4: x = 2 * x - 1; break;
				case 5: x = -(2 * x - 1); break;
				case 6: x = 1 - x; break;
				case 7: x = 2 * x; break;
				case 8: x = -2 * x; break;
				case 11: x = std::fabs(x); break;
				case 12: x = -std::fabs(x); break;
				default: break;
				}
			}
			return o;
		}

		void write(State& s, const Inst& in, const Vec& v)
		{
			Vec* d = reg(s, in.dst.type, in.dst.idx);
			if (!d) return;
			uint8_t mask = in.dst.mask;
			if (in.predicated)
			{
				const Vec pv = read(s, in.pred);
				for (int k = 0; k < 4; k++)
				{
					bool on = pv[k] != 0.0f;
					if (in.pred.mod == 13) on = !on;
					if (!on) mask = static_cast<uint8_t>(mask & ~(1 << k));
				}
			}
			const float scale = in.dst.shift >= 0 ? static_cast<float>(1 << in.dst.shift)
				: 1.0f / static_cast<float>(1 << -in.dst.shift);
			for (int k = 0; k < 4; k++)
				if (mask & (1 << k)) (*d)[k] = in.dst.sat ? sat_(v[k] * scale) : v[k] * scale;
		}

		static bool compare(uint8_t ctrl, float a, float b)
		{
			switch (ctrl)
			{
			case 1: return a > b;
			case 2: return a == b;
			case 3: return a >= b;
			case 4: return a < b;
			case 5: return a != b;
			case 6: return a <= b;
			default: return false;
			}
		}

		// One instruction's result (instructions with a destination only).
		Vec compute(State& s, const Inst& in)
		{
			const Vec a = in.nsrc > 0 ? read(s, in.src[0]) : Vec{};
			const Vec b = in.nsrc > 1 && in.op != OP_TEX && in.op != OP_TEXLDL && in.op != OP_TEXLDD ? read(s, in.src[1]) : Vec{};
			const Vec c = in.nsrc > 2 && in.op != OP_TEXLDD ? read(s, in.src[2]) : Vec{};
			Vec o{};
			auto each = [&](auto f) { for (int k = 0; k < 4; k++) o[k] = f(k); };
			auto fill = [&](float x) { o = { x, x, x, x }; };
			switch (in.op)
			{
			case OP_MOV: o = a; break;
			case OP_ADD: each([&](int k) { return a[k] + b[k]; }); break;
			case OP_SUB: each([&](int k) { return a[k] - b[k]; }); break;
			case OP_MUL: each([&](int k) { return a[k] * b[k]; }); break;
			case OP_MAD: each([&](int k) { return a[k] * b[k] + c[k]; }); break;
			case OP_MIN: each([&](int k) { return a[k] < b[k] ? a[k] : b[k]; }); break;
			case OP_MAX: each([&](int k) { return a[k] >= b[k] ? a[k] : b[k]; }); break;
			case OP_SLT: each([&](int k) { return a[k] < b[k] ? 1.0f : 0.0f; }); break;
			case OP_SGE: each([&](int k) { return a[k] >= b[k] ? 1.0f : 0.0f; }); break;
			case OP_FRC: each([&](int k) { return a[k] - std::floor(a[k]); }); break;
			case OP_ABS: each([&](int k) { return std::fabs(a[k]); }); break;
			case OP_LRP: each([&](int k) { return a[k] * (b[k] - c[k]) + c[k]; }); break;
			case OP_CMP: each([&](int k) { return a[k] >= 0.0f ? b[k] : c[k]; }); break;
			case OP_CND: each([&](int k) { return a[k] > 0.5f ? b[k] : c[k]; }); break;
			case OP_RCP: fill(rcp_(a[0])); break;
			case OP_RSQ: fill(rsq_(a[0])); break;
			case OP_EXP: fill(std::exp2(a[0])); break;
			case OP_LOG: fill(a[0] == 0.0f ? -FLT_MAX : std::log2(std::fabs(a[0]))); break;
			case OP_POW: fill(std::pow(std::fabs(a[0]), b[0])); break;
			case OP_DP3: fill(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]); break;
			case OP_DP4: fill(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]); break;
			case OP_DP2ADD: fill(a[0] * b[0] + a[1] * b[1] + c[0]); break;
			case OP_NRM:
			{
				const float f = rsq_(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
				each([&](int k) { return a[k] * f; });
				break;
			}
			case OP_CRS: o = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0.0f }; break;
			case OP_SINCOS: o = { std::cos(a[0]), std::sin(a[0]), 0.0f, 0.0f }; break;
			case OP_DSX: case OP_DSY: o = {}; break;   // derivatives are evaluated as 0
			case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2:
			{
				const int rows = in.op == OP_M4x4 || in.op == OP_M3x4 ? 4 : in.op == OP_M3x2 ? 2 : 3;
				const int cols = in.op == OP_M4x4 || in.op == OP_M4x3 ? 4 : 3;
				for (int r = 0; r < rows; r++)
				{
					Src s1 = in.src[1];
					s1.idx = static_cast<uint16_t>(s1.idx + r);
					const Vec m = read(s, s1);
					float acc = 0;
					for (int k = 0; k < cols; k++) acc += a[k] * m[k];
					o[r] = acc;
				}
				break;
			}
			case OP_SETP: each([&](int k) { return compare(in.ctrl, a[k], b[k]) ? 1.0f : 0.0f; }); break;
			case OP_TEX: case OP_TEXLDL: case OP_TEXLDD:
			{
				Vec coord = a;
				int mode = 0;
				float lod = 0.0f;
				if (in.op == OP_TEX && in.ctrl == 1 && coord[3] != 0.0f)
					for (int k = 0; k < 3; k++) coord[k] /= coord[3];
				if (in.op == OP_TEX && in.ctrl == 2) { mode = 1; lod = coord[3]; }
				if (in.op == OP_TEXLDL) { mode = 2; lod = coord[3]; }
				const Vec smp = sampler ? sampler->sample(in.src[1].idx, coord, lod, mode) : Vec{};
				for (int k = 0; k < 4; k++) o[k] = smp[in.src[1].swz[k]];
				break;
			}
			default:
				break;
			}
			return o;
		}

		// Runs instruction `pc`; returns the next pc.
		int step(State& s, int pc, std::vector<std::pair<int, int>>& loops)
		{
			const Inst& in = prog.code[pc];
			switch (in.op)
			{
			case OP_TEXKILL:
			{
				const Vec* r = reg(s, in.src[0].type, in.src[0].idx);
				if (r)
					for (int k = 0; k < 4; k++)
						if ((in.dst.mask & (1 << k)) && (*r)[k] < 0.0f) s.killed = true;
				return pc + 1;
			}
			case OP_IF:
			{
				const Vec v = read(s, in.src[0]);
				bool on = v[0] != 0.0f;
				if (in.src[0].mod == 13) on = !on;
				return on ? pc + 1 : in.jump + 1;
			}
			case OP_IFC:
			{
				const Vec a = read(s, in.src[0]), b = read(s, in.src[1]);
				return compare(in.ctrl, a[0], b[0]) ? pc + 1 : in.jump + 1;
			}
			case OP_ELSE:
			{
				int j = pc;
				while (j >= 0 && prog.code[j].op != OP_ENDIF) j = prog.code[j].jump;
				return j < 0 ? static_cast<int>(prog.code.size()) : j + 1;
			}
			case OP_ENDIF:
				return pc + 1;
			case OP_REP:
			{
				const int count = s.i[in.src[0].idx & 15][0];
				if (count <= 0) return in.jump + 1;
				loops.push_back({ pc, count });
				return pc + 1;
			}
			case OP_LOOP:
			{
				const auto& iv = s.i[in.src[1].idx & 15];
				if (iv[0] <= 0) return in.jump + 1;
				s.aL = iv[1];
				loops.push_back({ pc, iv[0] });
				return pc + 1;
			}
			case OP_ENDREP: case OP_ENDLOOP:
			{
				if (loops.empty()) return pc + 1;
				auto& l = loops.back();
				if (--l.second > 0)
				{
					if (in.op == OP_ENDLOOP) s.aL += s.i[prog.code[l.first].src[1].idx & 15][2];
					return l.first + 1;
				}
				loops.pop_back();
				return pc + 1;
			}
			case OP_BREAK: case OP_BREAKC: case OP_BREAKP:
			{
				bool go = true;
				if (in.op == OP_BREAKC)
				{
					const Vec a = read(s, in.src[0]), b = read(s, in.src[1]);
					go = compare(in.ctrl, a[0], b[0]);
				}
				if (in.op == OP_BREAKP)
				{
					const Vec v = read(s, in.src[0]);
					go = v[0] != 0.0f;
					if (in.src[0].mod == 13) go = !go;
				}
				if (!go || loops.empty()) return pc + 1;
				const int open = loops.back().first;
				loops.pop_back();
				return prog.code[open].jump + 1;
			}
			default:
				if (in.has_dst) write(s, in, compute(s, in));
				return pc + 1;
			}
		}

		// The whole program, with a guard against runaway loops.
		void run(State& s)
		{
			std::vector<std::pair<int, int>> loops;
			int pc = 0;
			const int n = static_cast<int>(prog.code.size());
			uint64_t guard = 0;
			while (pc < n && guard++ < 100000)
			{
				pc = step(s, pc, loops);
				executed++;
			}
		}
	};

	inline void load_defs(const Program& p, State& s)
	{
		for (const auto& d : p.defs) s.c[d.first & 255] = d.second;
		for (const auto& d : p.defis) s.i[d.first & 15] = d.second;
		for (const auto& d : p.defbs) s.b[d.first & 15] = d.second;
	}

	// Which components of source `si` an instruction reads, given the
	// components of its result that matter (per-component ops map through the
	// swizzle; reductions read a fixed set).
	inline uint8_t src_read_mask(const Inst& in, int si, uint8_t dst_live)
	{
		const Src& s = in.src[si];
		auto through = [&](uint8_t m) {
			uint8_t r = 0;
			for (int k = 0; k < 4; k++)
				if (m & (1 << k)) r = static_cast<uint8_t>(r | (1 << s.swz[k]));
			return r;
		};
		switch (in.op)
		{
		case OP_MOV: case OP_ADD: case OP_SUB: case OP_MUL: case OP_MAD: case OP_MIN: case OP_MAX: case OP_SLT:
		case OP_SGE: case OP_FRC: case OP_ABS: case OP_LRP: case OP_CMP: case OP_CND: case OP_SETP:
			return through(dst_live);
		case OP_NRM: return through(static_cast<uint8_t>(dst_live | 7));
		case OP_DP3: case OP_CRS: return through(7);
		case OP_DP4: return through(15);
		case OP_DP2ADD: return si == 2 ? through(1) : through(3);
		case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_POW: case OP_SINCOS: return through(1);
		case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: return si == 0 ? through(15) : 0;
		default: return through(15);
		}
	}

	// The specialisation of a flow-free program for a bake: each instruction
	// is Dead, Uniform (replayed from one reference run) or Varying.
	struct Plan
	{
		enum Kind : uint8_t { Dead, Uniform, Varying };
		std::vector<Kind> kind;
		int live = 0, varying = 0;
		bool ok = false;
	};

	// Each instruction's source values (after swizzle and modifier) from one
	// reference run with the bake's constants.
	using SrcValues = std::array<Vec, 4>;

	// `input_varying(type, idx)`: the component mask of a per-texel input (the
	// UV). With `ref`, a uniform 0 factor (a light colour of 0) or a uniform
	// select (cmp / cnd / lrp on a constant) cuts the dependence on the other
	// operand, which folds the lighting away; the plan is then valid for those
	// constants only.
	template <class InputVarying>
	Plan specialize(const Program& p, InputVarying&& input_varying, const std::vector<SrcValues>* ref = nullptr)
	{
		Plan plan;
		if (p.flow || !p.error.empty()) return plan;
		const int n = static_cast<int>(p.code.size());
		plan.kind.assign(n, Plan::Dead);
		// Forward: varying component masks per temp register.
		uint8_t vr[32] = {};
		std::vector<uint8_t> inst_var(n, 0);
		auto src_var = [&](const Src& s, uint8_t read) -> bool {
			uint8_t m = 0;
			if (s.type == REG_TEMP) m = vr[s.idx & 31];
			else if (s.type == REG_INPUT || s.type == REG_TEXTURE) m = input_varying(s.type, static_cast<int>(s.idx));
			return (m & read) != 0;
		};
		for (int pc = 0; pc < n; pc++)
		{
			const Inst& in = p.code[pc];
			if (!in.has_dst) continue;
			uint8_t out = 0;
			// Source si's component j (after swizzle) varies.
			auto var_c = [&](int si, int j) { return src_var(in.src[si], static_cast<uint8_t>(1 << in.src[si].swz[j])); };
			auto zero_c = [&](int si, int j) { return ref && !var_c(si, j) && (*ref)[pc][si][j] == 0.0f; };
			auto term = [&](int j) { return !(zero_c(0, j) || zero_c(1, j)) && (var_c(0, j) || var_c(1, j)); };
			for (int k = 0; k < 4; k++)
			{
				if (!(in.dst.mask & (1 << k))) continue;
				bool v = false;
				bool handled = ref != nullptr;
				switch (ref ? in.op : 0xFFFF)
				{
				case OP_MUL: v = term(k); break;
				case OP_MAD: v = term(k) || var_c(2, k); break;
				case OP_DP3: v = term(0) || term(1) || term(2); break;
				case OP_DP4: v = term(0) || term(1) || term(2) || term(3); break;
				case OP_DP2ADD: v = term(0) || term(1) || var_c(2, 0); break;
				case OP_CMP: case OP_CND:
					if (var_c(0, k)) v = true;
					else
					{
						const float s0 = (*ref)[pc][0][k];
						const bool first = in.op == OP_CMP ? s0 >= 0.0f : s0 > 0.5f;
						v = var_c(first ? 1 : 2, k);
					}
					break;
				case OP_LRP:
					if (!var_c(0, k) && (*ref)[pc][0][k] == 0.0f) v = var_c(2, k);
					else if (!var_c(0, k) && (*ref)[pc][0][k] == 1.0f) v = var_c(1, k);
					else v = var_c(0, k) || var_c(1, k) || var_c(2, k);
					break;
				default:
					handled = false;
					break;
				}
				if (!handled)
					for (int si = 0; si < in.nsrc; si++)
						v = v || src_var(in.src[si], src_read_mask(in, si, static_cast<uint8_t>(1 << k)));
				if (v) out = static_cast<uint8_t>(out | (1 << k));
			}
			inst_var[pc] = out;
			if (in.dst.type == REG_TEMP)
				vr[in.dst.idx & 31] = static_cast<uint8_t>((vr[in.dst.idx & 31] & ~in.dst.mask) | out);
		}
		// Backward liveness from oC0 / oDepth and texkill.
		uint8_t live[32] = {};
		for (int pc = n - 1; pc >= 0; pc--)
		{
			const Inst& in = p.code[pc];
			uint8_t dl = 0;
			if (in.op == OP_TEXKILL)
			{
				if (in.src[0].type == REG_TEMP) live[in.src[0].idx & 31] = static_cast<uint8_t>(live[in.src[0].idx & 31] | in.dst.mask);
				plan.kind[pc] = Plan::Varying;   // kept; its operand decides
				continue;
			}
			if (!in.has_dst) continue;
			if (in.dst.type == REG_COLOROUT) dl = in.dst.mask;
			else if (in.dst.type == REG_TEMP) dl = static_cast<uint8_t>(live[in.dst.idx & 31] & in.dst.mask);
			if (!dl) continue;
			plan.live++;
			const bool var = (inst_var[pc] & dl) != 0;
			plan.kind[pc] = var ? Plan::Varying : Plan::Uniform;
			if (var) plan.varying++;
			if (in.dst.type == REG_TEMP) live[in.dst.idx & 31] = static_cast<uint8_t>(live[in.dst.idx & 31] & ~in.dst.mask);
			if (!var) continue;   // a uniform result replays; its sources need not be live per texel
			for (int si = 0; si < in.nsrc; si++)
				if (in.src[si].type == REG_TEMP)
					live[in.src[si].idx & 31] = static_cast<uint8_t>(live[in.src[si].idx & 31] | src_read_mask(in, si, dl));
		}
		plan.ok = true;
		return plan;
	}

	inline std::string reg_str(uint8_t type, int idx)
	{
		const char* pre = "?";
		switch (type)
		{
		case REG_TEMP: pre = "r"; break;
		case REG_INPUT: pre = "v"; break;
		case REG_CONST: pre = "c"; break;
		case REG_TEXTURE: pre = "t"; break;
		case REG_COLOROUT: pre = "oC"; break;
		case REG_SAMPLER: pre = "s"; break;
		case REG_CONSTINT: pre = "i"; break;
		case REG_CONSTBOOL: pre = "b"; break;
		case REG_PREDICATE: pre = "p"; break;
		default: break;
		}
		return pre + std::to_string(idx);
	}

	// One instruction as assembly text.
	inline std::string disasm(const Inst& in)
	{
		static const char xyzw[] = "xyzw";
		std::string s = op_name(in.op, in.ctrl);
		if (in.has_dst && in.dst.sat) s += "_sat";
		s += " ";
		if (in.has_dst || in.op == OP_TEXKILL)
		{
			s += reg_str(in.dst.type, in.dst.idx);
			if (in.dst.mask != 0xF)
			{
				s += ".";
				for (int k = 0; k < 4; k++)
					if (in.dst.mask & (1 << k)) s += xyzw[k];
			}
			if (in.op == OP_TEXKILL) return s;
		}
		for (int i = 0; i < in.nsrc; i++)
		{
			const Src& r = in.src[i];
			s += (in.has_dst || i) ? ", " : "";
			std::string body = reg_str(r.type, r.idx);
			if (r.mod == 11 || r.mod == 12) body += "_abs";
			if (r.mod == 1 || r.mod == 12) body = "-" + body;
			if (r.mod == 6) body = "1-" + body;
			s += body;
			if (!(r.swz[0] == 0 && r.swz[1] == 1 && r.swz[2] == 2 && r.swz[3] == 3))
			{
				s += ".";
				if (r.swz[0] == r.swz[1] && r.swz[1] == r.swz[2] && r.swz[2] == r.swz[3]) s += xyzw[r.swz[0]];
				else
					for (int k = 0; k < 4; k++) s += xyzw[r.swz[k]];
			}
		}
		return s;
	}
}
