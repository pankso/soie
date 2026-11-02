/*
 * Digests, HMAC and PBKDF2 for crypto.subtle.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_DIGEST_H
#define SOIE_DIGEST_H

#include <stddef.h>
#include <stdint.h>

/* The digest of data with alg ("SHA-1", "SHA-256", "SHA-384",
 * "SHA-512") into out (64 bytes room): its length, -1 for another
 * algorithm. SHA-512 pads to 128-byte blocks and a 128-bit length; the
 * high 64 bits are always zero here. */
int digest(const char *alg, const void *data, size_t len, uint8_t *out);

/* HMAC of data with key and alg, into out (64 bytes room): its length,
 * -1 for another algorithm */
int hmac(const char *alg, const void *key, size_t klen, const void *data,
	size_t len, uint8_t *out);

/* PBKDF2 with HMAC-alg, iter iterations, dklen bytes into out: 0, -1
 * for another algorithm or no iteration */
int pbkdf2(const char *alg, const void *pass, size_t plen, const void *salt,
	size_t slen, unsigned long iter, uint8_t *out, size_t dklen);

#endif
