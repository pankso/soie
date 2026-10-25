/*
 * The browser's shell: pages and their resources, history, forms,
 * scripts and find, without a toolkit. A front (front.h, xsoie on
 * Xlib) shows the page, its address and title, and feeds the input in. Coordinates
 * are the page view's, its top left corner at 0,0.
 */
#ifndef SOIE_SHELL_H
#define SOIE_SHELL_H

#include <stddef.h>
#include <soie/soie.h>

#define SHELL_HOME "about:soie"

/* What is under the pointer */
enum { SHELL_ARROW, SHELL_HAND, SHELL_TEXT };

/* Start the fetch threads; returns the descriptor to poll, readable
 * when resources arrive, -1 on failure. scripts: 0 turns them off. */
int shell_init(int scripts);
/* The size of the page view. */
void shell_resize(int w, int h);
/* Run what is due: arrived resources, late layouts, timers, media, a
 * meta refresh, scripts' navigations. *repaint is 1 when the page
 * changed, 2 when only the rectangle shell_dirty() gives did (video
 * frames). Returns ms until the next call is due, -1 without a timer. */
int shell_tick(int *repaint);
/* The front's pixels are B G R A, not R G B A (the default). */
void shell_bgra(int on);
/* Paint the page view into rgba (stride bytes a row). */
void shell_paint(unsigned char *rgba, int stride);
/* After shell_tick() said repaint 1: paint only what changed in the
 * page, when the shell knows (the engine's damage); returns how many
 * rects of the view (x, y, w, h, at most max) it painted, -1 when it
 * does not know: shell_paint() then. */
int shell_paint_changed(unsigned char *rgba, int stride, int (*rects)[4],
	int max);
/* rgba holds the last shell_paint(): when the page only scrolled since,
 * move its pixels and paint what came into view, *dy the move (0: the
 * view is as it was). Returns 0 when it cannot: shell_paint() then. */
int shell_paint_scroll(unsigned char *rgba, int stride, int *dy);
/* After a repaint of 2: the view's rectangle to paint again; 0 when
 * it is out of view. */
int shell_dirty(int *x, int *y, int *w, int *h);
/* Paint that rectangle of the view only, at its place in rgba. */
void shell_paint_rect(unsigned char *rgba, int stride, int x, int y, int w,
	int h);

/* Navigation */
void shell_load(const char *url);
/* What was typed in an address bar: a URL, a path, a host, or words
 * to search for ($SOIE_SEARCH, a URL with %s, or DuckDuckGo Lite). */
void shell_go(const char *typed);
void shell_history(int delta);
void shell_reload(void);
const char *shell_url(void);
/* Resources on their way, the page itself counting as one; *text is a
 * line to show while loading. */
int shell_loading(const char **text);

/* Input. mods are SOIE_MOD_*. Keys are DOM key values ("Enter",
 * "ArrowLeft", "a"), text what the key typed, in UTF-8. */
void shell_click(int x, int y, int mods);
/* 1 when the page took the key, 2 when it only scrolled the page */
int shell_key(const char *key, const char *text, int mods);
/* The cursor for x,y; the link under it in link, or "". */
int shell_hover(int x, int y, char *link, size_t size);
void shell_scroll(int dy);
void shell_scroll_to(int y);
/* The scroll position, the page height and the view's. */
void shell_view(int *scroll, int *height, int *view);
/* The focused page control loses the focus. */
void shell_blur(void);
/* Edit the value attribute of el (an input) at caret *cp with a key;
 * 1 when it changed. The front's own text fields use it too. */
int shell_edit(soie_node *el, int *cp, const char *key, const char *text);
/* Into the focused text control: 1 when there is one. */
int shell_paste(const char *text);
/* The value of the focused text control, NULL without one. */
const char *shell_field(void);

/* Find in the page: a new text shows its first match; with the same
 * text, dir 1 the next one, -1 the previous, 0 counts again (after a
 * layout). Returns the number of matches, the current one in *cur
 * (from 1); it shows highlighted, scrolled to. NULL or "" ends it. */
int shell_find(const char *text, int dir, int *cur);

/* Offscreen: load url, wait for it and its scripts, render to a PNG. */
int shell_render_png(const char *url, const char *path);

/* Save the page for a bug report in ~/.cache/soie (page-TIME.html and
 * .txt); path gets the .txt file's. */
int shell_dump(char *path, size_t size);
/* Run the file $SOIE_DEBUG_JS names in the page. */
int shell_debug_js(void);

/* Front calls, defined by the front */
void ui_title(const char *title);
/* A load started or a page arrived: show its address. */
void ui_address(const char *url);
/* A script's alert */
void ui_alert(const char *text);

#endif
