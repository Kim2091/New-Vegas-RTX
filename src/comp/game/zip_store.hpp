#pragma once

// A minimal zip writer: deflated or stored entries with CRC-32, UTF-8
// names, no zip64. Deflate is miniz's (deps/miniz), streamed straight to
// the file. Every zip tool and Windows' own Extract All read it.
// The archive stays under 4 GB: an entry that would cross kMaxBytes is
// refused, and add() returns false.
namespace comp::game::zip_store
{
	enum class Method : uint16_t
	{
		Store = 0,      // for data that is compressed already (PNG)
		Deflate = 8,
	};

	class writer
	{
	public:
		static constexpr uint64_t kMaxBytes = 0xF0000000ull;

		writer() = default;
		~writer();
		writer(const writer&) = delete;
		writer& operator=(const writer&) = delete;

		// Creates (replaces) the file at `path`.
		bool open(const std::wstring& path);

		// An entry from memory. `name` is the path inside the zip, '/'
		// separated, UTF-8; `mtime` its time (UTC).
		bool add(const std::string& name, const void* data, size_t size, const FILETIME& mtime,
			Method method = Method::Deflate);

		// An entry streamed from a file on disk, in 1 MiB pieces.
		bool add_file(const std::string& name, const std::wstring& path, Method method = Method::Deflate);

		// An entry written in pieces: begin_entry(), write_entry() for each
		// piece, end_entry(). `size_bound` is the most it can hold (the 4 GB
		// check). end_entry() is false when the entry is incomplete.
		bool begin_entry(const std::string& name, const FILETIME& mtime, uint64_t size_bound, Method method = Method::Deflate);
		bool write_entry(const void* data, size_t size);
		bool end_entry();

		// Past this GetTickCount64() time every write fails and timed_out()
		// is true.
		void set_deadline(uint64_t tick_ms) { deadline_ = tick_ms; }
		bool timed_out() const { return timed_out_; }

		// Writes the central directory and closes the file. False: the zip
		// is incomplete and should be deleted.
		bool close();

		size_t entries() const { return entries_.size(); }
		uint64_t bytes() const { return offset_; }
		// The entries' sizes before compression.
		uint64_t raw_bytes() const { return raw_bytes_; }

	private:
		struct entry
		{
			std::string name;
			Method method = Method::Deflate;
			uint32_t crc = 0;
			uint32_t packed = 0;    // size in the zip
			uint32_t size = 0;      // size unpacked
			uint32_t offset = 0;
			uint16_t dos_time = 0;
			uint16_t dos_date = 0;
		};

		// Streams one entry: begin() writes the local header, feed() the data
		// (CRC-32 and deflate as it goes), finish() fills the header's CRC
		// and sizes in and records the entry.
		bool begin(entry& e, const FILETIME& mtime, uint64_t size);
		bool feed(entry& e, const void* data, size_t size);
		bool finish(entry& e);

		bool write(const void* data, size_t size);
		static int put(const void* data, int size, void* user);

		HANDLE file_ = INVALID_HANDLE_VALUE;
		uint64_t offset_ = 0;
		uint64_t raw_bytes_ = 0;
		uint64_t data_start_ = 0;
		bool failed_ = false;
		bool timed_out_ = false;
		uint64_t deadline_ = 0;
		bool in_entry_ = false;
		entry current_;               // the entry begin_entry() opened
		void* deflate_ = nullptr;     // miniz's tdefl_compressor, made at the first deflated entry
		std::vector<entry> entries_;
	};

	// CRC-32 (IEEE 802.3, the zip one), continued from `crc`.
	uint32_t crc32(uint32_t crc, const void* data, size_t size);
}
