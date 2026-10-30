#include "common.hpp"
#include "path_rules.hpp"
#include "rtti.hpp"

namespace fnvr::path_rules
{
	namespace
	{
		bool glob(const char* p, const char* s)
		{
			const char* star = nullptr;
			const char* back = nullptr;
			while (*s)
			{
				if (*p == '*') { star = p++; back = s; continue; }
				if (*p == '?' || *p == *s) { p++; s++; continue; }
				if (!star) return false;
				p = star + 1;
				s = ++back;
			}
			while (*p == '*') p++;
			return !*p;
		}
	}

	std::string normalise(std::string s)
	{
		for (auto& c : s)
		{
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			else if (c == '/') c = '\\';
		}
		return s;
	}

	const std::string* match(const std::vector<std::string>& rules, const std::string& path)
	{
		for (const auto& r : rules)
			if (glob(r.c_str(), path.c_str())) return &r;
		return nullptr;
	}

	std::vector<std::string> split(const std::string& list)
	{
		std::vector<std::string> out;
		size_t at = 0;
		while (at <= list.size())
		{
			const size_t end = list.find(';', at);
			std::string item = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
			while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
			while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
			if (!item.empty()) out.push_back(normalise(item));
			if (end == std::string::npos) break;
			at = end + 1;
		}
		return out;
	}

	std::string join(const std::vector<std::string>& rules)
	{
		std::string s;
		for (const auto& r : rules) s += (s.empty() ? "" : "; ") + r;
		return s;
	}

	std::string read_ini(const wchar_t* section, const wchar_t* key, const wchar_t* def)
	{
		const std::wstring ini = log::dir() + L"fnv_remix_nvse.ini";
		std::wstring w(4096, L'\0');
		const DWORD n = GetPrivateProfileStringW(section, key, def, w.data(), static_cast<DWORD>(w.size()), ini.c_str());
		std::string s;
		for (DWORD i = 0; i < n; i++) s.push_back(w[i] < 0x80 ? static_cast<char>(w[i]) : '?');
		return s;
	}

	bool read_cstr(uintptr_t p, std::string& out)
	{
		out.clear();
		if (!guard::plausible(p & ~3u) || p < 0x10000) return false;
		char buf[64];
		for (uintptr_t at = p; out.size() < 260; at += sizeof(buf))
		{
			// Up to the page end per read, so a string at a page's tail reads.
			const size_t room = static_cast<size_t>(0x1000 - (at & 0xFFF));
			const size_t n = (std::min)(sizeof(buf), room);
			if (!guard::read(at, buf, n)) return false;
			for (size_t k = 0; k < n; k++)
			{
				if (!buf[k]) return !out.empty();
				if (static_cast<unsigned char>(buf[k]) < 0x20) return false;
				out.push_back(buf[k]);
			}
			if (n < sizeof(buf)) at -= sizeof(buf) - n;
		}
		return false;
	}

	namespace
	{
		bool nif_path(uintptr_t str, std::string& out)
		{
			if (!read_cstr(str, out)) return false;
			out = normalise(out);
			if (out.size() < 4 || out.compare(out.size() - 4, 4, ".nif") != 0) return false;
			if (out.rfind("meshes\\", 0) != 0) out = "meshes\\" + out;
			return true;
		}
	}

	bool model_path(uint8_t base_type, uintptr_t base, std::string& out)
	{
		uint32_t str = 0;
		return base_type == engine::kFormType_STAT && guard::plausible(base) &&
			guard::get(base + engine::STAT_modelPath, str) && nif_path(str, out);
	}

	bool form_model_path(uintptr_t base, std::string& out)
	{
		const int32_t at = rtti::base_offset(base, ".?AVTESModel@@");
		uint32_t str = 0;
		return at >= 0 && guard::get(base + static_cast<uint32_t>(at) + engine::MODEL_path, str) && nif_path(str, out);
	}
}
