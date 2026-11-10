/*
 * The browser's networking: HTTP(S), file:// and data: fetches.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_NET_H
#define SOIE_NET_H

#include <stddef.h>
#include <soie/soie.h>
#include <soie/ext.h>

/*
 * A download: the body of a page load that is not shown (an archive,
 * an ISO image) goes to a file as it comes, not into memory. The
 * caller sets path (a mkstemp() template) and points response.dl at
 * it; the fetch fills the rest, the caller reads got and state and may
 * set cancel from its thread.
 */
enum { DL_NONE, DL_STREAMING, DL_DONE, DL_FAILED };
struct download {
	char path[4096];	/* the template, then the file */
	int fd;			/* the fetch's, -1 when not streaming */
	char *headers;		/* the response's header block, once started */
	long long total;	/* Content-Length, -1 unknown */
	volatile long long got;	/* bytes in the file */
	volatile int state;	/* DL_*: set last, the rest is ready then */
	volatile int cancel;	/* the caller's: stop */
};

/* Where an address is, from the most public: the private network guard
 * (SOIE_REQ_NO_LAN, SOIE_REQ_NO_LOOPBACK) keeps a page's requests at
 * its own level or more public */
enum { NET_PUBLIC, NET_LAN, NET_LOOPBACK };

struct response {
	int status;
	char *body;
	size_t len, cap;
	char type[128];		/* Content-Type without parameters */
	char charset[64];
	char url[4096];		/* after redirects */
	char *headers;		/* "name: value\r\n", names lower case */
	char error[256];
	int auth;		/* sent with the user's login (net_auth_set) */
	int level;		/* NET_*: the address it came from (file://:
				 * NET_LOOPBACK, the others NET_PUBLIC) */
	struct download *dl;	/* the caller's, or NULL: all in memory */
	int cut;		/* the body ended before its Content-Length or
				 * its last chunk: shown, never cached */
};

/* A request flag beside SOIE_REQ_*: the user's reload, the page is
 * asked again even when the cache holds it fresh */
#define NET_RELOAD 0x100

/* A request through the extensions (blocking, cookies, schemes), the
 * HTTP cache (cache.c) and the network. A body without a Content-Type
 * header is sent as a urlencoded form. SOIE_REQ_NO_LAN and
 * SOIE_REQ_NO_LOOPBACK are checked on the address connected to, after
 * each redirection (no DNS rebinding around them). */
int net_fetch(const soie_request *req, struct response *r);
/* Open a connection to url's server and leave it in the pool for the
 * requests to come, unless n are there already. */
int net_preconnect(const char *url, int n);
/* The same as an extension request function (data unused). */
int net_request(void *data, const soie_request *req, soie_response *res);
/* Content blocking (the net extensions' filter) on or off, on at
 * first; any thread. */
void net_blocking(int on);
void response_free(struct response *r);
/* HTTP Basic logins, by origin, in memory: the user's for url's origin
 * (-1: a ':' in user, a bad URL), forget it, forget all of the private
 * windows (priv 1) or the others (0). Sent with that origin's own
 * requests only. */
int net_auth_set(const char *url, const char *user, const char *password,
	int priv);
void net_auth_forget(const char *url, int priv);
void net_auth_clear(int priv);
/* Does a response with these headers ask for a Basic login? Its realm
 * (or "") in realm. */
int net_auth_basic(const char *headers, char *realm, size_t size);
/* Is a response with these headers (a header block, or "name: value"
 * lines) shown as a page, or downloaded? */
int net_shown(const char *headers);

#endif
