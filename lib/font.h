/*
 * Fonts and shaping internals (font.c, text.c). Never installed.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_FONT_H
#define SOIE_FONT_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#ifdef SOIE_HARFBUZZ
#include <hb.h>
#include <hb-ft.h>
#endif
#include "layout.h"

struct face {
	char *family;		/* lowercase */
	char *path;		/* system font, or */
	unsigned char *data;	/* web font bytes */
	size_t len;
	long index;
	int weight, italic;
	const void *doc;	/* web font: its document */
	FT_Face ft;
	int upem;
	int failed;
#ifdef SOIE_HARFBUZZ
	hb_font_t *hb;
#endif
};

extern struct face *faces;
extern int nfaces;
extern const void *font_doc;	/* document laid out or painted */

int fonts_stub(void);
void fonts_scan(const char *extra);
int face_open(struct face *x);
void face_close(struct face *x);
struct face *font_add_memory(const void *doc, const char *family,
	unsigned char *data, size_t len, int weight, int italic);
void fonts_drop_document(const void *doc);
void font_cache_clear(void);
void width_cache_clear(void);
int font_pick(const struct style *st, unsigned long cp);
int font_primary(const struct style *st);
void face_metrics(int fi, float size, struct fontm *m);

/* A shaped glyph, positions in px from the start of the text. */
struct glyph {
	int face;		/* -1: stub font, draw a box */
	unsigned id;
	float x, y;		/* pen position plus offsets */
	float adv;
	unsigned cluster;	/* byte offset in the text */
};

int text_shape(const struct style *st, const char *s, size_t len,
	struct glyph **out);

#endif
