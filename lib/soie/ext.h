/*
 * Soie extensions: hooks the engine and the host call, filled by code
 * that is compiled in or loaded with dlopen().
 *
 * ABI rules:
 * - every hook set starts with a struct soie_ext header;
 * - abi is SOIE_EXT_ABI when the extension is built: the engine
 *   refuses another major version;
 * - size is sizeof() the hook set the extension was built with: hooks
 *   appended by a later minor version read as NULL from an older
 *   extension (use SOIE_EXT_HOOK to read one);
 * - any hook may be NULL;
 * - strings returned by hooks are malloc()ed and freed by the caller.
 *
 * A module for soie_ext_load() exports "int soie_ext_init(void)",
 * which registers its hook sets with soie_ext_register() and returns
 * 0. Hook sets are kept by pointer: give static storage.
 */
#ifndef SOIE_EXT_H
#define SOIE_EXT_H

#include <stddef.h>
#include "dom.h"

#define SOIE_EXT_ABI_MAJOR 1
#define SOIE_EXT_ABI_MINOR 0
#define SOIE_EXT_ABI (SOIE_EXT_ABI_MAJOR << 16 | SOIE_EXT_ABI_MINOR)

typedef enum {
	SOIE_EXT_IMAGE = 1,	/* image decoders */
	SOIE_EXT_CONTENT,	/* user style sheets, document changes */
	SOIE_EXT_NET,		/* request filters, URL schemes */
	SOIE_EXT_COOKIES,	/* the cookie jar */
	SOIE_EXT_SCRIPT,	/* a script engine */
	SOIE_EXT_MEDIA		/* audio and video */
} soie_ext_kind;

struct soie_ext {
	unsigned abi;		/* SOIE_EXT_ABI */
	unsigned size;		/* sizeof the whole hook set */
	soie_ext_kind kind;
	const char *name;
	void *data;		/* passed back as the first hook argument */
};

/* NULL when the hook set e (a pointer to a hook set of type T) is too
 * old to have the hook. */
#define SOIE_EXT_HOOK(e, T, hook) \
	(offsetof(T, hook) + sizeof((e)->hook) <= (e)->base.size ? \
	 (e)->hook : NULL)

/* Returns -1 for a wrong ABI version or kind. */
int soie_ext_register(const struct soie_ext *e);
/* The i-th registered hook set of a kind, NULL past the last one. */
const struct soie_ext *soie_ext_get(soie_ext_kind kind, int i);
/* dlopen() a module and run its soie_ext_init(). */
int soie_ext_load(const char *path);
/* Load every *.so of a directory, in name order; returns how many. */
int soie_ext_load_dir(const char *dir);

/*
 * Requests, for hosts and the extensions that fetch through them.
 * All response strings are malloc()ed; soie_response_free() them.
 */
typedef struct {
	const char *url;
	const char *method;	/* NULL is GET */
	const char *headers;	/* extra "Name: value\r\n" lines, or NULL */
	const char *body;	/* request body, or NULL */
	size_t body_len;
	const char *first_party; /* the page's URL, NULL for the page */
} soie_request;

typedef struct {
	int status;		/* HTTP status, 200 for files */
	char *url;		/* final URL, after redirects */
	char *headers;		/* "Name: value\r\n" lines, names lower case */
	char *body;
	size_t len;
} soie_response;

/* Return 0 and fill res, or -1 on failure (res left empty). */
typedef int (*soie_request_fn)(void *data, const soie_request *req,
	soie_response *res);
void soie_response_free(soie_response *res);
/* Value of a response header (first one), malloc()ed, or NULL. */
char *soie_response_header(const soie_response *res, const char *name);

/* Image decoders, tried when the built-in ones (PNG, JPEG, GIF, SVG)
 * do not know the bytes. */
struct soie_ext_image {
	struct soie_ext base;
	/* Straight-alpha RGBA, w * 4 bytes per row, malloc()ed; NULL when
	 * the bytes are not in this extension's format. */
	unsigned char *(*decode)(void *data, const unsigned char *bytes,
		size_t len, int *w, int *h);
};

/* Content hooks, called by the host through the helpers below. */
struct soie_ext_content {
	struct soie_ext base;
	/* A user style sheet for the page at url, or NULL. */
	char *(*user_css)(void *data, const char *url);
	/* The page is parsed; scripts have not run yet. */
	void (*document)(void *data, soie_node *doc, const char *url);
};

/* css (may be NULL) followed by every extension's user sheet for url;
 * malloc()ed, NULL when there is none at all. */
char *soie_ext_user_css(const char *css, const char *url);
void soie_ext_document(soie_node *doc, const char *url);

/* Network hooks, called by the host before and instead of its own
 * fetches. The engine never fetches by itself. */
struct soie_ext_net {
	struct soie_ext base;
	/* Nonzero cancels the request (content blocking). */
	int (*filter)(void *data, const soie_request *req);
	/* Fetch the request instead of the host (a scheme the host does
	 * not know, a cache): 0 when done, -1 to let the host do it. */
	int (*fetch)(void *data, const soie_request *req,
		soie_response *res);
};

/* Nonzero when a net extension cancels req. */
int soie_ext_blocked(const soie_request *req);
/* 0 when a net extension fetched req, -1 otherwise. */
int soie_ext_fetch(const soie_request *req, soie_response *res);

/* The cookie jar. first_party is the URL of the page that makes the
 * request (or that url itself for a page load); script is nonzero for
 * document.cookie, which never sees HttpOnly cookies. */
struct soie_ext_cookies {
	struct soie_ext base;
	/* The Cookie header value for a request to url, or NULL. */
	char *(*get)(void *data, const char *url, const char *first_party,
		int script);
	/* A Set-Cookie header value (or a document.cookie assignment). */
	void (*set)(void *data, const char *url, const char *first_party,
		const char *line, int script);
};

/* The first cookie jar's answer, NULL without one. */
char *soie_cookies_get(const char *url, const char *first_party,
	int script);
void soie_cookies_set(const char *url, const char *first_party,
	const char *line, int script);

#endif
