/*
 * Grid layout (CSS Grid 1, simplified): track lists with px, %, fr,
 * auto, min-content, max-content, minmax(), fit-content(), repeat()
 * (with auto-fill/auto-fit), line names and template areas; explicit
 * and row-flow auto placement; implicit tracks; track sizing; gaps;
 * item and content alignment.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"

enum { TK_LEN, TK_PCT, TK_FR, TK_AUTO, TK_MIN, TK_MAX };

struct tsize {
	int kind;
	float v;
	float px;	/* TK_PCT: the length added, calc(100% - 200px) */
};

struct track {
	struct tsize min, max;
	float base;		/* used size */
	float cmin, cmax;	/* content contributions */
	unsigned char fit;	/* repeated by auto-fit */
	unsigned char gone;	/* auto-fit and empty: collapsed, no size
				 * and no gutters */
};

struct tracks {
	struct track *t;
	int n;
	char **names;		/* line names, "a b " per line */
};

struct area {
	char name[64];
	int r0, c0, r1, c1;
};

struct gitem {
	struct box *b;
	int r0, r1, c0, c1;	/* lines, 0-based, end exclusive */
	int rdef, cdef;		/* definite position in that axis */
	float oh;		/* its outer height at its column width */
	int bl;			/* baseline aligned in its row: 1 first,
				 * 2 last */
	float asc;		/* its margin box top to that baseline */
};

static struct ucontext unit_ctx(const struct style *st)
{
	struct ucontext u;

	doc_units(st, &u);
	return u;
}

static int track_size(struct cur *c, const struct style *st,
	struct tsize *s)
{
	const struct cv *v = cur_peek(c);
	struct ucontext u = unit_ctx(st);

	if (!v)
		return 0;
	if (v->type == CT_DIMENSION && ascii_ieq(v->str, "fr")) {
		s->kind = TK_FR;
		s->v = (float)v->num;
		cur_next(c);
		return 1;
	}
	if (cv_ident(v, "auto")) {
		s->kind = TK_AUTO;
	} else if (cv_ident(v, "min-content")) {
		s->kind = TK_MIN;
	} else if (cv_ident(v, "max-content")) {
		s->kind = TK_MAX;
	} else {
		struct slen *l = parse_length(c, 0);
		struct len r;

		if (!l)
			return 0;
		slen_resolve(l, &u, &r);
		slen_free(l);
		if (r.calc)
			calc_release(r.calc);
		s->kind = r.pct ? TK_PCT : TK_LEN;
		s->v = r.pct ? r.pct : r.px;
		s->px = r.pct ? r.px : 0;
		return 1;
	}
	cur_next(c);
	return 1;
}

static void add_track(struct tracks *t, struct tsize mn, struct tsize mx)
{
	int fresh = !t->names;	/* else line 0 may have names already */

	t->t = xrealloc(t->t, (size_t)(t->n + 1) * sizeof(*t->t));
	memset(&t->t[t->n], 0, sizeof(*t->t));
	t->t[t->n].min = mn;
	t->t[t->n].max = mx;
	t->n++;
	t->names = xrealloc(t->names, (size_t)(t->n + 1) * sizeof(*t->names));
	t->names[t->n] = NULL;
	if (fresh)
		t->names[0] = NULL;
}

static void add_names(struct tracks *t, const struct cv *block)
{
	int line = t->n, i;
	struct buf b = { 0 };

	if (!t->names) {
		t->names = xmalloc(sizeof(*t->names));
		t->names[0] = NULL;
	}
	buf_add(&b, " ", 1);
	if (t->names[line]) {
		buf_add(&b, t->names[line], strlen(t->names[line]));
		free(t->names[line]);
	}
	for (i = 0; i < block->kids.n; i++)
		if (block->kids.v[i].type == CT_IDENT) {
			buf_add(&b, block->kids.v[i].str,
				strlen(block->kids.v[i].str));
			buf_putc(&b, ' ');
		}
	t->names[line] = b.s;
}

/* One <track-size>, minmax() or fit-content(). */
static int one_track(struct cur *c, const struct style *st, struct tracks *t)
{
	const struct cv *v = cur_peek(c);
	struct tsize a, b;

	if (cv_func(v, "minmax") || cv_func(v, "fit-content")) {
		struct cur in;

		cur_init(&in, &v->kids);
		if (cv_func(v, "fit-content")) {
			if (!track_size(&in, st, &b))
				return 0;
			a.kind = TK_AUTO;
			a.v = 0;
		} else if (!track_size(&in, st, &a) || !cur_next(&in) ||
			   !track_size(&in, st, &b)) {
			return 0;
		}
		cur_next(c);
		add_track(t, a, b);
		return 1;
	}
	if (!track_size(c, st, &a))
		return 0;
	b = a;
	if (a.kind == TK_FR) {
		a.kind = TK_AUTO;	/* 1fr is minmax(auto, 1fr) */
		a.v = 0;
	}
	add_track(t, a, b);
	return 1;
}

static float fixed_size(const struct tsize *s, float avail)
{
	if (s->kind == TK_LEN)
		return s->v;
	if (s->kind == TK_PCT) {
		float v = avail > 0 ? s->v * avail / 100 + s->px : 0;

		return v < 0 ? 0 : v;
	}
	return 0;
}

static void parse_tracks(const char *text, const struct style *st,
	float avail, float g, struct tracks *t)
{
	struct cvlist l;
	struct cur c;
	const struct cv *v;

	memset(t, 0, sizeof(*t));
	if (!text || !strcmp(text, "none") || !strncmp(text, "subgrid", 7))
		return;
	css_component_values(text, strlen(text), &l);
	cur_init(&c, &l);
	while ((v = cur_peek(&c))) {
		if (v->type == CT_BLOCK && v->delim == '[') {
			add_names(t, v);
			cur_next(&c);
		} else if (cv_func(v, "repeat")) {
			struct cur in;
			const struct cv *n;
			int count = 1, k, first, auto_rep = 0, fit;

			cur_init(&in, &v->kids);
			n = cur_next(&in);
			if (n && n->type == CT_NUMBER)
				count = (int)n->num;
			else if (cv_ident(n, "auto-fill") ||
				 cv_ident(n, "auto-fit"))
				auto_rep = 1;
			fit = cv_ident(n, "auto-fit");
			cur_next(&in);	/* comma */
			first = t->n;
			{
				struct cur body = in;

				while (cur_peek(&body)) {
					const struct cv *b = cur_peek(&body);

					if (b->type == CT_BLOCK && b->delim == '[') {
						add_names(t, b);
						cur_next(&body);
					} else if (!one_track(&body, st, t)) {
						break;
					}
				}
			}
			if (auto_rep) {
				/* as many as fit, at least one */
				float one = 0;
				int per = t->n - first;

				for (k = first; k < t->n; k++) {
					float s = fixed_size(&t->t[k].max, avail);

					if (s <= 0)
						s = fixed_size(&t->t[k].min, avail);
					one += s + g;
				}
				count = one > 0 && avail > 0 ?
					(int)((avail + g) / one) : 1;
				if (count < 1)
					count = 1;
				if (count > 1000)
					count = 1000;
				(void)per;
			}
			if (count > 1000)
				count = 1000;
			{
				int per = t->n - first, r;

				for (r = 1; r < count; r++)
					for (k = 0; k < per; k++)
						add_track(t, t->t[first + k].min,
							t->t[first + k].max);
				for (k = first; fit && k < t->n; k++)
					t->t[k].fit = 1;
			}
			cur_next(&c);
		} else if (!one_track(&c, st, t)) {
			cur_next(&c);	/* skip what we do not know */
		}
	}
	cvlist_free(&l);
}

static void free_tracks(struct tracks *t)
{
	int i;

	for (i = 0; t->names && i <= t->n; i++)
		free(t->names[i]);
	free(t->names);
	free(t->t);
}

/* grid-template-areas: "a a b" "c c b" */
static int parse_areas(const char *text, struct area *a, int max, int *rows,
	int *cols)
{
	struct cvlist l;
	int n = 0, r = 0, i;

	*rows = *cols = 0;
	if (!text || !strcmp(text, "none"))
		return 0;
	css_component_values(text, strlen(text), &l);
	for (i = 0; i < l.n; i++) {
		const char *s;
		int col = 0;

		if (l.v[i].type != CT_STRING)
			continue;
		for (s = l.v[i].str; *s; ) {
			char name[64];
			size_t k = 0;
			int j;

			while (*s == ' ' || *s == '\t' || *s == '\n')
				s++;
			if (!*s)
				break;
			while (*s && *s != ' ' && *s != '\t' && *s != '\n' &&
			       k < sizeof(name) - 1)
				name[k++] = *s++;
			name[k] = '\0';
			if (name[0] != '.') {
				for (j = 0; j < n; j++)
					if (!strcmp(a[j].name, name))
						break;
				if (j == n && n < max) {
					memset(&a[n], 0, sizeof(a[n]));
					strcpy(a[n].name, name);
					a[n].r0 = r;
					a[n].c0 = col;
					n++;
				}
				if (j < n) {
					a[j].r1 = r + 1;
					a[j].c1 = col + 1;
				}
			}
			col++;
		}
		if (col > *cols)
			*cols = col;
		r++;
	}
	*rows = r;
	cvlist_free(&l);
	return n;
}

/* The first line named name, explicit or of an area's edges
 * (area a makes lines a-start and a-end), or -1 */
static int line_named(const struct tracks *t, const struct area *areas,
	int nareas, const char *name, int row)
{
	char key[80];
	size_t n = strlen(name);
	int i, end = -1;

	if (n > 6 && !strcmp(name + n - 6, "-start"))
		end = 0, n -= 6;
	else if (n > 4 && !strcmp(name + n - 4, "-end"))
		end = 1, n -= 4;
	snprintf(key, sizeof(key), " %s ", name);
	for (i = 0; t->names && i <= t->n; i++)
		if (t->names[i] && strstr(t->names[i], key))
			return i;
	for (i = 0; end >= 0 && i < nareas; i++)
		if (strlen(areas[i].name) == n &&
		    !strncmp(areas[i].name, name, n))
			return row ? (end ? areas[i].r1 : areas[i].r0) :
				(end ? areas[i].c1 : areas[i].c0);
	return -1;
}

/* A line by its <custom-ident> (CSS Grid 8.3.1): a line named
 * name-start (name-end for an end), the area's edge, else one named
 * name. */
static int named_line(const struct tracks *t, const struct area *areas,
	int nareas, const char *name, int end, int row)
{
	char edge[80];
	int i;

	snprintf(edge, sizeof(edge), "%s%s", name, end ? "-end" : "-start");
	if ((i = line_named(t, areas, nareas, edge, row)) >= 0)
		return i;
	return line_named(t, areas, nareas, name, row);
}

/* grid-row-start and friends: line (0-based) or -1 auto; *span. */
/* Lines beyond are clamped, as browsers do (Firefox: 10000): the
 * occupancy grid is rows x columns */
#define GRID_LINES 10000

static int line_of(const char *s, const struct tracks *t,
	const struct area *areas, int nareas, int end, int row, int *span)
{
	struct cvlist l;
	struct cur c;
	const struct cv *v;
	int line = -1, n = 0, is_span = 0;
	const char *name = NULL;

	*span = 0;
	if (!s || !strcmp(s, "auto"))
		return -1;
	css_component_values(s, strlen(s), &l);
	cur_init(&c, &l);
	while ((v = cur_next(&c))) {
		if (cv_ident(v, "span"))
			is_span = 1;
		else if (v->type == CT_NUMBER)
			n = v->num > GRID_LINES ? GRID_LINES : v->num <
				-GRID_LINES ? -GRID_LINES : (int)v->num;
		else if (v->type == CT_IDENT)
			name = v->str;
	}
	if (is_span) {
		*span = n > 0 ? n : 1;
	} else if (name) {
		line = named_line(t, areas, nareas, name, end, row);
	} else if (n > 0) {
		line = n - 1;
	} else if (n < 0) {
		line = t->n + 1 + n;
		if (line < 0)
			line = 0;
	}
	cvlist_free(&l);
	return line;
}

/* Fill [from, to) of an occupancy grid. */
struct occ {
	char *cell;
	int rows, cols;
};

static void occ_grow(struct occ *o, int rows, int cols)
{
	char *n;
	int r;

	if (rows <= o->rows && cols <= o->cols)
		return;
	if (rows < o->rows)
		rows = o->rows;
	if (cols < o->cols)
		cols = o->cols;
	n = calloc((size_t)rows * (size_t)cols + 1, 1);
	if (!n)
		abort();
	for (r = 0; r < o->rows; r++)
		memcpy(n + r * cols, o->cell + r * o->cols, (size_t)o->cols);
	free(o->cell);
	o->cell = n;
	o->rows = rows;
	o->cols = cols;
}

static int occ_free(struct occ *o, int r0, int r1, int c0, int c1)
{
	int r, c;

	for (r = r0; r < r1; r++)
		for (c = c0; c < c1; c++)
			if (r < o->rows && c < o->cols &&
			    o->cell[r * o->cols + c])
				return 0;
	return 1;
}

static void occ_set(struct occ *o, int r0, int r1, int c0, int c1)
{
	int r, c;

	occ_grow(o, r1, c1);
	for (r = r0; r < r1; r++)
		for (c = c0; c < c1; c++)
			o->cell[r * o->cols + c] = 1;
}

/* Implicit tracks get grid-auto-rows/columns (first track only). */
static void extend(struct tracks *t, int n, const char *autos,
	const struct style *st)
{
	struct tracks a;

	parse_tracks(autos, st, 0, 0, &a);
	while (t->n < n) {
		struct tsize s = { TK_AUTO, 0, 0 };

		if (a.n)
			add_track(t, a.t[0].min, a.t[0].max);
		else
			add_track(t, s, s);
	}
	free_tracks(&a);
}

/* Size tracks for a total of avail (or -1: intrinsic). */
/* The tracks not collapsed */
static int live(const struct tracks *t)
{
	int i, n = 0;

	for (i = 0; i < t->n; i++)
		n += !t->t[i].gone;
	return n;
}

static void size_tracks(struct tracks *t, float avail, float g)
{
	float sum = 0, fr = 0, free_space;
	int i, flex = 0, again, nl = live(t);

	for (i = 0; i < t->n; i++) {
		struct track *k = &t->t[i];

		if (k->gone) {
			/* a fixed 0 */
			k->min.kind = k->max.kind = TK_LEN;
			k->min.v = k->max.v = k->cmin = k->cmax = 0;
		}
		if (k->min.kind == TK_LEN || k->min.kind == TK_PCT)
			k->base = fixed_size(&k->min, avail);
		else if (k->min.kind == TK_MAX)
			k->base = k->cmax;
		else
			k->base = k->cmin;	/* auto, min-content */
		if ((k->max.kind == TK_LEN || k->max.kind == TK_PCT) &&
		    fixed_size(&k->max, avail) < k->base &&
		    k->min.kind != TK_AUTO)
			k->base = fixed_size(&k->max, avail);
		sum += k->base;
	}
	sum += g * (float)(nl > 0 ? nl - 1 : 0);
	free_space = avail >= 0 ? avail - sum : 0;
	/* maximize tracks (12.6): the free space shared equally by the
	 * inflexible tracks, each up to its growth limit (a fixed maximum,
	 * else its content). MDN's minmax(0, 48rem) content column stayed
	 * 0 wide, its text one letter a line. */
	if (avail < 0) {
		for (i = 0; i < t->n; i++) {
			struct track *k = &t->t[i];

			if ((k->max.kind == TK_AUTO || k->max.kind == TK_MAX) &&
			    k->cmax > k->base)
				k->base = k->cmax;
		}
	} else {
		float *gl = xmalloc((size_t)(t->n + 1) * sizeof(*gl));
		int open;

		for (i = 0; i < t->n; i++) {
			struct track *k = &t->t[i];

			gl[i] = k->base;
			if (k->max.kind == TK_LEN || k->max.kind == TK_PCT)
				gl[i] = fixed_size(&k->max, avail);
			else if (k->max.kind == TK_AUTO || k->max.kind == TK_MAX)
				gl[i] = k->cmax;
			if (gl[i] < k->base)
				gl[i] = k->base;
		}
		do {
			float share;

			open = 0;
			for (i = 0; i < t->n; i++)
				open += t->t[i].max.kind != TK_FR &&
					gl[i] > t->t[i].base;
			if (!open || free_space <= 0.01f)
				break;
			share = free_space / (float)open;
			for (i = 0; i < t->n; i++) {
				struct track *k = &t->t[i];
				float d = gl[i] - k->base;

				if (k->max.kind == TK_FR || d <= 0)
					continue;
				if (d > share)
					d = share;
				k->base += d;
				free_space -= d;
			}
		} while (open);
		free(gl);
	}
	/* flexible tracks share what is left (12.7) */
	for (i = 0; i < t->n; i++)
		if (t->t[i].max.kind == TK_FR) {
			fr += t->t[i].max.v;
			flex = 1;
		}
	if (!flex || avail < 0)
		goto stretch;
	do {
		float rest = avail - g * (float)(nl > 0 ? nl - 1 : 0), unit;

		again = 0;
		fr = 0;
		for (i = 0; i < t->n; i++) {
			struct track *k = &t->t[i];

			if (k->max.kind == TK_FR && k->max.v > 0)
				fr += k->max.v;
			else
				rest -= k->base;
		}
		unit = fr > 0 ? rest / (fr < 1 ? 1 : fr) : 0;
		for (i = 0; i < t->n; i++) {
			struct track *k = &t->t[i];

			if (k->max.kind == TK_FR && k->max.v > 0 &&
			    k->max.v * unit < k->base) {
				/* too small for its content: inflexible */
				k->max.kind = TK_LEN;
				k->max.v = k->base;
				again = 1;
			}
		}
		if (!again)
			for (i = 0; i < t->n; i++)
				if (t->t[i].max.kind == TK_FR)
					t->t[i].base = t->t[i].max.v * unit >
						0 ? t->t[i].max.v * unit : 0;
	} while (again);
	return;
stretch:
	/* auto tracks take the rest (normal content alignment) */
	if (avail >= 0 && free_space > 0) {
		int autos = 0;

		for (i = 0; i < t->n; i++)
			autos += t->t[i].max.kind == TK_AUTO;
		for (i = 0; autos && i < t->n; i++)
			if (t->t[i].max.kind == TK_AUTO)
				t->t[i].base += free_space / (float)autos;
	}
}

static float span_size(const struct tracks *t, int a, int b, float g)
{
	float s = 0;
	int i, n = 0;

	for (i = a; i < b && i < t->n; i++)
		if (!t->t[i].gone) {
			s += t->t[i].base;
			n++;
		}
	for (; i < b; i++)
		n++;	/* implicit, not created */
	return s + g * (float)(n - 1 > 0 ? n - 1 : 0);
}

static float track_pos(const struct tracks *t, int a, float g)
{
	float s = 0;
	int i;

	for (i = 0; i < a && i < t->n; i++)
		if (!t->t[i].gone)
			s += t->t[i].base + g;
	return s;
}

static int align_kw(const char *s, int def)
{
	if (!s || !strcmp(s, "normal") || !strcmp(s, "auto") ||
	    !strcmp(s, "legacy"))
		return def;
	if (strstr(s, "center"))
		return 'c';
	if (strstr(s, "end") || strstr(s, "right"))
		return 'e';
	if (strstr(s, "stretch"))
		return 's';
	if (strstr(s, "space-between"))
		return 'b';
	if (strstr(s, "space-around"))
		return 'a';
	if (strstr(s, "space-evenly"))
		return 'v';
	return 'f';
}

/* Line i of t where the grid starts at o: the start edge of track i
 * (end 0), the end edge of track i - 1 (end 1); auto (-1) or a line
 * past the grid is the padding edge pad */
static float line_at(const struct tracks *t, int i, int end, float g,
	float pad, float o)
{
	if (i < 0 || i > t->n)
		return pad;
	if (end && i > 0)
		return o + track_pos(t, i - 1, g) + t->t[i - 1].base;
	if (i == t->n)
		return i ? o + track_pos(t, i - 1, g) + t->t[i - 1].base : o;
	return o + track_pos(t, i, g);
}

/* An absolute child's self alignment in its static position
 * rectangle: 'f', 'c' or 'e' (normal and stretch are start, the last
 * baseline end) */
static int abs_align(const char *self, const char *items)
{
	const char *s = self && strcmp(self, "auto") ? self : items;
	int a;

	if (s && strstr(s, "last"))
		return 'e';
	a = align_kw(s, 'f');
	return a == 'c' || a == 'e' ? a : 'f';
}

/*
 * An item of one row aligned by its baseline, laid out at the origin,
 * outer height h: its bl and asc (the bottom of its border box when it
 * has no baseline), and its row's la: the largest ascent of the first
 * baseline group, the largest descent of the last one, and the largest
 * part of their items on the other side.
 */
static void baseline_item(struct gitem *it, const struct style *gst,
	float h, float *la)
{
	struct box *b = it->b;
	const char *s = b->style->v[P_ALIGN_SELF].u.str;
	float y, a;

	if (!s || !strcmp(s, "auto"))
		s = gst->v[P_ALIGN_ITEMS].u.str;
	it->bl = 0;
	if (!s || !strstr(s, "baseline"))
		return;
	it->bl = strstr(s, "last") ? 2 : 1;
	y = box_baseline(b, it->bl == 2);
	it->asc = y > -1e29f ? b->m[0] + b->b[0] + b->p[0] + y - b->y :
		h - b->m[2];
	a = it->bl == 2 ? h - it->asc : it->asc;
	if (a > la[it->bl - 1])
		la[it->bl - 1] = a;
	if (h - a > la[it->bl + 1])
		la[it->bl + 1] = h - a;
}

/* justify-content, align-content: the free space before the first
 * track and between two, n tracks */
static void distribute(const char *s, float free, int n, float *off,
	float *sp)
{
	switch (align_kw(s, 'f')) {
	case 'c': *off = free / 2; break;
	case 'e': *off = free; break;
	case 'b': *sp = n > 1 ? free / (float)(n - 1) : 0; break;
	case 'a': *sp = free / (float)n; *off = *sp / 2; break;
	case 'v': *sp = *off = free / (float)(n + 1); break;
	}
}

static float gapv(const struct style *st, int prop, float ref)
{
	return st->v[prop].kw ? 0 : len_val(&st->v[prop], ref);
}

/* An absolute child's lines, -1 for auto (the padding edge): a span
 * against auto is auto too (CSS Grid 1, 9.1) */
static void abs_place(struct box *c, struct tracks *cols,
	struct tracks *rows, struct area *areas, int nareas,
	struct gitem **abs, int *nabs)
{
	const struct style *st = c->style;
	struct gitem *it;
	int s1, s2, a, b;

	*abs = xrealloc(*abs, (size_t)(*nabs + 1) * sizeof(**abs));
	it = &(*abs)[(*nabs)++];
	memset(it, 0, sizeof(*it));
	it->b = c;
	a = line_of(st->v[P_GRID_COLUMN_START].u.str, cols, areas, nareas, 0,
		0, &s1);
	b = line_of(st->v[P_GRID_COLUMN_END].u.str, cols, areas, nareas, 1,
		0, &s2);
	it->c0 = a;
	it->c1 = b >= 0 ? b : a >= 0 && s2 ? a + s2 : -1;
	a = line_of(st->v[P_GRID_ROW_START].u.str, rows, areas, nareas, 0,
		1, &s1);
	b = line_of(st->v[P_GRID_ROW_END].u.str, rows, areas, nareas, 1,
		1, &s2);
	it->r0 = a;
	it->r1 = b >= 0 ? b : a >= 0 && s2 ? a + s2 : -1;
}

/* Shared by layout and intrinsic sizes. */
static int place(struct box *gb, float W, float H, struct tracks *cols,
	struct tracks *rows, struct gitem **out, float cg, float rg,
	struct gitem **abs, int *nabs)
{
	const struct style *st = gb->style;
	struct area areas[64];
	int nareas, arows, acols, n = 0, i, cur_r = 0, cur_c = 0;
	struct gitem *v = NULL;
	struct occ o = { NULL, 0, 0 };
	struct box *c;
	int column_flow = strstr(st->v[P_GRID_AUTO_FLOW].u.str, "column") !=
		NULL;

	parse_tracks(st->v[P_GRID_TEMPLATE_COLUMNS].u.str, st, W, cg, cols);
	parse_tracks(st->v[P_GRID_TEMPLATE_ROWS].u.str, st, H, rg, rows);
	nareas = parse_areas(st->v[P_GRID_TEMPLATE_AREAS].u.str, areas, 64,
		&arows, &acols);
	extend(cols, acols, st->v[P_GRID_AUTO_COLUMNS].u.str, st);
	extend(rows, arows, st->v[P_GRID_AUTO_ROWS].u.str, st);
	occ_grow(&o, rows->n ? rows->n : 1, cols->n ? cols->n : 1);
	for (c = gb->first; c; c = c->next) {
		struct gitem *it;
		int rs, re, cs, ce, s1, s2;

		if (c->flags & BF_ABS) {
			box_edges(c, W);
			abs_static(c, gb->x, gb->y);
			if (abs)
				abs_place(c, cols, rows, areas, nareas, abs,
					nabs);
			continue;
		}
		v = xrealloc(v, (size_t)(n + 1) * sizeof(*v));
		it = &v[n++];
		memset(it, 0, sizeof(*it));
		it->b = c;
		cs = line_of(c->style->v[P_GRID_COLUMN_START].u.str, cols, areas,
			nareas, 0, 0, &s1);
		ce = line_of(c->style->v[P_GRID_COLUMN_END].u.str, cols, areas,
			nareas, 1, 0, &s2);
		if (cs >= 0) {
			it->c0 = cs;
			it->c1 = ce > cs ? ce : cs + (s2 ? s2 : 1);
			it->cdef = 1;
		} else if (ce >= 0) {
			it->c1 = ce;
			it->c0 = ce - (s1 ? s1 : 1);
			if (it->c0 < 0)
				it->c0 = 0;
			it->cdef = 1;
		} else {
			it->c1 = s1 ? s1 : s2 ? s2 : 1;
		}
		rs = line_of(c->style->v[P_GRID_ROW_START].u.str, rows, areas,
			nareas, 0, 1, &s1);
		re = line_of(c->style->v[P_GRID_ROW_END].u.str, rows, areas,
			nareas, 1, 1, &s2);
		if (rs >= 0) {
			it->r0 = rs;
			it->r1 = re > rs ? re : rs + (s2 ? s2 : 1);
			it->rdef = 1;
		} else if (re >= 0) {
			it->r1 = re;
			it->r0 = re - (s1 ? s1 : 1);
			if (it->r0 < 0)
				it->r0 = 0;
			it->rdef = 1;
		} else {
			it->r1 = s1 ? s1 : s2 ? s2 : 1;
		}
		if (it->cdef && it->rdef)
			occ_set(&o, it->r0, it->r1, it->c0, it->c1);
	}
	/* auto placement, row by row (or column by column) */
	for (i = 0; i < n; i++) {
		struct gitem *it = &v[i];
		int rspan = it->r1 - it->r0, cspan = it->c1 - it->c0;
		int ncols = o.cols > cspan ? o.cols : cspan;

		if (it->cdef && it->rdef)
			continue;
		if (it->rdef && !it->cdef) {
			int col = 0;

			while (!occ_free(&o, it->r0, it->r1, col, col + cspan))
				col++;
			it->c0 = col;
			it->c1 = col + cspan;
		} else if (it->cdef && !it->rdef) {
			int row = 0;

			while (!occ_free(&o, row, row + rspan, it->c0, it->c1))
				row++;
			it->r0 = row;
			it->r1 = row + rspan;
		} else if (column_flow) {
			int nrows = o.rows > rspan ? o.rows : rspan;

			for (;;) {
				if (cur_r + rspan > nrows) {
					cur_r = 0;
					cur_c++;
				}
				if (occ_free(&o, cur_r, cur_r + rspan, cur_c,
					     cur_c + cspan))
					break;
				cur_r++;
			}
			it->r0 = cur_r;
			it->r1 = cur_r + rspan;
			it->c0 = cur_c;
			it->c1 = cur_c + cspan;
			cur_r += rspan;
		} else {
			for (;;) {
				if (cur_c + cspan > ncols) {
					cur_c = 0;
					cur_r++;
				}
				if (occ_free(&o, cur_r, cur_r + rspan, cur_c,
					     cur_c + cspan))
					break;
				cur_c++;
			}
			it->r0 = cur_r;
			it->r1 = cur_r + rspan;
			it->c0 = cur_c;
			it->c1 = cur_c + cspan;
			cur_c += cspan;
		}
		occ_set(&o, it->r0, it->r1, it->c0, it->c1);
	}
	for (i = 0; i < n; i++) {
		extend(cols, v[i].c1, st->v[P_GRID_AUTO_COLUMNS].u.str, st);
		extend(rows, v[i].r1, st->v[P_GRID_AUTO_ROWS].u.str, st);
	}
	/* auto-fit tracks no item is in collapse */
	for (i = 0; i < cols->n; i++)
		cols->t[i].gone = cols->t[i].fit;
	for (i = 0; i < rows->n; i++)
		rows->t[i].gone = rows->t[i].fit;
	for (i = 0; i < n; i++) {
		int k;

		for (k = v[i].c0; k < v[i].c1 && k < cols->n; k++)
			cols->t[k].gone = 0;
		for (k = v[i].r0; k < v[i].r1 && k < rows->n; k++)
			rows->t[k].gone = 0;
	}
	free(o.cell);
	*out = v;
	return n;
}

/*
 * An item over several tracks, a to b: what its min and max
 * contributions exceed of those tracks (their fixed sizes, else their
 * content sizes so far) and of the gutters between is shared equally
 * by the tracks sized by their content (CSS Grid 12.5, step 3,
 * simplified: no order by span, no growth limits).
 */
static void span_contribution(struct tracks *t, int a, int b, float mn,
	float mx, float avail, float g)
{
	float smin = 0, smax = 0, e, emax;
	int i, n = 0, nl = 0;

	for (i = a; i < b && i < t->n; i++) {
		struct track *k = &t->t[i];

		if (k->gone)
			continue;
		nl++;
		if (k->min.kind == TK_LEN || k->min.kind == TK_PCT) {
			float f = fixed_size(&k->min, avail);

			if ((k->max.kind == TK_LEN || k->max.kind == TK_PCT) &&
			    fixed_size(&k->max, avail) > f)
				f = fixed_size(&k->max, avail);
			smin += f;
			smax += f;
		} else {
			smin += k->cmin;
			smax += k->cmax > k->cmin ? k->cmax : k->cmin;
			n++;
		}
	}
	if (!n)
		return;
	g *= (float)(nl > 1 ? nl - 1 : 0);
	e = (mn - smin - g) / (float)n;
	emax = (mx - smax - g) / (float)n;
	for (i = a; i < b && i < t->n; i++) {
		struct track *k = &t->t[i];

		if (k->gone || k->min.kind == TK_LEN || k->min.kind == TK_PCT)
			continue;
		if (emax > 0)
			k->cmax = (k->cmax > k->cmin ? k->cmax : k->cmin) + emax;
		if (e > 0)
			k->cmin += e;
		if (k->cmax < k->cmin)
			k->cmax = k->cmin;
	}
}

static void column_contributions(struct tracks *cols, struct gitem *v, int n,
	float avail, float g)
{
	int i;

	for (i = 0; i < n; i++) {
		float mn, mx;

		if (v[i].c1 - v[i].c0 != 1)
			continue;
		box_sizes(v[i].b, &mn, &mx);
		if (mn > cols->t[v[i].c0].cmin)
			cols->t[v[i].c0].cmin = mn;
		if (mx > cols->t[v[i].c0].cmax)
			cols->t[v[i].c0].cmax = mx;
	}
	for (i = 0; i < n; i++) {
		float mn, mx;

		if (v[i].c1 - v[i].c0 < 2)
			continue;
		box_sizes(v[i].b, &mn, &mx);
		span_contribution(cols, v[i].c0, v[i].c1, mn, mx, avail, g);
	}
}

float layout_grid(struct box *gb, float cbh)
{
	const struct style *st = gb->style;
	struct tracks cols, rows;
	struct gitem *v;
	/* cbh is the grid's own content height when definite, as
	 * layout_contents() passes it: its height, or one set from
	 * outside (it was resolved again against itself: height: 50% gave
	 * rows for a quarter) */
	float W = gb->w, H = cbh;
	float lo = box_spec_h(gb, P_MIN_HEIGHT, cbh);
	float hi = box_spec_h(gb, P_MAX_HEIGHT, cbh);
	float cg = gapv(st, P_COLUMN_GAP, W), rg = gapv(st, P_ROW_GAP, 0);
	float total, xoff = 0, yoff = 0, xs = 0, ys = 0;
	struct gitem *av = NULL;
	float *rasc;		/* per row as baseline_item() says */
	int n, i, nav = 0;

	n = place(gb, W, H, &cols, &rows, &v, cg, rg, &av, &nav);
	for (i = 0; i < rows.n; i++)
		if (rows.t[i].min.kind == TK_PCT || rows.t[i].max.kind == TK_PCT ||
		    rows.t[i].max.kind == TK_FR)
			box_pct_seen();	/* fr rows share a definite height */
	rasc = xmalloc(((size_t)rows.n * 4 + 4) * sizeof(*rasc));
	memset(rasc, 0, ((size_t)rows.n * 4 + 4) * sizeof(*rasc));
	column_contributions(&cols, v, n, W, cg);
	size_tracks(&cols, W, cg);
	/* items at their column widths give the row contributions */
	for (i = 0; i < n; i++) {
		struct box *b = v[i].b;
		float aw = span_size(&cols, v[i].c0, v[i].c1, cg), w, h;

		box_edges(b, aw);
		w = box_spec_w(b, P_WIDTH, aw);
		h = box_spec_h(b, P_HEIGHT, -1);
		if (w < 0 && b->type == B_REPLACED) {
			/* its width, through its ratio from its height, else
			 * its natural one */
			w = h >= 0 && box_has_ratio(b) ? box_ratio_w(b, h) :
				b->iw > 0 ? b->iw : 0;
		} else if (w < 0) {
			w = aw - b->m[1] - b->m[3] - b->b[1] - b->b[3] -
				b->p[1] - b->p[3];
		}
		layout_item(b, 0, 0, box_clamp_w(b, w, aw), h);
		h = b->h + b->m[0] + b->m[2] + b->b[0] + b->b[2] + b->p[0] +
			b->p[2];
		v[i].oh = h;
		if (v[i].r1 - v[i].r0 == 1) {
			baseline_item(&v[i], st, h, rasc + 4 * v[i].r0);
			if (h > rows.t[v[i].r0].cmin)
				rows.t[v[i].r0].cmin = h;
			if (h > rows.t[v[i].r0].cmax)
				rows.t[v[i].r0].cmax = h;
		}
	}
	for (i = 0; i < rows.n; i++) {
		/* a row holds its baseline groups whole */
		const float *la = rasc + 4 * i;
		float g = la[0] + la[2] > la[1] + la[3] ? la[0] + la[2] :
			la[1] + la[3];

		if (g > rows.t[i].cmin)
			rows.t[i].cmin = g;
		if (g > rows.t[i].cmax)
			rows.t[i].cmax = g;
	}
	for (i = 0; i < n; i++)
		if (v[i].r1 - v[i].r0 > 1)
			span_contribution(&rows, v[i].r0, v[i].r1, v[i].oh,
				v[i].oh, H, rg);
	size_tracks(&rows, H, rg);
	if (H < 0 && (lo >= 0 || hi >= 0)) {
		/* an automatic height within min-height and max-height:
		 * the rows are sized for the one that holds (min-height:
		 * 100vh; rows auto 1fr auto: the footer at the bottom) */
		total = span_size(&rows, 0, rows.n, rg);
		if (lo >= 0 && total < lo)
			H = lo;
		else if (hi >= 0 && total > hi)
			H = hi;
		if (H >= 0)
			size_tracks(&rows, H, rg);
	}
	/* content alignment of the whole grid */
	total = span_size(&cols, 0, cols.n, cg);
	if (total < W)
		distribute(st->v[P_JUSTIFY_CONTENT].u.str, W - total, live(&cols),
			&xoff, &xs);
	total = span_size(&rows, 0, rows.n, rg);
	if (H >= 0 && total < H)
		distribute(st->v[P_ALIGN_CONTENT].u.str, H - total, live(&rows),
			&yoff, &ys);
	for (i = 0; i < n; i++) {
		struct box *b = v[i].b;
		float ax = gb->x + xoff + track_pos(&cols, v[i].c0, cg + xs) ;
		float ay = gb->y + yoff + track_pos(&rows, v[i].r0, rg + ys);
		float aw = span_size(&cols, v[i].c0, v[i].c1, cg);
		float ah = span_size(&rows, v[i].r0, v[i].r1, rg);
		const char *js = b->style->v[P_JUSTIFY_SELF].u.str;
		const char *as = b->style->v[P_ALIGN_SELF].u.str;
		int jal = align_kw(js && strcmp(js, "auto") ? js :
			st->v[P_JUSTIFY_ITEMS].u.str, b->type == B_REPLACED ?
			'f' : 's');
		int aal = align_kw(as && strcmp(as, "auto") ? as :
			st->v[P_ALIGN_ITEMS].u.str, b->type == B_REPLACED ?
			'f' : 's');
		float ew = b->m[1] + b->m[3] + b->b[1] + b->b[3] + b->p[1] +
			b->p[3];
		float eh = b->m[0] + b->m[2] + b->b[0] + b->b[2] + b->p[0] +
			b->p[2];
		float dx = 0, dy = 0;

		if (jal == 's' && is_auto(b->style, P_WIDTH) &&
		    b->type != B_REPLACED &&
		    b->w != box_clamp_w(b, aw - ew, aw)) {
			layout_item(b, 0, 0, box_clamp_w(b, aw - ew, aw), -1);
		} else if (jal != 's') {
			/* shrink to fit when not stretched */
			if (is_auto(b->style, P_WIDTH) &&
			    b->type != B_REPLACED) {
				float mn, mx, w;

				content_sizes(b, &mn, &mx);
				w = mx < aw - ew ? mx : aw - ew;
				if (w < mn)
					w = mn;
				layout_item(b, 0, 0, w, -1);
			}
			if (jal == 'c')
				dx = (aw - b->w - ew) / 2;
			else if (jal == 'e')
				dx = aw - b->w - ew;
		}
		if (v[i].bl) {
			const float *la = rasc + 4 * v[i].r0;

			dy = v[i].bl == 1 ? la[0] - v[i].asc :
				ah - la[1] - v[i].asc;
		} else if (aal == 's' && is_auto(b->style, P_HEIGHT) &&
		    b->type != B_REPLACED && !box_has_ratio(b))
			b->h = box_clamp_h(b, ah - eh, ah);
		else if (aal == 'c')
			dy = (ah - b->h - eh) / 2;
		else if (aal == 'e')
			dy = ah - b->h - eh;
		translate(b, ax + dx + b->m[3] + b->b[3] + b->p[3] - b->x,
			ay + dy + b->m[0] + b->b[0] + b->p[0] - b->y);
		{
			float rx, ry;

			rel_offset(b, W, &rx, &ry);
			if (rx || ry)
				translate(b, rx, ry);
		}
	}
	total = yoff + span_size(&rows, 0, rows.n, rg + ys);
	{
		/* absolute children: their static position rectangle,
		 * the content box, and their grid area */
		float ch = box_clamp_h(gb, H >= 0 ? H : total, cbh);
		float px0 = gb->x - gb->p[3], py0 = gb->y - gb->p[0];
		float px1 = gb->x + W + gb->p[1];
		float py1 = gb->y + ch + gb->p[2];

		for (i = 0; i < nav; i++) {
			struct gitem *a = &av[i];
			const struct style *is = a->b->style;
			float x0 = line_at(&cols, a->c0, 0, cg + xs, px0,
				gb->x + xoff), x1 = line_at(&cols, a->c1, 1,
				cg + xs, px1, gb->x + xoff);
			float y0 = line_at(&rows, a->r0, 0, rg + ys, py0,
				gb->y + yoff), y1 = line_at(&rows, a->r1, 1,
				rg + ys, py1, gb->y + yoff);

			abs_rect(a->b, gb->x, gb->y, W, ch,
				abs_align(is->v[P_JUSTIFY_SELF].u.str,
					st->v[P_JUSTIFY_ITEMS].u.str),
				abs_align(is->v[P_ALIGN_SELF].u.str,
					st->v[P_ALIGN_ITEMS].u.str));
			abs_area(a->b, x0, y0, x1 > x0 ? x1 - x0 : 0,
				y1 > y0 ? y1 - y0 : 0);
		}
		free(av);
	}
	free(rasc);
	free(v);
	free_tracks(&cols);
	free_tracks(&rows);
	return H >= 0 ? H : total;
}

void grid_sizes(struct box *gb, float *mn, float *mx)
{
	struct tracks cols, rows;
	struct gitem *v;
	float cg = gapv(gb->style, P_COLUMN_GAP, 0), unit = 0;
	int n, i;

	n = place(gb, -1, -1, &cols, &rows, &v, cg, 0, NULL, NULL);
	column_contributions(&cols, v, n, -1, cg);
	*mn = *mx = 0;
	/* flexible tracks share one size per fr: the largest max-content
	 * per fr of them (CSS Grid 1, 12.7.1), four 1fr columns of cards
	 * are four times the widest card */
	for (i = 0; i < cols.n; i++) {
		struct track *k = &cols.t[i];

		if (k->max.kind == TK_FR && k->max.v > 0 &&
		    k->cmax / k->max.v > unit)
			unit = k->cmax / k->max.v;
	}
	for (i = 0; i < cols.n; i++) {
		struct track *k = &cols.t[i];
		float a = k->min.kind == TK_LEN ? k->min.v : k->cmin;
		float b = k->max.kind == TK_LEN ? k->max.v :
			k->max.kind == TK_FR ? unit * k->max.v : k->cmax;

		if (b < a)
			b = a;
		*mn += a;
		*mx += b;
	}
	*mn += cg * (float)(cols.n > 1 ? cols.n - 1 : 0);
	*mx += cg * (float)(cols.n > 1 ? cols.n - 1 : 0);
	free(v);
	free_tracks(&cols);
	free_tracks(&rows);
}
