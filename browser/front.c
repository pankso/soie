/*
 * The fronts' common part (front.h): the toolbar and find bar are
 * little HTML pages rendered by Soie, the status line too; the page
 * view is the shell's. A redraw paints only what changed: the toolbar
 * alone, a page scrolled moved and its new band painted, the bars over
 * the page with the page again under where they were.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <soie/soie.h>
#include "shell.h"
#include "front.h"

#define BAR 34			/* toolbar height */
#define FINDH 28		/* find bar height */
#define SB 10			/* scroll bar width */
#define STATUSH 22		/* status line height */

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

static const char find_html[] =
	"<!DOCTYPE html><style>"
	"body{margin:0;font:13px sans-serif;background:#dfe1e5;"
	"border-top:1px solid #aaa;border-right:1px solid #aaa;height:27px}"
	"table{width:100%;border-spacing:4px;border-collapse:separate}"
	"td{padding:0;vertical-align:middle;white-space:nowrap}"
	"input{width:100%;box-sizing:border-box;height:20px;padding:1px 5px;"
	"border:1px solid #999;border-radius:3px;background:white;"
	"font:13px sans-serif}#n{padding:0 4px;color:#444}"
	"</style><table><tr><td style=width:3em>Find"
	"<td><input id=find value=\"\"><td id=n style=width:7em></table>";

static const char *prog = "soie", *start = SHELL_HOME;
static int fd = -1;

/* The window's pixels */
static unsigned char *px;
static int win_w = 1024, win_h = 768, stride, bgra;

static soie_node *chrome, *findbar;
/* Text fields of the front: none, the address, the find bar */
enum { F_NONE, F_URL, F_FIND };
static int field;
static int url_caret, url_fresh;	/* fresh: typing replaces it all */
static int find_on, find_caret, find_n, find_cur;
static char status[4096];
static int drag = -1;		/* scroll bar thumb grabbed, at that offset */

/*
 * What to paint again: the toolbar, the whole page view, the find
 * bar's content. The rest is found at the redraw: the page scrolled,
 * the thumb and the status line changed.
 */
enum { D_CHROME = 1, D_PAGE = 2, D_FIND = 4, D_ALL = 7,
	D_CHANGED = 8 };	/* the page changed: its damage, if known */
static int dirty = D_ALL;
/* Rects shown at the end of the redraw, rects of page painted during
 * it (the bars over them are painted again) */
#define NRECT 8
static int shown[NRECT][4], nshown, fresh[NRECT][4], nfresh;
/* The bars over the page as they were painted: the scroll bar's thumb
 * and its shade; at the bottom left the find bar, or the status line
 * and its text */
static struct over {
	int on, at[4], shade;
	char text[sizeof(status)];
} thumb_o, low_o;
/* The status line: its page, its text and width */
static soie_node *sdoc;
static char stext[sizeof(status)];
static int sw;

static soie_node *find_id(soie_node *n, const char *id)
{
	soie_node *c, *r;
	const char *a;

	if (soie_node_get_type(n) == SOIE_ELEMENT &&
	    (a = soie_element_get_attr(n, "id")) && !strcmp(a, id))
		return n;
	for (c = soie_node_first_child(n); c; c = soie_node_next_sibling(c))
		if ((r = find_id(c, id)))
			return r;
	return NULL;
}

static soie_node *ancestor(soie_node *n, const char *tag)
{
	for (; n && soie_node_get_type(n) == SOIE_ELEMENT;
	     n = soie_node_parent(n))
		if (!strcmp(soie_element_name(n), tag))
			return n;
	return NULL;
}

static char *escape_html(const char *s)
{
	size_t n = strlen(s), i, k = 0;
	char *o = malloc(n * 6 + 1);

	if (!o)
		abort();
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

/* Bars */

static int find_w(void)
{
	return win_w < 460 ? win_w : 460;
}

static void chrome_update(void)
{
	soie_style_document(chrome, NULL);
	soie_layout_document(chrome, win_w, BAR);
	soie_set_focus(chrome, field == F_URL ? find_id(chrome, "url") : NULL,
		url_caret);
	dirty |= D_CHROME;
}

static void find_update(void)
{
	soie_node *n = find_id(findbar, "n"), *t;
	const char *v = soie_element_get_attr(find_id(findbar, "find"),
		"value");
	char buf[64] = "";

	while ((t = soie_node_first_child(n)))
	{
		soie_node_remove(t);
		soie_node_free(t);
	}
	if (v && *v) {
		if (find_n)
			snprintf(buf, sizeof(buf), "%d of %d", find_cur, find_n);
		else
			snprintf(buf, sizeof(buf), "not found");
	}
	soie_node_append(n, soie_text_new(findbar, buf, strlen(buf)));
	soie_style_document(findbar, NULL);
	soie_layout_document(findbar, find_w(), FINDH);
	soie_set_focus(findbar, field == F_FIND ? find_id(findbar, "find") :
		NULL, find_caret);
	dirty |= D_FIND;
}

static void find_close(void)
{
	find_on = 0;
	if (field == F_FIND)
		field = F_NONE;
	shell_find(NULL, 0, &find_cur);
	dirty |= D_PAGE;
}

/* Search again for what the find bar holds: dir as shell_find(). */
static void find_go(int dir)
{
	find_n = shell_find(soie_element_get_attr(find_id(findbar, "find"),
		"value"), dir, &find_cur);
	find_update();
	dirty |= D_PAGE;
}

/* Front calls of the shell */

void ui_address(const char *url)
{
	soie_element_set_attr(find_id(chrome, "url"), "value", url);
	if (field == F_URL)
		field = F_NONE;
	if (px)
		chrome_update();
}

void ui_alert(const char *text)
{
	snprintf(status, sizeof(status), "%s", text);
}

/* Drawing */

static void paint(soie_node *doc, unsigned char *p, int w, int h)
{
	if (bgra)
		soie_paint_bgra(doc, p, w, h, stride, 0, 0);
	else
		soie_paint(doc, p, w, h, stride, 0, 0);
}

static unsigned char *at(int x, int y)
{
	return px + (size_t)y * (size_t)stride + (size_t)x * 4;
}

static int meets(const int *a, const int *b)
{
	return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] &&
		a[1] < b[1] + b[3] && b[1] < a[1] + a[3];
}

/* Add the rect to a list, clipped to the window; a full list becomes
 * one rect, the union of all. */
static void add(int (*list)[4], int *n, int x, int y, int w, int h)
{
	int i;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > win_w)
		w = win_w - x;
	if (y + h > win_h)
		h = win_h - y;
	if (w <= 0 || h <= 0)
		return;
	if (*n == NRECT) {
		for (i = 1; i < NRECT; i++) {
			int x1 = list[0][0] + list[0][2], y1 = list[0][1] +
				list[0][3];

			if (list[i][0] < list[0][0])
				list[0][0] = list[i][0];
			if (list[i][1] < list[0][1])
				list[0][1] = list[i][1];
			if (list[i][0] + list[i][2] > x1)
				x1 = list[i][0] + list[i][2];
			if (list[i][1] + list[i][3] > y1)
				y1 = list[i][1] + list[i][3];
			list[0][2] = x1 - list[0][0];
			list[0][3] = y1 - list[0][1];
		}
		*n = 1;
	}
	list[*n][0] = x;
	list[*n][1] = y;
	list[*n][2] = w;
	list[*n][3] = h;
	(*n)++;
}

/* The page again in a rect of the window, under the bars */
static void page_rect(int x, int y, int w, int h)
{
	if (y < BAR) {
		h -= BAR - y;
		y = BAR;
	}
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (x + w > win_w)
		w = win_w - x;
	if (y + h > win_h)
		h = win_h - y;
	if (w <= 0 || h <= 0)
		return;
	shell_paint_rect(at(0, BAR), stride, x, y - BAR, w, h);
	add(fresh, &nfresh, x, y, w, h);
	add(shown, &nshown, x, y, w, h);
}

/* Whether the page was painted again where r is */
static int repainted(const int *r)
{
	int i;

	for (i = 0; i < nfresh; i++)
		if (meets(fresh[i], r))
			return 1;
	return 0;
}

/* The scroll bar's thumb: 0 when the page fits the view. */
static int thumb(int *y, int *h)
{
	int scroll, height, view;

	shell_view(&scroll, &height, &view);
	if (height <= view || view <= 0)
		return 0;
	*h = (int)((double)view * view / height);
	if (*h < 24)
		*h = 24;
	*y = BAR + (int)((double)scroll * (view - *h) / (height - view));
	return 1;
}

/* The bars over the page as they are to be now */
static void overlays_now(struct over *t, struct over *l)
{
	const char *loading;
	int ty, th;

	memset(t, 0, sizeof(*t));
	if (thumb(&ty, &th)) {
		t->on = 1;
		t->at[0] = win_w - SB + 2;
		t->at[1] = ty;
		t->at[2] = SB - 4;
		t->at[3] = th < win_h - ty ? th : win_h - ty;
		t->shade = drag >= 0 ? 90 : 140;
	}
	memset(l, 0, sizeof(*l));
	if (find_on) {
		l->on = 1;
		l->at[0] = 0;
		l->at[1] = win_h - FINDH;
		l->at[2] = find_w();
		l->at[3] = FINDH;
	} else if (*status || shell_loading(&loading)) {
		/* the hovered link, or the loading, bottom left */
		l->on = 2;
		l->at[0] = 0;
		l->at[1] = win_h - STATUSH;
		l->at[2] = win_w / 2;
		l->at[3] = STATUSH;
		snprintf(l->text, sizeof(l->text), "%s", *status ? status :
			loading);
	}
}

static void paint_low(const struct over *l)
{
	if (l->on == 1) {
		paint(findbar, at(0, l->at[1]), l->at[2], l->at[3]);
		return;
	}
	if (!sdoc || strcmp(l->text, stext) || l->at[2] != sw) {
		char html[sizeof(l->text) + 200];

		if (sdoc)
			soie_node_free(sdoc);
		snprintf(html, sizeof(html), "<body style='margin:0;"
			"font:12px sans-serif;background:#f0f0f0;border:1px "
			"solid #aaa;padding:3px;white-space:nowrap;"
			"overflow:hidden'>%s", l->text);
		sdoc = soie_html_parse(html, strlen(html), NULL);
		soie_style_document(sdoc, NULL);
		soie_layout_document(sdoc, l->at[2], l->at[3]);
		strcpy(stext, l->text);
		sw = l->at[2];
	}
	paint(sdoc, at(0, l->at[1]), l->at[2], l->at[3]);
}

/*
 * The thumb and the bottom bar: what changed is painted, the page again
 * under where it was; what the page covered is painted again, and the
 * bottom bar after the thumb, over it.
 */
static void overlays(void)
{
	struct over t, l;
	int ct, cl, y, x;

	overlays_now(&t, &l);
	ct = t.on != thumb_o.on || (t.on && (memcmp(t.at, thumb_o.at,
		sizeof(t.at)) || t.shade != thumb_o.shade));
	cl = l.on != low_o.on || (l.on && (memcmp(l.at, low_o.at,
		sizeof(l.at)) || strcmp(l.text, low_o.text) || (l.on == 1 &&
		(dirty & D_FIND))));
	if (ct && thumb_o.on)
		page_rect(thumb_o.at[0], thumb_o.at[1], thumb_o.at[2],
			thumb_o.at[3]);
	if (cl && low_o.on)
		page_rect(low_o.at[0], low_o.at[1], low_o.at[2], low_o.at[3]);
	ct = ct || (t.on && repainted(t.at));
	cl = cl || (l.on && repainted(l.at)) || (ct && l.on && t.on &&
		meets(t.at, l.at));
	if (ct && t.on) {
		for (y = t.at[1]; y < t.at[1] + t.at[3]; y++)
			for (x = t.at[0]; x < t.at[0] + t.at[2]; x++) {
				unsigned char *p = at(x, y);

				p[0] = p[1] = p[2] = (unsigned char)t.shade;
			}
		add(shown, &nshown, t.at[0], t.at[1], t.at[2], t.at[3]);
	}
	if (cl && l.on) {
		paint_low(&l);
		add(shown, &nshown, l.at[0], l.at[1], l.at[2], l.at[3]);
	}
	thumb_o = t;
	low_o = l;
}

/* Paint what changed into px, its rects in shown. */
static void paint_frame(void)
{
	int dy, all = 1, n, i, r[NRECT][4];

	nfresh = 0;
	if (dirty & D_CHROME) {
		paint(chrome, px, win_w, BAR);
		add(shown, &nshown, 0, 0, win_w, BAR);
	}
	if (!(dirty & D_PAGE) && (dirty & D_CHANGED) &&
	    (n = shell_paint_changed(at(0, BAR), stride, r, NRECT)) >= 0) {
		/* what changed only, the bars over it after */
		for (i = 0; i < n; i++) {
			add(fresh, &nfresh, r[i][0], r[i][1] + BAR, r[i][2],
				r[i][3]);
			add(shown, &nshown, r[i][0], r[i][1] + BAR, r[i][2],
				r[i][3]);
		}
		all = 0;
	} else if ((dirty & (D_PAGE | D_CHANGED)) ||
	    !shell_paint_scroll(at(0, BAR), stride, &dy)) {
		shell_paint(at(0, BAR), stride);
	} else if (dy) {
		/* the bars moved with the page: the page under them */
		if (thumb_o.on)
			page_rect(thumb_o.at[0], thumb_o.at[1] - dy,
				thumb_o.at[2], thumb_o.at[3]);
		if (low_o.on)
			page_rect(low_o.at[0], low_o.at[1] - dy, low_o.at[2],
				low_o.at[3]);
	} else {
		all = 0;
	}
	if (all) {
		/* no bar left: the page is everywhere */
		add(fresh, &nfresh, 0, BAR, win_w, win_h - BAR);
		add(shown, &nshown, 0, BAR, win_w, win_h - BAR);
		memset(&thumb_o, 0, sizeof(thumb_o));
		memset(&low_o, 0, sizeof(low_o));
	}
	overlays();
	dirty = 0;
}

/*
 * $SOIE_DEBUG_PAINT: after each redraw, the window painted whole into
 * another buffer must be the same (2 levels of rounding per channel),
 * but in the players' rect: their frames come from decoding threads.
 */
static void paint_check(void)
{
	static unsigned char *whole;
	static size_t size;
	struct over t = thumb_o, l = low_o;
	unsigned char *keep = px;
	int x, y, k, bad = 0, bx = 0, by = 0, v[4] = { 0, 0, 0, 0 };

	if (size != (size_t)stride * (size_t)win_h) {
		free(whole);
		size = (size_t)stride * (size_t)win_h;
		if (!(whole = malloc(size)))
			abort();
	}
	px = whole;
	dirty = D_ALL;
	memset(&thumb_o, 0, sizeof(thumb_o));
	memset(&low_o, 0, sizeof(low_o));
	paint_frame();
	nshown = 0;
	thumb_o = t;
	low_o = l;
	px = keep;
	if (shell_dirty(&v[0], &v[1], &v[2], &v[3]))
		v[1] += BAR;
	for (y = 0; y < win_h; y++) {
		const unsigned char *w = whole + (size_t)y * (size_t)stride;

		if (!memcmp(at(0, y), w, (size_t)win_w * 4))
			continue;
		for (x = 0; x < win_w; x++)
			for (k = 0; k < 3; k++) {
				int d = at(x, y)[k] - w[x * 4 + k];

				if (x >= v[0] && x < v[0] + v[2] && y >= v[1] &&
				    y < v[1] + v[3])
					break;

				if ((d > 2 || d < -2) && !bad++) {
					bx = x;
					by = y;
				}
			}
	}
	if (bad)
		fprintf(stderr, "soie: partial redraw differs: %d channels, "
			"first at %d,%d\n", bad, bx, by);
}

static void show(void)
{
	static int check = -1;

	if (!nshown)
		return;
	if (check < 0)
		check = getenv("SOIE_DEBUG_PAINT") != NULL;
	tk_show((const int (*)[4])shown, nshown);
	nshown = 0;
	if (check)
		paint_check();
}

void front_redraw(void)
{
	if (!px)
		return;
	paint_frame();
	show();
}

void front_expose(void)
{
	if (!px)
		return;
	if (dirty & (D_PAGE | D_CHANGED)) {
		front_redraw();
		return;
	}
	add(shown, &nshown, 0, 0, win_w, win_h);
	show();
}

/* Only the players' rectangle changed (a video frame): the page again
 * there, and the bars over it */
static void redraw_rect(void)
{
	int x, y, w, h;

	if (!px || !shell_dirty(&x, &y, &w, &h))
		return;
	nfresh = 0;
	page_rect(x, y + BAR, w, h);
	overlays();
	show();
}

/* Input */

/* Type text into a front field: line breaks become spaces. */
static void field_insert(soie_node *el, int *caret, const char *text)
{
	char *t = strdup(text), *p;

	if (!t)
		abort();
	for (p = t; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f)
			*p = ' ';
	shell_edit(el, caret, "", t);
	free(t);
}

void front_pasted(int to, const char *text)
{
	if (to == F_URL && field == F_URL) {
		soie_node *u = find_id(chrome, "url");

		if (url_fresh) {
			soie_element_set_attr(u, "value", "");
			url_caret = 0;
			url_fresh = 0;
		}
		field_insert(u, &url_caret, text);
		chrome_update();
	} else if (to == F_FIND && field == F_FIND) {
		field_insert(find_id(findbar, "find"), &find_caret, text);
		find_go(0);
	} else if (to == F_NONE) {
		shell_paste(text);
		dirty |= D_PAGE;
	}
}

/* The page's focused control loses the focus: its caret goes. */
static void blur(void)
{
	shell_blur();
	dirty |= D_PAGE;
}

/* select: the address becomes the primary selection, as if selected */
static void focus_url(int select)
{
	soie_node *u = find_id(chrome, "url");

	field = F_URL;
	url_fresh = 1;
	blur();
	url_caret = (int)strlen(soie_element_get_attr(u, "value"));
	if (select)
		tk_copy(0, soie_element_get_attr(u, "value"));
	chrome_update();
}

static void focus_find(void)
{
	field = F_FIND;
	find_on = 1;
	blur();
	find_caret = (int)strlen(soie_element_get_attr(find_id(findbar,
		"find"), "value"));
	find_go(0);
}

/* A new window, another process, on url. */
static void spawn(const char *url)
{
	static char self[4096];
	ssize_t n;
	pid_t pid;
	int fd;

	if (!*self) {
		n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (n <= 0)
			return;
		self[n] = '\0';
	}
	/* twice, so that init reaps it */
	if ((pid = fork()) == 0) {
		if (fork() == 0) {
			for (fd = 3; fd < 1024; fd++)
				close(fd);
			execl(self, prog, url, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* select: as focus_url() */
static void click_chrome(int x, int y, int select)
{
	soie_node *n = soie_hit_test(chrome, (float)x, (float)y), *b;
	const char *id;

	if ((b = ancestor(n, "button")) && (id = soie_element_get_attr(b,
	    "id"))) {
		field = F_NONE;
		if (!strcmp(id, "back"))
			shell_history(-1);
		else if (!strcmp(id, "fwd"))
			shell_history(1);
		else if (!strcmp(id, "reload"))
			shell_reload();
		dirty |= D_PAGE;
	} else if (ancestor(n, "input")) {
		if (field != F_URL || !select)
			focus_url(select);
	} else {
		field = F_NONE;
	}
	chrome_update();
}

static void click_find(int x, int y)
{
	soie_node *n = soie_hit_test(findbar, (float)x, (float)y);

	if (ancestor(n, "input"))
		focus_find();
}

/* A key in the address bar */
static void key_url(const char *k, const char *txt)
{
	soie_node *u = find_id(chrome, "url");
	int typed = *txt && (unsigned char)txt[0] >= 0x20 && txt[0] != 0x7f;

	if (!strcmp(k, "Enter")) {
		char *v = strdup(soie_element_get_attr(u, "value"));

		field = F_NONE;
		shell_go(v);
		free(v);
		dirty |= D_PAGE;
	} else if (!strcmp(k, "Escape")) {
		field = F_NONE;
		soie_element_set_attr(u, "value", shell_url());
	} else {
		/* the first key after focusing replaces the address, as a
		 * selected one would be */
		if (url_fresh && (typed || !strcmp(k, "Backspace") ||
		    !strcmp(k, "Delete"))) {
			soie_element_set_attr(u, "value", "");
			url_caret = 0;
			if (!typed) {
				url_fresh = 0;
				chrome_update();
				return;
			}
		}
		url_fresh = 0;
		shell_edit(u, &url_caret, k, txt);
	}
	chrome_update();
}

/* A key in the find bar */
static void key_find(const char *k, const char *txt, int shift)
{
	if (!strcmp(k, "Enter")) {
		find_go(shift ? -1 : 1);
	} else if (!strcmp(k, "Escape")) {
		find_close();
	} else if (shell_edit(find_id(findbar, "find"), &find_caret, k,
		   txt)) {
		if (strncmp(k, "Arrow", 5) && strcmp(k, "Home") &&
		    strcmp(k, "End"))
			find_go(0);
		else
			find_update();
	}
}

/* A key alone, Shift or not: "l" and "L" are both letter l. */
static int is(const char *k, char c)
{
	return k[0] && !k[1] && (k[0] == c || k[0] == c - 'a' + 'A');
}

static int modifier(const char *k, const char *txt)
{
	static const char *const mods[] = { "Shift", "Control", "Alt",
		"AltGraph", "Meta", "Super", "Hyper", "CapsLock", "NumLock" };
	size_t i;

	for (i = 0; i < sizeof(mods) / sizeof(*mods); i++)
		if (!strcmp(k, mods[i]))
			return 1;
	return !strcmp(k, "Unidentified") && !*txt;
}

void front_key(const char *k, const char *text, int mods)
{
	int ctrl = mods & SOIE_MOD_CTRL, alt = mods & SOIE_MOD_ALT;
	int shift = mods & SOIE_MOD_SHIFT;
	const char *txt = ctrl || alt ? "" : text;	/* shortcuts type nothing */

	if (ctrl && (is(k, 'q') || is(k, 'w')))
		exit(0);
	if ((ctrl && is(k, 'l')) || !strcmp(k, "F6")) {
		focus_url(1);
		return;
	}
	if (ctrl && is(k, 'n')) {
		spawn(SHELL_HOME);
		return;
	}
	if (ctrl && is(k, 'f')) {
		focus_find();
		return;
	}
	if (!strcmp(k, "F3")) {
		if (!find_on)
			focus_find();
		find_go(shift ? -1 : 1);
		return;
	}
	if (ctrl && shift && is(k, 'j')) {
		if (shell_debug_js())
			ui_alert("no $SOIE_DEBUG_JS to run");
		dirty |= D_PAGE;
		return;
	}
	if (!strcmp(k, "F12")) {
		char path[700], *e;

		if (shell_dump(path, sizeof(path)))
			snprintf(path, sizeof(path), "cannot save the page");
		e = escape_html(path);
		ui_alert(e);
		free(e);
		return;
	}
	if (!strcmp(k, "F5") || (ctrl && is(k, 'r'))) {
		shell_reload();
		dirty |= D_PAGE;
		return;
	}
	if (alt && (!strcmp(k, "ArrowLeft") || !strcmp(k, "ArrowRight"))) {
		shell_history(!strcmp(k, "ArrowLeft") ? -1 : 1);
		dirty |= D_PAGE;
		return;
	}
	if ((ctrl && is(k, 'v')) || (shift && !strcmp(k, "Insert"))) {
		tk_paste(ctrl ? 1 : 0, field);
		return;
	}
	if (ctrl && is(k, 'c')) {
		const char *v = field == F_URL ? soie_element_get_attr(
			find_id(chrome, "url"), "value") : field == F_FIND ?
			NULL : shell_field();

		if (v && *v)
			tk_copy(1, v);
		return;
	}
	if (field != F_NONE && modifier(k, txt))
		return;
	if (field == F_URL) {
		key_url(k, txt);
		return;
	}
	if (field == F_FIND) {
		key_find(k, txt, shift);
		return;
	}
	if (!strcmp(k, "Escape") && find_on && !shell_field()) {
		find_close();
		return;
	}
	/* a key that only scrolled the page leaves its pixels to move */
	if (shell_key(k, txt, mods) != 2)
		dirty |= D_PAGE;
}

void front_motion(int x, int y)
{
	soie_node *n;
	int c = SHELL_ARROW, ty, th;
	char link[4096];

	if (drag >= 0) {
		int scroll, height, view;

		if (!thumb(&ty, &th))
			return;
		shell_view(&scroll, &height, &view);
		shell_scroll_to((int)((double)(y - drag - BAR) *
			(height - view) / (view - th)));
		return;
	}
	*status = '\0';
	if (y < BAR) {
		n = soie_hit_test(chrome, (float)x, (float)y);
		if (ancestor(n, "input"))
			c = SHELL_TEXT;
		else if (ancestor(n, "button"))
			c = SHELL_HAND;
	} else if (find_on && y >= win_h - FINDH && x < find_w()) {
		n = soie_hit_test(findbar, (float)x, (float)(y - win_h + FINDH));
		if (ancestor(n, "input"))
			c = SHELL_TEXT;
	} else if (!(x >= win_w - SB && thumb(&ty, &th))) {
		c = shell_hover(x, y - BAR, link, sizeof(link));
		if (*link) {
			char *e = escape_html(link);

			snprintf(status, sizeof(status), "%s", e);
			free(e);
		}
	}
	tk_cursor(c);
}

/* Button 1 or 2 on the page view; 1 when the scroll bar took it. */
static int click_scrollbar(int x, int y)
{
	int ty, th, scroll, height, view;

	if (x < win_w - SB || !thumb(&ty, &th))
		return 0;
	shell_view(&scroll, &height, &view);
	if (y >= ty && y < ty + th)
		drag = y - ty;
	else
		shell_scroll(y < ty ? -(view - 40) : view - 40);
	return 1;
}

void front_wheel(int dy)
{
	shell_scroll(dy);
}

void front_button(int button, int x, int y, int mods)
{
	char link[4096];

	switch (button) {
	case 4: front_wheel(-60); return;
	case 5: front_wheel(60); return;
	case 8: shell_history(-1); dirty |= D_PAGE; return;
	case 9: shell_history(1); dirty |= D_PAGE; return;
	case 1: case 2: break;
	default: return;
	}
	if (y < BAR) {
		click_chrome(x, y, button == 1);
		if (button == 2 && field == F_URL)
			/* the selection replaces the address */
			tk_paste(0, F_URL);
		return;
	}
	if (find_on && y >= win_h - FINDH && x < find_w()) {
		click_find(x, y - win_h + FINDH);
		if (button == 2 && field == F_FIND)
			tk_paste(0, F_FIND);
		return;
	}
	if (field != F_NONE) {
		field = F_NONE;
		chrome_update();
		if (find_on)
			find_update();
	}
	if (button == 1 && click_scrollbar(x, y))
		return;
	dirty |= D_PAGE;
	if (button == 2) {
		/* a link opens in a new window, a field takes the
		 * selection */
		if (shell_hover(x, y - BAR, link, sizeof(link)) ==
		    SHELL_HAND && *link) {
			spawn(link);
			return;
		}
		shell_click(x, y - BAR, mods);
		if (shell_field())
			tk_paste(0, F_NONE);
		return;
	}
	shell_click(x, y - BAR, mods);
}

void front_release(void)
{
	drag = -1;
}

/* Window */

void front_buffer(unsigned char *p, int w, int h, int s, int order)
{
	int resized = w != win_w || h != win_h || !px;

	px = p;
	win_w = w;
	win_h = h;
	stride = s;
	bgra = order;
	shell_bgra(bgra);
	memset(&thumb_o, 0, sizeof(thumb_o));
	memset(&low_o, 0, sizeof(low_o));
	dirty = D_ALL;
	if (resized) {
		chrome_update();
		find_update();
		shell_resize(win_w, win_h - BAR);
	}
}

void front_icon(unsigned char *rgba, int s)
{
	char html[400];
	soie_node *d;

	snprintf(html, sizeof(html), "<body style='margin:0;"
		"background:#5a3e9c;color:white;text-align:center;"
		"font:bold %dpx/%dpx serif'>S", s * 3 / 4, s);
	d = soie_html_parse(html, strlen(html), NULL);
	soie_style_document(d, NULL);
	soie_layout_document(d, s, s);
	memset(rgba, 0, (size_t)s * (size_t)s * 4);
	soie_paint(d, rgba, s, s, s * 4, 0, 0);
	soie_node_free(d);
}

int front_tick(void)
{
	const char *text;
	int repaint, t = shell_tick(&repaint);

	if (repaint == 2 && !(dirty & D_PAGE)) {
		/* a scroll first, the frame painted at the new place */
		paint_frame();
		redraw_rect();
	} else if (repaint) {
		if (find_on)
			find_go(0);
		dirty |= D_CHANGED;
	}
	/* the loading line is kept current */
	if (shell_loading(&text) && (t < 0 || t > 500))
		t = 500;
	front_redraw();
	return t;
}

int front_fd(void)
{
	return fd;
}

void front_load(void)
{
	shell_load(start);
	dirty |= D_PAGE;
}

int front_start(const char *name, int argc, char **argv, int *w, int *h)
{
	const char *out = NULL;
	int i, js = 1;

	prog = name;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w") && i + 1 < argc)
			*w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-H") && i + 1 < argc)
			*h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "-s"))
			js = 0;
		else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: %s [-s] [-w W] [-H H] "
				"[-o out.png] [URL]\n", prog);
			return 2;
		} else
			start = argv[i];
	}
	if (*w < 100)
		*w = 100;
	if (*h < BAR + 50)
		*h = BAR + 50;
	fd = shell_init(js);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot start the fetch threads\n", prog);
		return 1;
	}
	chrome = soie_html_parse(chrome_html, strlen(chrome_html), NULL);
	findbar = soie_html_parse(find_html, strlen(find_html), NULL);
	if (out) {
		shell_resize(*w, *h - BAR);
		return shell_render_png(start, out);
	}
	return -1;
}
