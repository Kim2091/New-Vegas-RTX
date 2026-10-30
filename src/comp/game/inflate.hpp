#pragma once
#include <cstddef>
#include <cstdint>

// Minimal zlib (RFC 1950) / raw DEFLATE (RFC 1951) decoder for reading
// compressed files out of Bethesda BSA v104 archives. It handles stored,
// fixed-Huffman and dynamic-Huffman blocks and decodes into a caller-sized
// buffer. BSA records carry the uncompressed size, so the output buffer is
// always exact and no streaming is needed.

namespace comp::game::tex_pack
{
	// Decodes a zlib stream (2-byte header, DEFLATE data, Adler-32 trailer).
	// Returns true when the stream decoded completely into exactly
	// `dst_size` bytes (the trailer's Adler-32 is verified when present).
	bool zlib_inflate(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size);

	// Decodes raw DEFLATE data. `out_len` receives the number of bytes
	// written; returns false on malformed input or output overflow.
	bool raw_inflate(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size,
		size_t& out_len, size_t* consumed = nullptr);
}
