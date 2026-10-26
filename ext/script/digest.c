/*
 * Digests for crypto.subtle.digest(): SHA-1, SHA-256, SHA-384 and
 * SHA-512 (FIPS 180-4), whole messages in memory.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdint.h>
#include <string.h>
#include "digest.h"

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		(uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)(v >> 32));
	put32(p + 4, (uint32_t)v);
}

/*
 * The message padded in blocks of bs bytes (64 or 128): the data, 0x80,
 * zeros, its length in bits (big endian, at the end); block()
 * runs on each.
 */
static void blocks(const uint8_t *data, size_t len, size_t bs, void *st,
	void (*block)(void *st, const uint8_t *b))
{
	uint8_t last[256];
	size_t i, n = len / bs * bs, rest = len - n, total;

	for (i = 0; i < n; i += bs)
		block(st, data + i);
	/* the length takes 8 bytes, 16 for 128-byte blocks */
	total = rest + 1 + bs / 8 > bs ? 2 * bs : bs;
	memset(last, 0, total);
	memcpy(last, data + n, rest);
	last[rest] = 0x80;
	put64(last + total - 8, (uint64_t)len << 3);
	for (i = 0; i < total; i += bs)
		block(st, last + i);
}

static void sha1_block(void *st, const uint8_t *b)
{
	uint32_t *h = st, w[80], a, bb, c, d, e, f, k, t;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = be32(b + 4 * i);
	for (; i < 80; i++)
		w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = h[0];
	bb = h[1];
	c = h[2];
	d = h[3];
	e = h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) {
			f = (bb & c) | (~bb & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = bb ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (bb & c) | (bb & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = bb ^ c ^ d;
			k = 0xca62c1d6;
		}
		t = ROL32(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = ROL32(bb, 30);
		bb = a;
		a = t;
	}
	h[0] += a;
	h[1] += bb;
	h[2] += c;
	h[3] += d;
	h[4] += e;
}

static const uint32_t k256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
	0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
	0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
	0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
	0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
	0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
	0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_block(void *st, const uint8_t *b)
{
	uint32_t *h = st, w[64], s[8], t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = be32(b + 4 * i);
	for (; i < 64; i++)
		w[i] = w[i - 16] + (ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^
			(w[i - 15] >> 3)) + w[i - 7] + (ROR32(w[i - 2], 17) ^
			ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10));
	memcpy(s, h, sizeof(s));
	for (i = 0; i < 64; i++) {
		t1 = s[7] + (ROR32(s[4], 6) ^ ROR32(s[4], 11) ^
			ROR32(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) +
			k256[i] + w[i];
		t2 = (ROR32(s[0], 2) ^ ROR32(s[0], 13) ^ ROR32(s[0], 22)) +
			((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
		memmove(s + 1, s, 7 * sizeof(*s));
		s[4] += t1;
		s[0] = t1 + t2;
	}
	for (i = 0; i < 8; i++)
		h[i] += s[i];
}

static const uint64_t k512[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
	0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
	0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
	0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
	0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
	0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
	0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
	0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
	0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
	0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
	0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
	0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
	0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
	0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
	0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
	0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
	0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
	0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
	0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
	0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
	0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void sha512_block(void *st, const uint8_t *b)
{
	uint64_t *h = st, w[80], s[8], t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = be64(b + 8 * i);
	for (; i < 80; i++)
		w[i] = w[i - 16] + (ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^
			(w[i - 15] >> 7)) + w[i - 7] + (ROR64(w[i - 2], 19) ^
			ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6));
	memcpy(s, h, sizeof(s));
	for (i = 0; i < 80; i++) {
		t1 = s[7] + (ROR64(s[4], 14) ^ ROR64(s[4], 18) ^
			ROR64(s[4], 41)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) +
			k512[i] + w[i];
		t2 = (ROR64(s[0], 28) ^ ROR64(s[0], 34) ^ ROR64(s[0], 39)) +
			((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
		memmove(s + 1, s, 7 * sizeof(*s));
		s[4] += t1;
		s[0] = t1 + t2;
	}
	for (i = 0; i < 8; i++)
		h[i] += s[i];
}

int digest(const char *alg, const void *data, size_t len, uint8_t *out)
{
	int i;

	if (!strcmp(alg, "SHA-1")) {
		uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe,
			0x10325476, 0xc3d2e1f0 };

		blocks(data, len, 64, h, sha1_block);
		for (i = 0; i < 5; i++)
			put32(out + 4 * i, h[i]);
		return 20;
	}
	if (!strcmp(alg, "SHA-256")) {
		uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372,
			0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
			0x5be0cd19 };

		blocks(data, len, 64, h, sha256_block);
		for (i = 0; i < 8; i++)
			put32(out + 4 * i, h[i]);
		return 32;
	}
	if (!strcmp(alg, "SHA-384") || !strcmp(alg, "SHA-512")) {
		static const uint64_t h384[8] = { 0xcbbb9d5dc1059ed8ULL,
			0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
			0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL,
			0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL,
			0x47b5481dbefa4fa4ULL };
		static const uint64_t h512[8] = { 0x6a09e667f3bcc908ULL,
			0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
			0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL,
			0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL,
			0x5be0cd19137e2179ULL };
		int n = alg[4] == '3' ? 6 : 8;
		uint64_t h[8];

		memcpy(h, n == 6 ? h384 : h512, sizeof(h));
		blocks(data, len, 128, h, sha512_block);
		for (i = 0; i < n; i++)
			put64(out + 8 * i, h[i]);
		return 8 * n;
	}
	return -1;
}
