/*
 * Text measurement and shaping. With fonts: HarfBuzz (or FreeType's
 * cmap, advances and kerning without it), at unhinted design units
 * scaled to the font size. Without fonts, or for tests, a stub: every
 * character is 0.5em wide, ascent 0.75em, descent 0.25em, normal
 * line-height 1.25em.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "font.h"

static float font_size(const struct style *st)
{
	return st->v[P_FONT_SIZE].u.len.px;
}

void font_metrics(const struct style *st, struct fontm *m)
{
	float em = font_size(st);
	int f;

	if (fonts_stub() || (f = font_primary(st)) < 0) {
		m->ascent = em * 0.75f;
		m->descent = em * 0.25f;
		m->normal_lh = em * 1.25f;
		return;
	}
	face_metrics(f, em, m);
}

static unsigned long utf8_cp(const char *s, size_t len, size_t *i)
{
	unsigned long c = (unsigned char)s[(*i)++];
	int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

	if (n)
		c &= 0x3f >> n;
	while (n-- && *i < len)
		c = c << 6 | ((unsigned char)s[(*i)++] & 0x3f);
	return c;
}

static void spacing(const struct style *st, float *ls, float *ws)
{
	*ls = st->v[P_LETTER_SPACING].kw ? 0 :
		st->v[P_LETTER_SPACING].u.len.px;
	*ws = st->v[P_WORD_SPACING].kw ? 0 : st->v[P_WORD_SPACING].u.len.px;
}

/* Room for glyph n; returns it (the array may move). */
static struct glyph *add_glyph(struct glyph **g, int n, int *cap)
{
	if (n == *cap) {
		*cap = *cap ? *cap * 2 : 32;
		*g = xrealloc(*g, (size_t)*cap * sizeof(**g));
	}
	return &(*g)[n];
}

/* Glyphs of s[from, to) in face fi, appended at pen x. */
static int shape_run(int fi, float em, const char *s, size_t len,
	size_t from, size_t to, struct glyph **g, int n, int *cap,
	float *pen)
{
	struct face *x = &faces[fi];
	float scale = em / (float)x->upem;
#ifdef SOIE_HARFBUZZ
	hb_buffer_t *buf;
	hb_glyph_info_t *info;
	hb_glyph_position_t *pos;
	unsigned i, count;

	if (!x->hb) {
		hb_face_t *hf = hb_ft_face_create_referenced(x->ft);

		x->hb = hb_font_create(hf);
		hb_face_destroy(hf);
	}
	buf = hb_buffer_create();
	hb_buffer_add_utf8(buf, s, (int)len, (unsigned)from,
		(int)(to - from));
	hb_buffer_guess_segment_properties(buf);
	hb_shape(x->hb, buf, NULL, 0);
	info = hb_buffer_get_glyph_infos(buf, &count);
	pos = hb_buffer_get_glyph_positions(buf, &count);
	for (i = 0; i < count; i++) {
		struct glyph *gl = add_glyph(g, n, cap);

		gl->face = fi;
		gl->id = info[i].codepoint;
		gl->cluster = info[i].cluster;
		gl->x = *pen + (float)pos[i].x_offset * scale;
		gl->y = -(float)pos[i].y_offset * scale;
		gl->adv = (float)pos[i].x_advance * scale;
		gl->em = em;
		*pen += gl->adv;
		n++;
	}
	hb_buffer_destroy(buf);
#else
	FT_UInt prev = 0;
	size_t i = from;

	(void)len;
	while (i < to) {
		size_t start = i;
		unsigned long cp = utf8_cp(s, to, &i);
		FT_UInt gid = FT_Get_Char_Index(x->ft, cp);
		FT_Fixed adv = 0;
		struct glyph *gl;

		if (prev && gid && FT_HAS_KERNING(x->ft)) {
			FT_Vector k;

			if (!FT_Get_Kerning(x->ft, prev, gid,
					    FT_KERNING_UNSCALED, &k))
				*pen += (float)k.x * scale;
		}
		FT_Get_Advance(x->ft, gid, FT_LOAD_NO_SCALE, &adv);
		gl = add_glyph(g, n, cap);
		gl->face = fi;
		gl->id = gid;
		gl->cluster = (unsigned)start;
		gl->x = *pen;
		gl->y = 0;
		gl->adv = (float)adv * scale;
		gl->em = em;
		*pen += gl->adv;
		prev = gid;
		n++;
	}
#endif
	return n;
}

/* font-variant-caps: 1 small-caps (petite-caps falls back to it, CSS
 * Fonts 3 6.3), 2 all-small-caps, else 0 */
static int small_caps(const struct style *st)
{
	if (kw(st, P_FONT_VARIANT_CAPS, "normal"))
		return 0;
	if (kw(st, P_FONT_VARIANT_CAPS, "small-caps") ||
	    kw(st, P_FONT_VARIANT_CAPS, "petite-caps"))
		return 1;
	return kw(st, P_FONT_VARIANT_CAPS, "all-small-caps") ||
		kw(st, P_FONT_VARIANT_CAPS, "all-petite-caps") ? 2 : 0;
}

/*
 * Small caps, synthesized as browsers do without the font's smcp
 * feature: s with its lowercase letters (all letters for all-small-
 * caps) in capitals, in a copy of the same length (ASCII and the
 * two-byte letters other_case() knows), and in small[] 1 at the bytes
 * drawn smaller.
 */
static char *caps_copy(const char *s, size_t len, int all, char *small)
{
	char *u = xmalloc(len + 1);
	size_t i;

	memcpy(u, s, len);
	memset(small, 0, len);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c >= 'a' && c <= 'z') {
			u[i] = (char)(c - 32);
			small[i] = 1;
		} else if (c >= 'A' && c <= 'Z') {
			small[i] = (char)all;
		} else if ((c & 0xe0) == 0xc0 && i + 1 < len) {
			unsigned w = (unsigned)(c & 0x1f) << 6 |
				((unsigned char)s[i + 1] & 0x3f);
			unsigned v = other_case(w, 1);

			if (v != w) {
				u[i] = (char)(0xc0 | v >> 6);
				u[i + 1] = (char)(0x80 | (v & 0x3f));
			}
			small[i] = small[i + 1] = (char)(v != w ||
				(all && other_case(w, 0) != w));
			i++;
		}
	}
	return u;
}

/* Firefox's size of a synthesized small cap */
#define SMALL_CAP 0.8f

/*
 * Shape s: split it into runs of the face each character needs (and,
 * for small caps, of one size), shape them, then add letter-spacing
 * and word-spacing. Returns the glyph count; *out is malloc'ed.
 */
int text_shape(const struct style *st, const char *s, size_t len,
	struct glyph **out)
{
	struct glyph *g = NULL;
	int n = 0, cap = 0, k, sc = small_caps(st);
	float pen = 0, ls, ws, shift = 0, em = font_size(st);
	size_t i = 0;
	const char *orig = s;
	char *u = NULL, *small = NULL;

	if (sc && len) {
		small = xmalloc(len);
		s = u = caps_copy(s, len, sc == 2, small);
	}
	if (fonts_stub()) {
		while (i < len) {
			struct glyph *gl = add_glyph(&g, n, &cap);
			size_t start = i;
			unsigned long cp = utf8_cp(s, len, &i);

			gl->face = -1;
			gl->id = (unsigned)cp;
			gl->cluster = (unsigned)start;
			gl->x = pen;
			gl->y = 0;
			gl->em = small && small[start] ? em * SMALL_CAP : em;
			gl->adv = gl->em * 0.5f;
			pen += gl->adv;
			n++;
		}
	} else {
		while (i < len) {
			size_t start = i, j;
			unsigned long cp = utf8_cp(s, len, &i);
			int fi = font_pick(st, cp);
			int sm = small && small[start];

			if (fi < 0 || !face_open(&faces[fi]))
				continue;
			/* extend the run while the same face fits */
			for (j = i; j < len; ) {
				size_t nx = j;
				unsigned long c2 = utf8_cp(s, len, &nx);

				if ((c2 != ' ' && font_pick(st, c2) != fi) ||
				    (small && small[j] != sm))
					break;
				j = nx;
			}
			n = shape_run(fi, sm ? em * SMALL_CAP : em, s, len,
				start, j, &g, n, &cap, &pen);
			i = j;
		}
	}
	free(u);
	free(small);
	s = orig;
	spacing(st, &ls, &ws);
	if (ls || ws)
		for (k = 0; k < n; k++) {
			unsigned c = g[k].cluster;

			g[k].x += shift;
			if (k + 1 == n || g[k + 1].cluster != c) {
				shift += ls;
				if (s[c] == ' ')
					shift += ws;
			}
		}
	*out = g;
	return n;
}

/* Width cache: the same words come back all the time. Two ways: a
 * word met again takes the first, the one it pushes out the second. */
#define NWIDTHS 8192

static struct wentry {
	const struct style *st;
	unsigned hash;
	unsigned char len;
	char s[31];
	float w;
} widths[NWIDTHS];

void width_cache_clear(void)
{
	memset(widths, 0, sizeof(widths));
}

float text_width(const struct style *st, const char *s, size_t len)
{
	struct glyph *g;
	struct wentry *e = NULL;
	unsigned h = 2166136261u;
	float w = 0, ls, ws, em;
	int n, k;
	size_t i;

	if (fonts_stub() && !small_caps(st)) {
		em = font_size(st);
		spacing(st, &ls, &ws);
		for (i = 0; i < len; i++) {
			unsigned char c = (unsigned char)s[i];

			if ((c & 0xc0) == 0x80)
				continue;
			w += em * 0.5f + ls;
			if (c == ' ')
				w += ws;
		}
		return w;
	}
	if (len < sizeof(e->s)) {
		for (i = 0; i < len; i++)
			h = (h ^ (unsigned char)s[i]) * 16777619u;
		h ^= (unsigned)(size_t)st;
		e = &widths[h % (NWIDTHS / 2) * 2];
		for (k = 0; k < 2; k++)
			if (e[k].st == st && e[k].hash == h && e[k].len == len &&
			    !memcmp(e[k].s, s, len))
				return e[k].w;
	}
	n = text_shape(st, s, len, &g);
	for (k = 0; k < n; k++)
		w += g[k].adv;
	free(g);
	spacing(st, &ls, &ws);
	if (ls || ws)
		for (i = 0; i < len; i++) {
			unsigned char c = (unsigned char)s[i];

			if ((c & 0xc0) == 0x80)
				continue;
			w += ls;
			if (c == ' ')
				w += ws;
		}
	if (e) {
		e[1] = e[0];
		e->st = st;
		e->hash = h;
		e->len = (unsigned char)len;
		memcpy(e->s, s, len);
		e->w = w;
	}
	return w;
}
