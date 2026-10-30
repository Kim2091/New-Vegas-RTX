#include "std_include.hpp"
#include "inflate.hpp"

/*
 * DEFLATE decoder (RFC 1951) with a zlib (RFC 1950) wrapper.
 *
 * Canonical-Huffman decoding with per-length symbol counts (the classic
 * "count / offset / symbol" table layout): a code is decoded bit by bit,
 * which is plenty for the few MB of height maps a cell load pulls out of a
 * BSA, and keeps the decoder small enough to audit.
 */

namespace comp::game::tex_pack
{
	namespace
	{
		constexpr int kMaxBits = 15;

		struct Huffman
		{
			uint16_t count[kMaxBits + 1] = {};
			uint16_t symbol[288] = {};
		};

		struct Reader
		{
			const uint8_t* src;
			size_t size;
			size_t pos = 0;
			uint32_t bitbuf = 0;
			int bitcnt = 0;
			bool overrun = false;

			// Next `need` bits, LSB first.
			uint32_t bits(int need)
			{
				uint32_t val = bitbuf;
				while (bitcnt < need)
				{
					if (pos >= size) { overrun = true; return 0; }
					val |= static_cast<uint32_t>(src[pos++]) << bitcnt;
					bitcnt += 8;
				}
				bitbuf = val >> need;
				bitcnt -= need;
				return val & ((1u << need) - 1u);
			}
		};

		// Builds a decoding table from code lengths. Returns < 0 for an
		// over-subscribed set, 0 for complete, > 0 for incomplete (allowed only
		// for single-code distance tables, which the caller tolerates).
		int build(Huffman& h, const uint8_t* lengths, int n)
		{
			for (auto& c : h.count) c = 0;
			for (int s = 0; s < n; s++) h.count[lengths[s]]++;
			if (h.count[0] == n) return 0;

			int left = 1;
			for (int len = 1; len <= kMaxBits; len++)
			{
				left <<= 1;
				left -= h.count[len];
				if (left < 0) return left;
			}

			uint16_t offs[kMaxBits + 1];
			offs[1] = 0;
			for (int len = 1; len < kMaxBits; len++) offs[len + 1] = static_cast<uint16_t>(offs[len] + h.count[len]);
			for (int s = 0; s < n; s++)
				if (lengths[s] != 0) h.symbol[offs[lengths[s]]++] = static_cast<uint16_t>(s);
			return left;
		}

		int decode(Reader& r, const Huffman& h)
		{
			int code = 0, first = 0, index = 0;
			for (int len = 1; len <= kMaxBits; len++)
			{
				code |= static_cast<int>(r.bits(1));
				if (r.overrun) return -1;
				const int count = h.count[len];
				if (code - count < first) return h.symbol[index + (code - first)];
				index += count;
				first += count;
				first <<= 1;
				code <<= 1;
			}
			return -1;
		}

		constexpr uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
			35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
		constexpr uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
			3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
		constexpr uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
			257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
		constexpr uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
			7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

		bool codes(Reader& r, const Huffman& lit, const Huffman& dist, uint8_t* dst, size_t dst_size, size_t& out)
		{
			for (;;)
			{
				int sym = decode(r, lit);
				if (sym < 0) return false;
				if (sym < 256)
				{
					if (out >= dst_size) return false;
					dst[out++] = static_cast<uint8_t>(sym);
				}
				else if (sym == 256)
				{
					return true;
				}
				else
				{
					sym -= 257;
					if (sym >= 29) return false;
					const size_t len = kLenBase[sym] + r.bits(kLenExtra[sym]);
					const int ds = decode(r, dist);
					if (ds < 0 || ds >= 30 || r.overrun) return false;
					const size_t d = kDistBase[ds] + r.bits(kDistExtra[ds]);
					if (r.overrun || d > out || len > dst_size - out) return false;
					// Byte-wise: the copy may overlap its own output (d < len).
					const uint8_t* from = dst + out - d;
					for (size_t i = 0; i < len; i++) dst[out + i] = from[i];
					out += len;
				}
			}
		}

		bool block_fixed(Reader& r, uint8_t* dst, size_t dst_size, size_t& out)
		{
			static Huffman s_lit, s_dist;
			static const bool s_built = [] {
				uint8_t l[288];
				int s = 0;
				for (; s < 144; s++) l[s] = 8;
				for (; s < 256; s++) l[s] = 9;
				for (; s < 280; s++) l[s] = 7;
				for (; s < 288; s++) l[s] = 8;
				build(s_lit, l, 288);
				for (s = 0; s < 30; s++) l[s] = 5;
				build(s_dist, l, 30);
				return true;
			}();
			(void)s_built;
			return codes(r, s_lit, s_dist, dst, dst_size, out);
		}

		bool block_dynamic(Reader& r, uint8_t* dst, size_t dst_size, size_t& out)
		{
			static constexpr uint8_t kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
			const int nlen = static_cast<int>(r.bits(5)) + 257;
			const int ndist = static_cast<int>(r.bits(5)) + 1;
			const int ncode = static_cast<int>(r.bits(4)) + 4;
			if (r.overrun || nlen > 286 || ndist > 30) return false;

			uint8_t lengths[320] = {};
			for (int i = 0; i < ncode; i++) lengths[kOrder[i]] = static_cast<uint8_t>(r.bits(3));
			if (r.overrun) return false;

			Huffman lencode, lit, dist;
			if (build(lencode, lengths, 19) != 0) return false;

			for (auto& l : lengths) l = 0;
			int idx = 0;
			while (idx < nlen + ndist)
			{
				int sym = decode(r, lencode);
				if (sym < 0) return false;
				if (sym < 16)
				{
					lengths[idx++] = static_cast<uint8_t>(sym);
					continue;
				}
				uint8_t val = 0;
				int rep;
				if (sym == 16)
				{
					if (idx == 0) return false;
					val = lengths[idx - 1];
					rep = 3 + static_cast<int>(r.bits(2));
				}
				else if (sym == 17) rep = 3 + static_cast<int>(r.bits(3));
				else rep = 11 + static_cast<int>(r.bits(7));
				if (r.overrun || idx + rep > nlen + ndist) return false;
				while (rep--) lengths[idx++] = val;
			}
			if (lengths[256] == 0) return false;   // no end-of-block code

			const int le = build(lit, lengths, nlen);
			if (le < 0 || (le > 0 && nlen - lit.count[0] != 1)) return false;
			const int de = build(dist, lengths + nlen, ndist);
			if (de < 0 || (de > 0 && ndist - dist.count[0] != 1)) return false;
			return codes(r, lit, dist, dst, dst_size, out);
		}
	}

	bool raw_inflate(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size,
		size_t& out_len, size_t* consumed)
	{
		Reader r{ src, src_size };
		size_t out = 0;
		for (;;)
		{
			const uint32_t last = r.bits(1);
			const uint32_t type = r.bits(2);
			if (r.overrun) return false;
			bool ok = false;
			if (type == 0)
			{
				// Stored: skip to a byte boundary, LEN / NLEN, raw bytes.
				r.bitbuf = 0;
				r.bitcnt = 0;
				if (r.pos + 4 > r.size) return false;
				const uint32_t len = src[r.pos] | (src[r.pos + 1] << 8);
				const uint32_t nlen = src[r.pos + 2] | (src[r.pos + 3] << 8);
				r.pos += 4;
				if (len != (~nlen & 0xFFFFu)) return false;
				if (r.pos + len > r.size || len > dst_size - out) return false;
				std::memcpy(dst + out, src + r.pos, len);
				r.pos += len;
				out += len;
				ok = true;
			}
			else if (type == 1) ok = block_fixed(r, dst, dst_size, out);
			else if (type == 2) ok = block_dynamic(r, dst, dst_size, out);
			if (!ok || r.overrun) return false;
			if (last) break;
		}
		out_len = out;
		if (consumed) *consumed = r.pos;   // whole bytes; the final partial byte counts as consumed
		return true;
	}

	bool zlib_inflate(const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size)
	{
		if (src_size < 2) return false;
		const uint8_t cmf = src[0], flg = src[1];
		if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;

		size_t out = 0, used = 0;
		if (!raw_inflate(src + 2, src_size - 2, dst, dst_size, out, &used)) return false;
		if (out != dst_size) return false;

		// Adler-32 trailer, big-endian. Some writers truncate it; only a
		// present-but-wrong checksum is an error.
		const size_t tpos = 2 + used;
		if (tpos + 4 <= src_size)
		{
			uint32_t a = 1, b = 0;
			for (size_t i = 0; i < out;)
			{
				// 5552 is the longest run that cannot overflow b before the modulo.
				const size_t end = (std::min)(out, i + 5552);
				for (; i < end; i++) { a += dst[i]; b += a; }
				a %= 65521u;
				b %= 65521u;
			}
			const uint32_t want = (static_cast<uint32_t>(src[tpos]) << 24) | (static_cast<uint32_t>(src[tpos + 1]) << 16) |
				(static_cast<uint32_t>(src[tpos + 2]) << 8) | src[tpos + 3];
			if (((b << 16) | a) != want) return false;
		}
		return true;
	}
}
