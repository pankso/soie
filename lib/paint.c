/*
 * Painting into an RGBA buffer: a software rasterizer for backgrounds,
 * borders (with radii), text, decorations and images, in the CSS 2
 * painting order (Appendix E, simplified).
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include FT_SYNTHESIS_H
#include "paint.h"

soie_node *paint_doc;

static void clip_to(struct canvas *c, float x0, float y0, float x1,
	float y1)
{
	int a = (int)floorf(x0 - c->ox), b = (int)floorf(y0 - c->oy);
	int d = (int)ceilf(x1 - c->ox), e = (int)ceilf(y1 - c->oy);

	if (a > c->cx0)
		c->cx0 = a;
	if (b > c->cy0)
		c->cy0 = b;
	if (d < c->cx1)
		c->cx1 = d;
	if (e < c->cy1)
		c->cy1 = e;
}

/* Source-over of an 8-bit color with coverage cov (0..255). */
static void blend(struct canvas *c, int x, int y, const unsigned char *rgba,
	int cov)
{
	unsigned char *p;
	int a, r = c->bgr ? 2 : 0;

	if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1)
		return;
	a = rgba[3] * cov / 255;
	a = (int)((float)a * c->alpha + 0.5f);
	if (c->mask)
		a = a * mask_at(c, x, y) / 255;
	if (a <= 0)
		return;
	p = c->px + (size_t)y * (size_t)c->stride + (size_t)x * 4;
	if (a >= 255) {
		p[0] = rgba[r];
		p[1] = rgba[1];
		p[2] = rgba[2 - r];
		p[3] = 255;
		return;
	}
	p[0] = (unsigned char)((rgba[r] * a + p[0] * (255 - a)) / 255);
	p[1] = (unsigned char)((rgba[1] * a + p[1] * (255 - a)) / 255);
	p[2] = (unsigned char)((rgba[2 - r] * a + p[2] * (255 - a)) / 255);
	p[3] = (unsigned char)(a + p[3] * (255 - a) / 255);
}

/* Rectangle with anti-aliased edges, page coordinates. */
void fill_rect(struct canvas *c, float x, float y, float w, float h,
	const unsigned char *rgba)
{
	float x0 = x - c->ox, y0 = y - c->oy, x1 = x0 + w, y1 = y0 + h;
	int ix0, iy0, ix1, iy1, i, j;

	if (w <= 0 || h <= 0 || !rgba[3])
		return;
	ix0 = (int)floorf(x0);
	iy0 = (int)floorf(y0);
	ix1 = (int)ceilf(x1);
	iy1 = (int)ceilf(y1);
	if (ix0 < c->cx0)
		ix0 = c->cx0;
	if (iy0 < c->cy0)
		iy0 = c->cy0;
	if (ix1 > c->cx1)
		ix1 = c->cx1;
	if (iy1 > c->cy1)
		iy1 = c->cy1;
	for (j = iy0; j < iy1; j++) {
		float cy = fminf(y1, (float)j + 1) - fmaxf(y0, (float)j);

		for (i = ix0; i < ix1; i++) {
			float cx = fminf(x1, (float)i + 1) - fmaxf(x0, (float)i);
			int cov = (int)(cx * cy * 255 + 0.5f);

			if (cov > 0)
				blend(c, i, j, rgba, cov > 255 ? 255 : cov);
		}
	}
}

/* Coverage of a pixel center by a rounded rectangle (4x4 samples). */
static int rrect_cov(float px, float py, float x0, float y0, float x1,
	float y1, const float *r)
{
	int n = 0, sx, sy;

	/* a pixel inside away from the corners is covered, one outside is
	 * not: the samples would all say so */
	if (px + 1 <= x0 || px >= x1 || py + 1 <= y0 || py >= y1)
		return 0;
	if (px >= x0 && px + 1 <= x1 && py >= y0 && py + 1 <= y1 &&
	    (px >= x0 + r[0] || py >= y0 + r[0]) &&
	    (px + 1 <= x1 - r[1] || py >= y0 + r[1]) &&
	    (px + 1 <= x1 - r[2] || py + 1 <= y1 - r[2]) &&
	    (px >= x0 + r[3] || py + 1 <= y1 - r[3]))
		return 255;
	for (sy = 0; sy < 4; sy++)
		for (sx = 0; sx < 4; sx++) {
			float x = px + ((float)sx + 0.5f) / 4;
			float y = py + ((float)sy + 0.5f) / 4;
			float cx, cy, rr;

			if (x < x0 || x > x1 || y < y0 || y > y1)
				continue;
			/* corners: top-left, top-right, bottom-right, -left */
			if (x < x0 + r[0] && y < y0 + r[0])
				cx = x0 + r[0], cy = y0 + r[0], rr = r[0];
			else if (x > x1 - r[1] && y < y0 + r[1])
				cx = x1 - r[1], cy = y0 + r[1], rr = r[1];
			else if (x > x1 - r[2] && y > y1 - r[2])
				cx = x1 - r[2], cy = y1 - r[2], rr = r[2];
			else if (x < x0 + r[3] && y > y1 - r[3])
				cx = x0 + r[3], cy = y1 - r[3], rr = r[3];
			else {
				n++;
				continue;
			}
			if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr)
				n++;
		}
	return n * 255 / 16;
}

/* Ring between an outer and an inner rounded rectangle (inner may be
 * empty): backgrounds use no inner, borders use both. */
static void fill_rrect(struct canvas *c, float x, float y, float w, float h,
	const float *r, float ix, float iy, float iw, float ih,
	const float *ir, const unsigned char *rgba)
{
	int i, j, i0, j0, i1, j1;

	if (w <= 0 || h <= 0 || !rgba[3])
		return;
	i0 = (int)floorf(x - c->ox);
	j0 = (int)floorf(y - c->oy);
	i1 = (int)ceilf(x + w - c->ox);
	j1 = (int)ceilf(y + h - c->oy);
	if (i0 < c->cx0)
		i0 = c->cx0;
	if (j0 < c->cy0)
		j0 = c->cy0;
	if (i1 > c->cx1)
		i1 = c->cx1;
	if (j1 > c->cy1)
		j1 = c->cy1;
	for (j = j0; j < j1; j++)
		for (i = i0; i < i1; i++) {
			float px = (float)i + c->ox, py = (float)j + c->oy;
			int cov = rrect_cov(px, py, x, y, x + w, y + h, r);

			if (cov && iw > 0 && ih > 0)
				cov -= rrect_cov(px, py, ix, iy, ix + iw,
					iy + ih, ir) * cov / 255;
			if (cov > 0)
				blend(c, i, j, rgba, cov);
		}
}

static void style_rgba(const struct style *st, int prop, unsigned char *out)
{
	const struct color *col = st->v[prop].u.color;

	if (!col) {
		memset(out, 0, 4);
		return;
	}
	color_to_rgba(col, out);
}

/* Glyphs */

struct gcache {
	int face, size, id, flags;
	int left, top, w, h, pitch;
	unsigned char *bits;
};

#define NGLYPHS 2048
static struct gcache glyphs[NGLYPHS];

void glyph_cache_clear(void)
{
	int i;

	for (i = 0; i < NGLYPHS; i++) {
		free(glyphs[i].bits);
		glyphs[i].bits = NULL;
		glyphs[i].face = -2;
	}
}

static const struct gcache *glyph(int fi, float size, unsigned id,
	int bold, int italic)
{
	int sz = (int)(size * 64 + 0.5f), flags = bold | italic << 1;
	unsigned h = ((unsigned)fi * 31u + (unsigned)sz) * 2654435761u ^ id *
		40503u ^ (unsigned)flags;
	struct gcache *g = &glyphs[h % NGLYPHS];
	struct face *x = &faces[fi];
	FT_GlyphSlot s;
	int row;

	if (g->bits && g->face == fi && g->size == sz && g->id == (int)id &&
	    g->flags == flags)
		return g;
	if (!face_open(x) || FT_Set_Char_Size(x->ft, 0, sz, 72, 72) ||
	    FT_Load_Glyph(x->ft, id, FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT))
		return NULL;
	s = x->ft->glyph;
	if (bold)
		FT_GlyphSlot_Embolden(s);
	if (italic)
		FT_GlyphSlot_Oblique(s);
	if (FT_Render_Glyph(s, FT_RENDER_MODE_NORMAL) ||
	    s->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
		return NULL;
	free(g->bits);
	g->face = fi;
	g->size = sz;
	g->id = (int)id;
	g->flags = flags;
	g->left = s->bitmap_left;
	g->top = s->bitmap_top;
	g->w = (int)s->bitmap.width;
	g->h = (int)s->bitmap.rows;
	g->pitch = g->w;
	g->bits = xmalloc((size_t)(g->w * g->h) + 1);
	for (row = 0; row < g->h; row++)
		memcpy(g->bits + row * g->w, s->bitmap.buffer +
			row * s->bitmap.pitch, (size_t)g->w);
	return g;
}

static void draw_text(struct canvas *c, const struct style *st,
	const char *s, size_t len, float x, float base, float jspace)
{
	struct glyph *gs;
	unsigned char rgba[4];
	float em = st->v[P_FONT_SIZE].u.len.px, extra = 0;
	int n, k, want = (int)st->v[P_FONT_WEIGHT].u.num;
	int italic = !kw(st, P_FONT_STYLE, "normal");

	style_rgba(st, P_COLOR, rgba);
	if (!rgba[3] || kw(st, P_VISIBILITY, "hidden"))
		return;
	n = text_shape(st, s, len, &gs);
	for (k = 0; k < n; k++) {
		struct glyph *g = &gs[k];
		float gx = x + g->x + extra;

		if (jspace && s[g->cluster] == ' ' &&
		    (k + 1 == n || gs[k + 1].cluster != g->cluster))
			extra += jspace;
		if (g->face < 0) {
			/* stub font: a box per visible character */
			if (g->id != ' ')
				fill_rect(c, gx + em * 0.1f, base - em * 0.6f,
					em * 0.3f, em * 0.6f, rgba);
			continue;
		} else {
			struct face *f = &faces[g->face];
			const struct gcache *b = glyph(g->face, em, g->id,
				want >= 600 && f->weight < 600,
				italic && !f->italic);
			int i, j, x0, y0;

			if (!b)
				continue;
			x0 = (int)floorf(gx - c->ox + 0.5f) + b->left;
			y0 = (int)floorf(base + g->y - c->oy + 0.5f) - b->top;
			for (j = 0; j < b->h; j++)
				for (i = 0; i < b->w; i++) {
					int cov = b->bits[j * b->pitch + i];

					if (cov)
						blend(c, x0 + i, y0 + j, rgba, cov);
				}
		}
	}
	free(gs);
}

void paint_text(struct canvas *c, const struct style *st, const char *s,
	size_t len, float x, float base)
{
	draw_text(c, st, s, len, x, base, 0);
}

/* Boxes */

static void radii(const struct box *b, float w, float h, float *r)
{
	static const int p[4] = { P_BORDER_TOP_LEFT_RADIUS,
		P_BORDER_TOP_RIGHT_RADIUS, P_BORDER_BOTTOM_RIGHT_RADIUS,
		P_BORDER_BOTTOM_LEFT_RADIUS };
	float m = (w < h ? w : h) / 2;
	int i;

	for (i = 0; i < 4; i++) {
		r[i] = len_val(&b->style->v[p[i]], w);
		if (r[i] > m)
			r[i] = m;
		if (r[i] < 0)
			r[i] = 0;
	}
}

static void shade(const unsigned char *in, int dark, unsigned char *out)
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = (unsigned char)(dark ? in[i] * 2 / 3 : in[i] +
			(255 - in[i]) / 3);
	out[3] = in[3];
}

static void border_side(struct canvas *c, const struct box *b, int side,
	float bx, float by, float bw, float bh)
{
	static const int sty[4] = { P_BORDER_TOP_STYLE, P_BORDER_RIGHT_STYLE,
		P_BORDER_BOTTOM_STYLE, P_BORDER_LEFT_STYLE };
	static const int col[4] = { P_BORDER_TOP_COLOR, P_BORDER_RIGHT_COLOR,
		P_BORDER_BOTTOM_COLOR, P_BORDER_LEFT_COLOR };
	const struct style *st = b->style;
	unsigned char rgba[4];
	float w = b->b[side], x, y, sw, sh, len, dash, gap, pos;
	int horiz = side == 0 || side == 2;

	if (w <= 0)
		return;
	style_rgba(st, col[side], rgba);
	if (kw(st, sty[side], "inset") || kw(st, sty[side], "groove"))
		shade(rgba, side == 0 || side == 3, rgba);
	else if (kw(st, sty[side], "outset") || kw(st, sty[side], "ridge"))
		shade(rgba, side == 1 || side == 2, rgba);
	x = side == 1 ? bx + bw - w : bx;
	y = side == 2 ? by + bh - w : by;
	sw = horiz ? bw : w;
	sh = horiz ? w : bh;
	if (kw(st, sty[side], "double") && w >= 3) {
		float t = w / 3;

		if (horiz) {
			fill_rect(c, x, y, sw, t, rgba);
			fill_rect(c, x, y + sh - t, sw, t, rgba);
		} else {
			fill_rect(c, x, y, t, sh, rgba);
			fill_rect(c, x + sw - t, y, t, sh, rgba);
		}
		return;
	}
	if (!kw(st, sty[side], "dashed") && !kw(st, sty[side], "dotted")) {
		/* corners: share them along the diagonal, roughly */
		fill_rect(c, x, y, sw, sh, rgba);
		return;
	}
	dash = kw(st, sty[side], "dotted") ? w : w * 3;
	gap = dash;
	len = horiz ? sw : sh;
	for (pos = 0; pos < len; pos += dash + gap) {
		float d = pos + dash > len ? len - pos : dash;

		if (horiz)
			fill_rect(c, x + pos, y, d, sh, rgba);
		else
			fill_rect(c, x, y + pos, sw, d, rgba);
	}
}

static void paint_background(struct canvas *c, const struct box *b,
	float x, float y, float w, float h)
{
	unsigned char rgba[4];
	float r[4];

	style_rgba(b->style, P_BACKGROUND_COLOR, rgba);
	radii(b, w, h, r);
	if (rgba[3]) {
		if (r[0] || r[1] || r[2] || r[3])
			fill_rrect(c, x, y, w, h, r, 0, 0, 0, 0, r, rgba);
		else
			fill_rect(c, x, y, w, h, rgba);
	}
	paint_bg_image(c, b, x, y, w, h);
}

static void paint_borders(struct canvas *c, const struct box *b, float x,
	float y, float w, float h)
{
	float r[4], ir[4];
	int i, same = 1;
	unsigned char rgba[4], first[4];

	radii(b, w, h, r);
	if (r[0] || r[1] || r[2] || r[3]) {
		/* rounded: one color ring (the top one), solid */
		for (i = 0; i < 4; i++) {
			float bw = b->b[i] > b->b[(i + 3) % 4] ? b->b[i] :
				b->b[(i + 3) % 4];

			ir[i] = r[i] - bw > 0 ? r[i] - bw : 0;
		}
		style_rgba(b->style, P_BORDER_TOP_COLOR, rgba);
		if (b->b[0] || b->b[1] || b->b[2] || b->b[3])
			fill_rrect(c, x, y, w, h, r, x + b->b[3], y + b->b[0],
				w - b->b[1] - b->b[3], h - b->b[0] - b->b[2],
				ir, rgba);
		return;
	}
	style_rgba(b->style, P_BORDER_TOP_COLOR, first);
	(void)same;
	for (i = 0; i < 4; i++)
		border_side(c, b, i, x, y, w, h);
}

static void paint_outline(struct canvas *c, const struct box *b, float x,
	float y, float w, float h)
{
	const struct style *st = b->style;
	float ow = st->v[P_OUTLINE_WIDTH].u.len.px;
	float off = st->v[P_OUTLINE_OFFSET].u.len.px;
	unsigned char rgba[4];

	if (ow <= 0 || kw(st, P_OUTLINE_STYLE, "none"))
		return;
	style_rgba(st, P_OUTLINE_COLOR, rgba);
	x -= off + ow;
	y -= off + ow;
	w += 2 * (off + ow);
	h += 2 * (off + ow);
	fill_rect(c, x, y, w, ow, rgba);
	fill_rect(c, x, y + h - ow, w, ow, rgba);
	fill_rect(c, x, y + ow, ow, h - 2 * ow, rgba);
	fill_rect(c, x + w - ow, y + ow, ow, h - 2 * ow, rgba);
}

static void decorate(struct canvas *c, const struct style *st, float x,
	float w, float base, float top)
{
	int lines = (int)st->v[P_TEXT_DECORATION_LINE].u.num;
	float em = st->v[P_FONT_SIZE].u.len.px, t = em / 14;
	unsigned char rgba[4];

	if (!lines)
		return;
	if (t < 1)
		t = 1;
	style_rgba(st, P_TEXT_DECORATION_COLOR, rgba);
	if (lines & 1)
		fill_rect(c, x, base + em * 0.1f, w, t, rgba);
	if (lines & 2)
		fill_rect(c, x, top, w, t, rgba);
	if (lines & 4)
		fill_rect(c, x, base - em * 0.3f, w, t, rgba);
}

/* Decorations come from the decorating box: the nearest ancestor with
 * text-decoration-line, for text inside it (CSS Text Decoration 3). */
static const struct style *decorating(const struct box *t,
	const struct box *container)
{
	const struct box *b;

	for (b = t->parent; b; b = b->parent) {
		if ((int)b->style->v[P_TEXT_DECORATION_LINE].u.num)
			return b->style;
		if (b == container)
			break;
	}
	return NULL;
}

static void paint_box(struct canvas *c, struct box *b, int stacking);

static void paint_lines(struct canvas *c, struct box *b)
{
	int i, j;

	for (i = 0; i < b->nlines; i++) {
		struct line *l = &b->lines[i];

		/* inline boxes first, then the text over them */
		for (j = l->first; j < l->first + l->n; j++) {
			struct frag *f = &b->frags[j];

			if (f->kind == F_INLINE) {
				struct box *ib = f->box;

				paint_background(c, ib, f->x, f->y, f->w, f->h);
				paint_borders(c, ib, f->x, f->y, f->w, f->h);
			}
		}
		for (j = l->first; j < l->first + l->n; j++) {
			struct frag *f = &b->frags[j];

			if (f->kind == F_TEXT) {
				const struct style *ds = decorating(f->box, b);
				float jw = l->jspace;
				float em = f->box->style->v[P_FONT_SIZE].u.len.px;

				/* text out of the clip, its glyphs reaching
				 * 2em up and 1em down, is not even shaped:
				 * a band of a long page paints fast */
				if (f->baseline + em < c->oy + (float)c->cy0 ||
				    f->baseline - 2 * em > c->oy + (float)c->cy1)
					continue;

				draw_text(c, f->box->style, f->box->text +
					f->start, f->len, f->x, f->baseline, jw);
				if (ds)
					decorate(c, ds, f->x, f->w, f->baseline,
						f->y);
			} else if (f->kind == F_ATOMIC) {
				paint_box(c, f->box, 1);
			}
		}
	}
}

/* Positioned boxes, painted after the flow, by z-index. */
struct zent {
	struct box *box;
	int z;
	int order;
};

static struct zent *zlist;
static int nz, capz;
static const struct box *bg_done;	/* its background is painted */

static int positioned(const struct box *b)
{
	return b->style && !kw(b->style, P_POSITION, "static");
}

static void zadd(struct box *b)
{
	if (nz == capz) {
		capz = capz ? capz * 2 : 32;
		zlist = xrealloc(zlist, (size_t)capz * sizeof(*zlist));
	}
	zlist[nz].box = b;
	zlist[nz].z = b->style->v[P_Z_INDEX].kw ? 0 :
		(int)b->style->v[P_Z_INDEX].u.num;
	zlist[nz].order = nz;
	nz++;
}

/* A positioned box with a z-index, fixed, or translucent, is a
 * stacking context: its positioned descendants are ordered within it,
 * then it takes its place among its own siblings. */
static int masked(const struct box *b)
{
	const char *m = b->style ? b->style->v[P_MASK_IMAGE].u.str : NULL;

	return m && strcmp(m, "none");
}

static int stacking_context(const struct box *b)
{
	return positioned(b) && (!b->style->v[P_Z_INDEX].kw ||
		kw(b->style, P_POSITION, "fixed") ||
		b->style->v[P_OPACITY].u.num < 1 || masked(b));
}

/*
 * mask-image: b's mask layers painted into a buffer of its border box
 * (clipped), whose alpha then weighs every pixel painted (blend());
 * inside another mask, both. *save keeps the mask before, for
 * mask_pop(). 0 when b has no mask.
 */
static int mask_push(struct canvas *c, const struct box *b,
	struct canvas *save)
{
	struct canvas t;
	float x, y, w, h;
	int x0, y0, x1, y1, i, j;
	unsigned char *m;

	if (!masked(b))
		return 0;
	*save = *c;
	x = b->x - b->p[3] - b->b[3];
	y = b->y - b->p[0] - b->b[0];
	w = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
	h = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
	x0 = (int)floorf(x - c->ox);
	y0 = (int)floorf(y - c->oy);
	x1 = (int)ceilf(x + w - c->ox);
	y1 = (int)ceilf(y + h - c->oy);
	x0 = x0 < c->cx0 ? c->cx0 : x0;
	y0 = y0 < c->cy0 ? c->cy0 : y0;
	x1 = x1 > c->cx1 ? c->cx1 : x1;
	y1 = y1 > c->cy1 ? c->cy1 : y1;
	if (x1 < x0)
		x1 = x0;
	if (y1 < y0)
		y1 = y0;
	memset(&t, 0, sizeof(t));
	t.w = x1 - x0;
	t.h = y1 - y0;
	t.stride = t.w * 4;
	t.px = calloc((size_t)t.w * (size_t)t.h + 1, 4);
	m = malloc((size_t)t.w * (size_t)t.h + 1);
	if (!t.px || !m)
		abort();
	t.ox = c->ox + (float)x0;
	t.oy = c->oy + (float)y0;
	t.cx1 = t.w;
	t.cy1 = t.h;
	t.alpha = 1;
	paint_layers_of(&t, b, P_MASK_IMAGE, x, y, w, h);
	for (j = 0; j < t.h; j++)
		for (i = 0; i < t.w; i++)
			m[(size_t)j * (size_t)t.w + (size_t)i] = (unsigned char)
				(t.px[(size_t)j * (size_t)t.stride +
				(size_t)i * 4 + 3] * mask_at(c, x0 + i,
				y0 + j) / 255);
	free(t.px);
	c->mask = m;
	c->mx0 = x0;
	c->my0 = y0;
	c->mx1 = x1;
	c->my1 = y1;
	return 1;
}

static void mask_pop(struct canvas *c, const struct canvas *save)
{
	free(c->mask);
	c->mask = save->mask;
	c->mx0 = save->mx0;
	c->my0 = save->my0;
	c->mx1 = save->mx1;
	c->my1 = save->my1;
}

static int zcmp(const void *a, const void *b)
{
	const struct zent *x = a, *y = b;

	if (x->z != y->z)
		return x->z < y->z ? -1 : 1;
	return x->order - y->order;
}

/* Floats and positioned boxes met in the text, also inside inline
 * boxes (a <span> around a dialog) */
static void paint_out_of_flow(struct canvas *c, struct box *b)
{
	struct box *k;

	for (k = b->first; k; k = k->next)
		if (k->flags & (BF_FLOAT | BF_ABS))
			paint_box(c, k, 0);
		else if (k->type == B_INLINE)
			paint_out_of_flow(c, k);
}

/*
 * Paint b and its descendants. Positioned descendants are deferred to
 * the z-list unless stacking (painting b itself as a unit).
 */
static void paint_box(struct canvas *c, struct box *b, int stacking)
{
	struct box *k;
	float x, y, w, h, save_alpha = c->alpha;
	int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
	struct canvas msave;
	int mk;

	if (!stacking && positioned(b) && b->parent) {
		zadd(b);
		return;
	}
	if (b->type == B_TEXT || b->type == B_BREAK)
		return;
	mk = mask_push(c, b, &msave);
	if (b->style && kw(b->style, P_VISIBILITY, "hidden") &&
	    b->type != B_INLINE) {
		/* descendants may be visible again: walk on */
	}
	c->alpha *= b->style ? (float)b->style->v[P_OPACITY].u.num : 1;
	x = b->x - b->p[3] - b->b[3];
	y = b->y - b->p[0] - b->b[0];
	w = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
	h = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
	if (bg_done == b) {
		bg_done = NULL;		/* painted under its negative layers */
	} else if (b->type != B_INLINE && b->parent &&
	    !kw(b->style, P_VISIBILITY, "hidden")) {
		paint_background(c, b, x, y, w, h);
		paint_borders(c, b, x, y, w, h);
	}
	if (b->type == B_REPLACED) {
		paint_replaced(c, b);
		paint_outline(c, b, x, y, w, h);
		c->alpha = save_alpha;
		if (mk)
			mask_pop(c, &msave);
		return;
	}
	if (b->style && b->parent && (!kw(b->style, P_OVERFLOW_X, "visible") ||
	    !kw(b->style, P_OVERFLOW_Y, "visible")))
		clip_to(c, b->x - b->p[3], b->y - b->p[0],
			b->x + b->w + b->p[1], b->y + b->h + b->p[2]);
	if (b->flags & BF_INLINE_KIDS) {
		paint_lines(c, b);
		paint_out_of_flow(c, b);
	} else {
		/* blocks, then floats over them (Appendix E, steps 4-5) */
		for (k = b->first; k; k = k->next)
			if (!(k->flags & BF_FLOAT))
				paint_box(c, k, 0);
		for (k = b->first; k; k = k->next)
			if (k->flags & BF_FLOAT)
				paint_box(c, k, 0);
	}
	c->cx0 = cx0;
	c->cy0 = cy0;
	c->cx1 = cx1;
	c->cy1 = cy1;
	paint_outline(c, b, x, y, w, h);
	c->alpha = save_alpha;
	if (mk)
		mask_pop(c, &msave);
}

static void paint_layer(struct canvas *c, struct box *b);

static int zindex(const struct box *b)
{
	return b->style->v[P_Z_INDEX].kw ? 0 :
		(int)b->style->v[P_Z_INDEX].u.num;
}

/* The negative z-index layers of the context b is the root of, not
 * those in the contexts they make */
static void negatives(struct box *b, struct zent **v, int *n, int *cap)
{
	struct box *k;

	for (k = b->first; k; k = k->next) {
		if (positioned(k)) {
			if (zindex(k) < 0) {
				if (*n == *cap) {
					*cap = *cap ? *cap * 2 : 8;
					*v = xrealloc(*v, (size_t)*cap * sizeof(**v));
				}
				(*v)[*n].box = k;
				(*v)[*n].z = zindex(k);
				(*v)[*n].order = *n;
				(*n)++;
			}
			if (stacking_context(k))
				continue;
		}
		negatives(k, v, n, cap);
	}
}

/* The deferred layers of the current context, by z-index then tree
 * order; the negative ones were painted under the flow */
static void paint_layers(struct canvas *c)
{
	int i;

	for (i = 0; i < nz; i++) {
		qsort(zlist + i, (size_t)(nz - i), sizeof(*zlist), zcmp);
		if (zlist[i].z >= 0)
			paint_layer(c, zlist[i].box);
	}
}

/*
 * A stacking context, CSS 2 Appendix E: the background and borders of
 * its root, its negative z-index layers, its flow, its other layers.
 */
static void paint_context(struct canvas *c, struct box *b)
{
	struct zent *outer = zlist, *neg = NULL;
	int onz = nz, ocap = capz, nneg = 0, capneg = 0, i;
	float alpha = c->alpha;
	float op = b->style ? (float)b->style->v[P_OPACITY].u.num : 1;

	zlist = NULL;
	nz = capz = 0;
	negatives(b, &neg, &nneg, &capneg);
	if (nneg) {
		c->alpha *= op;
		if (b->type != B_INLINE && b->parent &&
		    !kw(b->style, P_VISIBILITY, "hidden")) {
			float x = b->x - b->p[3] - b->b[3];
			float y = b->y - b->p[0] - b->b[0];
			float w = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
			float h = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];

			paint_background(c, b, x, y, w, h);
			paint_borders(c, b, x, y, w, h);
			bg_done = b;
		}
		qsort(neg, (size_t)nneg, sizeof(*neg), zcmp);
		for (i = 0; i < nneg; i++)
			paint_layer(c, neg[i].box);
		c->alpha = alpha;
		free(neg);
	}
	paint_box(c, b, 1);
	c->alpha *= op;
	{
		/* its positioned descendants under its mask too */
		struct canvas msave;
		int mk = mask_push(c, b, &msave);

		paint_layers(c);
		if (mk)
			mask_pop(c, &msave);
	}
	c->alpha = alpha;
	free(zlist);
	zlist = outer;
	nz = onz;
	capz = ocap;
}

/* A layer: a stacking context paints its own layers, under its
 * opacity; another one leaves them to the context it is in */
static void paint_layer(struct canvas *c, struct box *b)
{
	if (!stacking_context(b))
		paint_box(c, b, 1);
	else
		paint_context(c, b);
}

/* The canvas background: the root's, or the body's (CSS 2 14.2). */
static void canvas_background(struct canvas *c, struct box *root)
{
	unsigned char rgba[4] = { 255, 255, 255, 255 }, bg[4];
	const struct box *src = root;
	int i, j;

	style_rgba(root->style, P_BACKGROUND_COLOR, bg);
	if (!bg[3] && root->first && root->first->node &&
	    is_html(root->first->node, TAG_BODY)) {
		src = root->first;
		style_rgba(src->style, P_BACKGROUND_COLOR, bg);
	}
	for (j = 0; j < c->h; j++)
		for (i = 0; i < c->w; i++)
			memcpy(c->px + (size_t)j * (size_t)c->stride +
				(size_t)i * 4, rgba, 4);
	if (bg[3])
		fill_rect(c, c->ox, c->oy, (float)c->w, (float)c->h, bg);
	(void)src;
}

static int paint(soie_node *doc, unsigned char *rgba, int width, int height,
	int stride, int sx, int sy, int bgr)
{
	struct canvas c;
	struct box *root;

	if (doc->type != SOIE_DOCUMENT || !doc->u.doc.layout)
		return -1;
	root = doc->u.doc.layout->root;
	memset(&c, 0, sizeof(c));
	c.px = rgba;
	c.w = width;
	c.h = height;
	c.stride = stride;
	c.ox = (float)sx;
	c.oy = (float)sy;
	c.cx1 = width;
	c.cy1 = height;
	c.alpha = 1;
	c.bgr = bgr;
	font_doc = doc;
	paint_doc = doc;
	if (!root) {
		memset(rgba, 255, (size_t)stride * (size_t)height);
		return 0;
	}
	canvas_background(&c, root);
	nz = 0;
	paint_context(&c, root);
	free(zlist);
	zlist = NULL;
	nz = capz = 0;
	return 0;
}

int soie_paint(soie_node *doc, unsigned char *rgba, int width, int height,
	int stride, int sx, int sy)
{
	return paint(doc, rgba, width, height, stride, sx, sy, 0);
}

int soie_paint_bgra(soie_node *doc, unsigned char *bgra, int width,
	int height, int stride, int sx, int sy)
{
	return paint(doc, bgra, width, height, stride, sx, sy, 1);
}
