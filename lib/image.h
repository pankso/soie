/*
 * Images internals: decoders, the per-document cache, SVG. Never
 * installed.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_IMAGE_H
#define SOIE_IMAGE_H

#include "paint.h"

struct image {
	char *url;
	int w, h;
	unsigned char *rgba;	/* straight alpha; NULL for SVG */
	soie_node *svgdoc;	/* SVG: its own document */
	const soie_node *svg;	/* its <svg> element */
	int failed;
	/* Large images keep their encoded bytes and are decoded when
	 * painted, within a budget (see image_pixels()) */
	unsigned char *data;
	size_t len;
	unsigned long used;
	struct images *owner;
};

/* The per-document image cache, made by soie_style_document(). */
struct images {
	struct image *v;
	int n, cap;
	soie_fetch_fn fetch;
	void *data;
	char *base;
	size_t pixels;		/* bytes of large images decoded now */
	unsigned long tick;
};

void images_setup(soie_node *doc, soie_fetch_fn fetch, void *data,
	const char *base);
void images_free(struct images *im);
const struct image *image_get(soie_node *doc, const char *url);
const struct image *image_for(const soie_node *el);
void draw_image(struct canvas *c, const struct image *im, float x, float y,
	float w, float h, const unsigned char *current);
void svg_render(struct canvas *c, const soie_node *el, float x, float y,
	float w, float h, const unsigned char *current);
void svg_size(const soie_node *el, float *w, float *h);
int decode_image(const unsigned char *data, size_t len, struct image *im);
void draw_fitted(struct canvas *c, const struct box *b,
	const struct image *im, const unsigned char *current);
void paint_media(struct canvas *c, const struct box *b);

#endif
