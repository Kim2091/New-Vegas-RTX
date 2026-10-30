#include "std_include.hpp"
#include "ps_protocol.hpp"
#include "tex_pack.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/ps_reflect.hpp"
#include "shared/common/remix_protocol.hpp"

namespace comp::game
{
	/*
	 * Protocol-v2 payload for a PS the classifier recognised (see
	 * shared/common/remix_protocol.hpp for the wire format).
	 *
	 *   RS 149  slot-role nibbles, plus
	 *           bit16 (RGB tangent-space normal) whenever the PS has a
	 *           NormalMap (every FNV normal map is a DirectX 0.5-biased RGB
	 *           tangent-space map), and
	 *           bit17 (normal alpha = spec mask) for the lit SLS/PAR/SKIN
	 *           shaders, recognised by their Toggles constant. Terrain arrays
	 *           (BaseMap[N>1]) never carry a spec mask in the normal alpha.
	 *   RS 169  Toggles.z, the Blinn-Phong exponent, when the shader can show
	 *           specular at all (NormalMap present). Absent otherwise, so the
	 *           runtime keeps its default roughness for unlit / spec-less
	 *           materials instead of turning them uniformly glossy.
	 *   RS 177  EmittanceColor when the shader has a GlowMap.
	 *   RS 42   `categories` plus DecalStatic for Decal/Decal2Map shaders.
	 *
	 * Gloss and emittance are the only per-material values; the rest is
	 * per-shader, so consecutive draws of one material class cost no writes.
	 *
	 * `pack_height_slot`: stage holding a texture-pack height map the wrapper
	 * bound itself (see bind_pack_height), used when the PS has no HeightMap.
	 */
	static remix_protocol::Payload ps_protocol_payload(const shared::common::PsSlotMap& map, uint32_t categories,
		uint8_t pack_height_slot = shared::common::PsSlotMap::kNoSlot)
	{
		using shared::common::PsSlotRole;
		using shared::common::PsSlotMap;
		const auto& ffp = shared::common::ffp_state::get();
		const float* pc = ffp.ps_const_data();

		const uint8_t diffuseSlot = map.slot(PsSlotRole::Diffuse);
		const uint8_t normalSlot  = map.slot(PsSlotRole::Normal);
		const uint8_t glowSlot    = map.slot(PsSlotRole::Glow);
		const uint8_t heightSlot  = map.has(PsSlotRole::Height) ? map.slot(PsSlotRole::Height) : pack_height_slot;

		// Decal samplers (blood splatters, dirt overlays, bullet holes) get
		// tagged DecalStatic so the path tracer applies decal placement /
		// blending. The decal-slot textures themselves aren't routed: FFP
		// rasterises the BaseMap only and the protocol has no decal nibble.
		if (map.has(PsSlotRole::Decal))
			categories |= remix_protocol::category_mask(remix_protocol::CategoryBit::DecalStatic);

		remix_protocol::Payload p;
		p.categories = categories ? categories : remix_protocol::kSentinel;

		uint32_t modifier = remix_protocol::encode_slot_roles(diffuseSlot, normalSlot, glowSlot, heightSlot);
		const bool has_normal = normalSlot != PsSlotMap::kNoSlot;
		const bool lit_spec = map.toggles_reg != PsSlotMap::kNoReg && map.base_map_count <= 1;
		if (has_normal)
		{
			modifier |= remix_protocol::kNormalRgbTangentSpace;
			if (lit_spec) modifier |= remix_protocol::kNormalAlphaIsSpecMask;
		}
		p.modifier = modifier;

		if (has_normal && lit_spec)
		{
			const float gloss = pc[map.toggles_reg * 4 + 2];
			// The engine's NiMaterialProperty glossiness; FNV content sits in
			// ~5-200. Non-finite / non-positive values are left absent.
			if (std::isfinite(gloss) && gloss > 0.0f)
				p.gloss = remix_protocol::float_bits((std::min)(gloss, 4096.0f));
		}

		if (glowSlot != PsSlotMap::kNoSlot && map.emittance_reg != PsSlotMap::kNoReg)
		{
			const float* e = pc + map.emittance_reg * 4;
			auto u8 = [](float v) {
				if (!(v > 0.0f)) return 0u;
				return v >= 1.0f ? 255u : static_cast<unsigned>(v * 255.0f + 0.5f);
			};
			// Alpha forced to 255 so a black tint is a non-zero word: 0 would
			// read as "absent" (= white) on the runtime side.
			p.emittance = D3DCOLOR_ARGB(255, u8(e[0]), u8(e[1]), u8(e[2]));
		}
		return p;
	}

	/*
	 * Texture-pack height map ([TexturePacks] HeightMaps, comp/game/tex_pack.*).
	 *
	 * Vanilla binds a HeightMap only for the PAR* parallax shaders. For every
	 * other shader with a diffuse, a pack's "<diffuse>_p.dds" is bound at a
	 * stage 1..6 the PS doesn't use for any role and named in the RS 149 height
	 * nibble; the runtime turns it into POM. PAR shaders keep their own.
	 *
	 * Stage choice keeps bridge traffic flat: the stage that already holds this
	 * height map (sticky, see ffp_state::bind_extra_texture), else the highest
	 * stage the game left NULL (no restore needed), else the highest non-role
	 * stage (setup_albedo_texture has nulled it and restore_textures puts the
	 * game's texture back). Stage 7 is out: the runtime's fixed-function
	 * sampler mask never uploads it.
	 *
	 * Returns the stage used, or kNoSlot.
	 */
	static uint8_t bind_pack_height(IDirect3DDevice9* dev, const shared::common::PsSlotMap& map, uint8_t diffuseSlot)
	{
		using shared::common::PsSlotRole;
		using shared::common::PsSlotMap;
		if (map.has(PsSlotRole::Height) || !tex_pack::enabled()) return PsSlotMap::kNoSlot;

		auto& ffp = shared::common::ffp_state::get();
		IDirect3DBaseTexture9* diffuse = ffp.cur_texture(diffuseSlot);
		IDirect3DTexture9* height = tex_pack::height_for(diffuse);
		if (!height) return PsSlotMap::kNoSlot;

		unsigned roles = 1u;   // stage 0 = the rasterised albedo
		for (size_t r = 0; r < static_cast<size_t>(PsSlotRole::Count); r++)
		{
			const uint8_t sl = map.slot_for_role[r];
			if (sl < 8) roles |= 1u << sl;
		}

		int stage = ffp.extra_stage_holding(height);
		if (stage > 6 || (stage > 0 && (roles & (1u << stage)))) stage = -1;
		for (int s = 6; stage < 0 && s >= 1; s--)
			if (!(roles & (1u << s)) && !ffp.cur_texture(static_cast<UINT>(s))) stage = s;
		for (int s = 6; stage < 0 && s >= 1; s--)
			if (!(roles & (1u << s))) stage = s;
		if (stage < 0) return PsSlotMap::kNoSlot;

		ffp.bind_extra_texture(dev, static_cast<UINT>(stage), height);
		tex_pack::note_bound(diffuse, static_cast<UINT>(stage));
		return static_cast<uint8_t>(stage);
	}

	/*
	 * PS-classifier-driven protocol setup for an FFP-engaged draw.
	 *
	 * setup_albedo_texture() picks an albedo by per-decl-shape heuristic and
	 * NULLs the slots the PS doesn't name. FNV's pixel shaders keep their
	 * sampler names, so the bound PS's (sampler name -> slot) map is used to
	 * rebind stage 0 to the slot the PS labels "BaseMap" (or DiffuseMap /
	 * TexMap), with that slot's addressing and filters (move_albedo_sampler).
	 * That fixes FFP rasterisation for shaders whose slot 0 is a NormalMap and
	 * keeps the runtime's legacy material hash (always the stage-0 texture) on
	 * the diffuse. The NormalMap / GlowMap / HeightMap slots stay bound at
	 * their game stages, where the runtime reads them from device state. The
	 * protocol-v2 payload is sticky and shadowed: only changed slots cross the
	 * bridge.
	 *
	 * A PS without a diffuse role gets `categories` alone with RS 149 absent.
	 */
	void apply_ps_protocol(IDirect3DDevice9* dev, const shared::common::PsSlotMap* map,
		uint32_t categories, bool pack_height)
	{
		auto& ffp = shared::common::ffp_state::get();

		const uint8_t diffuseSlot = map && map->any_classified
			? map->slot(shared::common::PsSlotRole::Diffuse)
			: shared::common::PsSlotMap::kNoSlot;

		// Without a diffuse role there is nothing to override at slot 0: the
		// AlbedoStage pick stays bound and the runtime converts the draw like
		// upstream (RS 149 absent).
		if (diffuseSlot == shared::common::PsSlotMap::kNoSlot)
		{
			if (categories) remix_protocol::categories_only(dev, categories);
			else remix_protocol::none(dev);
			return;
		}

		// Rebind slot 0 to the actual diffuse texture. setup_albedo_texture
		// may already have bound this exact pointer via the AlbedoStage
		// heuristic (e.g. when diffuseSlot matches the picked stage, or the
		// same texture is bound at two slots), in which case the write is skipped.
		if (auto* tex = ffp.cur_texture(diffuseSlot)) {
			if (tex != ffp.last_albedo_stage0()) {
				dev->SetTexture(0, tex);
				// setup_albedo_texture marks stage 0 dirty only when its pick
				// differs from the game's binding. With AlbedoStage=0 the pick is
				// the game's binding, so without this the override would never be
				// restored and later passthrough draws would sample it at s0.
				ffp.note_stage0_override(tex);
			}
			// The diffuse's own addressing and filters go with it: the
			// runtime samples stage 0's texture with stage 0's sampler.
			ffp.move_albedo_sampler(dev, diffuseSlot);
		}

		// setup_albedo_texture(dev, psmap) skipped the null-write for the
		// normal / glow / height slots (they're in the kept-bound mask), so
		// the device already holds the correct textures there.
		const uint8_t packHeight = pack_height
			? bind_pack_height(dev, *map, diffuseSlot)
			: shared::common::PsSlotMap::kNoSlot;
		remix_protocol::submit(dev, ps_protocol_payload(*map, categories, packHeight));
	}
}
