// Standalone test of comp/game/shared_mesh_keys.hpp: the names of a fixed
// sample, which remix-mod-converter's hash.rs and tools/verify.py check too,
// and the table's line format. Runs on the CPU only. Built by build_test.bat;
// not part of the shipped DLLs.

#define XXH_INLINE_ALL
#include "xxhash.h"
#include "comp/game/shared_mesh_keys.hpp"

#include <cstdio>

namespace asset_keys = comp::game::api::asset_keys;

namespace
{
	int g_failed = 0;

	void check(bool ok, const char* what)
	{
		std::printf("%s %s\n", ok ? "  ok  " : "FAILED", what);
		if (!ok) g_failed++;
	}
}

int main()
{
	check(asset_keys::scoped_name(0x68D5753FAD2F21FAull, 0x8FB50DDF575FAA77ull) == 0x21EE810D9894C9FFull, "sample 1");
	check(asset_keys::scoped_name(0x0000000000000001ull, 0xBAC10A99602CFB8Dull) == 0x6631669FDBA0F337ull, "sample 2");
	check(asset_keys::scoped_name(0xFFFFFFFFFFFFFFFFull, 0x0000000000000002ull) == 0xCA08B3BBC5C35B06ull, "sample 3");

	check(asset_keys::colour_name(0x68D5753FAD2F21FAull, 0x8FB50DDF575FAA77ull, 0x0123456789ABCDEFull) == 0x3B697E938BB5ECD6ull, "colour name 1");
	check(asset_keys::colour_name(0x0000000000000001ull, 0xBAC10A99602CFB8Dull, 0x0000000000000001ull) == 0xB0AC3314D3012A1Aull, "colour name 2");

	// (r, g, b, a) bytes 10 20 30 255 and 200 100 50 128, with an unused vertex between them.
	const std::vector<uint32_t> tris = { 2, 0, 2 };
	check(asset_keys::colour_hash({ 0xFF0A141Eu, 0x04010203u, 0x80C86432u }, tris) == 0x96E87F22F4BEBDB3ull, "colour hash 1");
	check(asset_keys::colour_hash({ 0x80C86432u, 0x04010203u, 0xFF0A141Eu }, tris) == 0x9DB83F69D36057C1ull, "colour hash 2");
	check(asset_keys::colour_hash({}, tris) == 0, "no colours");
	check(asset_keys::colour_hash({ 0u, 0u, 0u }, tris) == 0, "all black");
	check(asset_keys::colour_hash({ 0xFFFFFFFFu, 5u, 0xFFFFFFFFu }, tris) == 0, "all white where used");
	check(asset_keys::colour_hash({ 0u, 0u, 0xFFFFFFFFu }, tris) != 0, "black and white");

	uint64_t key = 0, diffuse = 0, colour = 0;
	check(asset_keys::parse_colour_line("68D5753FAD2F21FA 8FB50DDF575FAA77 0123456789ABCDEF", key, diffuse, colour) &&
		key == 0x68D5753FAD2F21FAull && diffuse == 0x8FB50DDF575FAA77ull && colour == 0x0123456789ABCDEFull, "a colour line");
	check(asset_keys::parse_colour_line("68D5753FAD2F21FA 8FB50DDF575FAA77 0123456789ABCDEF\r", key, diffuse, colour), "a CRLF colour line");
	check(!asset_keys::parse_colour_line("68D5753FAD2F21FA 8FB50DDF575FAA77", key, diffuse, colour), "a pair is no colour line");
	check(!asset_keys::parse_scoped_line("68D5753FAD2F21FA 8FB50DDF575FAA77 0123456789ABCDEF", key, diffuse), "a colour line is no pair");
	check(!asset_keys::parse_colour_line("68D5753FAD2F21FA 8FB50DDF575FAA77 0123456789ABCDEX", key, diffuse, colour), "colour not hex");
	check(asset_keys::parse_scoped_line("68D5753FAD2F21FA 8FB50DDF575FAA77", key, diffuse) &&
		key == 0x68D5753FAD2F21FAull && diffuse == 0x8FB50DDF575FAA77ull, "a line");
	check(asset_keys::parse_scoped_line("0000000000000001 BAC10A99602CFB8D\r", key, diffuse) && key == 1, "a CRLF line");
	check(!asset_keys::parse_scoped_line("", key, diffuse), "an empty line");
	check(!asset_keys::parse_scoped_line("68D5753FAD2F21FA", key, diffuse), "one value");
	check(!asset_keys::parse_scoped_line("68D5753FAD2F21FX 8FB50DDF575FAA77", key, diffuse), "not hex");
	check(!asset_keys::parse_scoped_line("68D5753FAD2F21FA 8FB50DDF575FAA777", key, diffuse), "too long");

	std::printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
	return g_failed ? 1 : 0;
}
