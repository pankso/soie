/*
 * The extension registry and the helpers that walk it.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <dlfcn.h>

#include "internal.h"
#include "soie/ext.h"

#define MAX_EXT 64

static const struct soie_ext *exts[MAX_EXT];
static int nexts;

int soie_ext_register(const struct soie_ext *e)
{
	if (!e || e->abi >> 16 != SOIE_EXT_ABI_MAJOR ||
	    e->size < sizeof(*e) || e->kind < SOIE_EXT_IMAGE ||
	    e->kind > SOIE_EXT_MEDIA || nexts == MAX_EXT)
		return -1;
	exts[nexts++] = e;
	return 0;
}

const struct soie_ext *soie_ext_get(soie_ext_kind kind, int i)
{
	int k;

	for (k = 0; k < nexts; k++)
		if (exts[k]->kind == kind && i-- == 0)
			return exts[k];
	return NULL;
}

/* Modules stay loaded: their hook sets are registered by pointer. */
int soie_ext_load(const char *path)
{
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	int (*init)(void);

	if (!h)
		return -1;
	*(void **)&init = dlsym(h, "soie_ext_init");
	if (!init || init()) {
		dlclose(h);
		return -1;
	}
	return 0;
}

static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

int soie_ext_load_dir(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char **names = NULL, *path;
	size_t n = 0, cap = 0, len, i;
	int loaded = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		len = strlen(de->d_name);
		if (len < 4 || strcmp(de->d_name + len - 3, ".so"))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			names = xrealloc(names, cap * sizeof(*names));
		}
		names[n++] = xstrndup(de->d_name, len);
	}
	closedir(d);
	if (n)
		qsort(names, n, sizeof(*names), cmp_names);
	for (i = 0; i < n; i++) {
		path = xmalloc(strlen(dir) + strlen(names[i]) + 2);
		strcpy(path, dir);
		strcat(path, "/");
		strcat(path, names[i]);
		if (!soie_ext_load(path))
			loaded++;
		free(path);
		free(names[i]);
	}
	free(names);
	return loaded;
}

/* Requests */

void soie_response_free(soie_response *res)
{
	free(res->url);
	free(res->headers);
	free(res->body);
	memset(res, 0, sizeof(*res));
}

char *soie_response_header(const soie_response *res, const char *name)
{
	size_t len = strlen(name);
	const char *p = res->headers, *e;

	while (p && *p) {
		e = strchr(p, '\n');
		if (!e)
			e = p + strlen(p);
		if ((size_t)(e - p) > len && p[len] == ':' &&
		    !strncasecmp(p, name, len)) {
			p += len + 1;
			while (*p == ' ' || *p == '\t')
				p++;
			while (e > p && (e[-1] == '\r' || e[-1] == ' '))
				e--;
			return xstrndup(p, (size_t)(e - p));
		}
		p = *e ? e + 1 : e;
	}
	return NULL;
}

/* Images: called by decode_image() */

unsigned char *ext_decode_image(const unsigned char *d, size_t len,
	int *w, int *h)
{
	const struct soie_ext_image *e;
	unsigned char *(*dec)(void *, const unsigned char *, size_t, int *,
		int *);
	unsigned char *px;
	int i;

	for (i = 0; (e = (const void *)soie_ext_get(SOIE_EXT_IMAGE, i));
	     i++) {
		dec = SOIE_EXT_HOOK(e, struct soie_ext_image, decode);
		if (dec && (px = dec(e->base.data, d, len, w, h))) {
			if (*w > 0 && *h > 0 && *w <= 16384 && *h <= 16384)
				return px;
			free(px);
		}
	}
	return NULL;
}

/* Content */

char *soie_ext_user_css(const char *css, const char *url)
{
	const struct soie_ext_content *e;
	char *(*hook)(void *, const char *), *s;
	struct buf b = { 0 };
	int i;

	if (css)
		buf_add(&b, css, strlen(css));
	for (i = 0; (e = (const void *)soie_ext_get(SOIE_EXT_CONTENT, i));
	     i++) {
		hook = SOIE_EXT_HOOK(e, struct soie_ext_content, user_css);
		if (hook && (s = hook(e->base.data, url))) {
			buf_putc(&b, '\n');
			buf_add(&b, s, strlen(s));
			free(s);
		}
	}
	if (!b.s)
		return NULL;
	buf_putc(&b, 0);
	return b.s;
}

void soie_ext_document(soie_node *doc, const char *url)
{
	const struct soie_ext_content *e;
	void (*hook)(void *, soie_node *, const char *);
	int i;

	for (i = 0; (e = (const void *)soie_ext_get(SOIE_EXT_CONTENT, i));
	     i++) {
		hook = SOIE_EXT_HOOK(e, struct soie_ext_content, document);
		if (hook)
			hook(e->base.data, doc, url);
	}
}

/* Network */

int soie_ext_blocked(const soie_request *req)
{
	const struct soie_ext_net *e;
	int (*hook)(void *, const soie_request *);
	int i;

	for (i = 0; (e = (const void *)soie_ext_get(SOIE_EXT_NET, i)); i++) {
		hook = SOIE_EXT_HOOK(e, struct soie_ext_net, filter);
		if (hook && hook(e->base.data, req))
			return 1;
	}
	return 0;
}

int soie_ext_fetch(const soie_request *req, soie_response *res)
{
	const struct soie_ext_net *e;
	int (*hook)(void *, const soie_request *, soie_response *);
	int i;

	memset(res, 0, sizeof(*res));
	for (i = 0; (e = (const void *)soie_ext_get(SOIE_EXT_NET, i)); i++) {
		hook = SOIE_EXT_HOOK(e, struct soie_ext_net, fetch);
		if (hook && !hook(e->base.data, req, res))
			return 0;
		soie_response_free(res);
	}
	return -1;
}

/* Cookies */

char *soie_cookies_get(const char *url, const char *first_party,
	int script)
{
	const struct soie_ext_cookies *e;
	char *(*hook)(void *, const char *, const char *, int);

	e = (const void *)soie_ext_get(SOIE_EXT_COOKIES, 0);
	if (!e || !(hook = SOIE_EXT_HOOK(e, struct soie_ext_cookies, get)))
		return NULL;
	return hook(e->base.data, url, first_party, script);
}

void soie_cookies_set(const char *url, const char *first_party,
	const char *line, int script)
{
	const struct soie_ext_cookies *e;
	void (*hook)(void *, const char *, const char *, const char *, int);

	e = (const void *)soie_ext_get(SOIE_EXT_COOKIES, 0);
	if (e && (hook = SOIE_EXT_HOOK(e, struct soie_ext_cookies, set)))
		hook(e->base.data, url, first_party, line, script);
}
