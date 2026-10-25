/*
 * Painting internals (paint.c, image.c). Never installed.
 */
#ifndef SOIE_PAINT_H
#define SOIE_PAINT_H

#include "layout.h"

struct canvas {
	unsigned char *px;	/* RGBA, or BGRA when bgr */
	int w, h, stride;
	float ox, oy;		/* page coordinates of pixel (0, 0) */
	int cx0, cy0, cx1, cy1;	/* clip, in pixels */
	float alpha;		/* opacity of what is painted now */
	int bgr;		/* red and blue swapped in px */
};

extern soie_node *paint_doc;

void fill_rect(struct canvas *c, float x, float y, float w, float h,
	const unsigned char *rgba);
void paint_replaced(struct canvas *c, const struct box *b);
void paint_bg_image(struct canvas *c, const struct box *b, float x,
	float y, float w, float h);
void glyph_cache_clear(void);
void paint_text(struct canvas *c, const struct style *st, const char *s,
	size_t len, float x, float base);
void paint_control(struct canvas *c, const struct box *b);

#endif
