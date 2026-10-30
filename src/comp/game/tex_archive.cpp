#include "std_include.hpp"
#include "tex_archive.hpp"
#include "inflate.hpp"

#include <shlobj.h>

/*
 * BSA v104 (Fallout 3 / New Vegas) layout, little-endian:
 *
 *   header (36 bytes)
 *     char[4] "BSA\0", u32 version = 104, u32 folderRecordOffset = 36,
 *     u32 archiveFlags, u32 folderCount, u32 fileCount,
 *     u32 totalFolderNameLength, u32 totalFileNameLength, u32 fileFlags
 *   folder records (folderCount x 16): u64 hash, u32 fileCount, u32 offset
 *   file record blocks, one per folder, in folder order:
 *     [bzstring folder name]        if archiveFlags & 0x1 (u8 len incl. NUL)
 *     file records (count x 16): u64 hash, u32 size, u32 offset (absolute)
 *   file name block (totalFileNameLength): NUL-terminated names, in the same
 *     order as the file records     if archiveFlags & 0x2
 *   file data
 *     [bstring full path]           if archiveFlags & 0x100 (u8 len, no NUL)
 *     [u32 uncompressed size + zlib stream]  if compressed
 *
 * A file is compressed when (archiveFlags & 0x4) XOR (size & 0x40000000).
 * The record size includes the embedded name and the u32 prefix.
 *
 * Only folders under "textures\" are indexed: that is all texture-pack
 * support ever asks for, and it keeps the index to a few MB.
 */

namespace comp::game::tex_pack
{
	namespace
	{
		constexpr uint32_t kFlagDirNames      = 0x1;
		constexpr uint32_t kFlagFileNames     = 0x2;
		constexpr uint32_t kFlagCompressed    = 0x4;
		constexpr uint32_t kFlagEmbedNames    = 0x100;
		constexpr uint32_t kSizeToggleCompress = 0x40000000u;
		constexpr uint32_t kSizeMask          = 0x3FFFFFFFu;

		// Largest file the reader will allocate for (a 4k DXT5 with mips is
		// ~22 MB; anything past this is not a height map).
		constexpr uint32_t kMaxFileBytes = 96u * 1024u * 1024u;

		std::string lower(std::string s)
		{
			for (auto& c : s)
			{
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
				else if (c == '/') c = '\\';
			}
			return s;
		}

		std::string trim(const std::string& s)
		{
			size_t a = 0, b = s.size();
			while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '"' || s[a] == '\r' || s[a] == '\n')) a++;
			while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '"' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
			return s.substr(a, b - a);
		}

		std::vector<std::string> split_list(const std::string& s)
		{
			std::vector<std::string> out;
			size_t start = 0;
			while (start <= s.size())
			{
				size_t end = s.find(',', start);
				if (end == std::string::npos) end = s.size();
				const std::string item = trim(s.substr(start, end - start));
				if (!item.empty()) out.push_back(item);
				start = end + 1;
			}
			return out;
		}

		bool file_exists(const std::string& path)
		{
			const DWORD a = GetFileAttributesA(path.c_str());
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		}

		std::string ini_string(const std::string& ini, const char* section, const char* key)
		{
			if (!file_exists(ini)) return {};
			std::string buf(4096, '\0');
			const DWORD n = GetPrivateProfileStringA(section, key, "", buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
			buf.resize(n);
			return buf;
		}

		bool read_at(void* handle, uint64_t offset, void* dst, uint32_t size)
		{
			LARGE_INTEGER li;
			li.QuadPart = static_cast<LONGLONG>(offset);
			if (!SetFilePointerEx(handle, li, nullptr, FILE_BEGIN)) return false;
			DWORD got = 0;
			return ReadFile(handle, dst, size, &got, nullptr) && got == size;
		}
	}

	std::vector<std::string> discover_archives(const std::string& data_dir, const std::string& extra,
		std::string& detail)
	{
		std::vector<std::string> order;
		auto add = [&](const std::string& name) {
			const std::string l = lower(name);
			for (const auto& o : order)
				if (lower(o) == l) return;
			order.push_back(name);
		};

		// ---- INI archive list -------------------------------------------------
		std::string game_dir = data_dir;
		if (game_dir.size() >= 5) game_dir.resize(game_dir.size() - 5);   // strip "Data\"

		char docs[MAX_PATH] = {};
		std::string my_games;
		if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs)))
			my_games = std::string(docs) + "\\My Games\\FalloutNV\\";

		std::string list, from;
		if (!my_games.empty())
		{
			list = ini_string(my_games + "FalloutCustom.ini", "Archive", "SArchiveList");
			if (!list.empty()) from = my_games + "FalloutCustom.ini";
			if (list.empty())
			{
				list = ini_string(my_games + "Fallout.ini", "Archive", "SArchiveList");
				if (!list.empty()) from = my_games + "Fallout.ini";
			}
		}
		if (list.empty())
		{
			list = ini_string(game_dir + "Fallout_default.ini", "Archive", "SArchiveList");
			if (!list.empty()) from = game_dir + "Fallout_default.ini";
		}
		if (list.empty())
		{
			list = "Fallout - Textures.bsa, Fallout - Textures2.bsa, Fallout - Meshes.bsa, "
				"Fallout - Voices1.bsa, Fallout - Sound.bsa, Fallout - Misc.bsa";
			from = "built-in default";
		}
		for (const auto& a : split_list(list)) add(a);
		detail = std::format("SArchiveList from {} ({} entries)", from, order.size());

		if (file_exists(data_dir + "Update.bsa")) add("Update.bsa");

		// ---- Plugin-named archives ---------------------------------------------
		std::vector<std::string> plugins = { "FalloutNV.esm" };
		{
			char local[MAX_PATH] = {};
			if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local)))
			{
				std::ifstream f(std::string(local) + "\\FalloutNV\\plugins.txt");
				std::string line;
				while (std::getline(f, line))
				{
					line = trim(line);
					if (!line.empty() && line[0] == '*') line = trim(line.substr(1));
					if (line.empty() || line[0] == '#') continue;
					if (lower(line) == "falloutnv.esm") continue;
					plugins.push_back(line);
				}
			}
		}
		size_t plugin_bsas = 0;
		for (const auto& p : plugins)
		{
			const size_t dot = p.find_last_of('.');
			const std::string stem = dot == std::string::npos ? p : p.substr(0, dot);
			if (stem.empty()) continue;
			if (file_exists(data_dir + stem + ".bsa")) { add(stem + ".bsa"); plugin_bsas++; }

			WIN32_FIND_DATAA fd;
			const HANDLE h = FindFirstFileA((data_dir + stem + " - *.bsa").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) continue;
			std::vector<std::string> found;
			do
			{
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) found.emplace_back(fd.cFileName);
			} while (FindNextFileA(h, &fd));
			FindClose(h);
			std::sort(found.begin(), found.end());
			for (const auto& n : found) { add(n); plugin_bsas++; }
		}
		detail += std::format(", {} active plugin(s) -> {} plugin archive(s)", plugins.size(), plugin_bsas);

		const auto extras = split_list(extra);
		for (const auto& e : extras) add(e);
		if (!extras.empty()) detail += std::format(", {} extra", extras.size());
		return order;
	}

	TextureFs::~TextureFs()
	{
		for (auto& a : archives_)
			if (a.handle) CloseHandle(a.handle);
	}

	void TextureFs::configure(std::string data_dir, std::vector<std::string> archives)
	{
		data_dir_ = std::move(data_dir);
		archive_names_ = std::move(archives);
	}

	bool TextureFs::loose_exists(const std::string& rel) const
	{
		return file_exists(data_dir_ + rel);
	}

	void TextureFs::build_index()
	{
		indexed_ = true;
		for (const auto& n : archive_names_)
		{
			if (archives_.size() >= 0xFFFF) break;
			if (index_archive(n, static_cast<uint16_t>(archives_.size()))) stats_.archives_opened++;
			else stats_.archives_failed++;
		}
		stats_.texture_entries = static_cast<uint32_t>(index_.size());
	}

	bool TextureFs::index_archive(const std::string& name, uint16_t id)
	{
		const std::string path = data_dir_ + name;
		const HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			report_.push_back(std::format("{}: not found", name));
			return false;
		}

		auto fail = [&](const char* why) {
			report_.push_back(std::format("{}: {}", name, why));
			CloseHandle(h);
			return false;
		};

		uint32_t hdr[9] = {};
		if (!read_at(h, 0, hdr, sizeof(hdr))) return fail("short header");
		if (hdr[0] != 0x00415342u) return fail("not a BSA");
		if (hdr[1] != 104) return fail(std::format("unsupported version {}", hdr[1]).c_str());
		const uint32_t flags = hdr[3], folders = hdr[4], files = hdr[5];
		const uint32_t folder_names = hdr[6], file_names = hdr[7];
		if (!(flags & kFlagDirNames) || !(flags & kFlagFileNames)) return fail("archive has no name tables");
		if (folders > 1000000 || files > 4000000) return fail("implausible counts");

		const uint64_t dir_bytes = static_cast<uint64_t>(folders) * 16 + folder_names + folders +
			static_cast<uint64_t>(files) * 16 + file_names;
		if (dir_bytes > 256ull * 1024 * 1024) return fail("directory too large");
		std::vector<uint8_t> dir(static_cast<size_t>(dir_bytes));
		if (!read_at(h, hdr[2], dir.data(), static_cast<uint32_t>(dir.size()))) return fail("short directory");

		// Pass 1: walk folder blocks, collecting (folder name, record ptr) in order.
		struct Rec { uint32_t size, offset; };
		std::vector<std::pair<std::string, Rec>> recs;   // full path (textures only) or empty
		recs.reserve(files);
		size_t p = static_cast<size_t>(folders) * 16;
		const uint8_t* d = dir.data();
		const size_t end_records = static_cast<size_t>(folders) * 16 + folder_names + folders + static_cast<size_t>(files) * 16;
		for (uint32_t f = 0; f < folders; f++)
		{
			uint32_t count;
			std::memcpy(&count, d + f * 16 + 8, 4);
			if (p >= end_records) return fail("folder block overrun");
			const uint8_t nlen = d[p++];
			if (p + nlen > end_records) return fail("folder name overrun");
			std::string folder(reinterpret_cast<const char*>(d + p), nlen ? nlen - 1u : 0u);
			p += nlen;
			folder = lower(folder);
			const bool textures = folder.rfind("textures\\", 0) == 0 || folder == "textures";
			if (p + static_cast<size_t>(count) * 16 > end_records) return fail("file record overrun");
			for (uint32_t i = 0; i < count; i++)
			{
				Rec r;
				std::memcpy(&r.size, d + p + 8, 4);
				std::memcpy(&r.offset, d + p + 12, 4);
				p += 16;
				recs.emplace_back(textures ? folder : std::string(), r);
			}
		}
		if (recs.size() != files) return fail("file count mismatch");

		// Pass 2: the name block, in record order.
		size_t q = end_records;
		const size_t names_end = end_records + file_names;
		uint32_t added = 0;
		for (auto& [folder, r] : recs)
		{
			const size_t start = q;
			while (q < names_end && d[q] != 0) q++;
			if (q >= names_end) return fail("file name overrun");
			const std::string fname(reinterpret_cast<const char*>(d + start), q - start);
			q++;
			if (folder.empty()) continue;

			Entry e;
			e.archive = id;
			e.offset = r.offset;
			e.size = r.size & kSizeMask;
			e.compressed = ((flags & kFlagCompressed) != 0) != ((r.size & kSizeToggleCompress) != 0);
			// Later archives override earlier ones, like the game's own load order.
			index_[folder + "\\" + lower(fname)] = e;
			added++;
		}

		Archive a;
		a.name = name;
		a.handle = h;
		a.flags = flags;
		archives_.push_back(std::move(a));
		report_.push_back(std::format("{}: {} files, {} under textures\\{}", name, files, added,
			(flags & kFlagCompressed) ? ", compressed" : ""));
		return true;
	}

	TextureFs::Source TextureFs::locate(const std::string& rel)
	{
		if (loose_exists(rel)) return Source::Loose;
		if (!indexed_) build_index();
		return index_.count(rel) ? Source::Archive : Source::None;
	}

	bool TextureFs::read(const std::string& rel, std::vector<uint8_t>& out, Source& src, std::string& err)
	{
		out.clear();
		src = Source::None;

		if (loose_exists(rel))
		{
			std::ifstream f(data_dir_ + rel, std::ios::binary | std::ios::ate);
			if (!f) { err = "loose file open failed"; stats_.read_errors++; return false; }
			const auto sz = static_cast<std::streamoff>(f.tellg());
			if (sz <= 0 || sz > kMaxFileBytes) { err = "loose file size out of range"; stats_.read_errors++; return false; }
			out.resize(static_cast<size_t>(sz));
			f.seekg(0);
			if (!f.read(reinterpret_cast<char*>(out.data()), sz)) { err = "loose file read failed"; stats_.read_errors++; return false; }
			src = Source::Loose;
			stats_.loose_hits++;
			return true;
		}

		if (!indexed_) build_index();
		const auto it = index_.find(rel);
		if (it == index_.end()) { stats_.misses++; err = "not found"; return false; }
		const Entry& e = it->second;
		const Archive& a = archives_[e.archive];
		if (e.size == 0 || e.size > kMaxFileBytes) { err = "archive entry size out of range"; stats_.read_errors++; return false; }

		std::vector<uint8_t> raw(e.size);
		if (!read_at(a.handle, e.offset, raw.data(), e.size)) { err = "archive read failed"; stats_.read_errors++; return false; }
		size_t p = 0;
		if (a.flags & kFlagEmbedNames)
		{
			p = 1u + raw[0];
			if (p > raw.size()) { err = "embedded name overrun"; stats_.read_errors++; return false; }
		}

		if (!e.compressed)
		{
			out.assign(raw.begin() + static_cast<std::ptrdiff_t>(p), raw.end());
		}
		else
		{
			if (p + 4 > raw.size()) { err = "compressed entry too short"; stats_.read_errors++; return false; }
			uint32_t orig = 0;
			std::memcpy(&orig, raw.data() + p, 4);
			p += 4;
			if (orig == 0 || orig > kMaxFileBytes) { err = "uncompressed size out of range"; stats_.read_errors++; return false; }
			out.resize(orig);
			if (!zlib_inflate(raw.data() + p, raw.size() - p, out.data(), out.size()))
			{
				out.clear();
				err = "inflate failed";
				stats_.read_errors++;
				return false;
			}
		}
		src = Source::Archive;
		stats_.archive_hits++;
		return true;
	}
}
