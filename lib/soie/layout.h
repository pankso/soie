/*
 * Soie layout: boxes, lines and their geometry.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_LAYOUT_H
#define SOIE_LAYOUT_H

#include <stddef.h>
#include "dom.h"

/*
 * Lay out a styled document (soie_style_document() first) for a
 * viewport of width x height CSS px (0: 1024x768). Running the
 * cascade again drops the layout.
 */
int soie_layout_document(soie_node *doc, int width, int height);

/*
 * After changes to the tree (a script), run the cascade and the layout
 * again with the options and viewport of the last calls; style sheets
 * are not parsed again unless <style>, <link> or <base> changed.
 * Returns 1 when something was redone, 0 when all was up to date.
 */
int soie_document_update(soie_node *doc);

/*
 * The host's scroll offset, the page point at the view's top left
 * corner: sticky and fixed boxes follow it. To call when it changes,
 * before painting; what moved is in soie_layout_damage(). Layouts keep
 * the offset.
 */
void soie_layout_scroll(soie_node *doc, float sx, float sy);

/* Height of the laid out page, in CSS px. */
float soie_layout_height(const soie_node *doc);

/*
 * The box tree as text, one "| " line per box, line box and fragment,
 * with border-box geometry "x,y wxh". For tests and debugging; the
 * format may change. Returns a string to free().
 */
char *soie_layout_dump(const soie_node *doc, size_t *len);

/*
 * Paint the laid out page into rgba (width x height pixels, R G B A
 * bytes, stride bytes per row), the pixel (0, 0) showing page point
 * (sx, sy): scrolling is the host's offset. Returns 0, or -1 when doc
 * has no layout.
 */
int soie_paint(soie_node *doc, unsigned char *rgba, int width, int height,
	int stride, int sx, int sy);
/* The same in B G R A bytes, the order of the 32-bit pixels of X and
 * Wayland servers on little-endian machines: no conversion before they
 * are sent. */
int soie_paint_bgra(soie_node *doc, unsigned char *bgra, int width,
	int height, int stride, int sx, int sy);

/*
 * Iframes: the host loads and paints their pages, the engine shows the
 * pixels in the iframe's content box, in its paint order (clips,
 * masks, what lies over it). A paint that reaches an iframe of doc
 * asks fn for them, with the content box (x, y, w, h, page
 * coordinates in device pixels) and the paint's byte order (bgra 1:
 * B G R A); fn answers opaque pixels in that order, w x h tight rows,
 * kept until the paint returns, or NULL (nothing, the box stays
 * empty). Being asked is how the host learns that an iframe shows,
 * and at which size: what it answers may be of the size before, until
 * it has painted the new one. NULL fn: iframes stay empty.
 */
typedef const unsigned char *(*soie_frame_fn)(void *data, soie_node *el,
	const float *box, int bgra, int *w, int *h);
void soie_document_frames(soie_node *doc, soie_frame_fn fn, void *data);

/*
 * What changed in the painting since the last call: up to max rects
 * (x, y, w, h each, page coordinates) into rects, their number (more
 * become their union); -1 when it may be all of it (a first layout,
 * the viewport changed, the root's or body's style). The first call starts the tracking (and returns -1): each
 * layout then compares the boxes it leaves with those it found, the
 * focus and caret count too. Images that arrive later are in it after
 * soie_layout_images() or a layout; fonts are not seen: a host paints
 * all of it then.
 */
int soie_layout_damage(soie_node *doc, float *rects, int max);

/*
 * Ask again for the images the fetch callback answered
 * SOIE_FETCH_LATER for; a paint never does, so that all of a page
 * shows an image from the same moment. Returns 0 when none came; 1
 * when some did and every box keeps its size: the boxes showing them
 * are in soie_layout_damage(), painting them is enough; 2 when a box
 * has another size: soie_layout_document() is to be called (the
 * damage is then the layout's). A layout asks for them too.
 */
int soie_layout_images(soie_node *doc);

/* Deepest element under page point (x, y), or NULL. */
soie_node *soie_hit_test(soie_node *doc, float x, float y);

/*
 * Border box of el in page coordinates; for an inline element, the
 * union of its fragments. Returns -1 when el has no box.
 */
int soie_element_rect(const soie_node *el, float *x, float *y, float *w,
	float *h);

/*
 * Find text in the laid out page (ASCII case folded; a match does not
 * span two elements or lines). Returns the number of matches and puts
 * the rect of match n (from 0), in page coordinates, in x, y, w, h.
 */
int soie_find(soie_node *doc, const char *text, int n, float *x, float *y,
	float *w, float *h);

/*
 * Text selection between two page points, points[0..1] where the
 * button went down and points[2..3] where it is now (or the other way):
 * each stands for the nearest character of the nearest text, snapped
 * to words (SOIE_SELECT_WORD) or to its blocks (SOIE_SELECT_BLOCK);
 * SOIE_SELECT_ALL is the whole page. List markers and generated
 * content are not in it. soie_select() puts the rects of its text, in
 * page coordinates, in rects (x, y, w, h, at most max) and returns how
 * many there are, 0 for an empty selection; soie_select_text() returns
 * its text (malloc()ed, NULL when empty), lines and blocks ended by
 * newlines.
 */
enum { SOIE_SELECT_CHAR, SOIE_SELECT_WORD, SOIE_SELECT_BLOCK,
	SOIE_SELECT_ALL };
int soie_select(soie_node *doc, const float *points, int unit, float *rects,
	int max);
char *soie_select_text(soie_node *doc, const float *points, int unit);

/*
 * The focused form control (or NULL) and the caret, a byte offset in
 * its value: soie_paint() draws the caret there. With a range, the
 * text from anchor to caret is selected (soie_set_focus(): none).
 */
void soie_set_focus(soie_node *doc, soie_node *el, int caret);
void soie_set_focus_range(soie_node *doc, soie_node *el, int anchor,
	int caret);
/* The focused control (set by the host or a script's focus()) and its
 * caret; the anchor of its selection (the caret: none). */
soie_node *soie_get_focus(const soie_node *doc, int *caret);
int soie_get_focus_anchor(const soie_node *doc);
/* The byte offset in the value of text control el (input, textarea)
 * nearest to page point x, y, as soie_paint() shows it; -1 when it
 * has no box. */
int soie_field_offset(soie_node *doc, soie_node *el, float x, float y);
/* The caret at byte offset off of textarea el moved by the lines it
 * shows (wrapped): to the nearest offset under the same x on the line
 * above or below (past the first or last line: the start or the end of
 * the text), or to the start or end of its line. -1 when el is not a
 * textarea with a box. */
enum { SOIE_FIELD_UP, SOIE_FIELD_DOWN, SOIE_FIELD_HOME, SOIE_FIELD_END };
int soie_field_move(soie_node *doc, soie_node *el, int off, int how);

/*
 * Fonts: system fonts are found under /usr/share/fonts and ~/.fonts;
 * add_dir() adds a directory (returns the number of faces found).
 * stub(1) replaces every font by a fixed-advance stub (0.5em per
 * character), for layout tests that must not depend on the fonts.
 */
int soie_fonts_add_dir(const char *dir);
void soie_fonts_stub(int on);

#endif
