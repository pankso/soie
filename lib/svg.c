/*
 * SVG, static subset: shapes, paths, groups with transforms, fill and
 * stroke, viewBox. Rendered from a DOM (inline <svg>, or an SVG file
 * parsed by the HTML parser, which handles foreign content) with an
 * anti-aliased polygon filler (analytic in x, 4 sub-scanlines in y).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "paint.h"
#include "image.h"

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

struct xing {
	float x;
	int dir;
};

static int xcmp(const void *a, const void *b)
{
	float d = ((const struct xing *)a)->x - ((const struct xing *)b)->x;

	return d < 0 ? -1 : d > 0;
}

/* Fill p (pixel coordinates of the canvas) with rgba. */
static void poly_fill(struct canvas *c, struct poly *p, int evenodd,
	const unsigned char *rgba)
{
	float ymin = 1e30f, ymax = -1e30f, *acc;
	struct xing *xs;
	int i, y, s, k, nx;

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
	for (y = (int)floorf(ymin); y < (int)ceilf(ymax); y++) {
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
					int ia = (int)floorf(a), ib = (int)floorf(b);

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
					a = rgba[3] * cov / 255;
					a = (int)((float)a * c->alpha + 0.5f);
					px = c->px + (size_t)y * (size_t)c->stride +
						(size_t)i * 4;
					px[0] = (unsigned char)((rgba[0] * a +
						px[0] * (255 - a)) / 255);
					px[1] = (unsigned char)((rgba[1] * a +
						px[1] * (255 - a)) / 255);
					px[2] = (unsigned char)((rgba[2] * a +
						px[2] * (255 - a)) / 255);
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

/* Transforms: a b c d e f, x' = a x + c y + e */
struct mat {
	float a, b, c, d, e, f;
};

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
	const struct mat *m;
	float x, y, sx, sy;	/* user space */
	int open;
};

static void pb_line(struct pb *b, float x, float y)
{
	float ax, ay, bx, by;

	tx(b->m, b->x, b->y, &ax, &ay);
	tx(b->m, x, y, &bx, &by);
	poly_edge(b->p, ax, ay, bx, by);
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
}

static void pb_cubic(struct pb *b, float x1, float y1, float x2, float y2,
	float x, float y)
{
	float x0 = b->x, y0 = b->y;
	int i, n = 16;

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
	const unsigned char *current;	/* currentColor */
};

static int svg_color(const char *v, const unsigned char *current,
	unsigned char *out)
{
	struct cvlist l;
	struct cur c;
	struct color col;
	int ok = 0;

	if (!v || !strcmp(v, "none"))
		return 0;
	if (!strncmp(v, "url(", 4)) {
		/* gradients and patterns: a neutral gray stand-in */
		out[0] = out[1] = out[2] = 128;
		out[3] = 255;
		return 1;
	}
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

static void inherit_paint(const soie_node *n, struct paintstate *ps)
{
	char buf[128];
	const char *v;

	if ((v = prop(n, "fill", buf, sizeof(buf))))
		ps->has_fill = svg_color(v, ps->current, ps->fill);
	if ((v = prop(n, "stroke", buf, sizeof(buf))))
		ps->has_stroke = svg_color(v, ps->current, ps->stroke);
	if ((v = prop(n, "stroke-width", buf, sizeof(buf))))
		ps->sw = fnum(v);
	if ((v = prop(n, "fill-rule", buf, sizeof(buf))))
		ps->evenodd = !strcmp(v, "evenodd");
	if ((v = prop(n, "opacity", buf, sizeof(buf))))
		ps->opacity *= fnum(v);
	if ((v = prop(n, "fill-opacity", buf, sizeof(buf))))
		ps->fill[3] = (unsigned char)(ps->fill[3] * fnum(v));
	if ((v = prop(n, "stroke-opacity", buf, sizeof(buf))))
		ps->stroke[3] = (unsigned char)(ps->stroke[3] * fnum(v));
}

static float attrf(const soie_node *n, const char *name, float dflt)
{
	const char *a = soie_element_get_attr(n, name);

	return a ? fnum(a) : dflt;
}

/* Stroke: every segment of the outline as a quad, joints as squares. */
static void stroke_poly(struct canvas *c, struct poly *outline, float w,
	const unsigned char *rgba)
{
	struct poly p;
	int i;

	memset(&p, 0, sizeof(p));
	for (i = 0; i < outline->n; i++) {
		struct edge *e = &outline->e[i];
		float dx = e->x1 - e->x0, dy = e->y1 - e->y0;
		float l = sqrtf(dx * dx + dy * dy), nx, ny;

		if (l == 0)
			continue;
		nx = -dy / l * w / 2;
		ny = dx / l * w / 2;
		poly_edge(&p, e->x0 + nx, e->y0 + ny, e->x1 + nx, e->y1 + ny);
		poly_edge(&p, e->x1 + nx, e->y1 + ny, e->x1 - nx, e->y1 - ny);
		poly_edge(&p, e->x1 - nx, e->y1 - ny, e->x0 - nx, e->y0 - ny);
		poly_edge(&p, e->x0 - nx, e->y0 - ny, e->x0 + nx, e->y0 + ny);
		/* each quad its own winding: nonzero merges them */
	}
	poly_fill(c, &p, 0, rgba);
	free(p.e);
}

static void shape(struct canvas *c, const soie_node *n, const struct mat *m,
	struct paintstate ps)
{
	struct poly p;
	struct pb b;
	const char *t = n->name, *a;
	float x, y, w, h, r, rx, ry;
	unsigned char col[4];

	memset(&p, 0, sizeof(p));
	memset(&b, 0, sizeof(b));
	b.p = &p;
	b.m = m;
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
		poly_fill(c, &f, ps.evenodd, col);
	}
	if (ps.has_stroke && ps.sw > 0) {
		float scale = sqrtf(fabsf(m->a * m->d - m->b * m->c));

		memcpy(col, ps.stroke, 4);
		col[3] = (unsigned char)(col[3] * ps.opacity);
		stroke_poly(c, &p, ps.sw * scale, col);
	}
	free(p.e);
}

static void walk(struct canvas *c, const soie_node *n, struct mat m,
	struct paintstate ps)
{
	const soie_node *k;
	const char *a;

	for (k = n->first; k; k = k->next) {
		struct mat km = m;
		struct paintstate kps = ps;
		char buf[128];

		if (k->type != SOIE_ELEMENT || k->ns != SOIE_NS_SVG)
			continue;
		if (!strcmp(k->name, "defs") || !strcmp(k->name, "title") ||
		    !strcmp(k->name, "desc") || !strcmp(k->name, "clipPath") ||
		    !strcmp(k->name, "mask") || !strcmp(k->name, "symbol") ||
		    !strcmp(k->name, "linearGradient") ||
		    !strcmp(k->name, "radialGradient") ||
		    !strcmp(k->name, "style") || !strcmp(k->name, "text"))
			continue;
		if ((a = prop(k, "display", buf, sizeof(buf))) &&
		    !strcmp(a, "none"))
			continue;
		if ((a = soie_element_get_attr(k, "transform")))
			parse_transform(a, &km);
		inherit_paint(k, &kps);
		if (!strcmp(k->name, "g") || !strcmp(k->name, "a") ||
		    !strcmp(k->name, "switch"))
			walk(c, k, km, kps);
		else
			shape(c, k, &km, kps);
	}
}

/* Draw <svg> element el into the box (x, y, w, h), page coordinates. */
void svg_render(struct canvas *c, const soie_node *el, float x, float y,
	float w, float h, const unsigned char *current)
{
	struct mat m = { 1, 0, 0, 1, 0, 0 };
	struct paintstate ps;
	const char *vb = soie_element_get_attr(el, "viewBox");
	const char *par = soie_element_get_attr(el, "preserveAspectRatio");
	float v[4];

	memset(&ps, 0, sizeof(ps));
	ps.fill[3] = 255;	/* black */
	ps.has_fill = 1;
	ps.sw = 1;
	ps.opacity = c->alpha;
	ps.current = current;
	m.e = x - c->ox;
	m.f = y - c->oy;
	if (vb) {
		const char *s = vb;
		int i;

		for (i = 0; i < 4; i++)
			v[i] = num(&s);
		if (!isnan(v[3]) && v[2] > 0 && v[3] > 0) {
			float sx = w / v[2], sy = h / v[3];

			if (!par || strcmp(par, "none")) {
				float s1 = sx < sy ? sx : sy;

				if (par && strstr(par, "slice"))
					s1 = sx > sy ? sx : sy;
				m.e += (w - v[2] * s1) / 2;
				m.f += (h - v[3] * s1) / 2;
				sx = sy = s1;
			}
			m.a = sx;
			m.d = sy;
			m.e -= v[0] * sx;
			m.f -= v[1] * sy;
		}
	}
	inherit_paint(el, &ps);
	{
		float a = c->alpha;

		c->alpha = 1;	/* opacity is in the paint state */
		walk(c, el, m, ps);
		c->alpha = a;
	}
}

/* Intrinsic size of an <svg>: width/height, else the viewBox. */
void svg_size(const soie_node *el, float *w, float *h)
{
	const char *vb = soie_element_get_attr(el, "viewBox");
	const char *s = vb;
	float v[4] = { 0, 0, 0, 0 };
	int i;

	for (i = 0; s && i < 4; i++)
		v[i] = num(&s);
	*w = attrf(el, "width", isnan(v[2]) ? 0 : v[2]);
	*h = attrf(el, "height", isnan(v[3]) ? 0 : v[3]);
	if (soie_element_get_attr(el, "width") &&
	    strchr(soie_element_get_attr(el, "width"), '%'))
		*w = v[2];
	if (soie_element_get_attr(el, "height") &&
	    strchr(soie_element_get_attr(el, "height"), '%'))
		*h = v[3];
}
