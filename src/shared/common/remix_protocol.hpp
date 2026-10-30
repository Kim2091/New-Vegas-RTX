// Writer for the D3D9 legacy material protocol. The FNV dxvk-remix runtime
// reads per-draw material metadata from four render-state slots the D3D9 spec
// leaves unused (runtime side: captureFnvLegacyProtocol).
//
//   RS  42  CategoryFlags  raw InstanceCategories bitmask, runtime INTERNAL bit order
//   RS 149  Modifier       slot-role nibbles + bit16 RGB tangent-space normal
//                          + bit17 normal-alpha-is-spec-mask
//   RS 169  Gloss          IEEE float bits of the Blinn-Phong exponent (Toggles.z)
//   RS 177  Emittance      D3DCOLOR glow tint (EmittanceColor)
//
// The slots are sticky device state. Every D3D9 call crosses the Remix
// bridge, so the wrapper keeps a shadow of what it last wrote and writes a
// slot only when its value changes; a slot is cleared (sentinel) only when a
// draw that must not carry it follows one that did. A run of draws of the
// same material class costs no writes.
//
// Absent: the runtime treats both 0x00000000 (the D3D9 default) and the
// sentinel 0xFEFEFEFE as "absent" for all four slots. RS 169/177 are only
// consulted while RS 149 is present, so non-protocol draws only need 42 and
// 149 absent and may leave 169/177 at whatever the last protocol draw wrote.
//
// Every draw the wrapper issues calls exactly one of:
//   submit(dev, payload)  protocol draw (RS 149 present)
//   ignore(dev)           draw tagged InstanceCategories::Ignore, RS 149 absent
//   categories_only(dev)  RS 42 only, RS 149 absent
//   none(dev)             plain draw: RS 42 and RS 149 absent
// immediately before the draw call. All are pure compares in steady state.

#pragma once

#include <d3d9.h>
#include <cstdint>
#include <cstring>

namespace remix_protocol {

	constexpr uint32_t kSentinel = 0xfefefefeu;

	// RS slot numbers of the protocol; they must match the runtime's
	// captureFnvLegacyProtocol.
	constexpr D3DRENDERSTATETYPE kRsCategoryFlags = static_cast<D3DRENDERSTATETYPE>(42);
	constexpr D3DRENDERSTATETYPE kRsModifier      = static_cast<D3DRENDERSTATETYPE>(149);
	constexpr D3DRENDERSTATETYPE kRsGloss         = static_cast<D3DRENDERSTATETYPE>(169);
	constexpr D3DRENDERSTATETYPE kRsEmittance     = static_cast<D3DRENDERSTATETYPE>(177);

	// RS 42 encoding: raw bitmask of InstanceCategories bit positions, mirroring
	// the enum in dxvk-remix/src/dxvk/rtx_render/rtx_types.h. This is the
	// runtime's internal order, which differs from the REMIXAPI_INSTANCE_
	// CATEGORY_BIT_* order the Remix C API uses for DrawInstance. Never mix
	// the two.
	// Bit positions counted from the upstream enum order in rtx_types.h:
	//   0 WorldUI, 1 WorldMatte, 2 Sky, 3 Ignore, 4 IgnoreLights,
	//   5 IgnoreAntiCulling, 6 IgnoreMotionBlur, 7 IgnoreOpacityMicromap,
	//   8 IgnoreAlphaChannel, 9 Hidden, 10 Particle, 11 Beam, 12 DecalStatic,
	//   13 DecalDynamic, 14 DecalSingleOffset, 15 DecalNoOffset,
	//   16 AlphaBlendToCutout, 17 Terrain, 18 AnimatedWater.
	// DecalStatic=12 is the cross-check that this counting is still in sync.
	enum class CategoryBit : uint32_t {
		WorldUI = 0,
		WorldMatte = 1,
		Sky = 2,
		Ignore = 3,
		// Tells Remix not to treat the albedo texture's alpha channel as
		// opacity. FNV's water albedo is its NoiseMap, whose alpha carries
		// nothing meaningful; without this the path tracer can resolve the
		// whole water surface to ~zero opacity, which looks like the draw was
		// culled.
		IgnoreAlphaChannel = 8,
		// Built but never traced (no primary hit, no shadow). A vertex-capture
		// draw keeps its rasterisation, so an orthographic UI quad tagged this
		// way still reaches the screen but does not float in the scene
		// (game/screen_space.hpp).
		Hidden = 9,
		// FNV flipbook particles (NOLIGHT017/019). Remix's particle handling
		// (unordered translucency, no shadow casting) instead of treating each
		// sprite quad as a translucent wall.
		Particle = 10,
		DecalStatic = 12,
		// Pairs with rtx.animatedWaterTextures. Remix animates the primary
		// texcoords and takes a second normal-map sample for draws in this
		// category, but only once the draw resolves to a translucent
		// material, which for FNV means a material replacement keyed on the
		// water albedo hash. Until then it does nothing.
		AnimatedWater = 18,
	};

	inline constexpr uint32_t category_mask(CategoryBit b) {
		return 1u << static_cast<uint32_t>(b);
	}

	// RS 42 bit 31, past every category bit: the runtime's D3D9 frontend
	// keeps the draw on its per-draw path (D3D9 v2 neither hands it to its
	// scene nor takes its camera; FnvLegacyProtocol::kPerDrawPath).
	constexpr uint32_t kPerDrawPath = 1u << 31;

	// RS 149 encoding: packed PS slot-role nibbles plus modifier bits.
	//   bits 0-3:   diffuse slot (0..7) or 0xF if PS has no diffuse role
	//   bits 4-7:   normal slot  (0..7) or 0xF if PS has no normal role
	//   bits 8-11:  glow slot    (0..7) or 0xF if PS has no glow role
	//   bits 12-15: height slot  (0..7) or 0xF if PS has no height role
	//   bit 16:     kNormalRgbTangentSpace: the normal slot is an RGB
	//               (DirectX, 0.5-biased) tangent-space normal map
	//   bit 17:     kNormalAlphaIsSpecMask: normal-map alpha is a per-texel
	//               specular mask (roughness = lerp(1, r, normal.a))
	//   bits 18-31: reserved (0)
	//
	// Example: PS has s0=BaseMap, s1=NormalMap          -> 0x0000_FF10
	//          PS has s0=BaseMap, s1=NormalMap, s4=Glow  -> 0x0000_F410
	//          ... FNV lit, normal-mapped                 -> 0x0003_FF10
	constexpr uint8_t kSlotAbsent = 0xF;
	constexpr uint32_t kNormalRgbTangentSpace = 1u << 16;
	constexpr uint32_t kNormalAlphaIsSpecMask = 1u << 17;

	inline uint32_t encode_slot_roles(uint8_t diffuseSlot,
	                                  uint8_t normalSlot,
	                                  uint8_t glowSlot   = kSlotAbsent,
	                                  uint8_t heightSlot = kSlotAbsent) {
		const uint32_t diff   = static_cast<uint32_t>(diffuseSlot & 0xF);
		const uint32_t norm   = static_cast<uint32_t>(normalSlot  & 0xF);
		const uint32_t glow   = static_cast<uint32_t>(glowSlot    & 0xF);
		const uint32_t height = static_cast<uint32_t>(heightSlot  & 0xF);
		return diff | (norm << 4) | (glow << 8) | (height << 12);
	}

	inline uint32_t float_bits(float f) {
		uint32_t u;
		std::memcpy(&u, &f, sizeof(u));
		return u;
	}

	// One protocol draw's worth of slot values. Absent fields hold kSentinel.
	struct Payload {
		uint32_t categories = kSentinel;
		uint32_t modifier   = kSentinel;
		uint32_t gloss      = kSentinel;
		uint32_t emittance  = kSentinel;
	};

	namespace detail {
		enum Slot : int { kCat = 0, kMod = 1, kGloss = 2, kEmit = 3, kCount = 4 };

		inline constexpr D3DRENDERSTATETYPE kSlotRs[kCount] = {
			kRsCategoryFlags, kRsModifier, kRsGloss, kRsEmittance
		};

		// What the device holds for each slot, as far as the wrapper knows.
		// known == false forces the next write through (after a device reset,
		// or when something else may have rewritten the slot).
		struct Shadow {
			uint32_t value[kCount] = {};
			bool     known[kCount] = {};
		};

		inline Shadow g_shadow;

		// Per-frame write counter for the status log: in steady state this is
		// the only protocol traffic that crosses the bridge.
		inline uint32_t g_writes_this_frame = 0;
		inline uint32_t g_writes_last_frame = 0;

		inline bool is_absent(uint32_t v) { return v == 0u || v == kSentinel; }

		inline void write(IDirect3DDevice9* dev, Slot s, uint32_t v) {
			auto& sh = g_shadow;
			if (sh.known[s] && sh.value[s] == v) return;
			dev->SetRenderState(kSlotRs[s], v);
			sh.value[s] = v;
			sh.known[s] = true;
			g_writes_this_frame++;
		}

		// Makes the slot absent, writing only when it isn't already.
		inline void clear(IDirect3DDevice9* dev, Slot s) {
			auto& sh = g_shadow;
			if (sh.known[s] && is_absent(sh.value[s])) return;
			write(dev, s, kSentinel);
		}

		// An absent payload field clears; anything else writes the value.
		inline void apply(IDirect3DDevice9* dev, Slot s, uint32_t v) {
			if (is_absent(v)) clear(dev, s);
			else write(dev, s, v);
		}

		// set_per_draw_path: every draw's RS 42 carries kPerDrawPath.
		inline bool g_per_draw_path = false;

		// RS 42 for a draw with these categories (absent: none).
		inline void apply_categories(IDirect3DDevice9* dev, uint32_t categories) {
			if (!g_per_draw_path) {
				apply(dev, kCat, categories);
				return;
			}
			write(dev, kCat, (is_absent(categories) ? 0u : categories) | kPerDrawPath);
		}
	}

	// From the next draw on, every draw asks the runtime for its per-draw
	// path (kPerDrawPath) while on is true.
	inline void set_per_draw_path(bool on) {
		detail::g_per_draw_path = on;
	}

	// Protocol draw: RS 149 present, every slot set to the payload (absent
	// fields cleared). A payload whose modifier is absent degenerates to
	// categories_only.
	inline void submit(IDirect3DDevice9* dev, const Payload& p) {
		using namespace detail;
		apply_categories(dev, p.categories);
		apply(dev, kMod, p.modifier);
		if (is_absent(p.modifier)) return;   // 169/177 are unread without 149
		apply(dev, kGloss, p.gloss);
		apply(dev, kEmit, p.emittance);
	}

	// RS 42 only; RS 149 absent. 169/177 are left alone (unread without 149).
	inline void categories_only(IDirect3DDevice9* dev, uint32_t categories) {
		using namespace detail;
		apply_categories(dev, categories);
		clear(dev, kMod);
	}

	// Draw tagged Ignore. RS 149 is cleared too: the runtime only honours
	// RS 42 when a texture is bound at the draw's first FFP stage
	// (d3d9_rtx.cpp, after setupCategoriesForTexture), so an "Ignored"
	// passthrough draw without one is still captured and must not pick up
	// the previous draw's protocol material.
	inline void ignore(IDirect3DDevice9* dev) {
		categories_only(dev, category_mask(CategoryBit::Ignore));
	}

	// Plain draw: no categories, no protocol material.
	inline void none(IDirect3DDevice9* dev) {
		using namespace detail;
		apply_categories(dev, kSentinel);
		clear(dev, kMod);
	}

	// Device reset returns every render state to its default. Forget the
	// shadow so the next draw re-establishes each slot explicitly.
	inline void on_reset() {
		detail::g_shadow = {};
	}

	// Called once per scene. State blocks the game (or an overlay) applies can
	// rewrite these slots behind the shadow; resetting it per scene limits a
	// stale slot to one scene for at most four writes.
	inline void on_begin_scene() {
		detail::g_shadow = {};
	}

	inline void on_present() {
		detail::g_writes_last_frame = detail::g_writes_this_frame;
		detail::g_writes_this_frame = 0;
	}

	inline uint32_t writes_last_frame() { return detail::g_writes_last_frame; }

	// The game (or anything else routed through the proxy's SetRenderState)
	// wrote one of the protocol slots directly; the shadow follows it.
	inline void on_external_set(D3DRENDERSTATETYPE state, DWORD value) {
		using namespace detail;
		for (int s = 0; s < kCount; s++) {
			if (kSlotRs[s] == state) {
				g_shadow.value[s] = value;
				g_shadow.known[s] = true;
			}
		}
	}

} // namespace remix_protocol
