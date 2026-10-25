/*
 * Content blocking by host name. The list is $SOIE_BLOCK, else
 * $XDG_CONFIG_HOME/soie/block.txt, else ~/.config/soie/block.txt: one
 * host per line, or hosts file lines ("0.0.0.0 ads.example.com"), '#'
 * starts a comment. A listed host blocks its subdomains too. Only the
 * resources of a page are blocked, never the page one asked for.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <soie/ext.h>

static char **set;		/* open addressing, power of two */
static size_t cap, count;

static unsigned long hash(const char *s, size_t len)
{
	unsigned long h = 5381;

	while (len--)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static int lookup(const char *s, size_t len)
{
	size_t i;

	if (!cap)
		return 0;
	for (i = hash(s, len) & (cap - 1); set[i]; i = (i + 1) & (cap - 1))
		if (strlen(set[i]) == len && !memcmp(set[i], s, len))
			return 1;
	return 0;
}

static void insert(const char *s, size_t len)
{
	char **old = set;
	size_t oldcap = cap, i;

	if (!len || lookup(s, len))
		return;
	if ((count + 1) * 2 > cap) {
		cap = cap ? cap * 2 : 1024;
		set = calloc(cap, sizeof(*set));
		if (!set)
			abort();
		for (i = 0; i < oldcap; i++)
			if (old[i]) {
				size_t j = hash(old[i], strlen(old[i])) &
					(cap - 1);

				while (set[j])
					j = (j + 1) & (cap - 1);
				set[j] = old[i];
			}
		free(old);
	}
	for (i = hash(s, len) & (cap - 1); set[i]; i = (i + 1) & (cap - 1))
		;
	set[i] = malloc(len + 1);
	if (!set[i])
		abort();
	memcpy(set[i], s, len);
	set[i][len] = 0;
	count++;
}

static void load(FILE *f)
{
	char line[1024], *p, *w, *last;
	size_t n;

	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "#\r\n")] = 0;
		last = NULL;
		for (p = line; *p; ) {
			while (isspace((unsigned char)*p))
				p++;
			for (w = p; *p && !isspace((unsigned char)*p); p++)
				*p = (char)tolower((unsigned char)*p);
			if (p > w)
				last = w;
			if (*p)
				*p++ = 0;
		}
		/* the last word: "host" or "0.0.0.0 host" */
		if (last && strcmp(last, "localhost") && strchr(last, '.') &&
		    strspn(last, "0123456789.:") != strlen(last)) {
			n = strlen(last);
			insert(last, n);
		}
	}
}

static int filter(void *data, const soie_request *req)
{
	const char *h, *e, *p;

	(void)data;
	if (!req->first_party || !count || !(h = strstr(req->url, "://")))
		return 0;
	h += 3;
	e = h + strcspn(h, "/?#:");
	if ((p = memchr(h, '@', (size_t)(e - h))))
		h = p + 1;
	if (e > h && e[-1] == '.')
		e--;		/* "ads.example.com." is ads.example.com */
	/* the host, then each parent domain */
	for (p = h; p < e; ) {
		char low[256];
		size_t n = (size_t)(e - p), i;

		if (n >= sizeof(low))
			return 0;
		for (i = 0; i < n; i++)
			low[i] = (char)tolower((unsigned char)p[i]);
		if (lookup(low, n))
			return 1;
		p = memchr(p, '.', n);
		if (!p)
			break;
		p++;
	}
	return 0;
}

static struct soie_ext_net hooks = {
	{ SOIE_EXT_ABI, sizeof(hooks), SOIE_EXT_NET, "block", NULL },
	filter, NULL
};

int soie_ext_init(void)
{
	const char *env = getenv("SOIE_BLOCK"), *base;
	char path[4096];
	FILE *f;

	if (env) {
		snprintf(path, sizeof(path), "%s", env);
	} else if ((base = getenv("XDG_CONFIG_HOME")) && *base) {
		snprintf(path, sizeof(path), "%s/soie/block.txt", base);
	} else if ((base = getenv("HOME"))) {
		snprintf(path, sizeof(path), "%s/.config/soie/block.txt", base);
	} else {
		return -1;
	}
	f = fopen(path, "r");
	if (!f)
		return -1;	/* no list, no hooks */
	load(f);
	fclose(f);
	return soie_ext_register(&hooks.base);
}
