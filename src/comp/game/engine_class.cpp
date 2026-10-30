#include "std_include.hpp"
#include "engine_class.hpp"
#include "scene_graph.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/vs_reflect.hpp"

#include <optional>

namespace comp::game::engine_class
{
	namespace
	{
		// FalloutNV.exe 1.4.0.525's pass enum (pass+4), from the names its
		// pass-name switch (BSShaderManager_GetPassName 0xB4F9D0, jump table
		// 0xB5342C) gives each value, in runs. 0x237 has no name: it is the
		// BSDistantTree property's render mode 15 pass (0xBB73F0), the
		// silhouette. The other unnamed values (0x158-0x159, 0x179-0x17E, and
		// the volumetric fog's 0x257-0x25B, which the fog draws outside the
		// dispatcher) stay Unknown. The land LOD passes are ADT_Mn (0xFE) and
		// ADT_MnShp (0x10B), which ShadowLightShader adds for LOD land
		// (0xBDE61C, 0xBDE65A) and sets the LOD constants for (0xB79206), and
		// LANDLO_A (0x230). ADT_OptLOD (0xFD) is an object pass.
		struct Run
		{
			uint16_t first, last;
			Pass pass;
			uint8_t flags;
		};
		constexpr Run kRuns[] = {
			{ 0x001, 0x015, Pass::Depth, 0 },   // BSSM_ZONLY .. BSSM_SELFILLUMALPHABLOCK_S
			{ 0x016, 0x03D, Pass::Grass, 0 },   // BSSM_GRASS_NOALPHA_DIRONLY_LF .. BSSM_GRASS_NOALPHA_1POINT_LFSPB
			{ 0x03E, 0x03F, Pass::Leaves, 0 },   // BSSM_LEAVES_AD .. BSSM_LEAVES_ADPt
			{ 0x040, 0x040, Pass::Fronds, 0 },   // BSSM_FRONDS
			{ 0x041, 0x041, Pass::Particle, 0 },   // BSSM_PARTICLE_PREPASS
			{ 0x042, 0x042, Pass::NoLighting, 0 },   // BSSM_NOLIGHTING
			{ 0x043, 0x043, Pass::NoLightingUntextured, 0 },   // BSSM_NOLIGHTING_VC
			{ 0x044, 0x044, Pass::NoLighting, 0 },   // BSSM_NOLIGHTING_TexVC
			{ 0x045, 0x045, Pass::NoLightingUntextured, 0 },   // BSSM_NOLIGHTING_MAT
			{ 0x046, 0x046, Pass::NoLighting, 0 },   // BSSM_NOLIGHTING_S
			{ 0x047, 0x047, Pass::NoLightingUntextured, 0 },   // BSSM_NOLIGHTING_VC_S
			{ 0x048, 0x048, Pass::NoLighting, 0 },   // BSSM_NOLIGHTING_TexVC_S
			{ 0x049, 0x049, Pass::NoLightingUntextured, 0 },   // BSSM_NOLIGHTING_MAT_S
			{ 0x04A, 0x04A, Pass::NoLighting, 0 },   // BSSM_NOLIGHTING_FALLOFF
			{ 0x04B, 0x04B, Pass::NoLightingUntextured, 0 },   // BSSM_NOLIGHTING_VC_FALLOFF
			{ 0x04C, 0x04C, Pass::NoLighting, 0 },   // BSSM_NOLIGHTING_TexVC_FALLOFF
			{ 0x04D, 0x04D, Pass::NoLightingUntextured, 0 },   // BSSM_NOLIGHTING_MAT_FALLOFF
			{ 0x04E, 0x04E, Pass::NoLighting, 0 },   // BSSM_NOLIGHTING_FALLOFF_S
			{ 0x04F, 0x04F, Pass::NoLightingUntextured, 0 },   // BSSM_NOLIGHTING_VC_FALLOFF_S
			{ 0x050, 0x050, Pass::NoLighting, 0 },   // BSSM_NOLIGHTING_TexVC_FALLOFF_S
			{ 0x051, 0x051, Pass::NoLightingUntextured, 0 },   // BSSM_NOLIGHTING_MAT_FALLOFF_S
			{ 0x052, 0x052, Pass::NoLightingPsys, 0 },   // BSSM_NOLIGHTING_PSYS
			{ 0x053, 0x053, Pass::ParticleSubTex, 0 },   // BSSM_NOLIGHTING_PSYS_SUBTEX_OFFSET
			{ 0x054, 0x054, Pass::NoLightingPsys, kPremultiplied },   // BSSM_NOLIGHTING_PSYS_PREMULT_ALPHA
			{ 0x055, 0x055, Pass::ParticleSubTex, kPremultiplied },   // BSSM_NOLIGHTING_PSYS_SUBTEX_OFFSET_PREMULT_ALPHA
			{ 0x056, 0x056, Pass::NoLightingPsys, 0 },   // BSSM_NOLIGHTING_STRIP_PSYS
			{ 0x057, 0x057, Pass::ParticleSubTex, 0 },   // BSSM_NOLIGHTING_STRIP_PSYS_SUBTEX
			{ 0x058, 0x067, Pass::Lit, 0 },   // BSSM_AMBIENT .. BSSM_AMBDIFFTEX_GVc
			{ 0x068, 0x068, Pass::Lit, kFaceGen },   // BSSM_AMBDIFFTEX_Fg
			{ 0x069, 0x06C, Pass::Lit, 0 },   // BSSM_AMBDIFFTEX_A .. BSSM_AMBDIFFTEX_GAVc
			{ 0x06D, 0x06E, Pass::Lit, kFaceGen },   // BSSM_AMBDIFFTEX_FgA .. BSSM_AMBDIFFTEX_GFgA
			{ 0x06F, 0x072, Pass::Lit, 0 },   // BSSM_AMBDIFFTEX_S .. BSSM_AMBDIFFTEX_SGVc
			{ 0x073, 0x073, Pass::Lit, kFaceGen },   // BSSM_AMBDIFFTEX_SFg
			{ 0x074, 0x077, Pass::Lit, 0 },   // BSSM_AMBDIFFTEX_SA .. BSSM_AMBDIFFTEX_SGAVc
			{ 0x078, 0x079, Pass::Lit, kFaceGen },   // BSSM_AMBDIFFTEX_SFgA .. BSSM_AMBDIFFTEX_SGFgA
			{ 0x07A, 0x07B, Pass::Lit, 0 },   // BSSM_AMBDIFFTEX_Sb .. BSSM_AMBDIFFTEX_SbF
			{ 0x07C, 0x07E, Pass::LightOnly, 0 },   // BSSM_AMBDIFFDIRANDPT .. BSSM_AMBDIFFDIRANDPT_Sb
			{ 0x07F, 0x084, Pass::Lit, 0 },   // BSSM_AMBDIFFTEX_F .. BSSM_AMBDIFFTEX_FAVc
			{ 0x085, 0x085, Pass::Lit, kFaceGen },   // BSSM_AMBDIFFTEX_FFg
			{ 0x086, 0x087, Pass::Lit, 0 },   // BSSM_AMBDIFFTEX_FGA .. BSSM_AMBDIFFTEX_FGAVc
			{ 0x088, 0x088, Pass::Lit, kFaceGen },   // BSSM_AMBDIFFTEX_FGFgA
			{ 0x089, 0x08E, Pass::Lit, 0 },   // BSSM_AMBDIFFTEX_SF .. BSSM_AMBDIFFTEX_SFAVc
			{ 0x08F, 0x08F, Pass::Lit, kFaceGen },   // BSSM_AMBDIFFTEX_SFFg
			{ 0x090, 0x091, Pass::Lit, 0 },   // BSSM_AMBDIFFTEX_SFGA .. BSSM_AMBDIFFTEX_SFGAVc
			{ 0x092, 0x092, Pass::Lit, kFaceGen },   // BSSM_AMBDIFFTEX_SFGFgA
			{ 0x093, 0x094, Pass::Land, 0 },   // BSSM_LANDAD .. BSSM_LANDAD_Shp
			{ 0x095, 0x097, Pass::Lit, 0 },   // BSSM_ADTS .. BSSM_ADTS_H
			{ 0x098, 0x098, Pass::Lit, kFaceGen },   // BSSM_ADTS_Fg
			{ 0x099, 0x09D, Pass::Lit, 0 },   // BSSM_ADTS_Px .. BSSM_ADTS_SH
			{ 0x09E, 0x09E, Pass::Lit, kFaceGen },   // BSSM_ADTS_SFg
			{ 0x09F, 0x0A2, Pass::Lit, 0 },   // BSSM_ADTS_Sb .. BSSM_ADTS_HShp
			{ 0x0A3, 0x0A3, Pass::Lit, kFaceGen },   // BSSM_ADTS_FgShp
			{ 0x0A4, 0x0A8, Pass::Lit, 0 },   // BSSM_ADTS_PxShp .. BSSM_ADTS_SHShp
			{ 0x0A9, 0x0A9, Pass::Lit, kFaceGen },   // BSSM_ADTS_SFgShp
			{ 0x0AA, 0x0AD, Pass::Lit, 0 },   // BSSM_ADTS_SbShp .. BSSM_ADTS2_H
			{ 0x0AE, 0x0AE, Pass::Lit, kFaceGen },   // BSSM_ADTS2_Fg
			{ 0x0AF, 0x0B3, Pass::Lit, 0 },   // BSSM_ADTS2_Px .. BSSM_ADTS2_SH
			{ 0x0B4, 0x0B4, Pass::Lit, kFaceGen },   // BSSM_ADTS2_SFg
			{ 0x0B5, 0x0B8, Pass::Lit, 0 },   // BSSM_ADTS2_Sb .. BSSM_ADTS2_HShp
			{ 0x0B9, 0x0B9, Pass::Lit, kFaceGen },   // BSSM_ADTS2_FgShp
			{ 0x0BA, 0x0BE, Pass::Lit, 0 },   // BSSM_ADTS2_PxShp .. BSSM_ADTS2_SHShp
			{ 0x0BF, 0x0BF, Pass::Lit, kFaceGen },   // BSSM_ADTS2_SFgShp
			{ 0x0C0, 0x0C4, Pass::Lit, 0 },   // BSSM_ADTS2_SbShp .. BSSM_ADT10_G
			{ 0x0C5, 0x0C5, Pass::Lit, kFaceGen },   // BSSM_ADT10_Fg
			{ 0x0C6, 0x0C7, Pass::Lit, 0 },   // BSSM_ADT10_S .. BSSM_ADT10_SG
			{ 0x0C8, 0x0C8, Pass::Lit, kFaceGen },   // BSSM_ADT10_SFg
			{ 0x0C9, 0x0CB, Pass::Lit, 0 },   // BSSM_ADT4 .. BSSM_ADT4_G
			{ 0x0CC, 0x0CC, Pass::Lit, kFaceGen },   // BSSM_ADT4_Fg
			{ 0x0CD, 0x0CE, Pass::Lit, 0 },   // BSSM_ADT4_S .. BSSM_ADT4_SG
			{ 0x0CF, 0x0CF, Pass::Lit, kFaceGen },   // BSSM_ADT4_SFg
			{ 0x0D0, 0x0D2, Pass::Lit, 0 },   // BSSM_ADTS10 .. BSSM_ADTS10_G
			{ 0x0D3, 0x0D3, Pass::Lit, kFaceGen },   // BSSM_ADTS10_Fg
			{ 0x0D4, 0x0D5, Pass::Lit, 0 },   // BSSM_ADTS10_S .. BSSM_ADTS10_SG
			{ 0x0D6, 0x0D6, Pass::Lit, kFaceGen },   // BSSM_ADTS10_SFg
			{ 0x0D7, 0x0DA, Pass::Lit, 0 },   // BSSM_AD2 .. BSSM_AD2_GPx
			{ 0x0DB, 0x0DB, Pass::Lit, kFaceGen },   // BSSM_AD2_Fg
			{ 0x0DC, 0x0DD, Pass::Lit, 0 },   // BSSM_AD2_S .. BSSM_AD2_SG
			{ 0x0DE, 0x0DE, Pass::Lit, kFaceGen },   // BSSM_AD2_SFg
			{ 0x0DF, 0x0E3, Pass::Lit, 0 },   // BSSM_AD2_Sb .. BSSM_AD2_GPxShp
			{ 0x0E4, 0x0E4, Pass::Lit, kFaceGen },   // BSSM_AD2_FgShp
			{ 0x0E5, 0x0E6, Pass::Lit, 0 },   // BSSM_AD2_SShp .. BSSM_AD2_SGShp
			{ 0x0E7, 0x0E7, Pass::Lit, kFaceGen },   // BSSM_AD2_SFgShp
			{ 0x0E8, 0x0EC, Pass::Lit, 0 },   // BSSM_AD2_SbShp .. BSSM_AD3_GPx
			{ 0x0ED, 0x0ED, Pass::Lit, kFaceGen },   // BSSM_AD3_Fg
			{ 0x0EE, 0x0EF, Pass::Lit, 0 },   // BSSM_AD3_S .. BSSM_AD3_SG
			{ 0x0F0, 0x0F0, Pass::Lit, kFaceGen },   // BSSM_AD3_SFg
			{ 0x0F1, 0x0F5, Pass::Lit, 0 },   // BSSM_AD3_Sb .. BSSM_AD3_GPxShp
			{ 0x0F6, 0x0F6, Pass::Lit, kFaceGen },   // BSSM_AD3_FgShp
			{ 0x0F7, 0x0F8, Pass::Lit, 0 },   // BSSM_AD3_SShp .. BSSM_AD3_SGShp
			{ 0x0F9, 0x0F9, Pass::Lit, kFaceGen },   // BSSM_AD3_SFgShp
			{ 0x0FA, 0x0FD, Pass::Lit, 0 },   // BSSM_AD3_SbShp .. BSSM_ADT_OptLOD
			{ 0x0FE, 0x0FE, Pass::LandLod, 0 },   // BSSM_ADT_Mn
			{ 0x0FF, 0x0FF, Pass::Lit, 0 },   // BSSM_ADT_G
			{ 0x100, 0x100, Pass::Lit, kFaceGen },   // BSSM_ADT_Fg
			{ 0x101, 0x106, Pass::Lit, 0 },   // BSSM_ADT_Px .. BSSM_ADT_SG
			{ 0x107, 0x107, Pass::Lit, kFaceGen },   // BSSM_ADT_SFg
			{ 0x108, 0x10A, Pass::Lit, 0 },   // BSSM_ADT_Sb .. BSSM_ADT_Shp
			{ 0x10B, 0x10B, Pass::LandLod, 0 },   // BSSM_ADT_MnShp
			{ 0x10C, 0x10C, Pass::Lit, 0 },   // BSSM_ADT_GShp
			{ 0x10D, 0x10D, Pass::Lit, kFaceGen },   // BSSM_ADT_FgShp
			{ 0x10E, 0x112, Pass::Lit, 0 },   // BSSM_ADT_PxShp .. BSSM_ADT_SGShp
			{ 0x113, 0x113, Pass::Lit, kFaceGen },   // BSSM_ADT_SFgShp
			{ 0x114, 0x117, Pass::Lit, 0 },   // BSSM_ADT_SbShp .. BSSM_ADT2_G
			{ 0x118, 0x118, Pass::Lit, kFaceGen },   // BSSM_ADT2_Fg
			{ 0x119, 0x11D, Pass::Lit, 0 },   // BSSM_ADT2_Px .. BSSM_ADT2_SG
			{ 0x11E, 0x11E, Pass::Lit, kFaceGen },   // BSSM_ADT2_SFg
			{ 0x11F, 0x122, Pass::Lit, 0 },   // BSSM_ADT2_Sb .. BSSM_ADT2_GShp
			{ 0x123, 0x123, Pass::Lit, kFaceGen },   // BSSM_ADT2_FgShp
			{ 0x124, 0x128, Pass::Lit, 0 },   // BSSM_ADT2_PxShp .. BSSM_ADT2_SGShp
			{ 0x129, 0x129, Pass::Lit, kFaceGen },   // BSSM_ADT2_SFgShp
			{ 0x12A, 0x12B, Pass::Lit, 0 },   // BSSM_ADT2_SbShp .. BSSM_ADT2_SHShp
			{ 0x12C, 0x132, Pass::LightOnly, 0 },   // BSSM_DIFFUSEDIR .. BSSM_DIFFUSEPT2
			{ 0x133, 0x133, Pass::LightOnly, kFaceGen },   // BSSM_DIFFUSEPT2_Fg
			{ 0x134, 0x135, Pass::LightOnly, 0 },   // BSSM_DIFFUSEPT2_Px .. BSSM_DIFFUSEPT2_S
			{ 0x136, 0x136, Pass::LightOnly, kFaceGen },   // BSSM_DIFFUSEPT2_SFg
			{ 0x137, 0x138, Pass::LightOnly, 0 },   // BSSM_DIFFUSEPT2_Sb .. BSSM_DIFFUSEPT2_Shp
			{ 0x139, 0x139, Pass::LightOnly, kFaceGen },   // BSSM_DIFFUSEPT2_FgShp
			{ 0x13A, 0x13B, Pass::LightOnly, 0 },   // BSSM_DIFFUSEPT2_PxShp .. BSSM_DIFFUSEPT2_SShp
			{ 0x13C, 0x13C, Pass::LightOnly, kFaceGen },   // BSSM_DIFFUSEPT2_SFgShp
			{ 0x13D, 0x13E, Pass::LightOnly, 0 },   // BSSM_DIFFUSEPT2_SbShp .. BSSM_DIFFUSEPT3
			{ 0x13F, 0x13F, Pass::LightOnly, kFaceGen },   // BSSM_DIFFUSEPT3_Fg
			{ 0x140, 0x141, Pass::LightOnly, 0 },   // BSSM_DIFFUSEPT3_Px .. BSSM_DIFFUSEPT3_S
			{ 0x142, 0x142, Pass::LightOnly, kFaceGen },   // BSSM_DIFFUSEPT3_SFg
			{ 0x143, 0x144, Pass::LightOnly, 0 },   // BSSM_DIFFUSEPT3_Sb .. BSSM_DIFFUSEPT3_Shp
			{ 0x145, 0x145, Pass::LightOnly, kFaceGen },   // BSSM_DIFFUSEPT3_FgShp
			{ 0x146, 0x147, Pass::LightOnly, 0 },   // BSSM_DIFFUSEPT3_PxShp .. BSSM_DIFFUSEPT3_SShp
			{ 0x148, 0x148, Pass::LightOnly, kFaceGen },   // BSSM_DIFFUSEPT3_SFgShp
			{ 0x149, 0x149, Pass::LightOnly, 0 },   // BSSM_DIFFUSEPT3_SbShp
			{ 0x14A, 0x14A, Pass::Lit, 0 },   // BSSM_TEXTURE
			{ 0x14B, 0x14B, Pass::Lit, kFaceGen },   // BSSM_TEXTURE_Fg
			{ 0x14C, 0x14F, Pass::Lit, 0 },   // BSSM_TEXTURE_H .. BSSM_TEXTURE_SVc
			{ 0x150, 0x150, Pass::Lit, kFaceGen },   // BSSM_TEXTURE_SFg
			{ 0x151, 0x153, Pass::Lit, 0 },   // BSSM_TEXTURE_Sb .. BSSM_TEXTURE_Px
			{ 0x154, 0x157, Pass::LightOnly, 0 },   // BSSM_SPECULARDIR .. BSSM_SPECULARPT_S
			{ 0x158, 0x159, Pass::Unknown, 0 },   // (unnamed)
			{ 0x15A, 0x177, Pass::LightOnly, 0 },   // BSSM_2x_SPECULARDIR .. BSSM_2x_SPECULARPT3_Sb
			{ 0x178, 0x178, Pass::Depth, 0 },   // BSSM_3XOCCLUSION
			{ 0x179, 0x17E, Pass::Unknown, 0 },   // (unnamed)
			{ 0x17F, 0x183, Pass::Lit, 0 },   // BSSM_3XLIGHTING .. BSSM_3XLIGHTING_Pxo
			{ 0x184, 0x185, Pass::Lit, kFaceGen },   // BSSM_3XLIGHTING_Fg .. BSSM_3XLIGHTING_SFg
			{ 0x186, 0x192, Pass::Lit, 0 },   // BSSM_3XLIGHTING_G .. BSSM_3XLIGHTING_PxoSpc
			{ 0x193, 0x194, Pass::Lit, kFaceGen },   // BSSM_3XLIGHTING_FgSpc .. BSSM_3XLIGHTING_SFgSpc
			{ 0x195, 0x1A1, Pass::Lit, 0 },   // BSSM_3XLIGHTING_GSpc .. BSSM_3XLIGHTING_PxoShp
			{ 0x1A2, 0x1A3, Pass::Lit, kFaceGen },   // BSSM_3XLIGHTING_FgShp .. BSSM_3XLIGHTING_SFgShp
			{ 0x1A4, 0x1B0, Pass::Lit, 0 },   // BSSM_3XLIGHTING_GShp .. BSSM_3XLIGHTING_PxoShpSpc
			{ 0x1B1, 0x1B2, Pass::Lit, kFaceGen },   // BSSM_3XLIGHTING_FgShpSpc .. BSSM_3XLIGHTING_SFgShpSpc
			{ 0x1B3, 0x1BA, Pass::Lit, 0 },   // BSSM_3XLIGHTING_GShpSpc .. BSSM_3XLIGHTING_VcSGShpSpc
			{ 0x1BB, 0x1BC, Pass::Depth, 0 },   // BSSM_3XSILHOUETTE .. BSSM_3XSILHOUETTE_Vc
			{ 0x1BD, 0x1C3, Pass::LightOnly, 0 },   // BSSM_3XENVMAP .. BSSM_3XENVMAP_EYE
			{ 0x1C4, 0x1DF, Pass::Depth, 0 },   // BSSM_3XLIGHTING_SIMPLESHADOW .. BSSM_3XLOCALMAP_CLEAR
			{ 0x1E0, 0x1E1, Pass::LightOnly, 0 },   // BSSM_3XTEXEFFECT .. BSSM_3XTEXEFFECT_S
			{ 0x1E2, 0x1EE, Pass::Depth, 0 },   // BSSM_RENDERNORMALS .. BSSM_LOCALMAP_CLEAR
			{ 0x1EF, 0x22F, Pass::Land, 0 },   // BSSM_LANDDIFF .. BSSM_LANDO_Am
			{ 0x230, 0x230, Pass::LandLod, 0 },   // BSSM_LANDLO_A
			{ 0x231, 0x234, Pass::Depth, 0 },   // BSSM_2x_SIMPLESHADOW .. BSSM_2x_SIMPLESHADOW_Sb
			{ 0x235, 0x235, Pass::Lit, 0 },   // BSSM_ADT_Sbb
			{ 0x236, 0x236, Pass::DistantTree, 0 },   // BSSM_DISTANT_TREE_BASE
			{ 0x237, 0x237, Pass::Depth, 0 },   // (unnamed)
			{ 0x238, 0x23E, Pass::Sky, 0 },   // BSSM_SKYBASEPRE .. BSSM_SKY_CLOUDS_UNDERWATER
			{ 0x23F, 0x240, Pass::WaterLod, 0 },   // BSSM_WATER_LOD .. BSSM_WATER_LOD_NO_REFLECTIONS
			{ 0x241, 0x241, Pass::Particle, 0 },   // BSSM_PARTICLE
			{ 0x242, 0x243, Pass::Effect, 0 },   // BSSM_BOLT .. BSSM_BEAM
			{ 0x244, 0x24B, Pass::LightOnly, 0 },   // BSSM_ENVMAP .. BSSM_2x_ENVMAP_EYE
			{ 0x24C, 0x24D, Pass::Decal, 0 },   // BSSM_GEOMDECAL .. BSSM_GEOMDECAL_S
			{ 0x24E, 0x256, Pass::LightOnly, 0 },   // BSSM_TEXEFFECT .. BSSM_FOG_Sb
			{ 0x257, 0x25B, Pass::Unknown, 0 },   // (unnamed)
			{ 0x25C, 0x283, Pass::Grass, 0 },   // BSSM_GRASS_DIRONLY_LF .. BSSM_GRASS_1POINT_LFSPB
			{ 0x284, 0x287, Pass::Depth, 0 },   // BSSM_GRASS_SHADOW_L .. BSSM_GRASS_SHADOW_LSB
			{ 0x288, 0x289, Pass::WaterStencil, 0 },   // BSSM_WATER_STENCIL .. BSSM_WATER_STENCIL_Vc
			{ 0x28A, 0x299, Pass::Water, 0 },   // BSSM_WATER_WADING .. BSSM_WATER_INTERIOR_WADING_NO_DEPTH_NO_REFLECTIONS
			{ 0x29A, 0x29A, Pass::WaterLight, 0 },   // BSSM_WATER_WADING_SPECULAR_LIGHTING
			{ 0x29B, 0x2AA, Pass::Water, 0 },   // BSSM_WATER_WADING_Vc .. BSSM_WATER_INTERIOR_WADING_NO_DEPTH_NO_REFLECTIONS_NO_REFRACTIONS_Vc
			{ 0x2AB, 0x2AB, Pass::WaterLight, 0 },   // BSSM_WATER_WADING_SPECULAR_LIGHTING_Vc
			{ 0x2AC, 0x2BB, Pass::Water, 0 },   // BSSM_WATER .. BSSM_WATER_INTERIOR_NODEPTH_NO_REFLECTIONS
			{ 0x2BC, 0x2BC, Pass::WaterLight, 0 },   // BSSM_WATER_SPECULAR_LIGHTING
			{ 0x2BD, 0x2CD, Pass::Water, 0 },   // BSSM_WATER_UNDERWATER .. BSSM_WATER_INTERIOR_NODEPTH_NO_REFLECTIONS_Vc
			{ 0x2CE, 0x2CE, Pass::WaterLight, 0 },   // BSSM_WATER_SPECULAR_LIGHTING_Vc
			{ 0x2CF, 0x2CF, Pass::Effect, 0 },   // BSSM_PRECIPITATION_RAIN
			{ 0x2D0, 0x2D5, Pass::Sky, 0 },   // BSSM_SKYBASEPOST .. BSSM_SELFILLUM_SKY_SKY_QUAD
			{ 0x2D6, 0x2D7, Pass::LightOnly, 0 },   // BSSM_SELFILLUMALPHA .. BSSM_SELFILLUMALPHA_S
			{ 0x2D8, 0x2EF, Pass::Depth, 0 },   // BSSM_WATER_DEPTH .. BSSM_NLWATER_DEPTH_AtVc_S
			{ 0x2F0, 0x2F1, Pass::Decal, 0 },   // BSSM_BLOOD_SPLATTER_FLARE .. BSSM_BLOOD_SPLATTER
			{ 0x2F2, 0x2F4, Pass::Depth, 0 },   // BSSM_SHADOWVOLUME_BACK .. BSSM_SHADOWVOLUME_WIRE
			{ 0x2F5, 0x2F5, Pass::Tile, 0 },   // BSSM_TILE
		};
		constexpr uint16_t kTypeCount = 0x2F6;
		constexpr uint32_t kShaderClassId = 0x68;   // BSShader+0x68
		constexpr uint32_t kFaceGenFlag1 = 1u << 10;   // BSShaderProperty flags1 FaceGen

		struct Entry
		{
			Pass pass = Pass::Unknown;
			uint8_t flags = 0;
		};

		struct Table
		{
			Entry e[kTypeCount];
			constexpr Table() : e()
			{
				for (const Run& r : kRuns)
					for (uint32_t t = r.first; t <= r.last && t < kTypeCount; t++)
						e[t] = Entry{ r.pass, r.flags };
			}
		};
		constexpr Table kTable;

		Draw g_draw;
		UINT g_draw_number = ~0u;
		UINT g_draw_frame = ~0u;
		// shader_class for g_draw; read on first ask.
		ShaderClass g_class = ShaderClass::Unknown;
		bool g_class_read = false;
	}

	const char* pass_name(Pass p)
	{
		switch (p)
		{
		case Pass::Depth:                return "Depth";
		case Pass::Lit:                  return "Lit";
		case Pass::LightOnly:            return "LightOnly";
		case Pass::Land:                 return "Land";
		case Pass::LandLod:              return "LandLod";
		case Pass::Grass:                return "Grass";
		case Pass::Leaves:               return "Leaves";
		case Pass::Fronds:               return "Fronds";
		case Pass::DistantTree:          return "DistantTree";
		case Pass::NoLighting:           return "NoLighting";
		case Pass::NoLightingUntextured: return "NoLightingUntextured";
		case Pass::NoLightingPsys:       return "NoLightingPsys";
		case Pass::ParticleSubTex:       return "ParticleSubTex";
		case Pass::Particle:             return "Particle";
		case Pass::Sky:                  return "Sky";
		case Pass::Water:                return "Water";
		case Pass::WaterLod:             return "WaterLod";
		case Pass::WaterLight:           return "WaterLight";
		case Pass::WaterStencil:         return "WaterStencil";
		case Pass::Decal:                return "Decal";
		case Pass::Effect:               return "Effect";
		case Pass::Tile:                 return "Tile";
		default:                         return "Unknown";
		}
	}

	const Draw& current()
	{
		const auto& ffp = shared::common::ffp_state::get();
		const UINT n = ffp.draw_call_count();
		const UINT f = ffp.frame_count();
		if (n == g_draw_number && f == g_draw_frame) return g_draw;
		g_draw_number = n;
		g_draw_frame = f;

		Draw d;
		d.geometry = scene_graph::current_geometry();
		if (d.geometry)
		{
			d.type = scene_graph::current_pass_type();
			if (d.type < kTypeCount)
			{
				d.pass = kTable.e[d.type].pass;
				d.flags = kTable.e[d.type].flags;
			}
		}
		g_draw = d;
		g_class_read = false;
		return g_draw;
	}

	ShaderClass shader_class()
	{
		const Draw& d = current();
		if (g_class_read) return g_class;
		g_class_read = true;
		g_class = ShaderClass::Unknown;
		uint32_t shader = 0, id = 0;
		// The engine is rendering the geometry: it and its shader are alive.
		if (d.geometry && scene_graph::read_u32(d.geometry + scene_graph::fnv::GEO_shader, shader) && shader &&
			scene_graph::read_u32(shader + kShaderClassId, id))
			g_class = static_cast<ShaderClass>(id);
		return g_class;
	}

	Tri has_base(const Draw& d)
	{
		switch (d.pass)
		{
		case Pass::Unknown:
			return Tri::Unknown;
		case Pass::Lit:
		case Pass::Land:
		case Pass::LandLod:
		case Pass::Grass:
		case Pass::Leaves:
		case Pass::Fronds:
		case Pass::DistantTree:
		case Pass::NoLighting:
		case Pass::NoLightingPsys:
		case Pass::ParticleSubTex:
		case Pass::Sky:
		case Pass::Water:
		case Pass::WaterLod:
			return Tri::Yes;
		default:
			// Depth, lighting-only, untextured, water light / stencil,
			// geometry decals and blood (their map is a decal), GPU
			// particles, bolts / beams / rain, tiles.
			return Tri::No;
		}
	}

	// ---- Engine versus shader ------------------------------------------------------

	namespace
	{
		constexpr const char* kRouteNames[] = {
			"water", "waterLod", "waterLight", "grass", "leaf", "distTree", "distLod",
			"particleSubTex", "unlit", "premultiplied", "land", "farLod", "nearLod", "base", "head",
		};
		static_assert(std::size(kRouteNames) == static_cast<size_t>(Route::Count));

		struct Counts
		{
			uint64_t engine = 0;          // decided by the engine
			uint64_t fallback = 0;        // no engine answer: decided by the shader
			uint64_t engine_only = 0;     // engine yes, shader no
			uint64_t shader_only = 0;     // engine no, shader yes
		};
		Counts g_counts[static_cast<size_t>(Route::Count)];
		UINT g_frames = 0;

		// What was logged already: one line per (route, pass type, VS, PS,
		// kind), at most kMaxLogged in a session.
		constexpr size_t kMaxLogged = 256;
		std::unordered_set<uint64_t> g_logged;
		uint64_t g_last_key = 0;

		uint32_t vs_hash()
		{
			const auto* d = shared::common::g_vs_classifier.classify(shared::common::ffp_state::get().last_vs());
			return d ? d->vs_hash : 0u;
		}

		uint32_t ps_hash()
		{
			const auto* m = shared::common::g_ps_classifier.classify(shared::common::ffp_state::get().last_ps());
			return m ? m->ps_hash : 0u;
		}

		void log_once(Route r, int kind, Tri engine, bool shader)
		{
			const auto& ffp = shared::common::ffp_state::get();
			const Draw& d = g_draw;
			uint64_t key = (static_cast<uint64_t>(r) << 56) ^ (static_cast<uint64_t>(kind) << 52) ^
				(static_cast<uint64_t>(d.type) << 36) ^
				(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ffp.last_vs())) << 4) ^
				static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ffp.last_ps()));
			if (key == g_last_key) return;
			g_last_key = key;
			if (g_logged.size() >= kMaxLogged || !g_logged.insert(key).second) return;

			const char* what = kind == 0 ? "no engine answer, the shader decided"
				: "engine and shader disagree, the engine decided";
			shared::common::log("EngineClass", std::format(
				"{}: {} -- engine {} (pass 0x{:03X} {}{}), shader {} (VS 0x{:08X} PS 0x{:08X})",
				kRouteNames[static_cast<size_t>(r)], what,
				engine == Tri::Unknown ? "-" : engine == Tri::Yes ? "yes" : "no",
				d.type == 0xFFFF ? 0u : static_cast<unsigned>(d.type), pass_name(d.pass),
				d.geometry ? "" : ", outside the dispatcher",
				shader ? "yes" : "no", vs_hash(), ps_hash()),
				kind == 0 ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}
	}

	bool decide(Route r, Tri engine, bool shader)
	{
		Counts& c = g_counts[static_cast<size_t>(r)];
		if (engine == Tri::Unknown)
		{
			c.fallback++;
			// Only a positive fallback is worth a line: a draw outside the
			// dispatcher that no route claims is the common case.
			if (shader) log_once(r, 0, engine, shader);
			return shader;
		}
		c.engine++;
		const bool yes = engine == Tri::Yes;
		if (yes != shader)
		{
			(yes ? c.engine_only : c.shader_only)++;
			log_once(r, 1, engine, shader);
		}
		return yes;
	}

	bool carries_base(const shared::common::PsSlotMap* psmap)
	{
		const bool shader = psmap && psmap->has(shared::common::PsSlotRole::Diffuse);
		return decide(Route::Base, has_base(current()), shader);
	}

	bool facegen_head(const shared::common::PsSlotMap* psmap)
	{
		using shared::common::PsSlotMap;
		const bool shader = psmap &&
			(psmap->facegen0_slot != PsSlotMap::kNoSlot || psmap->facegen1_slot != PsSlotMap::kNoSlot);
		const Draw& d = current();
		Tri engine = Tri::Unknown;
		if (d.known())
		{
			// An "Fg" pass binds the FaceGen maps; SkinShader draws heads
			// whose shade property carries FaceGen (flags1 0x400).
			bool head = d.facegen();
			if (!head && shader_class() == ShaderClass::Skin)
			{
				scene_graph::GeoRead g;
				scene_graph::ShadeFlags f;
				head = scene_graph::read_geo(d.geometry, g) &&
					scene_graph::read_shade_flags(g.prop(scene_graph::fnv::kPropShade), f) &&
					(f.flags1 & kFaceGenFlag1) != 0;
			}
			engine = head ? Tri::Yes : Tri::No;
		}
		return decide(Route::Head, engine, shader);
	}

	bool premultiplied(const shared::common::PsSlotMap* psmap)
	{
		const Draw& d = current();
		const bool shader = psmap && psmap->premultiplied_alpha;
		return decide(Route::Premultiplied,
			d.known() ? ((d.flags & kPremultiplied) ? Tri::Yes : Tri::No) : Tri::Unknown, shader);
	}

	bool land(bool shader)
	{
		const Draw& d = current();
		return decide(Route::Land, d.known() ? (d.pass == Pass::Land ? Tri::Yes : Tri::No) : Tri::Unknown, shader);
	}

	bool land_lod(const shared::common::PsSlotMap* psmap, bool with_normal)
	{
		const Draw& d = current();
		const bool shader = psmap && psmap->has_lod_sampler;
		return decide(with_normal ? Route::NearLod : Route::FarLod,
			d.known() ? (d.pass == Pass::LandLod ? Tri::Yes : Tri::No) : Tri::Unknown, shader);
	}

	Water water(const shared::common::PsSlotMap* psmap)
	{
		const Draw& d = current();
		const bool surface = psmap && psmap->is_water_surface;
		// Near variants declare the NoiseMap sampler; WATER034 (distant) declares none.
		const bool near_sampler = surface && psmap->has_water_sampler;
		const bool light = psmap && psmap->is_water_light_pass;
		const auto says = [&](Pass p) { return d.known() ? (d.pass == p ? Tri::Yes : Tri::No) : Tri::Unknown; };
		// Every question is asked, so each is counted.
		const bool is_near = decide(Route::Water, says(Pass::Water), near_sampler);
		const bool is_lod = decide(Route::WaterLod, says(Pass::WaterLod), surface && !near_sampler);
		const bool is_light = decide(Route::WaterLight, says(Pass::WaterLight), light);
		return is_near ? Water::Near : is_lod ? Water::Distant : is_light ? Water::Light : Water::None;
	}

	namespace
	{
		using shared::common::PsSlotMap;
		using shared::common::PsSlotRole;

		void set_slot(PsSlotMap& m, PsSlotRole r, uint8_t slot)
		{
			m.slot_for_role[static_cast<size_t>(r)] = slot;
			m.any_classified = true;
		}

		// The vanilla layout of a pass (shader packages 003-019; the role
		// names and registers are the same in every one).
		bool vanilla_layout(const Draw& d, PsSlotMap& m)
		{
			switch (d.pass)
			{
			case Pass::Lit:
				set_slot(m, PsSlotRole::Diffuse, 0);
				set_slot(m, PsSlotRole::Normal, 1);
				m.base_map_count = 1;
				m.normal_map_count = 1;
				m.normal_map_reg = 1;
				m.sampler_count = 2;
				// SKIN2000-2003 / SLS: FaceGenMap0 s2, FaceGenMap1 s3.
				if (d.facegen()) { m.facegen0_slot = 2; m.facegen1_slot = 3; m.sampler_count = 4; }
				return true;
			case Pass::LandLod:
				// SLS2003 / 2006 / 2082: BaseMap s0, the LOD noise / parent maps after it.
				set_slot(m, PsSlotRole::Diffuse, 0);
				m.base_map_count = 1;
				m.sampler_count = 1;
				m.has_lod_sampler = true;
				return true;
			case Pass::Grass:
			case Pass::Leaves:
			case Pass::Fronds:
			case Pass::DistantTree:
				set_slot(m, PsSlotRole::Diffuse, 0);
				m.base_map_count = 1;
				m.sampler_count = 1;
				// GRASS / DISTLOD AlphaTestRef.
				if (d.pass == Pass::Grass) m.alpha_test_ref_reg = 3;
				return true;
			case Pass::NoLighting:
			case Pass::NoLightingPsys:
			case Pass::ParticleSubTex:
				set_slot(m, PsSlotRole::Diffuse, 0);
				m.base_map_count = 1;
				m.sampler_count = 1;
				m.material_color_reg = 0;
				m.premultiplied_alpha = (d.flags & kPremultiplied) != 0;
				return true;
			case Pass::NoLightingUntextured:
				m.sampler_count = 0;
				m.material_color_reg = 0;
				return true;
			case Pass::Water:
				m.has_water_sampler = true;
				m.water_albedo_slot = 2;   // NoiseMap
				[[fallthrough]];
			case Pass::WaterLod:
				m.shallow_color_reg = 2;
				m.deep_color_reg = 3;
				m.reflection_color_reg = 4;
				m.var_amounts_reg = 8;
				m.is_water_surface = true;
				return true;
			case Pass::WaterLight:
				m.is_water_light_pass = true;
				return true;
			default:
				return false;
			}
		}

		// By (pass type, PS hash); empty when the pass has no vanilla layout.
		std::unordered_map<uint64_t, std::optional<PsSlotMap>> g_vanilla_maps;
		uint64_t g_last_map_key = ~0ull;
		const PsSlotMap* g_last_map = nullptr;
	}

	const shared::common::PsSlotMap* ps_map(const shared::common::PsSlotMap* reflected)
	{
		if (reflected && reflected->sampler_count != PsSlotMap::kUnknownSamplers) return reflected;
		const Draw& d = current();
		if (!d.known()) return reflected;

		const uint32_t hash = reflected ? reflected->ps_hash : 0u;
		const uint64_t key = (static_cast<uint64_t>(d.type) << 32) | hash;
		if (key == g_last_map_key) return g_last_map ? g_last_map : reflected;
		g_last_map_key = key;

		auto it = g_vanilla_maps.find(key);
		if (it == g_vanilla_maps.end())
		{
			PsSlotMap m;
			m.ps_hash = hash;
			const bool ok = vanilla_layout(d, m);
			it = g_vanilla_maps.emplace(key, ok ? std::optional<PsSlotMap>(m) : std::nullopt).first;
			shared::common::log("EngineClass", std::format(
				"PS 0x{:08X} has no constant table; pass 0x{:03X} ({}) {}", hash, d.type, pass_name(d.pass),
				ok ? "-- read with that pass's vanilla layout" : "has no vanilla layout here: the draw routes as an unknown shader"),
				ok ? shared::common::LOG_TYPE::LOG_TYPE_DEFAULT : shared::common::LOG_TYPE::LOG_TYPE_WARN);
		}
		g_last_map = it->second ? &*it->second : nullptr;
		return g_last_map ? g_last_map : reflected;
	}

	void on_present()
	{
		g_frames++;
		const int interval = shared::common::config::get().remix_api.stats_interval_frames;
		if (interval <= 0 || g_frames < static_cast<UINT>(interval)) return;

		const double frames = static_cast<double>(g_frames);
		std::string body;
		uint64_t engine = 0, fallback = 0, disagree = 0;
		for (size_t i = 0; i < static_cast<size_t>(Route::Count); i++)
		{
			Counts& c = g_counts[i];
			engine += c.engine;
			fallback += c.fallback;
			disagree += c.engine_only + c.shader_only;
			if (c.engine || c.fallback)
				body += std::format(" {}={:.0f}/{:.0f}{}", kRouteNames[i], c.engine / frames, c.fallback / frames,
					(c.engine_only || c.shader_only)
						? std::format(" (differ {:.1f}+{:.1f})", c.engine_only / frames, c.shader_only / frames)
						: std::string());
			c = Counts{};
		}
		shared::common::log("EngineClass", std::format(
			"decisions/frame over {} frames, engine/shader fallback per route:{} | total engine={:.0f} fallback={:.0f} "
			"differ={:.1f} (differ a+b: engine-only yes + shader-only yes)",
			g_frames, body.empty() ? std::string(" -") : body, engine / frames, fallback / frames, disagree / frames),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, false);
		g_frames = 0;
	}
}
