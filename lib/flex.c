/*
 * Flexbox (CSS Flexible Box Layout 1, section 9): order, flex base
 * sizes, lines, the flexible lengths with frozen violations, auto
 * margins, justify-content, align-items/self, align-content and gaps.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "layout.h"

struct item {
	struct box *b;
	int order, idx;
	float base, hypo;	/* main sizes (content box) */
	float mn, mx;		/* main min/max */
	float main;		/* resolved main size */
	float cross;		/* content cross size */
	float mbefore, mafter;	/* main axis margins + edges */
	float cbefore, cafter;	/* cross axis margins + edges */
	float grow, shrink;
	int frozen;
	int auto_ms, auto_me, auto_cs, auto_ce;	/* auto margins */
};

static int by_order(const void *a, const void *b)
{
	const struct item *x = a, *y = b;

	return x->order != y->order ? (x->order < y->order ? -1 : 1) :
		x->idx - y->idx;
}

/* Stretched across the line: boxes, and form controls (replaced here,
 * not in CSS); images and the like keep their size */
static int stretches(const struct box *b)
{
	return b->type != B_REPLACED || (b->node &&
		(is_html(b->node, TAG_INPUT) || is_html(b->node, TAG_SELECT) ||
		 is_html(b->node, TAG_TEXTAREA)));
}

static int is_row(const struct style *st)
{
	return kw(st, P_FLEX_DIRECTION, "row") ||
		kw(st, P_FLEX_DIRECTION, "row-reverse");
}

static int is_reverse(const struct style *st)
{
	return kw(st, P_FLEX_DIRECTION, "row-reverse") ||
		kw(st, P_FLEX_DIRECTION, "column-reverse");
}

/* align-self, align-items, justify-content: text keywords. */
static int align_of(const char *s, const char *def)
{
	if (!s || !strcmp(s, "normal") || !strcmp(s, "auto"))
		s = def;
	if (strstr(s, "center"))
		return 'c';
	if (strstr(s, "flex-end") || !strcmp(s, "end") || strstr(s, "self-end") ||
	    !strcmp(s, "right") || strstr(s, "last baseline"))
		return 'e';
	if (strstr(s, "stretch"))
		return 's';
	if (strstr(s, "space-between"))
		return 'b';
	if (strstr(s, "space-around"))
		return 'a';
	if (strstr(s, "space-evenly"))
		return 'v';
	return 'f';	/* start, flex-start, baseline, left */
}

static float gap(const struct style *st, int prop, float ref)
{
	const struct cval *v = &st->v[prop];

	return v->kw ? 0 : len_val(v, ref);
}

static float max_content_height(struct box *b, float w)
{
	layout_item(b, 0, 0, w, -1);
	return b->h;
}

/* Flex base size and hypothetical main size (9.2.3). */
static void base_size(struct item *it, int row, float W, float H)
{
	struct box *b = it->b;
	const struct style *st = b->style;
	const struct cval *fb = &st->v[P_FLEX_BASIS];
	float mn, mx, sz = -1;

	if (!fb->kw)
		sz = len_val(fb, row ? W : (H >= 0 ? H : 0));
	if (sz >= 0 && kw(st, P_BOX_SIZING, "border-box"))
		sz -= row ? it->mbefore + it->mafter -
			(b->m[3] + b->m[1]) : 0;
	if (sz < 0 || (fb->kw && kw(st, P_FLEX_BASIS, "auto")))
		sz = row ? box_spec_w(b, P_WIDTH, W) :
			box_spec_h(b, P_HEIGHT, H);
	if (sz < 0) {
		/* content: max-content in the main axis */
		content_sizes(b, &mn, &mx);
		if (row) {
			sz = mx;
		} else {
			float cw = box_spec_w(b, P_WIDTH, W);

			if (cw < 0)
				cw = W - it->cbefore - it->cafter;
			sz = b->type == B_REPLACED ? b->ih :
				max_content_height(b, cw);
		}
	}
	if (sz < 0)
		sz = 0;
	it->base = sz;
	/* automatic minimum size: the min-content size */
	if (row) {
		float smn = box_spec_w(b, P_MIN_WIDTH, W);

		if (smn < 0 && kw(st, P_OVERFLOW_X, "visible")) {
			content_sizes(b, &mn, &mx);
			smn = mn;
			if (box_spec_w(b, P_WIDTH, W) >= 0 &&
			    box_spec_w(b, P_WIDTH, W) < smn)
				smn = box_spec_w(b, P_WIDTH, W);
		}
		it->mn = smn < 0 ? 0 : smn;
		it->mx = box_spec_w(b, P_MAX_WIDTH, W);
	} else {
		float smn = box_spec_h(b, P_MIN_HEIGHT, H);

		it->mn = smn < 0 ? 0 : smn;
		it->mx = box_spec_h(b, P_MAX_HEIGHT, H);
	}
	if (it->mx < 0)
		it->mx = 1e30f;
	it->hypo = it->base < it->mn ? it->mn : it->base > it->mx ? it->mx :
		it->base;
}

static float outer_hypo(const struct item *it)
{
	return it->hypo + it->mbefore + it->mafter;
}

/* Resolve the flexible lengths of one line (9.7). */
static void flex_line(struct item *v, int n, float avail)
{
	float used = 0, free_space, sum;
	int i, grow, iter;

	for (i = 0; i < n; i++)
		used += outer_hypo(&v[i]);
	grow = used < avail;
	for (i = 0; i < n; i++) {
		struct item *it = &v[i];

		it->frozen = (grow ? it->grow == 0 || it->base > it->hypo :
			it->shrink == 0 || it->base < it->hypo);
		it->main = it->hypo;
	}
	for (iter = 0; iter < n + 1; iter++) {
		float total = 0, viol = 0;
		int active = 0;

		free_space = avail;
		for (i = 0; i < n; i++)
			free_space -= v[i].mbefore + v[i].mafter +
				(v[i].frozen ? v[i].main : v[i].base);
		sum = 0;
		for (i = 0; i < n; i++)
			if (!v[i].frozen) {
				sum += grow ? v[i].grow : v[i].shrink *
					v[i].base;
				active++;
			}
		if (!active)
			break;
		if (grow && sum < 1) {
			/* factors below 1 use only part of the space */
			float f = 0;

			for (i = 0; i < n; i++)
				if (!v[i].frozen)
					f += v[i].grow;
			if (free_space * f < free_space)
				free_space *= f;
		}
		for (i = 0; i < n; i++) {
			struct item *it = &v[i];
			float t;

			if (it->frozen)
				continue;
			if (grow)
				t = it->base + (sum > 0 ? free_space * it->grow /
					sum : 0);
			else
				t = it->base + (sum > 0 ? free_space *
					it->shrink * it->base / sum : 0);
			it->main = t;
			if (t < it->mn)
				it->main = it->mn;
			if (it->main > it->mx)
				it->main = it->mx;
			if (it->main < 0)
				it->main = 0;
			viol += it->main - t;
			total++;
		}
		/* freeze the violators, or everything if none */
		for (i = 0; i < n; i++) {
			struct item *it = &v[i];
			float t;

			if (it->frozen)
				continue;
			t = it->main;
			if (viol == 0 || (viol > 0 && t == it->mn) ||
			    (viol < 0 && t == it->mx))
				it->frozen = 1;
		}
		(void)total;
	}
}

float layout_flex(struct box *f, float cbh)
{
	const struct style *st = f->style;
	int row = is_row(st), rev = is_reverse(st);
	int wrap = !kw(st, P_FLEX_WRAP, "nowrap");
	float W = f->w, H = box_spec_h(f, P_HEIGHT, cbh);
	float main_avail, mgap, cgap, y, cross_total = 0, extra;
	struct item *v = NULL;
	struct box *c;
	int n = 0, i, start, nlines = 0, *lstart = NULL;
	float *lcross = NULL;
	int jc = align_of(st->v[P_JUSTIFY_CONTENT].u.str, "flex-start");
	int ac = align_of(st->v[P_ALIGN_CONTENT].u.str, "stretch");

	for (c = f->first; c; c = c->next) {
		struct item *it;

		if (c->flags & BF_ABS) {
			box_edges(c, W);
			abs_static(c, f->x, f->y);
			continue;
		}
		v = xrealloc(v, (size_t)(n + 1) * sizeof(*v));
		it = &v[n];
		memset(it, 0, sizeof(*it));
		it->b = c;
		it->idx = n;
		it->order = (int)c->style->v[P_ORDER].u.num;
		box_edges(c, W);
		it->grow = (float)c->style->v[P_FLEX_GROW].u.num;
		it->shrink = (float)c->style->v[P_FLEX_SHRINK].u.num;
		if (row) {
			it->mbefore = c->m[3] + c->b[3] + c->p[3];
			it->mafter = c->m[1] + c->b[1] + c->p[1];
			it->cbefore = c->m[0] + c->b[0] + c->p[0];
			it->cafter = c->m[2] + c->b[2] + c->p[2];
			it->auto_ms = is_auto(c->style, P_MARGIN_LEFT);
			it->auto_me = is_auto(c->style, P_MARGIN_RIGHT);
			it->auto_cs = is_auto(c->style, P_MARGIN_TOP);
			it->auto_ce = is_auto(c->style, P_MARGIN_BOTTOM);
		} else {
			it->mbefore = c->m[0] + c->b[0] + c->p[0];
			it->mafter = c->m[2] + c->b[2] + c->p[2];
			it->cbefore = c->m[3] + c->b[3] + c->p[3];
			it->cafter = c->m[1] + c->b[1] + c->p[1];
			it->auto_ms = is_auto(c->style, P_MARGIN_TOP);
			it->auto_me = is_auto(c->style, P_MARGIN_BOTTOM);
			it->auto_cs = is_auto(c->style, P_MARGIN_LEFT);
			it->auto_ce = is_auto(c->style, P_MARGIN_RIGHT);
		}
		n++;
	}
	qsort(v, (size_t)n, sizeof(*v), by_order);
	mgap = gap(st, row ? P_COLUMN_GAP : P_ROW_GAP, row ? W : 0);
	cgap = gap(st, row ? P_ROW_GAP : P_COLUMN_GAP, row ? 0 : W);
	main_avail = row ? W : (H >= 0 ? H : 1e30f);
	for (i = 0; i < n; i++)
		base_size(&v[i], row, W, H);
	/* lines */
	lstart = xmalloc((size_t)(n + 2) * sizeof(*lstart));
	lcross = xmalloc((size_t)(n + 2) * sizeof(*lcross));
	start = 0;
	while (start < n || (!n && !nlines)) {
		float used = 0;
		int end = start;

		while (end < n) {
			float o = outer_hypo(&v[end]) + (end > start ? mgap : 0);

			if (wrap && end > start && used + o > main_avail)
				break;
			used += o;
			end++;
		}
		lstart[nlines++] = start;
		if (end == start)
			break;
		{
			float avail = main_avail >= 1e29f ? used : main_avail;

			flex_line(v + start, end - start, avail -
				mgap * (float)(end - start - 1));
		}
		start = end;
	}
	lstart[nlines] = n;
	/* cross sizes: lay items out at their main size */
	for (i = 0; i < n; i++) {
		struct item *it = &v[i];
		struct box *b = it->b;

		if (row) {
			float hs = box_spec_h(b, P_HEIGHT, H);

			layout_item(b, 0, 0, it->main, hs);
			it->cross = b->h;
		} else {
			float ws = box_spec_w(b, P_WIDTH, W);

			/* auto: the intrinsic width of a replaced box, or the
			 * line's (stretched later) */
			if (ws < 0 && b->type == B_REPLACED && b->iw > 0)
				ws = b->iw;
			else if (ws < 0)
				ws = W - it->cbefore - it->cafter;
			ws = box_clamp_w(b, ws, W);
			layout_item(b, 0, 0, ws, it->main);
			it->cross = b->w;
		}
	}
	for (i = 0; i < nlines; i++) {
		int k;

		lcross[i] = 0;
		for (k = lstart[i]; k < lstart[i + 1]; k++) {
			float o = v[k].cross + v[k].cbefore + v[k].cafter;

			if (o > lcross[i])
				lcross[i] = o;
		}
		cross_total += lcross[i];
	}
	cross_total += cgap * (float)(nlines - 1);
	{
		/* a definite cross size: single lines fill it */
		float cross_avail = row ? H : W;

		if (cross_avail >= 0 && nlines == 1 && !wrap)
			lcross[0] = cross_avail, cross_total = cross_avail;
		else if (cross_avail >= 0 && ac == 's' && nlines &&
			 cross_avail > cross_total) {
			float add = (cross_avail - cross_total) / (float)nlines;

			for (i = 0; i < nlines; i++)
				lcross[i] += add;
			cross_total = cross_avail;
		}
		extra = cross_avail >= 0 ? cross_avail - cross_total : 0;
	}
	/* place */
	y = 0;
	if (ac == 'c')
		y = extra / 2;
	else if (ac == 'e')
		y = extra;
	for (i = 0; i < nlines; i++) {
		int a = lstart[i], z = lstart[i + 1], k, autos = 0;
		float used = 0, free_space, pos, space = 0, lineavail;

		for (k = a; k < z; k++) {
			used += v[k].main + v[k].mbefore + v[k].mafter;
			autos += v[k].auto_ms + v[k].auto_me;
		}
		used += mgap * (float)(z - a - 1 > 0 ? z - a - 1 : 0);
		lineavail = row ? W : (H >= 0 ? H : used);
		free_space = lineavail - used;
		pos = 0;
		if (free_space > 0 && autos) {
			float each = free_space / (float)autos;

			for (k = a; k < z; k++) {
				if (v[k].auto_ms)
					v[k].mbefore += each;
				if (v[k].auto_me)
					v[k].mafter += each;
			}
			free_space = 0;
		}
		if (free_space > 0 || jc == 'c' || jc == 'e') {
			switch (jc) {
			case 'e': pos = free_space; break;
			case 'c': pos = free_space / 2; break;
			case 'b':
				space = z - a > 1 ? free_space /
					(float)(z - a - 1) : 0;
				break;
			case 'a':
				space = free_space / (float)(z - a);
				pos = space / 2;
				break;
			case 'v':
				space = free_space / (float)(z - a + 1);
				pos = space;
				break;
			}
		}
		for (k = a; k < z; k++) {
			int idx = rev ? z - 1 - (k - a) : k;
			struct item *it = &v[idx];
			struct box *b = it->b;
			const char *as = b->style->v[P_ALIGN_SELF].u.str;
			int al = align_of(as && strcmp(as, "auto") ? as :
				st->v[P_ALIGN_ITEMS].u.str, "stretch");
			float cpos = 0, cross = it->cross, mx, my, dx, dy;

			if (al == 's' && (row ? is_auto(b->style, P_HEIGHT) :
			    is_auto(b->style, P_WIDTH)) && !it->auto_cs &&
			    !it->auto_ce && stretches(b)) {
				cross = lcross[i] - it->cbefore - it->cafter;
				if (row)
					cross = box_clamp_h(b, cross, H);
				else
					cross = box_clamp_w(b, cross, W);
				if (cross < 0)
					cross = 0;
				if (!row) {
					/* a new width: lay out again */
					layout_item(b, 0, 0, cross, it->main);
				}
			} else if (it->auto_cs && it->auto_ce) {
				cpos = (lcross[i] - cross - it->cbefore -
					it->cafter) / 2;
			} else if (it->auto_cs || al == 'e') {
				cpos = lcross[i] - cross - it->cbefore -
					it->cafter;
			} else if (al == 'c') {
				cpos = (lcross[i] - cross - it->cbefore -
					it->cafter) / 2;
			}
			if (row) {
				b->h = cross;
				mx = f->x + pos + it->mbefore;
				my = f->y + y + cpos + it->cbefore;
			} else {
				b->w = cross;
				mx = f->x + y + cpos + it->cbefore;
				my = f->y + pos + it->mbefore;
				b->h = it->main;
			}
			dx = mx - b->x;
			dy = my - b->y;
			translate(b, dx, dy);
			{
				float rx, ry;

				rel_offset(b, W, &rx, &ry);
				if (rx || ry)
					translate(b, rx, ry);
			}
			pos += it->main + it->mbefore + it->mafter + mgap +
				space;
		}
		y += lcross[i] + cgap;
		if (ac == 'b' && nlines > 1 && extra > 0)
			y += extra / (float)(nlines - 1);
		else if (ac == 'a' && extra > 0)
			y += extra / (float)nlines;
	}
	{
		float h;

		if (row) {
			h = cross_total + (extra > 0 && H >= 0 ? extra : 0);
		} else {
			float used = 0;

			for (i = 0; i < n; i++)
				used += v[i].main + v[i].mbefore + v[i].mafter;
			used += mgap * (float)(n > 1 ? n - 1 : 0);
			h = H >= 0 ? H : used;
		}
		free(v);
		free(lstart);
		free(lcross);
		return h;
	}
}

/* Intrinsic widths: a row puts items side by side. */
void flex_sizes(struct box *f, float *mn, float *mx)
{
	struct box *c;
	int row = is_row(f->style), wrap = !kw(f->style, P_FLEX_WRAP,
		"nowrap"), n = 0;
	float g = gap(f->style, P_COLUMN_GAP, 0);

	*mn = *mx = 0;
	for (c = f->first; c; c = c->next) {
		float a, b;

		if (c->flags & BF_ABS)
			continue;
		box_sizes(c, &a, &b);
		if (row) {
			*mx += b + (n ? g : 0);
			if (wrap)
				*mn = a > *mn ? a : *mn;
			else
				*mn += a + (n ? g : 0);
		} else {
			*mx = b > *mx ? b : *mx;
			*mn = a > *mn ? a : *mn;
		}
		n++;
	}
}
