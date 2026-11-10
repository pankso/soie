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
	int bl;			/* baseline aligned: 1 first, 2 last */
	float asc;		/* its margin box top to that baseline */
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

/* An item aligned by its baseline: it->bl and it->asc, the baseline
 * synthesized from the border box's bottom when it has none; in a
 * column, from its left edge (right for the last baseline), the text
 * running across the cross axis */
static int baseline_of(struct item *it, const struct style *fst, int row)
{
	const struct box *b = it->b;
	const char *s = b->style->v[P_ALIGN_SELF].u.str;
	float y;

	if (!s || !strcmp(s, "auto"))
		s = fst->v[P_ALIGN_ITEMS].u.str;
	it->bl = 0;
	if (!s || !strstr(s, "baseline"))
		return 0;
	it->bl = strstr(s, "last") ? 2 : 1;
	if (!row) {
		it->asc = it->bl == 1 ? b->m[3] : it->cbefore + it->cross +
			b->p[1] + b->b[1];
		return 1;
	}
	y = box_baseline(b, it->bl == 2);
	it->asc = y > -1e29f ? it->cbefore + y - b->y :
		it->cbefore + it->cross + b->p[2] + b->b[2];
	return 1;
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
/* A scroll container in that axis: overflow other than visible and
 * clip */
static int scrolls(const struct style *st, int prop)
{
	return !kw(st, prop, "visible") && !kw(st, prop, "clip");
}

/* The width of a column's item: its own, else the line's when it
 * stretches, else fit-content (an item aligned center, end or by its
 * baseline was as wide as the line, nothing left to align) */
static float column_width(const struct item *it, const struct style *fst,
	float W)
{
	struct box *b = it->b;
	const char *as = b->style->v[P_ALIGN_SELF].u.str;
	float w = box_spec_w(b, P_WIDTH, W), avail = W - it->cbefore -
		it->cafter, mn, mx;

	if (w >= 0)
		return box_clamp_w(b, w, W);
	if (b->type == B_REPLACED && b->iw > 0)
		return box_clamp_w(b, b->iw, W);
	if (b->type != B_REPLACED && !it->auto_cs && !it->auto_ce &&
	    align_of(as && strcmp(as, "auto") ? as :
		fst->v[P_ALIGN_ITEMS].u.str, "stretch") == 's')
		return box_clamp_w(b, avail, W);
	content_sizes(b, &mn, &mx);
	w = mx < avail ? mx : avail;
	return box_clamp_w(b, w < mn ? mn : w, W);
}

static void base_size(struct item *it, int row, float W, float H,
	const struct style *fst)
{
	struct box *b = it->b;
	const struct style *st = b->style;
	const struct cval *fb = &st->v[P_FLEX_BASIS];
	float mn, mx, sz = -1, cw = -1;
	int content = 0;

	if (!fb->kw)
		sz = len_val(fb, row ? W : (H >= 0 ? H : 0));
	if (sz >= 0 && kw(st, P_BOX_SIZING, "border-box"))
		sz -= row ? it->mbefore + it->mafter -
			(b->m[3] + b->m[1]) : 0;
	if (sz < 0 || (fb->kw && kw(st, P_FLEX_BASIS, "auto")))
		sz = row ? box_spec_w(b, P_WIDTH, W) :
			box_spec_h(b, P_HEIGHT, H);
	if (!row) {
		/* the width it is laid out at below, max-width included:
		 * measured wider, its text wraps less, the height is short
		 * (a centred section of 1000 px in a body that is a column) */
		cw = column_width(it, fst, W);
	}
	if (sz < 0) {
		/* content: max-content in the main axis */
		content = 1;
		if (row) {
			content_sizes(b, &mn, &mx);
			sz = mx;
		} else {
			sz = b->type == B_REPLACED ? b->ih :
				max_content_height(b, cw);
		}
	}
	if (sz < 0)
		sz = 0;
	it->base = sz;
	/*
	 * automatic minimum size (4.5): the content size suggestion, the
	 * min-content size (through the aspect ratio of a replaced box
	 * from its definite cross size), clamped by the max size, and the
	 * specified size if smaller; 0 for a scroll container (clip is
	 * not one)
	 */
	if (row) {
		float smn = box_spec_w(b, P_MIN_WIDTH, W);
		float spec = box_spec_w(b, P_WIDTH, W);

		it->mx = box_spec_w(b, P_MAX_WIDTH, W);
		if (smn < 0 && !scrolls(st, P_OVERFLOW_X)) {
			float ch = box_spec_h(b, P_HEIGHT, H);

			if (box_has_ratio(b) && ch >= 0) {
				smn = box_ratio_w(b, box_clamp_h(b, ch, H));
			} else {
				content_sizes(b, &mn, &mx);
				smn = mn;
			}
			if (it->mx >= 0 && it->mx < smn)
				smn = it->mx;
			if (spec >= 0 && spec < smn)
				smn = spec;
		}
		it->mn = smn < 0 ? 0 : smn;
	} else {
		float smn = box_spec_h(b, P_MIN_HEIGHT, H);
		float spec = box_spec_h(b, P_HEIGHT, H);

		it->mx = box_spec_h(b, P_MAX_HEIGHT, H);
		/* its content height, its height if smaller (flex: 1 is
		 * a basis of 0: go.dev's <main> was 0 high, the footer
		 * over it) */
		if (smn < 0 && !scrolls(st, P_OVERFLOW_Y)) {
			if (b->type == B_REPLACED && box_has_ratio(b))
				smn = box_ratio_h(b, cw);
			else if (b->type == B_REPLACED)
				smn = b->ih;
			else
				smn = content ? sz : box_content_height(b, cw);
			if (it->mx >= 0 && it->mx < smn)
				smn = it->mx;
			if (spec >= 0 && spec < smn)
				smn = spec;
		}
		it->mn = smn < 0 ? 0 : smn;
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

/*
 * The static position of the absolute children: as the only item of
 * the container, its content box (CSS Flexbox 1, 4.1), placed by
 * justify-content on the main axis (space-between as start, around
 * and evenly as center) and by its align-self on the cross axis.
 */
static void abs_children(struct box *f, float w, float h, int row,
	int rev, int jc)
{
	const struct style *st = f->style;
	int wrev = kw(st, P_FLEX_WRAP, "wrap-reverse");
	struct box *c;

	jc = jc == 'c' || jc == 'a' || jc == 'v' ? 'c' : jc == 'e' ? 'e' :
		'f';
	if (rev)
		jc = jc == 'e' ? 'f' : jc == 'f' ? 'e' : jc;
	for (c = f->first; c; c = c->next) {
		const char *as = c->style->v[P_ALIGN_SELF].u.str;
		int a;

		if (!(c->flags & BF_ABS))
			continue;
		a = align_of(as && strcmp(as, "auto") ? as :
			st->v[P_ALIGN_ITEMS].u.str, "stretch");
		a = a == 'c' || a == 'e' ? a : 'f';
		if (wrev)
			a = a == 'e' ? 'f' : a == 'f' ? 'e' : a;
		abs_rect(c, f->x, f->y, w, h, row ? jc : a, row ? a : jc);
	}
}

/*
 * cbh is f's own content height when definite, else -1: its height,
 * or one set from outside (a stretched item, an abs box between its
 * top and bottom), as layout_contents() passes it.
 */
float layout_flex(struct box *f, float cbh)
{
	const struct style *st = f->style;
	int row = is_row(st), rev = is_reverse(st);
	int wrap = !kw(st, P_FLEX_WRAP, "nowrap");
	int wrev = kw(st, P_FLEX_WRAP, "wrap-reverse");
	float W = f->w, H = cbh;
	float lo = box_spec_h(f, P_MIN_HEIGHT, cbh);
	float hi = box_spec_h(f, P_MAX_HEIGHT, cbh);
	float colh = -1;	/* a column's height, auto but clamped */
	float main_avail, mgap, cgap, y, cross_total = 0, extra;
	struct item *v = NULL;
	struct box *c;
	int n = 0, i, start, nlines = 0, *lstart = NULL;
	float *lcross = NULL, *lasc;
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
		base_size(&v[i], row, W, H, st);
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

			/* a column of auto height: its content's, within
			 * its min and max (body { min-height: 100vh }: the
			 * footer at the bottom of the screen) */
			if (main_avail >= 1e29f && nlines == 1 && end == n) {
				if (hi >= 0 && avail > hi)
					avail = hi;
				if (lo >= 0 && avail < lo)
					avail = lo;
				colh = avail;
			}

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
			layout_item(b, 0, 0, column_width(it, st, W),
				it->main);
			it->cross = b->w;
		}
	}
	/* per line: the largest ascent of the first baseline items, the
	 * largest descent of the last baseline ones, and the size of each
	 * group */
	lasc = xmalloc((size_t)(nlines + 1) * 4 * sizeof(*lasc));
	for (i = 0; i < nlines; i++) {
		float *la = lasc + 4 * i;
		float below = 0, above = 0;
		int k;

		lcross[i] = 0;
		la[0] = la[1] = 0;
		for (k = lstart[i]; k < lstart[i + 1]; k++) {
			float o = v[k].cross + v[k].cbefore + v[k].cafter;

			if (o > lcross[i])
				lcross[i] = o;
			if (baseline_of(&v[k], st, row)) {
				int l = v[k].bl == 2;
				float a = l ? o - v[k].asc : v[k].asc;

				if (a > la[l])
					la[l] = a;
				if (l && v[k].asc > above)
					above = v[k].asc;
				else if (!l && o - v[k].asc > below)
					below = o - v[k].asc;
			}
		}
		la[2] = la[0] + below;
		la[3] = la[1] + above;
		if (la[2] > lcross[i])
			lcross[i] = la[2];
		if (la[3] > lcross[i])
			lcross[i] = la[3];
		cross_total += lcross[i];
	}
	cross_total += cgap * (float)(nlines - 1);
	{
		/* a definite cross size: single lines fill it */
		float cross_avail = row ? H : W;

		if (cross_avail >= 0 && nlines == 1 && !wrap) {
			lcross[0] = cross_avail, cross_total = cross_avail;
		} else if (row && nlines == 1 && !wrap) {
			/* single line of auto height: within the container's
			 * min and max heights (CSS Flexbox 9.4 step 15) */
			if (hi >= 0 && lcross[0] > hi)
				lcross[0] = hi;
			if (lo >= 0 && lcross[0] < lo)
				lcross[0] = lo;
			cross_total = lcross[0];
		} else if (cross_avail >= 0 && ac == 's' && nlines &&
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
		/* the lines from the cross end with wrap-reverse */
		float ly = wrev ? cross_total + extra - y - lcross[i] : y;

		for (k = a; k < z; k++) {
			used += v[k].main + v[k].mbefore + v[k].mafter;
			autos += v[k].auto_ms + v[k].auto_me;
		}
		used += mgap * (float)(z - a - 1 > 0 ? z - a - 1 : 0);
		lineavail = row ? W : H >= 0 ? H : colh >= 0 ? colh : used;
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

			if (it->bl && !it->auto_cs && !it->auto_ce) {
				const float *la = lasc + 4 * i;

				/* the group at the line's cross start (end
				 * for the last baseline), wrap-reverse swaps
				 * them */
				cpos = it->bl == 1 ? la[0] - it->asc :
					lcross[i] - la[1] - it->asc;
				if (wrev)
					cpos += it->bl == 1 ? lcross[i] - la[2] :
						la[3] - lcross[i];
			} else if (al == 's' && (row ? is_auto(b->style, P_HEIGHT) :
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
				} else if (cross != it->cross) {
					/* a definite height (CSS Flexbox 9.4
					 * step 11): its percentages and its own
					 * flex lines resolve against it */
					layout_item(b, 0, 0, it->main, cross);
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
			if (wrev && !it->bl)
				/* wrap-reverse: cross start and end swap */
				cpos = lcross[i] - cross - it->cbefore -
					it->cafter - cpos;
			if (row) {
				b->h = cross;
				mx = f->x + pos + it->mbefore;
				my = f->y + ly + cpos + it->cbefore;
			} else {
				b->w = cross;
				mx = f->x + ly + cpos + it->cbefore;
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
			h = H >= 0 ? H : colh >= 0 ? colh : used;
		}
		abs_children(f, W, box_clamp_h(f, h, cbh), row, rev, jc);
		free(lasc);
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
