#pragma once
#include "shared/common/ffp_state.hpp"

namespace comp
{
	// Resets the renderer's cached device state (the WORLD=identity shadow)
	// on device loss. Called from d3d9ex Reset with ffp_state::on_reset.
	void renderer_on_reset();

	namespace tex_addons
	{
		extern bool initialized;
		extern LPDIRECT3DTEXTURE9 berry;
		extern void init_texture_addons(bool release = false);
	}

	// Render states a draw changed, put back after it. restore_all also
	// resets the texture 0 transform to identity.
	class drawcall_mod_context
	{
	public:
		// Returns false if the render state was already saved.
		bool save_rs(IDirect3DDevice9* device, const D3DRENDERSTATETYPE& state)
		{
			if (saved_render_state_.contains(state)) {
				return false;
			}

			DWORD temp;
			device->GetRenderState(state, &temp);
			saved_render_state_[state] = temp;
			dirty_ = true;
			return true;
		}

		// Sets the texture 0 transform back to identity.
		void restore_texture_transform(IDirect3DDevice9* device)
		{
			device->SetTransform(D3DTS_TEXTURE0, &shared::globals::IDENTITY);
			// Written straight to the device, so the FFP tracker's stage-0
			// transform shadow no longer describes what is bound.
			shared::common::ffp_state::get().invalidate_stage0_tex_transform();
		}

		void restore_all(IDirect3DDevice9* device)
		{
			if (!dirty_) return;

			restore_texture_transform(device);

			for (auto& rs : saved_render_state_) {
				device->SetRenderState(rs.first, rs.second);
			}
		}

		void reset_context()
		{
			if (!dirty_) return;

			saved_render_state_.clear();
			dirty_ = false;
		}

		drawcall_mod_context() = default;

	private:
		// Set by save_rs. restore_all and reset_context early-return when
		// false, so a draw that changed nothing skips the map.
		bool dirty_ = false;

		std::unordered_map<D3DRENDERSTATETYPE, DWORD> saved_render_state_;
	};

	class renderer final : public shared::common::loader::component_module
	{
	public:
		renderer();
		~renderer();

		static inline renderer* p_this = nullptr;
		static renderer* get() { return p_this; }

		static bool is_initialized()
		{
			if (const auto mod = get(); mod && mod->m_initialized) {
				return true;
			}
			return false;
		}

		HRESULT on_draw_primitive(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const UINT& StartVertex, const UINT& PrimitiveCount);
		HRESULT on_draw_indexed_prim(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const INT& BaseVertexIndex, const UINT& MinVertexIndex, const UINT& NumVertices, const UINT& startIndex, const UINT& primCount);

		static inline drawcall_mod_context dc_ctx {};

	private:
		bool m_initialized = false;
	};
}
