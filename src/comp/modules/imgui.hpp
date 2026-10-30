#pragma once

namespace comp
{
	class imgui final : public shared::common::loader::component_module
	{
	public:
		imgui();

		static inline imgui* p_this = nullptr;
		static imgui* get() { return p_this; }

		static void on_present();

		void devgui();
		void theme();
		bool input_message(UINT message_type, WPARAM wparam, LPARAM lparam);

		bool m_initialized_device = false;

		class ImGuiStats
		{
		private:
			static inline bool m_tracking_enabled = false;

		public:
			bool is_tracking_enabled() const {
				return m_tracking_enabled;
			}

			void enable_tracking(const bool state)
			{
				// Reset the stats when tracking is turned off.
				if (!state && m_tracking_enabled) {
					this->reset_stats();
				}

				m_tracking_enabled = state;
			}

			class StatObj
			{
			public:
				enum class Mode
				{
					Single = 0,
					ConditionalCheck = 1,
				};

				StatObj(Mode mode) : m_mode(mode) {};

				auto& get_mode() const { return m_mode; }
				auto& get_total() const { return m_num_total; }
				auto& get_successful() const { return m_num_successful; }

				void track_single()
				{
					if (m_tracking_enabled) {
						++m_num_total;
					}
				}

				void reset()
				{
					m_num_total = 0u;
					m_num_successful = 0u;
				}

				Mode m_mode = Mode::Single;

			private:
				std::uint32_t m_num_total{ 0 };
				std::uint32_t m_num_successful{ 0 };
			};

			StatObj _drawcall_prim = { StatObj::Mode::Single };
			StatObj _drawcall_prim_incl_ignored = { StatObj::Mode::Single };
			StatObj _drawcall_using_vs = { StatObj::Mode::Single };

			StatObj _drawcall_indexed_prim{ StatObj::Mode::Single };
			StatObj _drawcall_indexed_prim_incl_ignored = { StatObj::Mode::Single };

			StatObj _drawcall_indexed_prim_using_vs = { StatObj::Mode::Single };


			ImGuiStats()
			{
				m_stat_list.emplace_back("DrawPrim Calls", &_drawcall_prim);
				m_stat_list.emplace_back("DrawPrim +Ignored", &_drawcall_prim_incl_ignored);
				m_stat_list.emplace_back("DrawPrim VS", &_drawcall_using_vs);

				m_stat_list.emplace_back();
				m_stat_list.emplace_back("DrawIndexedPrim Calls", &_drawcall_indexed_prim);
				m_stat_list.emplace_back("DrawIndexedPrim +Ignored", &_drawcall_indexed_prim_incl_ignored);
				m_stat_list.emplace_back("DrawIndexedPrim VS", &_drawcall_indexed_prim_using_vs);
			}

			void draw_stats();

			void reset_stats()
			{
				for (auto& p : m_stat_list)
				{
					if (p.second) {
						p.second->reset();
					}
				}
			}

		private:
			std::vector<std::pair<const char*, StatObj*>> m_stat_list;

			void display_single_stat(const char* name, const StatObj& stat);
		};

		ImGuiStats m_stats = {};

		static bool is_initialized()
		{
			if (const auto im = imgui::get(); im && im->m_initialized){
				return true;
			}
			return false;
		}

	private:
		void tab_about();
		void tab_dev();
		void tab_ffp();
		
		bool m_im_window_focused = false;
		bool m_im_window_hovered = false;
		std::string m_devgui_custom_footer_content;

		bool m_initialized = false;
	};
}
