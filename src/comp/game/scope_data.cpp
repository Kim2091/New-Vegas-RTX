#include "std_include.hpp"
#include "scope_data.hpp"

#include "inflate.hpp"
#include "nvse_link.hpp"
#include "session_files.hpp"

namespace comp::game::scopes::data
{
	namespace
	{
		void log(const std::string& msg, shared::common::LOG_TYPE type = shared::common::LOG_TYPE::LOG_TYPE_DEFAULT)
		{
			shared::common::log("Scopes", msg, type, false);
		}

		// ---- EditorIDs from the plugin files ---------------------------------------

		// A record or group header (TES4 format, 24 bytes).
		struct Header
		{
			char type[4];
			uint32_t size;       // data size; a group's size includes its header
			uint32_t flags;      // a group's label
			uint32_t form_id;    // a group's type
			uint32_t stamp;
			uint32_t version;
		};
		static_assert(sizeof(Header) == 24);

		constexpr uint32_t kRecordCompressed = 0x00040000;
		constexpr uint32_t kRecordDeleted = 0x20;
		constexpr size_t kMaxGroup = 64u << 20;
		constexpr size_t kMaxRecord = 1u << 20;

		bool read_at(HANDLE h, uint64_t off, void* dst, DWORD n)
		{
			LARGE_INTEGER at = {};
			at.QuadPart = static_cast<LONGLONG>(off);
			DWORD got = 0;
			return SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, dst, n, &got, nullptr) && got == n;
		}

		bool is(const char (&type)[4], const char* want)
		{
			return std::memcmp(type, want, 4) == 0;
		}

		std::string lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// The record's EDID, from its (decompressed) data.
		std::string record_edid(const uint8_t* p, size_t n)
		{
			size_t i = 0;
			uint32_t big = 0;
			while (i + 6 <= n)
			{
				const uint8_t* s = p + i;
				uint32_t len = static_cast<uint32_t>(s[4] | (s[5] << 8));
				if (big) { len = big; big = 0; }
				if (std::memcmp(s, "XXXX", 4) == 0 && len == 4 && i + 10 <= n)
				{
					std::memcpy(&big, s + 6, 4);
					i += 10;
					continue;
				}
				if (std::memcmp(s, "EDID", 4) == 0 && i + 6 + len <= n)
				{
					std::string id(reinterpret_cast<const char*>(s + 6), len);
					while (!id.empty() && id.back() == '\0') id.pop_back();
					return id;
				}
				i += 6 + len;
			}
			return {};
		}

		// One plugin file's WEAP records into `out`, keyed by run-time form
		// ID: a record's high byte indexes the file's masters, or the file
		// itself past them.
		void scan_plugin(const std::string& name, uint32_t index,
			const std::unordered_map<std::string, uint32_t>& loaded, std::unordered_map<uint32_t, std::string>& out,
			uint32_t& weapons)
		{
			const std::wstring path = session_files::game_dir() + L"Data\\" + session_files::wide(name);
			const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
			{
				log(std::format("EditorIDs: cannot open Data\\{}", name), shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			LARGE_INTEGER size = {};
			GetFileSizeEx(h, &size);
			const uint64_t end = static_cast<uint64_t>(size.QuadPart);

			// The header record's masters, as load indices (0xFF: not loaded).
			std::vector<uint32_t> masters;
			Header tes = {};
			std::vector<uint8_t> buf;
			if (!read_at(h, 0, &tes, sizeof(tes)) || !is(tes.type, "TES4") || tes.size > kMaxRecord)
			{
				CloseHandle(h);
				return;
			}
			buf.resize(tes.size);
			if (tes.size && !read_at(h, sizeof(tes), buf.data(), tes.size)) { CloseHandle(h); return; }
			for (size_t i = 0; i + 6 <= buf.size();)
			{
				const uint32_t len = static_cast<uint32_t>(buf[i + 4] | (buf[i + 5] << 8));
				if (std::memcmp(&buf[i], "MAST", 4) == 0 && i + 6 + len <= buf.size())
				{
					std::string m(reinterpret_cast<const char*>(&buf[i + 6]), len);
					while (!m.empty() && m.back() == '\0') m.pop_back();
					const auto it = loaded.find(lower(m));
					masters.push_back(it == loaded.end() ? 0xFFu : it->second);
				}
				i += 6 + len;
			}

			uint64_t at = sizeof(tes) + tes.size;
			Header g = {};
			while (at + sizeof(g) <= end && read_at(h, at, &g, sizeof(g)) && is(g.type, "GRUP") && g.size >= sizeof(g))
			{
				char label[4];
				std::memcpy(label, &g.flags, 4);
				if (g.form_id == 0 && std::memcmp(label, "WEAP", 4) == 0 && g.size - sizeof(g) <= kMaxGroup)
				{
					buf.resize(g.size - sizeof(g));
					if (!read_at(h, at + sizeof(g), buf.data(), static_cast<DWORD>(buf.size()))) break;
					std::vector<uint8_t> raw;
					for (size_t i = 0; i + sizeof(Header) <= buf.size();)
					{
						Header r;
						std::memcpy(&r, &buf[i], sizeof(r));
						const size_t data = i + sizeof(r);
						if (is(r.type, "GRUP")) { i += sizeof(r); continue; }
						if (data + r.size > buf.size()) break;
						i = data + r.size;
						if (!is(r.type, "WEAP") || (r.flags & kRecordDeleted)) continue;

						std::string edid;
						if (r.flags & kRecordCompressed)
						{
							if (r.size < 4) continue;
							uint32_t full = 0;
							std::memcpy(&full, &buf[data], 4);
							if (!full || full > kMaxRecord) continue;
							raw.resize(full);
							if (!tex_pack::zlib_inflate(&buf[data + 4], r.size - 4, raw.data(), raw.size())) continue;
							edid = record_edid(raw.data(), raw.size());
						}
						else edid = record_edid(&buf[data], r.size);
						if (edid.empty()) continue;

						const uint32_t hi = r.form_id >> 24;
						const uint32_t load = hi < masters.size() ? masters[hi] : index;
						if (load > 0xFE) continue;
						out[(load << 24) | (r.form_id & 0x00FFFFFFu)] = std::move(edid);
						weapons++;
					}
				}
				at += g.size;
			}
			CloseHandle(h);
		}

		bool g_ids_built = false;
		std::unordered_map<uint32_t, std::string> g_ids;

		// The load order is "IX Name" per line, IX hex.
		bool build_ids()
		{
			const std::string list = nvse_link::load_order();
			if (list.empty()) return false;
			std::vector<std::pair<uint32_t, std::string>> files;
			std::unordered_map<std::string, uint32_t> loaded;
			for (size_t at = 0; at < list.size();)
			{
				const size_t end = (std::min)(list.find('\n', at), list.size());
				const std::string line = list.substr(at, end - at);
				at = end + 1;
				if (line.size() < 4 || line[2] != ' ') continue;
				const uint32_t index = static_cast<uint32_t>(std::strtoul(line.substr(0, 2).c_str(), nullptr, 16));
				std::string name = line.substr(3);
				while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
				if (name.empty() || name.front() == '(') continue;
				loaded[lower(name)] = index;
				files.emplace_back(index, std::move(name));
			}
			if (files.empty()) return false;

			uint32_t weapons = 0;
			const DWORD t0 = GetTickCount();
			for (const auto& [index, name] : files) scan_plugin(name, index, loaded, g_ids, weapons);
			log(std::format("EditorIDs: {} weapons named from {} plugin files ({} WEAP records, {} ms)", g_ids.size(),
				files.size(), weapons, GetTickCount() - t0));
			return true;
		}

		// ---- Lens files ----------------------------------------------------------

		using Keys = std::vector<std::pair<std::string, std::string>>;

		struct File
		{
			bool loaded = false;
			bool dirty = false;
			std::wstring path;              // the file read (it is also where Save writes); empty: none
			std::map<int, Keys> sections;   // kBase, then mod masks 0..7
		};
		std::unordered_map<uint32_t, File> g_files;

		// The folders searched, lowest priority first: the folder itself, then
		// every subfolder (at any depth) by its path, case-insensitive.
		std::vector<std::wstring> g_folders;
		bool g_folders_built = false;

		const std::vector<std::wstring>& folders()
		{
			if (g_folders_built) return g_folders;
			g_folders_built = true;
			g_folders.clear();
			const std::wstring base = folder();
			g_folders.push_back(base);
			std::vector<std::wstring> subs;
			std::error_code ec;
			for (std::filesystem::recursive_directory_iterator it(base, std::filesystem::directory_options::skip_permission_denied, ec), end;
				!ec && it != end; it.increment(ec))
			{
				if (it->is_directory(ec)) subs.push_back(it->path().wstring() + L"\\");
			}
			std::sort(subs.begin(), subs.end(), [](const std::wstring& a, const std::wstring& b) {
				return _wcsicmp(a.c_str(), b.c_str()) < 0;
			});
			for (auto& s : subs) g_folders.push_back(std::move(s));
			if (g_folders.size() > 1)
				log(std::format("lens file folders: {} patch folder(s) over {} (the last by name wins)", g_folders.size() - 1,
					session_files::utf8(base)));
			return g_folders;
		}

		std::string_view trim(std::string_view v)
		{
			while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
			while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
			return v;
		}

		bool iequals(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
		}

		// "Lens" -> kBase; "Lens.<slots>" -> the mask; -2 anything else.
		int parse_section(std::string_view name)
		{
			if (iequals(name, "Lens")) return kBase;
			if (name.size() < 6 || !iequals(name.substr(0, 5), "Lens.")) return -2;
			int mask = 0;
			for (const char c : name.substr(5))
			{
				if (c == '1') mask |= 1;
				else if (c == '2') mask |= 2;
				else if (c == '3') mask |= 4;
				else if (c != '0') return -2;
			}
			return mask;
		}

		std::wstring file_name(const std::string& edid)
		{
			std::string safe;
			for (const char c : edid)
				safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
			return session_files::wide(safe) + L".ini";
		}

		// The weapon's file in the base folder (where a new entry is saved).
		std::wstring file_path(const std::string& edid)
		{
			return folder() + file_name(edid);
		}

		// The highest-priority file for the weapon, or empty.
		std::wstring find_file(const std::string& edid)
		{
			const std::wstring name = file_name(edid);
			const auto& list = folders();
			for (auto it = list.rbegin(); it != list.rend(); ++it)
			{
				const std::wstring p = *it + name;
				const DWORD attr = GetFileAttributesW(p.c_str());
				if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return p;
			}
			return {};
		}

		File& file(uint32_t form_id)
		{
			File& f = g_files[form_id];
			if (f.loaded) return f;
			f.loaded = true;
			const std::string* edid = editor_id(form_id);
			if (!edid) { f.loaded = false; return f; }
			std::string text;
			f.path = find_file(*edid);
			if (f.path.empty() || !session_files::read_file(f.path, text, 1u << 20)) return f;
			int section = -2;
			for (size_t at = 0; at < text.size();)
			{
				const size_t end = (std::min)(text.find('\n', at), text.size());
				const std::string_view line = trim(std::string_view(text).substr(at, end - at));
				at = end + 1;
				if (line.empty() || line.front() == ';' || line.front() == '#') continue;
				if (line.front() == '[')
				{
					const size_t close = line.find(']');
					section = parse_section(trim(line.substr(1, close == std::string_view::npos ? line.size() - 1 : close - 1)));
					if (section != -2) f.sections[section];
					continue;
				}
				const size_t eq = line.find('=');
				if (section == -2 || eq == std::string_view::npos) continue;
				f.sections[section].emplace_back(std::string(trim(line.substr(0, eq))), std::string(trim(line.substr(eq + 1))));
			}
			log(std::format("lens file for {}: {} ({} section(s))", *edid, session_files::utf8(f.path), f.sections.size()));
			return f;
		}

		const std::string* find(const Keys& keys, std::string_view key)
		{
			for (const auto& [k, v] : keys)
				if (iequals(k, key)) return &v;
			return nullptr;
		}

		void read_floats(const std::string* v, float* out, int n)
		{
			if (!v) return;
			const char* p = v->c_str();
			for (int i = 0; i < n; i++)
			{
				char* e = nullptr;
				const float f = std::strtof(p, &e);
				if (e == p) return;
				if (std::isfinite(f)) out[i] = f;
				p = e;
				while (*p == ' ' || *p == ',' || *p == '\t') p++;
			}
		}

		constexpr const char* kReticles[] = { "none", "crosshair", "duplex", "dot", "circledot", "mildot", "german",
			"drop", "stadia", "frame", "grid", "dial" };
		static_assert(static_cast<int>(std::size(kReticles)) == kReticleCount);
		constexpr const char* kKinds[] = { "telescopic", "reflex" };
		constexpr const char* kApertures[] = { "circle", "rectangle", "binocular" };
		constexpr const char* kVisionModes[] = { "none", "night", "thermal" };
		constexpr const char* kReticlePlanes[] = { "second", "first" };

		// The index of `v` in `names`, or `fallback`.
		template <size_t N>
		int pick(const std::string* v, const char* const (&names)[N], int fallback)
		{
			if (!v) return fallback;
			for (size_t i = 0; i < N; i++)
				if (iequals(*v, names[i])) return static_cast<int>(i);
			return fallback;
		}

		void apply(const Keys& k, Lens& l)
		{
			read_floats(find(k, "EyepieceCenter"), l.eyepiece, 3);
			read_floats(find(k, "Axis"), l.axis, 3);
			read_floats(find(k, "Up"), l.up, 3);
			read_floats(find(k, "EyepieceRadius"), &l.eyepiece_radius, 1);
			read_floats(find(k, "ObjectiveDistance"), &l.objective_distance, 1);
			read_floats(find(k, "ObjectiveRadius"), &l.objective_radius, 1);
			read_floats(find(k, "EyeRelief"), &l.eye_relief, 1);
			read_floats(find(k, "Magnification"), &l.magnification, 1);
			read_floats(find(k, "FieldStop"), &l.field_stop, 1);
			l.reticle = pick(find(k, "Reticle"), kReticles, l.reticle);
			read_floats(find(k, "ReticleColor"), l.reticle_color, 3);
			read_floats(find(k, "ReticleRadiance"), &l.reticle_radiance, 1);
			read_floats(find(k, "ReticleLineWidth"), &l.reticle_line_width, 1);
			read_floats(find(k, "Tint"), l.tint, 3);
			l.kind = pick(find(k, "Kind"), kKinds, l.kind);
			l.aperture = pick(find(k, "Aperture"), kApertures, l.aperture);
			read_floats(find(k, "ApertureAspect"), &l.aperture_aspect, 1);
			l.vision_mode = pick(find(k, "VisionMode"), kVisionModes, l.vision_mode);
			if (const std::string* vm = find(k, "VisionModes"))
			{
				l.vision_count = 0;
				for (size_t at = 0; at <= vm->size();)
				{
					const size_t end = (std::min)(vm->find(',', at), vm->size());
					const std::string word(trim(std::string_view(*vm).substr(at, end - at)));
					const int mode = pick(&word, kVisionModes, -1);
					bool seen = mode < 0;
					for (int i = 0; i < l.vision_count && !seen; i++) seen = l.vision_list[i] == mode;
					if (!seen && l.vision_count < 3) l.vision_list[l.vision_count++] = static_cast<uint8_t>(mode);
					at = end + 1;
				}
			}
			if (const std::string* hh = find(k, "HighlightHostiles"))
				l.highlight_hostiles = *hh == "1" || iequals(*hh, "true") || iequals(*hh, "yes");
			read_floats(find(k, "MinMagnification"), &l.min_magnification, 1);
			read_floats(find(k, "MaxMagnification"), &l.max_magnification, 1);
			read_floats(find(k, "ReticleAccentColor"), l.reticle_accent_color, 3);
			read_floats(find(k, "ReticleAccentRadiance"), &l.reticle_accent_radiance, 1);
			read_floats(find(k, "EdgeGlowColor"), l.edge_glow_color, 3);
			read_floats(find(k, "EdgeGlowStrength"), &l.edge_glow_strength, 1);
			read_floats(find(k, "EdgeShade"), &l.edge_shade, 1);
			read_floats(find(k, "EdgeTint"), l.edge_tint, 3);
			read_floats(find(k, "NightTint"), l.night_tint, 3);
			read_floats(find(k, "NightGain"), &l.night_gain, 1);
			if (const std::string* nd = find(k, "Node")) strncpy_s(l.node, nd->c_str(), _TRUNCATE);
			l.reticle_plane = pick(find(k, "ReticlePlane"), kReticlePlanes, l.reticle_plane);
			if (const std::string* mc = find(k, "MarkingColor"))
			{
				l.marking_color_set = true;
				read_floats(mc, l.marking_color, 3);
			}
			read_floats(find(k, "MarkingRadiance"), &l.marking_radiance, 1);
			read_floats(find(k, "MarkingOpacity"), &l.marking_opacity, 1);
			read_floats(find(k, "ParallaxDistance"), &l.parallax_m, 1);
			read_floats(find(k, "GlareStrength"), &l.glare, 1);
			read_floats(find(k, "DirtStrength"), &l.dirt, 1);
			if (const std::string* gr = find(k, "Grain"))
				l.grain = !(*gr == "0" || iequals(*gr, "off") || iequals(*gr, "false") || iequals(*gr, "no"));
			if (const std::string* tx = find(k, "ReticleTexture")) strncpy_s(l.reticle_texture, tx->c_str(), _TRUNCATE);
			if (const std::string* md = find(k, "Models"))
			{
				l.models[0] = 0;
				for (size_t at = 0; at <= md->size();)
				{
					const size_t end = (std::min)(md->find(',', at), md->size());
					const std::string one = normalize_model(trim(std::string_view(*md).substr(at, end - at)));
					if (!one.empty()) add_model(l, one);
					at = end + 1;
				}
			}
			if (const std::string* mk = find(k, "Markings"))
			{
				l.markings = 0;
				for (size_t at = 0; at <= mk->size();)
				{
					const size_t end = (std::min)(mk->find(',', at), mk->size());
					const std::string_view word = trim(std::string_view(*mk).substr(at, end - at));
					if (iequals(word, "labels")) l.markings |= 1;
					else if (iequals(word, "rangefinder")) l.markings |= 2;
					at = end + 1;
				}
			}
		}

		std::string num(float v)
		{
			char b[32];
			snprintf(b, sizeof(b), "%.6g", v);
			return b;
		}

		std::string vec(const float* v)
		{
			return num(v[0]) + ", " + num(v[1]) + ", " + num(v[2]);
		}

		Keys to_keys(const Lens& l)
		{
			Keys keys = {
				{ "EyepieceCenter", vec(l.eyepiece) },
				{ "Axis", vec(l.axis) },
				{ "Up", vec(l.up) },
				{ "EyepieceRadius", num(l.eyepiece_radius) },
				{ "ObjectiveDistance", num(l.objective_distance) },
				{ "ObjectiveRadius", num(l.objective_radius) },
				{ "EyeRelief", num(l.eye_relief) },
				{ "Magnification", num(l.magnification) },
				{ "FieldStop", num(l.field_stop) },
				{ "Reticle", kReticles[std::clamp(l.reticle, 0, kReticleCount - 1)] },
				{ "ReticleColor", vec(l.reticle_color) },
				{ "ReticleRadiance", num(l.reticle_radiance) },
				{ "ReticleLineWidth", num(l.reticle_line_width) },
				{ "Tint", vec(l.tint) },
				{ "Kind", kKinds[std::clamp(l.kind, 0, 1)] },
				{ "Aperture", kApertures[std::clamp(l.aperture, 0, 2)] },
				{ "ApertureAspect", num(l.aperture_aspect) },
				{ "HighlightHostiles", l.highlight_hostiles ? "1" : "0" },
				{ "MinMagnification", num(l.min_magnification) },
				{ "MaxMagnification", num(l.max_magnification) },
				{ "ReticleAccentColor", vec(l.reticle_accent_color) },
				{ "ReticleAccentRadiance", num(l.reticle_accent_radiance) },
				{ "EdgeGlowColor", vec(l.edge_glow_color) },
				{ "EdgeGlowStrength", num(l.edge_glow_strength) },
				{ "EdgeShade", num(l.edge_shade) },
				{ "EdgeTint", vec(l.edge_tint) },
				{ "NightTint", vec(l.night_tint) },
				{ "NightGain", num(l.night_gain) },
			};
			if (l.node[0]) keys.emplace_back("Node", l.node);
			if (l.reticle_plane == 1) keys.emplace_back("ReticlePlane", kReticlePlanes[1]);
			if (l.marking_color_set)
			{
				keys.emplace_back("MarkingColor", vec(l.marking_color));
				keys.emplace_back("MarkingRadiance", num(l.marking_radiance));
			}
			if (l.marking_opacity > 0.0f) keys.emplace_back("MarkingOpacity", num(l.marking_opacity));
			if (l.parallax_m >= 0.0f) keys.emplace_back("ParallaxDistance", num(l.parallax_m));
			if (l.glare >= 0.0f) keys.emplace_back("GlareStrength", num(l.glare));
			if (l.dirt >= 0.0f) keys.emplace_back("DirtStrength", num(l.dirt));
			if (!l.grain) keys.emplace_back("Grain", "off");
			if (l.reticle_texture[0]) keys.emplace_back("ReticleTexture", l.reticle_texture);
			if (l.models[0])
			{
				std::string list = l.models;
				for (size_t i = 0; (i = list.find(',', i)) != std::string::npos; i += 2) list.insert(i + 1, " ");
				keys.emplace_back("Models", list);
			}
			if (l.markings & 3)
				keys.emplace_back("Markings", (l.markings & 3) == 3 ? "labels,rangefinder" : (l.markings & 1) ? "labels"
					: "rangefinder");
			// VisionMode is written only when the entry sets one; without it
			// the weapon's own applies.
			if (l.vision_mode >= 0) keys.emplace_back("VisionMode", kVisionModes[std::clamp(l.vision_mode, 0, 2)]);
			if (l.vision_count > 0)
			{
				std::string list;
				for (int i = 0; i < l.vision_count; i++)
					list += std::string(i ? ", " : "") + kVisionModes[std::clamp(static_cast<int>(l.vision_list[i]), 0, 2)];
				keys.emplace_back("VisionModes", list);
			}
			return keys;
		}
	}

	std::string normalize_model(std::string_view path)
	{
		std::string s(trim(path));
		for (char& c : s)
		{
			if (c == '/') c = '\\';
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		while (!s.empty() && s.front() == '\\') s.erase(0, 1);
		for (const char* prefix : { "data\\meshes\\", "meshes\\" })
			if (s.rfind(prefix, 0) == 0) s.erase(0, std::strlen(prefix));
		return s;
	}

	bool lists_model(const Lens& lens, std::string_view model)
	{
		const std::string_view all = lens.models;
		for (size_t at = 0; at <= all.size();)
		{
			const size_t end = (std::min)(all.find(',', at), all.size());
			if (all.substr(at, end - at) == model) return true;
			at = end + 1;
		}
		return false;
	}

	void add_model(Lens& lens, std::string_view model)
	{
		if (model.empty() || lists_model(lens, model)) return;
		std::string all = lens.models;
		if (!all.empty()) all += ',';
		all += model;
		if (all.size() < sizeof(lens.models)) strncpy_s(lens.models, all.c_str(), _TRUNCATE);
	}

	std::wstring source(uint32_t form_id)
	{
		const auto it = g_files.find(form_id);
		return it == g_files.end() ? std::wstring() : it->second.path;
	}

	const std::string* editor_id(uint32_t form_id)
	{
		if (!g_ids_built) g_ids_built = build_ids();
		if (!g_ids_built) return nullptr;
		const auto it = g_ids.find(form_id);
		return it == g_ids.end() ? nullptr : &it->second;
	}

	bool resolve(uint32_t form_id, uint8_t mods, Lens& out, int* section)
	{
		const File& f = file(form_id);
		const auto base = f.sections.find(kBase);
		const auto combo = f.sections.find(mods & 7);
		if (base == f.sections.end() && combo == f.sections.end()) return false;
		out = {};
		if (base != f.sections.end()) apply(base->second, out);
		if (combo != f.sections.end()) apply(combo->second, out);
		if (section) *section = combo != f.sections.end() ? combo->first : kBase;
		return true;
	}

	bool has_any(uint32_t form_id)
	{
		return !file(form_id).sections.empty();
	}

	bool set(uint32_t form_id, int section, const Lens& lens)
	{
		if (!editor_id(form_id)) return false;
		File& f = file(form_id);
		f.sections[section] = to_keys(lens);
		f.dirty = true;
		return true;
	}

	bool save(uint32_t form_id, std::string& why)
	{
		const std::string* edid = editor_id(form_id);
		if (!edid) { why = "the weapon has no EditorID"; return false; }
		File& f = file(form_id);
		if (f.sections.empty()) { why = "nothing to save"; return false; }

		std::string text = std::format("; New Vegas RTX scope lens for {}.\r\n; First-person weapon node space: +X toward "
			"the muzzle, +Y up, game units.\r\n", *edid);
		for (const auto& [section, keys] : f.sections)
		{
			text += "\r\n" + section_name(section) + "\r\n";
			for (const auto& [k, v] : keys) text += k + " = " + v + "\r\n";
		}

		// Into the file the entry came from (a patch folder's own), else the
		// base folder.
		const std::wstring path = f.path.empty() ? file_path(*edid) : f.path;
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
		const std::wstring tmp = path + L".tmp";
		{
			std::ofstream out(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			if (!out) { why = "cannot write " + session_files::utf8(tmp); return false; }
		}
		if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			why = std::format("cannot replace {} (error {})", session_files::utf8(path), GetLastError());
			DeleteFileW(tmp.c_str());
			return false;
		}
		f.dirty = false;
		f.path = path;
		why = session_files::utf8(path);
		log("lens saved: " + why, shared::common::LOG_TYPE::LOG_TYPE_STATUS);
		return true;
	}

	bool dirty(uint32_t form_id)
	{
		const auto it = g_files.find(form_id);
		return it != g_files.end() && it->second.dirty;
	}

	void reload()
	{
		g_files.clear();
		g_folders_built = false;
		log("lens files are read again on use");
	}

	std::wstring folder()
	{
		return session_files::game_dir() + L"Data\\Config\\NewVegasRTX\\Scopes\\";
	}

	std::string section_name(int section)
	{
		if (section == kBase) return "[Lens]";
		std::string s = "[Lens.";
		if (!section) s += '0';
		if (section & 1) s += '1';
		if (section & 2) s += '2';
		if (section & 4) s += '3';
		return s + "]";
	}
}
