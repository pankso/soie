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
#include <unistd.h>
#include <png.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <soie/soie.h>
#include "net.h"

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

/* Resources of the current page, fetched once. */
struct res {
	char *url;
	char *data;
	size_t len;
};

static struct {
	char url[4096];
	soie_node *doc;
	float height;
	int scroll;
	struct res *res;
	int nres;
	char base[4096];
	char *user_css;		/* from the content extensions */
} page;

static struct {
	char *url[256];
	int scroll[256];
	int n, cur;
} hist;

static soie_node *chrome;
static soie_node *focus;	/* focused page control */
static int caret;
static int url_focus, url_caret;
static int win_w = 1024, win_h = 768;
static char status[4096];

static Display *dpy;
static Window win;
static GC gc;
static XImage *ximg;
static unsigned char *rgba;
static Cursor cur_arrow, cur_hand, cur_text;

/* Fetching */

static int page_fetch(void *data, const char *url, char **buf, size_t *len)
{
	soie_request rq;
	struct response r;
	int i;

	(void)data;
	for (i = 0; i < page.nres; i++)
		if (!strcmp(page.res[i].url, url)) {
			if (!page.res[i].data)
				return -1;
			*buf = malloc(page.res[i].len + 1);
			memcpy(*buf, page.res[i].data, page.res[i].len);
			*len = page.res[i].len;
			return 0;
		}
	page.res = realloc(page.res, (size_t)(page.nres + 1) *
		sizeof(*page.res));
	page.res[page.nres].url = strdup(url);
	page.res[page.nres].data = NULL;
	page.res[page.nres].len = 0;
	page.nres++;
	memset(&rq, 0, sizeof(rq));
	rq.url = url;
	rq.first_party = page.url;
	if (net_fetch(&rq, &r) || r.status >= 400) {
		response_free(&r);
		return -1;
	}
	page.res[page.nres - 1].data = r.body;
	page.res[page.nres - 1].len = r.len;
	*buf = malloc(r.len + 1);
	memcpy(*buf, r.body, r.len);
	*len = r.len;
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
	soie_style_document(page.doc, &so);
	soie_layout_document(page.doc, win_w, win_h - BAR);
	page.height = soie_layout_height(page.doc);
	soie_set_focus(page.doc, focus, caret);
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
	soie_paint(page.doc, rgba + (size_t)win_w * BAR * 4, win_w,
		win_h - BAR, win_w * 4, 0, page.scroll);
	soie_paint(chrome, rgba, win_w, BAR, win_w * 4, 0, 0);
	if (*status) {
		/* the hovered link, bottom left */
		static soie_node *sdoc;
		char html[4300];
		int sw = win_w / 2, sh = 22;

		if (sdoc)
			soie_node_free(sdoc);
		snprintf(html, sizeof(html), "<body style='margin:0;font:12px "
			"sans-serif;background:#f0f0f0;border:1px solid #aaa;"
			"padding:3px;white-space:nowrap;overflow:hidden'>%s",
			status);
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

static void load(const char *url, const char *post, int push)
{
	soie_request rq = { NULL, NULL, NULL, NULL, 0, NULL };
	struct response r;
	soie_html_options ho = { NULL, 0 };
	char *html = NULL;
	const char *src;
	size_t len;

	if (page.doc && push && hist.cur >= 0)
		hist.scroll[hist.cur] = page.scroll;
	drop_resources();
	focus = NULL;
	if (page.doc)
		soie_node_free(page.doc);
	page.doc = NULL;
	memset(&r, 0, sizeof(r));
	rq.url = url;
	rq.body = post;
	rq.body_len = post ? strlen(post) : 0;
	if (!strcmp(url, HOME)) {
		snprintf(page.url, sizeof(page.url), "%s", HOME);
		page.doc = soie_html_parse(home_html, strlen(home_html), NULL);
	} else if (net_fetch(&rq, &r)) {
		char *e = escape_html(r.error), *u = escape_html(url);
		char buf[9000];

		snprintf(page.url, sizeof(page.url), "%s", url);
		snprintf(buf, sizeof(buf), "<!DOCTYPE html><title>Error</title>"
			"<body style='font-family:sans-serif;margin:3em'>"
			"<h1>Cannot open this page</h1><p>%s</p><p>%s</p>", u,
			e);
		page.doc = soie_html_parse(buf, strlen(buf), NULL);
		free(e);
		free(u);
	} else {
		snprintf(page.url, sizeof(page.url), "%s", r.url);
		ho.charset = *r.charset ? r.charset : NULL;
		src = r.body ? r.body : "";
		len = r.len;
		if (!strncmp(r.type, "image/", 6)) {
			char *u = escape_html(r.url);

			html = malloc(strlen(u) + 100);
			sprintf(html, "<body style='margin:0;background:#333'>"
				"<img src=\"%s\">", u);
			free(u);
			/* keep the bytes: the image comes from the cache */
			page.res = malloc(sizeof(*page.res));
			page.res[0].url = strdup(r.url);
			page.res[0].data = r.body;
			page.res[0].len = r.len;
			page.nres = 1;
			r.body = NULL;
			src = html;
			len = strlen(html);
		} else if (!strncmp(r.type, "text/", 5) &&
			   strcmp(r.type, "text/html")) {
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
	response_free(&r);
	free(page.user_css);
	page.user_css = soie_ext_user_css(NULL, page.url);
	soie_ext_document(page.doc, page.url);
	if (push) {
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
	relayout();
	scroll_to_fragment();
	if (!push && hist.cur >= 0 && hist.cur < hist.n)
		page.scroll = hist.scroll[hist.cur];
	clamp_scroll();
	set_title();
	{
		soie_node *u = find_id(chrome, "url");

		soie_element_set_attr(u, "value", page.url);
		url_focus = 0;
		chrome_update();
	}
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
	char *abs;

	if (!href || !strncasecmp(href, "javascript:", 11))
		return;
	abs = soie_url_resolve(page.url, href);
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
	load(abs, NULL, 1);
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
	const char *action = soie_element_get_attr(form, "action");
	const char *method = soie_element_get_attr(form, "method");
	char *q = calloc(1, 1), *abs, *url;
	size_t len = 0;

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

static void click_page(int x, int y)
{
	soie_node *n = soie_hit_test(page.doc, (float)x,
		(float)(y - BAR + page.scroll)), *a, *form;
	const char *t;

	focus = NULL;
	if (!n) {
		soie_set_focus(page.doc, NULL, 0);
		return;
	}
	if (is_text_control(n)) {
		const char *v = soie_element_get_attr(n, "value");

		focus = n;
		caret = v ? (int)strlen(v) : 0;
		soie_set_focus(page.doc, focus, caret);
		return;
	}
	soie_set_focus(page.doc, NULL, 0);
	form = ancestor(n, "form");
	if (!strcmp(soie_element_name(n), "input")) {
		t = soie_element_get_attr(n, "type");
		if (t && !strcasecmp(t, "checkbox")) {
			if (soie_element_get_attr(n, "checked"))
				soie_element_remove_attr(n, "checked");
			else
				soie_element_set_attr(n, "checked", "");
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
		focus = NULL;
		soie_set_focus(page.doc, NULL, 0);
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
		} else if (edit(u, &url_caret, ks, txt, n)) {
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
			focus = NULL;
			soie_set_focus(page.doc, NULL, 0);
			return;
		}
		if (edit(focus, &caret, ks, txt, n))
			soie_set_focus(page.doc, focus, caret);
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
		n = soie_hit_test(page.doc, (float)x, (float)(y - BAR +
			page.scroll));
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
		else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: soie [-w W] [-H H] "
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
	if (out) {
		chrome_update();
		load(start, NULL, 1);
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
					click_page(ev.xbutton.x, ev.xbutton.y);
				}
			}
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
			redraw();
			break;
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == wm_delete)
				return 0;
			break;
		}
	}
}
