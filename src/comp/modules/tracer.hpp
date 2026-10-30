#pragma once

namespace comp
{
	class tracer final : public shared::common::loader::component_module
	{
	public:
		tracer();
		~tracer();

		static inline tracer* p_this = nullptr;
		static tracer* get() { return p_this; }

		// Hot path: one bool test.
		static bool is_active() { return p_this && p_this->capturing_; }
		static tracer& ref() { return *p_this; }

		void start_capture(int num_frames, const std::string& filename);
		void stop_capture();
		bool is_capturing() const { return capturing_; }

		// Frame boundaries, called from d3d9ex.cpp.
		void on_present();
		void on_reset();

		// JSONL record builder, called from the generated dispatch functions.
		void record_begin(const char* method, int slot);
		void record_arg_uint(const char* name, DWORD val);
		void record_arg_int(const char* name, INT val);
		void record_arg_float(const char* name, float val);
		void record_arg_ptr(const char* name, const void* val);
		void record_data_float(const char* name, const float* data, UINT count);
		void record_data_int(const char* name, const int* data, UINT count);
		void record_data_shader(const char* name, const DWORD* bytecode);
		void record_data_vtxdecl(const char* name, const BYTE* elements);
		void record_clear_data(DWORD flags, DWORD color, float z, DWORD stencil);
		void record_backtrace();
		void record_end();

		// After a Create* call: records the created handle.
		void record_created_handle(const void* handle);

		int frames_to_capture() const { return frames_to_capture_; }
		int frames_captured() const { return frames_captured_; }
		int sequence() const { return sequence_; }
		int backtrace_depth() const { return backtrace_depth_; }
		void set_backtrace_depth(int d) { backtrace_depth_ = d; }
		const std::string& last_capture_path() const { return last_capture_path_; }
		uint64_t last_capture_size() const { return last_capture_size_; }
		int last_capture_records() const { return last_capture_records_; }
		const std::string& output_dir() const { return output_dir_; }

		// Default timestamped file name, without extension.
		static std::string generate_default_filename();

	private:
		bool capturing_ = false;
		int frames_to_capture_ = 2;
		int trigger_poll_ = 0;
		void poll_trigger_file();
		int frames_captured_ = 0;
		int sequence_ = 0;
		int frame_index_ = 0;

		HANDLE log_file_ = INVALID_HANDLE_VALUE;
		static constexpr size_t BUFFER_SIZE = 4 * 1024 * 1024;
		static constexpr size_t FLUSH_THRESHOLD = 3 * 1024 * 1024;
		char* buffer_ = nullptr;
		size_t write_pos_ = 0;

		// JSON state machine
		bool first_arg_ = true;
		bool args_open_ = false;
		bool data_open_ = false;

		int backtrace_depth_ = 8;
		std::string output_dir_ = "captures";

		std::string last_capture_path_;
		uint64_t last_capture_size_ = 0;
		int last_capture_records_ = 0;

		void flush_buffer();
		void buf_append(const char* str, size_t len);
		void buf_printf(const char* fmt, ...);
		void buf_write_float(float val);
		void close_sections();
	};
}
