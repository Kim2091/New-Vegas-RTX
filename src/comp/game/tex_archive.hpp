#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Texture file resolution for texture-pack support: the game's loose Data
// folder first, then its loaded BSA archives, the way the engine resolves a
// texture path. It is a standalone reader for Fallout 3 / New Vegas BSA
// version 104 and does not call the engine's own resolver.
//
// Not thread-safe: owned by the texture-pack worker thread.

namespace comp::game::tex_pack
{
	// Archive names in load order (lowest priority first), as the game would
	// load them: [Archive] SArchiveList from Fallout.ini (FalloutCustom.ini
	// overrides it, Fallout_default.ini is the fallback), Update.bsa when
	// present and unlisted, then for every active plugin (plugins.txt, with
	// FalloutNV.esm implied first) "<Plugin>.bsa" and "<Plugin> - *.bsa", then
	// `extra` (comma-separated, from [TexturePacks] ExtraArchives).
	// `detail` receives a human-readable account for the log.
	std::vector<std::string> discover_archives(const std::string& data_dir, const std::string& extra,
		std::string& detail);

	class TextureFs
	{
	public:
		enum class Source : uint8_t { None, Loose, Archive };

		struct Stats
		{
			uint32_t archives_opened = 0;
			uint32_t archives_failed = 0;
			uint32_t texture_entries = 0;   // unique textures\ paths across all archives
			uint32_t loose_hits = 0;
			uint32_t archive_hits = 0;
			uint32_t misses = 0;
			uint32_t read_errors = 0;
		};

		~TextureFs();

		// `data_dir` ends with a backslash; `archives` in load order.
		void configure(std::string data_dir, std::vector<std::string> archives);

		// `rel` is a normalised path: lower case, backslashes, starting with
		// "textures\". Indexes the archives on first use.
		Source locate(const std::string& rel);

		// Reads (and inflates) the file. Loose files win over archives; among
		// archives the last-loaded one wins.
		bool read(const std::string& rel, std::vector<uint8_t>& out, Source& src, std::string& err);

		const Stats& stats() const { return stats_; }
		bool indexed() const { return indexed_; }
		// One line per archive, for the log.
		const std::vector<std::string>& archive_report() const { return report_; }

	private:
		struct Archive
		{
			std::string name;
			void* handle = nullptr;   // HANDLE
			uint32_t flags = 0;
		};

		struct Entry
		{
			uint32_t offset = 0;
			uint32_t size = 0;        // stored size (without the compression toggle bit)
			uint16_t archive = 0;
			bool compressed = false;
		};

		std::string data_dir_;
		std::vector<std::string> archive_names_;
		std::vector<Archive> archives_;
		std::unordered_map<std::string, Entry> index_;
		std::vector<std::string> report_;
		Stats stats_;
		bool indexed_ = false;

		void build_index();
		bool index_archive(const std::string& name, uint16_t id);
		bool loose_exists(const std::string& rel) const;
	};
}
