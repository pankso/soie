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
 * This is ABI 1.0, frozen with the library's 1.0: later versions only
 * append hooks to the hook sets and add kinds.
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
	int flags;		/* SOIE_REQ_* */
} soie_request;

#define SOIE_REQ_NO_COOKIES 1	/* credentials omitted: no jar either way */

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

/*
 * Scripts. The host gives these services to the script engine; the
 * engine runs <script> elements through soie_script_start() and the
 * calls below, and changes the tree through the public DOM API. The
 * host restyles when the document's generation moved
 * (soie_document_update()).
 */
enum {
	SOIE_MSG_LOG,		/* console.log, console.info */
	SOIE_MSG_WARN,
	SOIE_MSG_ERROR,		/* console.error, uncaught exceptions */
	SOIE_MSG_ALERT		/* alert() */
};

typedef struct {
	void *data;
	/* blocking: the engine calls it for scripts, fetch(), XHR */
	soie_request_fn request;
	/* location changes: the host loads url once the script returns */
	void (*navigate)(void *data, const char *url, int replace);
	void (*message)(void *data, int kind, const char *text);
	/* viewport size and scroll position, CSS px; and scrolling */
	void (*viewport)(void *data, int *w, int *h, float *sx, float *sy);
	void (*scroll)(void *data, float x, float y);
} soie_script_host;

#define SOIE_MOD_SHIFT 1
#define SOIE_MOD_CTRL 2
#define SOIE_MOD_ALT 4
#define SOIE_MOD_META 8

typedef struct {
	const char *type;	/* "click", "keydown", "input", "submit"... */
	float x, y;		/* mouse: viewport coordinates */
	int button;		/* 0 main, 1 middle, 2 secondary */
	int mods;		/* SOIE_MOD_* */
	const char *key;	/* keys: the DOM key value ("a", "Enter") */
} soie_event;

struct soie_ext_script {
	struct soie_ext base;
	/* A context for one document; NULL on failure. host stays valid
	 * until stop. */
	void *(*start)(void *data, soie_node *doc, const char *url,
		const soie_script_host *host);
	void (*stop)(void *ctx);
	/* Run a script: src (UTF-8, len bytes) of element el, from url
	 * (the page's for inline scripts); module for type=module. */
	void (*run)(void *ctx, soie_node *el, const char *src, size_t len,
		const char *url, int module);
	/* Dispatch ev at target (NULL: the window). Nonzero when a
	 * listener cancelled it: the host skips the default action. */
	int (*event)(void *ctx, soie_node *target, const soie_event *ev);
	/* Run due timers and jobs; ms until the next timer, -1: none. */
	int (*tick)(void *ctx);
};

/*
 * Start the first script engine on a parsed (and preferably styled)
 * document: run its scripts in order, external ones fetched through
 * host->request, deferred and module scripts after the others, then
 * fire DOMContentLoaded and load. -1 without a script engine.
 */
int soie_script_start(soie_node *doc, const char *url,
	const soie_script_host *host);
/* Run a script element the engine just inserted, once. */
int soie_script_element(soie_node *el);
/* Mark a script element as never to run (innerHTML insertions). */
void soie_script_skip(soie_node *el);
/* The script element running now, or NULL. */
soie_node *soie_script_current(const soie_node *doc);
/* Dispatch an event; 1 when cancelled (skip the default action). */
int soie_script_event(soie_node *doc, soie_node *target,
	const soie_event *ev);
/* Timers: run the due ones; ms until the next one, -1 when none. */
int soie_script_tick(soie_node *doc);
/* Stop scripts (soie_node_free() on the document does it too). */
void soie_script_stop(soie_node *doc);

/*
 * Audio and video. The engine opens a player for a <video> or <audio>
 * element when it is first played, paints its frames, and draws
 * simple controls; the extension demuxes, decodes and plays the
 * sound, in its own threads. Hooks are called from the host's thread.
 */
typedef struct {
	double duration;	/* seconds, 0 while unknown */
	double time;		/* current position, seconds */
	int paused, ended;
	int width, height;	/* video size, 0 for sound only */
	int error;		/* nonzero: cannot play */
	int changes;		/* bumped at each new frame or state */
} soie_media_state;

struct soie_ext_media {
	struct soie_ext base;
	/* Nonzero when the MIME type (may be NULL) looks playable. */
	int (*can_play)(void *data, const char *type);
	/* A player for url; request fetches its bytes (from another
	 * thread, with Range headers when it can). NULL on failure. */
	void *(*open)(void *data, const char *url, soie_request_fn request,
		void *request_data);
	void (*close)(void *player);
	void (*play)(void *player);
	void (*pause)(void *player);
	void (*seek)(void *player, double seconds);
	void (*volume)(void *player, double volume, int muted);
	void (*state)(void *player, soie_media_state *st);
	/* The frame to show now, straight RGBA, valid until the next
	 * call; NULL when there is none (yet). */
	const unsigned char *(*frame)(void *player, int *w, int *h);
};

enum {
	SOIE_MEDIA_PLAY,
	SOIE_MEDIA_PAUSE,
	SOIE_MEDIA_TOGGLE,
	SOIE_MEDIA_SEEK,	/* value: seconds */
	SOIE_MEDIA_VOLUME,	/* value: 0 to 1 */
	SOIE_MEDIA_MUTE		/* value: 0 or 1 */
};

/* Control the player of a <video> or <audio> element, opening it
 * when needed. -1 without a media extension or a source. */
int soie_media_control(soie_node *el, int op, double value);
/* Its state; -1 when it has no player. */
int soie_media_get_state(soie_node *el, soie_media_state *st);
/* A click at (x, y) in page coordinates on el, on its controls (play,
 * pause, the seek bar): 1 when handled. */
int soie_media_click(soie_node *el, float x, float y);
/* Call while players play: repaint is set when a frame or the
 * controls changed; returns ms until the next call, -1 when none
 * plays. Also fires the media events (timeupdate, ended...). */
int soie_media_tick(soie_node *doc, int *repaint);
/* Nonzero when a media extension says it can play type. */
int soie_media_can_play(const char *type);

#endif
