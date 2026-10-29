/*
 * URL resolution: a reference against a base, after RFC 3986 section 5
 * with the WHATWG tolerances pages rely on (backslashes, whitespace).
 * No IDNA, no percent-encoding normalization.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "internal.h"

struct parts {
	const char *scheme, *auth, *path, *query, *frag;
	size_t ls, la, lp, lq, lf;
	int has_auth, has_query, has_frag;
};

static int is_alpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void split(const char *s, size_t n, struct parts *p)
{
	size_t i = 0, start;

	memset(p, 0, sizeof(*p));
	if (n && is_alpha((unsigned char)s[0])) {
		for (i = 1; i < n && (is_alpha((unsigned char)s[i]) ||
		     (s[i] >= '0' && s[i] <= '9') || s[i] == '+' ||
		     s[i] == '-' || s[i] == '.'); i++)
			;
		if (i < n && s[i] == ':') {
			p->scheme = s;
			p->ls = i;
			i++;
		} else {
			i = 0;
		}
	}
	if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
		i += 2;
		start = i;
		while (i < n && s[i] != '/' && s[i] != '?' && s[i] != '#')
			i++;
		p->auth = s + start;
		p->la = i - start;
		p->has_auth = 1;
	}
	start = i;
	while (i < n && s[i] != '?' && s[i] != '#')
		i++;
	p->path = s + start;
	p->lp = i - start;
	if (i < n && s[i] == '?') {
		start = ++i;
		while (i < n && s[i] != '#')
			i++;
		p->query = s + start;
		p->lq = i - start;
		p->has_query = 1;
	}
	if (i < n && s[i] == '#') {
		p->frag = s + i + 1;
		p->lf = n - i - 1;
		p->has_frag = 1;
	}
}

/* RFC 3986 5.2.4, appending the result to out. */
static void remove_dots(const char *in, size_t n, struct buf *out)
{
	size_t base = out->len;

	while (n) {
		if (n >= 3 && !memcmp(in, "../", 3)) {
			in += 3, n -= 3;
		} else if (n >= 2 && !memcmp(in, "./", 2)) {
			in += 2, n -= 2;
		} else if (n >= 3 && !memcmp(in, "/./", 3)) {
			in += 2, n -= 2;
		} else if (n == 2 && !memcmp(in, "/.", 2)) {
			in = "/", n = 1;
		} else if ((n >= 4 && !memcmp(in, "/../", 4)) ||
			   (n == 3 && !memcmp(in, "/..", 3))) {
			if (n == 3)
				in = "/", n = 1;
			else
				in += 3, n -= 3;
			while (out->len > base && out->s[out->len - 1] != '/')
				out->len--;
			if (out->len > base)
				out->len--;
		} else if ((n == 1 && in[0] == '.') ||
			   (n == 2 && !memcmp(in, "..", 2))) {
			n = 0;
		} else {
			size_t k = in[0] == '/' ? 1 : 0;

			while (k < n && in[k] != '/')
				k++;
			buf_add(out, in, k);
			in += k, n -= k;
		}
	}
	buf_add(out, "", 0);
}

static int special(const struct parts *p)
{
	static const char *const s[] = { "http", "https", "ftp", "file",
		"ws", "wss", NULL };
	int i;

	for (i = 0; s[i]; i++)
		if (p->ls == strlen(s[i]) && !strncmp(p->scheme, s[i], p->ls))
			return 1;
	return 0;
}

char *soie_url_resolve(const char *base, const char *ref)
{
	struct buf clean = { 0 }, out = { 0 };
	struct parts b, r;
	size_t n, i;
	const char *s;

	/* Trim C0 controls and spaces, drop tabs and newlines. */
	while (*ref && (unsigned char)*ref <= ' ')
		ref++;
	n = strlen(ref);
	while (n && (unsigned char)ref[n - 1] <= ' ')
		n--;
	buf_add(&clean, "", 0);
	for (i = 0; i < n; i++)
		if (ref[i] != '\t' && ref[i] != '\n' && ref[i] != '\r')
			buf_putc(&clean, ref[i]);
	s = clean.s;
	split(base ? base : "", base ? strlen(base) : 0, &b);
	if (special(&b)) {
		for (i = 0; i < clean.len && clean.s[i] != '?' &&
		     clean.s[i] != '#'; i++)
			if (clean.s[i] == '\\')
				clean.s[i] = '/';
	}
	split(s, clean.len, &r);
	buf_add(&out, "", 0);
	if (r.scheme && !(special(&r) && b.scheme && r.ls == b.ls &&
	    !strncmp(r.scheme, b.scheme, r.ls) && !r.has_auth)) {
		buf_add(&out, r.scheme, r.ls + 1);
		if (r.has_auth) {
			buf_add(&out, "//", 2);
			buf_add(&out, r.auth, r.la);
		}
		remove_dots(r.path, r.lp, &out);
		goto tail;
	}
	if (r.scheme) {
		/* "http:foo" against an http base: relative */
		s += r.ls + 1;
		split(s, strlen(s), &r);
	}
	if (!b.scheme) {
		buf_free(&out);
		return clean.s;		/* no usable base */
	}
	buf_add(&out, b.scheme, b.ls + 1);
	if (r.has_auth) {
		buf_add(&out, "//", 2);
		buf_add(&out, r.auth, r.la);
		remove_dots(r.path, r.lp, &out);
		goto tail;
	}
	if (b.has_auth) {
		buf_add(&out, "//", 2);
		buf_add(&out, b.auth, b.la);
	}
	if (!r.lp) {
		buf_add(&out, b.path, b.lp);
		if (!r.has_query && b.has_query) {
			buf_putc(&out, '?');
			buf_add(&out, b.query, b.lq);
		}
	} else if (r.path[0] == '/') {
		remove_dots(r.path, r.lp, &out);
	} else {
		struct buf merged = { 0 };
		size_t k = b.lp;

		while (k && b.path[k - 1] != '/')
			k--;
		if (b.has_auth && !b.lp)
			buf_putc(&merged, '/');
		buf_add(&merged, b.path, k);
		buf_add(&merged, r.path, r.lp);
		remove_dots(merged.s, merged.len, &out);
		buf_free(&merged);
	}
tail:
	if (r.has_query) {
		buf_putc(&out, '?');
		buf_add(&out, r.query, r.lq);
	}
	if (r.has_frag) {
		buf_putc(&out, '#');
		buf_add(&out, r.frag, r.lf);
	}
	buf_free(&clean);
	return out.s;
}

/* n bytes of a and b equal, ASCII case aside (schemes, hosts) */
static int same_ci(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		int x = (unsigned char)a[i], y = (unsigned char)b[i];

		if (x >= 'A' && x <= 'Z')
			x += 32;
		if (y >= 'A' && y <= 'Z')
			y += 32;
		if (x != y)
			return 0;
	}
	return 1;
}

static int is_scheme(const struct parts *p, const char *name)
{
	return p->ls == strlen(name) && same_ci(p->scheme, name, p->ls);
}

int soie_url_rewritable(const char *doc, const char *target)
{
	struct parts a, b;

	if (!doc || !target)
		return 0;
	split(doc, strlen(doc), &a);
	split(target, strlen(target), &b);
	/* the same scheme, user and host and port */
	if (!a.scheme || !b.scheme || a.ls != b.ls ||
	    !same_ci(a.scheme, b.scheme, a.ls) || a.has_auth != b.has_auth ||
	    a.la != b.la || !same_ci(a.auth, b.auth, a.la))
		return 0;
	if (is_scheme(&a, "http") || is_scheme(&a, "https"))
		return 1;
	if (a.lp != b.lp || memcmp(a.path, b.path, a.lp))
		return 0;
	if (is_scheme(&a, "file"))
		return 1;
	return a.has_query == b.has_query && a.lq == b.lq &&
		!memcmp(a.query, b.query, a.lq);
}
