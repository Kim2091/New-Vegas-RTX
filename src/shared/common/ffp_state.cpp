#include "std_include.hpp"
#include "ffp_state.hpp"
#include "ps_reflect.hpp"

namespace shared::common
{
	ffp_state& ffp_state::get()
	{
		static ffp_state instance;
		return instance;
	}

	void ffp_state::init(IDirect3DDevice9* /*real_device*/)
	{
		cfg_ = &config::get().ffp;
		enabled_ = cfg_->enabled;

		log("FFP", std::format("State tracker initialized, FFP={}", enabled_ ? "ON" : "OFF"));
	}

	void ffp_state::on_set_vs_const_f(UINT start_reg, const float* data, UINT count)
	{
		PROFILE_ZONE_N("ffp::on_set_vs_const_f");
		if (!data || start_reg + count > 256) return;

		std::memcpy(&vs_const_[start_reg * 4], data, count * 4 * sizeof(float));

		// Dirty tracking on the game's register layout.
		UINT end_reg = start_reg + count;
		if (start_reg < static_cast<UINT>(cfg_->vs_reg_proj_end) &&
			end_reg > static_cast<UINT>(cfg_->vs_reg_view_start))
		{
			view_proj_dirty_ = true;
		}
		if (start_reg < static_cast<UINT>(cfg_->vs_reg_world_end) &&
			end_reg > static_cast<UINT>(cfg_->vs_reg_world_start))
		{
			world_dirty_ = true;
		}

		// View+Proj are valid once both ranges have been written.
		auto view_start = static_cast<UINT>(cfg_->vs_reg_view_start);
		auto proj_start = static_cast<UINT>(cfg_->vs_reg_proj_start);
		auto proj_end = static_cast<UINT>(cfg_->vs_reg_proj_end);

		if (start_reg <= proj_start && start_reg + count >= proj_end)
		{
			view_proj_valid_ = true;
		}
		else if (start_reg == view_start && count >= 4 && vs_const_write_log_[proj_start])
		{
			view_proj_valid_ = true;
		}
		else if (start_reg == proj_start && count >= 4 && vs_const_write_log_[view_start])
		{
			view_proj_valid_ = true;
		}

		// The check above reads two entries (view_start, proj_start); the
		// rest of the log is read only by diagnostics and imgui. The two
		// sentinels are always kept current; the full fill runs only while
		// something observes the log.
		if (start_reg <= view_start && view_start < start_reg + count)
			vs_const_write_log_[view_start] = 1;
		if (start_reg <= proj_start && proj_start < start_reg + count)
			vs_const_write_log_[proj_start] = 1;

		if (vs_write_log_enabled_)
		{
			for (UINT i = 0; i < count; i++)
			{
				if (start_reg + i < 256)
					vs_const_write_log_[start_reg + i] = 1;
			}
		}

		// Bone palette detection.
		if (config::get().skinning.enabled &&
			start_reg >= static_cast<UINT>(cfg_->vs_reg_bone_threshold) &&
			count >= static_cast<UINT>(cfg_->vs_bone_min_regs) &&
			(count % static_cast<UINT>(cfg_->vs_regs_per_bone)) == 0)
		{
			bone_start_reg_ = static_cast<int>(start_reg);
			num_bones_ = static_cast<int>(count) / cfg_->vs_regs_per_bone;
		}
	}

	void ffp_state::on_set_ps_const_f(UINT start_reg, const float* data, UINT count)
	{
		PROFILE_ZONE_N("ffp::on_set_ps_const_f");
		if (!data || start_reg + count > 32) return;

		std::memcpy(&ps_const_[start_reg * 4], data, count * 4 * sizeof(float));
	}

	bool ffp_state::vs_const_matches(UINT start_reg, const float* data, UINT count) const
	{
		if (!data || start_reg + count > 256) return false;
		return std::memcmp(&vs_const_[start_reg * 4], data, count * 4 * sizeof(float)) == 0;
	}

	bool ffp_state::ps_const_matches(UINT start_reg, const float* data, UINT count) const
	{
		if (!data || start_reg + count > 32) return false;
		return std::memcmp(&ps_const_[start_reg * 4], data, count * 4 * sizeof(float)) == 0;
	}

	void ffp_state::on_set_vertex_shader(IDirect3DVertexShader9* shader)
	{
		PROFILE_ZONE_N("ffp::on_set_vertex_shader");
		if (shader) shader->AddRef();
		if (last_vs_) last_vs_->Release();
		last_vs_ = shader;
		ffp_active_ = false;
	}

	bool ffp_state::on_set_pixel_shader(IDirect3DPixelShader9* shader)
	{
		PROFILE_ZONE_N("ffp::on_set_pixel_shader");
		if (shader) shader->AddRef();
		if (last_ps_) last_ps_->Release();
		last_ps_ = shader;

		// Swallowed while FFP is engaged.
		return ffp_active_;
	}

	void ffp_state::on_set_texture(UINT stage, IDirect3DBaseTexture9* texture)
	{
		PROFILE_ZONE_N("ffp::on_set_texture");
		if (stage < kTrackedStages)
			cur_texture_[stage] = texture;
		// The game's write goes to the device after this and replaces any
		// extra on that stage.
		if (stage < 8) extra_mask_ &= static_cast<uint8_t>(~(1u << stage));
	}

	void ffp_state::on_set_stream_source(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride)
	{
		PROFILE_ZONE_N("ffp::on_set_stream_source");
		if (stream < 4)
		{
			stream_vb_[stream] = vb;
			stream_offset_[stream] = offset;
			stream_stride_[stream] = stride;
		}
	}

	void ffp_state::on_set_vertex_declaration(IDirect3DVertexDeclaration9* decl)
	{
		PROFILE_ZONE_N("ffp::on_set_vertex_declaration");
		last_decl_ = decl;
		cur_decl_is_skinned_ = false;
		cur_decl_has_texcoord_ = false;
		cur_decl_has_normal_ = false;
		cur_decl_has_color_ = false;
		cur_decl_color_off_ = -1;
		cur_decl_color_type_ = -1;
		cur_decl_has_blendindices_ = false;
		cur_decl_has_pos_t_ = false;
		cur_decl_texcoord_type_ = -1;
		cur_decl_texcoord_off_ = 0;
		cur_decl_texcoord1_type_ = -1;
		cur_decl_texcoord1_off_ = -1;
		cur_decl_n_texcoords_ = 0;
		cur_decl_num_weights_ = 0;
		cur_decl_blend_indices_off_ = 0;
		cur_decl_blend_indices_type_ = -1;
		cur_decl_multi_stream_ = false;
		cur_decl_pos_off_ = 0;
		cur_decl_pos_type_ = -1;
		cur_decl_normal_off_ = 0;
		cur_decl_normal_type_ = -1;
		cur_decl_num_elems_ = 0;

		if (!decl) return;

		UINT num_elems = 0;
		if (FAILED(decl->GetDeclaration(nullptr, &num_elems))) return;
		if (num_elems == 0 || num_elems > 32) return;

		D3DVERTEXELEMENT9 elems[32];
		if (FAILED(decl->GetDeclaration(elems, &num_elems))) return;

		bool has_blend_weight = false;
		bool has_blend_indices = false;
		BYTE blend_weight_type = 0;

		for (UINT e = 0; e < num_elems; e++)
		{
			const auto& el = elems[e];
			if (el.Stream == 0xFF) break;
			if (el.Stream != 0) cur_decl_multi_stream_ = true;
			cur_decl_elems_[cur_decl_num_elems_++] = el;

			switch (el.Usage)
			{
			case D3DDECLUSAGE_POSITIONT:
				cur_decl_has_pos_t_ = true;
				break;

			case D3DDECLUSAGE_BLENDWEIGHT:
				has_blend_weight = true;
				blend_weight_type = el.Type;
				break;

			case D3DDECLUSAGE_BLENDINDICES:
				has_blend_indices = true;
				cur_decl_has_blendindices_ = true;
				cur_decl_blend_indices_off_ = el.Offset;
				cur_decl_blend_indices_type_ = el.Type;
				break;

			case D3DDECLUSAGE_POSITION:
				if (el.Stream == 0)
				{
					cur_decl_pos_off_ = el.Offset;
					cur_decl_pos_type_ = el.Type;
				}
				break;

			case D3DDECLUSAGE_NORMAL:
				if (el.Stream == 0)
				{
					cur_decl_has_normal_ = true;
					cur_decl_normal_off_ = el.Offset;
					cur_decl_normal_type_ = el.Type;
				}
				break;

			case D3DDECLUSAGE_TEXCOORD:
				cur_decl_n_texcoords_++;
				if (el.UsageIndex == 0 && el.Stream == 0)
				{
					cur_decl_has_texcoord_ = true;
					cur_decl_texcoord_type_ = el.Type;
					cur_decl_texcoord_off_ = el.Offset;
				}
				// TEXCOORD1.x carries the coarse-LOD Z that the distant-terrain
				// vertex shader (SLS2002) geomorphs against. lod_sink needs it.
				else if (el.UsageIndex == 1 && el.Stream == 0)
				{
					cur_decl_texcoord1_type_ = el.Type;
					cur_decl_texcoord1_off_ = el.Offset;
				}
				break;

			case D3DDECLUSAGE_COLOR:
				cur_decl_has_color_ = true;
				if (el.UsageIndex == 0 && el.Stream == 0)
				{
					cur_decl_color_off_ = el.Offset;
					cur_decl_color_type_ = el.Type;
				}
				break;
			}
		}

		if (has_blend_weight && has_blend_indices)
		{
			cur_decl_is_skinned_ = true;

			switch (blend_weight_type)
			{
			case D3DDECLTYPE_FLOAT1:  cur_decl_num_weights_ = 1; break;
			case D3DDECLTYPE_FLOAT2:  cur_decl_num_weights_ = 2; break;
			case D3DDECLTYPE_FLOAT3:  cur_decl_num_weights_ = 3; break;
			case D3DDECLTYPE_FLOAT4:  cur_decl_num_weights_ = 3; break;
			case D3DDECLTYPE_UBYTE4N: cur_decl_num_weights_ = 3; break;
			default:                  cur_decl_num_weights_ = 3; break;
			}
		}
	}

	void ffp_state::on_present()
	{
		PROFILE_ZONE_N("ffp::on_present");
		frame_count_++;
		ffp_setup_ = false;
		draw_call_count_ = 0;
		scene_count_ = 0;
		disengage(shared::globals::d3d_device);
		std::memset(vs_const_write_log_, 0, sizeof(vs_const_write_log_));
	}

	void ffp_state::on_begin_scene()
	{
		PROFILE_ZONE_N("ffp::on_begin_scene");
		ffp_setup_ = false;
		scene_count_++;
	}

	void ffp_state::on_reset()
	{
		if (last_vs_) { last_vs_->Release(); last_vs_ = nullptr; }
		if (last_ps_) { last_ps_->Release(); last_ps_ = nullptr; }

		// Device reset destroys every D3DPOOL_DEFAULT resource. These mirrors
		// are raw, un-AddRef'd copies of the game's bindings, so anything left
		// here is a dangling pointer once Reset returns, and restore_textures
		// would SetTexture freed objects on the first draw of the new device.
		// Clearing the declaration through on_set_vertex_declaration(nullptr)
		// also zeroes every cur_decl_* flag derived from it.
		for (auto& t : cur_texture_) t = nullptr;
		for (auto& vb : stream_vb_) vb = nullptr;
		for (auto& off : stream_offset_) off = 0;
		for (auto& stride : stream_stride_) stride = 0;
		on_set_vertex_declaration(nullptr);

		view_proj_valid_ = false;
		ffp_setup_ = false;
		tss_mode_ = tss_mode_t::none;
		world_dirty_ = false;
		view_proj_dirty_ = false;
		ffp_active_ = false;
		bone_start_reg_ = 0;
		num_bones_ = 0;
		kept_bound_mask_ = 0;
		extra_mask_ = 0;   // Reset unbinds every texture
		stage0_changed_ = false;
		stage0_sampler_saved_mask_ = 0;   // Reset restores the default sampler states
		last_albedo_stage0_ = nullptr;
		invalidate_stage0_tex_transform();
		tex0_ttff_ = kTex0Unknown;
		tex0_ours_ = false;

		log("FFP", "State reset");
	}

	void ffp_state::engage(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("ffp::engage");
		if (!enabled_ || !dev) return;

		if (!ffp_active_)
		{
			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			ffp_active_ = true;
		}

		apply_transforms(dev);
		setup_texture_stages(dev);
		set_stage0_tex_transform(dev, nullptr);

		if (!ffp_setup_)
		{
			setup_lighting(dev);
			ffp_setup_ = true;
		}
	}

	void ffp_state::disengage(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("ffp::disengage");
		if (!dev) return;

		// Wrapper extras must not reach a draw that runs the game's shaders.
		flush_extra_textures(dev);

		// A stage-0 transform left enabled by a TexCoordTranform draw would be
		// picked up by Remix for the next passthrough draw too: the runtime
		// reads the stage-0 transform whether or not a VS is bound. Costs a
		// call only when one is live.
		if (tex0_ours_ && tex0_ttff_ != D3DTTFF_DISABLE) {
			dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			tex0_ttff_ = D3DTTFF_DISABLE;
		}
		tex0_ours_ = false;

		if (!ffp_active_) return;

		dev->SetVertexShader(last_vs_);
		dev->SetPixelShader(last_ps_);
		ffp_active_ = false;
	}

	void ffp_state::setup_albedo_texture(IDirect3DDevice9* dev)
	{
		setup_albedo_texture(dev, nullptr);
	}

	void ffp_state::setup_albedo_texture(IDirect3DDevice9* dev, const PsSlotMap* psmap)
	{
		PROFILE_ZONE_N("ffp::setup_albedo_texture");
		if (!dev) return;

		int as = cfg_->albedo_stage;

		// Per-declaration-shape AlbedoStage override ([FFP] TerrainAlbedoStage,
		// BIAlbedoStage). Both default to stage 1, since stage 0 holds
		// LOD-atlas-style globals in FNV.
		if (cur_decl_has_color_ && cur_decl_n_texcoords_ >= 2)
			as = cfg_->terrain_albedo_stage;
		else if (cur_decl_has_blendindices_ && !cur_decl_is_skinned_)
			as = cfg_->bi_albedo_stage;

		auto* albedo = (as >= 0 && as < 8) ? cur_texture_[as] : cur_texture_[0];

		// Whether stage 0 changed, so restore_textures can skip the write when
		// the pick is the game's own stage-0 texture.
		stage0_changed_ = (albedo != cur_texture_[0]);
		last_albedo_stage0_ = albedo;
		dev->SetTexture(0, albedo);
		move_albedo_sampler(dev, (as >= 0 && as < 8) ? static_cast<UINT>(as) : 0u);

		// Build the keep-bound mask from the PS slot map. Stages mentioned in
		// the classifier (normal/glow/height; diffuse if non-zero) will be
		// rebound by apply_ps_protocol anyway, so they skip both the null-write
		// here and the restore later.
		uint8_t keep = 0;
		if (psmap && psmap->any_classified) {
			const uint8_t kNo = PsSlotMap::kNoSlot;
			const uint8_t diffuseSlot = psmap->slot(PsSlotRole::Diffuse);
			const uint8_t normalSlot  = psmap->slot(PsSlotRole::Normal);
			const uint8_t glowSlot    = psmap->slot(PsSlotRole::Glow);
			const uint8_t heightSlot  = psmap->slot(PsSlotRole::Height);
			if (diffuseSlot != kNo && diffuseSlot >= 1 && diffuseSlot < 8) keep |= (1u << diffuseSlot);
			if (normalSlot  != kNo && normalSlot  >= 1 && normalSlot  < 8) keep |= (1u << normalSlot);
			if (glowSlot    != kNo && glowSlot    >= 1 && glowSlot    < 8) keep |= (1u << glowSlot);
			if (heightSlot  != kNo && heightSlot  >= 1 && heightSlot  < 8) keep |= (1u << heightSlot);
		}
		kept_bound_mask_ = keep;

		// A sticky extra sitting on a stage this PS reads as a role (the game
		// left that role's texture NULL) would be read by the runtime as that
		// role's texture: put the game's NULL back.
		if (const uint8_t stale = static_cast<uint8_t>(extra_mask_ & keep))
		{
			for (DWORD ts = 1; ts < 8; ts++)
				if (stale & (1u << ts)) dev->SetTexture(ts, cur_texture_[ts]);
			extra_mask_ &= static_cast<uint8_t>(~stale);
		}

		// No null-write for slots the game left null: cur_texture_[] mirrors
		// every game-side SetTexture, and nothing between that and this point
		// changes textures. Slots the PS protocol rebinds are skipped too.
		for (DWORD ts = 1; ts < 8; ts++)
			if (cur_texture_[ts] != nullptr && !(keep & (1u << ts)))
			{
				dev->SetTexture(ts, nullptr);
				extra_mask_ &= static_cast<uint8_t>(~(1u << ts));
			}
	}

	void ffp_state::note_stage0_override(IDirect3DBaseTexture9* tex)
	{
		last_albedo_stage0_ = tex;
		// Recomputed rather than OR-ed in: an override that lands back on the
		// game's own stage-0 texture leaves nothing to restore.
		stage0_changed_ = (tex != cur_texture_[0]);
	}

	namespace
	{
		// The sampler states that travel with the albedo (ffp_state::kAlbedoSamplerStateCount).
		constexpr D3DSAMPLERSTATETYPE kAlbedoSamplerStates[] = {
			D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
		};
	}

	void ffp_state::move_albedo_sampler(IDirect3DDevice9* dev, UINT stage)
	{
		static_assert(sizeof(kAlbedoSamplerStates) / sizeof(kAlbedoSamplerStates[0]) == kAlbedoSamplerStateCount);
		if (!dev || stage >= 8) return;
		for (UINT i = 0; i < kAlbedoSamplerStateCount; i++)
		{
			const uint8_t bit = static_cast<uint8_t>(1u << i);
			const D3DSAMPLERSTATETYPE type = kAlbedoSamplerStates[i];
			DWORD want = 0;
			if (stage == 0)
			{
				// Back to the game's own stage-0 value, only where it was changed.
				if (!(stage0_sampler_saved_mask_ & bit)) continue;
				want = stage0_sampler_saved_[i];
			}
			else if (FAILED(dev->GetSamplerState(stage, type, &want))) continue;
			DWORD cur = 0;
			if (FAILED(dev->GetSamplerState(0, type, &cur)) || cur == want) continue;
			if (!(stage0_sampler_saved_mask_ & bit))
			{
				stage0_sampler_saved_[i] = cur;
				stage0_sampler_saved_mask_ |= bit;
			}
			dev->SetSamplerState(0, type, want);
		}
	}

	void ffp_state::restore_textures(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("ffp::restore_textures");
		if (!dev) return;

		// Stage 0 only needs a restore when setup_albedo_texture actually
		// re-pointed it.
		if (stage0_changed_) {
			dev->SetTexture(0, cur_texture_[0]);
		}
		// Stage 0's own sampler states, where the albedo's replaced them.
		if (stage0_sampler_saved_mask_)
		{
			for (UINT i = 0; i < kAlbedoSamplerStateCount; i++)
				if (stage0_sampler_saved_mask_ & (1u << i))
					dev->SetSamplerState(0, kAlbedoSamplerStates[i], stage0_sampler_saved_[i]);
			stage0_sampler_saved_mask_ = 0;
		}
		// Stages 1-7: setup only changed slots where cur_texture_[ts] was
		// non-null AND the slot wasn't in the kept-bound mask (apply_ps_protocol
		// kept those bound to the same texture; nothing to restore).
		const uint8_t keep = kept_bound_mask_;
		for (DWORD ts = 1; ts < 8; ts++)
			if (cur_texture_[ts] != nullptr && !(keep & (1u << ts)))
			{
				dev->SetTexture(ts, cur_texture_[ts]);
				extra_mask_ &= static_cast<uint8_t>(~(1u << ts));
			}
		// An extra on a stage the game holds non-NULL but kept bound (a role
		// slot) cannot exist: bind_extra_texture is never given a role slot.
		// Extras left in extra_mask_ sit on stages the game has NULL; they stay
		// bound (sticky) until flush_extra_textures.
		kept_bound_mask_ = 0;
		stage0_changed_ = false;
	}

	void ffp_state::bind_extra_texture(IDirect3DDevice9* dev, UINT stage, IDirect3DBaseTexture9* tex)
	{
		if (!dev || stage < 1 || stage > 6) return;
		const uint8_t bit = static_cast<uint8_t>(1u << stage);
		if ((extra_mask_ & bit) && extra_tex_[stage] == tex) return;
		dev->SetTexture(stage, tex);
		extra_mask_ |= bit;
		extra_tex_[stage] = tex;
	}

	void ffp_state::flush_extra_textures(IDirect3DDevice9* dev)
	{
		if (!extra_mask_ || !dev) return;
		for (UINT s = 1; s < 8; s++)
			if (extra_mask_ & (1u << s))
				dev->SetTexture(s, cur_texture_[s]);
		extra_mask_ = 0;
	}

	int ffp_state::extra_stage_holding(IDirect3DBaseTexture9* tex) const
	{
		for (int s = 1; s < 8; s++)
			if ((extra_mask_ & (1u << s)) && extra_tex_[s] == tex) return s;
		return -1;
	}

	void ffp_state::apply_transforms(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("ffp::apply_transforms");
		float transposed[16];

		if (view_proj_dirty_)
		{
			mat4_transpose(transposed, &vs_const_[cfg_->vs_reg_view_start * 4]);
			dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(transposed));

			mat4_transpose(transposed, &vs_const_[cfg_->vs_reg_proj_start * 4]);
			dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(transposed));

			view_proj_dirty_ = false;
		}

		if (world_dirty_)
		{
			mat4_transpose(transposed, &vs_const_[cfg_->vs_reg_world_start * 4]);
			dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(transposed));

			world_dirty_ = false;
		}
	}

	void ffp_state::setup_lighting(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("ffp::setup_lighting");
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);

		D3DMATERIAL9 mat = {};
		mat.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
		mat.Ambient = { 1.0f, 1.0f, 1.0f, 1.0f };
		dev->SetMaterial(&mat);
	}

	void ffp_state::setup_texture_stages(IDirect3DDevice9* dev)
	{
		PROFILE_ZONE_N("ffp::setup_texture_stages");
		if (tss_mode_ == tss_mode_t::ffp_geo) return;

		// Stage 0: texture colour modulated by the vertex/material diffuse.
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		tex0_ttff_ = D3DTTFF_DISABLE;

		// Disable stages 1-7: the game binds shadow maps, LUTs, normal maps etc.
		// on higher stages for its pixel shaders. In FFP mode those become active
		// and Remix may consume the wrong textures.
		for (DWORD s = 1; s <= 7; s++)
		{
			dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
			dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		}

		tss_mode_ = tss_mode_t::ffp_geo;
	}

	void ffp_state::set_stage0_tex_transform(IDirect3DDevice9* dev, const D3DMATRIX* m)
	{
		if (!dev) return;

		if (!m)
		{
			if (tex0_ttff_ != D3DTTFF_DISABLE)
			{
				dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
				tex0_ttff_ = D3DTTFF_DISABLE;
			}
			tex0_ours_ = false;
			return;
		}

		// The matrix is only consulted while the flags enable it, so the two
		// shadows are independent: toggling between transformed and plain draws
		// that share one matrix re-writes the flags alone.
		if (!tex0_mat_valid_ || std::memcmp(&tex0_mat_, m, sizeof(D3DMATRIX)) != 0)
		{
			dev->SetTransform(D3DTS_TEXTURE0, m);
			tex0_mat_ = *m;
			tex0_mat_valid_ = true;
		}
		if (tex0_ttff_ != D3DTTFF_COUNT2)
		{
			dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
			tex0_ttff_ = D3DTTFF_COUNT2;
		}
		tex0_ours_ = true;
	}

	void ffp_state::on_set_texture_stage_state(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
	{
		if (stage == 0 && type == D3DTSS_TEXTURETRANSFORMFLAGS)
		{
			tex0_ttff_ = value;
			tex0_ours_ = false;
		}
	}

	void ffp_state::on_set_transform(D3DTRANSFORMSTATETYPE state)
	{
		if (state == D3DTS_TEXTURE0)
			tex0_mat_valid_ = false;
	}

	void ffp_state::mat4_transpose(float* dst, const float* src)
	{
		dst[0]  = src[0];  dst[1]  = src[4];  dst[2]  = src[8];  dst[3]  = src[12];
		dst[4]  = src[1];  dst[5]  = src[5];  dst[6]  = src[9];  dst[7]  = src[13];
		dst[8]  = src[2];  dst[9]  = src[6];  dst[10] = src[10]; dst[11] = src[14];
		dst[12] = src[3];  dst[13] = src[7];  dst[14] = src[11]; dst[15] = src[15];
	}

	bool ffp_state::mat4_is_interesting(const float* m)
	{
		bool all_zero = true;
		for (int i = 0; i < 16; i++)
		{
			if (m[i] != 0.0f) { all_zero = false; break; }
		}
		if (all_zero) return false;

		if (m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f  && m[3] == 0.0f &&
			m[4] == 0.0f && m[5] == 1.0f && m[6] == 0.0f  && m[7] == 0.0f &&
			m[8] == 0.0f && m[9] == 0.0f && m[10] == 1.0f && m[11] == 0.0f &&
			m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f && m[15] == 1.0f)
			return false;

		return true;
	}
}
