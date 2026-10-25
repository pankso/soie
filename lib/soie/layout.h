/*
 * Soie layout: boxes, lines and their geometry.
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
 * are sent. Since 1.3. */
int soie_paint_bgra(soie_node *doc, unsigned char *bgra, int width,
	int height, int stride, int sx, int sy);

/*
 * What changed in the painting since the last call: up to max rects
 * (x, y, w, h each, page coordinates) into rects, their number (more
 * become their union); -1 when it may be all of it (a first layout,
 * the viewport changed, the root's or body's style). The first call starts the tracking (and returns -1): each
 * layout then compares the boxes it leaves with those it found, the
 * focus and caret count too. Images and fonts that arrive later are
 * not seen: a host paints all of it then. Since 1.3.
 */
int soie_layout_damage(soie_node *doc, float *rects, int max);

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
 * Since 1.1.
 */
int soie_find(soie_node *doc, const char *text, int n, float *x, float *y,
	float *w, float *h);

/*
 * The focused form control (or NULL) and the caret, a byte offset in
 * its value: soie_paint() draws the caret there.
 */
void soie_set_focus(soie_node *doc, soie_node *el, int caret);
/* The focused control (set by the host or a script's focus()) and its
 * caret. Since 1.1. */
soie_node *soie_get_focus(const soie_node *doc, int *caret);

/*
 * Fonts: system fonts are found under /usr/share/fonts and ~/.fonts;
 * add_dir() adds a directory (returns the number of faces found).
 * stub(1) replaces every font by a fixed-advance stub (0.5em per
 * character), for layout tests that must not depend on the fonts.
 */
int soie_fonts_add_dir(const char *dir);
void soie_fonts_stub(int on);

#endif
