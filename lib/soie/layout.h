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

/* Height of the laid out page, in CSS px. */
float soie_layout_height(const soie_node *doc);

/*
 * The box tree as text, one "| " line per box, line box and fragment,
 * with border-box geometry "x,y wxh". For tests and debugging; the
 * format may change. Returns a string to free().
 */
char *soie_layout_dump(const soie_node *doc, size_t *len);

#endif
