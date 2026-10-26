/*
 * The browser's shell: pages and their resources, history, forms,
 * scripts and find, without a toolkit. Each shell is a page view with
 * its own history (a window, a tab); a front (front.h: xsoie on Xlib,
 * wsoie on Wayland; a toolkit's widget) shows its page, address and
 * title, and feeds the input in. All shells live in one thread, the
 * one that called shell_init(). Coordinates are the page view's, its
 * top left corner at 0,0.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_SHELL_H
#define SOIE_SHELL_H

#include <stddef.h>
#include <soie/soie.h>

#define SHELL_HOME "about:soie"

/* What is under the pointer */
enum { SHELL_ARROW, SHELL_HAND, SHELL_TEXT };

typedef struct shell shell;

/* What a shell tells its front, with shell_new()'s data; any may be
 * NULL */
struct shell_ui {
	void (*title)(void *data, const char *title);
	/* a load started or a page arrived: show its address */
	void (*address)(void *data, const char *url);
	/* a script's alert */
	void (*alert)(void *data, const char *text);
	/* a new window (tab) on url: a link of target _blank, a script's
	 * window.open() for the user's click or key; NULL: the link
	 * opens here, the script's window not at all */
	void (*open)(void *data, const char *url);
	/* a download (id, from url, of type, total bytes or -1) starts:
	 * dest holds where it goes (~/Downloads/its name, a free one),
	 * which the front may change, "" not at all; NULL: there, said
	 * through alert. It is then the process's (shell_downloads()). */
	void (*download)(void *data, unsigned id, const char *url,
		const char *type, long long total, char *dest, size_t size);
};

/* Load the extensions, start the fetch threads, once; returns the
 * descriptor to poll, readable when resources arrive, -1 on failure.
 * It is all shells': when it is readable, shell_tick() each. */
int shell_init(void);
/* What came on the descriptor to the shells that asked for it, the
 * rest dropped; shell_tick() does it, a front without a shell (all
 * closed) calls it instead. */
void shell_route(void);
/*
 * Downloads are the process's, not their shell's: freeing it does not
 * stop them. progress: how far one is, every quarter of a second while
 * it grows (any shell's tick); done: it ended, error NULL when the file
 * is at its destination. Without done, its shell's alert says it.
 */
void shell_downloads(void (*progress)(void *data, unsigned id,
	long long got, long long total), void (*done)(void *data,
	unsigned id, const char *error), void *data);
/* Stop download id (a ui download() call gave it) */
void shell_download_cancel(unsigned id);
/*
 * A URL scheme of the front's own ("toile"): fn answers url (with its
 * query) in the shells' thread, *body a malloc()ed page of *len bytes,
 * its Content-Type in type (size bytes; "" is text/html); nonzero when
 * there is none. The front's pages, what the user types and history
 * reach it, other pages cannot (no link, resource or script request
 * from the web). Up to 8 schemes; again with the same name replaces.
 */
typedef int (*shell_scheme_fn)(void *data, const char *url, char **body,
	size_t *len, char *type, size_t size);
void shell_scheme(const char *name, shell_scheme_fn fn, void *data);
/* May the page of s lead to url (a link the front opens itself, the
 * middle button's)? Not to a front's scheme from the web. */
int shell_may_open(shell *s, const char *url);
/* A new page view, empty until a load; scripts: 0 turns them off. ui
 * is copied. */
shell *shell_new(const struct shell_ui *ui, void *data, int scripts);
/* Its page and scripts stop; not from inside its own ui calls. */
void shell_free(shell *s);
/* A private window's view, before its first load: its requests and
 * scripts use the private cookie jar (in memory only, shared by the
 * private views, empty at start). */
void shell_private(shell *s);
/* The size of the page view. */
void shell_resize(shell *s, int w, int h);
/* The page's zoom: device px a CSS px, 1 by default, 0.1 to 10; a
 * screen's scale times the user's zoom. */
void shell_zoom(shell *s, float zoom);
float shell_get_zoom(shell *s);
/* Run what is due: arrived resources, late layouts, timers, media, a
 * meta refresh, scripts' navigations. *repaint is 1 when the page
 * changed, 2 when only the rectangle shell_dirty() gives did (video
 * frames). Returns ms until the next call is due, -1 without a timer. */
int shell_tick(shell *s, int *repaint);
/* The front's pixels are B G R A, not R G B A (the default). */
void shell_bgra(shell *s, int on);
/* Paint the page view into rgba (stride bytes a row). */
void shell_paint(shell *s, unsigned char *rgba, int stride);
/* After shell_tick() said repaint 1: paint only what changed in the
 * page, when the shell knows (the engine's damage); returns how many
 * rects of the view (x, y, w, h, at most max) it painted, -1 when it
 * does not know: shell_paint() then. */
int shell_paint_changed(shell *s, unsigned char *rgba, int stride,
	int (*rects)[4], int max);
/* rgba holds the last shell_paint(): when the page only scrolled since,
 * move its pixels and paint what came into view, *dy the move (0: the
 * view is as it was). Returns 0 when it cannot: shell_paint() then. */
int shell_paint_scroll(shell *s, unsigned char *rgba, int stride,
	int *dy);
/* After a repaint of 2: the view's rectangle to paint again; 0 when
 * it is out of view. */
int shell_dirty(shell *s, int *x, int *y, int *w, int *h);
/* Paint that rectangle of the view only, at its place in rgba. */
void shell_paint_rect(shell *s, unsigned char *rgba, int stride, int x,
	int y, int w, int h);

/* Navigation */
void shell_load(shell *s, const char *url);
/* Show html as the page at url (its base; "about:blank" for NULL),
 * a new history entry. */
void shell_load_html(shell *s, const char *html, const char *url);
/* What was typed in an address bar: a URL, a path, a host, or words
 * to search for ($SOIE_SEARCH, a URL with %s, or DuckDuckGo Lite). */
void shell_go(shell *s, const char *typed);
void shell_history(shell *s, int delta);
/* 1 when shell_history(s, delta) has a page to go to */
int shell_can_go(shell *s, int delta);
void shell_reload(shell *s);
const char *shell_url(shell *s);
/* Resources on their way, the page itself counting as one; *text is a
 * line to show while loading. */
int shell_loading(shell *s, const char **text);

/* Input. mods are SOIE_MOD_*. Keys are DOM key values ("Enter",
 * "ArrowLeft", "a"), text what the key typed, in UTF-8. */
void shell_click(shell *s, int x, int y, int mods);
/* 1 when the page took the key, 2 when it only scrolled the page, 0
 * when it is the front's (with Ctrl, Alt or Meta: its shortcuts) */
int shell_key(shell *s, const char *key, const char *text, int mods);
/* The cursor for x,y; the link under it in link, or "". */
int shell_hover(shell *s, int x, int y, char *link, size_t size);
/* What is at x,y, for a context menu: the link and the image there
 * (absolute URLs, "" for none), 1 when it is a text field. */
int shell_hit(shell *s, int x, int y, char *link, char *image,
	size_t size);
void shell_scroll(shell *s, int dy);
void shell_scroll_to(shell *s, int y);
/* The scroll position, the page height and the view's. */
void shell_view(shell *s, int *scroll, int *height, int *view);
/* The focused page control loses the focus. */
void shell_blur(shell *s);
/* Edit the value attribute of el (an input) at caret *cp with a key;
 * 1 when it changed. The front's own text fields use it too. */
int shell_edit(soie_node *el, int *cp, const char *key, const char *text);
/* Into the focused text control: 1 when there is one. */
int shell_paste(shell *s, const char *text);
/* The value of the focused text control, NULL without one. */
const char *shell_field(shell *s);

/* Find in the page: a new text shows its first match; with the same
 * text, dir 1 the next one, -1 the previous, 0 counts again (after a
 * layout). Returns the number of matches, the current one in *cur
 * (from 1); it shows highlighted, scrolled to. NULL or "" ends it. */
int shell_find(shell *s, const char *text, int dir, int *cur);

/* Select text: from view x,y (unit SOIE_SELECT_CHAR, _WORD, _BLOCK for
 * a single, double, triple click; none starts on a link or a field,
 * theirs), then to x,y as the pointer moves. A click clears it. They
 * return 1 when the view must be painted again (shell_paint()). */
int shell_select_start(shell *s, int x, int y, int unit);
int shell_select_to(shell *s, int x, int y);
void shell_select_all(shell *s);
int shell_select_clear(shell *s);
/* Its text, malloc()ed; NULL without one. */
char *shell_selection(shell *s);

/* Offscreen: load url, wait for it and its scripts, render to a PNG. */
int shell_render_png(shell *s, const char *url, const char *path);

/* Save the page for a bug report in ~/.cache/soie (page-TIME.html and
 * .txt); path gets the .txt file's. */
int shell_dump(shell *s, char *path, size_t size);
/* Run the file $SOIE_DEBUG_JS names in the page. */
int shell_debug_js(shell *s);

#endif
