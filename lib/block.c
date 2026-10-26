/*
 * Block layout: normal flow with margin collapsing (CSS 2 8.3.1, 10.3,
 * 10.6), floats (9.5), relative and absolute positioning (9.3, 10.3.7).
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "layout.h"

/* Used values */

float len_val(const struct cval *v, float ref)
{
	if (v->u.len.calc)
		return calc_eval(v->u.len.calc, ref);
	return v->u.len.px + v->u.len.pct * ref / 100;
}

int is_auto(const struct style *st, int prop)
{
	return st->v[prop].kw != 0;	/* auto, none, *-content... */
}

static int is_pct(const struct style *st, int prop)
{
	const struct cval *v = &st->v[prop];

	return !v->kw && (v->u.len.pct != 0 || v->u.len.calc);
}

/* Floats of a block formatting context, page coordinates. */
struct flt {
	float x, y, w, h;	/* margin box */
	int left;
};

struct bfc {
	struct flt *v;
	int n, cap;
};

float bfc_left(const struct bfc *f, float y, float h, float x0)
{
	int i;

	for (i = 0; f && i < f->n; i++) {
		const struct flt *t = &f->v[i];

		if (t->left && t->y < y + (h > 0 ? h : 0.01f) &&
		    t->y + t->h > y && t->x + t->w > x0)
			x0 = t->x + t->w;
	}
	return x0;
}

float bfc_right(const struct bfc *f, float y, float h, float x1)
{
	int i;

	for (i = 0; f && i < f->n; i++) {
		const struct flt *t = &f->v[i];

		if (!t->left && t->y < y + (h > 0 ? h : 0.01f) &&
		    t->y + t->h > y && t->x < x1)
			x1 = t->x;
	}
	return x1;
}

/* The first float bottom below y, or y if none. */
float bfc_next(const struct bfc *f, float y)
{
	float next = 1e30f;
	int i;

	for (i = 0; f && i < f->n; i++)
		if (f->v[i].y + f->v[i].h > y && f->v[i].y + f->v[i].h < next)
			next = f->v[i].y + f->v[i].h;
	return next < 1e30f ? next : y;
}

/* Bottom of the floats on a side ('l', 'r' or 'b'oth). */
static float bfc_clear(const struct bfc *f, int side)
{
	float y = -1e30f;
	int i;

	for (i = 0; f && i < f->n; i++)
		if ((side == 'b' || (side == 'l') == f->v[i].left) &&
		    f->v[i].y + f->v[i].h > y)
			y = f->v[i].y + f->v[i].h;
	return y;
}

static float bfc_bottom(const struct bfc *f)
{
	return bfc_clear(f, 'b');
}

/* Absolutely positioned boxes, laid out once their parent's is done. */
static struct {
	struct layout *l;
	struct box **abs;
	int nabs, cap;
	unsigned pass;		/* this layout_root(), for abs_pass */
	int trial;		/* in a table's trial layouts (sizes) */
	/* the static positions those met, as they were before */
	struct saved {
		struct box *b;
		float sx, sy;
	} *saved;
	int nsaved, capsaved;
	/* every abs_add(), twice when laid out twice: what a box held */
	struct box **log;
	int nlog, caplog;
} g;

static void abs_add(struct box *b);

/* An absolute box's static position, the last one when its flow is
 * laid out more than once (measures): it is laid out once, after */
void abs_static(struct box *b, float x, float y)
{
	if (g.trial) {
		/* a table measured: its real layout is another time, the
		 * static position comes back after */
		if (g.nsaved == g.capsaved) {
			g.capsaved = g.capsaved ? g.capsaved * 2 : 16;
			g.saved = xrealloc(g.saved, (size_t)g.capsaved *
				sizeof(*g.saved));
		}
		g.saved[g.nsaved].b = b;
		g.saved[g.nsaved].sx = b->sx;
		g.saved[g.nsaved++].sy = b->sy;
		return;
	}
	b->x = b->sx = x;
	b->y = b->sy = y;
	abs_add(b);
}

static void abs_add(struct box *b)
{
	if (g.nlog == g.caplog) {
		g.caplog = g.caplog ? g.caplog * 2 : 16;
		g.log = xrealloc(g.log, (size_t)g.caplog * sizeof(*g.log));
	}
	g.log[g.nlog++] = b;
	if (b->abs_pass == g.pass)
		return;
	b->abs_pass = g.pass;
	if (g.nabs == g.cap) {
		g.cap = g.cap ? g.cap * 2 : 16;
		g.abs = xrealloc(g.abs, (size_t)g.cap * sizeof(*g.abs));
	}
	g.abs[g.nabs++] = b;
}

static void move(struct box *b, float dx, float dy, int root)
{
	struct box *c;
	int i;

	b->x += dx;
	b->y += dy;
	b->lx += dx;
	b->ly += dy;
	if (!root) {
		/* a static position is in the parent's flow: the box
		 * placed itself does not move it, its ancestors do */
		b->sx += dx;
		b->sy += dy;
	}
	for (i = 0; i < b->nlines; i++) {
		b->lines[i].x += dx;
		b->lines[i].y += dy;
		b->lines[i].baseline += dy;
	}
	for (i = 0; i < b->nfrags; i++) {
		b->frags[i].x += dx;
		b->frags[i].y += dy;
		b->frags[i].baseline += dy;
	}
	b->baseline += dy;
	b->last_baseline += dy;
	for (c = b->first; c; c = c->next)
		move(c, dx, dy, 0);
}

void translate(struct box *b, float dx, float dy)
{
	move(b, dx, dy, 1);
}

void translate_flow(struct box *b, float dx, float dy)
{
	move(b, dx, dy, 0);
}

/* Margins, borders and paddings: percentages refer to the width. */
static void edges(struct box *b, float cbw)
{
	static const int bw[4] = { P_BORDER_TOP_WIDTH, P_BORDER_RIGHT_WIDTH,
		P_BORDER_BOTTOM_WIDTH, P_BORDER_LEFT_WIDTH };
	static const int pd[4] = { P_PADDING_TOP, P_PADDING_RIGHT,
		P_PADDING_BOTTOM, P_PADDING_LEFT };
	static const int mg[4] = { P_MARGIN_TOP, P_MARGIN_RIGHT,
		P_MARGIN_BOTTOM, P_MARGIN_LEFT };
	const struct style *st = b->style;
	int i;

	for (i = 0; i < 4; i++) {
		b->b[i] = st->v[bw[i]].u.len.px;
		b->p[i] = len_val(&st->v[pd[i]], cbw);
		b->m[i] = is_auto(st, mg[i]) ? 0 : len_val(&st->v[mg[i]], cbw);
	}
	if (b->type == B_INLINE || b->type == B_TEXT || b->type == B_BREAK) {
		/* vertical margins of inline boxes do not apply */
		b->m[0] = b->m[2] = 0;
	}
}

void box_edges(struct box *b, float cbw)
{
	edges(b, cbw);
}

static float hedges(const struct box *b)
{
	return b->b[1] + b->b[3] + b->p[1] + b->p[3];
}

static float vedges(const struct box *b)
{
	return b->b[0] + b->b[2] + b->p[0] + b->p[2];
}

static int border_box(const struct style *st)
{
	return kw(st, P_BOX_SIZING, "border-box");
}

/* A width property as a content width, or -1 for auto. */
static float spec_width(const struct box *b, int prop, float cbw)
{
	const struct style *st = b->style;
	float w;

	if (is_auto(st, prop))
		return -1;
	if (cbw < 0 && is_pct(st, prop))
		return -1;
	w = len_val(&st->v[prop], cbw);
	if (border_box(st))
		w -= hedges(b);
	return w < 0 ? 0 : w;
}

/* Heights: percentages need a definite containing block height. */
static float spec_height(const struct box *b, int prop, float cbh)
{
	const struct style *st = b->style;
	float h;

	if (is_auto(st, prop))
		return -1;
	if (cbh < 0 && is_pct(st, prop))
		return -1;
	h = len_val(&st->v[prop], cbh);
	if (border_box(st))
		h -= vedges(b);
	return h < 0 ? 0 : h;
}

static float clamp_w(const struct box *b, float w, float cbw)
{
	float mx = spec_width(b, P_MAX_WIDTH, cbw);
	float mn = spec_width(b, P_MIN_WIDTH, cbw);

	if (mx >= 0 && w > mx)
		w = mx;
	if (mn >= 0 && w < mn)
		w = mn;
	return w;
}

static float clamp_h(const struct box *b, float h, float cbh)
{
	float mx = spec_height(b, P_MAX_HEIGHT, cbh);
	float mn = spec_height(b, P_MIN_HEIGHT, cbh);

	if (mx >= 0 && h > mx)
		h = mx;
	if (mn >= 0 && h < mn)
		h = mn;
	return h;
}

static int establishes_bfc(const struct box *b)
{
	const struct style *st = b->style;

	return (b->flags & (BF_BFC | BF_FLOAT | BF_ABS)) || b->role ||
		b->type == B_ATOMIC || !kw(st, P_OVERFLOW_X, "visible") ||
		!kw(st, P_OVERFLOW_Y, "visible") ||
		kw(st, P_DISPLAY, "flow-root") || kw(st, P_DISPLAY, "flex") ||
		kw(st, P_DISPLAY, "grid") || kw(st, P_DISPLAY, "table") ||
		kw(st, P_DISPLAY, "table-cell") ||
		kw(st, P_DISPLAY, "table-caption");
}

/* Intrinsic sizes (CSS Sizing 3): content widths, then outer. */

void content_sizes(struct box *b, float *mn, float *mx)
{
	float w, cmin, cmax;
	struct box *c;

	edges(b, 0);
	if (b->sized) {
		*mn = b->cmin;
		*mx = b->cmax;
		return;
	}
	if (b->type == B_REPLACED) {
		w = spec_width(b, P_WIDTH, -1);
		*mn = *mx = w >= 0 ? w : b->iw > 0 ? b->iw : 0;
		return;
	}
	w = spec_width(b, P_WIDTH, -1);
	if (w >= 0) {
		*mn = *mx = w;
		return;
	}
	*mn = *mx = 0;
	if (b->role == R_TABLE) {
		int mark = g.nsaved;

		g.trial++;
		layout_table(b, 0);
		*mn = b->w;
		layout_table(b, 1e7f);
		*mx = b->w;
		g.trial--;
		while (g.nsaved > mark) {
			struct saved *v = &g.saved[--g.nsaved];

			v->b->sx = v->sx;
			v->b->sy = v->sy;
		}
		/* its geometry is the trials' now: whatever holds it is
		 * laid out again, not moved */
		for (c = b; c; c = c->parent)
			c->lok = 0;
		b->cmin = *mn;
		b->cmax = *mx;
		b->sized = 1;
		return;
	}
	if (is_flexgrid(b)) {
		if (kw(b->style, P_DISPLAY, "flex") ||
		    kw(b->style, P_DISPLAY, "inline-flex"))
			flex_sizes(b, mn, mx);
		else
			grid_sizes(b, mn, mx);
	} else if (b->flags & BF_INLINE_KIDS) {
		inline_sizes(b, mn, mx);
	} else {
		for (c = b->first; c; c = c->next) {
			if (c->flags & BF_ABS)
				continue;
			box_sizes(c, &cmin, &cmax);
			if (cmin > *mn)
				*mn = cmin;
			if (cmax > *mx)
				*mx = cmax;
		}
	}
	*mn = clamp_w(b, *mn, -1);
	*mx = clamp_w(b, *mx, -1);
	b->cmin = *mn;
	b->cmax = *mx;
	b->sized = 1;
}

/* Margin box intrinsic widths (percentages count as zero). */
void box_sizes(struct box *b, float *mn, float *mx)
{
	float e;

	content_sizes(b, mn, mx);
	e = hedges(b) + b->m[1] + b->m[3];
	*mn += e;
	*mx += e;
}

/* min(max(min-content, available), max-content) */
static float shrink_to_fit(struct box *b, float avail)
{
	float mn, mx;

	content_sizes(b, &mn, &mx);
	if (avail < mn)
		avail = mn;
	return avail < mx ? avail : mx;
}

/* Margin collapsing */

struct strut {
	float pos, neg;
};

static void strut_add(struct strut *s, float m)
{
	if (m > s->pos)
		s->pos = m;
	if (m < s->neg)
		s->neg = m;
}

static float strut_val(const struct strut *s)
{
	return s->pos + s->neg;
}

static float layout_flow(struct box *b, struct bfc *f, float y,
	struct strut *s, float cbh);

/*
 * The content of a block container whose x, w and edges are set.
 * y is the flow position of its content top, before the margins in s
 * when collapse_top. Sets b->y and returns the content height. On
 * return s holds the margins still pending at the bottom.
 */
static float layout_contents(struct box *b, struct bfc *f, float y,
	struct strut *s, int collapse_top, float cbh)
{
	struct box *c;
	float end, h;

	if (is_flexgrid(b)) {
		b->y = y;
		return kw(b->style, P_DISPLAY, "flex") ||
			kw(b->style, P_DISPLAY, "inline-flex") ?
			layout_flex(b, cbh) : layout_grid(b, cbh);
	}
	if (b->flags & BF_INLINE_KIDS) {
		float top = collapse_top ? y + strut_val(s) : y;

		b->y = top;
		h = layout_inline(b, f, b->x, top, b->w, cbh);
		if (b->nlines && collapse_top)
			s->pos = s->neg = 0;
		return h;
	}
	if (!collapse_top) {
		b->y = y;
		end = layout_flow(b, f, y, s, cbh);
		return end - y;
	}
	end = layout_flow(b, f, y, s, cbh);
	/* the content top is where the first in-flow child landed */
	for (c = b->first; c; c = c->next)
		if (!(c->flags & (BF_FLOAT | BF_ABS)))
			break;
	if (c && (c->h || c->nlines || vedges(c))) {
		float dx, dy;

		/* where it landed in the flow: before position: relative */
		rel_offset(c, b->w, &dx, &dy);
		b->y = c->y - dy - c->p[0] - c->b[0];
		return end - b->y;
	}
	b->y = y + strut_val(s);	/* empty: margins go through */
	return end > b->y ? end - b->y : 0;
}

/* Lay out a box laid out alone at (x, y): floats, inline-blocks, abs. */
static void layout_alone(struct box *b, float x, float y, float w,
	float cbh, float defh);

static void rel_position(const struct box *b, float cbw, float *dx,
	float *dy);

/* A length of a transform function: px, em, rem, % of ref */
static float tr_len(const char **sp, const struct box *b, float ref)
{
	char *e;
	float v = strtof(*sp, &e);

	if (e == *sp)
		return 0;
	if (*e == '%') {
		v = v * ref / 100;
		e++;
	} else if (!strncmp(e, "px", 2)) {
		v *= doc_zoom();
		e += 2;
	} else if (!strncmp(e, "rem", 3)) {
		v *= 16 * doc_zoom();
		e += 3;
	} else if (!strncmp(e, "em", 2)) {
		v *= b->style->v[P_FONT_SIZE].u.len.px;
		e += 2;
	}
	*sp = e;
	return v;
}

/* The numbers of a matrix() or matrix3d(), n at most */
static int tr_numbers(const char *s, float *v, int n)
{
	int k = 0;
	char *e;

	while (k < n && *s && *s != ')') {
		while (*s == ' ' || *s == ',')
			s++;
		v[k] = strtof(s, &e);
		if (e == s)
			break;
		s = e;
		k++;
	}
	return k;
}

/*
 * The translation transform gives b: translate(), translateX(),
 * translateY(), translate3d(), and the translation of matrix() and
 * matrix3d(); percentages of the border box. Scales and rotations are
 * left out: the box keeps its size, moves only.
 */
static void transform_offset(const struct box *b, float *dx, float *dy)
{
	const char *s = b->style ? b->style->v[P_TRANSFORM].u.str : NULL;
	float bw = b->w + hedges(b), bh = b->h + vedges(b), m[16];

	*dx = *dy = 0;
	if (!s || !strcmp(s, "none"))
		return;
	while (*s) {
		const char *name = s, *a;
		size_t n;

		while (*s && *s != '(')
			s++;
		if (!*s)
			break;
		while (name < s && (*name == ' ' || *name == ','))
			name++;
		n = (size_t)(s - name);
		a = ++s;
		while (*a == ' ')
			a++;
		if ((n == 9 && !strncmp(name, "translate", 9)) ||
		    (n == 11 && !strncmp(name, "translate3d", 11))) {
			*dx += tr_len(&a, b, bw);
			while (*a == ' ' || *a == ',')
				a++;
			if (*a != ')')
				*dy += tr_len(&a, b, bh);
		} else if (n == 10 && !strncmp(name, "translateX", 10)) {
			*dx += tr_len(&a, b, bw);
		} else if (n == 10 && !strncmp(name, "translateY", 10)) {
			*dy += tr_len(&a, b, bh);
		} else if (n == 6 && !strncmp(name, "matrix", 6)) {
			if (tr_numbers(a, m, 6) == 6) {
				*dx += m[4] * doc_zoom();
				*dy += m[5] * doc_zoom();
			}
		} else if (n == 8 && !strncmp(name, "matrix3d", 8)) {
			if (tr_numbers(a, m, 16) == 16) {
				*dx += m[12] * doc_zoom();
				*dy += m[13] * doc_zoom();
			}
		}
		while (*s && *s != ')')
			s++;
		if (*s)
			s++;
	}
}

/* Where position: relative and transform move b */
void rel_offset(const struct box *b, float cbw, float *dx, float *dy)
{
	float tx, ty;

	transform_offset(b, &tx, &ty);
	rel_position(b, cbw, dx, dy);
	*dx += tx;
	*dy += ty;
}

/* position: relative and sticky offsets alone */
static void rel_position(const struct box *b, float cbw, float *dx,
	float *dy)
{
	const struct style *st = b->style;

	*dx = *dy = 0;
	if (!kw(st, P_POSITION, "relative") && !kw(st, P_POSITION, "sticky"))
		return;
	if (!is_auto(st, P_LEFT))
		*dx = len_val(&st->v[P_LEFT], cbw);
	else if (!is_auto(st, P_RIGHT))
		*dx = -len_val(&st->v[P_RIGHT], cbw);
	if (!is_auto(st, P_TOP))
		*dy = len_val(&st->v[P_TOP], 0);
	else if (!is_auto(st, P_BOTTOM))
		*dy = -len_val(&st->v[P_BOTTOM], 0);
	if (kw(st, P_POSITION, "sticky"))
		*dx = *dy = 0;	/* sticky needs scrolling: static here */
}

/* Form controls, <audio> and <iframe> have a size, not a ratio: a
 * button given a height keeps its width. */
static int has_ratio(const struct box *b)
{
	const soie_node *n = b->node;
	const char *t;

	if (!n || n->ns != SOIE_NS_HTML)
		return 1;
	if (!strcmp(n->name, "input")) {
		t = soie_element_get_attr(n, "type");
		return t && ascii_ieq(t, "image");
	}
	return strcmp(n->name, "textarea") && strcmp(n->name, "select") &&
		strcmp(n->name, "audio") && strcmp(n->name, "iframe");
}

/* Replaced elements: CSS 2 10.3.2 and 10.6.2 */
static void replaced_size(struct box *b, float cbw, float cbh)
{
	float w = spec_width(b, P_WIDTH, cbw), h = spec_height(b, P_HEIGHT,
		cbh);
	float iw = b->iw > 0 ? b->iw : 0, ih = b->ih > 0 ? b->ih : 0;

	if (w < 0 && h < 0) {
		w = iw;
		h = ih;
	} else if (w < 0) {
		w = ih > 0 && has_ratio(b) ? h * iw / ih : iw;
	} else if (h < 0) {
		h = iw > 0 && has_ratio(b) ? w * ih / iw : ih;
	}
	b->w = clamp_w(b, w, cbw);
	b->h = clamp_h(b, h, cbh);
}

/*
 * text-align: -webkit-center (<center>, <td align=center>) centers block
 * children too, unless they set their margins.
 */
static int webkit_center(const struct box *b)
{
	const struct style *ps = b->parent ? b->parent->style : NULL;

	return ps && kw(ps, P_TEXT_ALIGN, "-webkit-center") &&
		!b->m[1] && !b->m[3];
}

/* Width and horizontal margins in flow (CSS 2 10.3.3). */
static void block_width(struct box *b, float avail, float cbw)
{
	const struct style *st = b->style;
	int center = webkit_center(b);
	int ml_auto = is_auto(st, P_MARGIN_LEFT) || center;
	int mr_auto = is_auto(st, P_MARGIN_RIGHT) || center;
	float w = spec_width(b, P_WIDTH, cbw), e = hedges(b), rest;

	if (b->type == B_REPLACED) {
		replaced_size(b, cbw, -1);
		w = b->w;
	} else if (w < 0 && is_auto(st, P_WIDTH) &&
		   !kw(st, P_WIDTH, "auto")) {
		/* min-content, max-content, fit-content */
		w = kw(st, P_WIDTH, "fit-content") ?
			shrink_to_fit(b, avail - e - b->m[1] - b->m[3]) : -1;
		if (w < 0) {
			float mn, mx;

			content_sizes(b, &mn, &mx);
			w = kw(st, P_WIDTH, "min-content") ? mn : mx;
		}
	}
	if (w < 0) {
		b->w = avail - b->m[1] - b->m[3] - e;
		if (b->w < 0)
			b->w = 0;
		w = clamp_w(b, b->w, cbw);
		if (w == b->w)
			return;
	}
	b->w = clamp_w(b, w, cbw);
	rest = avail - b->w - e;
	if (ml_auto && mr_auto) {
		b->m[3] = b->m[1] = rest > 0 ? rest / 2 : 0;
		if (rest < 0)
			b->m[1] = rest;
	} else if (ml_auto) {
		b->m[3] = rest - b->m[1];
	} else {
		b->m[1] = rest - b->m[3];	/* over-constrained: ltr */
	}
}

static int clear_side(const struct style *st)
{
	if (kw(st, P_CLEAR, "left") || kw(st, P_CLEAR, "inline-start"))
		return 'l';
	if (kw(st, P_CLEAR, "right") || kw(st, P_CLEAR, "inline-end"))
		return 'r';
	if (kw(st, P_CLEAR, "both"))
		return 'b';
	return 0;
}

/*
 * One in-flow block-level box placed at flow position y with pending
 * margins s. Returns the flow position after it; s holds its bottom
 * margin (collapsed with what came out of it).
 */
static float layout_block(struct box *b, struct bfc *f, float cbx, float cbw,
	float cbh, float y, struct strut *s)
{
	int bfc = establishes_bfc(b), side = clear_side(b->style);
	int ctop, cbot;
	float avail_x = cbx, avail_w = cbw, hspec, h, top, dx, dy;

	struct bfc own;

	edges(b, cbw);
	strut_add(s, b->m[0]);
	if (side) {
		float cy = bfc_clear(f, side);

		if (cy > y + strut_val(s)) {
			y = cy;
			s->pos = s->neg = 0;
		}
	}
	if (bfc && f && f->n) {
		/* BFC roots sit beside floats */
		float ty = y + strut_val(s);

		avail_x = bfc_left(f, ty, 1, cbx);
		avail_w = bfc_right(f, ty, 1, cbx + cbw) - avail_x;
	}
	if (b->role == R_TABLE) {
		/* tables size themselves, then sit like a fixed width */
		float ml = is_auto(b->style, P_MARGIN_LEFT) ? 0 : b->m[3];
		float mr = is_auto(b->style, P_MARGIN_RIGHT) ? 0 : b->m[1];
		float rest, ty = y + strut_val(s);

		s->pos = s->neg = 0;
		layout_table(b, avail_w - ml - mr - hedges(b));
		rest = avail_w - b->w - hedges(b);
		if ((is_auto(b->style, P_MARGIN_LEFT) &&
		     is_auto(b->style, P_MARGIN_RIGHT)) || webkit_center(b))
			b->m[3] = b->m[1] = rest > 0 ? rest / 2 : 0;
		else if (is_auto(b->style, P_MARGIN_LEFT))
			b->m[3] = rest - b->m[1];
		translate(b, avail_x + b->m[3] + b->b[3] + b->p[3],
			ty + b->b[0] + b->p[0]);
		strut_add(s, b->m[2]);
		rel_offset(b, cbw, &dx, &dy);
		if (dx || dy)
			translate(b, dx, dy);
		return b->y - dy + b->h + b->p[2] + b->b[2];
	}
	block_width(b, avail_w, cbw);
	b->x = avail_x + b->m[3] + b->b[3] + b->p[3];
	ctop = !bfc && b->type == B_BLOCK && !b->b[0] && !b->p[0];
	hspec = b->type == B_REPLACED ? b->h : spec_height(b, P_HEIGHT, cbh);
	cbot = !bfc && b->type == B_BLOCK && hspec < 0 && !b->b[2] && !b->p[2];
	if (b->type == B_REPLACED) {
		top = y + strut_val(s);
		s->pos = s->neg = 0;
		b->y = top + b->b[0] + b->p[0];
		strut_add(s, b->m[2]);
		return b->y + b->h + b->p[2] + b->b[2];
	}
	if (ctop) {
		/* the margins above may collapse with the first child's */
		top = y;
	} else {
		top = y + strut_val(s);
		s->pos = s->neg = 0;
		top += b->b[0] + b->p[0];
	}
	memset(&own, 0, sizeof(own));
	h = layout_contents(b, bfc ? &own : f, top, s, ctop,
		hspec >= 0 ? hspec : -1);
	if (!cbot || hspec >= 0) {
		/* the bottom edge keeps the last child's margin inside */
		if (!(b->flags & BF_INLINE_KIDS))
			h += strut_val(s);
		s->pos = s->neg = 0;
	}
	if (bfc && own.n) {
		/* a BFC root contains its floats */
		float fb = bfc_bottom(&own) - b->y;

		if (fb > h)
			h = fb;
	}
	free(own.v);
	b->h = clamp_h(b, hspec >= 0 ? hspec : h, cbh);
	if (b->h != h)
		s->pos = s->neg = 0;
	strut_add(s, b->m[2]);
	if (ctop && cbot && !b->h && !b->nlines) {
		/* empty: its margins collapsed through, the flow stays */
		b->y = y + strut_val(s);
		return y;
	}
	rel_offset(b, cbw, &dx, &dy);
	if (dx || dy)
		translate(b, dx, dy);
	return b->y - dy + b->h + b->p[2] + b->b[2];
}

/* Children of a block container with block children. */
static float layout_flow(struct box *b, struct bfc *f, float y,
	struct strut *s, float cbh)
{
	struct box *c;

	for (c = b->first; c; c = c->next) {
		if (c->flags & BF_ABS) {
			edges(c, b->w);
			abs_static(c, b->x, y + strut_val(s));
			continue;
		}
		if (c->flags & BF_FLOAT) {
			place_float(c, f, y + strut_val(s), b->x, b->x + b->w,
				b->w);
			continue;
		}
		y = layout_block(c, f, b->x, b->w, cbh, y, s);
	}
	return y;
}

/* Floats (CSS 2 9.5.1, simplified): as high as possible, then sideways. */
void place_float(struct box *b, struct bfc *f, float y, float x0, float x1,
	float cbw)
{
	int left = !kw(b->style, P_FLOAT, "right") &&
		!kw(b->style, P_FLOAT, "inline-end");
	int side = clear_side(b->style);
	float w, mw, mh, l, r, cy;
	struct flt *t;

	edges(b, cbw);
	if (b->type == B_REPLACED) {
		replaced_size(b, cbw, -1);
		w = b->w;
	} else {
		w = spec_width(b, P_WIDTH, cbw);
		if (w < 0)
			w = shrink_to_fit(b, cbw - b->m[1] - b->m[3] -
				hedges(b));
		w = clamp_w(b, w, cbw);
	}
	mw = w + hedges(b) + b->m[1] + b->m[3];
	layout_alone(b, 0, 0, w, -1, -1);
	mh = b->h + vedges(b) + b->m[0] + b->m[2];
	if (side && (cy = bfc_clear(f, side)) > y)
		y = cy;
	/* not above earlier floats */
	if (f && f->n && f->v[f->n - 1].y > y)
		y = f->v[f->n - 1].y;
	for (;;) {
		l = bfc_left(f, y, mh, x0);
		r = bfc_right(f, y, mh, x1);
		if (r - l >= mw || (l == x0 && r == x1))
			break;
		/* move below the float ending first */
		{
			float next = 1e30f;
			int i;

			for (i = 0; i < f->n; i++)
				if (f->v[i].y + f->v[i].h > y &&
				    f->v[i].y + f->v[i].h < next)
					next = f->v[i].y + f->v[i].h;
			if (next >= 1e30f)
				break;
			y = next;
		}
	}
	translate(b, (left ? l : r - mw) + b->m[3] + b->b[3] + b->p[3] - b->x,
		y + b->m[0] + b->b[0] + b->p[0] - b->y);
	if (f->n == f->cap) {
		f->cap = f->cap ? f->cap * 2 : 8;
		f->v = xrealloc(f->v, (size_t)f->cap * sizeof(*f->v));
	}
	t = &f->v[f->n++];
	t->x = left ? l : r - mw;
	t->y = y;
	t->w = mw;
	t->h = mh;
	t->left = left;
	{
		float dx, dy;

		rel_offset(b, cbw, &dx, &dy);
		if (dx || dy)
			translate(b, dx, dy);
	}
}

/*
 * A box that starts a new formatting context, at content position
 * (x, y) with content width w: floats, inline-blocks, abs boxes. defh,
 * when not -1, is its height when height is auto but set from outside
 * (an abs box between its top and bottom, a stretched flex or grid
 * item): definite for its children.
 */
static void layout_alone(struct box *b, float x, float y, float w,
	float cbh, float defh)
{
	struct bfc own;
	struct strut s = { 0, 0 };
	float h, hspec;
	int first;

	if (b->type == B_REPLACED) {
		b->x = x;
		b->y = y;
		return;
	}
	if (b->role == R_TABLE) {
		layout_table(b, w);
		translate(b, x, y);
		return;
	}
	if (b->lok && b->lw == w && b->lcbh == cbh && b->ldefh == defh &&
	    !g.trial) {
		/* laid out the same way before (incremental layout): its
		 * subtree is where lx, ly say (abs_static() set x, y) */
		int i;

		translate(b, x - b->lx, y - b->ly);
		b->x = x;
		b->y = y;
		b->w = w;	/* flex and grid may have set them since */
		b->h = b->lh;
		for (i = 0; i < b->nabsv; i++)
			abs_add(b->absv[i]);
		return;
	}
	b->x = x;
	b->y = y;
	b->w = w;
	hspec = spec_height(b, P_HEIGHT, cbh);
	if (hspec < 0)
		hspec = defh;
	memset(&own, 0, sizeof(own));
	first = g.nlog;
	h = layout_contents(b, &own, y, &s, 0, hspec);
	/* the absolute boxes it holds, for the times it only moves */
	free(b->absv);
	b->absv = NULL;
	b->nabsv = 0;
	if (g.nlog > first) {
		int i, k;

		b->absv = xmalloc((size_t)(g.nlog - first) * sizeof(*b->absv));
		for (i = first; i < g.nlog; i++) {
			for (k = 0; k < b->nabsv && b->absv[k] != g.log[i]; k++)
				;
			if (k == b->nabsv)
				b->absv[b->nabsv++] = g.log[i];
		}
	}
	if (!(b->flags & BF_INLINE_KIDS))
		h += strut_val(&s);
	if (own.n && bfc_bottom(&own) - y > h)
		h = bfc_bottom(&own) - y;
	free(own.v);
	b->h = clamp_h(b, hspec >= 0 ? hspec : h, cbh);
	b->lok = !g.trial;	/* without its absolute boxes */
	b->lw = w;
	b->lcbh = cbh;
	b->ldefh = defh;
	b->lh = b->h;
	b->lx = b->x;
	b->ly = b->y;
}

/* A table cell or caption: a block formatting context of width w. */
void layout_cell(struct box *c, float x, float y, float w)
{
	edges(c, w);
	if (c->role == R_CAPTION) {
		float cw = w - c->m[1] - c->m[3] - hedges(c);

		layout_alone(c, x + c->m[3] + c->b[3] + c->p[3],
			y + c->m[0] + c->b[0] + c->p[0], cw < 0 ? 0 : cw, -1,
			-1);
		return;
	}
	c->m[0] = c->m[1] = c->m[2] = c->m[3] = 0;
	w -= hedges(c);
	layout_alone(c, x + c->b[3] + c->p[3], y + c->b[0] + c->p[0],
		w < 0 ? 0 : w, -1, -1);
}

/* For flex and grid */

float box_spec_w(const struct box *b, int prop, float cbw)
{
	return spec_width(b, prop, cbw);
}

float box_spec_h(const struct box *b, int prop, float cbh)
{
	return spec_height(b, prop, cbh);
}

float box_clamp_w(const struct box *b, float w, float cbw)
{
	return clamp_w(b, w, cbw);
}

float box_clamp_h(const struct box *b, float h, float cbh)
{
	return clamp_h(b, h, cbh);
}

/*
 * A flex or grid item: content box at (x, y), width w; h >= 0 forces
 * the content height (stretch, definite sizes). Edges must be set.
 */
void layout_item(struct box *b, float x, float y, float w, float h)
{
	if (b->type == B_REPLACED) {
		b->x = x;
		b->y = y;
		b->w = w;
		if (h >= 0)
			b->h = h;
		return;
	}
	layout_alone(b, x, y, w < 0 ? 0 : w, -1, h);
	if (h >= 0)
		b->h = h;
}

/* Inline-blocks and inline replaced boxes, at the origin. */
void layout_atomic(struct box *b, float cbw, float cbh)
{
	float w;

	edges(b, cbw);
	if (b->type == B_REPLACED) {
		replaced_size(b, cbw, cbh);
		b->x = b->m[3] + b->b[3] + b->p[3];
		b->y = b->m[0] + b->b[0] + b->p[0];
		return;
	}
	w = spec_width(b, P_WIDTH, cbw);
	if (w < 0)
		w = shrink_to_fit(b, cbw - b->m[1] - b->m[3] - hedges(b));
	w = clamp_w(b, w, cbw);
	layout_alone(b, b->m[3] + b->b[3] + b->p[3],
		b->m[0] + b->b[0] + b->p[0], w, cbh, -1);
}

/* Absolute positioning (CSS 2 10.3.7, 10.6.4, simplified). */
static void layout_abs(struct box *b)
{
	const struct style *st = b->style;
	struct box *cb;
	float cx = 0, cy = 0, cw = g.l->width, ch = g.l->height;
	float sx = b->sx, sy = b->sy, w, x, y, l, r, t, bt, e, h;
	int fixed = kw(st, P_POSITION, "fixed");

	for (cb = b->parent; cb && !fixed; cb = cb->parent)
		if (cb->type != B_INLINE && cb->style &&
		    !kw(cb->style, P_POSITION, "static")) {
			cx = cb->x - cb->p[3];
			cy = cb->y - cb->p[0];
			cw = cb->w + cb->p[1] + cb->p[3];
			ch = cb->h + cb->p[0] + cb->p[2];
			break;
		}
	edges(b, cw);
	e = hedges(b);
	l = is_auto(st, P_LEFT) ? -1e30f : len_val(&st->v[P_LEFT], cw);
	r = is_auto(st, P_RIGHT) ? -1e30f : len_val(&st->v[P_RIGHT], cw);
	t = is_auto(st, P_TOP) ? -1e30f : len_val(&st->v[P_TOP], ch);
	bt = is_auto(st, P_BOTTOM) ? -1e30f : len_val(&st->v[P_BOTTOM], ch);
	if (b->type == B_REPLACED) {
		replaced_size(b, cw, ch);
		w = b->w;
	} else {
		w = spec_width(b, P_WIDTH, cw);
		if (w < 0 && l > -1e29f && r > -1e29f)
			w = cw - l - r - e - b->m[1] - b->m[3];
		else if (w < 0)
			w = shrink_to_fit(b, cw - (l > -1e29f ? l : 0) -
				(r > -1e29f ? r : 0) - e - b->m[1] - b->m[3]);
		w = clamp_w(b, w < 0 ? 0 : w, cw);
	}
	if (l > -1e29f)
		x = cx + l + b->m[3];
	else if (r > -1e29f)
		x = cx + cw - r - b->m[1] - w - e;
	else
		x = sx + b->m[3];
	x += b->b[3] + b->p[3];
	y = t > -1e29f ? cy + t + b->m[0] : sy + b->m[0];
	y += b->b[0] + b->p[0];
	if (b->type == B_REPLACED) {
		b->x = x;
		b->y = y;
	} else {
		float hspec = spec_height(b, P_HEIGHT, ch);

		if (hspec < 0 && t > -1e29f && bt > -1e29f)
			hspec = ch - t - bt - vedges(b) - b->m[0] - b->m[2];
		layout_alone(b, x, y, w, ch, hspec >= 0 ? hspec : -1);
		if (hspec >= 0)
			b->h = clamp_h(b, hspec, ch);
	}
	h = b->h + vedges(b);
	if (t <= -1e29f && bt > -1e29f)
		translate(b, 0, cy + ch - bt - b->m[2] - h + b->b[0] + b->p[0] -
			b->y);
	{
		float tx, ty;

		transform_offset(b, &tx, &ty);
		if (tx || ty)
			translate(b, tx, ty);
	}
}

void layout_root(struct layout *l)
{
	struct box *root = l->root;
	struct bfc f;
	struct strut s = { 0, 0 };
	float bottom;
	int i;

	g.l = l;
	g.nabs = 0;
	g.pass++;
	memset(&f, 0, sizeof(f));
	bottom = layout_block(root, &f, 0, l->width, l->height, 0, &s);
	bottom += strut_val(&s);
	if (bfc_bottom(&f) > bottom)
		bottom = bfc_bottom(&f);
	free(f.v);
	/* abs boxes found while laying out abs boxes join the list */
	for (i = 0; i < g.nabs; i++) {
		float b;

		layout_abs(g.abs[i]);
		b = g.abs[i]->y + g.abs[i]->h + g.abs[i]->p[2] +
			g.abs[i]->b[2] + g.abs[i]->m[2];
		if (b > bottom)
			bottom = b;
	}
	free(g.abs);
	g.abs = NULL;
	g.nabs = g.cap = 0;
	free(g.log);
	g.log = NULL;
	g.nlog = g.caplog = 0;
	free(g.saved);
	g.saved = NULL;
	g.nsaved = g.capsaved = 0;
	l->doc_height = bottom;
}
