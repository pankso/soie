/*
 * soie: a small web browser on libsoie and Xlib. Its toolbar is itself
 * a little HTML page rendered by Soie. No JavaScript, no cookies: those
 * are extensions (phase 9).
 *
 * Usage: soie [-w WIDTH] [-H HEIGHT] [-o out.png] [URL]
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <png.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <soie/soie.h>
#include "net.h"
#include "fetch.h"

#define BAR 34			/* toolbar height */
#define HOME "about:soie"

static const char chrome_html[] =
	"<!DOCTYPE html><style>"
	"body{margin:0;font:14px sans-serif;background:#dfe1e5;"
	"border-bottom:1px solid #aaa;height:33px}"
	"table{width:100%;border-spacing:4px;border-collapse:separate}"
	"td{padding:0;vertical-align:middle}"
	"button{font:16px sans-serif;width:28px;height:26px;padding:0;"
	"border:1px solid #999;border-radius:4px;background:#f6f6f6}"
	"input{width:100%;box-sizing:border-box;height:26px;padding:3px 6px;"
	"border:1px solid #999;border-radius:13px;background:white;"
	"font:14px sans-serif}"
	"</style><table><tr>"
	"<td style=width:28px><button id=back>\xe2\x97\x80</button>"
	"<td style=width:28px><button id=fwd>\xe2\x96\xb6</button>"
	"<td style=width:28px><button id=reload>\xe2\x86\xbb</button>"
	"<td><input id=url></table>";

static const char home_html[] =
	"<!DOCTYPE html><title>Soie</title><style>body{font-family:sans-serif;"
	"max-width:40em;margin:3em auto;color:#333}h1{color:#5a3e9c}"
	"code{background:#eee;padding:1px 4px}</style><h1>Soie</h1>"
	"<p>A lightweight HTML rendering engine, and this browser.</p>"
	"<p>Type an address above. <code>Alt+\xe2\x86\x90</code> back, "
	"<code>F5</code> reload, <code>Ctrl+L</code> address bar, "
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
} page;

static int fetch_fd = -1;	/* readable when jobs finish */
static int in_style;		/* page_fetch() called by the cascade */
static struct job *deferred;	/* a page that came while scripts ran */

static struct {
	char *url[256];
	int scroll[256];
	int n, cur;
} hist;

static soie_node *chrome;
static soie_node *focus;	/* focused page control */
static int caret;
static int url_focus, url_caret;
static int url_fresh;		/* just focused: typing replaces it all */
static int win_w = 1024, win_h = 768;
static char status[4096];
static int scripts = 1;		/* -s turns JavaScript off */
static char *pending_nav;	/* a script's navigation, done after it */
static int pending_replace;
static char *refresh_url;	/* <meta http-equiv=refresh> */
static double refresh_at;

static Display *dpy;
static Window win;
static GC gc;
static XImage *ximg;
static unsigned char *rgba;
static Cursor cur_arrow, cur_hand, cur_text;

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
		snprintf(status, sizeof(status), "%s", text);
		fprintf(stderr, "alert: %s\n", text);
	} else if (getenv("SOIE_DEBUG")) {
		fprintf(stderr, "%s: %s\n", kind == SOIE_MSG_ERROR ? "error" :
			kind == SOIE_MSG_WARN ? "warn" : "log", text);
	}
}

static void js_viewport(void *data, int *w, int *h, float *sx, float *sy)
{
	(void)data;
	*w = win_w;
	*h = win_h - BAR;
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
	js_message, js_viewport, js_scroll };

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

/* After scripts ran: their navigation, or their changes on screen. */
static int after_scripts(void)
{
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
	return 0;
}

/* Layout and drawing */

static void relayout(void)
{
	soie_style_options so;

	memset(&so, 0, sizeof(so));
	so.width = win_w;
	so.height = win_h - BAR;
	so.base_url = page.url;
	so.fetch = page_fetch;
	so.user_css = page.user_css;
	so.request = net_request;	/* audio and video */
	in_style = 1;
	soie_style_document(page.doc, &so);
	in_style = 0;
	soie_layout_document(page.doc, win_w, win_h - BAR);
	layout_done();
}

static void layout_done(void)
{
	page.laid_out = now_s();
	page.height = soie_layout_height(page.doc);
	soie_set_focus(page.doc, focus, caret);
	clamp_scroll();
	set_title();
}

static void chrome_update(void)
{
	soie_node *url = find_id(chrome, "url");

	soie_style_document(chrome, NULL);
	soie_layout_document(chrome, win_w, BAR);
	soie_set_focus(chrome, url_focus ? url : NULL, url_caret);
}

static void clamp_scroll(void)
{
	int max = (int)page.height - (win_h - BAR);

	if (page.scroll > max)
		page.scroll = max;
	if (page.scroll < 0)
		page.scroll = 0;
}

static void redraw(void)
{
	size_t i, n = (size_t)win_w * (size_t)win_h;

	if (!dpy)
		return;
	/* until the new page is laid out, the old one stays */
	if (!page.doc || soie_paint(page.doc, rgba + (size_t)win_w * BAR * 4,
	    win_w, win_h - BAR, win_w * 4, 0, page.scroll))
		if (!page.old || soie_paint(page.old, rgba +
		    (size_t)win_w * BAR * 4, win_w, win_h - BAR, win_w * 4, 0,
		    page.old_scroll))
			memset(rgba + (size_t)win_w * BAR * 4, 255,
				(size_t)win_w * (size_t)(win_h - BAR) * 4);
	soie_paint(chrome, rgba, win_w, BAR, win_w * 4, 0, 0);
	if (*status || page.loading || res_pending(-1)) {
		/* the hovered link, or the loading, bottom left */
		static soie_node *sdoc;
		char html[4300];
		int sw = win_w / 2, sh = 22;

		if (sdoc)
			soie_node_free(sdoc);
		if (*status)
			snprintf(html, sizeof(html), "<body style='margin:0;"
				"font:12px sans-serif;background:#f0f0f0;border:1px "
				"solid #aaa;padding:3px;white-space:nowrap;"
				"overflow:hidden'>%s", status);
		else
			snprintf(html, sizeof(html), "<body style='margin:0;"
				"font:12px sans-serif;background:#f0f0f0;border:1px "
				"solid #aaa;padding:3px;white-space:nowrap;"
				"overflow:hidden'>Loading%s (%d)...",
				page.loading == L_DOC ? " the page" : "",
				res_pending(-1));
		sdoc = soie_html_parse(html, strlen(html), NULL);
		soie_style_document(sdoc, NULL);
		soie_layout_document(sdoc, sw, sh);
		soie_paint(sdoc, rgba + (size_t)(win_h - sh) * (size_t)win_w * 4,
			sw, sh, win_w * 4, 0, 0);
	}
	/* RGBA to the server's BGRA */
	for (i = 0; i < n; i++) {
		unsigned char *p = rgba + i * 4, t = p[0];

		p[0] = p[2];
		p[2] = t;
	}
	XPutImage(dpy, win, gc, ximg, 0, 0, 0, 0, (unsigned)win_w,
		(unsigned)win_h);
	XFlush(dpy);
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
	if (dpy) {
		char full[600];

		snprintf(full, sizeof(full), "%s - Soie", title);
		XStoreName(dpy, win, full);
		XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", 0),
			XInternAtom(dpy, "UTF8_STRING", 0), 8, PropModeReplace,
			(unsigned char *)full, (int)strlen(full));
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

/* The page's bytes arrived: parse them, ask for what it needs. */
static void doc_arrived(struct job *j)
{
	soie_html_options ho = { NULL, 0 };
	struct response *r = &j->r;
	char *html = NULL;
	const char *src;
	size_t len;
	int push = page.push;

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
		free(html);
	}
	free(page.user_css);
	page.user_css = soie_ext_user_css(NULL, page.url);
	soie_ext_document(page.doc, page.url);
	if (push == 2 && hist.cur >= 0) {
		free(hist.url[hist.cur]);
		hist.url[hist.cur] = strdup(page.url);
		hist.scroll[hist.cur] = 0;
	} else if (push) {
		int i;

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
	page.scroll = 0;
	prefetch();
	page.loading = L_STYLE;
	if (!res_pending(K_STYLE))
		first_render();
}

/* The style sheets are here: lay out and show the page. */
static void first_render(void)
{
	page.restyle = page.relayout = 0;
	relayout();
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
	{
		soie_node *u = find_id(chrome, "url");

		soie_element_set_attr(u, "value", page.url);
		url_focus = 0;
		chrome_update();
	}
	page.loading = scripts_on() && strcmp(page.url, HOME) ? L_SCRIPTS :
		L_NONE;
	if (page.loading == L_SCRIPTS && !res_pending(K_SCRIPT))
		start_scripts();
}

static void start_scripts(void)
{
	page.loading = L_NONE;
	if (!soie_script_start(page.doc, page.url, &js_host))
		after_scripts();
}

/* push: 1 adds a history entry, 2 replaces the current one. The page
 * on screen stays until the new one arrives. */
static void load(const char *url, const char *post, int push)
{
	if (page.doc && push && hist.cur >= 0)
		hist.scroll[hist.cur] = page.scroll;
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
	{
		soie_node *u = find_id(chrome, "url");

		soie_element_set_attr(u, "value", url);
		chrome_update();
	}
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
			}
			if (r->kind == K_STYLE || !strcmp(j->r.type, "text/css") ||
			    strstr(j->r.type, "font"))
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
 * do. */
static int apply_late(void)
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
		soie_layout_document(page.doc, win_w, win_h - BAR);
		layout_done();
	}
	page.restyle = page.relayout = 0;
	redraw();
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

static void click_page(int x, int y, int mods)
{
	soie_node *n = page.doc ? soie_hit_test(page.doc, (float)x,
		(float)(y - BAR + page.scroll)) : NULL, *a, *form;
	const char *t;
	float cy = (float)(y - BAR);

	if (n) {
		fire(n, "mousedown", (float)x, cy, NULL, mods);
		fire(n, "mouseup", (float)x, cy, NULL, mods);
		if (fire(n, "click", (float)x, cy, NULL, mods) || pending_nav)
			return;
		/* play, pause, seek */
		if (((a = ancestor(n, "video")) || (a = ancestor(n, "audio"))) &&
		    soie_media_click(a, (float)x, (float)(y - BAR + page.scroll)))
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

static void click_chrome(int x, int y)
{
	soie_node *n = soie_hit_test(chrome, (float)x, (float)y), *b;
	const char *id;

	url_focus = 0;
	if (n && (b = ancestor(n, "button")) &&
	    (id = soie_element_get_attr(b, "id"))) {
		if (!strcmp(id, "back"))
			go_history(-1);
		else if (!strcmp(id, "fwd"))
			go_history(1);
		else if (!strcmp(id, "reload"))
			load(page.url, NULL, 0);
	} else if (n && !strcmp(soie_element_name(n), "input")) {
		url_fresh = !url_focus;
		url_focus = 1;
		url_caret = (int)strlen(soie_element_get_attr(n, "value"));
	}
	chrome_update();
}

/* Edit the value of input el at caret *cp with a key. */
static int edit(soie_node *el, int *cp, KeySym ks, const char *txt, int n)
{
	const char *v = soie_element_get_attr(el, "value");
	char buf[16384];
	size_t len = v ? strlen(v) : 0;
	int c = *cp;

	if (!v)
		v = "";
	if (c > (int)len)
		c = (int)len;
	snprintf(buf, sizeof(buf), "%s", v);
	if (ks == XK_BackSpace && c > 0) {
		int k = c - 1;

		while (k > 0 && ((unsigned char)buf[k] & 0xc0) == 0x80)
			k--;
		memmove(buf + k, buf + c, len - (size_t)c + 1);
		c = k;
	} else if (ks == XK_Delete && c < (int)len) {
		int k = c + 1;

		while (k < (int)len && ((unsigned char)buf[k] & 0xc0) == 0x80)
			k++;
		memmove(buf + c, buf + k, len - (size_t)k + 1);
	} else if (ks == XK_Left && c > 0) {
		c--;
		while (c > 0 && ((unsigned char)buf[c] & 0xc0) == 0x80)
			c--;
	} else if (ks == XK_Right && c < (int)len) {
		c++;
		while (c < (int)len && ((unsigned char)buf[c] & 0xc0) == 0x80)
			c++;
	} else if (ks == XK_Home) {
		c = 0;
	} else if (ks == XK_End) {
		c = (int)len;
	} else if (n > 0 && (unsigned char)txt[0] >= 0x20 && txt[0] != 0x7f &&
		   len + (size_t)n < sizeof(buf) - 1) {
		memmove(buf + c + n, buf + c, len - (size_t)c + 1);
		memcpy(buf + c, txt, (size_t)n);
		c += n;
	} else {
		return 0;
	}
	soie_element_set_attr(el, "value", buf);
	*cp = c;
	return 1;
}

/* Latin-1 from XLookupString to UTF-8. */
static int to_utf8(const char *in, int n, char *out)
{
	int i, k = 0;

	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)in[i];

		if (c < 0x80) {
			out[k++] = (char)c;
		} else {
			out[k++] = (char)(0xc0 | c >> 6);
			out[k++] = (char)(0x80 | (c & 0x3f));
		}
	}
	return k;
}

/* The DOM key value of a key press. */
static const char *key_name(KeySym ks, const char *txt, int n)
{
	static char buf[8];

	switch (ks) {
	case XK_Return: case XK_KP_Enter: return "Enter";
	case XK_BackSpace: return "Backspace";
	case XK_Tab: return "Tab";
	case XK_Escape: return "Escape";
	case XK_Delete: return "Delete";
	case XK_Left: return "ArrowLeft";
	case XK_Right: return "ArrowRight";
	case XK_Up: return "ArrowUp";
	case XK_Down: return "ArrowDown";
	case XK_Home: return "Home";
	case XK_End: return "End";
	case XK_Prior: return "PageUp";
	case XK_Next: return "PageDown";
	case XK_Shift_L: case XK_Shift_R: return "Shift";
	case XK_Control_L: case XK_Control_R: return "Control";
	case XK_Alt_L: case XK_Alt_R: return "Alt";
	}
	if (ks >= XK_F1 && ks <= XK_F12) {
		snprintf(buf, sizeof(buf), "F%d", (int)(ks - XK_F1 + 1));
		return buf;
	}
	if (n > 0 && n < (int)sizeof(buf) && (unsigned char)txt[0] >= 0x20) {
		memcpy(buf, txt, (size_t)n);
		buf[n] = 0;
		return buf;
	}
	return "Unidentified";
}

static void key(XKeyEvent *e)
{
	char raw[32], txt[64];
	KeySym ks;
	int n = XLookupString(e, raw, sizeof(raw), &ks, NULL);
	int ctrl = e->state & ControlMask, alt = e->state & Mod1Mask;
	int view = win_h - BAR;

	n = to_utf8(raw, n, txt);
	if (ctrl && (ks == XK_q || ks == XK_w))
		exit(0);
	if ((ctrl && ks == XK_l) || ks == XK_F6) {
		url_focus = 1;
		url_fresh = 1;
		set_page_focus(NULL, 0);
		url_caret = (int)strlen(soie_element_get_attr(
			find_id(chrome, "url"), "value"));
		chrome_update();
		return;
	}
	if (ks == XK_F5 || (ctrl && ks == XK_r)) {
		load(page.url, NULL, 0);
		return;
	}
	if (alt && ks == XK_Left) {
		go_history(-1);
		return;
	}
	if (alt && ks == XK_Right) {
		go_history(1);
		return;
	}
	if (!url_focus && page.doc) {
		int mods = (e->state & ShiftMask ? SOIE_MOD_SHIFT : 0) |
			(ctrl ? SOIE_MOD_CTRL : 0) | (alt ? SOIE_MOD_ALT : 0);
		soie_node *t = focus ? focus : find_tag(page.doc, "body");

		if (fire(t, "keydown", 0, 0, key_name(ks, txt, n), mods) ||
		    pending_nav)
			return;
	}
	if (url_focus) {
		soie_node *u = find_id(chrome, "url");

		if (ks == XK_Return || ks == XK_KP_Enter) {
			const char *v = soie_element_get_attr(u, "value");
			char url[4200];

			if (strstr(v, "://") || !strcmp(v, HOME))
				snprintf(url, sizeof(url), "%s", v);
			else if (v[0] == '/')
				snprintf(url, sizeof(url), "file://%s", v);
			else
				snprintf(url, sizeof(url), "https://%s", v);
			load(url, NULL, 1);
		} else if (ks == XK_Escape) {
			url_focus = 0;
			soie_element_set_attr(u, "value", page.url);
			chrome_update();
		} else {
			/* the first key after focusing replaces the address,
			 * as a selected one would be */
			if (url_fresh && (ks == XK_BackSpace || ks == XK_Delete ||
			    (n > 0 && (unsigned char)txt[0] >= 0x20 &&
			     txt[0] != 0x7f))) {
				soie_element_set_attr(u, "value", "");
				url_caret = 0;
				if (ks == XK_BackSpace || ks == XK_Delete) {
					url_fresh = 0;
					chrome_update();
					return;
				}
			}
			url_fresh = 0;
			if (edit(u, &url_caret, ks, txt, n))
				chrome_update();
		}
		return;
	}
	if (focus) {
		if (ks == XK_Return || ks == XK_KP_Enter) {
			soie_node *form = ancestor(focus, "form");

			if (form && strcmp(soie_element_name(focus), "textarea")) {
				submit(form, NULL);
				return;
			}
		}
		if (ks == XK_Escape) {
			set_page_focus(NULL, 0);
			return;
		}
		if (edit(focus, &caret, ks, txt, n)) {
			soie_set_focus(page.doc, focus, caret);
			if (ks != XK_Left && ks != XK_Right && ks != XK_Home &&
			    ks != XK_End)
				fire(focus, "input", 0, 0, NULL, 0);
		}
		return;
	}
	switch (ks) {
	case XK_Down: page.scroll += 40; break;
	case XK_Up: page.scroll -= 40; break;
	case XK_Next: case XK_space: page.scroll += view - 40; break;
	case XK_Prior: page.scroll -= view - 40; break;
	case XK_Home: page.scroll = 0; break;
	case XK_End: page.scroll = (int)page.height; break;
	case XK_BackSpace: go_history(-1); return;
	}
	clamp_scroll();
}

static void hover(int x, int y)
{
	soie_node *n = NULL, *a;
	Cursor c = cur_arrow;
	char old[sizeof(status)];

	snprintf(old, sizeof(old), "%s", status);
	*status = '\0';
	if (y >= BAR) {
		n = !page.doc ? NULL : soie_hit_test(page.doc, (float)x,
			(float)(y - BAR + page.scroll));
		if ((a = ancestor(n, "a")) && soie_element_get_attr(a, "href")) {
			char *abs = soie_url_resolve(page.url,
				soie_element_get_attr(a, "href")), *e;

			e = escape_html(abs);
			snprintf(status, sizeof(status), "%s", e);
			free(e);
			free(abs);
			c = cur_hand;
		} else if (is_text_control(n)) {
			c = cur_text;
		}
	} else {
		n = soie_hit_test(chrome, (float)x, (float)y);
		if (ancestor(n, "button"))
			c = cur_hand;
		else if (n && !strcmp(soie_element_name(n), "input"))
			c = cur_text;
	}
	XDefineCursor(dpy, win, c);
	if (strcmp(old, status))
		redraw();
}

static void resize(int w, int h)
{
	if (w == win_w && h == win_h && ximg)
		return;
	win_w = w < 100 ? 100 : w;
	win_h = h < BAR + 50 ? BAR + 50 : h;
	if (ximg) {
		ximg->data = NULL;
		XDestroyImage(ximg);
	}
	free(rgba);
	rgba = calloc((size_t)win_w * (size_t)win_h, 4);
	ximg = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
		ZPixmap, 0, (char *)rgba, (unsigned)win_w, (unsigned)win_h, 32,
		win_w * 4);
	chrome_update();
	if (page.doc) {
		relayout();
		clamp_scroll();
	}
}

/* Offscreen: wait for the page and its resources, 60 s at most. */
static void pump(void)
{
	double end = now_s() + 60;
	struct pollfd p;

	while ((page.loading != L_NONE || res_pending(-1)) && now_s() < end) {
		p.fd = fetch_fd;
		p.events = POLLIN;
		poll(&p, 1, 200);
		collect_jobs(0);
		apply_late();
	}
	page.laid_out = 0;
	apply_late();
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
	unsigned char *px = calloc((size_t)win_w * (size_t)(win_h - BAR), 4);
	FILE *f = fopen(path, "wb");
	png_structp p;
	png_infop info;
	int y, h = win_h - BAR;

	if (!f || !px)
		return 1;
	soie_paint(page.doc, px, win_w, h, win_w * 4, 0, 0);
	p = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png_create_info_struct(p);
	if (setjmp(png_jmpbuf(p)))
		return 1;
	png_init_io(p, f);
	png_set_IHDR(p, info, (png_uint_32)win_w, (png_uint_32)h, 8,
		PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(p, info);
	for (y = 0; y < h; y++)
		png_write_row(p, px + (size_t)y * (size_t)win_w * 4);
	png_write_end(p, info);
	png_destroy_write_struct(&p, &info);
	fclose(f);
	free(px);
	return 0;
}

int main(int argc, char **argv)
{
	const char *start = HOME, *out = NULL;
	XEvent ev;
	Atom wm_delete;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w") && i + 1 < argc)
			win_w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-H") && i + 1 < argc)
			win_h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "-s"))
			scripts = 0;
		else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: soie [-s] [-w W] [-H H] "
				"[-o out.png] [URL]\n");
			return 2;
		} else
			start = argv[i];
	}
	{
		const char *dir = getenv("SOIE_EXT_DIR");

		soie_ext_load_dir(dir ? dir : SOIE_EXT_DIR);
	}
	chrome = soie_html_parse(chrome_html, strlen(chrome_html), NULL);
	hist.cur = -1;
	fetch_fd = fetch_start(6);
	if (fetch_fd < 0) {
		fputs("soie: cannot start the fetch threads\n", stderr);
		return 1;
	}
	if (out) {
		chrome_update();
		load(start, NULL, 1);
		pump();
		settle_offscreen();
		return render_png(out);
	}
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fputs("soie: cannot open the display\n", stderr);
		return 1;
	}
	win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0,
		(unsigned)win_w, (unsigned)win_h, 0, 0, 0xffffff);
	XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask |
		PointerMotionMask | StructureNotifyMask);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);
	gc = XCreateGC(dpy, win, 0, NULL);
	cur_arrow = XCreateFontCursor(dpy, XC_left_ptr);
	cur_hand = XCreateFontCursor(dpy, XC_hand2);
	cur_text = XCreateFontCursor(dpy, XC_xterm);
	XMapWindow(dpy, win);
	resize(win_w, win_h);
	load(start, NULL, 1);
	redraw();
	for (;;) {
		/* timers run while no X event waits */
		while (!XPending(dpy)) {
			struct pollfd p[2];
			int t = page.doc ? soie_script_tick(page.doc) : -1;
			int m = -1, repaint = 0;

			{
				/* a new page, a new stage, a resource: show it */
				soie_node *d = page.doc;
				int l = page.loading, n = res_pending(-1);

				collect_jobs(0);
				if (d != page.doc || l != page.loading ||
				    n != res_pending(-1))
					redraw();
			}
			m = apply_late();
			if (m >= 0 && (t < 0 || m < t))
				t = m;
			m = -1;

			if (page.doc)
				m = soie_media_tick(page.doc, &repaint);
			if (m >= 0 && (t < 0 || m < t))
				t = m;
			m = refresh_wait();
			if (m >= 0 && (t < 0 || m < t))
				t = m;
			refresh_due();
			if (after_scripts() || repaint)
				redraw();
			if (XPending(dpy))
				break;
			p[0].fd = ConnectionNumber(dpy);
			p[0].events = POLLIN;
			p[1].fd = fetch_fd;
			p[1].events = POLLIN;
			if (pending_nav)
				t = 0;
			if (page.loading == L_DOC || res_pending(-1)) {
				/* the loading line: keep it current */
				static double shown;

				if (t < 0 || t > 500)
					t = 500;
				if (now_s() - shown > 0.5) {
					shown = now_s();
					redraw();
				}
			}
			poll(p, 2, t);
		}
		XNextEvent(dpy, &ev);
		switch (ev.type) {
		case Expose:
			if (!ev.xexpose.count)
				redraw();
			break;
		case ConfigureNotify:
			if (ev.xconfigure.width != win_w ||
			    ev.xconfigure.height != win_h) {
				resize(ev.xconfigure.width, ev.xconfigure.height);
				redraw();
			}
			break;
		case ButtonPress:
			if (ev.xbutton.button == 4)
				page.scroll -= 60;
			else if (ev.xbutton.button == 5)
				page.scroll += 60;
			else if (ev.xbutton.button == 8)
				go_history(-1);
			else if (ev.xbutton.button == 9)
				go_history(1);
			else if (ev.xbutton.button == 1) {
				if (ev.xbutton.y < BAR) {
					click_chrome(ev.xbutton.x, ev.xbutton.y);
				} else {
					if (url_focus) {
						url_focus = 0;
						chrome_update();
					}
					click_page(ev.xbutton.x, ev.xbutton.y,
						(ev.xbutton.state & ShiftMask ?
						 SOIE_MOD_SHIFT : 0) |
						(ev.xbutton.state & ControlMask ?
						 SOIE_MOD_CTRL : 0));
				}
			}
			after_scripts();
			clamp_scroll();
			redraw();
			break;
		case MotionNotify:
			while (XCheckTypedEvent(dpy, MotionNotify, &ev))
				;
			hover(ev.xmotion.x, ev.xmotion.y);
			break;
		case KeyPress:
			key(&ev.xkey);
			after_scripts();
			redraw();
			break;
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == wm_delete)
				return 0;
			break;
		}
	}
}
