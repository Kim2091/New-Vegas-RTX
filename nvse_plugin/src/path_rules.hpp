#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Path rules of fnv_remix_nvse.ini ([Decals]): engine file paths
// lower-cased with '\' separators, rules with '*' (any run) and '?' (one
// character) wildcards, ';' between rules. Main thread only.

namespace fnvr::path_rules
{
	std::string normalise(std::string s);

	// The first rule that matches the (normalised) path, or null.
	const std::string* match(const std::vector<std::string>& rules, const std::string& path);

	// A ';' list, each rule trimmed and normalised; empty items dropped.
	std::vector<std::string> split(const std::string& list);

	// The rules joined with "; ", for the log.
	std::string join(const std::vector<std::string>& rules);

	// A string value of fnv_remix_nvse.ini (ASCII; other characters read '?').
	std::string read_ini(const wchar_t* section, const wchar_t* key, const wchar_t* def);

	// A C string from engine memory: printable characters up to a NUL.
	bool read_cstr(uintptr_t p, std::string& out);

	// A STAT's model path, normalised, as "meshes\...". False for any other
	// base form type or an unreadable path.
	bool model_path(uint8_t base_type, uintptr_t base, std::string& out);

	// Any base form's model path, normalised, as "meshes\...": its TESModel
	// component, found through the form's MSVC RTTI. False when the form has
	// no TESModel or the path does not read.
	bool form_model_path(uintptr_t base, std::string& out);
}
