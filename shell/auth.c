/*
 * about:auth: the login form for a page that answered 401 with a
 * Basic challenge. It posts to itself, without a script; the shell
 * keeps which page asked and what it said, the form only carries what
 * the user typed. Only about:auth may post to about:auth.
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
#include <strings.h>
#include "auth.h"
#include "settings.h"

int auth_url(const char *url)
{
	size_t n = strlen(AUTH_URL);

	return !strncasecmp(url, AUTH_URL, n) && (!url[n] ||
		url[n] == '?' || url[n] == '#');
}

struct buf {
	char *s;
	size_t len, cap;
};

static void add(struct buf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2;
		if (!(b->s = realloc(b->s, b->cap)))
			abort();
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static void adds(struct buf *b, const char *s)
{
	add(b, s, strlen(s));
}

static void addesc(struct buf *b, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n && s[i]; i++) {
		const char *e = s[i] == '&' ? "&amp;" : s[i] == '<' ? "&lt;" :
			s[i] == '>' ? "&gt;" : s[i] == '"' ? "&quot;" : NULL;

		if (e)
			adds(b, e);
		else
			add(b, s + i, 1);
	}
}

/* Is the host of url this machine: no one else can read the wire */
static int local_host(const char *h, size_t n)
{
	return (n == 9 && !strncasecmp(h, "localhost", 9)) ||
		(n > 4 && !strncmp(h, "127.", 4));
}

char *auth_page(const char *url, const char *realm, const char *said)
{
	struct buf b = { NULL, 0, 0 };
	const char *h = url ? strstr(url, "://") : NULL;
	size_t n = 0, host;

	if (h) {
		h += 3;
		n = strcspn(h, "/?#");
	}
	adds(&b, "<!DOCTYPE html><html><head><meta charset=utf-8>"
		"<title>Log in</title>");
	adds(&b, settings_style);
	adds(&b, "</head><body><main><h1>Log in</h1>");
	if (!h) {
		adds(&b, "<p>No page is asking for a login.</p>"
			"</main></body></html>");
		return b.s;
	}
	if (said) {
		adds(&b, "<p class=said>");
		adds(&b, said);
		adds(&b, "</p>");
	}
	adds(&b, "<section><p><b>");
	addesc(&b, h, n);
	adds(&b, "</b> asks for a user name and a password.</p>");
	if (*realm) {
		adds(&b, "<p class=hint>The server says: <code>");
		addesc(&b, realm, strlen(realm));
		adds(&b, "</code></p>");
	}
	host = strcspn(h, ":/?#");
	if (!strncasecmp(url, "http:", 5) && !local_host(h, host))
		adds(&b, "<p class=said>This connection is not encrypted: "
			"the password goes over the network as it is.</p>");
	adds(&b, "<form method=post action=\"" AUTH_URL "\">"
		"<p><label>User name<input type=text name=user></label></p>"
		"<p><label>Password<input type=password name=password>"
		"</label></p><p><button class=go type=submit name=login "
		"value=1>Log in</button> <button type=submit name=cancel "
		"value=1>Cancel</button></p></form></section>");
	adds(&b, "<p class=hint>Kept in memory until the browser closes, "
		"sent to this server only.</p></main></body></html>");
	return b.s;
}

int auth_form(const char *post, char **user, char **password)
{
	char *body = strdup(post), *p, *next, *v;
	int cancel = 0;

	if (!body)
		abort();
	*user = *password = NULL;
	for (p = body; *p; p = next) {
		if ((next = strchr(p, '&')))
			*next++ = '\0';
		else
			next = p + strlen(p);
		if ((v = strchr(p, '=')))
			*v++ = '\0';
		else
			v = p + strlen(p);
		settings_decode(p);
		settings_decode(v);
		if (!strcmp(p, "cancel"))
			cancel = 1;
		else if (!strcmp(p, "user") && !*user)
			*user = strdup(v);
		else if (!strcmp(p, "password") && !*password)
			*password = strdup(v);
	}
	memset(body, 0, strlen(post));
	free(body);
	/* Enter in a field sends no button: a login too */
	if (cancel || !*user || !*password) {
		if (*password)
			memset(*password, 0, strlen(*password));
		free(*user);
		free(*password);
		*user = *password = NULL;
		return cancel ? 0 : -1;
	}
	return 1;
}
