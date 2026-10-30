// Pixel shader sampler-name classifier. Each unique pixel shader (by
// bytecode hash) is parsed with D3DXGetShaderConstantTable; each declared
// sampler's name maps to a role (Diffuse, Normal, Glow, Height, Decal,
// Other), and the role -> sampler slot table is cached per PS hash, together
// with the few named constants the routes need. The renderer and the API
// paths read it per draw to pick the albedo for FFP, the slots dxvk-remix
// reads from device state, and the RS 149 payload (remix_protocol.hpp).

#pragma once

#include <fstream>

#include "shared/common/console.hpp"

namespace shared::common
{
	enum class PsSlotRole : uint8_t {
		Other   = 0,
		Diffuse = 1,
		Normal  = 2,
		Glow    = 3,
		Height  = 4,
		Decal   = 5,
		Count   = 6,
	};

	// Per-PS slot-role map. slot_for_role[role] holds the D3D9 sampler stage
	// (0..7) that the PS declared for that role, or 0xF if the PS does not
	// have a sampler in that role. any_classified is true iff at least one
	// role was identified.
	struct PsSlotMap {
		static constexpr uint8_t kNoSlot = 0xF;
		uint8_t slot_for_role[static_cast<size_t>(PsSlotRole::Count)] = {
			kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot
		};
		bool any_classified = false;
		// Bytecode hash of the PS this map was parsed from (0 if unset).
		uint32_t ps_hash = 0;
		// True iff the PS declares any sampler whose name starts with "LOD"
		// (e.g. LODLandNoise, LODParentNormals, LODParentTex). FNV's distant-
		// terrain shaders do their own UV math to sample an LOD atlas at s0, so
		// s0 taken as plain albedo tiles the atlas across each terrain tile.
		// The renderer sends these to the far-LOD or near-LOD route instead.
		bool has_lod_sampler = false;
		// LODLandNoise's stage (SLS2003 / 2006: s7, SLS2082: s2), or kNoSlot.
		uint8_t lod_noise_slot = kNoSlot;

		// True iff the PS belongs to FNV's WATER shader family.
		//
		// No WATER variant declares BaseMap / DiffuseMap / TexMap, so water has
		// no Diffuse role and needs this signal of its own.
		//
		// Across all 16 shipped shaderpackage*.sdp files, ReflectionMap,
		// RefractionMap, DepthMap and DisplacementMap are declared only by the
		// WATER family. NoiseMap is shared with two ImageSpace shaders
		// (ISNOISENORMALMAP, ISNOISESCROLLANDBLEND), which bind it at s0, while
		// WATER always binds it at s2, so the slot tells them apart.
		bool has_water_sampler = false;

		// Samplers the PS declares (0 for vertex-colour-only shaders such as
		// NOLIGHTVCONLY, FNV's baked fake shadows; see game::should_skip_fake_shadow).
		// kUnknownSamplers when the constant table couldn't be read.
		static constexpr uint8_t kUnknownSamplers = 0xFF;
		uint8_t sampler_count = kUnknownSamplers;

		// The slot WATER declared NoiseMap at (s2 in every shipped variant).
		// Water's other samplers (ReflectionMap, RefractionMap, DepthMap) are
		// render targets whose contents and hashes change every frame, so no
		// Remix material replacement could key on them. NoiseMap is water's
		// only static texture: both the albedo and the stable hash a translucent
		// water material is keyed on. kNoSlot when the variant declares no
		// NoiseMap; the renderer then does not take the water route.
		uint8_t water_albedo_slot = kNoSlot;

		// Multi-layer terrain (PS declares BaseMap[N] for N >= 2, optionally
		// with a matching NormalMap[N] at samplers s7..s(7+N-1)). 0 means "not
		// multi-layer terrain"; values 2-7 mean "N layers detected."
		uint8_t multi_layer_count = 0;

		// True when BaseMap[N>=2] is declared with a NormalMap whose
		// layout doesn't line up (different N, or NormalMap doesn't start at
		// s7). multi_layer_count is 0 in that case.
		bool multi_layer_ambiguous = false;

		// Declared sampler-register counts of the Diffuse-role BaseMap and of
		// NormalMap (1 for a plain sampler, N for FNV's terrain arrays), and
		// the first register of NormalMap. The land detector reads these; a
		// single-layer land PS is BaseMap s0 x1 + NormalMap s7 x1, which the
		// multi-layer fields above don't describe.
		uint8_t base_map_count = 0;
		uint8_t normal_map_count = 0;
		uint8_t normal_map_reg = kNoSlot;

		// Float-constant registers of the few PS inputs the FFP routes have to
		// reproduce themselves, resolved by name from the CTAB (kNoReg when the
		// PS doesn't declare one):
		//   AlphaTestRef:  GRASS / DISTLOD cutout threshold (x, 0..1). The PS
		//                  writes alpha = (tex.a > ref), so FFP needs the same
		//                  compare as ALPHAFUNC=GREATER / ALPHAREF.
		//   MaterialColor: NOLIGHT* (particles / unlit FX) output multiplier:
		//                  out = tex * vertexColour * MaterialColor.
		static constexpr uint8_t kNoReg = 0xFF;
		uint8_t alpha_test_ref_reg = kNoReg;
		uint8_t material_color_reg = kNoReg;

		// Material inputs, resolved by name so a package with a different
		// register layout still resolves:
		//   Toggles:        lit SLS/PAR/SKIN: z = Blinn-Phong glossiness
		//                   exponent (PS c27 in package 004). Not EnvToggles.
		//   EmittanceColor: glow tint for GlowMap (PS c2 in package 004).
		uint8_t toggles_reg = kNoReg;
		uint8_t emittance_reg = kNoReg;
		//   AmbientColor:   lit SLS/SKIN: .w is the material / fade alpha
		//                   (SLS2000 / 2017 / 2031: oC0.a = BaseMap.a * c1.w,
		//                   and the Toggles.w texkill only runs while
		//                   c1.w < 1), so an actor with c1.w = 1 is opaque
		//                   even when the engine left SrcAlpha blending on.
		uint8_t ambient_color_reg = kNoReg;
		//   HairTint:       hair (SM3002...): colour = lerp(Base, Layer) x (1 + v0.g (2 HairTint - 1))
		uint8_t hair_tint_reg = kNoReg;

		// WATER family constants. Every water surface shader (WATER000-034)
		// declares ShallowColor + DeepColor, including WATER034 (distant
		// water), which declares no sampler at all and is invisible to the
		// sampler-based has_water_sampler signal above. WATER035 (mask pass)
		// declares no constants; WATER036/037 (additive point-light specular
		// passes) declare LightPos/FresnelRI but no ShallowColor.
		uint8_t shallow_color_reg = kNoReg;
		uint8_t deep_color_reg = kNoReg;
		uint8_t reflection_color_reg = kNoReg;
		uint8_t var_amounts_reg = kNoReg;
		bool is_water_surface = false;
		bool is_water_light_pass = false;

		// FaceGen maps (SKIN2000-2003 / 2010-2012: FaceGenMap0 at s2,
		// FaceGenMap1 at s3; SLS1017 names them FaceGenMap / FaceGenMap2). The
		// engine draws FaceGen heads, bodies and hands with these PSes: the
		// albedo is 4 * FaceGen1 * (BaseMap + 2 * FaceGen0 - 1), FaceGen0 the
		// per-NPC base mod (0.5 neutral), FaceGen1 the race / age detail map
		// (0.25 neutral). FFP shows BaseMap alone; the skinned API path builds
		// the full albedo (material::Desc::facegen0).
		uint8_t facegen0_slot = kNoSlot;
		uint8_t facegen1_slot = kNoSlot;
		// The hair PS's LayerMap (SM3002...: s5). Its colour replaces the
		// BaseMap's by its alpha: lerp(Base, Layer, Layer.a) (hair_tint_reg).
		uint8_t hair_layer_slot = kNoSlot;

		// NOLIGHTTEXVCPMA (flames): oC0.rgb = tex * COLOR * MaterialColor
		// premultiplied by that product's alpha, oC0.a = 1, blended ONE / ONE.
		// FFP gives the runtime the unmultiplied colour and the alpha apart;
		// the renderer turns the draw's SRCBLEND ONE into SRCALPHA so the sum
		// stays the same and the alpha mask reaches the runtime.
		bool premultiplied_alpha = false;

		uint8_t slot(PsSlotRole role) const {
			return slot_for_role[static_cast<size_t>(role)];
		}
		bool has(PsSlotRole role) const {
			return slot(role) != kNoSlot;
		}
	};

	// Caches PS_hash -> PsSlotMap. classify() returns a stable pointer into
	// the cache; null when the shader is null. Not thread-safe; the wrapper
	// drives all D3D9 traffic on the render thread.
	class PsShaderClassifier {
	public:
		// No destructor calls clear(). The global instance is destroyed during
		// static teardown, which can run after the D3D9 device and the Remix
		// bridge DLL are gone; releasing the cached shaders there crashes on
		// exit, so the process reclaims them instead.

		// Returns a stable pointer to the cached PsSlotMap for this shader,
		// classifying it on first sight. Returns nullptr if shader is null.
		const PsSlotMap* classify(IDirect3DPixelShader9* shader) {
			if (!shader) return nullptr;

			// Pointer cache first; PS pointers are reused across draws.
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

			PsSlotMap map = parse_samplers(bytecode.data(), hash);
			hash_cache_[hash] = map;
			return &insert_ptr_alias(shader, map);
		}

		// Drops every reference the pointer cache owns. Not called anywhere
		// (see above); only safe while the D3D9 device is alive, and
		// invalidates every map handed out by classify().
		void clear() {
			for (auto& [shader, map] : ptr_cache_) {
				if (shader) shader->Release();
			}
			ptr_cache_.clear();
			hash_cache_.clear();
		}

	private:
		// Adds the (shader pointer -> map) alias, taking a reference on the
		// shader. The pointer is the cache key, so if the game released a
		// shader and the allocator reused its address for another one, later
		// lookups would return the old shader's slot map. Owning a reference
		// keeps the address unique for the process lifetime. FNV creates a few
		// hundred pixel shaders, so keeping them all alive is cheap.
		PsSlotMap& insert_ptr_alias(IDirect3DPixelShader9* shader, const PsSlotMap& map) {
			const auto [it, inserted] = ptr_cache_.try_emplace(shader, map);
			if (inserted) {
				shader->AddRef();
			}
			return it->second;
		}

		static PsSlotRole role_for_name(const char* name) {
			if (!name) return PsSlotRole::Other;
			if (std::strcmp(name, "BaseMap") == 0)    return PsSlotRole::Diffuse;
			if (std::strcmp(name, "DiffuseMap") == 0) return PsSlotRole::Diffuse;
			if (std::strcmp(name, "TexMap") == 0)     return PsSlotRole::Diffuse;
			if (std::strcmp(name, "NormalMap") == 0)  return PsSlotRole::Normal;
			if (std::strcmp(name, "GlowMap") == 0)    return PsSlotRole::Glow;
			if (std::strcmp(name, "HeightMap") == 0)  return PsSlotRole::Height;
			// Decal samplers tag the draw via RS 42 InstanceCategories::DecalStatic;
			// the slot itself isn't routed (FFP rasterises BaseMap only), but the
			// path tracer needs the category to apply decal placement / blending.
			if (std::strcmp(name, "DecalMap") == 0)   return PsSlotRole::Decal;
			if (std::strcmp(name, "Decal2Map") == 0)  return PsSlotRole::Decal;
			return PsSlotRole::Other;
		}

		static PsSlotMap parse_samplers(const BYTE* bytecode, uint32_t hash) {
			PsSlotMap map;
			map.ps_hash = hash;

			ID3DXConstantTable* table = nullptr;
			HRESULT hr = D3DXGetShaderConstantTable(
				reinterpret_cast<const DWORD*>(bytecode), &table);
			if (FAILED(hr) || !table) {
				log("PSReflect",
					std::format("PS 0x{:08X}: D3DXGetShaderConstantTable failed (hr=0x{:08X})",
						hash, static_cast<unsigned>(hr)),
					LOG_TYPE::LOG_TYPE_WARN);
				return map;
			}

			D3DXCONSTANTTABLE_DESC tdesc = {};
			table->GetDesc(&tdesc);

			std::string summary;
			UINT sampler_count = 0;

			// WATER binds NoiseMap here in every shipped variant; the two
			// ImageSpace shaders that also declare a NoiseMap bind it at s0.
			constexpr uint32_t kWaterNoiseSlot = 2;
			uint8_t noise_slot = PsSlotMap::kNoSlot;
			bool has_light_pos = false;
			bool has_fresnel = false;
			bool has_attenuation_map = false;

			for (UINT i = 0; i < tdesc.Constants; i++) {
				D3DXHANDLE h = table->GetConstant(nullptr, i);
				if (!h) continue;

				D3DXCONSTANT_DESC cdesc = {};
				UINT count = 1;
				if (FAILED(table->GetConstantDesc(h, &cdesc, &count))) continue;

				if (cdesc.RegisterSet == D3DXRS_FLOAT4 && cdesc.Name && cdesc.RegisterIndex < 32) {
					const auto reg = static_cast<uint8_t>(cdesc.RegisterIndex);
					const char* n = cdesc.Name;
					if      (std::strcmp(n, "AlphaTestRef") == 0)    map.alpha_test_ref_reg = reg;
					else if (std::strcmp(n, "MaterialColor") == 0)   map.material_color_reg = reg;
					else if (std::strcmp(n, "Toggles") == 0)         map.toggles_reg = reg;
					else if (std::strcmp(n, "EmittanceColor") == 0)  map.emittance_reg = reg;
					else if (std::strcmp(n, "AmbientColor") == 0)    map.ambient_color_reg = reg;
					else if (std::strcmp(n, "HairTint") == 0)        map.hair_tint_reg = reg;
					else if (std::strcmp(n, "ShallowColor") == 0)    map.shallow_color_reg = reg;
					else if (std::strcmp(n, "DeepColor") == 0)       map.deep_color_reg = reg;
					else if (std::strcmp(n, "ReflectionColor") == 0) map.reflection_color_reg = reg;
					else if (std::strcmp(n, "VarAmounts") == 0)      map.var_amounts_reg = reg;
					else if (std::strcmp(n, "LightPos") == 0)        has_light_pos = true;
					else if (std::strcmp(n, "FresnelRI") == 0)       has_fresnel = true;
				}

				if (cdesc.RegisterSet != D3DXRS_SAMPLER) continue;

				const uint32_t slot = cdesc.RegisterIndex;
				if (slot >= 8) continue;  // D3D9 has 8 PS sampler stages

				const PsSlotRole role = role_for_name(cdesc.Name);

				if (!summary.empty()) summary += " ";
				summary += std::format("s{}={}", slot, cdesc.Name ? cdesc.Name : "<null>");
				sampler_count++;

				// LOD samplers by name prefix. They have no role of their own;
				// they mark the draw as distant land for the renderer.
				if (cdesc.Name && std::strncmp(cdesc.Name, "LOD", 3) == 0) {
					map.has_lod_sampler = true;
					if (std::strcmp(cdesc.Name, "LODLandNoise") == 0) map.lod_noise_slot = static_cast<uint8_t>(slot);
				}

				// WATER family (see PsSlotMap::has_water_sampler), independent of
				// the role check since water declares no Diffuse-role sampler.
				if (cdesc.Name) {
					if (std::strcmp(cdesc.Name, "ReflectionMap") == 0 ||
						std::strcmp(cdesc.Name, "RefractionMap") == 0 ||
						std::strcmp(cdesc.Name, "DepthMap") == 0 ||
						std::strcmp(cdesc.Name, "DisplacementMap") == 0) {
						map.has_water_sampler = true;
					}
					else if (std::strcmp(cdesc.Name, "NoiseMap") == 0) {
						noise_slot = static_cast<uint8_t>(slot);
						if (slot == kWaterNoiseSlot) map.has_water_sampler = true;
					}
					else if (std::strcmp(cdesc.Name, "FaceGenMap0") == 0 ||
						std::strcmp(cdesc.Name, "FaceGenMap") == 0) {
						map.facegen0_slot = static_cast<uint8_t>(slot);
					}
					else if (std::strcmp(cdesc.Name, "FaceGenMap1") == 0 ||
						std::strcmp(cdesc.Name, "FaceGenMap2") == 0) {
						map.facegen1_slot = static_cast<uint8_t>(slot);
					}
					else if (std::strcmp(cdesc.Name, "LayerMap") == 0) {
						map.hair_layer_slot = static_cast<uint8_t>(slot);
					}
					else if (std::strcmp(cdesc.Name, "AttenuationMap") == 0) {
						has_attenuation_map = true;
					}
				}

				if (role == PsSlotRole::Other) continue;

				// The first claim wins per role. With FNV's shaders each role is
				// declared once; the log line lists every declaration.
				const size_t idx = static_cast<size_t>(role);
				if (map.slot_for_role[idx] == PsSlotMap::kNoSlot) {
					map.slot_for_role[idx] = static_cast<uint8_t>(slot);
					map.any_classified = true;
				}

				// Multi-layer terrain: BaseMap is a sampler array (RegisterCount
				// > 1). FNV terrain declares BaseMap[N] at s0 and NormalMap[N] at
				// s7; the NormalMap alignment is checked after the loop.
				if (role == PsSlotRole::Diffuse && cdesc.RegisterCount > 1 && map.multi_layer_count == 0) {
					map.multi_layer_count = static_cast<uint8_t>(cdesc.RegisterCount);
				}

				// Register counts of the first-claimed BaseMap / NormalMap.
				const auto rc = static_cast<uint8_t>(cdesc.RegisterCount > 8 ? 8 : cdesc.RegisterCount);
				if (role == PsSlotRole::Diffuse && map.base_map_count == 0 &&
					map.slot_for_role[idx] == slot) {
					map.base_map_count = rc;
				}
				if (role == PsSlotRole::Normal && map.normal_map_count == 0 &&
					map.slot_for_role[idx] == slot) {
					map.normal_map_count = rc;
					map.normal_map_reg = static_cast<uint8_t>(slot);
				}
			}

			map.is_water_surface = map.shallow_color_reg != PsSlotMap::kNoReg
				&& map.deep_color_reg != PsSlotMap::kNoReg;
			map.is_water_light_pass = !map.is_water_surface && has_light_pos && has_fresnel;

			// Water's albedo is its NoiseMap, the only sampler in the family
			// that isn't a per-frame render target. A WATER variant with no
			// NoiseMap leaves this at kNoSlot and the renderer declines the
			// water route rather than guessing at a render target.
			if (map.has_water_sampler) {
				map.water_albedo_slot = noise_slot;
			}

			// Skin shaders (SKIN2002-2007; SM3008/009/020/021 and their LL
			// twins, the FaceGen heads): their GlowMap is the subsurface tint
			// the PS multiplies into its back / rim light terms, never output
			// on its own. As a Glow role the runtime would emit it at full
			// strength: a flat, reddish face and hands lit in the dark. Real
			// glow shaders declare EmittanceColor; across all 16 shipped
			// packages, GlowMap without it next to AttenuationMap or a
			// FaceGenMap is exactly these skin shaders.
			if (map.has(PsSlotRole::Glow) && map.emittance_reg == PsSlotMap::kNoReg &&
				(has_attenuation_map || map.facegen0_slot != PsSlotMap::kNoSlot ||
				 map.facegen1_slot != PsSlotMap::kNoSlot)) {
				map.slot_for_role[static_cast<size_t>(PsSlotRole::Glow)] = PsSlotMap::kNoSlot;
			}

			// NOLIGHTTEXVCPMA (see PsSlotMap::premultiplied_alpha): in all 16
			// shader packages the only PS that declares exactly DiffuseMap and
			// MaterialColor. The engine's PREMULT_ALPHA pass types decide
			// first (comp/game/engine_class.hpp); this answers outside them.
			map.premultiplied_alpha = tdesc.Constants == 2 && sampler_count == 1 &&
				map.material_color_reg != PsSlotMap::kNoReg && map.has(PsSlotRole::Diffuse);

			// SLS2146/2147 declare BaseMap[7] with no NormalMap at all; that is
			// a valid layout (no normal layers), not a misaligned one.
			if (map.multi_layer_count > 0 && map.normal_map_count > 0) {
				// NormalMap must be an array of the same count at s7.
				bool normalAligns = false;
				for (UINT i = 0; i < tdesc.Constants; i++) {
					D3DXHANDLE h2 = table->GetConstant(nullptr, i);
					if (!h2) continue;
					D3DXCONSTANT_DESC cd = {};
					UINT cnt = 1;
					if (FAILED(table->GetConstantDesc(h2, &cd, &cnt))) continue;
					if (cd.RegisterSet != D3DXRS_SAMPLER) continue;
					if (cd.Name && std::strcmp(cd.Name, "NormalMap") == 0
							&& cd.RegisterIndex == 7
							&& cd.RegisterCount == map.multi_layer_count) {
						normalAligns = true;
						break;
					}
				}
				if (!normalAligns) {
					log("PSReflect",
						std::format("PS 0x{:08X}: BaseMap[{}] declared but NormalMap[N] at s7 doesn't match -- multi-layer terrain disabled for this shader.",
							hash, map.multi_layer_count),
						LOG_TYPE::LOG_TYPE_WARN);
					map.multi_layer_ambiguous = true;
					map.multi_layer_count = 0;
				}
			}

			table->Release();

			map.sampler_count = static_cast<uint8_t>(sampler_count > 254 ? 254 : sampler_count);
			if (sampler_count == 0) {
				log("PSReflect",
					std::format("PS 0x{:08X}: no samplers declared{}", hash,
						map.is_water_surface ? " [water surface]" : ""),
					LOG_TYPE::LOG_TYPE_DEFAULT);
			} else {
				log("PSReflect",
					std::format("PS 0x{:08X} ({} samplers): {} [diff=s{} norm=s{} glow=s{} height=s{} decal=s{} lod={} water={} wtex=s{} mlc={} toggles=c{} emit=c{} wsurf={} wlight={} fg0=s{} fg1=s{}]",
						hash, sampler_count, summary,
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Diffuse)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Normal)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Glow)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Height)],
						map.slot_for_role[static_cast<size_t>(PsSlotRole::Decal)],
						map.has_lod_sampler ? '1' : '0',
						map.has_water_sampler ? '1' : '0',
						map.water_albedo_slot,
						map.multi_layer_count,
						map.toggles_reg, map.emittance_reg,
						map.is_water_surface ? 1 : 0, map.is_water_light_pass ? 1 : 0,
						map.facegen0_slot, map.facegen1_slot),
					LOG_TYPE::LOG_TYPE_GREEN);
			}

			return map;
		}

		std::unordered_map<uint32_t, PsSlotMap> hash_cache_;
		std::unordered_map<IDirect3DPixelShader9*, PsSlotMap> ptr_cache_;
	};

	// Queried per draw by the renderer and the API paths. SetPixelShader
	// fills the cache on first sight.
	inline PsShaderClassifier g_ps_classifier;
}
