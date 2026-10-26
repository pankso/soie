/*
 * Painting internals (paint.c, image.c). Never installed.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
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
	/* mask-image: the alpha of pixels mx0..mx1, my0..my1 (0 out of
	 * them), NULL without a mask */
	unsigned char *mask;
	int mx0, my0, mx1, my1;
};

/* The mask's alpha at pixel x, y; 255 without one */
static inline int mask_at(const struct canvas *c, int x, int y)
{
	if (!c->mask)
		return 255;
	if (x < c->mx0 || y < c->my0 || x >= c->mx1 || y >= c->my1)
		return 0;
	return c->mask[(size_t)(y - c->my0) * (size_t)(c->mx1 - c->mx0) +
		(size_t)(x - c->mx0)];
}

extern soie_node *paint_doc;

void fill_rect(struct canvas *c, float x, float y, float w, float h,
	const unsigned char *rgba);
void paint_replaced(struct canvas *c, const struct box *b);
void paint_bg_image(struct canvas *c, const struct box *b, float x,
	float y, float w, float h);
void paint_layers_of(struct canvas *c, const struct box *b, int img_p,
	float x, float y, float w, float h);
void glyph_cache_clear(void);
void paint_text(struct canvas *c, const struct style *st, const char *s,
	size_t len, float x, float base);
void paint_control(struct canvas *c, const struct box *b);

#endif
