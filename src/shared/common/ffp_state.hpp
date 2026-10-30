#pragma once
#include "config.hpp"

namespace shared::common
{
	struct PsSlotMap;
	// FFP state tracker: mirrors the D3D9 state the fixed-function routes need.
	// The D3D9Device proxy calls the on_*() methods when the game sets shaders,
	// constants, textures, declarations and so on. The renderer reads the
	// state to route draws and calls engage/disengage to switch between the
	// game's shaders and FFP.
	class ffp_state
	{
	public:
		static ffp_state& get();

		void init(IDirect3DDevice9* real_device);

		// State mutators, called from the D3D9Device proxy.
		void on_set_vs_const_f(UINT start_reg, const float* data, UINT count);
		void on_set_ps_const_f(UINT start_reg, const float* data, UINT count);

		// Returns true if the write would be a no-op (cached constants already
		// match the new data and the range has been written at least once).
		// The d3d9 proxy uses it to skip redundant SetVSConstF / SetPSConstF
		// calls; the game rewrites identical constants per draw very often.
		bool vs_const_matches(UINT start_reg, const float* data, UINT count) const;
		bool ps_const_matches(UINT start_reg, const float* data, UINT count) const;
		void on_set_vertex_shader(IDirect3DVertexShader9* shader);

		// Returns true if the call is swallowed (not forwarded to the device).
		bool on_set_pixel_shader(IDirect3DPixelShader9* shader);

		void on_set_texture(UINT stage, IDirect3DBaseTexture9* texture);
		void on_set_stream_source(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride);
		void on_set_vertex_declaration(IDirect3DVertexDeclaration9* decl);
		// Game-side stage-0 texture transform writes. Only used to keep the
		// shadow behind set_stage0_tex_transform accurate: FNV's shader path
		// never needs them, but a game write must not be mistaken for one of
		// the tracker's own.
		void on_set_texture_stage_state(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
		void on_set_transform(D3DTRANSFORMSTATETYPE state);
		void on_present();
		void on_begin_scene();
		void on_reset();

		// State consumers, called from the renderer's draw routing.
		void engage(IDirect3DDevice9* dev);
		void disengage(IDirect3DDevice9* dev);

		// Bind albedo texture to stage 0, NULL stages 1-7. Call before draw.
		void setup_albedo_texture(IDirect3DDevice9* dev);

		// Classifier-aware variant: skips nulling stages 1-7 for slots the
		// caller will rebind via apply_ps_protocol (normal/glow/height roles).
		// Also tracks the kept-bound mask so restore_textures can skip those
		// slots, whose device binding never changed. nullptr behaves like the
		// no-argument overload.
		void setup_albedo_texture(IDirect3DDevice9* dev, const PsSlotMap* psmap);

		// Restore original texture bindings on all 8 stages. Call after draw.
		void restore_textures(IDirect3DDevice9* dev);

		// Tells the tracker that the caller re-pointed stage 0 itself after
		// setup_albedo_texture ran (apply_ps_protocol does this when the PS
		// labelled a non-zero slot as the diffuse). Without this, the common
		// AlbedoStage=0 path leaves stage0_changed_ false and restore_textures
		// skips stage 0, and the device would keep the override bound for
		// every following passthrough draw.
		void note_stage0_override(IDirect3DBaseTexture9* tex);

		// Stage 0 samples with the albedo's own sampler: its ADDRESSU / V and
		// MAG / MIN / MIP filters become `stage`'s (the stage the albedo was
		// moved from; 0 puts the game's stage-0 values back). The runtime's
		// D3D9 route builds the material's sampler from the stage the albedo
		// texture is bound at, so a texture moved without its sampler state
		// would take the addressing of whatever the game left at stage 0.
		// setup_albedo_texture calls it for its pick; apply_ps_protocol for the
		// PS's Diffuse slot.
		// restore_textures puts stage 0's own values back.
		void move_albedo_sampler(IDirect3DDevice9* dev, UINT stage);

		// Wrapper-owned texture at a stage 1..6 the game's own binding doesn't
		// use for this draw (texture-pack height maps: the runtime reads the
		// slot straight from device state via RS 149). Sticky like the
		// protocol render states: a stage the game left NULL keeps the
		// texture across FFP draws, so a run of draws sharing one height map
		// costs one SetTexture. It is put back to the game's binding by
		// restore_textures when the game had something there, and by
		// flush_extra_textures (called from disengage, i.e. before every
		// passthrough draw, and before the proxy's UP draws) otherwise; a
		// shader draw must never sample a texture the game didn't bind.
		// Stage 7 is never used: dxvk's fixed-function sampler mask stops at
		// stage 6, so a texture there is never uploaded.
		void bind_extra_texture(IDirect3DDevice9* dev, UINT stage, IDirect3DBaseTexture9* tex);
		void flush_extra_textures(IDirect3DDevice9* dev);
		// Stage that already holds `tex` as an extra, or -1.
		int extra_stage_holding(IDirect3DBaseTexture9* tex) const;

		// Stage-0 texture transform for FFP draws whose vertex shader applies an
		// affine UV transform (FNV's NOLIGHT TexCoordTranform family). Pass a
		// matrix to enable it with D3DTTFF_COUNT2, or nullptr to disable it.
		// Shadowed: the matrix and the flags are each written only when they
		// differ from what this tracker last put on the device, so a run of
		// draws sharing one transform costs nothing after the first.
		//
		// engage() calls this with nullptr on every FFP draw, which is what
		// keeps a transform from leaking into the next ordinary draw; the
		// disable is a no-op compare unless a transform is actually live.
		void set_stage0_tex_transform(IDirect3DDevice9* dev, const D3DMATRIX* m);

		// Forgets the D3DTS_TEXTURE0 matrix the device holds (something else
		// wrote it). The next enabled set_stage0_tex_transform rewrites it
		// unconditionally. The flags shadow is unaffected.
		void invalidate_stage0_tex_transform() { tex0_mat_valid_ = false; }

		bool is_enabled() const { return enabled_; }
		void set_enabled(bool e) { enabled_ = e; }

		bool view_proj_valid() const { return view_proj_valid_; }
		bool is_ffp_active() const { return ffp_active_; }
		bool cur_decl_is_skinned() const { return cur_decl_is_skinned_; }
		bool cur_decl_has_normal() const { return cur_decl_has_normal_; }
		bool cur_decl_has_pos_t() const { return cur_decl_has_pos_t_; }
		bool cur_decl_has_texcoord() const { return cur_decl_has_texcoord_; }
		bool cur_decl_has_color() const { return cur_decl_has_color_; }
		int cur_decl_color_off() const { return cur_decl_color_off_; }
		int cur_decl_color_type() const { return cur_decl_color_type_; }
		bool cur_decl_has_blendindices() const { return cur_decl_has_blendindices_; }
		int cur_decl_texcoord_type() const { return cur_decl_texcoord_type_; }
		int cur_decl_blend_indices_type() const { return cur_decl_blend_indices_type_; }
		// True when any element of the declaration reads a stream other than 0.
		// The CPU vertex expanders only rewrite stream 0 and decline these.
		bool cur_decl_multi_stream() const { return cur_decl_multi_stream_; }
		int cur_decl_n_texcoords() const { return cur_decl_n_texcoords_; }

		// The bound declaration's elements (terminator excluded), copied at
		// SetVertexDeclaration time. For consumers that need an element the
		// flags above don't describe (e.g. terrain's TEXCOORD2 weights).
		const D3DVERTEXELEMENT9* cur_decl_elements() const { return cur_decl_elems_; }
		UINT cur_decl_num_elements() const { return cur_decl_num_elems_; }

		IDirect3DVertexShader9* last_vs() const { return last_vs_; }
		IDirect3DPixelShader9* last_ps() const { return last_ps_; }
		IDirect3DVertexDeclaration9* last_decl() const { return last_decl_; }

		const float* vs_const_data() const { return vs_const_; }
		const float* ps_const_data() const { return ps_const_; }
		const int* vs_const_write_log() const { return vs_const_write_log_; }

		// The per-register write log is read only by the diagnostics module
		// (frame log) and the imgui overlay (heatmap). Filling it costs up to
		// ~256 writes per draw, so it is gated on this flag and the hot path
		// keeps only the two sentinels the view/proj check reads (view_start,
		// proj_start). The consumers turn it on while they are active.
		static inline bool vs_write_log_enabled_ = false;
		UINT draw_call_count() const { return draw_call_count_; }
		UINT frame_count() const { return frame_count_; }
		UINT scene_count() const { return scene_count_; }

		// Texture tracking (PS samplers 0-15). The FFP paths only ever touch
		// stages 0-7; 8-15 are mirrored for FNV terrain, whose NormalMap[N]
		// array occupies s7..s(7+N-1).
		static constexpr UINT kTrackedStages = 16;
		IDirect3DBaseTexture9* cur_texture(UINT stage) const { return stage < kTrackedStages ? cur_texture_[stage] : nullptr; }

		IDirect3DVertexBuffer9* stream_vb(UINT stream) const { return stream < 4 ? stream_vb_[stream] : nullptr; }
		UINT stream_offset(UINT stream) const { return stream < 4 ? stream_offset_[stream] : 0; }
		UINT stream_stride(UINT stream) const { return stream < 4 ? stream_stride_[stream] : 0; }

		// Bone palette found by on_set_vs_const_f (with [Skinning] Enabled) and
		// the skinned declaration's layout.
		int bone_start_reg() const { return bone_start_reg_; }
		int num_bones() const { return num_bones_; }
		int cur_decl_num_weights() const { return cur_decl_num_weights_; }
		int cur_decl_blend_indices_off() const { return cur_decl_blend_indices_off_; }
		int cur_decl_pos_off() const { return cur_decl_pos_off_; }
		int cur_decl_pos_type() const { return cur_decl_pos_type_; }
		int cur_decl_normal_off() const { return cur_decl_normal_off_; }
		int cur_decl_normal_type() const { return cur_decl_normal_type_; }
		int cur_decl_texcoord_off() const { return cur_decl_texcoord_off_; }
		int cur_decl_texcoord1_off() const { return cur_decl_texcoord1_off_; }
		int cur_decl_texcoord1_type() const { return cur_decl_texcoord1_type_; }

		void increment_draw_count() { draw_call_count_++; }

		static void mat4_transpose(float* dst, const float* src);
		static bool mat4_is_interesting(const float* m);

	private:
		bool enabled_ = true;

		float vs_const_[256 * 4] = {};
		float ps_const_[32 * 4] = {};

		bool world_dirty_ = false;
		bool view_proj_dirty_ = false;
		bool view_proj_valid_ = false;
		bool ffp_active_ = false;
		bool ffp_setup_ = false;
		// Whether the FFP texture stage setup (ALPHAOP=MODULATE, DIFFUSE) is
		// programmed. Consecutive FFP draws skip the ~22 SetTextureStageState
		// calls. Reset on device loss.
		enum class tss_mode_t : uint8_t { none, ffp_geo };
		tss_mode_t tss_mode_ = tss_mode_t::none;

		IDirect3DVertexShader9* last_vs_ = nullptr;
		IDirect3DPixelShader9* last_ps_ = nullptr;

		IDirect3DVertexDeclaration9* last_decl_ = nullptr;
		bool cur_decl_is_skinned_ = false;
		bool cur_decl_has_texcoord_ = false;
		bool cur_decl_has_normal_ = false;
		bool cur_decl_has_color_ = false;
		int cur_decl_color_off_ = -1;   // byte offset of COLOR0 (usage idx 0, stream 0), or -1
		int cur_decl_color_type_ = -1;  // D3DDECLTYPE of COLOR0
		bool cur_decl_has_blendindices_ = false;
		bool cur_decl_has_pos_t_ = false;
		int cur_decl_texcoord_type_ = -1;
		int cur_decl_texcoord_off_ = 0;
		// TEXCOORD1 (stream 0). -1 offset means the decl has no TEXCOORD1.
		int cur_decl_texcoord1_type_ = -1;
		int cur_decl_texcoord1_off_ = -1;
		int cur_decl_n_texcoords_ = 0;
		D3DVERTEXELEMENT9 cur_decl_elems_[32] = {};
		UINT cur_decl_num_elems_ = 0;

		// Skinning-related declaration data
		int cur_decl_num_weights_ = 0;
		int cur_decl_blend_indices_off_ = 0;
		int cur_decl_blend_indices_type_ = -1;
		bool cur_decl_multi_stream_ = false;
		int cur_decl_pos_off_ = 0;
		int cur_decl_pos_type_ = -1;
		int cur_decl_normal_off_ = 0;
		int cur_decl_normal_type_ = -1;

		int bone_start_reg_ = 0;
		int num_bones_ = 0;

		IDirect3DBaseTexture9* cur_texture_[kTrackedStages] = {};

		// Bitmask of slots 1-7 that setup_albedo_texture left bound (because
		// apply_ps_protocol will rebind them). restore_textures reads this to
		// skip restoring those slots, whose device binding is unchanged.
		// Bit 0 unused (stage 0 always handled separately).
		uint8_t kept_bound_mask_ = 0;

		// Stages whose device binding is a wrapper texture (extra_tex_[s])
		// rather than cur_texture_[s]. See bind_extra_texture.
		uint8_t extra_mask_ = 0;
		IDirect3DBaseTexture9* extra_tex_[8] = {};

		// True when setup_albedo_texture rebound stage 0 to a different
		// texture than cur_texture_[0]; restore_textures only writes stage 0
		// when this is true.
		bool stage0_changed_ = false;

		// Stage-0 sampler states move_albedo_sampler overwrote (bit i =
		// kAlbedoSamplerStates[i]) and their game values, for restore_textures.
		static constexpr UINT kAlbedoSamplerStateCount = 5;
		uint8_t stage0_sampler_saved_mask_ = 0;
		DWORD stage0_sampler_saved_[kAlbedoSamplerStateCount] = {};

		// The pointer most recently bound at stage 0 by setup_albedo_texture
		// (the albedo pick). apply_ps_protocol reads this to dedup its
		// slot-0 rebind: if the picked albedo already equals the diffuse
		// texture, the rebind is a no-op.
		IDirect3DBaseTexture9* last_albedo_stage0_ = nullptr;

	public:
		IDirect3DBaseTexture9* last_albedo_stage0() const { return last_albedo_stage0_; }
	private:

		IDirect3DVertexBuffer9* stream_vb_[4] = {};
		UINT stream_offset_[4] = {};
		UINT stream_stride_[4] = {};

		UINT frame_count_ = 0;
		UINT draw_call_count_ = 0;
		UINT scene_count_ = 0;

		// Which VS constant registers were written this frame (diagnostics).
		int vs_const_write_log_[256] = {};

		// Stage-0 texture transform shadow (see set_stage0_tex_transform).
		// kTex0Unknown: not known to match anything the tracker wrote.
		static constexpr DWORD kTex0Unknown = 0xFFFFFFFFu;
		DWORD tex0_ttff_ = kTex0Unknown;
		D3DMATRIX tex0_mat_ = {};
		bool tex0_mat_valid_ = false;
		// True while the enabled transform on the device is the tracker's own;
		// disengage only undoes that, never a value the game set.
		bool tex0_ours_ = false;

		const config::ffp_settings* cfg_ = nullptr;

		void apply_transforms(IDirect3DDevice9* dev);
		void setup_lighting(IDirect3DDevice9* dev);
		void setup_texture_stages(IDirect3DDevice9* dev);
	};
}
