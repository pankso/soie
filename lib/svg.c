/*
 * SVG, static subset: shapes, paths, groups with transforms, fill and
 * stroke, viewBox. Rendered from a DOM (inline <svg>, or an SVG file
 * parsed by the HTML parser, which handles foreign content) with an
 * anti-aliased polygon filler (analytic in x, 4 sub-scanlines in y).
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "paint.h"
#include "image.h"
#include "font.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Polygon filling */

struct edge {
	float x0, y0, x1, y1;
	int dir;
};

struct poly {
	struct edge *e;
	int n, cap;
	float px, py;		/* current point */
	float sx, sy;		/* subpath start */
};

static void poly_edge(struct poly *p, float x0, float y0, float x1, float y1)
{
	struct edge *e;

	if (y0 == y1)
		return;
	/* NaN, infinities, absurd sizes: nothing an int pixel can hold */
	if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
	    fabsf(x0) > 1e7f || fabsf(y0) > 1e7f || fabsf(x1) > 1e7f ||
	    fabsf(y1) > 1e7f)
		return;
	if (p->n == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 64;
		p->e = xrealloc(p->e, (size_t)p->cap * sizeof(*p->e));
	}
	e = &p->e[p->n++];
	e->dir = y1 > y0 ? 1 : -1;
	if (y0 < y1) {
		e->x0 = x0, e->y0 = y0, e->x1 = x1, e->y1 = y1;
	} else {
		e->x0 = x1, e->y0 = y1, e->x1 = x0, e->y1 = y0;
	}
}

/* A segment of a stroke's outline, as drawn (horizontal ones too) */
static void seg_add(struct poly *p, float x0, float y0, float x1, float y1)
{
	struct edge *e;

	if ((x0 == x1 && y0 == y1) || !isfinite(x0) || !isfinite(y0) ||
	    !isfinite(x1) || !isfinite(y1) || fabsf(x0) > 1e7f ||
	    fabsf(y0) > 1e7f || fabsf(x1) > 1e7f || fabsf(y1) > 1e7f)
		return;
	if (p->n == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 64;
		p->e = xrealloc(p->e, (size_t)p->cap * sizeof(*p->e));
	}
	e = &p->e[p->n++];
	e->x0 = x0, e->y0 = y0, e->x1 = x1, e->y1 = y1;
	e->dir = 0;
}

struct xing {
	float x;
	int dir;
};

static int xcmp(const void *a, const void *b)
{
	float d = ((const struct xing *)a)->x - ((const struct xing *)b)->x;

	return d < 0 ? -1 : d > 0;
}

/* Transforms: a b c d e f, x' = a x + c y + e */
struct mat {
	float a, b, c, d, e, f;
};

/*
 * A gradient, ready to paint: pixel centres through inv into its
 * space, there a position t along it, its color in lut (straight
 * RGBA, t 0..1 in 256 steps).
 */
struct grad {
	int radial;
	char spread;		/* 'p'ad, 'r'eflect, re'p'eat: 'p', 'r', 't' */
	struct mat inv;
	float x1, y1, x2, y2;	/* linear */
	float cx, cy, r, fx, fy, fr;	/* radial */
	unsigned char lut[256][4];
};

/* The color of g at pixel x, y */
static const unsigned char *grad_at(const struct grad *g, int x, int y)
{
	float px = (float)x + 0.5f, py = (float)y + 0.5f;
	float u = g->inv.a * px + g->inv.c * py + g->inv.e;
	float v = g->inv.b * px + g->inv.d * py + g->inv.f;
	float t;

	if (!g->radial) {
		float dx = g->x2 - g->x1, dy = g->y2 - g->y1;
		float l = dx * dx + dy * dy;

		t = l > 0 ? ((u - g->x1) * dx + (v - g->y1) * dy) / l : 1;
	} else {
		/* the largest t whose circle, from the focal one to the
		 * end one, goes through (u, v) */
		float cdx = g->cx - g->fx, cdy = g->cy - g->fy;
		float pdx = u - g->fx, pdy = v - g->fy, dr = g->r - g->fr;
		float a = cdx * cdx + cdy * cdy - dr * dr;
		float b = pdx * cdx + pdy * cdy + g->fr * dr;
		float cc = pdx * pdx + pdy * pdy - g->fr * g->fr;

		if (fabsf(a) < 1e-6f) {
			t = b != 0 ? cc / (2 * b) : 0;
		} else {
			float d = b * b - a * cc;

			t = d < 0 ? 0 : (b + sqrtf(d)) / a;
			if (a < 0 && d >= 0 && g->fr + t * dr < 0)
				t = (b - sqrtf(d)) / a;
		}
	}
	if (g->spread == 't')
		t -= floorf(t);
	else if (g->spread == 'r') {
		t = fmodf(fabsf(t), 2);
		if (t > 1)
			t = 2 - t;
	}
	if (!(t > 0))
		t = 0;
	if (t > 1)
		t = 1;
	return g->lut[(int)(t * 255 + 0.5f)];
}

/* Fill p (pixel coordinates of the canvas) with rgba, or with g (when
 * not NULL) and rgba's alpha on top. */
static void poly_fill(struct canvas *c, struct poly *p, int evenodd,
	const unsigned char *rgba, const struct grad *g)
{
	float ymin = 1e30f, ymax = -1e30f, *acc;
	struct xing *xs;
	int i, y, s, k, nx, r = c->bgr ? 2 : 0;

	if (!p->n || !rgba[3])
		return;
	for (i = 0; i < p->n; i++) {
		if (p->e[i].y0 < ymin)
			ymin = p->e[i].y0;
		if (p->e[i].y1 > ymax)
			ymax = p->e[i].y1;
	}
	if (ymin < (float)c->cy0)
		ymin = (float)c->cy0;
	if (ymax > (float)c->cy1)
		ymax = (float)c->cy1;
	acc = calloc((size_t)c->w + 2, sizeof(float));
	xs = xmalloc((size_t)p->n * sizeof(*xs));
	if (!acc)
		abort();
	for (y = ifloor(ymin); y < iceil(ymax); y++) {
		int any = 0;

		for (s = 0; s < 4; s++) {
			float sy = (float)y + ((float)s + 0.5f) / 4;
			int wind = 0;

			nx = 0;
			for (i = 0; i < p->n; i++) {
				struct edge *e = &p->e[i];

				if (sy < e->y0 || sy >= e->y1)
					continue;
				xs[nx].x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) /
					(e->y1 - e->y0);
				xs[nx].dir = e->dir;
				nx++;
			}
			qsort(xs, (size_t)nx, sizeof(*xs), xcmp);
			for (k = 0; k + 1 < nx + 1 && k < nx; k++) {
				float a, b;
				int in;

				wind += xs[k].dir;
				in = evenodd ? (k % 2 == 0) : wind != 0;
				if (!in || k + 1 >= nx)
					continue;
				a = xs[k].x;
				b = xs[k + 1].x;
				if (a < (float)c->cx0)
					a = (float)c->cx0;
				if (b > (float)c->cx1)
					b = (float)c->cx1;
				if (b <= a)
					continue;
				/* span [a, b) with fractional ends */
				{
					int ia = ifloor(a), ib = ifloor(b);

					if (ia == ib) {
						acc[ia] += (b - a) / 4;
					} else {
						int q;

						acc[ia] += ((float)ia + 1 - a) / 4;
						for (q = ia + 1; q < ib; q++)
							acc[q] += 0.25f;
						if (ib < c->w)
							acc[ib] += (b - (float)ib) / 4;
					}
					any = 1;
				}
			}
		}
		if (!any)
			continue;
		for (i = c->cx0; i < c->cx1; i++) {
			if (acc[i] > 0) {
				int cov = (int)(acc[i] * 255 + 0.5f);
				unsigned char *px;
				int a;

				if (cov > 255)
					cov = 255;
				if (y >= c->cy0 && y < c->cy1) {
					const unsigned char *col = g ?
						grad_at(g, i, y) : rgba;

					a = rgba[3] * cov / 255;
					if (g)
						a = a * col[3] / 255;
					a = (int)((float)a * c->alpha + 0.5f);
					if (c->mask)
						a = a * mask_at(c, i, y) / 255;
					px = c->px + (size_t)y * (size_t)c->stride +
						(size_t)i * 4;
					px[0] = (unsigned char)((col[r] * a +
						px[0] * (255 - a)) / 255);
					px[1] = (unsigned char)((col[1] * a +
						px[1] * (255 - a)) / 255);
					px[2] = (unsigned char)((col[2 - r] *
						a + px[2] * (255 - a)) / 255);
					px[3] = (unsigned char)(a + px[3] *
						(255 - a) / 255);
				}
			}
			acc[i] = 0;
		}
	}
	free(acc);
	free(xs);
}

void fill_polygon(struct canvas *c, const float *pts, int n,
	const unsigned char *rgba)
{
	struct poly p;
	int i;

	memset(&p, 0, sizeof(p));
	for (i = 0; i < n; i++) {
		int k = (i + 1) % n;

		poly_edge(&p, pts[2 * i] - c->ox, pts[2 * i + 1] - c->oy,
			pts[2 * k] - c->ox, pts[2 * k + 1] - c->oy);
	}
	poly_fill(c, &p, 0, rgba, NULL);
	free(p.e);
}

static void mul(struct mat *m, const struct mat *n)
{
	struct mat r;

	r.a = m->a * n->a + m->c * n->b;
	r.b = m->b * n->a + m->d * n->b;
	r.c = m->a * n->c + m->c * n->d;
	r.d = m->b * n->c + m->d * n->d;
	r.e = m->a * n->e + m->c * n->f + m->e;
	r.f = m->b * n->e + m->d * n->f + m->f;
	*m = r;
}

/* A number of SVG syntax; NAN if none. No strtod(): locales. */
static float num(const char **s)
{
	const char *p;
	double v = 0, f = 0.1, sign = 1;
	int digits = 0, e = 0, es = 1;

	while (**s == ' ' || **s == ',' || **s == '\t' || **s == '\n' ||
	       **s == '\r')
		(*s)++;
	p = *s;
	if (*p == '-' || *p == '+')
		sign = *p++ == '-' ? -1 : 1;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0'), digits++;
	if (*p == '.')
		for (p++; *p >= '0' && *p <= '9'; p++, f /= 10)
			v += (*p - '0') * f, digits++;
	if (!digits)
		return NAN;
	if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+' ||
	    (p[1] >= '0' && p[1] <= '9'))) {
		p++;
		if (*p == '-' || *p == '+')
			es = *p++ == '-' ? -1 : 1;
		while (*p >= '0' && *p <= '9')
			e = e * 10 + (*p++ - '0');
		v *= pow(10, es * (e > 300 ? 300 : e));
	}
	*s = p;
	return (float)(sign * v);
}

static float fnum(const char *s)
{
	float v = s ? num(&s) : NAN;

	return isnan(v) ? 0 : v;
}

/* A length attribute with its unit in px (user units); em, ex and ch
 * from the font size em */
static float ulen(const char *s, float em)
{
	float v = s ? num(&s) : NAN;
	static const struct {
		const char *u;
		float px;
	} units[] = { { "px", 1 }, { "pt", 96 / 72.f }, { "pc", 16 },
		{ "in", 96 }, { "cm", 96 / 2.54f }, { "mm", 96 / 25.4f },
		{ "q", 96 / 101.6f } };
	unsigned i;

	if (isnan(v))
		return 0;
	if (!strncmp(s, "em", 2) || !strncmp(s, "rem", 3))
		return v * em;
	if (!strncmp(s, "ex", 2) || !strncmp(s, "ch", 2))
		return v * em / 2;
	for (i = 0; i < sizeof(units) / sizeof(*units); i++)
		if (!strncmp(s, units[i].u, strlen(units[i].u)))
			return v * units[i].px;
	return v;
}

static void parse_transform(const char *s, struct mat *m)
{
	while (s && *s) {
		struct mat t = { 1, 0, 0, 1, 0, 0 };
		const char *p = strchr(s, '(');
		float v[6];
		int n = 0;

		if (!p)
			break;
		while (*s == ' ' || *s == ',')
			s++;
		{
			const char *q = p + 1;

			while (n < 6 && !isnan(v[n] = num(&q)))
				n++;
		}
		if (!strncmp(s, "matrix", 6) && n == 6) {
			t.a = v[0], t.b = v[1], t.c = v[2], t.d = v[3];
			t.e = v[4], t.f = v[5];
		} else if (!strncmp(s, "translate", 9) && n >= 1) {
			t.e = v[0];
			t.f = n > 1 ? v[1] : 0;
		} else if (!strncmp(s, "scale", 5) && n >= 1) {
			t.a = v[0];
			t.d = n > 1 ? v[1] : v[0];
		} else if (!strncmp(s, "rotate", 6) && n >= 1) {
			float r = v[0] * (float)M_PI / 180;
			struct mat rot = { cosf(r), sinf(r), -sinf(r), cosf(r),
				0, 0 };

			if (n == 3) {
				struct mat a = { 1, 0, 0, 1, v[1], v[2] };
				struct mat b = { 1, 0, 0, 1, -v[1], -v[2] };

				mul(&a, &rot);
				mul(&a, &b);
				rot = a;
			}
			t = rot;
		} else if (!strncmp(s, "skewX", 5) && n >= 1) {
			t.c = tanf(v[0] * (float)M_PI / 180);
		} else if (!strncmp(s, "skewY", 5) && n >= 1) {
			t.b = tanf(v[0] * (float)M_PI / 180);
		}
		mul(m, &t);
		s = strchr(p, ')');
		if (s)
			s++;
	}
}

static void tx(const struct mat *m, float x, float y, float *ox, float *oy)
{
	*ox = m->a * x + m->c * y + m->e;
	*oy = m->b * x + m->d * y + m->f;
}

/* Path building in user units, transformed as edges are added. */
struct pb {
	struct poly *p;
	struct poly *s;		/* the segments, for the stroke */
	const struct mat *m;
	float x, y, sx, sy;	/* user space */
	int open;
	float bb[4];		/* bounding box, user space: x0 y0 x1 y1 */
};

static void pb_box(struct pb *b, float x, float y)
{
	if (x < b->bb[0])
		b->bb[0] = x;
	if (y < b->bb[1])
		b->bb[1] = y;
	if (x > b->bb[2])
		b->bb[2] = x;
	if (y > b->bb[3])
		b->bb[3] = y;
}

static void pb_line(struct pb *b, float x, float y)
{
	float ax, ay, bx, by;

	tx(b->m, b->x, b->y, &ax, &ay);
	tx(b->m, x, y, &bx, &by);
	poly_edge(b->p, ax, ay, bx, by);
	seg_add(b->s, ax, ay, bx, by);
	pb_box(b, x, y);
	b->x = x;
	b->y = y;
	b->open = 1;
}

static void pb_close(struct pb *b)
{
	if (b->open && (b->x != b->sx || b->y != b->sy))
		pb_line(b, b->sx, b->sy);
	b->open = 0;
}

static void pb_move(struct pb *b, float x, float y)
{
	pb_close(b);
	b->x = b->sx = x;
	b->y = b->sy = y;
	pb_box(b, x, y);
}

/*
 * Curves are cut into lines: at least 16 a cubic and 16 a half turn of
 * an arc (what small drawings always had), more when the drawing is
 * large on screen, for 0.2 device px at most between a line and its
 * curve (a 380 px logo's arcs had flats).
 */
#define PB_TOL 0.2f
#define PB_MAX 1024

/* Device px a user unit, the larger of the two axes */
static float pb_scale(const struct mat *m)
{
	float sx = m->a * m->a + m->b * m->b, sy = m->c * m->c + m->d * m->d;

	return sqrtf(sx > sy ? sx : sy);
}

static void pb_cubic(struct pb *b, float x1, float y1, float x2, float y2,
	float x, float y)
{
	float x0 = b->x, y0 = b->y;
	/* Wang's bound: the larger second difference of the points */
	float ax = x0 - 2 * x1 + x2, ay = y0 - 2 * y1 + y2,
		bx = x1 - 2 * x2 + x, by = y1 - 2 * y2 + y;
	float l = sqrtf(ax * ax + ay * ay), l2 = sqrtf(bx * bx + by * by);
	float m = sqrtf(0.75f * (l > l2 ? l : l2) * pb_scale(b->m) / PB_TOL);
	int i, n = 16;

	if (m > n)
		n = m < PB_MAX ? iceil(m) : PB_MAX;

	for (i = 1; i <= n; i++) {
		float t = (float)i / (float)n, u = 1 - t;

		pb_line(b, u * u * u * x0 + 3 * u * u * t * x1 +
			3 * u * t * t * x2 + t * t * t * x,
			u * u * u * y0 + 3 * u * u * t * y1 +
			3 * u * t * t * y2 + t * t * t * y);
	}
}

static void pb_quad(struct pb *b, float x1, float y1, float x, float y)
{
	pb_cubic(b, b->x + 2.0f / 3 * (x1 - b->x), b->y + 2.0f / 3 * (y1 - b->y),
		x + 2.0f / 3 * (x1 - x), y + 2.0f / 3 * (y1 - y), x, y);
}

/* SVG arcs (F.6.5): endpoint to center parameterization. */
static void pb_arc(struct pb *b, float rx, float ry, float rot, int large,
	int sweep, float x, float y)
{
	float x0 = b->x, y0 = b->y, phi = rot * (float)M_PI / 180;
	float cp = cosf(phi), sp = sinf(phi), dx = (x0 - x) / 2,
		dy = (y0 - y) / 2, x1 = cp * dx + sp * dy,
		y1 = -sp * dx + cp * dy, lam, sq, cx1, cy1, cx, cy, t1, dt;
	int i, n;

	rx = fabsf(rx);
	ry = fabsf(ry);
	if (!rx || !ry) {
		pb_line(b, x, y);
		return;
	}
	lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
	if (lam > 1) {
		rx *= sqrtf(lam);
		ry *= sqrtf(lam);
	}
	sq = (rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1) /
		(rx * rx * y1 * y1 + ry * ry * x1 * x1);
	sq = sq < 0 ? 0 : sqrtf(sq);
	if (large == sweep)
		sq = -sq;
	cx1 = sq * rx * y1 / ry;
	cy1 = -sq * ry * x1 / rx;
	cx = cp * cx1 - sp * cy1 + (x0 + x) / 2;
	cy = sp * cx1 + cp * cy1 + (y0 + y) / 2;
	t1 = atan2f((y1 - cy1) / ry, (x1 - cx1) / rx);
	dt = atan2f((-y1 - cy1) / ry, (-x1 - cx1) / rx) - t1;
	if (sweep && dt < 0)
		dt += 2 * (float)M_PI;
	else if (!sweep && dt > 0)
		dt -= 2 * (float)M_PI;
	n = (int)(fabsf(dt) / (float)M_PI * 16) + 2;
	{
		/* the angle of a line 0.2 px from its arc at most */
		float r = (rx > ry ? rx : ry) * pb_scale(b->m), a;

		if (r > PB_TOL) {
			a = 2 * acosf(1 - PB_TOL / r);
			a = fabsf(dt) / a;
			if (a > n)
				n = a < PB_MAX ? iceil(a) : PB_MAX;
		}
	}
	for (i = 1; i <= n; i++) {
		float t = t1 + dt * (float)i / (float)n;
		float ex = rx * cosf(t), ey = ry * sinf(t);

		pb_line(b, cp * ex - sp * ey + cx, sp * ex + cp * ey + cy);
	}
}

/* An arc's flag: one character, 0 or 1, separators optional
 * ("a1 1 0 000 2" is flags 0 and 0, then 0 2) */
static float flag(const char **d)
{
	while (**d == ' ' || **d == ',' || **d == '\n' || **d == '\t' ||
	       **d == '\r')
		(*d)++;
	if (**d != '0' && **d != '1')
		return NAN;
	return (float)(*(*d)++ - '0');
}

static void parse_path(struct pb *b, const char *d)
{
	char cmd = 0, prev = 0;
	float cx2 = 0, cy2 = 0;	/* last control point */

	while (d && *d) {
		float v[7];
		int k, need, rel;

		while (*d == ' ' || *d == ',' || *d == '\n' || *d == '\t' ||
		       *d == '\r')
			d++;
		if (!*d)
			break;
		if (strchr("MmLlHhVvCcSsQqTtAaZz", *d))
			cmd = *d++;
		else if (!cmd)
			break;
		rel = cmd >= 'a';
		switch (cmd | 32) {
		case 'z':
			pb_close(b);
			b->x = b->sx;
			b->y = b->sy;
			prev = cmd;
			cmd = 0;
			continue;
		case 'h': case 'v': need = 1; break;
		case 'm': case 'l': case 't': need = 2; break;
		case 's': case 'q': need = 4; break;
		case 'c': need = 6; break;
		case 'a': need = 7; break;
		default: return;
		}
		for (k = 0; k < need; k++)
			if (isnan(v[k] = (cmd | 32) == 'a' && (k == 3 || k == 4) ?
			    flag(&d) : num(&d)))
				return;
		switch (cmd | 32) {
		case 'm':
			pb_move(b, v[0] + (rel ? b->x : 0),
				v[1] + (rel ? b->y : 0));
			cmd = rel ? 'l' : 'L';	/* implicit lineto */
			break;
		case 'l':
			pb_line(b, v[0] + (rel ? b->x : 0),
				v[1] + (rel ? b->y : 0));
			break;
		case 'h':
			pb_line(b, v[0] + (rel ? b->x : 0), b->y);
			break;
		case 'v':
			pb_line(b, b->x, v[0] + (rel ? b->y : 0));
			break;
		case 'c':
			if (rel)
				for (k = 0; k < 6; k += 2)
					v[k] += b->x, v[k + 1] += b->y;
			pb_cubic(b, v[0], v[1], v[2], v[3], v[4], v[5]);
			cx2 = v[2];
			cy2 = v[3];
			break;
		case 's':
			if (rel)
				for (k = 0; k < 4; k += 2)
					v[k] += b->x, v[k + 1] += b->y;
			if (strchr("CcSs", prev))
				pb_cubic(b, 2 * b->x - cx2, 2 * b->y - cy2,
					v[0], v[1], v[2], v[3]);
			else
				pb_cubic(b, b->x, b->y, v[0], v[1], v[2],
					v[3]);
			cx2 = v[0];
			cy2 = v[1];
			break;
		case 'q':
			if (rel)
				for (k = 0; k < 4; k += 2)
					v[k] += b->x, v[k + 1] += b->y;
			pb_quad(b, v[0], v[1], v[2], v[3]);
			cx2 = v[0];
			cy2 = v[1];
			break;
		case 't': {
			float qx = strchr("QqTt", prev) ? 2 * b->x - cx2 : b->x;
			float qy = strchr("QqTt", prev) ? 2 * b->y - cy2 : b->y;

			if (rel)
				v[0] += b->x, v[1] += b->y;
			pb_quad(b, qx, qy, v[0], v[1]);
			cx2 = qx;
			cy2 = qy;
			break;
		}
		case 'a':
			if (rel)
				v[5] += b->x, v[6] += b->y;
			pb_arc(b, v[0], v[1], v[2], v[3] != 0, v[4] != 0, v[5],
				v[6]);
			break;
		}
		prev = cmd;
	}
}

/* Presentation: attribute, or the style="" declaration. */
static const char *prop(const soie_node *n, const char *name, char *buf,
	size_t size)
{
	const char *st = soie_element_get_attr(n, "style"), *p;
	size_t len = strlen(name);

	for (p = st; p && (p = strstr(p, name)); p += len) {
		if ((p == st || p[-1] == ';' || p[-1] == ' ') &&
		    p[len] == ':') {
			size_t k = 0;

			p += len + 1;
			while (*p == ' ')
				p++;
			while (*p && *p != ';' && k < size - 1)
				buf[k++] = *p++;
			while (k && buf[k - 1] == ' ')
				k--;
			buf[k] = '\0';
			return buf;
		}
	}
	return soie_element_get_attr(n, name);
}

struct paintstate {
	unsigned char fill[4], stroke[4];
	int has_fill, has_stroke, evenodd;
	float sw, opacity;
	char join, cap;		/* 'm'iter, 'r'ound, 'b'evel; 'b'utt, 'r'ound,
				 * 's'quare */
	float miter;		/* stroke-miterlimit */
	float vw, vh;		/* the viewport, for percentages */
	unsigned char current[4];	/* currentColor */
	unsigned char fa, sa;	/* the paints' own alpha, styled elements */
	const soie_node *fill_g, *stroke_g;	/* gradients, else NULL */
	/* text */
	const struct style *base;	/* what the font props change */
	float fsize;		/* font-size, user units */
	const char *family;	/* interned; NULL: base's */
	int weight;
	char anchor;		/* text-anchor: 's'tart, 'm'iddle, 'e'nd */
	int nest;		/* groups and svgs entered, up to SVG_NEST */
};

static const soie_node *by_id(const soie_node *n, const char *id);
static const char *use_href(const soie_node *n);

static const soie_node *root_of(const soie_node *n)
{
	while (n->parent)
		n = n->parent;
	return n;
}

static int is_grad(const soie_node *n)
{
	return n && n->type == SOIE_ELEMENT && n->ns == SOIE_NS_SVG &&
		(!strcmp(n->name, "linearGradient") ||
		 !strcmp(n->name, "radialGradient"));
}

/*
 * A font-family value kept for as long as the library is loaded, the
 * same pointer for the same value (fonts cache their chains by it),
 * single quotes made double ones as computed values have them.
 */
static const char *intern_family(const char *v)
{
	static char *names[64];
	static int n;
	char buf[128], *q;
	int i;

	if (strlen(v) >= sizeof(buf))
		return NULL;
	strcpy(buf, v);
	for (q = buf; *q; q++)
		if (*q == '\'')
			*q = '"';
	for (i = 0; i < n; i++)
		if (!strcmp(names[i], buf))
			return names[i];
	if (n == 64)
		return NULL;
	names[n] = xmalloc(strlen(buf) + 1);
	strcpy(names[n], buf);
	return names[n++];
}

static int svg_color(const char *v, const unsigned char *current,
	unsigned char *out)
{
	struct cvlist l;
	struct cur c;
	struct color col;
	int ok = 0;

	if (!v || !strcmp(v, "none"))
		return 0;
	css_component_values(v, strlen(v), &l);
	cur_init(&c, &l);
	if (parse_color(&c, &col)) {
		if (col.current)
			memcpy(out, current, 4);
		else
			color_to_rgba(&col, out);
		ok = 1;
	}
	cvlist_free(&l);
	return ok;
}

/*
 * A fill or stroke of n: a color, or url(#id) with an optional fallback
 * color, *g the gradient it names. Patterns are a neutral gray stand-in;
 * a reference to nothing is its fallback, else none.
 */
static int svg_paint(const soie_node *n, const char *v,
	const unsigned char *current, unsigned char *out,
	const soie_node **g)
{
	*g = NULL;
	if (v && !strncmp(v, "url(", 4)) {
		const char *e = strchr(v, ')'), *id = v + 4;
		const soie_node *t = NULL;
		char buf[128];
		size_t len;

		if (!e)
			return 0;
		while (*id == ' ' || *id == '"' || *id == '\'')
			id++;
		len = (size_t)(e - id);
		while (len && (id[len - 1] == ' ' || id[len - 1] == '"' ||
		       id[len - 1] == '\''))
			len--;
		if (*id == '#' && len > 1 && len < sizeof(buf)) {
			memcpy(buf, id + 1, len - 1);
			buf[len - 1] = '\0';
			t = by_id(root_of(n), buf);
		}
		if (is_grad(t)) {
			*g = t;
			out[0] = out[1] = out[2] = 0;
			out[3] = 255;
			return 1;
		}
		if (t && !strcmp(t->name, "pattern")) {
			out[0] = out[1] = out[2] = 128;
			out[3] = 255;
			return 1;
		}
		for (v = e + 1; *v == ' '; v++)
			;
		return *v ? svg_color(v, current, out) : 0;
	}
	return svg_color(v, current, out);
}

/* The value of prop is n's own: not what its parent has. An element
 * drawn through <use> takes what it does not set from the <use>, not
 * from where it is written: the paint state carries those. */
static int own(const soie_node *n, int prop)
{
	const struct style *p = n->parent && n->parent->type == SOIE_ELEMENT ?
		n->parent->u.el.style : NULL;
	const struct cval *a = &n->u.el.style->v[prop], *b;

	if (!p)
		return 1;
	b = &p->v[prop];
	if (a->kw != b->kw)
		return 1;
	if (a->kw)
		return 0;
	switch (props[prop].kind) {
	case K_RAW:
	case K_FAMILY:
		return a->u.str != b->u.str && (!a->u.str || !b->u.str ||
			strcmp(a->u.str, b->u.str));
	case K_COLOR:
		return a->u.color != b->u.color && memcmp(a->u.color,
			b->u.color, sizeof(*a->u.color));
	case K_FSIZE:
		return a->u.len.px != b->u.len.px;
	default:
		return a->u.num != b->u.num;
	}
}

static int is_current(const char *v)
{
	return v && ascii_ieq(v, "currentcolor");
}

/* The paint state of an element styled by the cascade (an SVG in a
 * page): its computed values; all of them for the outer <svg>. */
static void styled_paint(const soie_node *n, struct paintstate *ps, int all)
{
	const struct style *st = n->u.el.style;
	const char *v;
	int color = all || own(n, P_COLOR), fo, so;

	if (color)
		color_to_rgba(st->v[P_COLOR].u.color, ps->current);
	v = st->v[P_FILL].u.str;
	fo = all || own(n, P_FILL_OPACITY);
	if (all || own(n, P_FILL) || (color && is_current(v))) {
		ps->has_fill = svg_paint(n, v, ps->current, ps->fill,
			&ps->fill_g);
		ps->fa = ps->fill[3];
		fo = 1;
	}
	if (fo)
		ps->fill[3] = (unsigned char)(ps->fa *
			st->v[P_FILL_OPACITY].u.num + 0.5);
	v = st->v[P_STROKE].u.str;
	so = all || own(n, P_STROKE_OPACITY);
	if (all || own(n, P_STROKE) || (color && is_current(v))) {
		ps->has_stroke = svg_paint(n, v, ps->current, ps->stroke,
			&ps->stroke_g);
		ps->sa = ps->stroke[3];
		so = 1;
	}
	if (so)
		ps->stroke[3] = (unsigned char)(ps->sa *
			st->v[P_STROKE_OPACITY].u.num + 0.5);
	if ((all || own(n, P_STROKE_WIDTH)) &&
	    (v = st->v[P_STROKE_WIDTH].u.str)) {
		float w = fnum(v);

		ps->sw = strstr(v, "em") ? w * st->v[P_FONT_SIZE].u.len.px /
			doc_zoom() : strchr(v, '%') ? w * (ps->vw + ps->vh) /
			200 : w;
	}
	if (all || own(n, P_STROKE_LINEJOIN))
		ps->join = kw(st, P_STROKE_LINEJOIN, "round") ? 'r' :
			kw(st, P_STROKE_LINEJOIN, "bevel") ? 'b' : 'm';
	if (all || own(n, P_STROKE_LINECAP))
		ps->cap = kw(st, P_STROKE_LINECAP, "round") ? 'r' :
			kw(st, P_STROKE_LINECAP, "square") ? 's' : 'b';
	if ((all || own(n, P_STROKE_MITERLIMIT)) &&
	    st->v[P_STROKE_MITERLIMIT].u.num >= 1)
		ps->miter = (float)st->v[P_STROKE_MITERLIMIT].u.num;
	if (all || own(n, P_FILL_RULE))
		ps->evenodd = kw(st, P_FILL_RULE, "evenodd");
	if (!all)	/* the outer one's is the canvas' alpha already */
		ps->opacity *= (float)st->v[P_OPACITY].u.num;
	if (!all && own(n, P_FONT_SIZE))
		ps->fsize = st->v[P_FONT_SIZE].u.len.px / doc_zoom();
	if (!all && own(n, P_FONT_FAMILY))
		ps->family = st->v[P_FONT_FAMILY].u.str;
	if (!all && own(n, P_FONT_WEIGHT))
		ps->weight = (int)st->v[P_FONT_WEIGHT].u.num;
	if (all || own(n, P_TEXT_ANCHOR))
		ps->anchor = kw(st, P_TEXT_ANCHOR, "middle") ? 'm' :
			kw(st, P_TEXT_ANCHOR, "end") ? 'e' : 's';
}

static void inherit_paint(const soie_node *n, struct paintstate *ps)
{
	char buf[128];
	const char *v;

	if (n->u.el.style) {
		styled_paint(n, ps, 0);
		return;
	}
	if ((v = prop(n, "fill", buf, sizeof(buf))))
		ps->has_fill = svg_paint(n, v, ps->current, ps->fill,
			&ps->fill_g);
	if ((v = prop(n, "stroke", buf, sizeof(buf))))
		ps->has_stroke = svg_paint(n, v, ps->current, ps->stroke,
			&ps->stroke_g);
	if ((v = prop(n, "stroke-width", buf, sizeof(buf))))
		ps->sw = fnum(v);
	if ((v = prop(n, "stroke-linejoin", buf, sizeof(buf))))
		ps->join = !strcmp(v, "round") ? 'r' : !strcmp(v, "bevel") ?
			'b' : 'm';
	if ((v = prop(n, "stroke-linecap", buf, sizeof(buf))))
		ps->cap = !strcmp(v, "round") ? 'r' : !strcmp(v, "square") ?
			's' : 'b';
	if ((v = prop(n, "stroke-miterlimit", buf, sizeof(buf))) &&
	    fnum(v) >= 1)
		ps->miter = fnum(v);
	if ((v = prop(n, "fill-rule", buf, sizeof(buf))))
		ps->evenodd = !strcmp(v, "evenodd");
	if ((v = prop(n, "opacity", buf, sizeof(buf))))
		ps->opacity *= fnum(v);
	if ((v = prop(n, "fill-opacity", buf, sizeof(buf))))
		ps->fill[3] = (unsigned char)(ps->fill[3] * fnum(v));
	if ((v = prop(n, "stroke-opacity", buf, sizeof(buf))))
		ps->stroke[3] = (unsigned char)(ps->stroke[3] * fnum(v));
	if ((v = prop(n, "font-size", buf, sizeof(buf))) && fnum(v) > 0)
		ps->fsize = strstr(v, "em") ? fnum(v) * ps->fsize :
			strchr(v, '%') ? fnum(v) * ps->fsize / 100 : fnum(v);
	if ((v = prop(n, "font-family", buf, sizeof(buf))))
		ps->family = intern_family(v);
	if ((v = prop(n, "font-weight", buf, sizeof(buf))))
		ps->weight = !strcmp(v, "bold") || !strcmp(v, "bolder") ? 700 :
			!strcmp(v, "normal") || !strcmp(v, "lighter") ? 400 :
			fnum(v) >= 1 ? (int)fnum(v) : ps->weight;
	if ((v = prop(n, "text-anchor", buf, sizeof(buf))))
		ps->anchor = !strcmp(v, "middle") ? 'm' : !strcmp(v, "end") ?
			'e' : 's';
}

static float attrf(const soie_node *n, const char *name, float dflt)
{
	const char *a = soie_element_get_attr(n, name);

	return a ? fnum(a) : dflt;
}

/* Gradients */

#define GRAD_DEPTH 8	/* href chain: no further (and no loop) */

/* The gradient n's href names, NULL if none */
static const soie_node *grad_href(const soie_node *n)
{
	const char *a = use_href(n);
	const soie_node *t;

	if (!a || *a != '#')
		return NULL;
	t = by_id(root_of(n), a + 1);
	return is_grad(t) && t != n ? t : NULL;
}

/* An attribute of gradient n or else of those it refers to */
static const char *grad_attr(const soie_node *n, const char *name)
{
	const char *a = NULL;
	int d;

	for (d = 0; n && d < GRAD_DEPTH; d++, n = grad_href(n))
		if ((a = soie_element_get_attr(n, name)))
			break;
	return a;
}

/* A length of gradient n: dflt when not given, percentages of ref
 * (in bounding box units, ref is 1 and plain numbers are fractions) */
static float grad_len(const soie_node *n, const char *name, float dflt,
	float ref)
{
	const char *a = grad_attr(n, name);

	if (!a)
		return dflt * ref;
	return strchr(a, '%') ? fnum(a) * ref / 100 : fnum(a);
}

static void lut_put(unsigned char *o, const float *pm)
{
	int i;

	o[3] = (unsigned char)(pm[3] + 0.5f);
	for (i = 0; i < 3; i++)
		o[i] = (unsigned char)(pm[3] > 0 ? pm[i] * 255 / pm[3] +
			0.5f : 0);
}

/* g's color table from the stops of n (or of the first gradient it
 * refers to that has some); 0 for none (the paint is then none) */
static int grad_stops(struct grad *g, const soie_node *n,
	const struct paintstate *ps)
{
	float off[64], col[64][4];
	const soie_node *k;
	int ns = 0, d, i, j;
	char buf[128];

	for (d = 0; n && d < GRAD_DEPTH && !ns; d++, n = grad_href(n)) {
		for (k = n->first; k && ns < 64; k = k->next) {
			unsigned char rgba[4] = { 0, 0, 0, 255 };
			const char *v;
			float o, a = 1;

			if (k->type != SOIE_ELEMENT || strcmp(k->name, "stop"))
				continue;
			v = soie_element_get_attr(k, "offset");
			o = v ? fnum(v) / (strchr(v, '%') ? 100 : 1) : 0;
			if (o < 0)
				o = 0;
			if (o > 1)
				o = 1;
			if (ns && o < off[ns - 1])
				o = off[ns - 1];
			if ((v = prop(k, "stop-color", buf, sizeof(buf))))
				svg_color(v, ps->current, rgba);
			if ((v = prop(k, "stop-opacity", buf, sizeof(buf))))
				a = fnum(v);
			if (a < 0)
				a = 0;
			if (a > 1)
				a = 1;
			off[ns] = o;
			/* premultiplied, interpolated so */
			col[ns][3] = rgba[3] * a;
			for (i = 0; i < 3; i++)
				col[ns][i] = rgba[i] * col[ns][3] / 255;
			ns++;
		}
	}
	if (!ns)
		return 0;
	for (i = 0, j = 0; i < 256; i++) {
		float t = (float)i / 255, pm[4], f;
		int q;

		while (j < ns && off[j] <= t)
			j++;
		if (j == 0 || j == ns) {
			lut_put(g->lut[i], col[j ? ns - 1 : 0]);
			continue;
		}
		f = off[j] > off[j - 1] ? (t - off[j - 1]) /
			(off[j] - off[j - 1]) : 1;
		for (q = 0; q < 4; q++)
			pm[q] = col[j - 1][q] + (col[j][q] - col[j - 1][q]) * f;
		lut_put(g->lut[i], pm);
	}
	return 1;
}

/*
 * Gradient n ready to paint a shape of bounding box bb (user space,
 * m from it to pixels); 0 when it paints nothing (no stops, empty box
 * in bounding box units, a transform that cannot be inverted).
 */
static int grad_make(struct grad *g, const soie_node *n, const struct mat *m,
	const float *bb, const struct paintstate *ps)
{
	const char *u = grad_attr(n, "gradientUnits"), *a;
	int obb = !u || strcmp(u, "userSpaceOnUse");
	float rw = obb ? 1 : ps->vw, rh = obb ? 1 : ps->vh;
	float rd = obb ? 1 : sqrtf((ps->vw * ps->vw + ps->vh * ps->vh) / 2);
	struct mat t = *m;
	float det;

	if (!grad_stops(g, n, ps))
		return 0;
	if (obb) {
		struct mat b = { bb[2] - bb[0], 0, 0, bb[3] - bb[1], bb[0],
			bb[1] };

		if (b.a <= 0 || b.d <= 0)
			return 0;
		mul(&t, &b);
	}
	if ((a = grad_attr(n, "gradientTransform")))
		parse_transform(a, &t);
	det = t.a * t.d - t.b * t.c;
	if (!isfinite(det) || fabsf(det) < 1e-12f)
		return 0;
	g->inv.a = t.d / det;
	g->inv.b = -t.b / det;
	g->inv.c = -t.c / det;
	g->inv.d = t.a / det;
	g->inv.e = (t.c * t.f - t.d * t.e) / det;
	g->inv.f = (t.b * t.e - t.a * t.f) / det;
	a = grad_attr(n, "spreadMethod");
	g->spread = a && !strcmp(a, "reflect") ? 'r' :
		a && !strcmp(a, "repeat") ? 't' : 'p';
	g->radial = !strcmp(n->name, "radialGradient");
	if (!g->radial) {
		g->x1 = grad_len(n, "x1", 0, rw);
		g->y1 = grad_len(n, "y1", 0, rh);
		g->x2 = grad_len(n, "x2", 1, rw);
		g->y2 = grad_len(n, "y2", 0, rh);
	} else {
		float dx, dy, l;

		g->cx = grad_len(n, "cx", 0.5f, rw);
		g->cy = grad_len(n, "cy", 0.5f, rh);
		g->r = grad_len(n, "r", 0.5f, rd);
		g->fr = grad_len(n, "fr", 0, rd);
		g->fx = grad_attr(n, "fx") ? grad_len(n, "fx", 0, rw) : g->cx;
		g->fy = grad_attr(n, "fy") ? grad_len(n, "fy", 0, rh) : g->cy;
		if (g->r <= 0)
			return 0;
		/* a focal point out of the circle comes back onto it */
		dx = g->fx - g->cx;
		dy = g->fy - g->cy;
		l = sqrtf(dx * dx + dy * dy);
		if (l > g->r * 0.999f) {
			g->fx = g->cx + dx * g->r * 0.999f / l;
			g->fy = g->cy + dy * g->r * 0.999f / l;
		}
	}
	return 1;
}

/*
 * Stroke: each segment of the outline a quad, its joints and the ends
 * of open subpaths their own convex pieces, all filled at once
 * (nonzero: the pieces turned the same way merge, no double blending).
 */

/* Convex polygon v (n points) into p, turned as the quads are */
static void piece(struct poly *p, const float *v, int n)
{
	float a = 0;
	int i, j;

	for (i = 0; i < n; i++) {
		j = (i + 1) % n;
		a += v[2 * i] * v[2 * j + 1] - v[2 * j] * v[2 * i + 1];
	}
	for (i = 0; i < n; i++) {
		j = (i + 1) % n;
		if (a <= 0)
			poly_edge(p, v[2 * i], v[2 * i + 1], v[2 * j],
				v[2 * j + 1]);
		else
			poly_edge(p, v[2 * j], v[2 * j + 1], v[2 * i],
				v[2 * i + 1]);
	}
}

static void disc(struct poly *p, float x, float y, float r)
{
	float v[32];
	int i;

	for (i = 0; i < 16; i++) {
		v[2 * i] = x + r * cosf((float)i * (float)M_PI / 8);
		v[2 * i + 1] = y + r * sinf((float)i * (float)M_PI / 8);
	}
	piece(p, v, 16);
}

/* e's direction, unit */
static void unit(const struct edge *e, float *ux, float *uy)
{
	float dx = e->x1 - e->x0, dy = e->y1 - e->y0;
	float l = sqrtf(dx * dx + dy * dy);

	*ux = dx / l;
	*uy = dy / l;
}

/* The joint of a and b at a's end; h is half the width */
static void join(struct poly *p, const struct edge *a, const struct edge *b,
	float h, const struct paintstate *ps)
{
	float x = a->x1, y = a->y1, ux, uy, vx, vy, cross, s, v[8];

	unit(a, &ux, &uy);
	unit(b, &vx, &vy);
	cross = ux * vy - uy * vx;
	if (fabsf(cross) < 1e-6f && ux * vx + uy * vy > 0)
		return;		/* straight on */
	if (ps->join == 'r') {
		disc(p, x, y, h);
		return;
	}
	/* the outer side: offset points A (a's) and B (b's) */
	s = cross > 0 ? -h : h;
	v[0] = x;
	v[1] = y;
	v[2] = x - uy * s;
	v[3] = y + ux * s;
	v[6] = x - vy * s;
	v[7] = y + vx * s;
	if (ps->join == 'm' && fabsf(cross) > 1e-6f) {
		/* the miter point: A + t a, on B's line too */
		float t = ((v[6] - v[2]) * vy - (v[7] - v[3]) * vx) / cross;

		v[4] = v[2] + t * ux;
		v[5] = v[3] + t * uy;
		if (hypotf(v[4] - x, v[5] - y) <= ps->miter * h) {
			piece(p, v, 4);
			return;
		}
	}
	v[4] = v[6];
	v[5] = v[7];
	piece(p, v, 3);
}

/* The cap of e at its start (end 0) or its end */
static void cap(struct poly *p, const struct edge *e, int end, float h,
	const struct paintstate *ps)
{
	float x = end ? e->x1 : e->x0, y = end ? e->y1 : e->y0, ux, uy, v[8];

	if (ps->cap == 'r') {
		disc(p, x, y, h);
		return;
	}
	if (ps->cap != 's')
		return;
	unit(e, &ux, &uy);
	if (!end) {
		ux = -ux;
		uy = -uy;
	}
	v[0] = x - uy * h;
	v[1] = y + ux * h;
	v[2] = v[0] + ux * h;
	v[3] = v[1] + uy * h;
	v[6] = x + uy * h;
	v[7] = y - ux * h;
	v[4] = v[6] + ux * h;
	v[5] = v[7] + uy * h;
	piece(p, v, 4);
}

static void stroke_poly(struct canvas *c, const struct poly *segs, float w,
	const struct paintstate *ps, const unsigned char *rgba,
	const struct grad *g)
{
	struct poly p;
	float h = w / 2;
	int i, first = 0;

	memset(&p, 0, sizeof(p));
	for (i = 0; i < segs->n; i++) {
		const struct edge *e = &segs->e[i], *f = &segs->e[first];
		const struct edge *next = i + 1 < segs->n ? e + 1 : NULL;
		float ux, uy, v[8];

		unit(e, &ux, &uy);
		v[0] = e->x0 - uy * h;
		v[1] = e->y0 + ux * h;
		v[2] = e->x1 - uy * h;
		v[3] = e->y1 + ux * h;
		v[4] = e->x1 + uy * h;
		v[5] = e->y1 - ux * h;
		v[6] = e->x0 + uy * h;
		v[7] = e->y0 - ux * h;
		piece(&p, v, 4);
		/* the subpath goes on, comes back to its start, or ends */
		if (next && next->x0 == e->x1 && next->y0 == e->y1) {
			join(&p, e, next, h, ps);
			continue;
		}
		if (i > first && f->x0 == e->x1 && f->y0 == e->y1) {
			join(&p, e, f, h, ps);
		} else {
			cap(&p, f, 0, h, ps);
			cap(&p, e, 1, h, ps);
		}
		first = i + 1;
	}
	poly_fill(c, &p, 0, rgba, g);
	free(p.e);
}

/* Gradient n for a shape, or NULL when it paints nothing; to free */
static struct grad *grad(const soie_node *n, const struct mat *m,
	const float *bb, const struct paintstate *ps)
{
	struct grad *g = xmalloc(sizeof(*g));

	if (bb[0] > bb[2] || !grad_make(g, n, m, bb, ps)) {
		free(g);
		return NULL;
	}
	return g;
}

static void shape(struct canvas *c, const soie_node *n, const struct mat *m,
	struct paintstate ps)
{
	struct poly p, segs;
	struct pb b;
	const char *t = n->name, *a;
	float x, y, w, h, r, rx, ry;
	unsigned char col[4];
	struct grad *g = NULL;

	memset(&p, 0, sizeof(p));
	memset(&segs, 0, sizeof(segs));
	memset(&b, 0, sizeof(b));
	b.p = &p;
	b.s = &segs;
	b.m = m;
	b.bb[0] = b.bb[1] = 1e30f;
	b.bb[2] = b.bb[3] = -1e30f;
	if (!strcmp(t, "rect")) {
		x = attrf(n, "x", 0);
		y = attrf(n, "y", 0);
		w = attrf(n, "width", 0);
		h = attrf(n, "height", 0);
		rx = attrf(n, "rx", -1);
		ry = attrf(n, "ry", rx);
		if (rx < 0)
			rx = ry < 0 ? 0 : ry;
		if (ry < 0)
			ry = rx;
		if (rx > w / 2)
			rx = w / 2;
		if (ry > h / 2)
			ry = h / 2;
		pb_move(&b, x + rx, y);
		pb_line(&b, x + w - rx, y);
		if (rx)
			pb_arc(&b, rx, ry, 0, 0, 1, x + w, y + ry);
		pb_line(&b, x + w, y + h - ry);
		if (rx)
			pb_arc(&b, rx, ry, 0, 0, 1, x + w - rx, y + h);
		pb_line(&b, x + rx, y + h);
		if (rx)
			pb_arc(&b, rx, ry, 0, 0, 1, x, y + h - ry);
		pb_line(&b, x, y + ry);
		if (rx)
			pb_arc(&b, rx, ry, 0, 0, 1, x + rx, y);
		pb_close(&b);
	} else if (!strcmp(t, "circle") || !strcmp(t, "ellipse")) {
		x = attrf(n, "cx", 0);
		y = attrf(n, "cy", 0);
		r = attrf(n, "r", 0);
		rx = !strcmp(t, "circle") ? r : attrf(n, "rx", 0);
		ry = !strcmp(t, "circle") ? r : attrf(n, "ry", 0);
		pb_move(&b, x + rx, y);
		pb_arc(&b, rx, ry, 0, 0, 1, x - rx, y);
		pb_arc(&b, rx, ry, 0, 0, 1, x + rx, y);
		pb_close(&b);
	} else if (!strcmp(t, "line")) {
		pb_move(&b, attrf(n, "x1", 0), attrf(n, "y1", 0));
		pb_line(&b, attrf(n, "x2", 0), attrf(n, "y2", 0));
		ps.has_fill = 0;
	} else if (!strcmp(t, "polyline") || !strcmp(t, "polygon")) {
		const char *s = soie_element_get_attr(n, "points");
		int first = 1;

		while (s) {
			x = num(&s);
			y = num(&s);
			if (isnan(x) || isnan(y))
				break;
			if (first)
				pb_move(&b, x, y);
			else
				pb_line(&b, x, y);
			first = 0;
		}
		if (!strcmp(t, "polygon"))
			pb_close(&b);
	} else if (!strcmp(t, "path") && (a = soie_element_get_attr(n, "d"))) {
		parse_path(&b, a);
	}
	if (ps.has_fill) {
		/* fill closes open subpaths */
		struct poly f = p;

		if (b.open && (b.x != b.sx || b.y != b.sy)) {
			float ax, ay, bx, by;

			tx(m, b.x, b.y, &ax, &ay);
			tx(m, b.sx, b.sy, &bx, &by);
			poly_edge(&p, ax, ay, bx, by);
			f = p;
		}
		memcpy(col, ps.fill, 4);
		col[3] = (unsigned char)(col[3] * ps.opacity);
		if (!ps.fill_g)
			poly_fill(c, &f, ps.evenodd, col, NULL);
		else if ((g = grad(ps.fill_g, m, b.bb, &ps)))
			poly_fill(c, &f, ps.evenodd, col, g);
		free(g);
		g = NULL;
	}
	if (ps.has_stroke && ps.sw > 0) {
		float scale = sqrtf(fabsf(m->a * m->d - m->b * m->c));

		memcpy(col, ps.stroke, 4);
		col[3] = (unsigned char)(col[3] * ps.opacity);
		if (!ps.stroke_g)
			stroke_poly(c, &segs, ps.sw * scale, &ps, col, NULL);
		else if ((g = grad(ps.stroke_g, m, b.bb, &ps)))
			stroke_poly(c, &segs, ps.sw * scale, &ps, col, g);
		free(g);
	}
	free(p.e);
	free(segs.e);
}

#define USE_DEPTH 8	/* <use> in <use>: no further (and no loop) */

/* A length attribute of a viewport, its percentages of ref */
static float vlen(const soie_node *n, const char *name, float ref,
	float dflt)
{
	const char *a = soie_element_get_attr(n, name);

	if (!a)
		return dflt;
	return strchr(a, '%') ? fnum(a) * ref / 100 : fnum(a);
}

/*
 * The transform of a viewport (x, y, w, h) in m's user units for el's
 * viewBox and preserveAspectRatio, onto m; the viewport size for the
 * percentages inside in *ps.
 */
static void viewport(struct mat *m, const soie_node *el, float x, float y,
	float w, float h, struct paintstate *ps)
{
	const char *vb = soie_element_get_attr(el, "viewBox");
	const char *par = soie_element_get_attr(el, "preserveAspectRatio");
	struct mat t = { 1, 0, 0, 1, 0, 0 };
	float v[4];
	int i;

	t.e = x;
	t.f = y;
	ps->vw = w;
	ps->vh = h;
	if (vb) {
		const char *s = vb;

		for (i = 0; i < 4; i++)
			v[i] = num(&s);
		if (!isnan(v[3]) && v[2] > 0 && v[3] > 0) {
			float sx = w / v[2], sy = h / v[3];

			if (!par || strcmp(par, "none")) {
				float s1 = sx < sy ? sx : sy;

				if (par && strstr(par, "slice"))
					s1 = sx > sy ? sx : sy;
				t.e += (w - v[2] * s1) / 2;
				t.f += (h - v[3] * s1) / 2;
				sx = sy = s1;
			}
			t.a = sx;
			t.d = sy;
			t.e -= v[0] * sx;
			t.f -= v[1] * sy;
			ps->vw = v[2];
			ps->vh = v[3];
		}
	}
	mul(m, &t);
}

static const soie_node *by_id(const soie_node *root, const char *id)
{
	const soie_node *k = root->first;
	const char *a;

	/* in tree order, without recursion: documents can be deep */
	while (k) {
		if (k->type == SOIE_ELEMENT) {
			if ((a = soie_element_get_attr(k, "id")) &&
			    !strcmp(a, id))
				return k;
			if (k->first) {
				k = k->first;
				continue;
			}
		}
		while (!k->next && k->parent != root)
			k = k->parent;
		k = k->next;
	}
	return NULL;
}

static void walk(struct canvas *c, const soie_node *n, struct mat m,
	struct paintstate ps, int depth);

/* <use>'s href, else its xlink:href (SVG 1.1, the namespaced one) */
static const char *use_href(const soie_node *n)
{
	const char *a = soie_element_get_attr(n, "href");
	size_t i;

	for (i = 0; !a && i < soie_element_attr_count(n); i++)
		if (soie_element_attr_ns(n, i) == SOIE_NS_XLINK &&
		    !strcmp(soie_element_attr_name(n, i), "href"))
			a = soie_element_attr_value(n, i);
	return a;
}

/*
 * A nested <svg> or a used <symbol>: its viewport (x, y, w, h) in m's
 * units, its content clipped to it when m keeps rectangles.
 */
static void nested(struct canvas *c, const soie_node *el, struct mat m,
	struct paintstate ps, float x, float y, float w, float h, int depth)
{
	int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;

	if (w <= 0 || h <= 0)
		return;
	if (m.b == 0 && m.c == 0) {
		float ax, ay, bx, by;
		int i0, j0, i1, j1;

		tx(&m, x, y, &ax, &ay);
		tx(&m, x + w, y + h, &bx, &by);
		i0 = ifloor(ax < bx ? ax : bx);
		j0 = ifloor(ay < by ? ay : by);
		i1 = iceil(ax < bx ? bx : ax);
		j1 = iceil(ay < by ? by : ay);
		if (i0 > c->cx0)
			c->cx0 = i0;
		if (j0 > c->cy0)
			c->cy0 = j0;
		if (i1 < c->cx1)
			c->cx1 = i1;
		if (j1 < c->cy1)
			c->cy1 = j1;
	}
	viewport(&m, el, x, y, w, h, &ps);
	walk(c, el, m, ps, depth);
	c->cx0 = cx0;
	c->cy0 = cy0;
	c->cx1 = cx1;
	c->cy1 = cy1;
}

/* Text */

/*
 * A style for text of ps at font size px (page pixels), color rgba:
 * ps's base with its font and color changed. Kept in a small ring, the
 * same style for the same values: text widths are cached by style.
 */
static const struct style *text_style(const struct paintstate *ps, float px,
	const unsigned char *rgba)
{
	static struct {
		struct style st;
		struct color col;
		const struct style *base;
		const char *family;
		float px;
		int weight;
		unsigned char rgba[4];
	} *ring;
	static int next, used;
	const char *fam = ps->family ? ps->family :
		ps->base->v[P_FONT_FAMILY].u.str;
	int i;

	if (!ring)
		ring = xmalloc(32 * sizeof(*ring));
	for (i = 0; i < used; i++)
		if (ring[i].base == ps->base && ring[i].family == fam &&
		    ring[i].px == px && ring[i].weight == ps->weight &&
		    !memcmp(ring[i].rgba, rgba, 4))
			return &ring[i].st;
	i = next;
	next = (next + 1) % 32;
	if (used < 32)
		used++;
	else
		font_cache_clear();	/* widths cached for the old one */
	ring[i].st = *ps->base;
	ring[i].base = ps->base;
	ring[i].family = fam;
	ring[i].px = px;
	ring[i].weight = ps->weight;
	memcpy(ring[i].rgba, rgba, 4);
	memset(&ring[i].col, 0, sizeof(ring[i].col));
	ring[i].col.space = CS_RGB_LEGACY;
	ring[i].col.c[0] = rgba[0];
	ring[i].col.c[1] = rgba[1];
	ring[i].col.c[2] = rgba[2];
	ring[i].col.alpha = rgba[3] / 255.0;
	ring[i].st.v[P_FONT_SIZE].u.len.px = px;
	ring[i].st.v[P_FONT_FAMILY].u.str = fam;
	ring[i].st.v[P_FONT_WEIGHT].u.num = ps->weight;
	ring[i].st.v[P_COLOR].kw = 0;
	ring[i].st.v[P_COLOR].u.color = &ring[i].col;
	return &ring[i].st;
}

/* A run of text at (x, y), user units; x set: a new chunk starts */
struct trun {
	char *s;
	size_t len;
	float x, y, w;		/* w: advance, user units */
	int chunk;
	struct paintstate ps;
};

struct tstate {
	struct trun *r;
	int n, cap;
	float x, y;		/* the current text position */
	int space;		/* the last character was a space */
	float scale;
};

static float coord(const soie_node *n, const char *name, float ref,
	int *set)
{
	const char *a = soie_element_get_attr(n, name), *s = a;
	float v;

	*set = 0;
	if (!a)
		return 0;
	v = num(&s);
	if (isnan(v))
		return 0;
	*set = 1;
	return *s == '%' ? v * ref / 100 : v;
}

static void text_runs(struct tstate *t, const soie_node *n,
	struct paintstate ps, int depth)
{
	const soie_node *k;
	char buf[16];
	const char *a;
	int sx, sy, chunk = 0;
	float v;

	if (depth > 0)
		inherit_paint(n, &ps);
	if ((a = prop(n, "display", buf, sizeof(buf))) && !strcmp(a, "none"))
		return;
	v = coord(n, "x", ps.vw, &sx);
	if (sx)
		t->x = v, chunk = 1;
	v = coord(n, "y", ps.vh, &sy);
	if (sy)
		t->y = v, chunk = 1;
	t->x += coord(n, "dx", ps.vw, &sx);
	t->y += coord(n, "dy", ps.vh, &sy);
	for (k = n->first; k; k = k->next) {
		if (k->type == SOIE_ELEMENT && k->ns == SOIE_NS_SVG &&
		    (!strcmp(k->name, "tspan") || !strcmp(k->name, "a"))) {
			text_runs(t, k, ps, depth + 1);
			continue;
		}
		if (k->type != SOIE_TEXT || !k->u.text.len || depth > 8)
			continue;
		{
			struct trun *r;
			const char *d = k->u.text.data;
			size_t i, o = 0;

			if (t->n == t->cap) {
				t->cap = t->cap ? t->cap * 2 : 8;
				t->r = xrealloc(t->r, (size_t)t->cap *
					sizeof(*t->r));
			}
			r = &t->r[t->n];
			r->s = xmalloc(k->u.text.len + 1);
			/* white space collapsed, none at the start */
			for (i = 0; i < k->u.text.len; i++) {
				char ch = d[i];

				if (ch == '\n' || ch == '\r' || ch == '\t')
					ch = ' ';
				if (ch == ' ' && t->space)
					continue;
				t->space = ch == ' ';
				r->s[o++] = ch;
			}
			if (!o) {
				free(r->s);
				continue;
			}
			r->len = o;
			r->x = t->x;
			r->y = t->y;
			r->chunk = chunk || !t->n;
			chunk = 0;
			r->ps = ps;
			{
				unsigned char rgba[4] = { 0, 0, 0, 255 };
				const struct style *st = text_style(&ps,
					ps.fsize * t->scale, rgba);

				r->w = text_width(st, r->s, r->len) / t->scale;
			}
			t->x += r->w;
			t->n++;
		}
	}
}

/* A <text> element: its runs placed, chunks anchored, drawn */
static void text_el(struct canvas *c, const soie_node *n, const struct mat *m,
	struct paintstate ps)
{
	struct tstate t;
	float scale = sqrtf(fabsf(m->a * m->d - m->b * m->c));
	int i, j;

	if (!ps.base || scale <= 0 || !(ps.fsize > 0))
		return;
	memset(&t, 0, sizeof(t));
	t.space = 1;
	t.scale = scale;
	text_runs(&t, n, ps, 0);
	/* no space at the end */
	if (t.n && t.r[t.n - 1].s[t.r[t.n - 1].len - 1] == ' ' &&
	    t.r[t.n - 1].len > 1) {
		t.r[t.n - 1].len--;
	}
	for (i = 0; i < t.n; i = j) {
		float w = 0, shift;

		for (j = i; j < t.n && (j == i || !t.r[j].chunk); j++)
			w += t.r[j].w;
		shift = t.r[i].ps.anchor == 'm' ? -w / 2 :
			t.r[i].ps.anchor == 'e' ? -w : 0;
		for (; i < j; i++) {
			struct trun *r = &t.r[i];
			unsigned char rgba[4];
			float px, py;

			if (!r->ps.has_fill)
				continue;
			memcpy(rgba, r->ps.fill, 4);
			rgba[3] = (unsigned char)(rgba[3] * r->ps.opacity);
			tx(m, r->x + shift, r->y, &px, &py);
			paint_text(c, text_style(&r->ps, r->ps.fsize * scale,
				rgba), r->s, r->len, px + c->ox, py + c->oy);
		}
	}
	for (i = 0; i < t.n; i++)
		free(t.r[i].s);
	free(t.r);
}

/* One element of the tree, with its paint state and transform */
/* Elements drawn by one svg_render(), those of <use> counted each
 * time: eight levels of thirty <use> each are 30^8 shapes in 4 KB */
#define SVG_BUDGET 200000
static int budget;

static void element(struct canvas *c, const soie_node *k, struct mat m,
	struct paintstate ps, int depth)
{
	const char *a;
	char buf[128];

	if (k->type != SOIE_ELEMENT || k->ns != SOIE_NS_SVG || --budget < 0)
		return;
	if (!strcmp(k->name, "defs") || !strcmp(k->name, "title") ||
	    !strcmp(k->name, "desc") || !strcmp(k->name, "clipPath") ||
	    !strcmp(k->name, "mask") || !strcmp(k->name, "symbol") ||
	    !strcmp(k->name, "linearGradient") ||
	    !strcmp(k->name, "radialGradient") ||
	    !strcmp(k->name, "style"))
		return;
	if (k->u.el.style) {
		if (kw(k->u.el.style, P_DISPLAY, "none"))
			return;
	} else if ((a = prop(k, "display", buf, sizeof(buf))) &&
	    !strcmp(a, "none")) {
		return;
	}
	if ((a = soie_element_get_attr(k, "transform")))
		parse_transform(a, &m);
	inherit_paint(k, &ps);
	if (!strcmp(k->name, "g") || !strcmp(k->name, "a") ||
	    !strcmp(k->name, "switch")) {
		walk(c, k, m, ps, depth);
	} else if (!strcmp(k->name, "svg")) {
		nested(c, k, m, ps, vlen(k, "x", ps.vw, 0),
			vlen(k, "y", ps.vh, 0), vlen(k, "width", ps.vw, ps.vw),
			vlen(k, "height", ps.vh, ps.vh), depth);
	} else if (!strcmp(k->name, "use")) {
		/* a symbol, an svg or an element of this document */
		const soie_node *t, *root = k;
		struct mat um = m;
		float x = vlen(k, "x", ps.vw, 0), y = vlen(k, "y", ps.vh, 0);

		a = use_href(k);
		if (!a || *a != '#' || depth >= USE_DEPTH)
			return;
		while (root->parent)
			root = root->parent;
		if (!(t = by_id(root, a + 1)) || t->type != SOIE_ELEMENT ||
		    t->ns != SOIE_NS_SVG)
			return;
		if (!strcmp(t->name, "symbol") || !strcmp(t->name, "svg")) {
			float w = vlen(k, "width", ps.vw, -1);
			float h = vlen(k, "height", ps.vh, -1);

			if (w < 0)
				w = vlen(t, "width", ps.vw, ps.vw);
			if (h < 0)
				h = vlen(t, "height", ps.vh, ps.vh);
			inherit_paint(t, &ps);
			nested(c, t, m, ps, x, y, w, h, depth + 1);
		} else {
			struct mat tr = { 1, 0, 0, 1, 0, 0 };

			tr.e = x;
			tr.f = y;
			mul(&um, &tr);
			element(c, t, um, ps, depth + 1);
		}
	} else if (k->u.el.style &&
	    !kw(k->u.el.style, P_VISIBILITY, "visible")) {
		/* hidden: neither its shape nor its text */
	} else if (!strcmp(k->name, "text")) {
		text_el(c, k, &m, ps);
	} else {
		shape(c, k, &m, ps);
	}
}

/* Groups nest this deep at most: walk() recurses through them */
#define SVG_NEST 256

static void walk(struct canvas *c, const soie_node *n, struct mat m,
	struct paintstate ps, int depth)
{
	const soie_node *k;

	if (++ps.nest > SVG_NEST)
		return;
	for (k = n->first; k; k = k->next)
		element(c, k, m, ps, depth);
}

/*
 * Draw <svg> element el into the box (x, y, w, h), page coordinates;
 * st, the style of that box, gives the text its font (the initial one
 * when the SVG is an image).
 */
void svg_render(struct canvas *c, const soie_node *el, float x, float y,
	float w, float h, const unsigned char *current,
	const struct style *st, int image)
{
	struct mat m = { 1, 0, 0, 1, 0, 0 };
	struct paintstate ps;

	memset(&ps, 0, sizeof(ps));
	ps.fill[3] = 255;	/* black */
	ps.has_fill = 1;
	ps.sw = 1;
	ps.join = 'm';
	ps.cap = 'b';
	ps.miter = 4;
	ps.opacity = c->alpha;
	memcpy(ps.current, current, 4);
	ps.base = st;
	ps.anchor = 's';
	if (st) {
		ps.fsize = image ? 16 : st->v[P_FONT_SIZE].u.len.px /
			doc_zoom();
		ps.weight = image ? 400 : (int)st->v[P_FONT_WEIGHT].u.num;
		ps.family = image ? intern_family("serif") : NULL;
	}
	m.e = -c->ox;
	m.f = -c->oy;
	{
		const char *aw = soie_element_get_attr(el, "width");
		const char *ah = soie_element_get_attr(el, "height");

		/* an image without viewBox but sized scales as if it had
		 * one of that size, as browsers do */
		if (image && !soie_element_get_attr(el, "viewBox") && aw &&
		    ah && !strchr(aw, '%') && !strchr(ah, '%') &&
		    fnum(aw) > 0 && fnum(ah) > 0) {
			struct mat t = { w / fnum(aw), 0, 0, h / fnum(ah), x,
				y };

			mul(&m, &t);
			ps.vw = fnum(aw);
			ps.vh = fnum(ah);
		} else {
			viewport(&m, el, x, y, w, h, &ps);
		}
	}
	if (el->u.el.style)
		styled_paint(el, &ps, 1);
	else
		inherit_paint(el, &ps);
	{
		float a = c->alpha;

		c->alpha = 1;	/* opacity is in the paint state */
		budget = SVG_BUDGET;
		walk(c, el, m, ps, 0);
		c->alpha = a;
	}
}

/* Intrinsic size of an <svg>: width/height, else the viewBox. */
void svg_size(const soie_node *el, float em, float *w, float *h)
{
	const char *vb = soie_element_get_attr(el, "viewBox");
	const char *aw = soie_element_get_attr(el, "width");
	const char *ah = soie_element_get_attr(el, "height");
	const char *s = vb;
	float v[4] = { 0, 0, 0, 0 };
	int i;

	for (i = 0; s && i < 4; i++)
		v[i] = num(&s);
	if (isnan(v[2]) || isnan(v[3]))
		v[2] = v[3] = 0;
	/* units count (width="1em", "5in"); a percentage is no size */
	*w = aw && !strchr(aw, '%') && !isnan(fnum(aw)) && *aw ?
		ulen(aw, em) : v[2];
	*h = ah && !strchr(ah, '%') && !isnan(fnum(ah)) && *ah ?
		ulen(ah, em) : v[3];
	if (*w < 0)
		*w = 0;
	if (*h < 0)
		*h = 0;
}

/* CSS clip-path on an HTML box */

/* A position after "at": keywords, offsets from them, lengths */
static void shape_at(struct cur *k, float w, float h, float em, float *x,
	float *y)
{
	const struct cv *v;
	int axis = 0;

	*x = w / 2;
	*y = h / 2;
	while ((v = cur_next(k))) {
		const struct cv *o = cur_peek(k);
		int far = cv_ident(v, "right") || cv_ident(v, "bottom");
		float off = 0;

		if (v->type == CT_IDENT && o && o->type != CT_IDENT)
			off = css_len_px(cur_next(k), 0, em);
		if (cv_ident(v, "left") || cv_ident(v, "right")) {
			*x = far ? w - off : off;
			axis = 1;
		} else if (cv_ident(v, "top") || cv_ident(v, "bottom")) {
			*y = far ? h - off : off;
			axis = 0;
		} else if (cv_ident(v, "center")) {
			axis = !axis;
		} else if (axis == 0) {
			*x = css_len_px(v, w, em);
			axis = 1;
		} else {
			*y = css_len_px(v, h, em);
			axis = 0;
		}
	}
}

/* An ellipse's radius: a length, closest-side (default), farthest-side */
static float shape_radius(const struct cv *v, float c, float side, float ref,
	float em)
{
	float a = c, b = side - c;

	if (cv_ident(v, "farthest-side"))
		return a > b ? a : b;
	if (!v || v->type == CT_IDENT)
		return a < b ? a : b;
	return css_len_px(v, ref, em);
}

/* inset()'s rounded rectangle, its radii scaled down to fit */
static void shape_inset(struct pb *b, struct cur *k, float w, float h,
	float em)
{
	const struct cv *v;
	float d[4], r[4] = { 0, 0, 0, 0 }, f = 1, x0, y0, x1, y1;
	int n = 0, nr = 0, i;

	while ((v = cur_next(k)) && !cv_ident(v, "round") && n < 4) {
		d[n] = css_len_px(v, n % 2 ? w : h, em);
		n++;
	}
	if (!n)
		return;
	for (i = n; i < 4; i++)	/* as margin: t, r = t, b = t, l = r */
		d[i] = d[i == 3 ? 1 : i - 2 < 0 ? 0 : i - 2];
	while (v && (v = cur_next(k)) && nr < 4 && v->type != CT_DELIM)
		r[nr++] = css_len_px(v, w, em);
	for (i = nr; nr && i < 4; i++)
		r[i] = r[i == 3 ? 1 : i - 2 < 0 ? 0 : i - 2];
	x0 = d[3];
	y0 = d[0];
	x1 = w - d[1];
	y1 = h - d[2];
	if (x1 <= x0 || y1 <= y0)
		return;
	for (i = 0; i < 4; i++) {
		float s = r[i] + r[(i + 1) % 4],
			side = i % 2 ? y1 - y0 : x1 - x0;

		if (s > side && side / s < f)
			f = side / s;
	}
	for (i = 0; i < 4; i++)
		r[i] *= f;
	pb_move(b, x0 + r[0], y0);
	pb_line(b, x1 - r[1], y0);
	pb_arc(b, r[1], r[1], 0, 0, 1, x1, y0 + r[1]);
	pb_line(b, x1, y1 - r[2]);
	pb_arc(b, r[2], r[2], 0, 0, 1, x1 - r[2], y1);
	pb_line(b, x0 + r[3], y1);
	pb_arc(b, r[3], r[3], 0, 0, 1, x0, y1 - r[3]);
	pb_line(b, x0, y0 + r[0]);
	pb_arc(b, r[0], r[0], 0, 0, 1, x0 + r[0], y0);
	pb_close(b);
}

static void shape_ellipse(struct pb *b, struct cur *k, int circle, float w,
	float h, float em)
{
	const struct cv *v, *r[2] = { NULL, NULL };
	float cx, cy, rx, ry;
	int n = 0;

	while ((v = cur_next(k)) && !cv_ident(v, "at"))
		if (n < 2)
			r[n++] = v;
	if (v)
		shape_at(k, w, h, em, &cx, &cy);
	else
		cx = w / 2, cy = h / 2;
	if (circle) {
		float ref = sqrtf((w * w + h * h) / 2);

		if (cv_ident(r[0], "farthest-side")) {
			float a = shape_radius(r[0], cx, w, w, em),
				c = shape_radius(r[0], cy, h, h, em);

			rx = a > c ? a : c;
		} else if (!r[0] || r[0]->type == CT_IDENT) {
			float a = shape_radius(NULL, cx, w, w, em),
				c = shape_radius(NULL, cy, h, h, em);

			rx = a < c ? a : c;
		} else {
			rx = css_len_px(r[0], ref, em);
		}
		ry = rx;
	} else {
		rx = shape_radius(r[0], cx, w, w, em);
		ry = shape_radius(r[1], cy, h, h, em);
	}
	if (rx <= 0 || ry <= 0)
		return;
	pb_move(b, cx + rx, cy);
	pb_arc(b, rx, ry, 0, 0, 1, cx - rx, cy);
	pb_arc(b, rx, ry, 0, 0, 1, cx + rx, cy);
	pb_close(b);
}

static void shape_polygon(struct pb *b, struct cur *k, float w, float h,
	float em, int *evenodd)
{
	const struct cv *v;
	float xy[2];
	int n = 0, first = 1;

	while ((v = cur_next(k))) {
		if (v->type == CT_COMMA)
			continue;
		if (cv_ident(v, "evenodd") || cv_ident(v, "nonzero")) {
			*evenodd = cv_ident(v, "evenodd");
			continue;
		}
		xy[n] = css_len_px(v, n ? h : w, em);
		if (++n < 2)
			continue;
		n = 0;
		if (first)
			pb_move(b, xy[0], xy[1]);
		else
			pb_line(b, xy[0], xy[1]);
		first = 0;
	}
	pb_close(b);
}

/* path([fill-rule,] "d"), in CSS px from the box's corner */
static void shape_path(struct pb *b, struct cur *k, int *evenodd)
{
	const struct cv *v;

	while ((v = cur_next(k)))
		if (cv_ident(v, "evenodd") || cv_ident(v, "nonzero"))
			*evenodd = cv_ident(v, "evenodd");
		else if (v->type == CT_STRING)
			parse_path(b, v->str);
}

/*
 * b's clip-path filled opaque into c: inset(), circle(), ellipse(),
 * polygon(), path() or url(#<clipPath>), in its reference box (border
 * box unless named); 0 for none or what is not one of them, which
 * clips nothing.
 */
int clip_path_paint(struct canvas *c, const struct box *b)
{
	const char *s = b->style->v[P_CLIP_PATH].u.str;
	float em = b->style->v[P_FONT_SIZE].u.len.px, z = doc_zoom();
	float bx = b->x - b->p[3] - b->b[3], by = b->y - b->p[0] - b->b[0];
	float bw = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
	float bh = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
	float e[4] = { 0, 0, 0, 0 };	/* the reference box, from bx.. */
	const struct cv *v, *fn = NULL;
	unsigned char black[4] = { 0, 0, 0, 255 };
	struct mat m = { 1, 0, 0, 1, 0, 0 };
	struct poly p, segs;
	struct pb pb;
	struct cvlist l;
	struct cur k;
	int i, evenodd = 0, ok = 0;

	if (!s || !strcmp(s, "none"))
		return 0;
	css_component_values(s, strlen(s), &l);
	cur_init(&k, &l);
	while ((v = cur_next(&k))) {
		if (v->type == CT_FUNCTION || v->type == CT_URL)
			fn = v;
		else if (cv_ident(v, "margin-box"))
			for (i = 0; i < 4; i++)
				e[i] = -b->m[i];
		else if (cv_ident(v, "padding-box"))
			for (i = 0; i < 4; i++)
				e[i] = b->b[i];
		else if (cv_ident(v, "content-box") ||
			 cv_ident(v, "fill-box"))
			for (i = 0; i < 4; i++)
				e[i] = b->b[i] + b->p[i];
	}
	bx += e[3];
	by += e[0];
	bw -= e[1] + e[3];
	bh -= e[0] + e[2];
	memset(&p, 0, sizeof(p));
	memset(&segs, 0, sizeof(segs));
	memset(&pb, 0, sizeof(pb));
	pb.p = &p;
	pb.s = &segs;
	pb.m = &m;
	m.e = bx - c->ox;
	m.f = by - c->oy;
	if (!fn) {
		/* a box alone: its rectangle */
		pb_move(&pb, 0, 0);
		pb_line(&pb, bw, 0);
		pb_line(&pb, bw, bh);
		pb_line(&pb, 0, bh);
		pb_close(&pb);
		ok = 1;
	} else if (fn->type == CT_URL || cv_func(fn, "url")) {
		const char *u = fn->type == CT_URL ? fn->str :
			fn->kids.n ? fn->kids.v[0].str : NULL;
		const soie_node *cp = u && *u == '#' && paint_doc ?
			by_id(paint_doc, u + 1) : NULL;

		if (cp && !strcmp(cp->name, "clipPath")) {
			const char *un = soie_element_get_attr(cp,
				"clipPathUnits");
			struct paintstate ps;

			memset(&ps, 0, sizeof(ps));
			memcpy(ps.fill, black, 4);
			ps.has_fill = 1;
			ps.opacity = 1;
			ps.join = 'm';
			ps.cap = 'b';
			ps.miter = 4;
			ps.vw = bw / z;
			ps.vh = bh / z;
			if (un && !strcmp(un, "objectBoundingBox")) {
				m.a = bw;
				m.d = bh;
			} else {
				m.a = m.d = z;
			}
			budget = SVG_BUDGET, walk(c, cp, m, ps, 0);
			ok = 1;
		}
	} else {
		struct cur a;

		cur_init(&a, &fn->kids);
		ok = 1;
		if (cv_func(fn, "inset"))
			shape_inset(&pb, &a, bw, bh, em);
		else if (cv_func(fn, "circle") || cv_func(fn, "ellipse"))
			shape_ellipse(&pb, &a, cv_func(fn, "circle"), bw, bh,
				em);
		else if (cv_func(fn, "polygon"))
			shape_polygon(&pb, &a, bw, bh, em, &evenodd);
		else if (cv_func(fn, "path")) {
			m.a = m.d = z;
			shape_path(&pb, &a, &evenodd);
		} else
			ok = 0;
	}
	if (ok)
		poly_fill(c, &p, evenodd, black, NULL);
	free(p.e);
	free(segs.e);
	cvlist_free(&l);
	return ok;
}
