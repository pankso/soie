/*
 * The browser's shell: a page, its resources, history, forms, scripts
 * and find, on libsoie, without a toolkit (shell.h).
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <png.h>
#include <soie/soie.h>
#include "net.h"
#include "fetch.h"
#include "shell.h"
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

static struct {
	char url[4096];
	soie_node *doc;
	float height;
	int scroll;
	struct res *res;
	int nres;
	char base[4096];
	char *user_css;		/* from the content extensions */
	unsigned gen;		/* bumped by each load: older jobs drop */
	int loading;		/* L_* */
	int push;		/* the history action of the load */
	char *want;		/* the URL being loaded */
	int restyle, relayout;	/* late style sheets, late images */
	double laid_out;	/* when, for throttling */
	soie_node *old;		/* on screen until the new one is laid out */
	int old_scroll;
	double t_load;		/* when the load started, for debugging */
	int done;		/* everything in: told once */
	float dirty[4];		/* page rectangle of the players' frames */
} page;

static int fetch_fd = -1;	/* readable when jobs finish */
static pthread_t main_thread;	/* the page's: scripts' background
				 * requests come from others */
static int in_style;		/* page_fetch() called by the cascade */
static struct job *deferred;	/* a page that came while scripts ran */

static struct {
	char *url[256];
	int scroll[256];
	int n, cur;
} hist;

static soie_node *focus;	/* focused page control */
static int caret;
static int view_w = 1024, view_h = 734;
/* The front's pixels are B G R A (shell_bgra()); what shell_paint()
 * showed last, at which scroll and document generation */
static int bgra, painted_scroll;
static soie_node *painted;
static unsigned long painted_gen;
/* Something changed that the engine's damage does not see: a page, a
 * stage, a resource (images, fonts), another size */
static int damage_full = 1;
static int page_changed(void);
static int scripts = 1;		/* 0: no JavaScript */
static char *pending_nav;	/* a script's navigation, done after it */
static int pending_replace;
static char *refresh_url;	/* <meta http-equiv=refresh> */
static double refresh_at;

/* Find in the page: the text, the current match (from 1), its rect */
static struct {
	char *text;
	int cur, n;
	float x, y, w, h;
} found;

/* Fetching */

static double now_s(void);

static struct res *res_find(const char *url)
{
	int i;

	for (i = 0; i < page.nres; i++)
		if (!strcmp(page.res[i].url, url))
			return &page.res[i];
	return NULL;
}

/* Queue a resource of the page; urgent unless it is an image. */
static struct res *res_add(const char *url, int kind)
{
	struct res *r;

	page.res = realloc(page.res, (size_t)(page.nres + 1) *
		sizeof(*page.res));
	if (!page.res)
		abort();
	r = &page.res[page.nres++];
	memset(r, 0, sizeof(*r));
	r->url = strdup(url);
	r->kind = kind;
	r->state = R_PENDING;
	fetch_add(url, NULL, page.url, kind, page.gen, kind != K_IMAGE);
	return r;
}

static int res_pending(int kind)
{
	int i, n = 0;

	for (i = 0; i < page.nres; i++)
		if (page.res[i].state == R_PENDING &&
		    (kind < 0 || page.res[i].kind == kind))
			n++;
	return n;
}

/*
 * The engine's fetch: what has arrived, or "later" while it is on its
 * way (the engine asks again after the next restyle or relayout).
 */
static int page_fetch(void *data, const char *url, char **buf, size_t *len)
{
	struct res *r = res_find(url);

	(void)data;
	if (r && r->state == R_GIVEN) {
		/* asked twice: fetch it again */
		r->state = R_PENDING;
		fetch_add(url, NULL, page.url, r->kind, page.gen, 0);
	}
	if (!r)
		r = res_add(url, in_style ? K_STYLE : K_IMAGE);
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

static void drop_resources(void)
{
	int i;

	for (i = 0; i < page.nres; i++) {
		free(page.res[i].url);
		free(page.res[i].data);
	}
	free(page.res);
	page.res = NULL;
	page.nres = 0;
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

static void clamp_scroll(void);
static void set_title(void);
static void navigate_to(const char *href, int replace);

/* page.url in the history: a new entry (push 1), the current one's
 * (push 2), none (0) */
static void hist_note(int push)
{
	int i;

	if (push == 2 && hist.cur >= 0) {
		free(hist.url[hist.cur]);
		hist.url[hist.cur] = strdup(page.url);
		hist.scroll[hist.cur] = 0;
	} else if (push) {
		for (i = hist.cur + 1; i < hist.n; i++)
			free(hist.url[i]);
		if (hist.cur + 1 >= 256) {
			free(hist.url[0]);
			memmove(hist.url, hist.url + 1, 255 * sizeof(char *));
			memmove(hist.scroll, hist.scroll + 1, 255 * sizeof(int));
			hist.cur--;
		}
		hist.cur++;
		hist.url[hist.cur] = strdup(page.url);
		hist.scroll[hist.cur] = 0;
		hist.n = hist.cur + 1;
	}
}

/* history.pushState(), replaceState(): the address only, no load */
static void js_url(void *data, const char *url, int push)
{
	(void)data;
	if (!strcmp(url, page.url))
		return;
	debug(2, "url %s%s", url, push ? "" : " (replaced)");
	if (push && hist.cur >= 0)
		hist.scroll[hist.cur] = page.scroll;
	snprintf(page.url, sizeof(page.url), "%s", url);
	hist_note(push ? 1 : 2);
	debug_page(page.url);
	ui_address(page.url);
}

static void js_navigate(void *data, const char *url, int replace)
{
	(void)data;
	free(pending_nav);
	pending_nav = strdup(url);
	pending_replace = replace;
}

static void js_message(void *data, int kind, const char *text)
{
	(void)data;
	if (kind == SOIE_MSG_ALERT) {
		ui_alert(text);
		fprintf(stderr, "alert: %s\n", text);
	} else {
		debug(kind == SOIE_MSG_ERROR ? 1 : 2, "js %s: %s",
			kind == SOIE_MSG_ERROR ? "error" : kind == SOIE_MSG_WARN ?
			"warn" : "log", text);
	}
}

static void js_viewport(void *data, int *w, int *h, float *sx, float *sy)
{
	(void)data;
	*w = view_w;
	*h = view_h;
	*sx = 0;
	*sy = (float)page.scroll;
}

static void js_scroll(void *data, float x, float y)
{
	(void)data;
	(void)x;
	page.scroll = (int)y;
}

static void collect_jobs(int nested);

/*
 * Scripts' requests: what the page already fetched (its scripts), waited
 * for if still on its way; anything else straight to the network.
 */
static int page_request(void *data, const soie_request *req,
	soie_response *res)
{
	struct res *r = NULL;
	struct pollfd p;

	/* from the script engine's threads (fetch, async XHR): straight to
	 * the network, the page's resources are the main thread's */
	if (!pthread_equal(pthread_self(), main_thread)) {
		int r = net_request(data, req, res);

		debug(2, "script request %s %s: %s %d", req->method ? req->method :
			"GET", req->url, r ? "failed" : "HTTP", r ? 0 : res->status);
		return r;
	}
	if (!req->body && (!req->method || !strcmp(req->method, "GET")))
		r = res_find(req->url);
	if (!r || r->state == R_GIVEN)
		return net_request(data, req, res);
	while (r->state == R_PENDING) {
		p.fd = fetch_fd;
		p.events = POLLIN;
		poll(&p, 1, 1000);
		collect_jobs(1);
		r = res_find(req->url);	/* the table may have moved */
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

static const soie_script_host js_host = { NULL, page_request, js_navigate,
	js_message, js_viewport, js_scroll, js_url };

/* Send a DOM event to the page; 1 when a script cancelled it. */
static int fire(soie_node *target, const char *type, float x, float y,
	const char *key, int mods)
{
	soie_event ev;

	if (!page.doc)
		return 0;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.x = x;
	ev.y = y;
	ev.key = key;
	ev.mods = mods;
	return soie_script_event(page.doc, target, &ev);
}

static void layout_done(void);
static void find_again(void);

/* After scripts ran: their navigation, or their changes on screen. */
static int after_scripts(void)
{
	int moved = 0;

	/* a script's focus(): keys go there now */
	if (page.doc) {
		int c;
		soie_node *f = soie_get_focus(page.doc, &c);

		if (f != focus) {
			focus = f;
			caret = c;
			moved = 1;
		}
	}
	if (pending_nav) {
		char *u = pending_nav;

		pending_nav = NULL;
		navigate_to(u, pending_replace);
		free(u);
		return 1;
	}
	if (page.doc && soie_document_update(page.doc)) {
		layout_done();
		return 1;
	}
	return moved;
}

/* Layout and drawing */

static void relayout(void)
{
	soie_style_options so;

	memset(&so, 0, sizeof(so));
	so.width = view_w;
	so.height = view_h;
	so.base_url = page.url;
	so.fetch = page_fetch;
	so.user_css = page.user_css;
	so.request = net_request;	/* audio and video */
	double t0 = now_s(), t1;

	in_style = 1;
	soie_style_document(page.doc, &so);
	in_style = 0;
	t1 = now_s();
	soie_layout_document(page.doc, view_w, view_h);
	debug(2, "relayout %dx%d: style %.3f s, layout %.3f s", view_w, view_h,
		t1 - t0, now_s() - t1);
	layout_done();
}

static void layout_done(void)
{
	page.laid_out = now_s();
	page.height = soie_layout_height(page.doc);
	soie_set_focus(page.doc, focus, caret);
	clamp_scroll();
	set_title();
	find_again();
}

static void clamp_scroll(void)
{
	int max = (int)page.height - view_h;

	if (page.scroll > max)
		page.scroll = max;
	if (page.scroll < 0)
		page.scroll = 0;
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

static void set_title(void)
{
	soie_node *t = find_tag(page.doc, "title");
	char title[512] = "";

	if (t)
		text_of(t, title, sizeof(title));
	if (!*title)
		snprintf(title, sizeof(title), "%s", page.url);
	ui_title(title);
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
static void meta_refresh(void)
{
	soie_node **v;
	size_t n, i;
	const char *c, *e;
	double delay;
	char *u;

	free(refresh_url);
	refresh_url = NULL;
	v = soie_query(page.doc, "meta[http-equiv=refresh i][content]", 0,
		&n);
	for (i = 0; v && i < n && !refresh_url; i++) {
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
			u = strdup(page.url);
		}
		refresh_url = soie_url_resolve(page.url, u);
		refresh_at = now_s() + (delay > 0 ? delay : 0);
		free(u);
	}
	free(v);
}

/* A due refresh becomes a navigation that replaces this page. */
static int refresh_due(void)
{
	if (!refresh_url || now_s() < refresh_at || pending_nav)
		return 0;
	pending_nav = refresh_url;
	pending_replace = 1;
	refresh_url = NULL;
	return 1;
}

/* ms until the refresh, -1 without one */
static int refresh_wait(void)
{
	double d;

	if (!refresh_url)
		return -1;
	d = (refresh_at - now_s()) * 1000;
	return d < 0 ? 0 : d > 1e9 ? 1000000000 : (int)d;
}

static void scroll_to_fragment(void)
{
	const char *h = strchr(page.url, '#');
	soie_node *el;
	float x, y, w, hh;

	page.scroll = 0;
	if (h && h[1] && (el = find_id(page.doc, h + 1)) &&
	    !soie_element_rect(el, &x, &y, &w, &hh))
		page.scroll = (int)y;
	clamp_scroll();
}

static void set_page_focus_none(void)
{
	focus = NULL;
}

static void first_render(void);
static void start_scripts(void);

static int scripts_on(void)
{
	return scripts && soie_ext_get(SOIE_EXT_SCRIPT, 0) != NULL;
}

/* What the page needs, all asked for at once, sheets and scripts
 * first: the network works in parallel while we wait. */
static void prefetch(void)
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

	v = soie_query(page.doc, "base[href]", 1, &n);
	b = v && n ? v[0] : NULL;
	base = b ? soie_url_resolve(page.url, soie_element_get_attr(b,
		"href")) : strdup(page.url);
	free(v);
	for (k = 0; sel[k]; k++) {
		if (kind[k] == K_SCRIPT && !scripts_on())
			continue;
		v = soie_query(page.doc, sel[k], 0, &n);
		for (i = 0; v && i < n; i++) {
			u = soie_url_resolve(base, soie_element_get_attr(v[i],
				attr[k]));
			if (u && strncmp(u, "data:", 5) && !res_find(u))
				res_add(u, kind[k]);
			free(u);
		}
		free(v);
	}
	free(base);
}

static void debug_js_start(void);

/* The page's bytes arrived: parse them, ask for what it needs. */
static void doc_arrived(struct job *j)
{
	soie_html_options ho = { NULL, 0 };
	struct response *r = &j->r;
	char *html = NULL;
	const char *src;
	size_t len;
	int push = page.push;

	if (j->ok)
		debug(1, "page %d %s, %zu bytes, %.2f s: %s", r->status, r->type,
			r->len, now_s() - page.t_load, r->url);
	else
		debug(1, "page failed, %s: %s", r->error, j->url);
	debug_page(j->ok ? r->url : j->url);
	drop_resources();
	set_page_focus_none();
	if (page.old)
		soie_node_free(page.old);
	page.old = page.doc;
	page.old_scroll = page.scroll;
	if (page.old)
		soie_script_stop(page.old);	/* no more events, timers */
	page.doc = NULL;
	if (!j->ok) {
		char *e = escape_html(r->error), *u = escape_html(j->url);
		char buf[9000];

		snprintf(page.url, sizeof(page.url), "%s", j->url);
		snprintf(buf, sizeof(buf), "<!DOCTYPE html><title>Error</title>"
			"<body style='font-family:sans-serif;margin:3em'>"
			"<h1>Cannot open this page</h1><p>%s</p><p>%s</p>", u,
			e);
		page.doc = soie_html_parse(buf, strlen(buf), NULL);
		free(e);
		free(u);
	} else {
		snprintf(page.url, sizeof(page.url), "%s", r->url);
		ho.charset = *r->charset ? r->charset : NULL;
		ho.scripting = scripts_on();
		src = r->body ? r->body : "";
		len = r->len;
		if (!strncmp(r->type, "image/", 6)) {
			char *u = escape_html(r->url);

			html = malloc(strlen(u) + 100);
			sprintf(html, "<body style='margin:0;background:#333'>"
				"<img src=\"%s\">", u);
			free(u);
			/* keep the bytes: the image comes from the cache */
			page.res = malloc(sizeof(*page.res));
			memset(page.res, 0, sizeof(*page.res));
			page.res[0].url = strdup(r->url);
			page.res[0].data = r->body;
			page.res[0].len = r->len;
			page.res[0].state = R_DONE;
			page.res[0].kind = K_IMAGE;
			page.nres = 1;
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
		page.doc = soie_html_parse(src, len, &ho);
		debug_js_start();
		free(html);
	}
	free(page.user_css);
	page.user_css = soie_ext_user_css(NULL, page.url);
	soie_ext_document(page.doc, page.url);
	hist_note(push);
	page.scroll = 0;
	prefetch();
	page.loading = L_STYLE;
	if (!res_pending(K_STYLE))
		first_render();
}

/* The style sheets are here: lay out and show the page. */
static void first_render(void)
{
	double t = now_s();

	page.restyle = page.relayout = 0;
	relayout();
	debug(1, "first paint %.2f s (layout %.3f s), %d resources asked",
		now_s() - page.t_load, now_s() - t, page.nres);
	if (page.old) {
		soie_node_free(page.old);
		page.old = NULL;
	}
	meta_refresh();
	scroll_to_fragment();
	if (!page.push && hist.cur >= 0 && hist.cur < hist.n)
		page.scroll = hist.scroll[hist.cur];
	clamp_scroll();
	set_title();
	ui_address(page.url);
	page.loading = scripts_on() && strcmp(page.url, HOME) ? L_SCRIPTS :
		L_NONE;
	if (page.loading == L_SCRIPTS && !res_pending(K_SCRIPT))
		start_scripts();
}

static void start_scripts(void)
{
	double t = now_s();

	page.loading = L_NONE;
	debug(1, "scripts start");
	if (!soie_script_start(page.doc, page.url, &js_host))
		after_scripts();
	debug(1, "scripts ran %.2f s", now_s() - t);
}

/* push: 1 adds a history entry, 2 replaces the current one. The page
 * on screen stays until the new one arrives. */
static void load(const char *url, const char *post, int push)
{
	if (page.doc && push && hist.cur >= 0)
		hist.scroll[hist.cur] = page.scroll;
	debug(1, "load%s %s", post ? " (POST)" : "", url);
	page.t_load = now_s();
	page.done = 0;
	page.gen++;
	page.push = push;
	free(page.want);
	page.want = strdup(url);
	free(refresh_url);
	refresh_url = NULL;
	if (!strcmp(url, HOME)) {
		struct job j;

		memset(&j, 0, sizeof(j));
		j.url = (char *)url;
		j.ok = 1;
		snprintf(j.r.url, sizeof(j.r.url), "%s", HOME);
		strcpy(j.r.type, "text/html");
		j.r.body = strdup(home_html);
		j.r.len = strlen(home_html);
		doc_arrived(&j);
		free(j.r.body);
		return;
	}
	fetch_add(url, post, NULL, K_DOC, page.gen, 1);
	page.loading = L_DOC;
	ui_address(url);
}

/* Finished jobs. nested: from inside a script (a script waiting for
 * a resource): a new page waits for the main loop. */
static void collect_jobs(int nested)
{
	struct job *j;
	struct res *r;

	if (!nested && deferred) {
		j = deferred;
		deferred = NULL;
		if (j->gen == page.gen)
			doc_arrived(j);
		job_free(j);
	}
	while ((j = fetch_done())) {
		if (j->gen != page.gen) {
			job_free(j);
			continue;
		}
		if (j->kind == K_DOC) {
			if (nested) {
				job_free(deferred);
				deferred = j;
				continue;
			}
			doc_arrived(j);
			job_free(j);
			continue;
		}
		r = res_find(j->url);
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
				page.restyle = 1;
			else
				page.relayout = 1;
		}
		job_free(j);
	}
	if (nested || !page.doc)
		return;
	if (page.loading == L_STYLE && !res_pending(K_STYLE))
		first_render();
	else if (page.loading == L_SCRIPTS && !res_pending(K_SCRIPT))
		start_scripts();
}

/* Late style sheets restyle, late images lay out again, at most every
 * 200 ms; returns ms until it should be called again, -1: nothing to
 * do. *repaint is set when it laid out. */
static int apply_late(int *repaint)
{
	double wait;

	if (page.loading == L_DOC || page.loading == L_STYLE || !page.doc ||
	    (!page.restyle && !page.relayout))
		return -1;
	wait = 0.2 - (now_s() - page.laid_out);
	if (wait > 0 && res_pending(-1))
		return (int)(wait * 1000) + 1;
	if (page.restyle) {
		relayout();
	} else {
		soie_layout_document(page.doc, view_w, view_h);
		layout_done();
	}
	page.restyle = page.relayout = 0;
	*repaint = 1;
	damage_full = 1;
	return -1;
}

static void go_history(int delta)
{
	int to = hist.cur + delta;

	if (to < 0 || to >= hist.n)
		return;
	hist.scroll[hist.cur] = page.scroll;
	hist.cur = to;
	load(hist.url[to], NULL, 0);
}

static void navigate(const char *href)
{
	if (!href || !strncasecmp(href, "javascript:", 11))
		return;
	navigate_to(href, 0);
}

static void navigate_to(const char *href, int replace)
{
	char *abs = soie_url_resolve(page.url, href);

	/* same document, other fragment: just scroll */
	{
		const char *h = strchr(abs, '#');
		size_t base = strcspn(page.url, "#");

		if (h && (size_t)(h - abs) == base &&
		    !strncmp(abs, page.url, base)) {
			soie_node *el = find_id(page.doc, h + 1);
			float x, y, w, hh;

			snprintf(page.url, sizeof(page.url), "%s", abs);
			hist_note(replace ? 2 : 1);
			ui_address(page.url);
			if (el && !soie_element_rect(el, &x, &y, &w, &hh)) {
				page.scroll = (int)y;
				clamp_scroll();
			}
			free(abs);
			return;
		}
	}
	load(abs, NULL, replace ? 2 : 1);
	free(abs);
}

/* Forms (HTML 4.10.21, the urlencoded part) */

static void urlencode(char **out, size_t *len, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";

	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		*out = realloc(*out, *len + 4);
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

static void submit(soie_node *form, soie_node *submitter)
{
	const char *action, *method;
	char *q, *abs, *url;
	size_t len = 0;

	if (fire(form, "submit", 0, 0, NULL, 0) || pending_nav)
		return;
	action = soie_element_get_attr(form, "action");
	method = soie_element_get_attr(form, "method");
	q = calloc(1, 1);
	collect(form, submitter, &q, &len);
	abs = soie_url_resolve(page.url, action && *action ? action :
		page.url);
	if (method && !strcasecmp(method, "post")) {
		load(abs, q, 1);
	} else {
		size_t n = strcspn(abs, "?#");

		url = malloc(n + len + 2);
		memcpy(url, abs, n);
		url[n] = '?';
		memcpy(url + n + 1, q, len + 1);
		load(url, NULL, 1);
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

static void set_page_focus(soie_node *n, int c)
{
	soie_node *old = focus;

	focus = n;
	caret = c;
	if (page.doc)
		soie_set_focus(page.doc, focus, caret);
	if (old != n) {
		if (old)
			fire(old, "blur", 0, 0, NULL, 0);
		if (n)
			fire(n, "focus", 0, 0, NULL, 0);
	}
}

/* A control changed by the user: input and change events. */
static void changed(soie_node *n)
{
	fire(n, "input", 0, 0, NULL, 0);
	fire(n, "change", 0, 0, NULL, 0);
}

static void click(int x, int y, int mods)
{
	soie_node *n = page.doc ? soie_hit_test(page.doc, (float)x,
		(float)(y + page.scroll)) : NULL, *a, *form;
	const char *t;
	float cy = (float)y;

	if (n) {
		fire(n, "mousedown", (float)x, cy, NULL, mods);
		fire(n, "mouseup", (float)x, cy, NULL, mods);
		if (fire(n, "click", (float)x, cy, NULL, mods) || pending_nav)
			return;
		/* play, pause, seek */
		if (((a = ancestor(n, "video")) || (a = ancestor(n, "audio"))) &&
		    soie_media_click(a, (float)x, (float)(y + page.scroll)))
			return;
	}
	if (is_text_control(n)) {
		const char *v = soie_element_get_attr(n, "value");

		set_page_focus(n, v ? (int)strlen(v) : 0);
		return;
	}
	set_page_focus(NULL, 0);
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
			changed(n);
			relayout();
			return;
		}
		if (t && !strcasecmp(t, "radio")) {
			const char *name = soie_element_get_attr(n, "name");
			soie_node *s = form ? form : page.doc, *o;

			for (o = s; o; ) {
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
				while (o && o != s && !soie_node_next_sibling(o))
					o = soie_node_parent(o);
				o = o && o != s ? soie_node_next_sibling(o) : NULL;
			}
			soie_element_set_attr(n, "checked", "");
			changed(n);
			relayout();
			return;
		}
		if (t && (!strcasecmp(t, "submit") || !strcasecmp(t, "image")) &&
		    form) {
			submit(form, n);
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
		changed(n);
		relayout();
		return;
	}
	if ((a = ancestor(n, "button")) && form) {
		t = soie_element_get_attr(a, "type");
		if (!t || !strcasecmp(t, "submit"))
			submit(form, a);
		return;
	}
	if ((a = ancestor(n, "a")) && soie_element_get_attr(a, "href"))
		navigate(soie_element_get_attr(a, "href"));
}


void shell_click(int x, int y, int mods)
{
	debug(2, "click %d,%d (scroll %d)", x, y, page.scroll);
	click(x, y, mods);
	after_scripts();
	clamp_scroll();
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

int shell_key(const char *key, const char *text, int mods)
{
	int r = 2;

	debug(2, "key %s%s%s%s", mods & SOIE_MOD_CTRL ? "Ctrl+" : "",
		mods & SOIE_MOD_ALT ? "Alt+" : "", mods & SOIE_MOD_SHIFT ?
		"Shift+" : "", key);
	if (page.doc && (fire(focus ? focus : find_tag(page.doc, "body"),
	    "keydown", 0, 0, key, mods) || pending_nav)) {
		after_scripts();
		return 1;
	}
	if (focus) {
		if (!strcmp(key, "Enter")) {
			soie_node *form = ancestor(focus, "form");

			if (form && strcmp(soie_element_name(focus), "textarea")) {
				submit(form, NULL);
				after_scripts();
				return 1;
			}
		}
		if (!strcmp(key, "Escape")) {
			set_page_focus(NULL, 0);
		} else if (shell_edit(focus, &caret, key, text)) {
			soie_set_focus(page.doc, focus, caret);
			if (strncmp(key, "Arrow", 5) && strcmp(key, "Home") &&
			    strcmp(key, "End"))
				fire(focus, "input", 0, 0, NULL, 0);
		}
		after_scripts();
		return 1;
	}
	if (!strcmp(key, "ArrowDown"))
		page.scroll += 40;
	else if (!strcmp(key, "ArrowUp"))
		page.scroll -= 40;
	else if (!strcmp(key, "PageDown") || !strcmp(key, " "))
		page.scroll += view_h - 40;
	else if (!strcmp(key, "PageUp"))
		page.scroll -= view_h - 40;
	else if (!strcmp(key, "Home"))
		page.scroll = 0;
	else if (!strcmp(key, "End"))
		page.scroll = (int)page.height;
	else if (!strcmp(key, "Backspace")) {
		go_history(-1);
		r = 1;
	} else {
		r = 0;
	}
	if (after_scripts() && r)
		r = 1;
	clamp_scroll();
	return r;
}

int shell_hover(int x, int y, char *link, size_t size)
{
	soie_node *n = page.doc ? soie_hit_test(page.doc, (float)x,
		(float)(y + page.scroll)) : NULL, *a;

	*link = '\0';
	if ((a = ancestor(n, "a")) && soie_element_get_attr(a, "href")) {
		char *abs = soie_url_resolve(page.url,
			soie_element_get_attr(a, "href"));

		snprintf(link, size, "%s", abs);
		free(abs);
		return SHELL_HAND;
	}
	return is_text_control(n) ? SHELL_TEXT : SHELL_ARROW;
}

void shell_scroll(int dy)
{
	debug(3, "scroll %+d", dy);
	page.scroll += dy;
	clamp_scroll();
}

void shell_scroll_to(int y)
{
	page.scroll = y;
	clamp_scroll();
}

void shell_view(int *scroll, int *height, int *view)
{
	*scroll = page.scroll;
	*height = (int)page.height;
	*view = view_h;
}

void shell_blur(void)
{
	set_page_focus(NULL, 0);
}

int shell_paste(const char *text)
{
	const char *v;
	char *buf, *o;
	size_t len;
	int multi;

	if (!focus)
		return 0;
	v = soie_element_get_attr(focus, "value");
	if (!v)
		v = "";
	len = strlen(v);
	if (caret > (int)len)
		caret = (int)len;
	multi = !strcmp(soie_element_name(focus), "textarea");
	buf = malloc(len + strlen(text) + 1);
	if (!buf)
		abort();
	memcpy(buf, v, (size_t)caret);
	o = buf + caret;
	for (; *text; text++) {
		if (*text == '\r' && multi)
			continue;
		if (*text == '\n' || *text == '\r')
			/* one line inputs: line breaks become spaces */
			*o++ = multi ? '\n' : ' ';
		else if ((unsigned char)*text >= 0x20 || *text == '\t')
			*o++ = *text;
	}
	strcpy(o, v + caret);
	caret = (int)(o - buf);
	soie_element_set_attr(focus, "value", buf);
	free(buf);
	soie_set_focus(page.doc, focus, caret);
	fire(focus, "input", 0, 0, NULL, 0);
	after_scripts();
	return 1;
}

const char *shell_field(void)
{
	const char *v;

	if (!focus)
		return NULL;
	v = soie_element_get_attr(focus, "value");
	return v ? v : "";
}

/* Find */

/* The current match again, after a layout or a new page. */
static void find_again(void)
{
	found.n = 0;
	if (!found.text || !page.doc)
		return;
	if (found.cur < 1)
		found.cur = 1;
	found.n = soie_find(page.doc, found.text, found.cur - 1, &found.x,
		&found.y, &found.w, &found.h);
	if (found.n && found.cur > found.n) {
		found.cur = found.n;
		soie_find(page.doc, found.text, found.cur - 1, &found.x,
			&found.y, &found.w, &found.h);
	}
}

int shell_find(const char *text, int dir, int *cur)
{
	*cur = 0;
	if (!text || !*text) {
		free(found.text);
		found.text = NULL;
		found.n = found.cur = 0;
		return 0;
	}
	if (!found.text || strcmp(found.text, text)) {
		free(found.text);
		found.text = strdup(text);
		found.cur = 1;
		dir = 1;	/* show it */
	} else if (dir && found.n) {
		found.cur += dir;
		if (found.cur < 1)
			found.cur = found.n;
		if (found.cur > found.n)
			found.cur = 1;
	}
	find_again();
	if (!found.n)
		return 0;
	*cur = found.cur;
	if (dir && (found.y < (float)page.scroll ||
	    found.y + found.h > (float)(page.scroll + view_h))) {
		page.scroll = (int)found.y - view_h / 3;
		clamp_scroll();
	}
	return found.n;
}

/* The loop */

int shell_tick(int *repaint)
{
	/* media events first: timers their listeners set count below */
	int r = 0, mt = page.doc ? soie_media_tick_rect(page.doc, &r,
		page.dirty) : -1;
	double t0 = now_s(), t1;
	int t = page.doc ? soie_script_tick(page.doc) : -1, m;
	soie_node *d = page.doc;
	int l = page.loading, n = res_pending(-1);

	/* what keeps the window from answering */
	if ((t1 = now_s()) - t0 > 0.1)
		debug(1, "busy %.2f s: timers and answers of scripts", t1 - t0);
	*repaint = r;
	if (mt >= 0 && (t < 0 || mt < t))
		t = mt;
	/* a new page, a new stage, a resource: show it */
	collect_jobs(0);
	if (d != page.doc || l != page.loading || n != res_pending(-1)) {
		*repaint = 1;
		damage_full = 1;
		/* scripts may have started: their timers, asked above,
		 * are asked again at once */
		t = 0;
	}
	m = apply_late(repaint);
	if (m >= 0 && (t < 0 || m < t))
		t = m;
	m = refresh_wait();
	if (m >= 0 && (t < 0 || m < t))
		t = m;
	refresh_due();
	t1 = now_s();
	if (after_scripts()) {
		*repaint = 1;
	} else if (page_changed()) {
		/* the scripts laid the page out: its height too */
		layout_done();
		*repaint = 1;
	}
	if (now_s() - t1 > 0.1)
		debug(1, "busy %.2f s: the scripts' changes (restyle, layout)",
			now_s() - t1);
	if (pending_nav)
		t = 0;
	if (page.doc && !page.done && page.loading == L_NONE &&
	    !res_pending(-1)) {
		int i, failed = 0;

		for (i = 0; i < page.nres; i++)
			failed += page.res[i].state == R_FAILED;
		page.done = 1;
		debug(1, "all in %.2f s: %d resources, %d failed, page %.0f px",
			now_s() - page.t_load, page.nres, failed, page.height);
	}
	return t;
}

int shell_dirty(int *x, int *y, int *w, int *h)
{
	int x0 = (int)page.dirty[0], y0 = (int)page.dirty[1] - page.scroll;
	int x1 = (int)(page.dirty[0] + page.dirty[2] + 1);
	int y1 = (int)(page.dirty[1] + page.dirty[3] + 1) - page.scroll;

	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > view_w ? view_w : x1;
	y1 = y1 > view_h ? view_h : y1;
	if (x0 >= x1 || y0 >= y1)
		return 0;
	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return 1;
}

void shell_bgra(int on)
{
	bgra = on;
}

static int paint_doc(soie_node *doc, unsigned char *px, int w, int h,
	int stride, int sx, int sy)
{
	return bgra ? soie_paint_bgra(doc, px, w, h, stride, sx, sy) :
		soie_paint(doc, px, w, h, stride, sx, sy);
}

/* The match found, an orange highlight, in the view's rect x, y, w, h */
static void highlight(unsigned char *rgba, int stride, int x, int y, int w,
	int h)
{
	int x0 = (int)found.x, y0 = (int)found.y - page.scroll;
	int x1 = (int)(found.x + found.w + 1);
	int y1 = (int)(found.y + found.h + 1) - page.scroll, i, j;
	int r = bgra ? 2 : 0;

	if (!found.n)
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

/*
 * Paint the view's rect x, y, w, h (rgba is the view's top left
 * corner): the page, or until the new one is laid out the old one, or
 * white. Returns the document shown.
 */
static soie_node *paint_view(unsigned char *rgba, int stride, int x, int y,
	int w, int h)
{
	unsigned char *p = rgba + (size_t)y * (size_t)stride + (size_t)x * 4;
	int j;

	if (page.doc && !paint_doc(page.doc, p, w, h, stride, x,
	    page.scroll + y)) {
		highlight(rgba, stride, x, y, w, h);
		return page.doc;
	}
	if (page.old && !paint_doc(page.old, p, w, h, stride, x,
	    page.old_scroll + y))
		return page.old;
	for (j = 0; j < h; j++)
		memset(p + (size_t)j * (size_t)stride, 255, (size_t)w * 4);
	return NULL;
}

void shell_paint_rect(unsigned char *rgba, int stride, int x, int y, int w,
	int h)
{
	paint_view(rgba, stride, x, y, w, h);
}

void shell_paint(unsigned char *rgba, int stride)
{
	float r[4];

	painted = paint_view(rgba, stride, 0, 0, view_w, view_h);
	painted_scroll = page.scroll;
	painted_gen = painted ? soie_document_generation(painted) : 0;
	/* what changed until now is painted (the first call starts the
	 * engine's tracking) */
	if (page.doc)
		soie_layout_damage(page.doc, r, 0);
	damage_full = 0;
}

int shell_paint_changed(unsigned char *rgba, int stride, int (*r)[4],
	int max)
{
	float f[4 * 32];
	int n, i, k = 0;

	if (damage_full || !page.doc || painted != page.doc ||
	    painted_scroll != page.scroll || max > 32 ||
	    (n = soie_layout_damage(page.doc, f, max)) < 0) {
		debug(3, "repaint all");
		return -1;
	}
	for (i = 0; i < n; i++) {
		int x0 = (int)floorf(f[i * 4]);
		int y0 = (int)floorf(f[i * 4 + 1]) - page.scroll;
		int x1 = (int)ceilf(f[i * 4] + f[i * 4 + 2]);
		int y1 = (int)ceilf(f[i * 4 + 1] + f[i * 4 + 3]) - page.scroll;

		x0 = x0 < 0 ? 0 : x0;
		y0 = y0 < 0 ? 0 : y0;
		x1 = x1 > view_w ? view_w : x1;
		y1 = y1 > view_h ? view_h : y1;
		if (x0 >= x1 || y0 >= y1)
			continue;
		paint_view(rgba, stride, x0, y0, x1 - x0, y1 - y0);
		r[k][0] = x0;
		r[k][1] = y0;
		r[k][2] = x1 - x0;
		r[k][3] = y1 - y0;
		k++;
	}
	painted_gen = soie_document_generation(page.doc);
	debug(3, "repaint %d rects", k);
	return k;
}

/* The page shown changed since it was painted: a script's layout (its
 * getBoundingClientRect()) leaves no update for after_scripts() to do */
static int page_changed(void)
{
	return page.doc && painted == page.doc &&
		soie_document_generation(page.doc) != painted_gen;
}

int shell_paint_scroll(unsigned char *rgba, int stride, int *dy)
{
	int d = page.scroll - painted_scroll, y;
	size_t row = (size_t)view_w * 4;

	*dy = 0;
	if (page_changed())
		return 0;
	/* the old page shown stays until the new one is laid out */
	if (!page.doc || painted != page.doc || !d)
		return 1;
	if (d >= view_h || -d >= view_h)
		return 0;
	if (d > 0) {
		for (y = 0; y < view_h - d; y++)
			memmove(rgba + (size_t)y * (size_t)stride, rgba +
				(size_t)(y + d) * (size_t)stride, row);
		paint_view(rgba, stride, 0, view_h - d, view_w, d);
	} else {
		for (y = view_h - 1; y >= -d; y--)
			memmove(rgba + (size_t)y * (size_t)stride, rgba +
				(size_t)(y + d) * (size_t)stride, row);
		paint_view(rgba, stride, 0, 0, view_w, -d);
	}
	painted_scroll = page.scroll;
	*dy = d;
	return 1;
}

static char *slurp(const char *path, size_t *len);

/*
 * Debugging: $SOIE_DEBUG_JS_START, a script that runs before the page's
 * own ones (first in its <head>), to watch them from the start.
 */
static void debug_js_start(void)
{
	const char *path = getenv("SOIE_DEBUG_JS_START");
	soie_node *head, *el;
	char *src;
	size_t len;

	if (!path || !page.doc || !(head = find_tag(page.doc, "head")) ||
	    !(src = slurp(path, &len)))
		return;
	el = soie_element_new(page.doc, SOIE_NS_HTML, "script");
	soie_node_append(el, soie_text_new(page.doc, src, len));
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
int shell_debug_js(void)
{
	const char *path = getenv("SOIE_DEBUG_JS");
	soie_node *body, *el, *t;
	char *src, *body_src;
	size_t len = 0, n;

	if (!path || !page.doc || !(body = find_tag(page.doc, "body")))
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
	el = soie_element_new(page.doc, SOIE_NS_HTML, "script");
	t = soie_text_new(page.doc, src, len);
	free(src);
	soie_node_append(el, t);
	soie_node_append(body, el);
	soie_script_element(el);
	soie_node_remove(el);
	after_scripts();
	return 0;
}

/*
 * Save the page for a bug report: its DOM as HTML (after the scripts)
 * and, beside it, the address, the trail, the DOM and box trees.
 */
int shell_dump(char *path, size_t size)
{
	const char *dir = debug_dir();
	char *html, *dom, *boxes;
	size_t hl = 0, dl = 0, bl = 0;
	FILE *f;
	long t = (long)time(NULL);

	if (!dir || !page.doc)
		return -1;
	snprintf(path, size, "%s/page-%ld.html", dir, t);
	html = soie_node_serialize(page.doc, 1, &hl);
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
		"%s/page-%ld.html\n\nlast events:\n", soie_version(), page.url,
		view_w, view_h, page.scroll, page.height, page.url, view_w,
		view_h, dir, t);
	fflush(f);
	debug_trail(fileno(f));
	/* with what most rendering bugs come down to */
	dom = soie_node_dump_style(page.doc, "display,position,z-index,"
		"opacity,transform,visibility,pointer-events,overflow,width,"
		"height,min-width,max-width,box-sizing,flex-grow,flex-shrink,"
		"flex-basis,margin-left,margin-right,padding-left,padding-right,"
		"background-color,color", &dl);
	boxes = soie_layout_dump(page.doc, &bl);
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

int shell_init(int js)
{
	const char *dir = getenv("SOIE_EXT_DIR");
	char tree[4200];
	ssize_t n;
	int k;

	debug_init();
	main_thread = pthread_self();
	scripts = js;
	/* not installed: run from the source tree, its extensions */
	if (!dir && access(SOIE_EXT_DIR, F_OK) &&
	    (n = readlink("/proc/self/exe", tree, 4096)) > 0) {
		tree[n] = '\0';
		if (strrchr(tree, '/'))
			strcpy(strrchr(tree, '/'), "/../ext");
		if (!access(tree, F_OK))
			dir = tree;
	}
	k = soie_ext_load_dir(dir ? dir : SOIE_EXT_DIR);
	debug(1, "soie %s, %d extensions from %s", soie_version(), k,
		dir ? dir : SOIE_EXT_DIR);
	hist.cur = -1;
	fetch_fd = fetch_start(6);
	return fetch_fd;
}

void shell_resize(int w, int h)
{
	debug(2, "resize %dx%d", w, h);
	view_w = w;
	view_h = h;
	damage_full = 1;
	if (page.doc) {
		relayout();
		clamp_scroll();
	}
}

void shell_load(const char *url)
{
	load(url, NULL, 1);
}

void shell_go(const char *typed)
{
	const char *e, *fmt, *s;
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
		fmt = getenv("SOIE_SEARCH");
		if (!fmt || !strstr(fmt, "%s"))
			fmt = "https://lite.duckduckgo.com/lite/?q=%s";
		s = strstr(fmt, "%s");
		q = calloc(1, 1);
		urlencode(&q, &qlen, v);
		url = malloc(strlen(fmt) + qlen + 1);
		memcpy(url, fmt, (size_t)(s - fmt));
		strcpy(url + (s - fmt), q);
		strcat(url, s + 2);
		free(q);
	}
	load(url, NULL, 1);
	free(url);
	free(v);
}

void shell_history(int delta)
{
	go_history(delta);
}

void shell_reload(void)
{
	load(page.url, NULL, 0);
}

const char *shell_url(void)
{
	return page.url;
}

int shell_loading(const char **text)
{
	static char buf[64];
	int n = res_pending(-1) + (page.loading == L_DOC);

	snprintf(buf, sizeof(buf), "Loading%s (%d)...",
		page.loading == L_DOC ? " the page" : "", n);
	*text = buf;
	return n;
}

/* Offscreen: wait for the page and its resources, 60 s at most. */
static void pump(void)
{
	double end = now_s() + 60;
	struct pollfd p;
	int r;

	while ((page.loading != L_NONE || res_pending(-1)) && now_s() < end) {
		p.fd = fetch_fd;
		p.events = POLLIN;
		poll(&p, 1, 200);
		collect_jobs(0);
		apply_late(&r);
	}
	page.laid_out = 0;
	apply_late(&r);
}

/* Offscreen: let scripts navigate and their timers run, 3 s at most. */
static void settle_offscreen(void)
{
	struct timespec t0, t;
	int d, spent, hops = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (refresh_wait() >= 0 && refresh_wait() <= 3000 && hops < 5) {
			struct timespec w;
			int ms = refresh_wait();

			w.tv_sec = ms / 1000;
			w.tv_nsec = (long)(ms % 1000) * 1000000;
			nanosleep(&w, NULL);
			refresh_due();
		}
		if (pending_nav && hops++ < 5) {
			after_scripts();
			pump();
			continue;
		}
		d = page.doc ? soie_script_tick(page.doc) : -1;
		if (after_scripts())
			pump();
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
static int render_png(const char *path)
{
	unsigned char *px = calloc((size_t)view_w * (size_t)view_h, 4);
	FILE *f = fopen(path, "wb");
	png_structp p;
	png_infop info;
	int y, h = view_h;

	if (!f || !px)
		return 1;
	soie_paint(page.doc, px, view_w, h, view_w * 4, 0, 0);
	p = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png_create_info_struct(p);
	if (setjmp(png_jmpbuf(p)))
		return 1;
	png_init_io(p, f);
	png_set_IHDR(p, info, (png_uint_32)view_w, (png_uint_32)h, 8,
		PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(p, info);
	for (y = 0; y < h; y++)
		png_write_row(p, px + (size_t)y * (size_t)view_w * 4);
	png_write_end(p, info);
	png_destroy_write_struct(&p, &info);
	fclose(f);
	free(px);
	return 0;
}


int shell_render_png(const char *url, const char *path)
{
	load(url, NULL, 1);
	pump();
	settle_offscreen();
	return render_png(path);
}
