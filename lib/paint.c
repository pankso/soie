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
	int a = ifloor(x0 - c->ox), b = ifloor(y0 - c->oy);
	int d = iceil(x1 - c->ox), e = iceil(y1 - c->oy);

	if (x1 <= x0 || y1 <= y0) {
		c->cx1 = c->cx0;	/* empty, not a pixel wide */
		c->cy1 = c->cy0;
		return;
	}
	if (a > c->cx0)
		c->cx0 = a;
	if (b > c->cy0)
		c->cy0 = b;
	if (d < c->cx1)
		c->cx1 = d;
	if (e < c->cy1)
		c->cy1 = e;
}

/* A length of a raw value (clip, clip-path): a percentage of ref, em
 * of the box's font (em: its size, already zoomed), CSS px zoomed */
float css_len_px(const struct cv *v, float ref, float em)
{
	if (!v)
		return 0;
	if (v->type == CT_PERCENT)
		return (float)v->num * ref / 100;
	if (v->type == CT_DIMENSION && ascii_ieq(v->str, "em"))
		return (float)v->num * em;
	if (v->type == CT_DIMENSION && ascii_ieq(v->str, "rem"))
		return (float)v->num * 16 * doc_zoom();
	if (v->type == CT_DIMENSION || v->type == CT_NUMBER)
		return (float)v->num * doc_zoom();
	return 0;
}

/* clip: rect(top, right, bottom, left) of an absolute box, from the
 * corner of its border box, auto its edge (commas optional) */
static void clip_rect(struct canvas *c, const struct box *b)
{
	const char *s = b->style ? b->style->v[P_CLIP].u.str : NULL;
	float x, y, v[4], em;
	const struct cv *f, *a;
	struct cvlist l;
	struct cur k;
	int n = 0;

	if (!s || !strcmp(s, "auto") || (!kw(b->style, P_POSITION,
	    "absolute") && !kw(b->style, P_POSITION, "fixed")))
		return;
	css_component_values(s, strlen(s), &l);
	cur_init(&k, &l);
	if (cv_func(f = cur_next(&k), "rect")) {
		x = b->x - b->p[3] - b->b[3];
		y = b->y - b->p[0] - b->b[0];
		v[0] = v[3] = 0;
		v[1] = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
		v[2] = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
		em = b->style->v[P_FONT_SIZE].u.len.px;
		cur_init(&k, &f->kids);
		while ((a = cur_next(&k)) && n < 4)
			if (a->type != CT_COMMA) {
				if (!cv_ident(a, "auto"))
					v[n] = css_len_px(a, 0, em);
				n++;
			}
		if (n == 4)
			clip_to(c, x + v[3], y + v[0], x + v[1], y + v[2]);
	}
	cvlist_free(&l);
}

/* Does b's overflow clip its content (to its padding box) */
static int clips(const struct box *b)
{
	return b->style && b->parent && b->type != B_INLINE &&
		(!kw(b->style, P_OVERFLOW_X, "visible") ||
		!kw(b->style, P_OVERFLOW_Y, "visible"));
}

/* A form control, which paints its own box inside */
static int control(const struct box *b)
{
	const soie_node *n = b->node;

	return n && n->ns == SOIE_NS_HTML && (n->tag == TAG_INPUT ||
		n->tag == TAG_TEXTAREA || n->tag == TAG_SELECT ||
		n->tag == TAG_BUTTON);
}

/* A form control whose border is still the user agent's: 2 px inset
 * or outset all around, no radius, no image */
static int native_control(const struct box *b)
{
	static const int bs[4] = { P_BORDER_TOP_STYLE, P_BORDER_RIGHT_STYLE,
		P_BORDER_BOTTOM_STYLE, P_BORDER_LEFT_STYLE };
	static const int br[4] = { P_BORDER_TOP_LEFT_RADIUS,
		P_BORDER_TOP_RIGHT_RADIUS, P_BORDER_BOTTOM_RIGHT_RADIUS,
		P_BORDER_BOTTOM_LEFT_RADIUS };
	const struct style *st = b->style;
	const char *im;
	int i, inset;

	if (!control(b) || !st)
		return 0;
	inset = kw(st, P_BORDER_TOP_STYLE, "inset");
	if (!inset && !kw(st, P_BORDER_TOP_STYLE, "outset"))
		return 0;
	for (i = 0; i < 4; i++)
		if (!kw(st, bs[i], inset ? "inset" : "outset") ||
		    b->b[i] != b->b[0] || b->b[i] < 2 || b->b[i] > 3 ||
		    st->v[br[i]].u.len.px || st->v[br[i]].u.len.pct)
			return 0;
	im = st->v[P_BACKGROUND_IMAGE].u.str;
	return !im || !strcmp(im, "none");
}

/* What clips b's content: overflow (to its padding box) and clip */
static void content_clip(struct canvas *c, const struct box *b)
{
	if (!b->style || !b->parent)
		return;
	clip_rect(c, b);
	if (clips(b))
		clip_to(c, b->x - b->p[3], b->y - b->p[0],
			b->x + b->w + b->p[1], b->y + b->h + b->p[2]);
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

/* An opaque color, no group opacity, no mask: covered pixels just
 * take it */
static int solid(const struct canvas *c, const unsigned char *rgba)
{
	return rgba[3] == 255 && c->alpha == 1 && !c->mask;
}

/* Pixels i0 to i1 (excluded) of row j fully covered by a solid color:
 * what blend() would give them, one store each */
static void fill_span(struct canvas *c, int j, int i0, int i1,
	const unsigned char *rgba)
{
	unsigned char v[4], *p;
	int r = c->bgr ? 2 : 0;

	v[0] = rgba[r];
	v[1] = rgba[1];
	v[2] = rgba[2 - r];
	v[3] = 255;
	p = c->px + (size_t)j * (size_t)c->stride + (size_t)i0 * 4;
	for (; i0 < i1; i0++, p += 4)
		memcpy(p, v, 4);
}

/* Rectangle with anti-aliased edges, page coordinates. */
void fill_rect(struct canvas *c, float x, float y, float w, float h,
	const unsigned char *rgba)
{
	float x0 = x - c->ox, y0 = y - c->oy, x1 = x0 + w, y1 = y0 + h;
	int ix0, iy0, ix1, iy1, i, j, fx0, fx1, fy0, fy1;

	if (w <= 0 || h <= 0 || !rgba[3])
		return;
	ix0 = ifloor(x0);
	iy0 = ifloor(y0);
	ix1 = iceil(x1);
	iy1 = iceil(y1);
	if (ix0 < c->cx0)
		ix0 = c->cx0;
	if (iy0 < c->cy0)
		iy0 = c->cy0;
	if (ix1 > c->cx1)
		ix1 = c->cx1;
	if (iy1 > c->cy1)
		iy1 = c->cy1;
	/* the pixels inside the edges, filled by rows when solid */
	fx0 = iceil(x0) > ix0 ? iceil(x0) : ix0;
	fx1 = ifloor(x1) < ix1 ? ifloor(x1) : ix1;
	fy0 = iceil(y0);
	fy1 = solid(c, rgba) && fx0 < fx1 ? ifloor(y1) : fy0;
	for (j = iy0; j < iy1; j++) {
		float cy = fminf(y1, (float)j + 1) - fmaxf(y0, (float)j);

		for (i = ix0; i < ix1; i++) {
			float cx;
			int cov;

			if (i == fx0 && j >= fy0 && j < fy1) {
				fill_span(c, j, fx0, fx1, rgba);
				i = fx1 - 1;
				continue;
			}
			cx = fminf(x1, (float)i + 1) - fmaxf(x0, (float)i);
			cov = (int)(cx * cy * 255 + 0.5f);
			if (cov > 0)
				blend(c, i, j, rgba, cov > 255 ? 255 : cov);
		}
	}
}

/*
 * Rounded rectangles: radii r[0..3] across (top-left, top-right,
 * bottom-right, bottom-left), r[4..7] down, the corners elliptical;
 * a corner with either radius 0 is square.
 */

/* Is x, y out of the curve of the corner it is in (0 when in no
 * corner) */
static int corner_out(float x, float y, float x0, float y0, float x1,
	float y1, const float *r)
{
	float cx, cy, dx, dy;

	if (x < x0 + r[0] && y < y0 + r[4])
		cx = x0 + r[0], cy = y0 + r[4], dx = r[0], dy = r[4];
	else if (x > x1 - r[1] && y < y0 + r[5])
		cx = x1 - r[1], cy = y0 + r[5], dx = r[1], dy = r[5];
	else if (x > x1 - r[2] && y > y1 - r[6])
		cx = x1 - r[2], cy = y1 - r[6], dx = r[2], dy = r[6];
	else if (x < x0 + r[3] && y > y1 - r[7])
		cx = x0 + r[3], cy = y1 - r[7], dx = r[3], dy = r[7];
	else
		return 0;
	dx = (x - cx) / dx;
	dy = (y - cy) / dy;
	return dx * dx + dy * dy > 1;
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
	    (px >= x0 + r[0] || py >= y0 + r[4]) &&
	    (px + 1 <= x1 - r[1] || py >= y0 + r[5]) &&
	    (px + 1 <= x1 - r[2] || py + 1 <= y1 - r[6]) &&
	    (px >= x0 + r[3] || py + 1 <= y1 - r[7]))
		return 255;
	for (sy = 0; sy < 4; sy++)
		for (sx = 0; sx < 4; sx++) {
			float x = px + ((float)sx + 0.5f) / 4;
			float y = py + ((float)sy + 0.5f) / 4;

			if (x >= x0 && x <= x1 && y >= y0 && y <= y1 &&
			    !corner_out(x, y, x0, y0, x1, y1, r))
				n++;
		}
	return n * 255 / 16;
}

/* Radii r scaled down together so that no two meet past a side of a
 * w x h box (css-backgrounds-3 5.5), a corner square if either of its
 * radii is 0 */
static void fit_radii(float *r, float w, float h)
{
	float f = 1, s;
	int i;

	for (i = 0; i < 8; i++)
		if (!(r[i] > 0))
			r[i] = 0;
	if ((s = r[0] + r[1]) > w)
		f = fminf(f, w / s);
	if ((s = r[3] + r[2]) > w)
		f = fminf(f, w / s);
	if ((s = r[4] + r[7]) > h)
		f = fminf(f, h / s);
	if ((s = r[5] + r[6]) > h)
		f = fminf(f, h / s);
	for (i = 0; i < 8; i++)
		r[i] = f < 1 ? r[i] * f : r[i];
	for (i = 0; i < 4; i++)
		if (!r[i] || !r[i + 4])
			r[i] = r[i + 4] = 0;
}

/* The radii of a curve e[top, right, bottom, left] inside another of
 * radii r (padding box, content box, a double border's line) */
static void inset_radii(const float *r, const float *e, float *out)
{
	static const int ax[4] = { 3, 1, 1, 3 }, ay[4] = { 0, 0, 2, 2 };
	int i;

	for (i = 0; i < 4; i++) {
		out[i] = fmaxf(r[i] - e[ax[i]], 0);
		out[i + 4] = fmaxf(r[i + 4] - e[ay[i]], 0);
	}
	for (i = 0; i < 4; i++)
		if (!out[i] || !out[i + 4])
			out[i] = out[i + 4] = 0;
}

/* Ring between an outer and an inner rounded rectangle (inner may be
 * empty): backgrounds use no inner, borders use both. */
static void fill_rrect(struct canvas *c, float x, float y, float w, float h,
	const float *r, float ix, float iy, float iw, float ih,
	const float *ir, const unsigned char *rgba)
{
	int i, j, i0, j0, i1, j1, f0 = 0, f1 = 0, g0 = 0, g1 = 0;

	if (w <= 0 || h <= 0 || !rgba[3])
		return;
	i0 = ifloor(x - c->ox);
	j0 = ifloor(y - c->oy);
	i1 = iceil(x + w - c->ox);
	j1 = iceil(y + h - c->oy);
	if (i0 < c->cx0)
		i0 = c->cx0;
	if (j0 < c->cy0)
		j0 = c->cy0;
	if (i1 > c->cx1)
		i1 = c->cx1;
	if (j1 > c->cy1)
		j1 = c->cy1;
	/* a solid background: the rows clear of the corners are filled
	 * between the edges' pixels */
	if (solid(c, rgba) && (iw <= 0 || ih <= 0)) {
		float top = fmaxf(r[4], r[5]), bottom = fmaxf(r[6], r[7]);

		f0 = iceil(x - c->ox) > i0 ? iceil(x - c->ox) : i0;
		f1 = ifloor(x + w - c->ox) < i1 ?
			ifloor(x + w - c->ox) : i1;
		g0 = iceil(y + top - c->oy);
		g1 = f0 < f1 ? ifloor(y + h - bottom - c->oy) : g0;
	}
	for (j = j0; j < j1; j++)
		for (i = i0; i < i1; i++) {
			float px = (float)i + c->ox, py = (float)j + c->oy;
			int cov;

			/* (both ends inside: all between are, float
			 * rounding or not) */
			if (i == f0 && j >= g0 && j < g1 &&
			    rrect_cov(px, py, x, y, x + w, y + h, r) == 255 &&
			    rrect_cov((float)(f1 - 1) + c->ox, py, x, y, x + w,
			    y + h, r) == 255) {
				fill_span(c, j, f0, f1, rgba);
				i = f1 - 1;
				continue;
			}
			cov = rrect_cov(px, py, x, y, x + w, y + h, r);

			if (cov && iw > 0 && ih > 0)
				cov -= rrect_cov(px, py, ix, iy, ix + iw,
					iy + ih, ir) * cov / 255;
			if (cov > 0)
				blend(c, i, j, rgba, cov);
		}
}

void fill_round(struct canvas *c, float x, float y, float w, float h,
	float r, const unsigned char *rgba)
{
	const float rr[8] = { r, r, r, r, r, r, r, r };

	fill_rrect(c, x, y, w, h, rr, 0, 0, 0, 0, NULL, rgba);
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

/* Set while a box's text is painted alone into a mask */
static int text_mask_pass;

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

	{
		/* -webkit-text-fill-color when set (kept as text: its
		 * currentcolor is each element's), else the color */
		const char *fc = st->v[P_WEBKIT_TEXT_FILL_COLOR].u.str;

		style_rgba(st, P_COLOR, rgba);
		if (fc && !ascii_ieq(fc, "currentcolor")) {
			struct cvlist l;
			struct cur k;
			struct color col;

			css_component_values(fc, strlen(fc), &l);
			cur_init(&k, &l);
			if (parse_color(&k, &col) && !col.current)
				color_to_rgba(&col, rgba);
			cvlist_free(&l);
		}
	}
	if (text_mask_pass) {
		/* the shape of the text alone, for background-clip: text */
		rgba[0] = rgba[1] = rgba[2] = 0;
		rgba[3] = 255;
	}
	/* nor glyphs of thousands of pixels (an SVG's text under a huge
	 * scale: gigabytes of bitmaps) */
	if (!rgba[3] || kw(st, P_VISIBILITY, "hidden") || em > 2000)
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
			x0 = ifloor(gx - c->ox + 0.5f) + b->left;
			y0 = ifloor(base + g->y - c->oy + 0.5f) - b->top;
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
	int i;

	/* one value: a percentage is of the width across, of the height
	 * down (50%: an ellipse) */
	for (i = 0; i < 4; i++) {
		r[i] = len_val(&b->style->v[p[i]], w);
		r[i + 4] = len_val(&b->style->v[p[i]], h);
	}
	fit_radii(r, w, h);
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
	/* from the first dash the clip shows to the last: a side of
	 * 1e10 px was painted dash by dash, all of it. In doubles: that
	 * far a float no longer moves by one dash, and the loop never
	 * ended */
	{
		double step = (double)dash + gap, at;
		double lo = horiz ? (double)c->ox + c->cx0 - x :
			(double)c->oy + c->cy0 - y;
		double hi = horiz ? (double)c->ox + c->cx1 - x :
			(double)c->oy + c->cy1 - y;
		double end = len;

		if (step <= 0)
			return;
		at = lo > 0 ? floor(lo / step) * step : 0;
		if (hi < end)
			end = hi;
		for (; at < end; at += step) {
			float d = at + dash > len ? (float)(len - at) : dash;

			pos = (float)at;
			if (horiz)
				fill_rect(c, x + pos, y, d, sh, rgba);
			else
				fill_rect(c, x, y + pos, sw, d, rgba);
		}
	}
}

/* A side of a border: its color (3D styles shaded) and its pattern,
 * 's' solid, 'd' dashed, 'o' dotted, '2' double */
struct bside {
	unsigned char rgba[4];
	int kind;
	float w;
};

static void side_of(const struct box *b, int side, struct bside *s)
{
	static const int sty[4] = { P_BORDER_TOP_STYLE, P_BORDER_RIGHT_STYLE,
		P_BORDER_BOTTOM_STYLE, P_BORDER_LEFT_STYLE };
	static const int col[4] = { P_BORDER_TOP_COLOR, P_BORDER_RIGHT_COLOR,
		P_BORDER_BOTTOM_COLOR, P_BORDER_LEFT_COLOR };
	const struct style *st = b->style;

	s->w = b->b[side];
	style_rgba(st, col[side], s->rgba);
	if (kw(st, sty[side], "inset") || kw(st, sty[side], "groove"))
		shade(s->rgba, side == 0 || side == 3, s->rgba);
	else if (kw(st, sty[side], "outset") || kw(st, sty[side], "ridge"))
		shade(s->rgba, side == 1 || side == 2, s->rgba);
	s->kind = kw(st, sty[side], "dashed") ? 'd' :
		kw(st, sty[side], "dotted") ? 'o' :
		kw(st, sty[side], "double") && s->w >= 3 ? '2' : 's';
}

/* Is a point of side s drawn: t its depth across the side (0 at the
 * outer edge, 1 at the inner), a its distance along the side, len the
 * side's length. Dots are round, about a width apart, spaced to fall
 * on both corners. */
static int on_side(const struct bside *s, float t, float a, float len)
{
	float d, u, p;
	int n;

	if (s->kind == 's' || s->kind == '2')
		return 1;
	if (s->kind == 'o') {
		n = (int)roundf((len - s->w) / (2 * s->w));
		p = n > 0 ? (len - s->w) / (float)n : 2 * s->w;
		u = fmodf(a - s->w / 2 + p / 2, p) - p / 2;
		d = (t - 0.5f) * s->w;
		return u * u + d * d <= s->w * s->w / 4;
	}
	d = s->w * 3;
	return fmodf(a, 2 * d) < d;
}

static int in_rrect(float x, float y, float x0, float y0, float x1, float y1,
	const float *r)
{
	if (x < x0 || x > x1 || y < y0 || y > y1)
		return 0;
	return !corner_out(x, y, x0, y0, x1, y1, r);
}

/*
 * A border whose sides differ, or rounded: each point of the ring goes
 * to the side it is nearest, by depth over the side's width, which
 * joins sides along the diagonals of the corners (CSS triangles are
 * the borders of an empty box), 4x4 samples a pixel near the corners
 * and the patterns. Straight solid parts are filled as they come.
 */
static void paint_ring(struct canvas *c, const struct box *b, float x,
	float y, float w, float h, const float *r, const float *ir)
{
	struct bside s[4];
	float x1 = x + w, y1 = y + h, bw[4], cl, cr, ct, cb;
	float ix = x + b->b[3], iy = y + b->b[0];
	float ix1 = x1 - b->b[1], iy1 = y1 - b->b[2];
	int inner = ix1 > ix && iy1 > iy, i, j, k, i0, i1, j0, j1;
	/* double: the outer and the inner thirds, rings of their own */
	float r1[8], r2[8], t1[4], t2[4];

	for (k = 0; k < 4; k++) {
		side_of(b, k, &s[k]);
		bw[k] = s[k].w > 0 ? s[k].w : 1e-30f;
		t1[k] = b->b[k] / 3;
		t2[k] = b->b[k] * 2 / 3;
	}
	inset_radii(r, t1, r1);
	inset_radii(r, t2, r2);
	cl = fmaxf(b->b[3], fmaxf(r[0], r[3]));
	cr = fmaxf(b->b[1], fmaxf(r[1], r[2]));
	ct = fmaxf(b->b[0], fmaxf(r[4], r[5]));
	cb = fmaxf(b->b[2], fmaxf(r[6], r[7]));
	i0 = ifloor(x - c->ox);
	i1 = iceil(x1 - c->ox);
	j0 = ifloor(y - c->oy);
	j1 = iceil(y1 - c->oy);
	if (i0 < c->cx0)
		i0 = c->cx0;
	if (i1 > c->cx1)
		i1 = c->cx1;
	if (j0 < c->cy0)
		j0 = c->cy0;
	if (j1 > c->cy1)
		j1 = c->cy1;
	for (j = j0; j < j1; j++) {
		float py = (float)j + c->oy;
		int band = py < y + ct || py + 1 > y1 - cb;

		for (i = i0; i < i1; i++) {
			float px = (float)i + c->ox;
			int n[4] = { 0, 0, 0, 0 }, sx, sy;

			/* between the side bands: nothing to paint */
			if (!band && px >= x + cl && px + 1 <= x1 - cr) {
				int to = ifloor(x1 - cr - c->ox) - 1;

				if (to > i)
					i = to;
				continue;
			}
			/* inside a straight solid part */
			k = -1;
			if (px >= x + cl && px + 1 <= x1 - cr) {
				if (py >= y && py + 1 <= iy)
					k = 0;
				else if (py >= iy1 && py + 1 <= y1)
					k = 2;
			} else if (py >= y + ct && py + 1 <= y1 - cb) {
				if (px >= x && px + 1 <= ix)
					k = 3;
				else if (px >= ix1 && px + 1 <= x1)
					k = 1;
			}
			if (k >= 0 && s[k].kind == 's') {
				blend(c, i, j, s[k].rgba, 255);
				continue;
			}
			/* inside the padding box: no border there */
			if (inner && rrect_cov(px, py, ix, iy, ix1, iy1, ir) == 255)
				continue;
			for (sy = 0; sy < 4; sy++)
				for (sx = 0; sx < 4; sx++) {
					float qx = px + ((float)sx + 0.5f) / 4;
					float qy = py + ((float)sy + 0.5f) / 4;
					float d[4], t;
					int m;

					if (!in_rrect(qx, qy, x, y, x1, y1, r) ||
					    (inner && in_rrect(qx, qy, ix, iy,
					    ix1, iy1, ir)))
						continue;
					d[0] = (qy - y) / bw[0];
					d[1] = (x1 - qx) / bw[1];
					d[2] = (y1 - qy) / bw[2];
					d[3] = (qx - x) / bw[3];
					for (m = 0, k = 1; k < 4; k++)
						if (d[k] < d[m])
							m = k;
					t = d[m];
					if (s[m].kind == '2' ?
					    !in_rrect(qx, qy, x + t1[3], y + t1[0],
					    x1 - t1[1], y1 - t1[2], r1) ||
					    in_rrect(qx, qy, x + t2[3], y + t2[0],
					    x1 - t2[1], y1 - t2[2], r2) :
					    on_side(&s[m], t, m == 0 || m == 2 ?
					    qx - x : qy - y, m == 0 || m == 2 ?
					    w : h))
						n[m]++;
				}
			for (k = 0; k < 4; k++)
				if (n[k])
					blend(c, i, j, s[k].rgba, n[k] * 255 / 16);
		}
	}
}

/* A box's edges on whole pixels, as browsers paint them: two boxes
 * meeting at a fractional edge share no pixel, one over another at
 * the same place covers it whole. */
static void snap(const struct canvas *c, float *x, float *y, float *w,
	float *h)
{
	float x1 = roundf(*x + *w - c->ox), y1 = roundf(*y + *h - c->oy);

	*x = roundf(*x - c->ox);
	*y = roundf(*y - c->oy);
	*w = x1 - *x;
	*h = y1 - *y;
	*x += c->ox;
	*y += c->oy;
}

/*
 * A rounded clip: what is painted next is weighed by the rounded
 * rectangle x, y, w, h (radii r), within the mask there is (a mask of
 * the clip's pixels, as mask_push()'s). *save keeps the mask before,
 * for mask_pop(); 0 when square, nothing pushed.
 */
static int round_push(struct canvas *c, float x, float y, float w, float h,
	const float *r, struct canvas *save)
{
	int x0, y0, x1, y1, i, j;
	unsigned char *m;

	if (!r[0] && !r[1] && !r[2] && !r[3])
		return 0;
	x0 = ifloor(x - c->ox);
	y0 = ifloor(y - c->oy);
	x1 = iceil(x + w - c->ox);
	y1 = iceil(y + h - c->oy);
	x0 = x0 < c->cx0 ? c->cx0 : x0;
	y0 = y0 < c->cy0 ? c->cy0 : y0;
	x1 = x1 > c->cx1 ? c->cx1 : x1;
	y1 = y1 > c->cy1 ? c->cy1 : y1;
	if (x1 < x0)
		x1 = x0;
	if (y1 < y0)
		y1 = y0;
	m = malloc((size_t)(x1 - x0) * (size_t)(y1 - y0) + 1);
	if (!m)
		abort();
	for (j = y0; j < y1; j++)
		for (i = x0; i < x1; i++) {
			int a = rrect_cov((float)i + c->ox, (float)j + c->oy, x,
				y, x + w, y + h, r);

			m[(size_t)(j - y0) * (size_t)(x1 - x0) + (size_t)(i -
				x0)] = (unsigned char)(a * mask_at(c, i, j) /
				255);
		}
	*save = *c;
	c->mask = m;
	c->mx0 = x0;
	c->my0 = y0;
	c->mx1 = x1;
	c->my1 = y1;
	return 1;
}

static void mask_pop(struct canvas *c, const struct canvas *save);

/* b's rounded clip: its padding box (overflow), or its content box
 * (inner: replaced content), the curves inside its border's */
static int round_clip(struct canvas *c, const struct box *b, int inner,
	struct canvas *save)
{
	float x = b->x - b->p[3] - b->b[3], y = b->y - b->p[0] - b->b[0];
	float w = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
	float h = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
	float r[8], ir[8], e[4];
	int i;

	if (!b->style)
		return 0;
	snap(c, &x, &y, &w, &h);
	radii(b, w, h, r);
	if (!r[0] && !r[1] && !r[2] && !r[3])
		return 0;
	for (i = 0; i < 4; i++)
		e[i] = b->b[i] + (inner ? b->p[i] : 0);
	inset_radii(r, e, ir);
	return round_push(c, x + e[3], y + e[0], w - e[1] - e[3],
		h - e[0] - e[2], ir, save);
}

static void paint_box(struct canvas *c, struct box *b, int stacking);
static void mask_pop(struct canvas *c, const struct canvas *save);

/*
 * background-clip: text: the text of b and of what it holds, painted
 * alone and opaque into a buffer of its border box, becomes the mask
 * its background is painted through (with the mask around it, if any).
 * Gradient headings were a gradient bar over or under their text.
 */
static int text_mask_push(struct canvas *c, const struct box *b,
	struct canvas *save)
{
	struct canvas t;
	float x = b->x - b->p[3] - b->b[3], y = b->y - b->p[0] - b->b[0];
	float w = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
	float h = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
	int x0 = ifloor(x - c->ox), y0 = ifloor(y - c->oy);
	int x1 = iceil(x + w - c->ox), y1 = iceil(y + h - c->oy);
	unsigned char *m;
	int i, j;

	if (text_mask_pass)
		return 0;
	x0 = x0 < c->cx0 ? c->cx0 : x0;
	y0 = y0 < c->cy0 ? c->cy0 : y0;
	x1 = x1 > c->cx1 ? c->cx1 : x1;
	y1 = y1 > c->cy1 ? c->cy1 : y1;
	if (x1 <= x0 || y1 <= y0)
		return 0;
	*save = *c;
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
	text_mask_pass = 1;
	paint_box(&t, (struct box *)b, 1);
	text_mask_pass = 0;
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

/* The box whose background is the canvas' (canvas_background()): not
 * painted again in its own box */
static const struct box *bg_canvas;

static void paint_background(struct canvas *c, const struct box *b,
	float x, float y, float w, float h)
{
	const char *im = b->style->v[P_BACKGROUND_IMAGE].u.str;
	const char *clip = b->style->v[P_BACKGROUND_CLIP].u.str, *last;
	struct canvas save, tsave;
	int rk = 0, tk = 0;
	unsigned char rgba[4];
	float r[8];

	if (b == bg_canvas)
		return;
	/* background-clip: text: all of it through the shape of the
	 * box's text */
	if (clip && strstr(clip, "text") && im && strcmp(im, "none"))
		tk = text_mask_push(c, b, &tsave);
	snap(c, &x, &y, &w, &h);
	style_rgba(b->style, P_BACKGROUND_COLOR, rgba);
	radii(b, w, h, r);
	/* the color is clipped as the last layer's background-clip says */
	last = clip && strrchr(clip, ',') ? strrchr(clip, ',') + 1 : clip;
	while (last && *last == ' ')
		last++;
	if (rgba[3] && last && (!strncmp(last, "padding-box", 11) ||
	    !strncmp(last, "content-box", 11))) {
		float e[4], ir[8];
		int i;

		for (i = 0; i < 4; i++)
			e[i] = b->b[i] + (*last == 'c' ? b->p[i] : 0);
		inset_radii(r, e, ir);
		if (w - e[1] - e[3] > 0 && h - e[0] - e[2] > 0)
			fill_rrect(c, x + e[3], y + e[0], w - e[1] - e[3],
				h - e[0] - e[2], ir, 0, 0, 0, 0, ir, rgba);
	} else if (rgba[3]) {
		if (r[0] || r[1] || r[2] || r[3])
			fill_rrect(c, x, y, w, h, r, 0, 0, 0, 0, r, rgba);
		else
			fill_rect(c, x, y, w, h, rgba);
	}
	/* images in the curve too (gradient buttons) */
	if (im && strcmp(im, "none"))
		rk = round_push(c, x, y, w, h, r, &save);
	paint_bg_image(c, b, x, y, w, h);
	if (rk)
		mask_pop(c, &save);
	if (tk)
		mask_pop(c, &tsave);
}

static void paint_borders(struct canvas *c, const struct box *b, float x,
	float y, float w, float h)
{
	float r[8], ir[8];
	int i, k, same = 1;
	struct bside sd[4];

	snap(c, &x, &y, &w, &h);
	if (!b->b[0] && !b->b[1] && !b->b[2] && !b->b[3])
		return;
	radii(b, w, h, r);
	/* sides alike, square corners: each side a rectangle */
	for (i = 0, k = -1; i < 4 && same; i++) {
		side_of(b, i, &sd[i]);
		if (sd[i].w <= 0)
			continue;
		if (k >= 0 && (memcmp(sd[i].rgba, sd[k].rgba, 4) ||
		    sd[i].kind != sd[k].kind))
			same = 0;
		k = i;
	}
	if (k < 0)
		return;		/* no side has a width (one of 1e10px: NaN) */
	if (same && sd[k].kind != 'o' && !r[0] && !r[1] && !r[2] && !r[3]) {
		for (i = 0; i < 4; i++)
			border_side(c, b, i, x, y, w, h);
		return;
	}
	inset_radii(r, b->b, ir);
	paint_ring(c, b, x, y, w, h, r, ir);
}

/* box-shadow */

struct shadow {
	float x, y, blur, spread;
	unsigned char rgba[4];
	int inset;
};

#define NSHADOWS 8

/* The shadows of a style, as listed (the first on top), their color
 * currentcolor unless given; 0 for none or a list that does not
 * parse. */
static int shadows(const struct style *st, struct shadow *sh)
{
	const char *s = st ? st->v[P_BOX_SHADOW].u.str : NULL;
	float em;
	struct cvlist l;
	struct cur k;
	const struct cv *v;
	unsigned char cur[4];
	int n = 0, nl = 0, ok = 1;

	if (!s || !strcmp(s, "none"))
		return 0;
	em = st->v[P_FONT_SIZE].u.len.px;
	style_rgba(st, P_COLOR, cur);
	memset(sh, 0, sizeof(*sh));
	memcpy(sh->rgba, cur, 4);
	css_component_values(s, strlen(s), &l);
	cur_init(&k, &l);
	while ((v = cur_peek(&k))) {
		struct color col;

		if (v->type == CT_COMMA) {
			cur_next(&k);
			if (nl < 2 || n + 1 >= NSHADOWS) {
				ok = 0;
				break;
			}
			n++;
			nl = 0;
			memset(&sh[n], 0, sizeof(*sh));
			memcpy(sh[n].rgba, cur, 4);
		} else if (cv_ident(v, "inset") && !sh[n].inset) {
			cur_next(&k);
			sh[n].inset = 1;
		} else if ((v->type == CT_DIMENSION || (v->type ==
		    CT_NUMBER && !v->num)) && nl < 4) {
			float px = css_len_px(v, 0, em);

			cur_next(&k);
			if (nl == 0)
				sh[n].x = px;
			else if (nl == 1)
				sh[n].y = px;
			else if (nl == 2)	/* 300 px at most, as browsers:
						 * its buffer grows with it */
				sh[n].blur = px > 300 * doc_zoom() ? 300 *
					doc_zoom() : px;
			else
				sh[n].spread = px;
			if (nl++ == 2 && px < 0) {
				ok = 0;
				break;
			}
		} else if (parse_color(&k, &col)) {
			if (!col.current)
				color_to_rgba(&col, sh[n].rgba);
		} else {
			ok = 0;
			break;
		}
	}
	cvlist_free(&l);
	return ok && nl >= 2 ? n + 1 : 0;
}

float shadow_reach(const struct style *st)
{
	struct shadow sh[NSHADOWS];
	float m = 0;
	int i, n = shadows(st, sh);

	for (i = 0; i < n; i++) {
		float r = fmaxf(fabsf(sh[i].x), fabsf(sh[i].y)) +
			sh[i].spread + sh[i].blur * 1.5f + 1;

		if (!sh[i].inset && r > m)
			m = r;
	}
	return m;
}

/* A corner's radius once its shape is spread by s (shrunk when s < 0):
 * a small radius grows less than the spread, square stays square
 * (css-backgrounds-3 7.1.1) */
static float spread_r(float r, float s)
{
	float q;

	if (r <= 0)
		return 0;
	if (s <= 0)
		return r + s > 0 ? r + s : 0;
	q = r / s;
	if (q >= 1)
		return r + s;
	q -= 1;
	return r + s * (1 + q * q * q);
}

/* One box blur over n values a step apart, its window i - lo to i + hi,
 * the ends repeated */
static void box_pass(unsigned char *p, int n, int step, int lo, int hi,
	unsigned char *t)
{
	int i, sum = 0, w = lo + hi + 1;

	for (i = 0; i < n; i++)
		t[i] = p[(size_t)i * (size_t)step];
	for (i = 1; i < n && t[i] == t[0]; i++)
		;
	if (i == n)
		return;			/* flat: stays so */
	for (i = -lo; i <= hi; i++)
		sum += t[i < 0 ? 0 : i >= n ? n - 1 : i];
	for (i = 0; i < n; i++) {
		int a = i - lo, z = i + hi + 1;

		p[(size_t)i * (size_t)step] = (unsigned char)((sum + w / 2) /
			w);
		sum += t[z >= n ? n - 1 : z] - t[a < 0 ? 0 : a];
	}
}

/* A Gaussian blur of a line, three box blurs of width d (SVG's
 * feGaussianBlur) */
static void blur_line(unsigned char *p, int n, int step, int d,
	unsigned char *t)
{
	if (d < 2)
		return;
	if (d & 1) {
		box_pass(p, n, step, d / 2, d / 2, t);
		box_pass(p, n, step, d / 2, d / 2, t);
		box_pass(p, n, step, d / 2, d / 2, t);
	} else {
		box_pass(p, n, step, d / 2, d / 2 - 1, t);
		box_pass(p, n, step, d / 2 - 1, d / 2, t);
		box_pass(p, n, step, d / 2, d / 2, t);
	}
}

/*
 * One shadow of a box whose border box is x, y, w, h (radii r) and
 * padding box px, py, pw, ph (radii pr). An outer shadow is the border
 * box moved and spread, blurred, painted out of the border box only;
 * an inset one is the padding box less the moved and shrunk hole,
 * blurred, painted in the padding box. The blur (a Gaussian of
 * deviation blur / 2) runs over a buffer of what the clip shows, with
 * the margin it reads from.
 */
static void paint_shadow(struct canvas *c, const struct shadow *s,
	float x, float y, float w, float h, const float *r, float px,
	float py, float pw, float ph, const float *pr)
{
	float sx, sy, sw, sh, sr[8];
	int d = ifloor(s->blur / 2 * 1.87997f + 0.5f);
	int m = d < 2 ? 0 : (d * 3 + 1) / 2 + 1;
	int i, j, i0, j0, i1, j1, b0, c0, b1, c1, bw, bh;
	unsigned char *buf, *t;

	if (s->inset) {
		sx = px + s->x + s->spread;
		sy = py + s->y + s->spread;
		sw = pw - 2 * s->spread;
		sh = ph - 2 * s->spread;
		for (i = 0; i < 8; i++)
			sr[i] = spread_r(pr[i], -s->spread);
		/* painted: the padding box */
		i0 = ifloor(px - c->ox);
		j0 = ifloor(py - c->oy);
		i1 = iceil(px + pw - c->ox);
		j1 = iceil(py + ph - c->oy);
	} else {
		sx = x + s->x - s->spread;
		sy = y + s->y - s->spread;
		sw = w + 2 * s->spread;
		sh = h + 2 * s->spread;
		if (sw <= 0 || sh <= 0)
			return;
		for (i = 0; i < 8; i++)
			sr[i] = spread_r(r[i], s->spread);
		/* painted: the shape and its blur */
		i0 = ifloor(sx - c->ox) - m;
		j0 = ifloor(sy - c->oy) - m;
		i1 = iceil(sx + sw - c->ox) + m;
		j1 = iceil(sy + sh - c->oy) + m;
	}
	if (sw < 0)
		sw = 0;
	if (sh < 0)
		sh = 0;
	fit_radii(sr, sw, sh);
	if (i0 < c->cx0)
		i0 = c->cx0;
	if (j0 < c->cy0)
		j0 = c->cy0;
	if (i1 > c->cx1)
		i1 = c->cx1;
	if (j1 > c->cy1)
		j1 = c->cy1;
	if (i0 >= i1 || j0 >= j1 || !s->rgba[3])
		return;
	/* the buffer: what is painted and the margin the blur reads */
	b0 = i0 - m;
	c0 = j0 - m;
	b1 = i1 + m;
	c1 = j1 + m;
	bw = b1 - b0;
	bh = c1 - c0;
	buf = malloc((size_t)bw * (size_t)bh);
	t = malloc((size_t)(bw > bh ? bw : bh));
	if (!buf || !t)
		abort();
	for (j = 0; j < bh; j++)
		for (i = 0; i < bw; i++) {
			int a = sw > 0 && sh > 0 ? rrect_cov((float)(b0 + i) +
				c->ox, (float)(c0 + j) + c->oy, sx, sy,
				sx + sw, sy + sh, sr) : 0;

			buf[(size_t)j * (size_t)bw + (size_t)i] =
				(unsigned char)(s->inset ? 255 - a : a);
		}
	for (j = 0; j < bh; j++)
		blur_line(buf + (size_t)j * (size_t)bw, bw, 1, d, t);
	for (i = 0; i < bw; i++)
		blur_line(buf + i, bh, bw, d, t);
	for (j = j0; j < j1; j++)
		for (i = i0; i < i1; i++) {
			int a = buf[(size_t)(j - c0) * (size_t)bw +
				(size_t)(i - b0)];
			float qx = (float)i + c->ox, qy = (float)j + c->oy;

			if (!a)
				continue;
			if (s->inset)
				a = a * rrect_cov(qx, qy, px, py, px + pw,
					py + ph, pr) / 255;
			else
				a = a * (255 - rrect_cov(qx, qy, x, y, x + w,
					y + h, r)) / 255;
			if (a > 0)
				blend(c, i, j, s->rgba, a);
		}
	free(t);
	free(buf);
}

/* A box's outer shadows, background, inset shadows and borders */
static void paint_decor(struct canvas *c, const struct box *b, float x,
	float y, float w, float h)
{
	struct shadow sh[NSHADOWS];
	int i, n = shadows(b->style, sh);
	float sx = x, sy = y, sw = w, shh = h, r[8], ir[8];

	if (text_mask_pass)
		return;
	if (n) {
		snap(c, &sx, &sy, &sw, &shh);
		radii(b, sw, shh, r);
		inset_radii(r, b->b, ir);
		for (i = n - 1; i >= 0; i--)
			if (!sh[i].inset)
				paint_shadow(c, &sh[i], sx, sy, sw, shh, r,
					sx + b->b[3], sy + b->b[0], sw - b->b[1] -
					b->b[3], shh - b->b[0] - b->b[2], ir);
	}
	if (native_control(b)) {
		/* a control left with the style sheet's own border: as
		 * browsers draw it, a thin rounded ring, not the 2 px inset
		 * or outset bevel its computed style says (its shadows are
		 * the page's: painted as for any box) */
		static const unsigned char ring[4] = { 143, 143, 157, 255 };
		unsigned char bg[4];
		float nx = x, ny = y, nw = w, nh = h;

		snap(c, &nx, &ny, &nw, &nh);
		style_rgba(b->style, P_BACKGROUND_COLOR, bg);
		if (!bg[3])
			bg[0] = bg[1] = bg[2] = bg[3] = 255;
		fill_round(c, nx, ny, nw, nh, 4, ring);
		fill_round(c, nx + 1, ny + 1, nw - 2, nh - 2, 3, bg);
	} else {
		paint_background(c, b, x, y, w, h);
	}
	for (i = n - 1; i >= 0; i--)
		if (sh[i].inset)
			paint_shadow(c, &sh[i], sx, sy, sw, shh, r, sx + b->b[3],
				sy + b->b[0], sw - b->b[1] - b->b[3], shh -
				b->b[0] - b->b[2], ir);
	if (!native_control(b))
		paint_borders(c, b, x, y, w, h);
}

static void paint_outline(struct canvas *c, const struct box *b, float x,
	float y, float w, float h)
{
	const struct style *st = b->style;
	float ow = st->v[P_OUTLINE_WIDTH].u.len.px;
	float off = st->v[P_OUTLINE_OFFSET].u.len.px;
	unsigned char rgba[4];

	if (text_mask_pass)
		return;

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
static const struct box *decorating(const struct box *t,
	const struct box *container)
{
	const struct box *b;

	for (b = t->parent; b; b = b->parent) {
		if ((int)b->style->v[P_TEXT_DECORATION_LINE].u.num)
			return b;
		if (b == container)
			break;
	}
	return NULL;
}

/* The decorating box's baseline in line l: a sup inside <u> is raised,
 * the underline is not */
static float deco_base(const struct box *d, const struct box *b,
	const struct line *l, const struct frag *f)
{
	int j;

	if (d->type != B_INLINE)
		return l->baseline;
	for (j = l->first; j < l->first + l->n; j++)
		if (b->frags[j].kind == F_INLINE && b->frags[j].box == d)
			return b->frags[j].baseline;
	return f->baseline;
}

static void paint_box(struct canvas *c, struct box *b, int stacking);

static void paint_lines(struct canvas *c, struct box *b)
{
	int i, j;

	for (i = 0; i < b->nlines; i++) {
		struct line *l = &b->lines[i];
		/* text-overflow: ellipsis: a line that runs out of a box
		 * that clips it ends with "..." (limit: where it goes) */
		float limit = 1e30f;

		if (b->style && kw(b->style, P_TEXT_OVERFLOW, "ellipsis") &&
		    !kw(b->style, P_OVERFLOW_X, "visible") && l->n) {
			float end = -1e30f;

			for (j = l->first; j < l->first + l->n; j++)
				if (b->frags[j].kind != F_INLINE &&
				    b->frags[j].x + b->frags[j].w > end)
					end = b->frags[j].x + b->frags[j].w;
			if (end > b->x + b->w + 0.5f)
				limit = b->x + b->w - text_width(b->style,
					"\xe2\x80\xa6", 3);
		}

		/* inline boxes first, then the text over them */
		for (j = l->first; j < l->first + l->n; j++) {
			struct frag *f = &b->frags[j];

			if (f->kind == F_INLINE) {
				struct box *ib = f->box;

				paint_decor(c, ib, f->x, f->y, f->w, f->h);
			}
		}
		for (j = l->first; j < l->first + l->n; j++) {
			struct frag *f = &b->frags[j];

			if (f->kind == F_TEXT) {
				const struct box *db = decorating(f->box, b);
				float jw = l->jspace;
				float em = f->box->style->v[P_FONT_SIZE].u.len.px;

				/* text out of the clip, its glyphs reaching
				 * 2em up and 1em down, is not even shaped:
				 * a band of a long page paints fast */
				if (f->baseline + em < c->oy + (float)c->cy0 ||
				    f->baseline - 2 * em > c->oy + (float)c->cy1)
					continue;
				if (f->x + f->w > limit) {
					/* what fits of it, then the ellipsis in
					 * the box's own style */
					const char *t = f->box->text + f->start;
					size_t n = 0, nx;
					float w = 0, cw;

					while (f->x < limit && n < f->len) {
						for (nx = n + 1; nx < f->len &&
						     ((unsigned char)t[nx] & 0xc0) ==
						     0x80; nx++)
							;
						cw = text_width(f->box->style, t + n,
							nx - n);
						if (f->x + w + cw > limit)
							break;
						w += cw;
						n = nx;
					}
					if (n)
						draw_text(c, f->box->style, t, n,
							f->x, f->baseline, 0);
					draw_text(c, b->style, "\xe2\x80\xa6", 3,
						f->x < limit ? f->x + w : limit,
						f->baseline, 0);
					break;
				}

				draw_text(c, f->box->style, f->box->text +
					f->start, f->len, f->x, f->baseline, jw);
				if (db) {
					float y = deco_base(db, b, l, f);
					struct fontm m;

					font_metrics(db->style, &m);
					decorate(c, db->style, f->x, f->w, y,
						y - m.ascent);
				}
			} else if (f->kind == F_ATOMIC) {
				if (f->x >= limit)
					break;
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

static void zadd(struct box *b)
{
	if (text_mask_pass)
		return;		/* painted in its turn, once */
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
	const char *k = b->style ? b->style->v[P_CLIP_PATH].u.str : NULL;

	return (m && strcmp(m, "none")) || (k && strcmp(k, "none"));
}

/*
 * mask-image and clip-path: b's mask layers and its shape painted into
 * a buffer of its border box (clipped), whose alpha then weighs every
 * pixel painted (blend()); inside another mask, both. *save keeps the mask before, for
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
	x0 = ifloor(x - c->ox);
	y0 = ifloor(y - c->oy);
	x1 = iceil(x + w - c->ox);
	y1 = iceil(y + h - c->oy);
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
	if (strcmp(b->style->v[P_MASK_IMAGE].u.str, "none"))
		paint_layers_of(&t, b, P_MASK_IMAGE, x, y, w, h);
	else
		memset(t.px, 255, (size_t)t.w * (size_t)t.h * 4);
	for (j = 0; j < t.h; j++)
		for (i = 0; i < t.w; i++)
			m[(size_t)j * (size_t)t.w + (size_t)i] = (unsigned char)
				(t.px[(size_t)j * (size_t)t.stride +
				(size_t)i * 4 + 3] * mask_at(c, x0 + i,
				y0 + j) / 255);
	/* clip-path: its shape's coverage too */
	memset(t.px, 0, (size_t)t.w * (size_t)t.h * 4);
	if (clip_path_paint(&t, b))
		for (j = 0; j < t.h; j++)
			for (i = 0; i < t.w; i++) {
				unsigned char *q = m + (size_t)j *
					(size_t)t.w + (size_t)i;

				*q = (unsigned char)(*q * t.px[(size_t)j *
					(size_t)t.stride + (size_t)i * 4 +
					3] / 255);
			}
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

/*
 * Floats are painted over the backgrounds of all the blocks of their
 * formatting context (CSS 2 Appendix E, steps 4 and 5), not only of
 * their siblings: a float kept by a block was covered by the
 * background of the block after it. They wait in this list for the
 * box that paints as a unit around them (float_owner()).
 */
static struct box **flist;
static int nfl, capfl;

static void float_add(struct box *b)
{
	if (nfl == capfl) {
		capfl = capfl ? capfl * 2 : 16;
		flist = realloc(flist, (size_t)capfl * sizeof(*flist));
		if (!flist)
			abort();
	}
	flist[nfl++] = b;
}

/* Painted with all it holds before anything after it: the root, a
 * stacking unit, a float, an inline-block, and what clips or fades its
 * content (the floats keep that clip and that opacity) */
static int float_owner(const struct box *b, int stacking)
{
	const struct style *st = b->style;
	const char *m, *k;

	if (stacking || !b->parent || !st || (b->flags & (BF_FLOAT | BF_ABS)) ||
	    b->type == B_ATOMIC || positioned(b))
		return 1;
	m = st->v[P_MASK_IMAGE].u.str;
	k = st->v[P_CLIP_PATH].u.str;
	return !kw(st, P_OVERFLOW_X, "visible") ||
		!kw(st, P_OVERFLOW_Y, "visible") || st->v[P_OPACITY].u.num < 1 ||
		(m && strcmp(m, "none")) || (k && strcmp(k, "none"));
}

/* Floats and positioned boxes met in the text, also inside inline
 * boxes (a <span> around a dialog) */
static void paint_out_of_flow(struct canvas *c, struct box *b)
{
	struct box *k;

	for (k = b->first; k; k = k->next)
		if (k->flags & BF_FLOAT)
			float_add(k);
		else if (k->flags & BF_ABS)
			paint_box(c, k, 0);
		else if (k->type == B_INLINE)
			paint_out_of_flow(c, k);
}

static void paint_context(struct canvas *c, struct box *b);
static void paint_context_in(struct canvas *c, struct box *b,
	struct zent *outer, struct zent *neg);

/* Painted through a matrix (paint_transformed()), a stacking context
 * of its own whatever its position */
static int transformed(const struct box *b)
{
	float m[6];

	return b->style && b->parent && b->type != B_TEXT &&
		box_transform(b, m) == 2;
}

/* Where b and everything below it are, untransformed */
static void subtree_bounds(const struct box *b, float *r, int depth)
{
	const struct box *k;
	float x = b->x - b->p[3] - b->b[3], y = b->y - b->p[0] - b->b[0];
	float x1 = b->x + b->w + b->p[1] + b->b[1];
	float y1 = b->y + b->h + b->p[2] + b->b[2];
	int i;

	if (b->type != B_INLINE && b->type != B_TEXT && b->type != B_BREAK) {
		float e = b->style ? shadow_reach(b->style) + 2 : 2;

		if (x - e < r[0])
			r[0] = x - e;
		if (y - e < r[1])
			r[1] = y - e;
		if (x1 + e > r[2])
			r[2] = x1 + e;
		if (y1 + e > r[3])
			r[3] = y1 + e;
	}
	for (i = 0; i < b->nfrags; i++) {
		const struct frag *f = &b->frags[i];

		if (f->x < r[0])
			r[0] = f->x;
		if (f->y < r[1])
			r[1] = f->y;
		if (f->x + f->w > r[2])
			r[2] = f->x + f->w;
		if (f->y + f->h > r[3])
			r[3] = f->y + f->h;
	}
	if (depth > 256 || (b->style && !kw(b->style, P_OVERFLOW_X,
	    "visible") && b->type != B_INLINE))
		return;		/* clipped to it */
	for (k = b->first; k; k = k->next)
		subtree_bounds(k, r, depth + 1);
}

/* transform-origin in the border box w x h: keywords, lengths,
 * percentages */
static void transform_origin(const struct box *b, float w, float h, float *ox,
	float *oy)
{
	const char *s = b->style->v[P_TRANSFORM_ORIGIN].u.str;
	float em = b->style->v[P_FONT_SIZE].u.len.px;
	struct cvlist l;
	struct cur k;
	const struct cv *v;
	int n = 0;

	*ox = w / 2;
	*oy = h / 2;
	if (!s)
		return;
	css_component_values(s, strlen(s), &l);
	cur_init(&k, &l);
	while ((v = cur_next(&k)) && n < 2) {
		if (cv_ident(v, "left"))
			*ox = 0;
		else if (cv_ident(v, "right"))
			*ox = w;
		else if (cv_ident(v, "top"))
			*oy = 0;
		else if (cv_ident(v, "bottom"))
			*oy = h;
		else if (cv_ident(v, "center"))
			;
		else if (!n)
			*ox = css_len_px(v, w, em);
		else
			*oy = css_len_px(v, h, em);
		n++;
	}
	cvlist_free(&l);
}

/* The box being painted into its own canvas, to be transformed */
static const struct box *tf_now;

/*
 * A box with a rotation, a scale or a skew (box_transform() gave m):
 * painted with everything it holds into a canvas of its own, as a
 * stacking context, then drawn through the matrix around its origin,
 * each pixel taking its color from where it comes from (bilinear).
 * Returns 0 when it cannot (too large): painted as it lies.
 */
static int paint_transformed(struct canvas *c, struct box *b, const float *m)
{
	float r[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
	float x = b->x - b->p[3] - b->b[3], y = b->y - b->p[0] - b->b[0];
	float w = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
	float h = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
	float ox, oy, det, inv[6], tx, ty, bx0, by0, bx1, by1;
	const struct box *outer = tf_now;
	struct canvas t;
	int i, j, k, x0, y0, x1, y1, ch = c->bgr ? 2 : 0;

	(void)ch;
	subtree_bounds(b, r, 0);
	if (r[2] <= r[0] || r[3] <= r[1])
		return 1;
	det = m[0] * m[3] - m[1] * m[2];
	if (fabsf(det) < 1e-6f)
		return 1;	/* scale(0): nothing shows */
	memset(&t, 0, sizeof(t));
	t.ox = floorf(r[0]);
	t.oy = floorf(r[1]);
	t.w = (int)(ceilf(r[2]) - t.ox);
	t.h = (int)(ceilf(r[3]) - t.oy);
	if (t.w <= 0 || t.h <= 0 || t.w > 8192 || t.h > 8192 ||
	    (size_t)t.w * (size_t)t.h > 16u << 20)
		return 0;
	t.stride = t.w * 4;
	t.px = calloc((size_t)t.w * (size_t)t.h + 1, 4);
	if (!t.px)
		return 0;
	t.cx1 = t.w;
	t.cy1 = t.h;
	t.alpha = 1;
	t.bgr = c->bgr;
	tf_now = b;
	paint_context(&t, b);
	tf_now = outer;
	/* page = O + M (p - O): where the origin stays put */
	transform_origin(b, w, h, &ox, &oy);
	ox += x;
	oy += y;
	tx = ox + m[4] - (m[0] * ox + m[2] * oy);
	ty = oy + m[5] - (m[1] * ox + m[3] * oy);
	inv[0] = m[3] / det;
	inv[1] = -m[1] / det;
	inv[2] = -m[2] / det;
	inv[3] = m[0] / det;
	inv[4] = -(inv[0] * tx + inv[2] * ty);
	inv[5] = -(inv[1] * tx + inv[3] * ty);
	/* what the canvas covers once transformed */
	bx0 = by0 = 1e30f;
	bx1 = by1 = -1e30f;
	for (k = 0; k < 4; k++) {
		float px = t.ox + (k & 1 ? (float)t.w : 0);
		float py = t.oy + (k & 2 ? (float)t.h : 0);
		float qx = m[0] * px + m[2] * py + tx;
		float qy = m[1] * px + m[3] * py + ty;

		if (qx < bx0)
			bx0 = qx;
		if (qx > bx1)
			bx1 = qx;
		if (qy < by0)
			by0 = qy;
		if (qy > by1)
			by1 = qy;
	}
	x0 = ifloor(bx0 - c->ox);
	y0 = ifloor(by0 - c->oy);
	x1 = iceil(bx1 - c->ox);
	y1 = iceil(by1 - c->oy);
	if (x0 < c->cx0)
		x0 = c->cx0;
	if (y0 < c->cy0)
		y0 = c->cy0;
	if (x1 > c->cx1)
		x1 = c->cx1;
	if (y1 > c->cy1)
		y1 = c->cy1;
	for (j = y0; j < y1; j++)
		for (i = x0; i < x1; i++) {
			float px = (float)i + c->ox + 0.5f;
			float py = (float)j + c->oy + 0.5f;
			float sx = inv[0] * px + inv[2] * py + inv[4] - t.ox - 0.5f;
			float sy = inv[1] * px + inv[3] * py + inv[5] - t.oy - 0.5f;
			int ix = ifloor(sx), iy = ifloor(sy);
			float fx = sx - (float)ix, fy = sy - (float)iy, f;
			float o[4] = { 0, 0, 0, 0 };
			unsigned char *d;
			int a, q;

			if (ix < -1 || iy < -1 || ix >= t.w || iy >= t.h)
				continue;
			for (q = 0; q < 4; q++) {
				int ux = ix + (q & 1), uy = iy + (q >> 1);
				float wgt = (q & 1 ? fx : 1 - fx) *
					(q >> 1 ? fy : 1 - fy);
				const unsigned char *sp;

				if (ux < 0 || uy < 0 || ux >= t.w || uy >= t.h)
					continue;
				sp = t.px + (size_t)uy * (size_t)t.stride +
					(size_t)ux * 4;
				for (k = 0; k < 4; k++)
					o[k] += sp[k] * wgt;
			}
			if (o[3] < 0.5f)
				continue;
			f = c->alpha * (c->mask ? (float)mask_at(c, i, j) / 255 :
				1);
			a = (int)(o[3] * f + 0.5f);
			if (a <= 0)
				continue;
			d = c->px + (size_t)j * (size_t)c->stride + (size_t)i * 4;
			/* the canvas of its own holds colors times alpha */
			for (k = 0; k < 3; k++) {
				float v = o[k] * f + d[k] * (255 - a) / 255.f;

				d[k] = (unsigned char)(v > 255 ? 255 : v + 0.5f);
			}
			d[3] = (unsigned char)(a + d[3] * (255 - a) / 255);
		}
	free(t.px);
	return 1;
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
	struct canvas msave, rsave;
	int mk, rk, fbase = nfl;

	if (!stacking && positioned(b) && b->parent) {
		zadd(b);
		return;
	}
	if (b->type == B_TEXT || b->type == B_BREAK)
		return;
	if (b != tf_now && b->style && b->parent) {
		float m[6];

		if (box_transform(b, m) == 2 && paint_transformed(c, b, m))
			return;
	}
	mk = mask_push(c, b, &msave);
	clip_rect(c, b);
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
		paint_decor(c, b, x, y, w, h);
	}
	if (b->type == B_REPLACED) {
		/* an image in the curve of its content box; controls
		 * draw their own */
		rk = !control(b) && round_clip(c, b, 1, &rsave);
		if (!text_mask_pass)
			paint_replaced(c, b);
		if (rk)
			mask_pop(c, &rsave);
		paint_outline(c, b, x, y, w, h);
		c->cx0 = cx0;
		c->cy0 = cy0;
		c->cx1 = cx1;
		c->cy1 = cy1;
		c->alpha = save_alpha;
		if (mk)
			mask_pop(c, &msave);
		return;
	}
	content_clip(c, b);
	rk = clips(b) && round_clip(c, b, 0, &rsave);
	if (b->flags & BF_INLINE_KIDS) {
		paint_lines(c, b);
		paint_out_of_flow(c, b);
	} else {
		for (k = b->first; k; k = k->next)
			if (k->flags & BF_FLOAT)
				float_add(k);
			else
				paint_box(c, k, 0);
	}
	if (float_owner(b, stacking)) {
		/* the floats met below, over the blocks */
		int i;

		for (i = fbase; i < nfl; i++)
			paint_box(c, flist[i], 0);
		nfl = fbase;
	}
	if (rk)
		mask_pop(c, &rsave);
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
		/* a transformed box is painted whole, with its own layers:
		 * they are not this context's */
		if (transformed(k))
			continue;
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
	float tm[6];

	/* transformed: all of it through its matrix, its background and
	 * its negative layers too (they were painted upright as well) */
	if (b != tf_now && b->style && b->parent && box_transform(b, tm) == 2 &&
	    paint_transformed(c, b, tm))
		return;
	paint_context_in(c, b, outer, neg);
}

static void paint_context_in(struct canvas *c, struct box *b,
	struct zent *outer, struct zent *neg)
{
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

			paint_decor(c, b, x, y, w, h);
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

/* The clipping of b's ancestors on its containing block chain
 * (overflow, clip), which their walk did not give it: it is painted
 * out of it. An absolute box escapes the static ones up to its
 * containing block, a fixed one all of them. */
#define NROUND 8

/* The clipping of b's containing block chain; the rounded ones (the
 * NROUND nearest) pushed as masks, saved in save[] for mask_pop(),
 * their number returned */
static int ancestor_clip(struct canvas *c, const struct box *b,
	struct canvas *save)
{
	const struct box *e = b, *a;
	int n = 0;

	for (a = b->parent; a; a = a->parent) {
		if (e->style && kw(e->style, P_POSITION, "fixed"))
			break;
		if (e->style && kw(e->style, P_POSITION, "absolute") &&
		    !positioned(a))
			continue;
		content_clip(c, a);
		if (n < NROUND && clips(a) && round_clip(c, a, 0, &save[n]))
			n++;
		e = a;
	}
	return n;
}

/* A layer: a stacking context paints its own layers, under its
 * opacity; another one leaves them to the context it is in */
static void paint_layer(struct canvas *c, struct box *b)
{
	int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
	struct canvas save[NROUND];
	int n = ancestor_clip(c, b, save);

	if (!stacking_context(b))
		paint_box(c, b, 1);
	else
		paint_context(c, b);
	while (n--)
		mask_pop(c, &save[n]);
	c->cx0 = cx0;
	c->cy0 = cy0;
	c->cx1 = cx1;
	c->cy1 = cy1;
}

/* The canvas background: the root's, or the body's (CSS 2 14.2). */
static void canvas_background(struct canvas *c, struct box *root)
{
	unsigned char rgba[4] = { 255, 255, 255, 255 }, bg[4];
	const struct box *src = root, *k;
	const char *im = root->style->v[P_BACKGROUND_IMAGE].u.str;
	int i, j;

	style_rgba(root->style, P_BACKGROUND_COLOR, bg);
	bg_canvas = NULL;
	if (!bg[3] && (!im || !strcmp(im, "none"))) {
		/* nothing on the root: the body's, colors and images, and
		 * the body does not paint it again in its own box */
		for (k = root->first; k; k = k->next)
			if (k->node && is_html(k->node, TAG_BODY))
				break;
		if (k) {
			src = bg_canvas = k;
			style_rgba(src->style, P_BACKGROUND_COLOR, bg);
		}
	}
	for (j = 0; j < c->h; j++)
		for (i = 0; i < c->w; i++)
			memcpy(c->px + (size_t)j * (size_t)c->stride +
				(size_t)i * 4, rgba, 4);
	if (bg[3])
		fill_rect(c, c->ox, c->oy, (float)c->w, (float)c->h, bg);
	paint_bg_canvas(c, src);
}

static int paint(soie_node *doc, unsigned char *rgba, int width, int height,
	int stride, int sx, int sy, int bgr)
{
	struct canvas c;
	struct box *root;

	if (doc->type != SOIE_DOCUMENT || !doc->u.doc->layout)
		return -1;
	root = doc->u.doc->layout->root;
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
