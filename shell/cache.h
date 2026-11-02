/*
 * The HTTP cache: GET responses kept in memory (RFC 9111, a private
 * cache), by partition (the top page's origin) and URL.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_CACHE_H
#define SOIE_CACHE_H

#include <stddef.h>
#include <time.h>
#include "net.h"

enum { CACHE_MISS, CACHE_FRESH, CACHE_STALE };

/* key: "partition url". CACHE_FRESH: r is filled (its body and headers
 * replaced), CACHE_STALE: cond holds the If-None-Match and
 * If-Modified-Since lines to ask with; revalidate: never fresh (the
 * user's reload); max: the most private level (NET_*) the request may
 * reach, an entry from further is a miss. */
int cache_get(const char *key, int revalidate, int max, struct response *r,
	char *cond, size_t size);
/* Keep r (asked at t, before the request went) if it may be kept. */
void cache_put(const char *key, const struct response *r, time_t t);
/* A 304 in r for key: the entry is fresh again with r's headers, r is
 * filled from it; -1 when the entry is gone. */
int cache_revalidated(const char *key, struct response *r, time_t t);
/* A POST or the like went to url: its entries, in all partitions, go */
void cache_drop(const char *url);
/* Hits, stale answers revalidated (304), misses, bytes kept */
void cache_stats(long *hits, long *revalidated, long *misses,
	size_t *bytes);

#endif
