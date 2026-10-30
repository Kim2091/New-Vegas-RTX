#include "std_include.hpp"
#include "zip_store.hpp"

// build.bat compiles deps\miniz\miniz.c with the same defines.
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#pragma warning(push, 0)
#include "miniz.h"
#pragma warning(pop)

namespace comp::game::zip_store
{
	namespace
	{
		constexpr uint32_t kLocalSig = 0x04034B50;
		constexpr uint32_t kCentralSig = 0x02014B50;
		constexpr uint32_t kEndSig = 0x06054B50;
		constexpr uint16_t kVersion = 20;           // 2.0: deflate, and entries in folders
		constexpr uint16_t kFlagUtf8 = 0x0800;      // general purpose bit 11: the name is UTF-8
		constexpr size_t kMaxEntries = 0xFFFF;
		constexpr size_t kChunk = 1u << 20;
		constexpr int kLevel = 6;                   // zlib's default: logs shrink about 10x

		const std::array<uint32_t, 256>& crc_table()
		{
			static const std::array<uint32_t, 256> table = []
				{
					std::array<uint32_t, 256> t = {};
					for (uint32_t i = 0; i < 256; i++)
					{
						uint32_t c = i;
						for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
						t[i] = c;
					}
					return t;
				}();
			return table;
		}

		void put16(std::vector<uint8_t>& b, uint16_t v)
		{
			b.push_back(static_cast<uint8_t>(v));
			b.push_back(static_cast<uint8_t>(v >> 8));
		}

		void put32(std::vector<uint8_t>& b, uint32_t v)
		{
			for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
		}

		// The entry time as MS-DOS local date and time; 1980-01-01 when the
		// time can't be expressed.
		void dos_time(const FILETIME& utc, uint16_t& time, uint16_t& date)
		{
			FILETIME local = {};
			WORD d = 0, t = 0;
			if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToDosDateTime(&local, &d, &t))
			{
				date = d;
				time = t;
				return;
			}
			date = (1 << 5) | 1;
			time = 0;
		}

		// CRC and sizes are 0 here: finish() writes them once the data is in.
		std::vector<uint8_t> local_header(const std::string& name, uint16_t method, uint16_t time, uint16_t date)
		{
			std::vector<uint8_t> h;
			h.reserve(30 + name.size());
			put32(h, kLocalSig);
			put16(h, kVersion);
			put16(h, kFlagUtf8);
			put16(h, method);
			put16(h, time);
			put16(h, date);
			put32(h, 0);                                // CRC-32
			put32(h, 0);                                // compressed size
			put32(h, 0);                                // uncompressed size
			put16(h, static_cast<uint16_t>(name.size()));
			put16(h, 0);                                // extra field length
			h.insert(h.end(), name.begin(), name.end());
			return h;
		}
	}

	uint32_t crc32(uint32_t crc, const void* data, size_t size)
	{
		const auto& table = crc_table();
		const auto* p = static_cast<const uint8_t*>(data);
		crc = ~crc;
		for (size_t i = 0; i < size; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	writer::~writer()
	{
		if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
		if (deflate_) tdefl_compressor_free(static_cast<tdefl_compressor*>(deflate_));
	}

	bool writer::open(const std::wstring& path)
	{
		file_ = CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		offset_ = 0;
		raw_bytes_ = 0;
		failed_ = file_ == INVALID_HANDLE_VALUE;
		entries_.clear();
		return !failed_;
	}

	bool writer::write(const void* data, size_t size)
	{
		if (failed_) return false;
		if (deadline_ && GetTickCount64() > deadline_)
		{
			timed_out_ = true;
			failed_ = true;
			return false;
		}
		const auto* p = static_cast<const uint8_t*>(data);
		while (size)
		{
			const DWORD n = static_cast<DWORD>((std::min)(size, kChunk));
			DWORD written = 0;
			if (!WriteFile(file_, p, n, &written, nullptr) || written != n)
			{
				failed_ = true;
				return false;
			}
			p += n;
			size -= n;
			offset_ += n;
		}
		return true;
	}

	// miniz's output callback: deflated data straight into the file.
	int writer::put(const void* data, int size, void* user)
	{
		auto* self = static_cast<writer*>(user);
		return size >= 0 && self->write(data, static_cast<size_t>(size)) ? 1 : 0;
	}

	bool writer::begin(entry& e, const FILETIME& mtime, uint64_t size)
	{
		if (failed_ || entries_.size() >= kMaxEntries) return false;
		// Deflate can grow data that does not compress by a few bytes per 64 KiB block.
		const uint64_t worst = size + size / 1024 + 64;
		if (offset_ + 30 + e.name.size() + worst + 46 * (entries_.size() + 1) + 22 > kMaxBytes) return false;
		e.offset = static_cast<uint32_t>(offset_);
		dos_time(mtime, e.dos_time, e.dos_date);
		if (e.method == Method::Deflate)
		{
			if (!deflate_) deflate_ = tdefl_compressor_alloc();
			if (!deflate_) return false;
			const int flags = static_cast<int>(tdefl_create_comp_flags_from_zip_params(kLevel, -MZ_DEFAULT_WINDOW_BITS,
				MZ_DEFAULT_STRATEGY));
			if (tdefl_init(static_cast<tdefl_compressor*>(deflate_), &writer::put, this, flags) != TDEFL_STATUS_OKAY)
				return false;
		}
		const auto h = local_header(e.name, static_cast<uint16_t>(e.method), e.dos_time, e.dos_date);
		if (!write(h.data(), h.size())) return false;
		data_start_ = offset_;
		return true;
	}

	bool writer::feed(entry& e, const void* data, size_t size)
	{
		if (failed_) return false;
		e.crc = crc32(e.crc, data, size);
		e.size += static_cast<uint32_t>(size);
		if (e.method == Method::Store) return write(data, size);
		return tdefl_compress_buffer(static_cast<tdefl_compressor*>(deflate_), data, size, TDEFL_NO_FLUSH) ==
			TDEFL_STATUS_OKAY && !failed_;
	}

	bool writer::finish(entry& e)
	{
		if (failed_) return false;
		if (e.method == Method::Deflate &&
			tdefl_compress_buffer(static_cast<tdefl_compressor*>(deflate_), nullptr, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE)
		{
			failed_ = true;
			return false;
		}
		if (failed_) return false;
		e.packed = static_cast<uint32_t>(offset_ - data_start_);

		std::vector<uint8_t> fix;
		put32(fix, e.crc);
		put32(fix, e.packed);
		put32(fix, e.size);
		LARGE_INTEGER at = {}, end = {};
		at.QuadPart = static_cast<LONGLONG>(e.offset) + 14;
		end.QuadPart = static_cast<LONGLONG>(offset_);
		DWORD written = 0;
		if (!SetFilePointerEx(file_, at, nullptr, FILE_BEGIN) || !WriteFile(file_, fix.data(), 12, &written, nullptr) ||
			written != 12 || !SetFilePointerEx(file_, end, nullptr, FILE_BEGIN))
		{
			failed_ = true;
			return false;
		}
		raw_bytes_ += e.size;
		entries_.push_back(std::move(e));
		return true;
	}

	bool writer::begin_entry(const std::string& name, const FILETIME& mtime, uint64_t size_bound, Method method)
	{
		if (in_entry_) return false;
		current_ = entry();
		current_.name = name;
		current_.method = method;
		in_entry_ = begin(current_, mtime, size_bound);
		return in_entry_;
	}

	bool writer::write_entry(const void* data, size_t size)
	{
		if (!in_entry_) return false;
		const auto* p = static_cast<const uint8_t*>(data);
		for (size_t done = 0; done < size;)
		{
			const size_t n = (std::min)(size - done, kChunk);
			if (!feed(current_, p + done, n)) return false;
			done += n;
		}
		return true;
	}

	bool writer::end_entry()
	{
		if (!in_entry_) return false;
		in_entry_ = false;
		return finish(current_);
	}

	bool writer::add(const std::string& name, const void* data, size_t size, const FILETIME& mtime, Method method)
	{
		if (!begin_entry(name, mtime, size, method)) return false;
		const bool ok = write_entry(data, size);
		return end_entry() && ok;
	}

	bool writer::add_file(const std::string& name, const std::wstring& path, Method method)
	{
		if (failed_) return false;
		const HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (in == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER size = {};
		FILETIME mtime = {};
		if (!GetFileSizeEx(in, &size) || !GetFileTime(in, nullptr, nullptr, &mtime) ||
			!begin_entry(name, mtime, static_cast<uint64_t>(size.QuadPart), method))
		{
			CloseHandle(in);
			return false;
		}

		// The size read now bounds the copy; a file still being written may
		// end up shorter, and the header gets what was copied.
		std::vector<uint8_t> buf(kChunk);
		const uint64_t total = static_cast<uint64_t>(size.QuadPart);
		uint64_t copied = 0;
		bool ok = true;
		while (copied < total)
		{
			DWORD got = 0;
			const DWORD want = static_cast<DWORD>((std::min)(static_cast<uint64_t>(kChunk), total - copied));
			if (!ReadFile(in, buf.data(), want, &got, nullptr) || got == 0) break;
			if (!write_entry(buf.data(), got))
			{
				ok = false;
				break;
			}
			copied += got;
		}
		CloseHandle(in);
		return end_entry() && ok;
	}

	bool writer::close()
	{
		if (file_ == INVALID_HANDLE_VALUE) return false;
		const uint32_t cd_offset = static_cast<uint32_t>(offset_);
		std::vector<uint8_t> cd;
		for (const entry& e : entries_)
		{
			put32(cd, kCentralSig);
			put16(cd, kVersion);                        // made by: MS-DOS, 2.0
			put16(cd, kVersion);
			put16(cd, kFlagUtf8);
			put16(cd, static_cast<uint16_t>(e.method));
			put16(cd, e.dos_time);
			put16(cd, e.dos_date);
			put32(cd, e.crc);
			put32(cd, e.packed);
			put32(cd, e.size);
			put16(cd, static_cast<uint16_t>(e.name.size()));
			put16(cd, 0);                               // extra field length
			put16(cd, 0);                               // comment length
			put16(cd, 0);                               // disk number
			put16(cd, 0);                               // internal attributes
			put32(cd, 0);                               // external attributes
			put32(cd, e.offset);
			cd.insert(cd.end(), e.name.begin(), e.name.end());
		}
		const uint32_t cd_size = static_cast<uint32_t>(cd.size());
		put32(cd, kEndSig);
		put16(cd, 0);                                   // this disk
		put16(cd, 0);                                   // the disk with the central directory
		put16(cd, static_cast<uint16_t>(entries_.size()));
		put16(cd, static_cast<uint16_t>(entries_.size()));
		put32(cd, cd_size);
		put32(cd, cd_offset);
		put16(cd, 0);                                   // comment length
		const bool ok = write(cd.data(), cd.size()) && FlushFileBuffers(file_);
		CloseHandle(file_);
		file_ = INVALID_HANDLE_VALUE;
		return ok && !failed_;
	}
}
