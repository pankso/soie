/*
 * The browser's shell: page views (struct shell), each with its page,
 * resources, history, forms, scripts and find, on libsoie, without a
 * toolkit (shell.h).
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE		/* dladdr() */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <png.h>
#include <soie/soie.h>
#include "net.h"
#include "fetch.h"
#include "shell.h"
#include "settings.h"
#include "debug.h"

#define HOME SHELL_HOME

static const char home_html[] =
	"<!DOCTYPE html><title>Soie</title><style>body{font-family:sans-serif;"
	"max-width:40em;margin:3em auto;color:#333}h1{color:#5a3e9c}"
	"code{background:#eee;padding:1px 4px}</style><h1>Soie</h1>"
	"<p>A lightweight HTML rendering engine, and this browser.</p>"
	"<p>Type an address or words to search for above. "
	"<code>Alt+\xe2\x86\x90</code> back, <code>F5</code> reload, "
	"<code>Ctrl+L</code> address bar, <code>Ctrl+F</code> find, "
	"<code>Ctrl+Q</code> quit.</p>";

/*
 * Resources of the current page, fetched once, in the background:
 * the page's style sheets and scripts first, then the rest.
 */
enum { K_DOC, K_STYLE, K_SCRIPT, K_IMAGE };	/* job kinds */
#define PRECONNECT 3	/* connections opened ahead to a page's server */
enum { R_PENDING, R_DONE, R_FAILED, R_GIVEN };	/* GIVEN: to the engine */
struct res {
	char *url;
	char *data;
	size_t len;
	int state, kind;
};

/* Loading stages: the page's bytes, its style sheets (the first paint
 * waits for them), its scripts (they run in order once all are here) */
enum { L_NONE, L_DOC, L_STYLE, L_SCRIPTS };

struct shell {
	struct {
		char url[4096];
		soie_node *doc;
		float height;
		int scroll;
		struct res *res;
		int nres;
		char *user_css;		/* from the content extensions */
		unsigned gen;		/* each load's, unique among the
					 * shells: older jobs drop */
		int loading;		/* L_* */
		int push;		/* the history action of the load */
		char *want;		/* the URL being loaded */
		int restyle, relayout;	/* late style sheets, late images */
		double laid_out;	/* when, for throttling */
		soie_node *old;		/* on screen until the new one is
					 * laid out */
		int old_scroll;
		double t_load;		/* when the load started, for
					 * debugging */
		int done;		/* everything in: told once */
		float dirty[4];		/* page rectangle of the players'
					 * frames */
	} page;
	struct shell_ui ui;
	void *ui_data;
	soie_script_host host;	/* data: the shell */
	struct shell *next;	/* all of them */
	struct job *inbox;	/* finished jobs of its page, in order */
	int in_style;		/* page_fetch() called by the cascade */
	struct job *deferred;	/* a page that came while scripts ran */
	struct {
		char *url[256];
		int scroll[256];
		int n, cur;
	} hist;
	soie_node *focus;	/* focused page control */
	int caret;
	int view_w, view_h;
	float zoom;		/* the page's, 1 */
	int dark;		/* prefers-color-scheme: dark */
	int private;		/* SOIE_REQ_PRIVATE: a private window */
	/* The front's pixels are B G R A (shell_bgra()); what shell_paint()
	 * showed last, at which scroll and document generation */
	int bgra, painted_scroll;
	soie_node *painted;
	unsigned long painted_gen;
	/* Something changed that the engine's damage does not see: a page,
	 * a stage, a resource (images, fonts), another size */
	int damage_full;
	char title[512];	/* the one the front shows */
	int scripts;		/* 0: no JavaScript */
	char *pending_nav;	/* a script's navigation, done after it */
	char *pending_open;	/* a script's new window, after it too */
	int in_input;		/* in the user's click or key */
	int pending_replace;
	char *refresh_url;	/* <meta http-equiv=refresh> */
	double refresh_at;
	/* Find in the page: the text, the current match (from 1), its
	 * rect */
	struct {
		char *text;
		int cur, n;
		float x, y, w, h;
	} found;
	/* The selection: its page points (where the button went down,
	 * where it is), its unit, its rects (n -1: to find again) */
	struct {
		soie_node *doc;
		float pt[4];
		int unit, n, cap;
		float *r;
	} sel;
	char loading_text[64];	/* shell_loading()'s */
	struct download *page_dl;	/* the page load's, its job's */
};

/* A download: a page load that was not a page. They are the process's:
 * one goes on when the shell it came from is freed. */
struct sdl {
	unsigned id;
	struct shell *s;	/* where it came from, NULL once freed */
	struct download *d;	/* its job's, until the job is collected */
	char *url;
	char dest[4096];	/* "" when cancelled */
	long long told;		/* bytes last told the front */
	double told_at;
	struct sdl *next;
};
static unsigned dl_ids;
static struct sdl *downloads;
static struct {
	void (*progress)(void *data, unsigned id, long long got,
		long long total);
	void (*done)(void *data, unsigned id, const char *error);
	void *data;
} dl_ui;

/* Shared by the shells: the fetch threads, their descriptor */
static int fetch_fd = -1;	/* readable when jobs finish */
static pthread_t main_thread;	/* the shells': scripts' background
				 * requests come from others */
static struct shell *shells;
static unsigned gens;		/* the last load's gen */

static int page_changed(struct shell *s);

/* Fetching */

static double now_s(void);

/* URL schemes of the front's own (toile://), answered in this thread */
#define NSCHEMES 8
static struct {
	char name[32];
	shell_scheme_fn fn;
	void *data;
} schemes[NSCHEMES];
static int nschemes;

void shell_scheme(const char *name, shell_scheme_fn fn, void *data)
{
	int i;

	for (i = 0; i < nschemes && strcasecmp(schemes[i].name, name); i++)
		;
	if (i == nschemes) {
		if (nschemes == NSCHEMES || strlen(name) >= sizeof(
		    schemes[0].name))
			return;
		nschemes++;
	}
	snprintf(schemes[i].name, sizeof(schemes[i].name), "%s", name);
	schemes[i].fn = fn;
	schemes[i].data = data;
}

/* The front's scheme of url, -1 when it has none */
static int scheme_of(const char *url)
{
	size_t n = strcspn(url, ":");
	int i;

	if (!url[n])
		return -1;
	for (i = 0; i < nschemes; i++)
		if (strlen(schemes[i].name) == n &&
		    !strncasecmp(url, schemes[i].name, n))
			return i;
	return -1;
}

/*
 * May the page at from have url? A front's scheme is for the front and
 * its own pages (what the user types, a toile:// page's links), never
 * for the web's (WebKit's display isolated schemes). file:// alike: a
 * link, a script, a refresh or a form of a web page never opens a
 * local file, only the user and local pages do. about:settings is the
 * user's and its own: a page that could post to it would change them.
 */
static int scheme_allowed(const char *from, const char *url)
{
	int k = scheme_of(url);

	if (!from || !*from)
		return 1;
	if (settings_url(url))
		return settings_url(from);
	if (!strncasecmp(url, "file:", 5))
		return !strncasecmp(from, "file:", 5);
	return k < 0 || scheme_of(from) == k;
}

/* The answer of the front's scheme k for url, as a finished job */
static void scheme_answer(int k, const char *url, struct job *j)
{
	char *body = NULL;
	size_t len = 0;

	memset(j, 0, sizeof(*j));
	j->url = (char *)url;
	snprintf(j->r.url, sizeof(j->r.url), "%s", url);
	if (schemes[k].fn(schemes[k].data, url, &body, &len, j->r.type,
	    sizeof(j->r.type)) || !body) {
		free(body);
		snprintf(j->r.error, sizeof(j->r.error), "not found");
		return;
	}
	j->ok = 1;
	j->r.status = 200;
	j->r.body = body;
	j->r.len = len;
	if (!*j->r.type)
		strcpy(j->r.type, "text/html");
	/* "text/html; charset=utf-8": the type, the charset apart */
	{
		char *c = strchr(j->r.type, ';'), *cs;

		if (c) {
			if ((cs = strstr(c, "charset=")))
				snprintf(j->r.charset, sizeof(j->r.charset),
					"%.*s", (int)strcspn(cs + 8, " ;"),
					cs + 8);
			while (c > j->r.type && c[-1] == ' ')
				c--;
			*c = '\0';
		}
	}
}

/* Ask for a resource: the front's scheme at once, else the network. */
static void res_fetch(struct shell *s, struct res *r, int urgent)
{
	int k = scheme_of(r->url);
	struct job j;

	if (k < 0) {
		fetch_add(r->url, NULL, s->page.url, r->kind, s->page.gen,
			urgent, s->private, NULL);
		return;
	}
	if (!scheme_allowed(s->page.url, r->url)) {
		r->state = R_FAILED;
		return;
	}
	scheme_answer(k, r->url, &j);
	r->state = j.ok ? R_DONE : R_FAILED;
	r->data = j.r.body;
	r->len = j.r.len;
}

static struct res *res_find(struct shell *s, const char *url)
{
	int i;

	for (i = 0; i < s->page.nres; i++)
		if (!strcmp(s->page.res[i].url, url))
			return &s->page.res[i];
	return NULL;
}

/* Queue a resource of the page; urgent unless it is an image. */
static struct res *res_add(struct shell *s, const char *url, int kind)
{
	struct res *r;

	s->page.res = realloc(s->page.res, (size_t)(s->page.nres + 1) *
		sizeof(*s->page.res));
	if (!s->page.res)
		abort();
	r = &s->page.res[s->page.nres++];
	memset(r, 0, sizeof(*r));
	r->url = strdup(url);
	r->kind = kind;
	r->state = R_PENDING;
	res_fetch(s, r, kind != K_IMAGE);
	return r;
}

static int res_pending(struct shell *s, int kind)
{
	int i, n = 0;

	for (i = 0; i < s->page.nres; i++)
		if (s->page.res[i].state == R_PENDING &&
		    (kind < 0 || s->page.res[i].kind == kind))
			n++;
	return n;
}

/*
 * The engine's fetch: what has arrived, or "later" while it is on its
 * way (the engine asks again after the next restyle or relayout).
 */
static int page_fetch(void *data, const char *url, char **buf, size_t *len)
{
	struct shell *s = data;
	struct res *r = res_find(s, url);

	if (r && r->state == R_GIVEN) {
		/* asked twice: fetch it again */
		r->state = R_PENDING;
		res_fetch(s, r, 0);
	}
	if (!r)
		r = res_add(s, url, s->in_style ? K_STYLE : K_IMAGE);
	if (r->state == R_PENDING)
		return SOIE_FETCH_LATER;
	if (r->state == R_FAILED || !r->data)
		return -1;
	if (r->kind == K_IMAGE) {
		/* the engine keeps images: no second copy here */
		*buf = r->data;
		*len = r->len;
		r->data = NULL;
		r->state = R_GIVEN;
		return 0;
	}
	*buf = malloc(r->len + 1);
	if (!*buf)
		abort();
	memcpy(*buf, r->data, r->len);
	*len = r->len;
	return 0;
}

static void drop_resources(struct shell *s)
{
	int i;

	for (i = 0; i < s->page.nres; i++) {
		free(s->page.res[i].url);
		free(s->page.res[i].data);
	}
	free(s->page.res);
	s->page.res = NULL;
	s->page.nres = 0;
}

/* DOM helpers */

static soie_node *find_tag(soie_node *n, const char *tag)
{
	soie_node *c, *r;

	if (soie_node_get_type(n) == SOIE_ELEMENT &&
	    !strcmp(soie_element_name(n), tag))
		return n;
	for (c = soie_node_first_child(n); c; c = soie_node_next_sibling(c))
		if ((r = find_tag(c, tag)))
			return r;
	return NULL;
}

static soie_node *find_id(soie_node *n, const char *id)
{
	soie_node *c, *r;
	const char *a;

	if (soie_node_get_type(n) == SOIE_ELEMENT &&
	    (((a = soie_element_get_attr(n, "id")) && !strcmp(a, id)) ||
	     (!strcmp(soie_element_name(n), "a") &&
	      (a = soie_element_get_attr(n, "name")) && !strcmp(a, id))))
		return n;
	for (c = soie_node_first_child(n); c; c = soie_node_next_sibling(c))
		if ((r = find_id(c, id)))
			return r;
	return NULL;
}

static void text_of(soie_node *n, char *out, size_t size)
{
	soie_node *c;
	size_t len = strlen(out);

	for (c = soie_node_first_child(n); c; c = soie_node_next_sibling(c)) {
		if (soie_node_get_type(c) == SOIE_TEXT) {
			size_t k;
			const char *d = soie_node_data(c, &k);

			if (len + k >= size)
				k = size - len - 1;
			memcpy(out + len, d, k);
			len += k;
			out[len] = '\0';
		} else {
			text_of(c, out, size);
			len = strlen(out);
		}
	}
}

static soie_node *ancestor(soie_node *n, const char *tag)
{
	for (; n && soie_node_get_type(n) == SOIE_ELEMENT;
	     n = soie_node_parent(n))
		if (!strcmp(soie_element_name(n), tag))
			return n;
	return NULL;
}

/* Scripts */

static void clamp_scroll(struct shell *s);
static void set_title(struct shell *s);
static void navigate_to(struct shell *s, const char *href, int replace);

/* page.url in the history: a new entry (push 1), the current one's
 * (push 2), none (0) */
static void hist_note(struct shell *s, int push)
{
	int i;

	if (push == 2 && s->hist.cur >= 0) {
		free(s->hist.url[s->hist.cur]);
		s->hist.url[s->hist.cur] = strdup(s->page.url);
		s->hist.scroll[s->hist.cur] = 0;
	} else if (push) {
		for (i = s->hist.cur + 1; i < s->hist.n; i++)
			free(s->hist.url[i]);
		if (s->hist.cur + 1 >= 256) {
			free(s->hist.url[0]);
			memmove(s->hist.url, s->hist.url + 1,
				255 * sizeof(char *));
			memmove(s->hist.scroll, s->hist.scroll + 1,
				255 * sizeof(int));
			s->hist.cur--;
		}
		s->hist.cur++;
		s->hist.url[s->hist.cur] = strdup(s->page.url);
		s->hist.scroll[s->hist.cur] = 0;
		s->hist.n = s->hist.cur + 1;
	}
}

/* history.pushState(), replaceState(): the address only, no load */
static void js_url(void *data, const char *url, int push)
{
	struct shell *s = data;

	if (!strcmp(url, s->page.url))
		return;
	/* another origin's address on this page would be a lie in the
	 * address bar, and let the page pass for it: about:settings,
	 * file:// (dom.js throws before, this is the host's word) */
	if (!soie_url_rewritable(s->page.url, url)) {
		debug(1, "not this page's address: %s", url);
		return;
	}
	debug(2, "url %s%s", url, push ? "" : " (replaced)");
	if (push && s->hist.cur >= 0)
		s->hist.scroll[s->hist.cur] = s->page.scroll;
	snprintf(s->page.url, sizeof(s->page.url), "%s", url);
	hist_note(s, push ? 1 : 2);
	debug_page(s->page.url);
	s->ui.address(s->ui_data, s->page.url);
}

static void js_navigate(void *data, const char *url, int replace)
{
	struct shell *s = data;

	if (replace == 2) {
		/* a new window: for the user's click or key only, as
		 * browsers block pop-ups */
		if (!s->in_input) {
			debug(1, "no window without a click: %s", url);
			return;
		}
		if (s->ui.open) {
			free(s->pending_open);
			s->pending_open = strdup(url);
			return;
		}
		replace = 0;
	}
	free(s->pending_nav);
	s->pending_nav = strdup(url);
	s->pending_replace = replace;
}

static void js_message(void *data, int kind, const char *text)
{
	struct shell *s = data;

	if (kind == SOIE_MSG_ALERT) {
		s->ui.alert(s->ui_data, text);
		fprintf(stderr, "alert: %s\n", text);
	} else {
		debug(kind == SOIE_MSG_ERROR ? 1 : 2, "js %s: %s",
			kind == SOIE_MSG_ERROR ? "error" : kind == SOIE_MSG_WARN ?
			"warn" : "log", text);
	}
}

static void js_viewport(void *data, int *w, int *h, float *sx, float *sy)
{
	struct shell *s = data;

	*w = s->view_w;
	*h = s->view_h;
	*sx = 0;
	*sy = (float)s->page.scroll;
}

static void js_scroll(void *data, float x, float y)
{
	struct shell *s = data;

	(void)x;
	s->page.scroll = (int)y;
}

static void collect_jobs(struct shell *s, int nested);

/*
 * Scripts' requests: what the page already fetched (its scripts), waited
 * for if still on its way; anything else straight to the network.
 */
static int page_request(void *data, const soie_request *req,
	soie_response *res)
{
	struct shell *s = data;	/* only in its own thread */
	struct res *r = NULL;
	struct pollfd p;

	/* from the script engine's threads (fetch, async XHR): straight to
	 * the network, the page's resources are the main thread's */
	if (!pthread_equal(pthread_self(), main_thread)) {
		int r;

		if (scheme_of(req->url) >= 0)
			return -1;	/* the front's: in its thread only */
		r = net_request(NULL, req, res);

		debug(2, "script request %s %s: %s %d", req->method ? req->method :
			"GET", req->url, r ? "failed" : "HTTP", r ? 0 : res->status);
		return r;
	}
	if (scheme_of(req->url) >= 0) {
		struct job j;

		if (!scheme_allowed(s->page.url, req->url))
			return -1;
		scheme_answer(scheme_of(req->url), req->url, &j);
		if (!j.ok)
			return -1;
		memset(res, 0, sizeof(*res));
		res->status = 200;
		res->url = strdup(req->url);
		res->headers = malloc(strlen(j.r.type) + 20);
		if (!res->url || !res->headers)
			abort();
		sprintf(res->headers, "content-type: %s\r\n", j.r.type);
		res->body = j.r.body;
		res->len = j.r.len;
		return 0;
	}
	if (!req->body && (!req->method || !strcmp(req->method, "GET")))
		r = res_find(s, req->url);
	if (!r || r->state == R_GIVEN) {
		soie_request q = *req;

		q.flags |= s->private;
		return net_request(NULL, &q, res);
	}
	while (r->state == R_PENDING) {
		p.fd = fetch_fd;
		p.events = POLLIN;
		poll(&p, 1, 1000);
		collect_jobs(s, 1);
		r = res_find(s, req->url);	/* the table may have moved */
	}
	if (r->state == R_FAILED)
		return -1;
	memset(res, 0, sizeof(*res));
	res->status = 200;
	res->url = strdup(req->url);
	res->headers = strdup("");
	if (!res->url || !res->headers)
		abort();
	res->len = r->len;
	if (r->kind == K_SCRIPT) {
		/* run once: no second copy here */
		res->body = r->data;
		r->data = NULL;
		r->state = R_GIVEN;
		return 0;
	}
	res->body = malloc(r->len + 1);
	if (!res->body)
		abort();
	memcpy(res->body, r->data, r->len);
	return 0;
}

/* Send a DOM event to the page; 1 when a script cancelled it. */
static int fire(struct shell *s, soie_node *target, const char *type, float x,
	float y, const char *key, int mods)
{
	soie_event ev;

	if (!s->page.doc)
		return 0;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.x = x;
	ev.y = y;
	ev.key = key;
	ev.mods = mods;
	return soie_script_event(s->page.doc, target, &ev);
}

static void layout_done(struct shell *s);
static void find_again(struct shell *s);

/* After scripts ran: their navigation, or their changes on screen. */
static int new_window(const char *target)
{
	return target && *target && strcasecmp(target, "_self") &&
		strcasecmp(target, "_top") && strcasecmp(target, "_parent");
}

int shell_may_open(struct shell *s, const char *url)
{
	return scheme_allowed(s->page.url, url);
}

/* The front opens url in a new window, when this page may have it */
static void open_window(struct shell *s, const char *url)
{
	if (!scheme_allowed(s->page.url, url)) {
		debug(1, "not from this page: %s", url);
		return;
	}
	debug(1, "new window %s", url);
	s->ui.open(s->ui_data, url);
}

static int after_scripts(struct shell *s)
{
	int moved = 0;

	/* a script's focus(): keys go there now */
	if (s->page.doc) {
		int c;
		soie_node *f = soie_get_focus(s->page.doc, &c);

		if (f != s->focus) {
			s->focus = f;
			s->caret = c;
			moved = 1;
		}
	}
	if (s->pending_open) {
		char *u = s->pending_open;

		s->pending_open = NULL;
		open_window(s, u);
		free(u);
	}
	if (s->pending_nav) {
		char *u = s->pending_nav;

		s->pending_nav = NULL;
		navigate_to(s, u, s->pending_replace);
		free(u);
		return 1;
	}
	if (s->page.doc && soie_document_update(s->page.doc)) {
		layout_done(s);
		return 1;
	}
	return moved;
}

/* Layout and drawing */

static int scripts_on(struct shell *s);

/* A private window's players: their requests use its jar */
static int private_request(void *data, const soie_request *req,
	soie_response *res)
{
	soie_request q = *req;

	(void)data;
	q.flags |= SOIE_REQ_PRIVATE;
	return net_request(NULL, &q, res);
}

static void relayout(struct shell *s)
{
	soie_style_options so;

	memset(&so, 0, sizeof(so));
	so.width = s->view_w;
	so.height = s->view_h;
	so.base_url = s->page.url;
	so.fetch = page_fetch;
	so.fetch_data = s;
	so.user_css = s->page.user_css;
	so.scripting = scripts_on(s);	/* @media (scripting), <noscript> */
	so.zoom = s->zoom;
	so.dark = s->dark;
	/* audio and video */
	so.request = s->private ? private_request : net_request;
	double t0 = now_s(), t1;

	s->in_style = 1;
	soie_style_document(s->page.doc, &so);
	s->in_style = 0;
	t1 = now_s();
	soie_layout_document(s->page.doc, s->view_w, s->view_h);
	debug(2, "relayout %dx%d: style %.3f s, layout %.3f s", s->view_w,
		s->view_h, t1 - t0, now_s() - t1);
	layout_done(s);
}

static void layout_done(struct shell *s)
{
	s->page.laid_out = now_s();
	s->page.height = soie_layout_height(s->page.doc);
	soie_set_focus(s->page.doc, s->focus, s->caret);
	clamp_scroll(s);
	set_title(s);
	find_again(s);
	s->sel.n = -1;
}

static void clamp_scroll(struct shell *s)
{
	int max = (int)s->page.height - s->view_h;

	if (s->page.scroll > max)
		s->page.scroll = max;
	if (s->page.scroll < 0)
		s->page.scroll = 0;
}
/* Navigation */

static char *escape_html(const char *s)
{
	size_t n = strlen(s), i, k = 0;
	char *o = malloc(n * 6 + 1);

	for (i = 0; i < n; i++) {
		const char *r = s[i] == '<' ? "&lt;" : s[i] == '&' ? "&amp;" :
			s[i] == '>' ? "&gt;" : NULL;

		if (r) {
			strcpy(o + k, r);
			k += strlen(r);
		} else {
			o[k++] = s[i];
		}
	}
	o[k] = '\0';
	return o;
}

static void set_title(struct shell *s)
{
	soie_node *t = find_tag(s->page.doc, "title");
	char title[512] = "";

	if (t)
		text_of(t, title, sizeof(title));
	if (!*title)
		snprintf(title, sizeof(title), "%s", s->page.url);
	/* at each layout: the front only hears of a new one */
	if (strcmp(title, s->title)) {
		memcpy(s->title, title, sizeof(title));
		s->ui.title(s->ui_data, title);
	}
}

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/*
 * <meta http-equiv=refresh content="5; url=next.html"> (HTML 4.2.5.3):
 * a delay, then the URL (the page itself when there is none).
 */
static void meta_refresh(struct shell *s)
{
	soie_node **v;
	size_t n, i;
	const char *c, *e;
	double delay;
	char *u;

	free(s->refresh_url);
	s->refresh_url = NULL;
	v = soie_query(s->page.doc, "meta[http-equiv=refresh i][content]", 0,
		&n);
	for (i = 0; v && i < n && !s->refresh_url; i++) {
		c = soie_element_get_attr(v[i], "content");
		while (*c == ' ' || *c == '\t' || *c == '\n')
			c++;
		if (!(*c >= '0' && *c <= '9') && *c != '.')
			continue;
		delay = strtod(c, (char **)&c);
		while (*c == ' ' || *c == '\t' || *c == '\n' || *c == ';' ||
		       *c == ',')
			c++;
		if (!strncasecmp(c, "url", 3)) {
			e = c + 3;
			while (*e == ' ' || *e == '\t')
				e++;
			if (*e == '=') {
				c = e + 1;
				while (*c == ' ' || *c == '\t')
					c++;
			}
		}
		u = strdup(c);
		if (*u == '\'' || *u == '"') {
			char q = *u, *end;

			memmove(u, u + 1, strlen(u));
			if ((end = strchr(u, q)))
				*end = 0;
		}
		if (!*u) {
			free(u);
			u = strdup(s->page.url);
		}
		s->refresh_url = soie_url_resolve(s->page.url, u);
		s->refresh_at = now_s() + (delay > 0 ? delay : 0);
		free(u);
	}
	free(v);
}

/* A due refresh becomes a navigation that replaces this page. */
static int refresh_due(struct shell *s)
{
	if (!s->refresh_url || now_s() < s->refresh_at || s->pending_nav)
		return 0;
	s->pending_nav = s->refresh_url;
	s->pending_replace = 1;
	s->refresh_url = NULL;
	return 1;
}

/* ms until the refresh, -1 without one */
static int refresh_wait(struct shell *s)
{
	double d;

	if (!s->refresh_url)
		return -1;
	d = (s->refresh_at - now_s()) * 1000;
	return d < 0 ? 0 : d > 1e9 ? 1000000000 : (int)d;
}

static void scroll_to_fragment(struct shell *s)
{
	const char *h = strchr(s->page.url, '#');
	soie_node *el;
	float x, y, w, hh;

	s->page.scroll = 0;
	if (h && h[1] && (el = find_id(s->page.doc, h + 1)) &&
	    !soie_element_rect(el, &x, &y, &w, &hh))
		s->page.scroll = (int)y;
	clamp_scroll(s);
}

static void first_render(struct shell *s);
static void start_scripts(struct shell *s);

static int scripts_on(struct shell *s)
{
	const struct shell_settings *st = settings_get();

	return s->scripts && (!st || st->scripts) &&
		soie_ext_get(SOIE_EXT_SCRIPT, 0) != NULL;
}

/* What the page needs, all asked for at once, sheets and scripts
 * first: the network works in parallel while we wait. */
static void prefetch(struct shell *s)
{
	static const char *const sel[] = {
		"link[rel~=stylesheet i][href]", "script[src]",
		"link[rel~=modulepreload i][href]",
		"link[rel~=preload i][as=script i][href]", "img[src]",
		"video[poster]", "input[type=image i][src]", NULL };
	static const int kind[] = { K_STYLE, K_SCRIPT, K_SCRIPT, K_SCRIPT,
		K_IMAGE, K_IMAGE, K_IMAGE };
	static const char *const attr[] = { "href", "src", "href", "href",
		"src", "poster", "src" };
	soie_node **v, *b;
	size_t n, i;
	char *base, *u;
	int k;

	v = soie_query(s->page.doc, "base[href]", 1, &n);
	b = v && n ? v[0] : NULL;
	base = b ? soie_url_resolve(s->page.url, soie_element_get_attr(b,
		"href")) : strdup(s->page.url);
	free(v);
	for (k = 0; sel[k]; k++) {
		if (kind[k] == K_SCRIPT && !scripts_on(s))
			continue;
		v = soie_query(s->page.doc, sel[k], 0, &n);
		for (i = 0; v && i < n; i++) {
			u = soie_url_resolve(base, soie_element_get_attr(v[i],
				attr[k]));
			if (u && strncmp(u, "data:", 5) && !res_find(s, u))
				res_add(s, u, kind[k]);
			free(u);
		}
		free(v);
	}
	free(base);
}

static void debug_js_start(struct shell *s);

/* The page's bytes arrived: parse them, ask for what it needs. */
static void doc_arrived(struct shell *s, struct job *j)
{
	soie_html_options ho = { NULL, 0 };
	struct response *r = &j->r;
	char *html = NULL;
	const char *src;
	size_t len;
	int push = s->page.push;

	if (j->ok)
		debug(1, "page %d %s, %zu bytes, %.2f s: %s", r->status, r->type,
			r->len, now_s() - s->page.t_load, r->url);
	else
		debug(1, "page failed, %s: %s", r->error, j->url);
	debug_page(j->ok ? r->url : j->url);
	drop_resources(s);
	s->focus = NULL;
	if (s->page.old)
		soie_node_free(s->page.old);
	s->page.old = s->page.doc;
	s->page.old_scroll = s->page.scroll;
	if (s->page.old)
		soie_script_stop(s->page.old);	/* no more events, timers */
	s->page.doc = NULL;
	if (!j->ok) {
		char *e = escape_html(r->error), *u = escape_html(j->url);
		char buf[9000];

		snprintf(s->page.url, sizeof(s->page.url), "%s", j->url);
		snprintf(buf, sizeof(buf), "<!DOCTYPE html><title>Error</title>"
			"<body style='font-family:sans-serif;margin:3em'>"
			"<h1>Cannot open this page</h1><p>%s</p><p>%s</p>", u,
			e);
		s->page.doc = soie_html_parse(buf, strlen(buf), NULL);
		free(e);
		free(u);
	} else {
		snprintf(s->page.url, sizeof(s->page.url), "%s", r->url);
		ho.charset = *r->charset ? r->charset : NULL;
		ho.scripting = scripts_on(s);
		src = r->body ? r->body : "";
		len = r->len;
		if (!strncmp(r->type, "image/", 6)) {
			char *u = escape_html(r->url);

			html = malloc(strlen(u) + 100);
			sprintf(html, "<body style='margin:0;background:#333'>"
				"<img src=\"%s\">", u);
			free(u);
			/* keep the bytes: the image comes from the cache */
			s->page.res = malloc(sizeof(*s->page.res));
			memset(s->page.res, 0, sizeof(*s->page.res));
			s->page.res[0].url = strdup(r->url);
			s->page.res[0].data = r->body;
			s->page.res[0].len = r->len;
			s->page.res[0].state = R_DONE;
			s->page.res[0].kind = K_IMAGE;
			s->page.nres = 1;
			r->body = NULL;
			src = html;
			len = strlen(html);
		} else if (!strncmp(r->type, "text/", 5) &&
			   strcmp(r->type, "text/html")) {
			char *t = escape_html(src);

			html = malloc(strlen(t) + 64);
			sprintf(html, "<pre style='white-space:pre-wrap'>%s", t);
			free(t);
			src = html;
			len = strlen(html);
			ho.charset = "UTF-8";
		}
		s->page.doc = soie_html_parse(src, len, &ho);
		debug_js_start(s);
		free(html);
	}
	free(s->page.user_css);
	s->page.user_css = soie_ext_user_css(NULL, s->page.url);
	soie_ext_document(s->page.doc, s->page.url);
	hist_note(s, push);
	s->page.scroll = 0;
	prefetch(s);
	s->page.loading = L_STYLE;
	if (!res_pending(s, K_STYLE))
		first_render(s);
}

/* The style sheets are here: lay out and show the page. */
static void first_render(struct shell *s)
{
	double t = now_s();

	s->page.restyle = s->page.relayout = 0;
	relayout(s);
	debug(1, "first paint %.2f s (layout %.3f s), %d resources asked",
		now_s() - s->page.t_load, now_s() - t, s->page.nres);
	if (s->page.old) {
		soie_node_free(s->page.old);
		s->page.old = NULL;
	}
	meta_refresh(s);
	scroll_to_fragment(s);
	if (!s->page.push && s->hist.cur >= 0 && s->hist.cur < s->hist.n)
		s->page.scroll = s->hist.scroll[s->hist.cur];
	clamp_scroll(s);
	set_title(s);
	s->ui.address(s->ui_data, s->page.url);
	/* none on the shell's own pages: nothing to run, nothing to
	 * reach about:settings from inside it */
	s->page.loading = scripts_on(s) && strcmp(s->page.url, HOME) &&
		!settings_url(s->page.url) ?
		L_SCRIPTS : L_NONE;
	if (s->page.loading == L_SCRIPTS && !res_pending(s, K_SCRIPT))
		start_scripts(s);
}

static void start_scripts(struct shell *s)
{
	double t = now_s();

	s->page.loading = L_NONE;
	debug(1, "scripts start");
	if (!soie_script_start(s->page.doc, s->page.url, &s->host))
		after_scripts(s);
	debug(1, "scripts ran %.2f s", now_s() - t);
}

static void dl_adopt(struct shell *s, struct download *d, const char *url,
	const char *headers);

/* push: 1 adds a history entry, 2 replaces the current one. The page
 * on screen stays until the new one arrives. */
static void load_start(struct shell *s, const char *url, int push)
{
	/* the load before: a download goes on, a page stops */
	if (s->page_dl) {
		if (s->page_dl->state != DL_NONE)
			dl_adopt(s, s->page_dl, s->page.want, NULL);
		else
			s->page_dl->cancel = 1;
		s->page_dl = NULL;
	}
	if (s->page.doc && push && s->hist.cur >= 0)
		s->hist.scroll[s->hist.cur] = s->page.scroll;
	s->page.t_load = now_s();
	s->page.done = 0;
	if (s->page.gen)
		fetch_drop(s->page.gen);
	s->page.gen = ++gens;
	s->page.push = push;
	free(s->page.want);
	s->page.want = strdup(url);
	free(s->refresh_url);
	s->refresh_url = NULL;
}

/* A page that is here already: html, at url */
static void load_here(struct shell *s, const char *url, const char *html)
{
	struct job j;

	memset(&j, 0, sizeof(j));
	j.url = (char *)url;
	j.ok = 1;
	snprintf(j.r.url, sizeof(j.r.url), "%s", url);
	strcpy(j.r.type, "text/html");
	strcpy(j.r.charset, "UTF-8");
	j.r.body = strdup(html);
	j.r.len = strlen(html);
	doc_arrived(s, &j);
	free(j.r.body);
}

static void load(struct shell *s, const char *url, const char *post, int push)
{
	int k;

	debug(1, "load%s %s", post ? " (POST)" : "", url);
	load_start(s, url, push);
	if (!strcmp(url, HOME)) {
		load_here(s, HOME, home_html);
		return;
	}
	if (settings_get() && settings_url(url)) {
		int changed;
		char *html = settings_page(post, &changed);

		load_here(s, SETTINGS_URL, html);
		free(html);
		if (changed && s->ui.settings)
			s->ui.settings(s->ui_data);
		return;
	}
	if ((k = scheme_of(url)) >= 0) {
		struct job j;

		scheme_answer(k, url, &j);
		doc_arrived(s, &j);
		free(j.r.body);
		return;
	}
	/* a body that is not a page streams to a file in the cache */
	{
		const char *dir = debug_dir();
		struct download *d = NULL;

		if (dir && (d = calloc(1, sizeof(*d)))) {
			d->fd = -1;
			snprintf(d->path, sizeof(d->path), "%s/dl-XXXXXX", dir);
		}
		s->page_dl = d;
		fetch_add(url, post, NULL, K_DOC, s->page.gen, 1, s->private,
			d);
		/* while the page comes, connections for its resources */
		fetch_preconnect(url, PRECONNECT, s->page.gen);
	}
	s->page.loading = L_DOC;
	s->ui.address(s->ui_data, url);
}

/*
 * Downloads: a page load whose response is not shown (net_shown()) is
 * adopted as a download: the page on screen stays, the front picks the
 * file (in ~/Downloads by default), the body streams there through a
 * file in the cache (net.c), or is written from memory at the end
 * (chunked, compressed bodies).
 */

/* The settings' folder, $XDG_DOWNLOAD_DIR, ~/Downloads (made), ~ */
static void dl_dir(char *out, size_t size)
{
	const struct shell_settings *st = settings_get();
	const char *d = getenv("XDG_DOWNLOAD_DIR"), *home = getenv("HOME");

	if (st && *st->downloads) {
		snprintf(out, size, "%s", st->downloads);
		if (!mkdir(out, 0755) || errno == EEXIST)
			return;
	}
	if (d && *d == '/') {
		snprintf(out, size, "%s", d);
		return;
	}
	snprintf(out, size, "%s/Downloads", home ? home : "/tmp");
	if (mkdir(out, 0755) && errno != EEXIST)
		snprintf(out, size, "%s", home ? home : "/tmp");
}

/* The file's name: Content-Disposition's filename, else the URL's last
 * segment; no directory, no leading dot, no control bytes. */
static void dl_name(const char *url, const char *headers, char *out,
	size_t size)
{
	const char *v = NULL, *p;
	size_t n = 0, i;

	if (headers && (p = strcasestr(headers, "filename="))) {
		v = p + 9;
		if (*v == '"')
			n = strcspn(++v, "\"\r\n");
		else
			n = strcspn(v, "; \r\n");
	}
	if (!n) {
		size_t end = strcspn(url, "?#");

		for (v = url + end; v > url && v[-1] != '/'; v--)
			;
		n = (size_t)(url + end - v);
	}
	if (n >= size)
		n = size - 1;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)v[i];

		out[i] = c < 0x20 || c == 0x7f || c == '/' || c == '\\' ?
			'_' : (char)c;
	}
	out[n] = '\0';
	while (*out == '.')
		memmove(out, out + 1, strlen(out));
	if (!*out)
		snprintf(out, size, "download");
}

/* dir/name, or dir/name-1.ext... when it is taken */
static void dl_free_path(const char *dir, const char *name, char *out,
	size_t size)
{
	const char *dot = strrchr(name, '.');
	int i, base = dot && dot != name ? (int)(dot - name) :
		(int)strlen(name);

	snprintf(out, size, "%s/%s", dir, name);
	for (i = 1; i < 1000 && !access(out, F_OK); i++)
		snprintf(out, size, "%s/%.*s-%d%s", dir, base, name, i,
			name + base);
}

void shell_downloads(void (*progress)(void *data, unsigned id,
	long long got, long long total), void (*done)(void *data,
	unsigned id, const char *error), void *data)
{
	dl_ui.progress = progress;
	dl_ui.done = done;
	dl_ui.data = data;
}

static struct sdl *dl_of(const struct download *d)
{
	struct sdl *x;

	for (x = downloads; d && x && x->d != d; x = x->next)
		;
	return d ? x : NULL;
}

/* The page load d (or a finished one, d NULL) is a download: the
 * front tells where it goes; the page on screen stays. */
static struct sdl *dl_start(struct shell *s, struct download *d,
	const char *url, const char *headers, const char *type,
	long long total)
{
	struct sdl *x = calloc(1, sizeof(*x));
	char dir[4096], name[512];

	if (!x)
		abort();
	x->id = ++dl_ids;
	x->s = s;
	x->d = d;
	x->url = strdup(url ? url : "");
	x->told = -1;
	dl_dir(dir, sizeof(dir));
	dl_name(x->url, headers, name, sizeof(name));
	dl_free_path(dir, name, x->dest, sizeof(x->dest));
	x->next = downloads;
	downloads = x;
	if (s->page_dl == d)
		s->page_dl = NULL;
	/* the page that was there stays, its address too */
	if (s->page.loading == L_DOC)
		s->page.loading = L_NONE;
	s->ui.address(s->ui_data, s->page.url);
	debug(1, "download %u: %s -> %s", x->id, x->url, x->dest);
	if (s->ui.download) {
		s->ui.download(s->ui_data, x->id, x->url, type, total, x->dest,
			sizeof(x->dest));
	} else {
		char msg[4200];

		snprintf(msg, sizeof(msg), "Downloading to %s", x->dest);
		s->ui.alert(s->ui_data, msg);
	}
	if (!*x->dest && d)
		d->cancel = 1;
	return x;
}

static void dl_adopt(struct shell *s, struct download *d, const char *url,
	const char *headers)
{
	const char *h = headers ? headers : d->headers;
	char type[128] = "";
	const char *v = h ? strcasestr(h, "content-type:") : NULL;

	if (dl_of(d))
		return;
	if (v) {
		v += 13;
		while (*v == ' ')
			v++;
		snprintf(type, sizeof(type), "%.*s",
			(int)strcspn(v, "; \r\n"), v);
	}
	dl_start(s, d, url, h, type, d->total);
}

/* Copy a file, for a destination on another file system */
static int copy_file(const char *from, const char *to)
{
	char buf[65536];
	int in = open(from, O_RDONLY), out, ok = 1;
	ssize_t n;

	if (in < 0)
		return -1;
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) {
			ok = 0;
			break;
		}
	if (n < 0)
		ok = 0;
	close(in);
	if (close(out) || !ok) {
		unlink(to);
		return -1;
	}
	unlink(from);
	return 0;
}

/* Its job came: the file goes to its place, the front is told. */
static void dl_finish(struct sdl *x, struct job *j)
{
	struct download *d = j->r.dl;
	const char *err = NULL;
	struct sdl **p;

	if (!*x->dest) {
		err = "cancelled";
	} else if (!j->ok) {
		err = *j->r.error ? j->r.error : "failed";
	} else if (d && d->state == DL_DONE) {
		if (rename(d->path, x->dest) && (errno != EXDEV ||
		    copy_file(d->path, x->dest)))
			err = strerror(errno);
		else
			*d->path = '\0';	/* taken */
	} else if (d && d->state != DL_NONE) {
		err = "failed";
	} else {
		/* in memory: chunked, compressed */
		FILE *f = fopen(x->dest, "wb");

		if (!f || fwrite(j->r.body ? j->r.body : "", 1, j->r.len, f) !=
		    j->r.len)
			err = strerror(errno ? errno : EIO);
		if (f && fclose(f) && !err)
			err = strerror(errno);
		if (err)
			unlink(x->dest);
	}
	debug(1, "download %u %s: %s", x->id, err ? err : "done", x->dest);
	if (dl_ui.done) {
		dl_ui.done(dl_ui.data, x->id, err);
	} else if (x->s) {
		char msg[4200];

		snprintf(msg, sizeof(msg), err ? "Download failed: %s" :
			"Saved to %s", err ? err : x->dest);
		x->s->ui.alert(x->s->ui_data, msg);
	}
	for (p = &downloads; *p && *p != x; p = &(*p)->next)
		;
	if (*p)
		*p = x->next;
	free(x->url);
	free(x);
}

/* How far the downloads are, told every quarter of a second */
static void dl_progress(struct shell *s)
{
	struct sdl *x;
	double t = now_s();

	for (x = downloads; x; x = x->next) {
		long long got = x->d ? x->d->got : 0;

		if (!dl_ui.progress || got == x->told ||
		    t - x->told_at < 0.25)
			continue;
		x->told = got;
		x->told_at = t;
		dl_ui.progress(dl_ui.data, x->id, got,
			x->d ? x->d->total : -1);
	}
	/* the page load streams: it is a download */
	if (s->page_dl && s->page_dl->state != DL_NONE)
		dl_adopt(s, s->page_dl, s->page.want, NULL);
}

void shell_download_cancel(unsigned id)
{
	struct sdl *x;

	for (x = downloads; x && x->id != id; x = x->next)
		;
	if (!x)
		return;
	*x->dest = '\0';
	if (x->d)
		x->d->cancel = 1;
}

/* The page's load, finished: a page, or a download after all */
static void doc_or_download(struct shell *s, struct job *j)
{
	struct download *d = j->r.dl;
	struct sdl *x;

	if (d && d == s->page_dl)
		s->page_dl = NULL;
	if ((d && d->state != DL_NONE) || (j->ok && j->r.status / 100 == 2 &&
	    j->r.headers && !net_shown(j->r.headers))) {
		const char *url = j->ok && *j->r.url ? j->r.url : j->url;

		x = d && d->state != DL_NONE ? (dl_adopt(s, d, url, NULL),
			dl_of(d)) : dl_start(s, d, url, j->r.headers,
			j->r.type, (long long)j->r.len);
		if (x)
			dl_finish(x, j);
		return;
	}
	doc_arrived(s, j);
}

/* The finished jobs to the inboxes of the shells whose page asked for
 * them: gens are unique, a job no page has now is dropped. */
void shell_route(void)
{
	struct job *j, **in;
	struct shell *s;

	while ((j = fetch_done())) {
		struct sdl *x = dl_of(j->r.dl);

		/* a download, whatever its shell does now */
		if (x) {
			dl_finish(x, j);
			job_free(j);
			continue;
		}
		for (s = shells; s && s->page.gen != j->gen; s = s->next)
			;
		if (!s) {
			job_free(j);
			continue;
		}
		for (in = &s->inbox; *in; in = &(*in)->next)
			;
		j->next = NULL;
		*in = j;
	}
}

static struct job *next_job(struct shell *s)
{
	struct job *j;

	shell_route();
	if ((j = s->inbox))
		s->inbox = j->next;
	return j;
}

/* Finished jobs. nested: from inside a script (a script waiting for
 * a resource): a new page waits for the main loop. */
static void collect_jobs(struct shell *s, int nested)
{
	struct job *j;
	struct res *r;

	if (!nested && s->deferred) {
		j = s->deferred;
		s->deferred = NULL;
		if (j->gen == s->page.gen)
			doc_or_download(s, j);
		job_free(j);
	}
	while ((j = next_job(s))) {
		if (j->gen != s->page.gen) {
			job_free(j);
			continue;
		}
		if (j->kind == K_DOC) {
			if (nested) {
				job_free(s->deferred);
				s->deferred = j;
				continue;
			}
			doc_or_download(s, j);
			job_free(j);
			continue;
		}
		r = res_find(s, j->url);
		if (r && r->state == R_PENDING) {
			if (j->ok && j->r.status < 400) {
				r->data = j->r.body;
				r->len = j->r.len;
				j->r.body = NULL;
				r->state = R_DONE;
			} else {
				r->state = R_FAILED;
				if (j->ok)
					debug(1, "failed, HTTP %d: %s", j->r.status,
						j->url);
				else
					debug(1, "failed, %s: %s", j->r.error,
						j->url);
			}
			/* a style sheet or a font, whoever asked (a restyle
			 * from a script is not in_style): copied to the
			 * engine each time, never given, or every restyle
			 * would fetch it again */
			if (!strcmp(j->r.type, "text/css") ||
			    strstr(j->r.type, "font"))
				r->kind = K_STYLE;
			if (r->kind == K_STYLE)
				s->page.restyle = 1;
			else
				s->page.relayout = 1;
		}
		job_free(j);
	}
	if (nested || !s->page.doc)
		return;
	if (s->page.loading == L_STYLE && !res_pending(s, K_STYLE))
		first_render(s);
	else if (s->page.loading == L_SCRIPTS && !res_pending(s, K_SCRIPT))
		start_scripts(s);
}

/* Late style sheets restyle, late images lay out again, at most every
 * 200 ms; returns ms until it should be called again, -1: nothing to
 * do. *repaint is set when it laid out. */
static int apply_late(struct shell *s, int *repaint)
{
	double wait;

	if (s->page.loading == L_DOC || s->page.loading == L_STYLE ||
	    !s->page.doc || (!s->page.restyle && !s->page.relayout))
		return -1;
	wait = 0.2 - (now_s() - s->page.laid_out);
	if (wait > 0 && res_pending(s, -1))
		return (int)(wait * 1000) + 1;
	if (s->page.restyle) {
		relayout(s);
	} else {
		soie_layout_document(s->page.doc, s->view_w, s->view_h);
		layout_done(s);
	}
	s->page.restyle = s->page.relayout = 0;
	*repaint = 1;
	s->damage_full = 1;
	return -1;
}

static void go_history(struct shell *s, int delta)
{
	int to = s->hist.cur + delta;

	if (to < 0 || to >= s->hist.n)
		return;
	s->hist.scroll[s->hist.cur] = s->page.scroll;
	s->hist.cur = to;
	load(s, s->hist.url[to], NULL, 0);
}

static void navigate(struct shell *s, const char *href)
{
	if (!href || !strncasecmp(href, "javascript:", 11))
		return;
	navigate_to(s, href, 0);
}

static void navigate_to(struct shell *s, const char *href, int replace)
{
	char *abs = soie_url_resolve(s->page.url, href);

	if (!abs || !scheme_allowed(s->page.url, abs)) {
		debug(1, "not from this page: %s", abs ? abs : href);
		free(abs);
		return;
	}

	/* same document, other fragment: just scroll */
	{
		const char *h = strchr(abs, '#');
		size_t base = strcspn(s->page.url, "#");

		if (h && (size_t)(h - abs) == base &&
		    !strncmp(abs, s->page.url, base)) {
			soie_node *el = find_id(s->page.doc, h + 1);
			float x, y, w, hh;

			snprintf(s->page.url, sizeof(s->page.url), "%s", abs);
			hist_note(s, replace ? 2 : 1);
			s->ui.address(s->ui_data, s->page.url);
			if (el && !soie_element_rect(el, &x, &y, &w, &hh)) {
				s->page.scroll = (int)y;
				clamp_scroll(s);
			}
			free(abs);
			return;
		}
	}
	load(s, abs, NULL, replace ? 2 : 1);
	free(abs);
}

/* Forms (HTML 4.10.21, the urlencoded part) */

static void urlencode(char **out, size_t *len, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";

	/* room for the worst: every byte as %XX */
	*out = realloc(*out, *len + strlen(s) * 3 + 1);
	(*out)[*len] = '\0';
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '*' || c == '-' ||
		    c == '.' || c == '_') {
			(*out)[(*len)++] = (char)c;
		} else if (c == ' ') {
			(*out)[(*len)++] = '+';
		} else {
			(*out)[(*len)++] = '%';
			(*out)[(*len)++] = hex[c >> 4];
			(*out)[(*len)++] = hex[c & 15];
		}
		(*out)[*len] = '\0';
	}
}

static void pair(char **q, size_t *len, const char *name, const char *value)
{
	if (*len) {
		*q = realloc(*q, *len + 2);
		(*q)[(*len)++] = '&';
		(*q)[*len] = '\0';
	}
	urlencode(q, len, name);
	*q = realloc(*q, *len + 2);
	(*q)[(*len)++] = '=';
	(*q)[*len] = '\0';
	urlencode(q, len, value);
}

static void collect(soie_node *n, soie_node *submitter, char **q, size_t *len)
{
	soie_node *c;

	for (c = soie_node_first_child(n); c; c = soie_node_next_sibling(c)) {
		const char *tag, *name, *type, *v;

		if (soie_node_get_type(c) != SOIE_ELEMENT)
			continue;
		tag = soie_element_name(c);
		name = soie_element_get_attr(c, "name");
		if (name && !soie_element_get_attr(c, "disabled")) {
			type = soie_element_get_attr(c, "type");
			v = soie_element_get_attr(c, "value");
			if (!strcmp(tag, "input")) {
				if (type && (!strcasecmp(type, "checkbox") ||
					     !strcasecmp(type, "radio"))) {
					if (soie_element_get_attr(c, "checked"))
						pair(q, len, name, v ? v : "on");
				} else if (type && (!strcasecmp(type, "submit") ||
						    !strcasecmp(type, "image") ||
						    !strcasecmp(type, "button") ||
						    !strcasecmp(type, "reset"))) {
					if (c == submitter)
						pair(q, len, name, v ? v : "");
				} else if (!type || strcasecmp(type, "file")) {
					pair(q, len, name, v ? v : "");
				}
			} else if (!strcmp(tag, "button")) {
				if (c == submitter)
					pair(q, len, name, v ? v : "");
			} else if (!strcmp(tag, "textarea")) {
				char t[16384] = "";

				if (v)
					snprintf(t, sizeof(t), "%s", v);
				else
					text_of(c, t, sizeof(t));
				pair(q, len, name, t);
			} else if (!strcmp(tag, "select")) {
				soie_node *o, *pick = NULL;

				for (o = find_tag(c, "option"); o;
				     o = soie_node_next_sibling(o))
					if (soie_node_get_type(o) == SOIE_ELEMENT &&
					    (!pick || soie_element_get_attr(o,
						    "selected")))
						pick = o;
				if (pick) {
					char t[1024] = "";

					v = soie_element_get_attr(pick, "value");
					if (!v)
						text_of(pick, t, sizeof(t));
					pair(q, len, name, v ? v : t);
				}
			}
		}
		if (strcmp(tag, "select") && strcmp(tag, "textarea"))
			collect(c, submitter, q, len);
	}
}

static void submit(struct shell *s, soie_node *form, soie_node *submitter)
{
	const char *action, *method;
	char *q, *abs, *url;
	size_t len = 0;

	if (fire(s, form, "submit", 0, 0, NULL, 0) || s->pending_nav)
		return;
	action = soie_element_get_attr(form, "action");
	method = soie_element_get_attr(form, "method");
	q = calloc(1, 1);
	collect(form, submitter, &q, &len);
	abs = soie_url_resolve(s->page.url, action && *action ? action :
		s->page.url);
	if (!abs || !scheme_allowed(s->page.url, abs)) {
		debug(1, "not from this page: %s", abs ? abs : "a form");
		free(abs);
		free(q);
		return;
	}
	if (method && !strcasecmp(method, "post")) {
		load(s, abs, q, 1);
	} else {
		size_t n = strcspn(abs, "?#");

		url = malloc(n + len + 2);
		memcpy(url, abs, n);
		url[n] = '?';
		memcpy(url + n + 1, q, len + 1);
		load(s, url, NULL, 1);
		free(url);
	}
	free(abs);
	free(q);
}

/* Input */

static int is_text_control(soie_node *n)
{
	const char *t;

	if (!n || soie_node_get_type(n) != SOIE_ELEMENT)
		return 0;
	if (!strcmp(soie_element_name(n), "textarea"))
		return 1;
	if (strcmp(soie_element_name(n), "input"))
		return 0;
	t = soie_element_get_attr(n, "type");
	return !t || !strcasecmp(t, "text") || !strcasecmp(t, "search") ||
		!strcasecmp(t, "email") || !strcasecmp(t, "url") ||
		!strcasecmp(t, "password") || !strcasecmp(t, "number") ||
		!strcasecmp(t, "tel");
}

static void set_page_focus(struct shell *s, soie_node *n, int c)
{
	soie_node *old = s->focus;

	s->focus = n;
	s->caret = c;
	if (s->page.doc)
		soie_set_focus(s->page.doc, s->focus, s->caret);
	if (old != n) {
		if (old)
			fire(s, old, "blur", 0, 0, NULL, 0);
		if (n)
			fire(s, n, "focus", 0, 0, NULL, 0);
	}
}

/* A control changed by the user: input and change events. */
static void changed(struct shell *s, soie_node *n)
{
	fire(s, n, "input", 0, 0, NULL, 0);
	fire(s, n, "change", 0, 0, NULL, 0);
}

static void click(struct shell *s, int x, int y, int mods)
{
	soie_node *n = s->page.doc ? soie_hit_test(s->page.doc, (float)x,
		(float)(y + s->page.scroll)) : NULL, *a, *form;
	const char *t;
	float cy = (float)y;

	if (n) {
		fire(s, n, "mousedown", (float)x, cy, NULL, mods);
		fire(s, n, "mouseup", (float)x, cy, NULL, mods);
		if (fire(s, n, "click", (float)x, cy, NULL, mods) ||
		    s->pending_nav)
			return;
		/* play, pause, seek */
		if (((a = ancestor(n, "video")) || (a = ancestor(n, "audio"))) &&
		    soie_media_click(a, (float)x, (float)(y + s->page.scroll)))
			return;
	}
	if (is_text_control(n)) {
		const char *v = soie_element_get_attr(n, "value");

		set_page_focus(s, n, v ? (int)strlen(v) : 0);
		return;
	}
	set_page_focus(s, NULL, 0);
	if (!n)
		return;
	form = ancestor(n, "form");
	if (!strcmp(soie_element_name(n), "input")) {
		t = soie_element_get_attr(n, "type");
		if (t && !strcasecmp(t, "checkbox")) {
			if (soie_element_get_attr(n, "checked"))
				soie_element_remove_attr(n, "checked");
			else
				soie_element_set_attr(n, "checked", "");
			changed(s, n);
			relayout(s);
			return;
		}
		if (t && !strcasecmp(t, "radio")) {
			const char *name = soie_element_get_attr(n, "name");
			soie_node *in = form ? form : s->page.doc, *o;

			for (o = in; o; ) {
				if (soie_node_get_type(o) == SOIE_ELEMENT &&
				    !strcmp(soie_element_name(o), "input") &&
				    name && soie_element_get_attr(o, "name") &&
				    !strcmp(soie_element_get_attr(o, "name"),
					    name))
					soie_element_remove_attr(o, "checked");
				if (soie_node_first_child(o)) {
					o = soie_node_first_child(o);
					continue;
				}
				while (o && o != in && !soie_node_next_sibling(o))
					o = soie_node_parent(o);
				o = o && o != in ? soie_node_next_sibling(o) : NULL;
			}
			soie_element_set_attr(n, "checked", "");
			changed(s, n);
			relayout(s);
			return;
		}
		if (t && (!strcasecmp(t, "submit") || !strcasecmp(t, "image")) &&
		    form) {
			submit(s, form, n);
			return;
		}
	}
	if (!strcmp(soie_element_name(n), "select")) {
		/* cycle through the options */
		soie_node *o, *cur = NULL, *next = NULL, *first = NULL;

		for (o = find_tag(n, "option"); o; o = soie_node_next_sibling(o)) {
			if (soie_node_get_type(o) != SOIE_ELEMENT)
				continue;
			if (!first)
				first = o;
			if (cur && !next)
				next = o;
			if (soie_element_get_attr(o, "selected"))
				cur = o;
		}
		if (cur)
			soie_element_remove_attr(cur, "selected");
		if (!next)
			next = cur ? first : (first ? soie_node_next_sibling(
				first) : NULL);
		if (next)
			soie_element_set_attr(next, "selected", "");
		changed(s, n);
		relayout(s);
		return;
	}
	if ((a = ancestor(n, "button")) && form) {
		t = soie_element_get_attr(a, "type");
		if (!t || !strcasecmp(t, "submit"))
			submit(s, form, a);
		return;
	}
	if ((a = ancestor(n, "a")) && soie_element_get_attr(a, "href")) {
		const char *h = soie_element_get_attr(a, "href");
		char *abs;

		if (s->ui.open && new_window(soie_element_get_attr(a,
		    "target")) && strncasecmp(h, "javascript:", 11) &&
		    (abs = soie_url_resolve(s->page.url, h))) {
			open_window(s, abs);
			free(abs);
		} else {
			navigate(s, h);
		}
	}
}


void shell_click(struct shell *s, int x, int y, int mods)
{
	debug(2, "click %d,%d (scroll %d)", x, y, s->page.scroll);
	s->in_input++;
	click(s, x, y, mods);
	after_scripts(s);
	s->in_input--;
	clamp_scroll(s);
}

int shell_edit(soie_node *el, int *cp, const char *key, const char *text)
{
	const char *v = soie_element_get_attr(el, "value");
	char buf[16384];
	size_t len = v ? strlen(v) : 0, n = text ? strlen(text) : 0;
	int c = *cp;

	if (!v)
		v = "";
	if (c > (int)len)
		c = (int)len;
	snprintf(buf, sizeof(buf), "%s", v);
	if (!strcmp(key, "Backspace")) {
		int k = c - 1;

		if (c == 0)
			return 0;
		while (k > 0 && ((unsigned char)buf[k] & 0xc0) == 0x80)
			k--;
		memmove(buf + k, buf + c, len - (size_t)c + 1);
		c = k;
	} else if (!strcmp(key, "Delete")) {
		int k = c + 1;

		if (c >= (int)len)
			return 0;
		while (k < (int)len && ((unsigned char)buf[k] & 0xc0) == 0x80)
			k++;
		memmove(buf + c, buf + k, len - (size_t)k + 1);
	} else if (!strcmp(key, "ArrowLeft") && c > 0) {
		c--;
		while (c > 0 && ((unsigned char)buf[c] & 0xc0) == 0x80)
			c--;
	} else if (!strcmp(key, "ArrowRight") && c < (int)len) {
		c++;
		while (c < (int)len && ((unsigned char)buf[c] & 0xc0) == 0x80)
			c++;
	} else if (!strcmp(key, "Home")) {
		c = 0;
	} else if (!strcmp(key, "End")) {
		c = (int)len;
	} else if (n > 0 && (unsigned char)text[0] >= 0x20 &&
		   text[0] != 0x7f && len + n < sizeof(buf) - 1) {
		memmove(buf + c + n, buf + c, len - (size_t)c + 1);
		memcpy(buf + c, text, n);
		c += (int)n;
	} else {
		return 0;
	}
	soie_element_set_attr(el, "value", buf);
	*cp = c;
	return 1;
}

static int key_in(struct shell *s, const char *key, const char *text, int mods)
{
	int r = 2;

	debug(2, "key %s%s%s%s", mods & SOIE_MOD_CTRL ? "Ctrl+" : "",
		mods & SOIE_MOD_ALT ? "Alt+" : "", mods & SOIE_MOD_SHIFT ?
		"Shift+" : "", key);
	if (s->page.doc && (fire(s, s->focus ? s->focus :
	    find_tag(s->page.doc, "body"), "keydown", 0, 0, key, mods) ||
	    s->pending_nav)) {
		after_scripts(s);
		return 1;
	}
	if (s->focus) {
		if (!strcmp(key, "Enter")) {
			soie_node *form = ancestor(s->focus, "form");

			if (form && strcmp(soie_element_name(s->focus),
			    "textarea")) {
				submit(s, form, NULL);
				after_scripts(s);
				return 1;
			}
		}
		if (!strcmp(key, "Escape")) {
			set_page_focus(s, NULL, 0);
		} else if (shell_edit(s->focus, &s->caret, key, text)) {
			soie_set_focus(s->page.doc, s->focus, s->caret);
			if (strncmp(key, "Arrow", 5) && strcmp(key, "Home") &&
			    strcmp(key, "End"))
				fire(s, s->focus, "input", 0, 0, NULL, 0);
		} else if (mods & (SOIE_MOD_CTRL | SOIE_MOD_ALT |
		    SOIE_MOD_META)) {
			/* a shortcut the field does not know: the front's */
			after_scripts(s);
			return 0;
		}
		after_scripts(s);
		return 1;
	}
	/* with Ctrl, Alt or Meta a key is the front's shortcut (Ctrl+Page
	 * Up a tab), but Ctrl+Home and Ctrl+End */
	if ((mods & (SOIE_MOD_CTRL | SOIE_MOD_ALT | SOIE_MOD_META)) &&
	    strcmp(key, "Home") && strcmp(key, "End"))
		r = 0;
	else if (!strcmp(key, "ArrowDown"))
		s->page.scroll += 40;
	else if (!strcmp(key, "ArrowUp"))
		s->page.scroll -= 40;
	else if (!strcmp(key, "PageDown") || !strcmp(key, " "))
		s->page.scroll += s->view_h - 40;
	else if (!strcmp(key, "PageUp"))
		s->page.scroll -= s->view_h - 40;
	else if (!strcmp(key, "Home"))
		s->page.scroll = 0;
	else if (!strcmp(key, "End"))
		s->page.scroll = (int)s->page.height;
	else if (!strcmp(key, "Backspace")) {
		go_history(s, -1);
		r = 1;
	} else {
		r = 0;
	}
	if (after_scripts(s) && r)
		r = 1;
	clamp_scroll(s);
	return r;
}

int shell_key(struct shell *s, const char *key, const char *text, int mods)
{
	int r;

	s->in_input++;
	r = key_in(s, key, text, mods);
	s->in_input--;
	return r;
}

int shell_hover(struct shell *s, int x, int y, char *link, size_t size)
{
	soie_node *n = s->page.doc ? soie_hit_test(s->page.doc, (float)x,
		(float)(y + s->page.scroll)) : NULL, *a;

	*link = '\0';
	if ((a = ancestor(n, "a")) && soie_element_get_attr(a, "href")) {
		char *abs = soie_url_resolve(s->page.url,
			soie_element_get_attr(a, "href"));

		snprintf(link, size, "%s", abs);
		free(abs);
		return SHELL_HAND;
	}
	return is_text_control(n) ? SHELL_TEXT : SHELL_ARROW;
}

int shell_hit(struct shell *s, int x, int y, char *link, char *image,
	size_t size)
{
	soie_node *n = s->page.doc ? soie_hit_test(s->page.doc, (float)x,
		(float)(y + s->page.scroll)) : NULL, *a;
	const char *v;
	char *abs;

	*link = *image = '\0';
	if ((a = ancestor(n, "a")) && (v = soie_element_get_attr(a, "href")) &&
	    strncasecmp(v, "javascript:", 11) &&
	    (abs = soie_url_resolve(s->page.url, v))) {
		snprintf(link, size, "%s", abs);
		free(abs);
	}
	if ((a = ancestor(n, "img")) && (v = soie_element_get_attr(a, "src")) &&
	    (abs = soie_url_resolve(s->page.url, v))) {
		snprintf(image, size, "%s", abs);
		free(abs);
	}
	return is_text_control(n);
}

void shell_scroll(struct shell *s, int dy)
{
	debug(3, "scroll %+d", dy);
	s->page.scroll += dy;
	clamp_scroll(s);
}

void shell_scroll_to(struct shell *s, int y)
{
	s->page.scroll = y;
	clamp_scroll(s);
}

void shell_view(struct shell *s, int *scroll, int *height, int *view)
{
	*scroll = s->page.scroll;
	*height = (int)s->page.height;
	*view = s->view_h;
}

void shell_blur(struct shell *s)
{
	set_page_focus(s, NULL, 0);
}

int shell_paste(struct shell *s, const char *text)
{
	const char *v;
	char *buf, *o;
	size_t len;
	int multi;

	if (!s->focus)
		return 0;
	v = soie_element_get_attr(s->focus, "value");
	if (!v)
		v = "";
	len = strlen(v);
	if (s->caret > (int)len)
		s->caret = (int)len;
	multi = !strcmp(soie_element_name(s->focus), "textarea");
	buf = malloc(len + strlen(text) + 1);
	if (!buf)
		abort();
	memcpy(buf, v, (size_t)s->caret);
	o = buf + s->caret;
	for (; *text; text++) {
		if (*text == '\r' && multi)
			continue;
		if (*text == '\n' || *text == '\r')
			/* one line inputs: line breaks become spaces */
			*o++ = multi ? '\n' : ' ';
		else if ((unsigned char)*text >= 0x20 || *text == '\t')
			*o++ = *text;
	}
	strcpy(o, v + s->caret);
	s->caret = (int)(o - buf);
	soie_element_set_attr(s->focus, "value", buf);
	free(buf);
	soie_set_focus(s->page.doc, s->focus, s->caret);
	fire(s, s->focus, "input", 0, 0, NULL, 0);
	after_scripts(s);
	return 1;
}

const char *shell_field(struct shell *s)
{
	const char *v;

	if (!s->focus)
		return NULL;
	v = soie_element_get_attr(s->focus, "value");
	return v ? v : "";
}

/* Find */

/* The current match again, after a layout or a new page. */
static void find_again(struct shell *s)
{
	s->found.n = 0;
	if (!s->found.text || !s->page.doc)
		return;
	if (s->found.cur < 1)
		s->found.cur = 1;
	s->found.n = soie_find(s->page.doc, s->found.text, s->found.cur - 1,
		&s->found.x, &s->found.y, &s->found.w, &s->found.h);
	if (s->found.n && s->found.cur > s->found.n) {
		s->found.cur = s->found.n;
		soie_find(s->page.doc, s->found.text, s->found.cur - 1,
			&s->found.x, &s->found.y, &s->found.w, &s->found.h);
	}
}

int shell_find(struct shell *s, const char *text, int dir, int *cur)
{
	*cur = 0;
	if (!text || !*text) {
		free(s->found.text);
		s->found.text = NULL;
		s->found.n = s->found.cur = 0;
		return 0;
	}
	if (!s->found.text || strcmp(s->found.text, text)) {
		free(s->found.text);
		s->found.text = strdup(text);
		s->found.cur = 1;
		dir = 1;	/* show it */
	} else if (dir && s->found.n) {
		s->found.cur += dir;
		if (s->found.cur < 1)
			s->found.cur = s->found.n;
		if (s->found.cur > s->found.n)
			s->found.cur = 1;
	}
	find_again(s);
	if (!s->found.n)
		return 0;
	*cur = s->found.cur;
	if (dir && (s->found.y < (float)s->page.scroll ||
	    s->found.y + s->found.h > (float)(s->page.scroll + s->view_h))) {
		s->page.scroll = (int)s->found.y - s->view_h / 3;
		clamp_scroll(s);
	}
	return s->found.n;
}

/* Selection */

/* The rects of the selection, found again after a layout; 0 without
 * one */
static int sel_rects(struct shell *s)
{
	int n;

	if (!s->sel.doc || s->sel.doc != s->page.doc)
		return 0;
	if (s->sel.n >= 0)
		return s->sel.n;
	n = soie_select(s->page.doc, s->sel.pt, s->sel.unit, s->sel.r,
		s->sel.cap);
	if (n > s->sel.cap) {
		s->sel.cap = n;
		s->sel.r = realloc(s->sel.r, (size_t)n * 4 * sizeof(float));
		if (!s->sel.r) {
			s->sel.cap = 0;
			return 0;
		}
		n = soie_select(s->page.doc, s->sel.pt, s->sel.unit, s->sel.r,
			s->sel.cap);
	}
	return s->sel.n = n;
}

int shell_select_start(struct shell *s, int x, int y, int unit)
{
	char link[16];
	int had = sel_rects(s) > 0;

	s->sel.doc = NULL;
	/* a press on a link or a field is theirs */
	if (!s->page.doc || shell_hover(s, x, y, link, sizeof(link)) !=
	    SHELL_ARROW)
		return had;
	s->sel.doc = s->page.doc;
	s->sel.pt[0] = s->sel.pt[2] = (float)x;
	s->sel.pt[1] = s->sel.pt[3] = (float)(y + s->page.scroll);
	s->sel.unit = unit;
	s->sel.n = -1;
	return had || sel_rects(s) > 0;
}

int shell_select_to(struct shell *s, int x, int y)
{
	if (!s->sel.doc || s->sel.doc != s->page.doc)
		return 0;
	if (s->sel.pt[2] == (float)x && s->sel.pt[3] == (float)(y +
	    s->page.scroll))
		return 0;
	s->sel.pt[2] = (float)x;
	s->sel.pt[3] = (float)(y + s->page.scroll);
	s->sel.n = -1;
	return 1;
}

void shell_select_all(struct shell *s)
{
	s->sel.doc = s->page.doc;
	s->sel.unit = SOIE_SELECT_ALL;
	s->sel.n = -1;
}

int shell_select_clear(struct shell *s)
{
	int had = sel_rects(s) > 0;

	s->sel.doc = NULL;
	return had;
}

char *shell_selection(struct shell *s)
{
	if (!sel_rects(s))
		return NULL;
	return soie_select_text(s->page.doc, s->sel.pt, s->sel.unit);
}

/* The loop */

int shell_tick(struct shell *s, int *repaint)
{
	/* media events first: timers their listeners set count below */
	int r = 0, mt = s->page.doc ? soie_media_tick_rect(s->page.doc, &r,
		s->page.dirty) : -1;
	double t0 = now_s(), t1;
	int t = s->page.doc ? soie_script_tick(s->page.doc) : -1, m;
	soie_node *d = s->page.doc;
	int l = s->page.loading, n = res_pending(s, -1);

	/* what keeps the window from answering */
	if ((t1 = now_s()) - t0 > 0.1)
		debug(1, "busy %.2f s: timers and answers of scripts", t1 - t0);
	*repaint = r;
	if (mt >= 0 && (t < 0 || mt < t))
		t = mt;
	/* a new page, a new stage, a resource: show it */
	collect_jobs(s, 0);
	if (d != s->page.doc || l != s->page.loading ||
	    n != res_pending(s, -1)) {
		/* a resource alone changes no pixel until apply_late()
		 * restyles or lays out with it: the loading line only */
		*repaint = 1;
		if (d != s->page.doc || l != s->page.loading)
			s->damage_full = 1;
		/* scripts may have started: their timers, asked above,
		 * are asked again at once */
		t = 0;
	}
	m = apply_late(s, repaint);
	if (m >= 0 && (t < 0 || m < t))
		t = m;
	m = refresh_wait(s);
	if (m >= 0 && (t < 0 || m < t))
		t = m;
	dl_progress(s);
	/* downloads on their way: told as they go */
	if ((downloads || s->page_dl) && (t < 0 || t > 250))
		t = 250;
	refresh_due(s);
	t1 = now_s();
	if (after_scripts(s)) {
		*repaint = 1;
	} else if (page_changed(s)) {
		/* the scripts laid the page out: its height too */
		layout_done(s);
		*repaint = 1;
	}
	if (now_s() - t1 > 0.1)
		debug(1, "busy %.2f s: the scripts' changes (restyle, layout)",
			now_s() - t1);
	if (s->pending_nav)
		t = 0;
	if (s->page.doc && !s->page.done && s->page.loading == L_NONE &&
	    !res_pending(s, -1)) {
		int i, failed = 0;

		for (i = 0; i < s->page.nres; i++)
			failed += s->page.res[i].state == R_FAILED;
		s->page.done = 1;
		debug(1, "all in %.2f s: %d resources, %d failed, page %.0f px",
			now_s() - s->page.t_load, s->page.nres, failed,
			s->page.height);
	}
	return t;
}

int shell_dirty(struct shell *s, int *x, int *y, int *w, int *h)
{
	int x0 = (int)s->page.dirty[0], y0 = (int)s->page.dirty[1] -
		s->page.scroll;
	int x1 = (int)(s->page.dirty[0] + s->page.dirty[2] + 1);
	int y1 = (int)(s->page.dirty[1] + s->page.dirty[3] + 1) -
		s->page.scroll;

	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > s->view_w ? s->view_w : x1;
	y1 = y1 > s->view_h ? s->view_h : y1;
	if (x0 >= x1 || y0 >= y1)
		return 0;
	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return 1;
}

void shell_bgra(struct shell *s, int on)
{
	s->bgra = on;
}

static int paint_doc(struct shell *s, soie_node *doc, unsigned char *px, int w,
	int h, int stride, int sx, int sy)
{
	return s->bgra ? soie_paint_bgra(doc, px, w, h, stride, sx, sy) :
		soie_paint(doc, px, w, h, stride, sx, sy);
}

/* The match found, an orange highlight, in the view's rect x, y, w, h */
static void highlight(struct shell *s, unsigned char *rgba, int stride, int x,
	int y, int w, int h)
{
	int x0 = (int)s->found.x, y0 = (int)s->found.y - s->page.scroll;
	int x1 = (int)(s->found.x + s->found.w + 1);
	int y1 = (int)(s->found.y + s->found.h + 1) - s->page.scroll, i, j;
	int r = s->bgra ? 2 : 0;

	if (!s->found.n)
		return;
	x0 = x0 < x ? x : x0;
	y0 = y0 < y ? y : y0;
	x1 = x1 > x + w ? x + w : x1;
	y1 = y1 > y + h ? y + h : y1;
	for (j = y0; j < y1; j++)
		for (i = x0; i < x1; i++) {
			unsigned char *p = rgba + (size_t)j * (size_t)stride +
				(size_t)i * 4;

			p[r] = (unsigned char)((p[r] + 255) / 2);
			p[1] = (unsigned char)((p[1] + 150) / 2);
			p[2 - r] = (unsigned char)(p[2 - r] / 2);
		}
}

/* The selection, in blue, in the view's rect x, y, w, h */
static void sel_paint(struct shell *s, unsigned char *rgba, int stride, int x,
	int y, int w, int h)
{
	int n = sel_rects(s), k, i, j;
	int r = s->bgra ? 2 : 0;

	for (k = 0; k < n; k++) {
		const float *f = s->sel.r + k * 4;
		/* rounded alike: rects side by side do not overlap */
		int x0 = (int)floorf(f[0] + 0.5f);
		int y0 = (int)floorf(f[1] + 0.5f) - s->page.scroll;
		int x1 = (int)floorf(f[0] + f[2] + 0.5f);
		int y1 = (int)floorf(f[1] + f[3] + 0.5f) - s->page.scroll;

		x0 = x0 < x ? x : x0;
		y0 = y0 < y ? y : y0;
		x1 = x1 > x + w ? x + w : x1;
		y1 = y1 > y + h ? y + h : y1;
		for (j = y0; j < y1; j++)
			for (i = x0; i < x1; i++) {
				unsigned char *p = rgba + (size_t)j *
					(size_t)stride + (size_t)i * 4;

				p[r] = (unsigned char)((p[r] + 0x3d) / 2);
				p[1] = (unsigned char)((p[1] + 0x8e) / 2);
				p[2 - r] = (unsigned char)((p[2 - r] + 0xe6) /
					2);
			}
	}
}

/*
 * Paint the view's rect x, y, w, h (rgba is the view's top left
 * corner): the page, or until the new one is laid out the old one, or
 * white. Returns the document shown.
 */
static soie_node *paint_view(struct shell *s, unsigned char *rgba,
	int stride, int x, int y, int w, int h)
{
	unsigned char *p = rgba + (size_t)y * (size_t)stride + (size_t)x * 4;
	int j;

	if (s->page.doc && !paint_doc(s, s->page.doc, p, w, h, stride, x,
	    s->page.scroll + y)) {
		sel_paint(s, rgba, stride, x, y, w, h);
		highlight(s, rgba, stride, x, y, w, h);
		return s->page.doc;
	}
	if (s->page.old && !paint_doc(s, s->page.old, p, w, h, stride, x,
	    s->page.old_scroll + y))
		return s->page.old;
	for (j = 0; j < h; j++)
		memset(p + (size_t)j * (size_t)stride, 255, (size_t)w * 4);
	return NULL;
}

void shell_paint_rect(struct shell *s, unsigned char *rgba, int stride, int x,
	int y, int w, int h)
{
	paint_view(s, rgba, stride, x, y, w, h);
}

void shell_paint(struct shell *s, unsigned char *rgba, int stride)
{
	float r[4];

	s->painted = paint_view(s, rgba, stride, 0, 0, s->view_w, s->view_h);
	s->painted_scroll = s->page.scroll;
	s->painted_gen = s->painted ? soie_document_generation(s->painted) : 0;
	/* what changed until now is painted (the first call starts the
	 * engine's tracking) */
	if (s->page.doc)
		soie_layout_damage(s->page.doc, r, 0);
	s->damage_full = 0;
}

int shell_paint_changed(struct shell *s, unsigned char *rgba, int stride,
	int (*r)[4], int max)
{
	float f[4 * 32];
	int n, i, k = 0;

	if (s->damage_full || !s->page.doc || s->painted != s->page.doc ||
	    s->painted_scroll != s->page.scroll || max > 32 ||
	    (n = soie_layout_damage(s->page.doc, f, max)) < 0) {
		debug(3, "repaint all");
		return -1;
	}
	for (i = 0; i < n; i++) {
		int x0 = (int)floorf(f[i * 4]);
		int y0 = (int)floorf(f[i * 4 + 1]) - s->page.scroll;
		int x1 = (int)ceilf(f[i * 4] + f[i * 4 + 2]);
		int y1 = (int)ceilf(f[i * 4 + 1] + f[i * 4 + 3]) -
			s->page.scroll;

		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 > s->view_w ? s->view_w : x1;
		y1 = y1 > s->view_h ? s->view_h : y1;
		if (x0 >= x1 || y0 >= y1)
			continue;
		paint_view(s, rgba, stride, x0, y0, x1 - x0, y1 - y0);
		r[k][0] = x0;
		r[k][1] = y0;
		r[k][2] = x1 - x0;
		r[k][3] = y1 - y0;
		k++;
	}
	s->painted_gen = soie_document_generation(s->page.doc);
	debug(3, "repaint %d rects", k);
	return k;
}

/* The page shown changed since it was painted: a script's layout (its
 * getBoundingClientRect()) leaves no update for after_scripts() to do */
static int page_changed(struct shell *s)
{
	return s->page.doc && s->painted == s->page.doc &&
		soie_document_generation(s->page.doc) != s->painted_gen;
}

int shell_paint_scroll(struct shell *s, unsigned char *rgba, int stride,
	int *dy)
{
	int d = s->page.scroll - s->painted_scroll, y;
	size_t row = (size_t)s->view_w * 4;

	*dy = 0;
	if (page_changed(s))
		return 0;
	/* the old page shown stays until the new one is laid out */
	if (!s->page.doc || s->painted != s->page.doc || !d)
		return 1;
	if (d >= s->view_h || -d >= s->view_h)
		return 0;
	if (d > 0) {
		for (y = 0; y < s->view_h - d; y++)
			memmove(rgba + (size_t)y * (size_t)stride, rgba +
				(size_t)(y + d) * (size_t)stride, row);
		paint_view(s, rgba, stride, 0, s->view_h - d, s->view_w, d);
	} else {
		for (y = s->view_h - 1; y >= -d; y--)
			memmove(rgba + (size_t)y * (size_t)stride, rgba +
				(size_t)(y + d) * (size_t)stride, row);
		paint_view(s, rgba, stride, 0, 0, s->view_w, -d);
	}
	s->painted_scroll = s->page.scroll;
	*dy = d;
	return 1;
}

static char *slurp(const char *path, size_t *len);

/*
 * Debugging: $SOIE_DEBUG_JS_START, a script that runs before the page's
 * own ones (first in its <head>), to watch them from the start.
 */
static void debug_js_start(struct shell *s)
{
	const char *path = getenv("SOIE_DEBUG_JS_START");
	soie_node *head, *el;
	char *src;
	size_t len;

	if (!path || !s->page.doc || !(head = find_tag(s->page.doc, "head")) ||
	    !(src = slurp(path, &len)))
		return;
	el = soie_element_new(s->page.doc, SOIE_NS_HTML, "script");
	soie_node_append(el, soie_text_new(s->page.doc, src, len));
	soie_node_insert_before(head, el, soie_node_first_child(head));
	free(src);
	debug(1, "%s runs first", path);
}

/* A whole file, NULL when it cannot be read */
static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "r");
	char *s = NULL;
	size_t n;

	*len = 0;
	if (!f)
		return NULL;
	for (;;) {
		s = realloc(s, *len + 65536);
		if (!s)
			abort();
		n = fread(s + *len, 1, 65536, f);
		*len += n;
		if (n < 65536)
			break;
	}
	fclose(f);
	return s;
}

/*
 * Debugging: run the JavaScript of $SOIE_DEBUG_JS in the page, read
 * again each time, in a block; its console.log() shows with
 * SOIE_DEBUG=2. A script element appended to the body, removed after.
 */
int shell_debug_js(struct shell *s)
{
	const char *path = getenv("SOIE_DEBUG_JS");
	soie_node *body, *el, *t;
	char *src, *body_src;
	size_t len = 0, n;

	if (!path || !s->page.doc || !(body = find_tag(s->page.doc, "body")))
		return -1;
	if (!(body_src = slurp(path, &n)))
		return -1;
	/* in a block: its const and let are new at each run */
	src = malloc(n + 3);
	if (!src)
		abort();
	src[len++] = '{';
	memcpy(src + len, body_src, n);
	len += n;
	free(body_src);
	src[len++] = '\n';
	src[len++] = '}';
	debug(1, "running %s", path);
	el = soie_element_new(s->page.doc, SOIE_NS_HTML, "script");
	t = soie_text_new(s->page.doc, src, len);
	free(src);
	soie_node_append(el, t);
	soie_node_append(body, el);
	soie_script_element(el);
	soie_node_remove(el);
	after_scripts(s);
	return 0;
}

/*
 * Save the page for a bug report: its DOM as HTML (after the scripts)
 * and, beside it, the address, the trail, the DOM and box trees.
 */
int shell_dump(struct shell *s, char *path, size_t size)
{
	const char *dir = debug_dir();
	char *html, *dom, *boxes;
	size_t hl = 0, dl = 0, bl = 0;
	FILE *f;
	long t = (long)time(NULL);

	if (!dir || !s->page.doc)
		return -1;
	snprintf(path, size, "%s/page-%ld.html", dir, t);
	html = soie_node_serialize(s->page.doc, 1, &hl);
	if (!(f = fopen(path, "w")))
		return -1;
	fwrite(html ? html : "", 1, hl, f);
	fclose(f);
	free(html);
	snprintf(path, size, "%s/page-%ld.txt", dir, t);
	if (!(f = fopen(path, "w")))
		return -1;
	fprintf(f, "soie %s\nurl: %s\nview: %dx%d, scroll %d, page height "
		"%.0f\nreplay: soie-render -X -b '%s' -w %d -H %d -o out.png "
		"%s/page-%ld.html\n\nlast events:\n", soie_version(),
		s->page.url, s->view_w, s->view_h, s->page.scroll,
		s->page.height, s->page.url, s->view_w, s->view_h, dir, t);
	fflush(f);
	debug_trail(fileno(f));
	/* with what most rendering bugs come down to */
	dom = soie_node_dump_style(s->page.doc, "display,position,z-index,"
		"opacity,transform,visibility,pointer-events,overflow,width,"
		"height,min-width,max-width,box-sizing,flex-grow,flex-shrink,"
		"flex-basis,margin-left,margin-right,padding-left,padding-right,"
		"background-color,color", &dl);
	boxes = soie_layout_dump(s->page.doc, &bl);
	fprintf(f, "\nDOM, computed styles:\n");
	fwrite(dom ? dom : "", 1, dl, f);
	fprintf(f, "\nboxes:\n");
	fwrite(boxes ? boxes : "", 1, bl, f);
	fclose(f);
	free(dom);
	free(boxes);
	debug(1, "page saved: %s", path);
	return 0;
}

/*
 * The source tree path is in: a directory above it (three at most)
 * with the engine's sources (lib/soie/soie.h) and ext/. Its ext/ in
 * out; 0 when path is not in a tree (installed).
 */
static int tree_ext(const char *path, char *out, size_t size)
{
	char d[4096], t[4200];
	char *e;
	int up;

	if (!path || !realpath(path, d))
		return 0;
	for (up = 0; up < 4 && (e = strrchr(d, '/')) && e != d; up++) {
		*e = '\0';
		snprintf(t, sizeof(t), "%s/lib/soie/soie.h", d);
		if (access(t, F_OK))
			continue;
		snprintf(out, size, "%s/ext", d);
		return !access(out, F_OK);
	}
	return 0;
}

/*
 * The extensions of the build in use: $SOIE_EXT_DIR, else those of the
 * source tree the program (xsoie, a demo) or libsoie (a program built
 * on the tree's) runs from, else the installed ones.
 */
static const char *ext_dir(char *buf, size_t size)
{
	const char *dir = getenv("SOIE_EXT_DIR");
	const char *(*fn)(void) = soie_version;
	Dl_info info;
	void *at;

	if (dir && *dir)
		return dir;
	if (tree_ext("/proc/self/exe", buf, size))
		return buf;
	/* a function's address as data, without a cast ISO C forbids */
	memcpy(&at, &fn, sizeof(at));
	if (dladdr(at, &info) &&
	    tree_ext(info.dli_fname, buf, size))
		return buf;
	return SOIE_EXT_DIR;
}

int shell_init(void)
{
	char buf[4200];
	const char *dir;
	int k;

	if (fetch_fd >= 0)
		return fetch_fd;
	debug_init();
	main_thread = pthread_self();
	dir = ext_dir(buf, sizeof(buf));
	k = soie_ext_load_dir(dir);
	debug(1, "soie %s, %d extensions from %s", soie_version(), k, dir);
	fetch_fd = fetch_start(6);
	return fetch_fd;
}

static void ui_none(void *data, const char *text)
{
	(void)data;
	(void)text;
}

struct shell *shell_new(const struct shell_ui *ui, void *data, int js)
{
	struct shell *s = calloc(1, sizeof(*s));

	if (!s)
		abort();
	if (ui)
		s->ui = *ui;
	if (!s->ui.title)
		s->ui.title = ui_none;
	if (!s->ui.address)
		s->ui.address = ui_none;
	if (!s->ui.alert)
		s->ui.alert = ui_none;
	s->ui_data = data;
	s->host.data = s;
	s->host.request = page_request;
	s->host.navigate = js_navigate;
	s->host.message = js_message;
	s->host.viewport = js_viewport;
	s->host.scroll = js_scroll;
	s->host.url = js_url;
	s->scripts = js;
	s->hist.cur = -1;
	s->view_w = 1024;
	s->view_h = 734;
	s->zoom = 1;
	s->damage_full = 1;
	s->next = shells;
	shells = s;
	return s;
}

void shell_free(struct shell *s)
{
	struct sdl *x;
	struct shell **p;
	struct job *j;
	int i;

	if (!s)
		return;
	for (p = &shells; *p && *p != s; p = &(*p)->next)
		;
	if (*p)
		*p = s->next;
	/* its page load stops, its downloads go on without it */
	if (s->page_dl)
		s->page_dl->cancel = 1;
	for (x = downloads; x; x = x->next)
		if (x->s == s)
			x->s = NULL;
	/* soie_node_free() stops their scripts */
	if (s->page.doc)
		soie_node_free(s->page.doc);
	if (s->page.old)
		soie_node_free(s->page.old);
	drop_resources(s);
	free(s->page.user_css);
	free(s->page.want);
	for (i = 0; i < s->hist.n; i++)
		free(s->hist.url[i]);
	while ((j = s->inbox)) {
		s->inbox = j->next;
		job_free(j);
	}
	job_free(s->deferred);
	free(s->pending_nav);
	free(s->pending_open);
	free(s->refresh_url);
	free(s->found.text);
	free(s->sel.r);
	free(s);
}

void shell_resize(struct shell *s, int w, int h)
{
	debug(2, "resize %dx%d", w, h);
	s->view_w = w;
	s->view_h = h;
	s->damage_full = 1;
	if (s->page.doc) {
		relayout(s);
		clamp_scroll(s);
	}
}

void shell_zoom(struct shell *s, float zoom)
{
	if (zoom < 0.1f)
		zoom = 0.1f;
	if (zoom > 10)
		zoom = 10;
	if (zoom == s->zoom)
		return;
	debug(2, "zoom %.2f", zoom);
	/* the same place of the page stays at the top */
	s->page.scroll = (int)((float)s->page.scroll * zoom / s->zoom);
	s->zoom = zoom;
	s->damage_full = 1;
	if (s->page.doc)
		relayout(s);
	clamp_scroll(s);
}

float shell_get_zoom(struct shell *s)
{
	return s->zoom;
}

void shell_dark(struct shell *s, int on)
{
	if (!on == !s->dark)
		return;
	debug(2, "%s colours", on ? "dark" : "light");
	s->dark = !!on;
	s->damage_full = 1;
	if (s->page.doc)
		relayout(s);
}

/* "dark" in a theme's name, any case: Adwaita-dark, Adwaita:dark */
static int dark_name(const char *n)
{
	for (; *n; n++)
		if (!strncasecmp(n, "dark", 4))
			return 1;
	return 0;
}

/* A GTK settings.ini: 1 dark, 0 light, -1 no file */
static int gtk_ini_dark(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];
	int dark = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '=');

		if (!v)
			continue;
		v++;
		v += strspn(v, " \t\"");
		if (!strncmp(line, "gtk-theme-name", 14))
			dark |= dark_name(v);
		else if (!strncmp(line, "gtk-application-prefer-dark-theme",
		    33))
			dark |= *v == '1' || !strncasecmp(v, "true", 4);
	}
	fclose(f);
	return dark;
}

int shell_system_dark(void)
{
	static const char *const ini[] = { "gtk-3.0", "gtk-4.0" };
	const char *t = getenv("GTK_THEME"), *c = getenv("XDG_CONFIG_HOME"),
		*h = getenv("HOME"), *sys = getenv("XDG_CONFIG_DIRS");
	char path[4096];
	size_t i;

	if (t && *t)
		return dark_name(t);
	if (!sys || !*sys)
		sys = "/etc/xdg";
	/* the user's files, then the system's (/etc/xdg) */
	for (i = 0; i < 2 * sizeof(ini) / sizeof(*ini); i++) {
		const char *dir = ini[i % 2];
		int d;

		if (i >= 2)
			/* the first of $XDG_CONFIG_DIRS */
			snprintf(path, sizeof(path), "%.*s/%s/settings.ini",
				(int)strcspn(sys, ":"), sys, dir);
		else if (c && *c)
			snprintf(path, sizeof(path), "%s/%s/settings.ini", c,
				dir);
		else if (h)
			snprintf(path, sizeof(path),
				"%s/.config/%s/settings.ini", h, dir);
		else
			continue;
		d = gtk_ini_dark(path);
		if (d >= 0)
			return d;
	}
	return 0;
}

void shell_private(struct shell *s)
{
	s->private = SOIE_REQ_PRIVATE;
	s->host.flags = SOIE_REQ_PRIVATE;
}

void shell_load(struct shell *s, const char *url)
{
	load(s, url, NULL, 1);
}

void shell_load_html(struct shell *s, const char *html, const char *url)
{
	if (!url || !*url)
		url = "about:blank";
	debug(1, "load html at %s", url);
	load_start(s, url, 1);
	load_here(s, url, html ? html : "");
}

void shell_go(struct shell *s, const char *typed)
{
	const char *e, *fmt, *pc;
	char *v, *q = NULL, *url;
	size_t n, qlen = 0;

	while (*typed == ' ' || *typed == '\t')
		typed++;
	for (e = typed + strlen(typed); e > typed && (e[-1] == ' ' ||
	     e[-1] == '\t'); e--)
		;
	if (e == typed)
		return;
	v = strndup(typed, (size_t)(e - typed));
	n = strlen(v);
	if (strstr(v, "://") || !strncmp(v, "about:", 6)) {
		url = strdup(v);
	} else if (v[0] == '/') {
		url = malloc(n + 8);
		sprintf(url, "file://%s", v);
	} else if (!strchr(v, ' ') && (strchr(v, '.') || strchr(v, ':') ||
		   !strcmp(v, "localhost"))) {
		url = malloc(n + 9);
		sprintf(url, "https://%s", v);
	} else {
		/* words: a search, %s in the URL becomes them */
		const struct shell_settings *st = settings_get();

		fmt = st && *st->search ? st->search : getenv("SOIE_SEARCH");
		if (!fmt || !strstr(fmt, "%s"))
			fmt = "https://lite.duckduckgo.com/lite/?q=%s";
		pc = strstr(fmt, "%s");
		q = calloc(1, 1);
		urlencode(&q, &qlen, v);
		url = malloc(strlen(fmt) + qlen + 1);
		memcpy(url, fmt, (size_t)(pc - fmt));
		strcpy(url + (pc - fmt), q);
		strcat(url, pc + 2);
		free(q);
	}
	load(s, url, NULL, 1);
	free(url);
	free(v);
}

int shell_can_go(struct shell *s, int delta)
{
	int to = s->hist.cur + delta;

	return delta && to >= 0 && to < s->hist.n;
}

void shell_history(struct shell *s, int delta)
{
	go_history(s, delta);
}

void shell_reload(struct shell *s)
{
	load(s, s->page.url, NULL, 0);
}

const char *shell_url(struct shell *s)
{
	return s->page.url;
}

int shell_loading(struct shell *s, const char **text)
{
	int n = res_pending(s, -1) + (s->page.loading == L_DOC);

	snprintf(s->loading_text, sizeof(s->loading_text),
		"Loading%s (%d)...", s->page.loading == L_DOC ? " the page" :
		"", n);
	*text = s->loading_text;
	return n;
}

/* Offscreen: wait for the page and its resources, 60 s at most. */
static void pump(struct shell *s)
{
	double end = now_s() + 60;
	struct pollfd p;
	int r;

	while ((s->page.loading != L_NONE || res_pending(s, -1)) &&
	    now_s() < end) {
		p.fd = fetch_fd;
		p.events = POLLIN;
		poll(&p, 1, 200);
		collect_jobs(s, 0);
		apply_late(s, &r);
	}
	s->page.laid_out = 0;
	apply_late(s, &r);
}

/* Offscreen: let scripts navigate and their timers run, 3 s at most. */
static void settle_offscreen(struct shell *s)
{
	struct timespec t0, t;
	int d, spent, hops = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (refresh_wait(s) >= 0 && refresh_wait(s) <= 3000 &&
		    hops < 5) {
			struct timespec w;
			int ms = refresh_wait(s);

			w.tv_sec = ms / 1000;
			w.tv_nsec = (long)(ms % 1000) * 1000000;
			nanosleep(&w, NULL);
			refresh_due(s);
		}
		if (s->pending_nav && hops++ < 5) {
			after_scripts(s);
			pump(s);
			continue;
		}
		d = s->page.doc ? soie_script_tick(s->page.doc) : -1;
		if (after_scripts(s))
			pump(s);
		clock_gettime(CLOCK_MONOTONIC, &t);
		spent = (int)((t.tv_sec - t0.tv_sec) * 1000 +
			(t.tv_nsec - t0.tv_nsec) / 1000000);
		if (d < 0 || spent + d > 3000)
			break;
		t.tv_sec = d / 1000;
		t.tv_nsec = (long)(d % 1000) * 1000000;
		nanosleep(&t, NULL);
	}
}

/* Offscreen: render the page to a PNG, for tests and scripts. */
static int render_png(struct shell *s, const char *path)
{
	unsigned char *px = calloc((size_t)s->view_w * (size_t)s->view_h, 4);
	FILE *f = fopen(path, "wb");
	png_structp p;
	png_infop info;
	int y, h = s->view_h, round;

	if (!f || !px)
		return 1;
	/* background images are asked for when painted: paint again
	 * once they came */
	for (round = 0; round < 3; round++) {
		soie_paint(s->page.doc, px, s->view_w, h, s->view_w * 4, 0, 0);
		if (!res_pending(s, -1))
			break;
		pump(s);
	}
	p = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png_create_info_struct(p);
	if (setjmp(png_jmpbuf(p)))
		return 1;
	png_init_io(p, f);
	png_set_IHDR(p, info, (png_uint_32)s->view_w, (png_uint_32)h, 8,
		PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(p, info);
	for (y = 0; y < h; y++)
		png_write_row(p, px + (size_t)y * (size_t)s->view_w * 4);
	png_write_end(p, info);
	png_destroy_write_struct(&p, &info);
	fclose(f);
	free(px);
	return 0;
}


int shell_render_png(struct shell *s, const char *url, const char *path)
{
	load(s, url, NULL, 1);
	pump(s);
	settle_offscreen(s);
	return render_png(s, path);
}
