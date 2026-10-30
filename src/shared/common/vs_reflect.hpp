// Vertex shader constant-table classifier for the CPU vertex expanders, the
// counterpart of ps_reflect.hpp. Some FNV vertex shaders build geometry or
// UVs from their constant registers (instancing, billboards, flipbook
// atlases, UV animation), work that is lost without the shader.
// comp/game/vs_expand.cpp replays that math on the CPU; this header tells it
// which math a shader needs and where its inputs live.
//
// Each unique vertex shader (by bytecode hash) is parsed once with
// D3DXGetShaderConstantTable. The family is decided from the constant names
// the shader declares, and every register the expander reads is resolved by
// name, so a shader package that lays its constants out differently still
// resolves.
//
// Families (checked against shader package 004's disassembly):
//   ParticleSubTex: NOLIGHT017/019, flipbook particles, SubTexOffsets[16]
//   NoLightUv:      NOLIGHT000-018 (even), affine UV via TexCoordTranform
//   Grass:          GRASS20xx / GRASS23xXXX, InstanceData + ScaleMask
//   DistLod:        DISTLOD2002/2003, InstanceData + EyePos + AlphaParam
//   DistTree:       DISTTREE, InstanceData, 2 registers per instance
//   Leaf:           STLEAF000-003, LeafBase + BillboardRight/Up

#pragma once

#include "shared/common/console.hpp"

namespace shared::common
{
	enum class VsFamily : uint8_t {
		None = 0,
		ParticleSubTex,
		NoLightUv,
		Grass,
		DistLod,
		DistTree,
		Leaf,
		Count,
	};

	inline const char* vs_family_name(VsFamily f) {
		switch (f) {
		case VsFamily::ParticleSubTex: return "Particle(SubTexOffsets)";
		case VsFamily::NoLightUv:      return "NoLight(TexCoordTranform)";
		case VsFamily::Grass:          return "Grass";
		case VsFamily::DistLod:        return "DistLod";
		case VsFamily::DistTree:       return "DistTree";
		case VsFamily::Leaf:           return "SpeedTreeLeaf";
		default:                       return "None";
		}
	}

	// Per-VS descriptor. Registers are c-register indices (0..255), kNoReg
	// when the shader doesn't declare that constant.
	struct VsDesc {
		static constexpr uint8_t kNoReg = 0xFF;

		uint32_t vs_hash = 0;
		VsFamily family = VsFamily::None;
		// False when the shader carries no constant table (a replacement
		// shader compiled without reflection data).
		bool has_table = false;

		uint8_t reg_mvp          = kNoReg;  // ModelViewProj (4 regs)
		uint8_t reg_sub_tex      = kNoReg;  // SubTexOffsets
		uint8_t sub_tex_count    = 0;       //   declared register count (16)
		uint8_t reg_tc_xform     = kNoReg;  // TexCoordTranform (2 regs, sic)
		uint8_t reg_instance     = kNoReg;  // InstanceData (indexed past its declared size)
		uint8_t reg_scale_mask   = kNoReg;  // ScaleMask
		uint8_t reg_wind         = kNoReg;  // WindData
		uint8_t reg_alpha_param  = kNoReg;  // AlphaParam (distance fade)
		uint8_t reg_eye_pos      = kNoReg;  // EyePos
		uint8_t reg_leaf_base    = kNoReg;  // LeafBase
		uint8_t leaf_base_count  = 0;       //   declared register count (48)
		uint8_t reg_bb_right     = kNoReg;  // BillboardRight
		uint8_t reg_bb_up        = kNoReg;  // BillboardUp
		// Not an expander input: WATER*.vso's noise tiling (world units per
		// NoiseMap tile in .x), read by the API water path.
		uint8_t reg_tex_scale    = kNoReg;  // TexScale

		bool has(uint8_t reg) const { return reg != kNoReg; }
	};

	// Caches VS_hash -> VsDesc. Same lifetime rules as PsShaderClassifier:
	// the pointer cache AddRefs each shader so a freed and reused address
	// cannot alias a different shader's descriptor, and nothing is released at
	// static teardown (the device and bridge may already be gone by then).
	class VsShaderClassifier {
	public:
		// Returns a stable pointer to the cached descriptor, classifying the
		// shader on first sight. nullptr for a null shader or a failed read.
		const VsDesc* classify(IDirect3DVertexShader9* shader) {
			if (!shader) return nullptr;

			if (const auto it = ptr_cache_.find(shader); it != ptr_cache_.end()) {
				return &it->second;
			}

			UINT bytecode_size = 0;
			if (FAILED(shader->GetFunction(nullptr, &bytecode_size)) || bytecode_size == 0) {
				return nullptr;
			}
			std::vector<BYTE> bytecode(bytecode_size);
			if (FAILED(shader->GetFunction(bytecode.data(), &bytecode_size))) {
				return nullptr;
			}
			const uint32_t hash = shared::utils::data_hash32(bytecode.data(), bytecode_size);

			if (const auto it = hash_cache_.find(hash); it != hash_cache_.end()) {
				return &insert_ptr_alias(shader, it->second);
			}

			VsDesc desc = parse(bytecode.data(), hash);
			hash_cache_[hash] = desc;
			return &insert_ptr_alias(shader, desc);
		}

	private:
		VsDesc& insert_ptr_alias(IDirect3DVertexShader9* shader, const VsDesc& desc) {
			const auto [it, inserted] = ptr_cache_.try_emplace(shader, desc);
			if (inserted) {
				shader->AddRef();
			}
			return it->second;
		}

		static VsDesc parse(const BYTE* bytecode, uint32_t hash) {
			VsDesc d;
			d.vs_hash = hash;

			ID3DXConstantTable* table = nullptr;
			const HRESULT hr = D3DXGetShaderConstantTable(reinterpret_cast<const DWORD*>(bytecode), &table);
			if (FAILED(hr) || !table) {
				log("VSReflect", std::format("no constant table in VS 0x{:08X} (hr=0x{:08X}): no family by name",
					hash, static_cast<unsigned>(hr)), LOG_TYPE::LOG_TYPE_WARN);
				return d;
			}
			d.has_table = true;

			D3DXCONSTANTTABLE_DESC tdesc = {};
			table->GetDesc(&tdesc);

			bool has_diffuse_dir = false;
			std::string names;   // "Name@cN[xK]" for the unclassified-VS log line
			for (UINT i = 0; i < tdesc.Constants; i++) {
				D3DXHANDLE h = table->GetConstant(nullptr, i);
				if (!h) continue;
				D3DXCONSTANT_DESC c = {};
				UINT count = 1;
				if (FAILED(table->GetConstantDesc(h, &c, &count))) continue;
				if (c.RegisterSet != D3DXRS_FLOAT4 || !c.Name || c.RegisterIndex > 255) continue;

				const auto reg = static_cast<uint8_t>(c.RegisterIndex);
				const auto cnt = static_cast<uint8_t>(c.RegisterCount > 255 ? 255 : c.RegisterCount);
				const char* n = c.Name;
				names += std::format("{}{}@c{}{}", names.empty() ? "" : " ", n, reg,
					cnt > 1 ? std::format("x{}", cnt) : std::string());

				if      (!std::strcmp(n, "ModelViewProj"))    d.reg_mvp = reg;
				else if (!std::strcmp(n, "SubTexOffsets"))    { d.reg_sub_tex = reg; d.sub_tex_count = cnt; }
				else if (!std::strcmp(n, "TexCoordTranform")) d.reg_tc_xform = reg;
				else if (!std::strcmp(n, "InstanceData"))     d.reg_instance = reg;
				else if (!std::strcmp(n, "ScaleMask"))        d.reg_scale_mask = reg;
				else if (!std::strcmp(n, "WindData"))         d.reg_wind = reg;
				else if (!std::strcmp(n, "AlphaParam"))       d.reg_alpha_param = reg;
				else if (!std::strcmp(n, "EyePos"))           d.reg_eye_pos = reg;
				else if (!std::strcmp(n, "LeafBase"))         { d.reg_leaf_base = reg; d.leaf_base_count = cnt; }
				else if (!std::strcmp(n, "BillboardRight"))   d.reg_bb_right = reg;
				else if (!std::strcmp(n, "BillboardUp"))      d.reg_bb_up = reg;
				else if (!std::strcmp(n, "TexScale"))         d.reg_tex_scale = reg;
				else if (!std::strcmp(n, "DiffuseDir"))       has_diffuse_dir = true;
			}
			table->Release();

			// Most specific family first. Each family requires
			// every register its expander reads, so a descriptor that names a
			// family is always safe to expand.
			if (d.has(d.reg_sub_tex) && d.sub_tex_count > 0) {
				d.family = VsFamily::ParticleSubTex;
			}
			else if (d.has(d.reg_tc_xform)) {
				d.family = VsFamily::NoLightUv;
			}
			else if (d.has(d.reg_instance) && d.has(d.reg_scale_mask) &&
				d.has(d.reg_mvp) && d.has(d.reg_alpha_param)) {
				d.family = VsFamily::Grass;
			}
			else if (d.has(d.reg_instance) && d.has(d.reg_eye_pos) && d.has(d.reg_alpha_param)) {
				d.family = VsFamily::DistLod;
			}
			// DISTTREE declares InstanceData with neither ScaleMask nor
			// AlphaParam. PARTICLE.vso also declares InstanceData (GPU particle
			// simulation, not reproducible here) but no DiffuseDir, which is
			// what tells the two apart.
			else if (d.has(d.reg_instance) && has_diffuse_dir && d.has(d.reg_mvp)) {
				d.family = VsFamily::DistTree;
			}
			else if (d.has(d.reg_leaf_base) && d.leaf_base_count > 0 &&
				d.has(d.reg_bb_right) && d.has(d.reg_bb_up)) {
				d.family = VsFamily::Leaf;
			}

			if (d.family != VsFamily::None) {
				log("VSReflect", std::format(
					"VS 0x{:08X}: family={} mvp=c{} subTex=c{}x{} tcXform=c{} inst=c{} scale=c{} "
					"alpha=c{} eye=c{} leafBase=c{}x{} bbR=c{} bbU=c{}",
					hash, vs_family_name(d.family), d.reg_mvp, d.reg_sub_tex, d.sub_tex_count,
					d.reg_tc_xform, d.reg_instance, d.reg_scale_mask, d.reg_alpha_param,
					d.reg_eye_pos, d.reg_leaf_base, d.leaf_base_count, d.reg_bb_right, d.reg_bb_up),
					LOG_TYPE::LOG_TYPE_GREEN);
			}
			else {
				// Every other VS once, with its constants: an FX draw that still
				// renders wrong (e.g. a flipbook showing its whole atlas) names its
				// shader here, and the constant names say what math it does.
				log("VSReflect", std::format("VS 0x{:08X}: unclassified | {}", hash, names),
					LOG_TYPE::LOG_TYPE_DEFAULT);
			}
			return d;
		}

		std::unordered_map<uint32_t, VsDesc> hash_cache_;
		std::unordered_map<IDirect3DVertexShader9*, VsDesc> ptr_cache_;
	};

	// SetVertexShader fills the cache on first sight; the draw paths look up
	// the bound VS per draw (a pointer-keyed hit).
	inline VsShaderClassifier g_vs_classifier;
}
