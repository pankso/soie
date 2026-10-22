/*
 * Images internals: decoders, the per-document cache, SVG. Never
 * installed.
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
};

/* The per-document image cache, made by soie_style_document(). */
struct images {
	struct image *v;
	int n, cap;
	soie_fetch_fn fetch;
	void *data;
	char *base;
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

#endif
