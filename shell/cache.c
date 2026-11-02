/*
 * The HTTP cache, a private one (RFC 9111): what the pages load comes
 * back from memory while it is fresh (Cache-Control max-age, Expires,
 * or a tenth of its age since Last-Modified, a day at most), then is
 * asked again with its validators (ETag, Last-Modified): a 304 costs a
 * header, not the body. GET only, status 200, never with a Set-Cookie,
 * no-store, a Vary on what changes between requests, a body larger
 * than a quarter of the cache.
 *
 * Partitioned by the top page's origin, as browsers now do: a site
 * cannot tell from the timing of its requests what another site
 * loaded. Memory only, RAM/64 between 8 and 64 MB, the least recently
 * used entries go first; a private window's requests do not use it
 * (net.c).
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "cache.h"
#include "debug.h"

struct entry {
	char *key, *body, *headers;
	size_t len;
	char type[128], charset[64];
	char *etag, *lastmod;	/* the validators, or NULL */
	time_t expires;		/* fresh until */
	int level;		/* NET_*: the address it came from */
	unsigned long used;	/* LRU stamp */
	struct entry *next;
};

static struct entry *entries;
static size_t bytes, budget;
static unsigned long stamp;
static long hits, revalidated, misses;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static size_t entry_size(const struct entry *e)
{
	return e->len + strlen(e->headers) + strlen(e->key) + sizeof(*e);
}

static void entry_free(struct entry *e)
{
	free(e->key);
	free(e->body);
	free(e->headers);
	free(e->etag);
	free(e->lastmod);
	free(e);
}

static size_t cache_budget(void)
{
	long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
	unsigned long long ram = pages > 0 && size > 0 ?
		(unsigned long long)pages * (unsigned long long)size : 0;
	size_t b = (size_t)(ram / 64 > (64u << 20) ? 64u << 20 : ram / 64);

	return b < (8u << 20) ? 8u << 20 : b;
}

/* A header's value in a "name: value\r\n" block (names lower case),
 * its length in *n; NULL when absent */
static const char *header(const char *h, const char *name, size_t *n)
{
	size_t k = strlen(name);
	const char *p;

	for (p = h; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		if (!strncmp(p, name, k) && p[k] == ':') {
			p += k + 1;
			p += strspn(p, " \t");
			*n = strcspn(p, "\r\n");
			while (*n && (p[*n - 1] == ' ' || p[*n - 1] == '\t'))
				(*n)--;
			return p;
		}
	}
	return NULL;
}

static char *header_dup(const char *h, const char *name)
{
	size_t n;
	const char *v = header(h, name, &n);

	return v && n ? strndup(v, n) : NULL;
}

/* Is token (lower case) in a comma list, ignoring parameters: its
 * "=value" in *arg (or NULL) */
static int has_directive(const char *v, size_t n, const char *tok,
	const char **arg)
{
	size_t k = strlen(tok), i = 0;

	while (i < n) {
		size_t s, e;

		i += strspn(v + i, " \t,");
		for (s = i; i < n && v[i] != ','; i++)
			;
		e = s;
		while (e < i && (isalnum((unsigned char)v[e]) || v[e] == '-'))
			e++;
		if (e - s == k && !strncasecmp(v + s, tok, k)) {
			if (arg)
				*arg = e < i && v[e] == '=' ? v + e + 1 : NULL;
			return 1;
		}
	}
	return 0;
}

/* An HTTP date (IMF-fixdate, RFC 850, asctime), -1 when it is not one */
static time_t http_date(const char *v, size_t n)
{
	static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
	long num[6];
	int nn = 0, mon = -1, before = 0, y, d, m, i;
	size_t p = 0;
	long days;

	while (p < n && nn < 6) {
		if (isdigit((unsigned char)v[p])) {
			num[nn++] = strtol(v + p, NULL, 10);
			while (p < n && isdigit((unsigned char)v[p]))
				p++;
		} else if (isalpha((unsigned char)v[p])) {
			size_t s = p;

			while (p < n && isalpha((unsigned char)v[p]))
				p++;
			for (i = 0; p - s == 3 && i < 12; i++)
				if (!strncasecmp(v + s, months + i * 3, 3)) {
					mon = i;
					before = nn == 0;
				}
		} else {
			p++;
		}
	}
	if (mon < 0 || nn != 5)
		return -1;
	if (before) {		/* asctime: Nov  6 08:49:37 1994 */
		d = (int)num[0];
		y = (int)num[4];
		memmove(num + 2, num + 1, 3 * sizeof(num[0]));
	} else {		/* 06 Nov 1994 08:49:37, 06-Nov-94 ... */
		d = (int)num[0];
		y = (int)num[1];
	}
	if (y < 100)
		y += y < 70 ? 2000 : 1900;
	if (d < 1 || d > 31 || num[2] > 23 || num[3] > 59 || num[4] > 60)
		return -1;
	/* days from 1970-01-01, civil calendar */
	m = mon + 1;
	y -= m <= 2;
	{
		long era = (y >= 0 ? y : y - 399) / 400;
		long yoe = y - era * 400;
		long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
		long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

		days = era * 146097 + doe - 719468;
	}
	return (time_t)(days * 86400 + num[2] * 3600 + num[3] * 60 + num[4]);
}

static time_t header_date(const char *h, const char *name)
{
	size_t n;
	const char *v = header(h, name, &n);

	return v ? http_date(v, n) : -1;
}

/*
 * How long a response stays fresh, from its headers (first those of
 * h, then of old for what h lacks: a 304's update a stored entry), and
 * when it was asked (t) and arrived (now); -1: not to be kept at all.
 */
static long lifetime(const char *h, const char *old, time_t t, time_t now)
{
	const char *cc, *arg, *a, *src;
	size_t ccn = 0, n;
	time_t date, exp, lm;
	long life = 0, age = 0;

	if (!(cc = header(h, "cache-control", &ccn)) && old)
		cc = header(old, "cache-control", &ccn);
	if (cc && has_directive(cc, ccn, "no-store", NULL))
		return -1;
	if ((date = header_date(h, "date")) < 0 &&
	    (!old || (date = header_date(old, "date")) < 0))
		date = now;
	src = header(h, "expires", &n) ? h : old && header(old, "expires",
		&n) ? old : NULL;
	if (cc && has_directive(cc, ccn, "no-cache", NULL)) {
		life = 0;
	} else if (cc && has_directive(cc, ccn, "max-age", &arg) && arg) {
		life = strtol(arg + (*arg == '"'), NULL, 10);
	} else if (src) {
		exp = header_date(src, "expires");
		life = exp < 0 ? 0 : (long)(exp - date);
	} else if ((lm = header_date(h, "last-modified")) >= 0 ||
		   (old && (lm = header_date(old, "last-modified")) >= 0)) {
		/* the heuristic: a tenth of its age, a day at most */
		life = date > lm ? (long)(date - lm) / 10 : 0;
		if (life > 86400)
			life = 86400;
	}
	if ((a = header(h, "age", &n)))
		age = strtol(a, NULL, 10);
	if (t > date && t - date > age)
		age = (long)(t - date);	/* apparent age */
	return life > age ? life - age : 0;
}

/* Only Vary on what never changes between our requests */
static int vary_ok(const char *h)
{
	size_t n, i = 0;
	const char *v = header(h, "vary", &n);

	while (v && i < n) {
		size_t s, e;

		i += strspn(v + i, " \t,");
		for (s = i; i < n && v[i] != ','; i++)
			;
		for (e = i; e > s && (v[e - 1] == ' ' || v[e - 1] == '\t');)
			e--;
		if (e > s && !(e - s == 15 &&
		    !strncasecmp(v + s, "accept-encoding", 15)) &&
		    !(e - s == 10 && !strncasecmp(v + s, "user-agent", 10)) &&
		    !(e - s == 15 && !strncasecmp(v + s, "accept-language", 15)))
			return 0;
	}
	return 1;
}

static struct entry **find(const char *key)
{
	struct entry **p;

	for (p = &entries; *p; p = &(*p)->next)
		if (!strcmp((*p)->key, key))
			return p;
	return p;
}

static void unlink_entry(struct entry **p)
{
	struct entry *e = *p;

	*p = e->next;
	bytes -= entry_size(e);
	entry_free(e);
}

/* r gets a copy of e */
static void fill(struct response *r, struct entry *e)
{
	free(r->body);
	free(r->headers);
	r->status = 200;
	r->body = malloc(e->len + 1);
	r->headers = strdup(e->headers);
	if (!r->body || !r->headers)
		abort();
	memcpy(r->body, e->body, e->len);
	r->body[e->len] = '\0';
	r->len = r->cap = e->len;
	memcpy(r->type, e->type, sizeof(r->type));
	memcpy(r->charset, e->charset, sizeof(r->charset));
	r->auth = 0;
	r->level = e->level;
	e->used = ++stamp;
}

int cache_get(const char *key, int revalidate, int max, struct response *r,
	char *cond, size_t size)
{
	struct entry **p, *e;
	int ret = CACHE_MISS;

	*cond = '\0';
	pthread_mutex_lock(&lock);
	p = find(key);
	/* not for a page that may not reach where it came from */
	if ((e = *p) && e->level <= max) {
		if (!revalidate && time(NULL) < e->expires) {
			fill(r, e);
			hits++;
			ret = CACHE_FRESH;
		} else if (e->etag || e->lastmod) {
			snprintf(cond, size, "%s%s%s%s%s%s",
				e->etag ? "If-None-Match: " : "",
				e->etag ? e->etag : "", e->etag ? "\r\n" : "",
				e->lastmod ? "If-Modified-Since: " : "",
				e->lastmod ? e->lastmod : "",
				e->lastmod ? "\r\n" : "");
			ret = CACHE_STALE;
		} else {
			unlink_entry(p);
		}
	}
	if (ret == CACHE_MISS)
		misses++;
	pthread_mutex_unlock(&lock);
	if (ret != CACHE_MISS)
		debug(2, "cache %s: %s", ret == CACHE_FRESH ? "hit" : "stale",
			strchr(key, ' ') + 1);
	return ret;
}

void cache_put(const char *key, const struct response *r, time_t t)
{
	struct entry *e, **p, **old;
	size_t n;
	long life;
	time_t now = time(NULL);

	if (!budget)
		budget = cache_budget();
	if (r->status != 200 || !r->headers || r->auth ||
	    r->len > budget / 4 || header(r->headers, "set-cookie", &n) ||
	    !vary_ok(r->headers) ||
	    (life = lifetime(r->headers, NULL, t, now)) < 0)
		return;
	if (!(e = calloc(1, sizeof(*e))))
		abort();
	e->etag = header_dup(r->headers, "etag");
	e->lastmod = header_dup(r->headers, "last-modified");
	if (!life && !e->etag && !e->lastmod) {
		entry_free(e);	/* neither fresh nor to be asked again */
		return;
	}
	e->key = strdup(key);
	e->body = malloc(r->len ? r->len : 1);
	e->headers = strdup(r->headers);
	if (!e->key || !e->body || !e->headers)
		abort();
	if (r->len)
		memcpy(e->body, r->body, r->len);
	e->len = r->len;
	memcpy(e->type, r->type, sizeof(e->type));
	memcpy(e->charset, r->charset, sizeof(e->charset));
	e->level = r->level;
	e->expires = now + life;
	pthread_mutex_lock(&lock);
	if (*(p = find(key)))
		unlink_entry(p);
	/* room: the least recently used go */
	while (entries && bytes + entry_size(e) > budget) {
		struct entry **q;

		old = &entries;
		for (q = &entries; *q; q = &(*q)->next)
			if ((*q)->used < (*old)->used)
				old = q;
		unlink_entry(old);
	}
	e->used = ++stamp;
	e->next = entries;
	entries = e;
	bytes += entry_size(e);
	pthread_mutex_unlock(&lock);
	debug(2, "cache keeps %zu bytes for %ld s: %s", r->len, life,
		strchr(key, ' ') + 1);
}

int cache_revalidated(const char *key, struct response *r, time_t t)
{
	struct entry *e;
	long life;
	char *v;

	pthread_mutex_lock(&lock);
	if (!(e = *find(key))) {
		pthread_mutex_unlock(&lock);
		return -1;
	}
	life = r->headers ? lifetime(r->headers, e->headers, t, time(NULL)) :
		0;
	e->expires = time(NULL) + (life > 0 ? life : 0);
	if (r->headers && (v = header_dup(r->headers, "etag"))) {
		free(e->etag);
		e->etag = v;
	}
	if (r->headers && (v = header_dup(r->headers, "last-modified"))) {
		free(e->lastmod);
		e->lastmod = v;
	}
	e->level = r->level;
	fill(r, e);
	revalidated++;
	pthread_mutex_unlock(&lock);
	debug(2, "cache revalidated (304): %s", strchr(key, ' ') + 1);
	return 0;
}

void cache_drop(const char *url)
{
	struct entry **p;
	size_t n = strlen(url);

	pthread_mutex_lock(&lock);
	for (p = &entries; *p; ) {
		const char *u = strchr((*p)->key, ' ');

		if (u && strlen(u + 1) == n && !strcmp(u + 1, url))
			unlink_entry(p);
		else
			p = &(*p)->next;
	}
	pthread_mutex_unlock(&lock);
}

void cache_stats(long *h, long *rv, long *m, size_t *b)
{
	pthread_mutex_lock(&lock);
	*h = hits;
	*rv = revalidated;
	*m = misses;
	*b = bytes;
	pthread_mutex_unlock(&lock);
}
