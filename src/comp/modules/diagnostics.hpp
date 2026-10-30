#pragma once

namespace comp
{
	// Draw-call logging for FFP debugging. After [Diagnostics] DelayMs
	// (default 50000) it logs LogFrames frames (default 3) to ffp_proxy.log in
	// the log directory: VS constant writes and matrices, vertex declarations
	// and skinning flags, texture stage bindings, draw parameters and strides,
	// and raw vertex bytes for the first draws. The delay lets the game reach
	// real in-game geometry first.
	class diagnostics final : public shared::common::loader::component_module
	{
	public:
		diagnostics();
		~diagnostics();

		static inline diagnostics* p_this = nullptr;
		static diagnostics* get() { return p_this; }

		bool is_active() const;

		// Truncates the log, resets declaration/texture state and the timer so
		// the next LogFrames frames are captured. Bound to F11.
		void restart();

		void on_draw_indexed_prim(UINT draw_count, IDirect3DDevice9* dev,
			D3DPRIMITIVETYPE pt, INT base_vtx, UINT num_verts, UINT prim_count);
		void on_draw_primitive(UINT draw_count, D3DPRIMITIVETYPE pt,
			UINT start_vtx, UINT prim_count);

		void on_present(UINT frame_count, UINT draw_count, UINT scene_count);
		void on_set_vs_const_f(UINT start_reg, const float* data, UINT count);

		// Logs each vertex declaration once.
		void log_decl(IDirect3DVertexDeclaration9* decl);

		// Writes the draw's routing decision while active.
		void route(const char* tag);

		// With [Diagnostics] HarvestPS=1, logs each pixel shader's hash and
		// draw-shape fingerprint to ps_harvest.log on first sight, to identify
		// UI and keep-diffuse shaders by matching first appearance to in-game
		// actions. Called per draw; does nothing unless harvesting is on.
		void harvest_draw();

	private:
		HANDLE log_file_ = INVALID_HANDLE_VALUE;
		HANDLE harvest_file_ = INVALID_HANDLE_VALUE;
		bool harvest_ps_ = false;
		DWORD delay_ms_ = 50000;
		int max_frames_ = 3;
		int logged_frames_ = 0;
		DWORD create_tick_ = 0;

		IDirect3DVertexDeclaration9* logged_decls_[32] = {};
		int logged_decl_count_ = 0;

		// Unique textures per stage this frame.
		IDirect3DBaseTexture9* diag_tex_seen_[8][32] = {};
		int diag_tex_uniq_[8] = {};

		void log_str(const char* s);
		void log_int(const char* prefix, int val);
		void log_hex(const char* prefix, unsigned int val);
		void log_floats_dec(const char* prefix, const float* data, unsigned int count);
		void log_matrix(const char* name, const float* m);

		void track_unique_textures();
		void reset_frame_diag();
	};
}
