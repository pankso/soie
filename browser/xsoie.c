/*
 * xsoie: the Soie browser on Xlib. Its toolbar and find bar are little
 * HTML pages rendered by Soie; the rest is the shell (shell.h).
 *
 * Usage: xsoie [-s] [-w WIDTH] [-H HEIGHT] [-o out.png] [URL]
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <soie/soie.h>
#include "shell.h"

#define BAR 34			/* toolbar height */
#define FINDH 28		/* find bar height */
#define SB 10			/* scroll bar width */

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

static Display *dpy;
static Window win;
static GC gc;
static XImage *ximg;
static XIM xim;
static XIC xic;
static unsigned char *rgba;
static Cursor cur_arrow, cur_hand, cur_text;
static Atom a_utf8, a_clipboard, a_targets, a_xsel;
static int win_w = 1024, win_h = 768;

static soie_node *chrome, *findbar;
/* Text fields of the front: none, the address, the find bar */
enum { F_NONE, F_URL, F_FIND };
static int field;
static int url_caret, url_fresh;	/* fresh: typing replaces it all */
static int find_on, find_caret, find_n, find_cur;
static char status[4096];
static int drag = -1;		/* scroll bar thumb grabbed, at that offset */

/* Selections we own: PRIMARY and CLIPBOARD, and where a paste goes */
static char *sel_text[2];
static int paste_to;

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
}

static void find_close(void)
{
	find_on = 0;
	if (field == F_FIND)
		field = F_NONE;
	shell_find(NULL, 0, &find_cur);
}

/* Search again for what the find bar holds: dir as shell_find(). */
static void find_go(int dir)
{
	find_n = shell_find(soie_element_get_attr(find_id(findbar, "find"),
		"value"), dir, &find_cur);
	find_update();
}

/* Front calls */

void ui_title(const char *title)
{
	char full[600];

	if (!dpy)
		return;
	snprintf(full, sizeof(full), "%s - Soie", title);
	XStoreName(dpy, win, full);
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", 0),
		a_utf8, 8, PropModeReplace, (unsigned char *)full,
		(int)strlen(full));
}

void ui_address(const char *url)
{
	soie_element_set_attr(find_id(chrome, "url"), "value", url);
	if (field == F_URL)
		field = F_NONE;
	if (dpy)
		chrome_update();
}

void ui_alert(const char *text)
{
	snprintf(status, sizeof(status), "%s", text);
}

/* Drawing */

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

static void redraw(void)
{
	static soie_node *sdoc;
	size_t i, n = (size_t)win_w * (size_t)win_h;
	const char *loading;
	int ty, th, x, y;

	if (!dpy)
		return;
	shell_paint(rgba + (size_t)win_w * BAR * 4, win_w * 4);
	soie_paint(chrome, rgba, win_w, BAR, win_w * 4, 0, 0);
	if (thumb(&ty, &th))
		for (y = ty; y < ty + th && y < win_h; y++)
			for (x = win_w - SB + 2; x < win_w - 2; x++) {
				unsigned char *p = rgba + ((size_t)y * win_w +
					(size_t)x) * 4;

				p[0] = p[1] = p[2] = drag >= 0 ? 90 : 140;
			}
	if (find_on)
		soie_paint(findbar, rgba + (size_t)(win_h - FINDH) * win_w * 4,
			find_w(), FINDH, win_w * 4, 0, 0);
	else if (*status || shell_loading(&loading)) {
		/* the hovered link, or the loading, bottom left */
		char html[4300];
		int sw = win_w / 2, sh = 22;

		if (sdoc)
			soie_node_free(sdoc);
		snprintf(html, sizeof(html), "<body style='margin:0;"
			"font:12px sans-serif;background:#f0f0f0;border:1px "
			"solid #aaa;padding:3px;white-space:nowrap;"
			"overflow:hidden'>%s", *status ? status : loading);
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

/* Only the players' rectangle changed (a video frame): it alone is
 * painted and sent, not the whole window */
static void redraw_rect(void)
{
	int x, y, w, h, i, j;

	if (!dpy || !shell_dirty(&x, &y, &w, &h))
		return;
	y += BAR;
	if (y + h > win_h - (find_on ? FINDH : 0))
		h = win_h - (find_on ? FINDH : 0) - y;
	if (h <= 0)
		return;
	shell_paint_rect(rgba + (size_t)win_w * BAR * 4, win_w * 4, x,
		y - BAR, w, h);
	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++) {
			unsigned char *p = rgba + ((size_t)j * win_w +
				(size_t)i) * 4, t = p[0];

			p[0] = p[2];
			p[2] = t;
		}
	XPutImage(dpy, win, gc, ximg, x, y, x, y, (unsigned)w, (unsigned)h);
	XFlush(dpy);
}

/* Selections */

static void own(Atom sel, const char *text, Time t)
{
	int i = sel == XA_PRIMARY ? 0 : 1;

	free(sel_text[i]);
	sel_text[i] = strdup(text);
	XSetSelectionOwner(dpy, sel, win, t);
}

/* Ask for a selection's text; it comes as a SelectionNotify. */
static void paste(Atom sel, int to, Time t)
{
	paste_to = to;
	XConvertSelection(dpy, sel, a_utf8, a_xsel, win, t);
}

/* Give our selection to another client. */
static void give(XSelectionRequestEvent *r)
{
	XSelectionEvent ev;
	const char *text = r->selection == XA_PRIMARY ? sel_text[0] :
		r->selection == a_clipboard ? sel_text[1] : NULL;
	Atom prop = r->property ? r->property : r->target;

	memset(&ev, 0, sizeof(ev));
	ev.type = SelectionNotify;
	ev.requestor = r->requestor;
	ev.selection = r->selection;
	ev.target = r->target;
	ev.time = r->time;
	ev.property = None;
	if (text && r->target == a_targets) {
		Atom t[3];

		t[0] = a_targets;
		t[1] = a_utf8;
		t[2] = XA_STRING;
		XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32,
			PropModeReplace, (unsigned char *)t, 3);
		ev.property = prop;
	} else if (text && r->target == a_utf8) {
		XChangeProperty(dpy, r->requestor, prop, a_utf8, 8,
			PropModeReplace, (unsigned char *)text,
			(int)strlen(text));
		ev.property = prop;
	} else if (text && r->target == XA_STRING) {
		/* Latin-1: what does not fit becomes '?' */
		char *l = malloc(strlen(text) + 1);
		const unsigned char *s = (const unsigned char *)text;
		int k = 0;

		if (!l)
			abort();
		while (*s) {
			if (*s < 0x80) {
				l[k++] = (char)*s++;
			} else if ((*s == 0xc2 || *s == 0xc3) &&
				   (s[1] & 0xc0) == 0x80) {
				l[k++] = (char)((*s & 3) << 6 | (s[1] & 0x3f));
				s += 2;
			} else {
				l[k++] = '?';
				for (s++; (*s & 0xc0) == 0x80; s++)
					;
			}
		}
		XChangeProperty(dpy, r->requestor, prop, XA_STRING, 8,
			PropModeReplace, (unsigned char *)l, k);
		free(l);
		ev.property = prop;
	}
	XSendEvent(dpy, r->requestor, False, 0, (XEvent *)&ev);
}

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

/* A selection's text arrived. */
static void pasted(XSelectionEvent *e)
{
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	char *text;
	size_t i, k = 0;

	if (e->property == None || XGetWindowProperty(dpy, win, e->property,
	    0, 65536, True, AnyPropertyType, &type, &fmt, &n, &after,
	    &data) != Success || !data)
		return;
	if (fmt != 8 || (type != a_utf8 && type != XA_STRING)) {
		XFree(data);
		return;
	}
	text = malloc(n * 2 + 1);
	if (!text)
		abort();
	for (i = 0; i < n; i++)
		if (type == XA_STRING && data[i] >= 0x80) {
			text[k++] = (char)(0xc0 | data[i] >> 6);
			text[k++] = (char)(0x80 | (data[i] & 0x3f));
		} else {
			text[k++] = (char)data[i];
		}
	text[k] = '\0';
	XFree(data);
	if (paste_to == F_URL && field == F_URL) {
		soie_node *u = find_id(chrome, "url");

		if (url_fresh) {
			soie_element_set_attr(u, "value", "");
			url_caret = 0;
			url_fresh = 0;
		}
		field_insert(u, &url_caret, text);
		chrome_update();
	} else if (paste_to == F_FIND && field == F_FIND) {
		field_insert(find_id(findbar, "find"), &find_caret, text);
		find_go(0);
	} else if (paste_to == F_NONE) {
		shell_paste(text);
	}
	free(text);
	redraw();
}

/* Input */

/* select: the address becomes the PRIMARY selection, as if selected */
static void focus_url(Time t, int select)
{
	soie_node *u = find_id(chrome, "url");

	field = F_URL;
	url_fresh = 1;
	shell_blur();
	url_caret = (int)strlen(soie_element_get_attr(u, "value"));
	if (select)
		own(XA_PRIMARY, soie_element_get_attr(u, "value"), t);
	chrome_update();
}

static void focus_find(void)
{
	field = F_FIND;
	find_on = 1;
	shell_blur();
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
			execl(self, "xsoie", url, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* select: as focus_url() */
static void click_chrome(int x, int y, Time t, int select)
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
	} else if (ancestor(n, "input")) {
		if (field != F_URL || !select)
			focus_url(t, select);
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

static void key(XKeyEvent *e)
{
	char raw[64], txt[256];
	KeySym ks = NoSymbol;
	Status st = XLookupNone;
	int n, ctrl = e->state & ControlMask, alt = e->state & Mod1Mask;
	int shift = e->state & ShiftMask;
	const char *k;

	if (xic) {
		n = Xutf8LookupString(xic, e, txt, sizeof(txt) - 1, &ks, &st);
		if (st == XBufferOverflow || st == XLookupNone ||
		    st == XLookupKeySym)
			n = 0;
	} else {
		n = XLookupString(e, raw, sizeof(raw), &ks, NULL);
		n = to_utf8(raw, n, txt);
	}
	txt[n] = '\0';
	k = key_name(ks, txt, n);
	if (ctrl || alt)
		*txt = '\0';	/* shortcuts type nothing */
	if (ctrl && (ks == XK_q || ks == XK_w))
		exit(0);
	if ((ctrl && ks == XK_l) || ks == XK_F6) {
		focus_url(e->time, 1);
		return;
	}
	if (ctrl && ks == XK_n) {
		spawn(SHELL_HOME);
		return;
	}
	if (ctrl && ks == XK_f) {
		focus_find();
		return;
	}
	if (ks == XK_F3) {
		if (!find_on)
			focus_find();
		find_go(shift ? -1 : 1);
		return;
	}
	if (ctrl && shift && (ks == XK_J || ks == XK_j)) {
		if (shell_debug_js())
			ui_alert("no $SOIE_DEBUG_JS to run");
		return;
	}
	if (ks == XK_F12) {
		char path[700], *e;

		if (shell_dump(path, sizeof(path)))
			snprintf(path, sizeof(path), "cannot save the page");
		e = escape_html(path);
		ui_alert(e);
		free(e);
		return;
	}
	if (ks == XK_F5 || (ctrl && ks == XK_r)) {
		shell_reload();
		return;
	}
	if (alt && (ks == XK_Left || ks == XK_Right)) {
		shell_history(ks == XK_Left ? -1 : 1);
		return;
	}
	if ((ctrl && ks == XK_v) || (shift && ks == XK_Insert)) {
		paste(ctrl ? a_clipboard : XA_PRIMARY, field, e->time);
		return;
	}
	if (ctrl && ks == XK_c) {
		const char *v = field == F_URL ? soie_element_get_attr(
			find_id(chrome, "url"), "value") : field == F_FIND ?
			NULL : shell_field();

		if (v && *v)
			own(a_clipboard, v, e->time);
		return;
	}
	if (field != F_NONE && IsModifierKey(ks))
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
	shell_key(k, txt, (shift ? SOIE_MOD_SHIFT : 0) |
		(ctrl ? SOIE_MOD_CTRL : 0) | (alt ? SOIE_MOD_ALT : 0));
}

static void hover(int x, int y)
{
	soie_node *n;
	Cursor c = cur_arrow;
	char old[sizeof(status)], link[4096];
	int ty, th;

	snprintf(old, sizeof(old), "%s", status);
	*status = '\0';
	if (y < BAR) {
		n = soie_hit_test(chrome, (float)x, (float)y);
		if (ancestor(n, "input"))
			c = cur_text;
		else if (ancestor(n, "button"))
			c = cur_hand;
	} else if (find_on && y >= win_h - FINDH && x < find_w()) {
		n = soie_hit_test(findbar, (float)x, (float)(y - win_h + FINDH));
		if (ancestor(n, "input"))
			c = cur_text;
	} else if (!(x >= win_w - SB && thumb(&ty, &th))) {
		int r = shell_hover(x, y - BAR, link, sizeof(link));

		if (*link) {
			char *e = escape_html(link);

			snprintf(status, sizeof(status), "%s", e);
			free(e);
		}
		c = r == SHELL_HAND ? cur_hand : r == SHELL_TEXT ? cur_text :
			cur_arrow;
	}
	XDefineCursor(dpy, win, c);
	if (strcmp(old, status))
		redraw();
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

static void drag_to(int y)
{
	int ty, th, scroll, height, view;

	if (!thumb(&ty, &th))
		return;
	shell_view(&scroll, &height, &view);
	shell_scroll_to((int)((double)(y - drag - BAR) * (height - view) /
		(view - th)));
}

static void button(XButtonEvent *b)
{
	int mods = (b->state & ShiftMask ? SOIE_MOD_SHIFT : 0) |
		(b->state & ControlMask ? SOIE_MOD_CTRL : 0);
	char link[4096];

	switch (b->button) {
	case 4: shell_scroll(-60); return;
	case 5: shell_scroll(60); return;
	case 8: shell_history(-1); return;
	case 9: shell_history(1); return;
	case 1: case 2: break;
	default: return;
	}
	if (b->y < BAR) {
		click_chrome(b->x, b->y, b->time, b->button == 1);
		if (b->button == 2 && field == F_URL)
			/* the selection replaces the address */
			paste(XA_PRIMARY, F_URL, b->time);
		return;
	}
	if (find_on && b->y >= win_h - FINDH && b->x < find_w()) {
		click_find(b->x, b->y - win_h + FINDH);
		if (b->button == 2 && field == F_FIND)
			paste(XA_PRIMARY, F_FIND, b->time);
		return;
	}
	if (field != F_NONE) {
		field = F_NONE;
		chrome_update();
		if (find_on)
			find_update();
	}
	if (b->button == 1 && click_scrollbar(b->x, b->y))
		return;
	if (b->button == 2) {
		/* a link opens in a new window, a field takes the
		 * selection */
		if (shell_hover(b->x, b->y - BAR, link, sizeof(link)) ==
		    SHELL_HAND && *link) {
			spawn(link);
			return;
		}
		shell_click(b->x, b->y - BAR, mods);
		if (shell_field())
			paste(XA_PRIMARY, F_NONE, b->time);
		return;
	}
	shell_click(b->x, b->y - BAR, mods);
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
	if (!rgba)
		abort();
	ximg = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
		ZPixmap, 0, (char *)rgba, (unsigned)win_w, (unsigned)win_h, 32,
		win_w * 4);
	chrome_update();
	find_update();
	shell_resize(win_w, win_h - BAR);
}

/* The window icon: an "S", painted by Soie, at 32 and 16 pixels. */
static void set_icon(void)
{
	static const int sizes[] = { 32, 16 };
	unsigned long *icon = malloc((2 + 32 * 32 + 2 + 16 * 16) *
		sizeof(*icon));
	int i, k = 0, j;

	if (!icon)
		return;
	for (i = 0; i < 2; i++) {
		int s = sizes[i];
		unsigned char px[32 * 32 * 4];
		char html[400];
		soie_node *d;

		snprintf(html, sizeof(html), "<body style='margin:0;"
			"background:#5a3e9c;color:white;text-align:center;"
			"font:bold %dpx/%dpx serif'>S", s * 3 / 4, s);
		d = soie_html_parse(html, strlen(html), NULL);
		soie_style_document(d, NULL);
		soie_layout_document(d, s, s);
		memset(px, 0, sizeof(px));
		soie_paint(d, px, s, s, s * 4, 0, 0);
		soie_node_free(d);
		icon[k++] = (unsigned long)s;
		icon[k++] = (unsigned long)s;
		for (j = 0; j < s * s; j++)
			icon[k++] = 0xff000000ul |
				(unsigned long)px[j * 4] << 16 |
				(unsigned long)px[j * 4 + 1] << 8 | px[j * 4 + 2];
	}
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", 0),
		XA_CARDINAL, 32, PropModeReplace, (unsigned char *)icon, k);
	free(icon);
}

/* UTF-8 keyboard input through the input method, when there is one. */
static void open_im(long *mask)
{
	long filter = 0;

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		setlocale(LC_CTYPE, "C");
	XSetLocaleModifiers("");
	if (!(xim = XOpenIM(dpy, NULL, NULL, NULL))) {
		XSetLocaleModifiers("@im=none");
		xim = XOpenIM(dpy, NULL, NULL, NULL);
	}
	if (!xim)
		return;
	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing |
		XIMStatusNothing, XNClientWindow, win, XNFocusWindow, win,
		(char *)NULL);
	if (xic && !XGetICValues(xic, XNFilterEvents, &filter, (char *)NULL))
		*mask |= filter;
}

int main(int argc, char **argv)
{
	const char *start = SHELL_HOME, *out = NULL;
	XEvent ev;
	Atom wm_delete;
	XClassHint class = { "xsoie", "Xsoie" };
	long mask = ExposureMask | KeyPressMask | ButtonPressMask |
		ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
		FocusChangeMask;
	int i, js = 1, fd;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w") && i + 1 < argc)
			win_w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-H") && i + 1 < argc)
			win_h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "-s"))
			js = 0;
		else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: xsoie [-s] [-w W] [-H H] "
				"[-o out.png] [URL]\n");
			return 2;
		} else
			start = argv[i];
	}
	fd = shell_init(js);
	if (fd < 0) {
		fputs("xsoie: cannot start the fetch threads\n", stderr);
		return 1;
	}
	chrome = soie_html_parse(chrome_html, strlen(chrome_html), NULL);
	findbar = soie_html_parse(find_html, strlen(find_html), NULL);
	if (out) {
		shell_resize(win_w, win_h - BAR);
		return shell_render_png(start, out);
	}
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fputs("xsoie: cannot open the display\n", stderr);
		return 1;
	}
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_xsel = XInternAtom(dpy, "XSOIE_SELECTION", False);
	win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0,
		(unsigned)win_w, (unsigned)win_h, 0, 0, 0xffffff);
	open_im(&mask);
	XSelectInput(dpy, win, mask);
	XSetClassHint(dpy, win, &class);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);
	set_icon();
	gc = XCreateGC(dpy, win, 0, NULL);
	cur_arrow = XCreateFontCursor(dpy, XC_left_ptr);
	cur_hand = XCreateFontCursor(dpy, XC_hand2);
	cur_text = XCreateFontCursor(dpy, XC_xterm);
	XMapWindow(dpy, win);
	resize(win_w, win_h);
	shell_load(start);
	redraw();
	for (;;) {
		/* timers run while no X event waits */
		while (!XPending(dpy)) {
			struct pollfd p[2];
			const char *text;
			int repaint, t = shell_tick(&repaint);

			if (repaint == 2 && !*status) {
				redraw_rect();
			} else if (repaint) {
				if (find_on)
					find_go(0);
				redraw();
			}
			if (XPending(dpy))
				break;
			p[0].fd = ConnectionNumber(dpy);
			p[0].events = POLLIN;
			p[1].fd = fd;
			p[1].events = POLLIN;
			if (shell_loading(&text)) {
				/* the loading line: keep it current */
				static struct timespec shown;
				struct timespec now;

				if (t < 0 || t > 500)
					t = 500;
				clock_gettime(CLOCK_MONOTONIC, &now);
				if ((now.tv_sec - shown.tv_sec) * 1000 +
				    (now.tv_nsec - shown.tv_nsec) / 1000000 > 500) {
					shown = now;
					redraw();
				}
			}
			poll(p, 2, t);
		}
		XNextEvent(dpy, &ev);
		if (XFilterEvent(&ev, None))
			continue;
		switch (ev.type) {
		case Expose:
			if (!ev.xexpose.count)
				redraw();
			break;
		case ConfigureNotify:
			/* a window dragged bigger sends many: the last one */
			while (XCheckTypedWindowEvent(dpy, win, ConfigureNotify,
			    &ev))
				;
			if (ev.xconfigure.width != win_w ||
			    ev.xconfigure.height != win_h) {
				resize(ev.xconfigure.width, ev.xconfigure.height);
				redraw();
			}
			break;
		case FocusIn:
			if (xic)
				XSetICFocus(xic);
			break;
		case FocusOut:
			if (xic)
				XUnsetICFocus(xic);
			break;
		case ButtonPress:
			button(&ev.xbutton);
			redraw();
			break;
		case ButtonRelease:
			if (drag >= 0) {
				drag = -1;
				redraw();
			}
			break;
		case MotionNotify:
			/* only the last of consecutive moves */
			while (XPending(dpy)) {
				XEvent next;

				XPeekEvent(dpy, &next);
				if (next.type != MotionNotify)
					break;
				XNextEvent(dpy, &ev);
			}
			if (drag >= 0) {
				drag_to(ev.xmotion.y);
				redraw();
			} else {
				hover(ev.xmotion.x, ev.xmotion.y);
			}
			break;
		case KeyPress:
			key(&ev.xkey);
			redraw();
			break;
		case SelectionNotify:
			pasted(&ev.xselection);
			break;
		case SelectionRequest:
			give(&ev.xselectionrequest);
			break;
		case SelectionClear:
			i = ev.xselectionclear.selection == XA_PRIMARY ? 0 : 1;
			free(sel_text[i]);
			sel_text[i] = NULL;
			break;
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == wm_delete)
				return 0;
			break;
		}
	}
}
