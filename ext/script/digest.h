/*
 * Digests for crypto.subtle.digest().
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

#endif
