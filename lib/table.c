/*
 * Tables (CSS 2 chapter 17): anonymous table objects, the automatic and
 * fixed table layout algorithms, colspan and rowspan, border-spacing,
 * captions and the vertical alignment of cells.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "layout.h"

/* Anonymous table objects (17.2.1) */

static int proper_child(const struct box *t, const struct box *c)
{
	switch (t->role) {
	case R_TABLE:
		/* rows directly in a table get an anonymous row group */
		return c->role == R_CAPTION || c->role == R_ROWGROUP ||
			c->role == R_COLGROUP || c->role == R_COL;
	case R_ROWGROUP:
		return c->role == R_ROW;
	case R_ROW:
		return c->role == R_CELL;
	}
	return 0;
}

static int spaces_only(const struct box *c)
{
	size_t i;

	if (c->type != B_TEXT)
		return 0;
	for (i = 0; i < c->len; i++)
		if (c->text[i] != ' ' && c->text[i] != '\t' &&
		    c->text[i] != '\n' && c->text[i] != '\r' &&
		    c->text[i] != '\f')
			return 0;
	return 1;
}

static struct box *anon(struct styledb *db, const struct box *parent,
	int role)
{
	struct box *b = box_new(B_BLOCK, anon_style(db, parent->style), NULL);

	b->flags = BF_ANON;
	b->role = (unsigned char)role;
	return b;
}

/* The anonymous parent a misplaced child needs inside t. */
static int wrapper_role(int parent)
{
	return parent == R_ROW ? R_CELL : parent == R_ROWGROUP ? R_ROW :
		R_ROWGROUP;
}

/*
 * Make the children of a table, row group or row proper, wrapping the
 * others in anonymous rows and cells. Whitespace between parts goes.
 */
static void fix_table_part(struct styledb *db, struct box *t)
{
	struct box tmp, *c, *n, *run = NULL;

	memset(&tmp, 0, sizeof(tmp));
	for (c = t->first; c; c = n) {
		n = c->next;
		if (proper_child(t, c)) {
			run = NULL;
			box_append(&tmp, c);
			continue;
		}
		if (spaces_only(c) && (!run || t->role != R_ROW)) {
			box_free(c);
			continue;
		}
		if (!run) {
			run = anon(db, t, wrapper_role(t->role));
			box_append(&tmp, run);
		}
		box_append(run, c);
	}
	t->first = tmp.first;
	t->last = tmp.last;
	for (c = t->first; c; c = c->next) {
		c->parent = t;
		if (c->role == R_ROWGROUP || c->role == R_ROW)
			fix_table_part(db, c);
	}
}

/* Cells and rows outside a table get an anonymous table around them. */
static void wrap_orphans(struct styledb *db, struct box *b)
{
	struct box tmp, *c, *n, *run = NULL;

	memset(&tmp, 0, sizeof(tmp));
	for (c = b->first; c; c = n) {
		n = c->next;
		if (c->role == R_CELL || c->role == R_ROW ||
		    c->role == R_ROWGROUP || c->role == R_CAPTION) {
			if (!run) {
				run = anon(db, b, R_TABLE);
				box_append(&tmp, run);
			}
			box_append(run, c);
			continue;
		}
		if (run && spaces_only(c)) {
			box_free(c);
			continue;
		}
		run = NULL;
		box_append(&tmp, c);
	}
	b->first = tmp.first;
	b->last = tmp.last;
	for (c = b->first; c; c = c->next)
		c->parent = b;
}

void table_fixup(struct styledb *db, struct box *b)
{
	if (b->role == R_TABLE || b->role == R_ROWGROUP || b->role == R_ROW)
		fix_table_part(db, b);
	else
		wrap_orphans(db, b);
}

/* Layout */

struct tcell {
	struct box *box;
	int row, col, rspan, cspan;
};

struct tgrid {
	struct box **rows;
	int nrows;
	struct tcell *cells;
	int ncells;
	int ncols;
	float *cmin, *cmax, *cw, *cpct;
	char *cfix;		/* column with a fixed width */
	float *rh, *ry;
	float hs, vs;		/* border-spacing */
};

static int span_attr(const struct box *c, const char *name)
{
	const char *a;
	int n;

	if (!c->node || !(a = soie_element_get_attr(c->node, name)))
		return 1;
	n = atoi(a);
	if (n < 1)
		n = !strcmp(name, "rowspan") && *a == '0' ? 1 : 1;
	return n > 1000 ? 1000 : n;
}

static void add_rows(struct tgrid *g, struct box *grp)
{
	struct box *r;

	for (r = grp->first; r; r = r->next)
		if (r->role == R_ROW) {
			g->rows = xrealloc(g->rows, (size_t)(g->nrows + 1) *
				sizeof(*g->rows));
			g->rows[g->nrows++] = r;
		}
}

static int is_group(const struct box *b, const char *display)
{
	return b->role == R_ROWGROUP && kw(b->style, P_DISPLAY, display);
}

static void build_grid(struct box *t, struct tgrid *g)
{
	struct box *c, *cell;
	char *occ = NULL;
	int occ_rows = 0, i, r, col;

	memset(g, 0, sizeof(*g));
	/* thead first, tfoot last (17.2) */
	for (c = t->first; c; c = c->next)
		if (is_group(c, "table-header-group"))
			add_rows(g, c);
	for (c = t->first; c; c = c->next)
		if (c->role == R_ROWGROUP && !is_group(c, "table-header-group") &&
		    !is_group(c, "table-footer-group"))
			add_rows(g, c);
	for (c = t->first; c; c = c->next)
		if (is_group(c, "table-footer-group"))
			add_rows(g, c);
	for (r = 0; r < g->nrows; r++) {
		col = 0;
		for (cell = g->rows[r]->first; cell; cell = cell->next) {
			struct tcell *tc;
			int rs, cs, k, j;

			if (cell->role != R_CELL)
				continue;
			cs = span_attr(cell, "colspan");
			rs = span_attr(cell, "rowspan");
			if (r + rs > g->nrows)
				rs = g->nrows - r;
			/* skip slots taken by rowspans from above */
			while (col < g->ncols && r < occ_rows &&
			       occ[r * 1024 + col])
				col++;
			if (col + cs > 1024)
				break;
			if (r + rs > occ_rows) {
				occ = xrealloc(occ, (size_t)(r + rs) * 1024);
				memset(occ + occ_rows * 1024, 0,
					(size_t)(r + rs - occ_rows) * 1024);
				occ_rows = r + rs;
			}
			for (k = r; k < r + rs; k++)
				for (j = col; j < col + cs; j++)
					occ[k * 1024 + j] = 1;
			g->cells = xrealloc(g->cells, (size_t)(g->ncells + 1) *
				sizeof(*g->cells));
			tc = &g->cells[g->ncells++];
			tc->box = cell;
			tc->row = r;
			tc->col = col;
			tc->rspan = rs;
			tc->cspan = cs;
			col += cs;
			if (col > g->ncols)
				g->ncols = col;
			/* a later cell may sit after a rowspan: recheck */
			while (r < occ_rows && col < 1024 && occ[r * 1024 + col])
				col++;
		}
	}
	free(occ);
	(void)i;
	g->cmin = calloc((size_t)g->ncols + 1, sizeof(float));
	g->cmax = calloc((size_t)g->ncols + 1, sizeof(float));
	g->cw = calloc((size_t)g->ncols + 1, sizeof(float));
	g->cpct = calloc((size_t)g->ncols + 1, sizeof(float));
	g->cfix = calloc((size_t)g->ncols + 1, 1);
	g->rh = calloc((size_t)g->nrows + 1, sizeof(float));
	g->ry = calloc((size_t)g->nrows + 1, sizeof(float));
	if (!g->cmin || !g->cmax || !g->cw || !g->cpct || !g->rh || !g->ry ||
	    !g->cfix)
		abort();
}

static void free_grid(struct tgrid *g)
{
	free(g->rows);
	free(g->cells);
	free(g->cmin);
	free(g->cmax);
	free(g->cw);
	free(g->cpct);
	free(g->cfix);
	free(g->rh);
	free(g->ry);
}

static float cell_hedges(struct box *c, float cbw)
{
	box_edges(c, cbw);
	return c->b[1] + c->b[3] + c->p[1] + c->p[3];
}

/* Column minimum and maximum widths (17.5.2.2, simplified). */
static void column_sizes(struct box *t, struct tgrid *g, float avail)
{
	int pass, i, k;

	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < g->ncells; i++) {
			struct tcell *tc = &g->cells[i];
			struct box *c = tc->box;
			const struct cval *w = &c->style->v[P_WIDTH];
			float mn, mx, e = cell_hedges(c, avail), smn = 0, smx = 0;

			if ((tc->cspan == 1) != (pass == 0))
				continue;
			content_sizes(c, &mn, &mx);
			if (!w->kw && !w->u.len.pct && !w->u.len.calc) {
				/* a fixed width is a minimum and the maximum */
				float fw = w->u.len.px;

				if (kw(c->style, P_BOX_SIZING, "border-box"))
					fw -= e;
				if (fw > mn)
					mn = fw;
				mx = mn > fw ? mn : fw;
				if (tc->cspan == 1)
					g->cfix[tc->col] = 1;
			} else if (!w->kw && w->u.len.pct && tc->cspan == 1 &&
				   w->u.len.pct > g->cpct[tc->col]) {
				g->cpct[tc->col] = w->u.len.pct;
			}
			mn += e;
			mx += e;
			if (tc->cspan == 1) {
				if (mn > g->cmin[tc->col])
					g->cmin[tc->col] = mn;
				if (mx > g->cmax[tc->col])
					g->cmax[tc->col] = mx;
				continue;
			}
			/* spanning cells: spread what is missing */
			for (k = tc->col; k < tc->col + tc->cspan; k++) {
				smn += g->cmin[k];
				smx += g->cmax[k];
			}
			smn += g->hs * (float)(tc->cspan - 1);
			smx += g->hs * (float)(tc->cspan - 1);
			for (k = tc->col; k < tc->col + tc->cspan; k++) {
				float share = smx > 0 ? g->cmax[k] / (smx - g->hs *
					(float)(tc->cspan - 1)) :
					1.0f / (float)tc->cspan;

				if (mn > smn)
					g->cmin[k] += (mn - smn) * share;
				if (mx > smx)
					g->cmax[k] += (mx - smx) * share;
			}
		}
	for (i = 0; i < g->ncols; i++)
		if (g->cmax[i] < g->cmin[i])
			g->cmax[i] = g->cmin[i];
	(void)t;
}

/* The used width of every column for a content width W. */
static void distribute(struct tgrid *g, float W)
{
	float smin = 0, smax = 0, sfree = 0;
	float room = W - g->hs * (float)(g->ncols + 1);
	int i, nfree = 0;

	for (i = 0; i < g->ncols; i++) {
		if (g->cpct[i] > 0) {
			float p = room * g->cpct[i] / 100;

			if (p > g->cmin[i])
				g->cmin[i] = g->cmax[i] = p;
			g->cfix[i] = 1;
		}
		smin += g->cmin[i];
		smax += g->cmax[i];
		if (!g->cfix[i]) {
			sfree += g->cmax[i];
			nfree++;
		}
	}
	for (i = 0; i < g->ncols; i++) {
		if (room <= smin)
			g->cw[i] = g->cmin[i];
		else if (room <= smax)
			g->cw[i] = g->cmin[i] + (g->cmax[i] - g->cmin[i]) *
				(room - smin) / (smax - smin);
		else if (nfree && g->cfix[i])
			g->cw[i] = g->cmax[i];	/* extra goes to auto columns */
		else if (nfree)
			g->cw[i] = g->cmax[i] + (sfree > 0 ? (room - smax) *
				g->cmax[i] / sfree : (room - smax) /
				(float)nfree);
		else
			g->cw[i] = g->cmax[i] + (smax > 0 ? (room - smax) *
				g->cmax[i] / smax : (room - smax) /
				(float)g->ncols);
	}
}

/* table-layout: fixed (17.5.2.1): the first row decides. */
static void fixed_columns(struct tgrid *g, float W)
{
	float room = W - g->hs * (float)(g->ncols + 1), used = 0;
	int i, k, free_cols = 0;

	for (i = 0; i < g->ncols; i++)
		g->cw[i] = -1;
	for (i = 0; i < g->ncells && g->cells[i].row == 0; i++) {
		struct tcell *tc = &g->cells[i];
		const struct cval *w = &tc->box->style->v[P_WIDTH];

		if (w->kw)
			continue;
		for (k = tc->col; k < tc->col + tc->cspan; k++)
			g->cw[k] = (len_val(w, room) + cell_hedges(tc->box,
				room)) / (float)tc->cspan;
	}
	for (i = 0; i < g->ncols; i++)
		if (g->cw[i] < 0)
			free_cols++;
		else
			used += g->cw[i];
	for (i = 0; i < g->ncols; i++)
		if (g->cw[i] < 0)
			g->cw[i] = free_cols && room > used ?
				(room - used) / (float)free_cols : 0;
}

static float span_width(const struct tgrid *g, const struct tcell *tc)
{
	float w = g->hs * (float)(tc->cspan - 1);
	int k;

	for (k = tc->col; k < tc->col + tc->cspan; k++)
		w += g->cw[k];
	return w;
}

static void valign(struct box *c, float room)
{
	struct box *k;
	float dy = 0;

	if (room <= 0)
		return;
	if (kw(c->style, P_VERTICAL_ALIGN, "middle"))
		dy = room / 2;
	else if (kw(c->style, P_VERTICAL_ALIGN, "bottom"))
		dy = room;
	if (!dy)
		return;
	c->lok = 0;	/* its content moved, not the cell */
	for (k = c->first; k; k = k->next)
		translate_flow(k, 0, dy);
	{
		int i;

		for (i = 0; i < c->nlines; i++) {
			c->lines[i].y += dy;
			c->lines[i].baseline += dy;
		}
		/* atomic boxes of the lines moved with the boxes above */
		for (i = 0; i < c->nfrags; i++) {
			c->frags[i].y += dy;
			c->frags[i].baseline += dy;
		}
	}
}

/*
 * Lay out table t at the origin (content box at 0,0): avail is the
 * content width it may take. Sets t->w, t->h and every part's box.
 */
void layout_table(struct box *t, float avail)
{
	struct tgrid g;
	const struct style *st = t->style;
	struct box *c;
	float W, mn = 0, mx = 0, y = 0, spec, cap_top = 0;
	int i, r, collapse = kw(st, P_BORDER_COLLAPSE, "collapse");

	build_grid(t, &g);
	g.hs = g.vs = collapse ? 0 : st->v[P_BORDER_SPACING].u.len.px;
	column_sizes(t, &g, avail);
	for (i = 0; i < g.ncols; i++) {
		mn += g.cmin[i];
		mx += g.cmax[i];
	}
	mn += g.hs * (float)(g.ncols + 1);
	mx += g.hs * (float)(g.ncols + 1);
	spec = is_auto(st, P_WIDTH) ? -1 : len_val(&st->v[P_WIDTH], avail);
	if (spec >= 0 && kw(st, P_BOX_SIZING, "border-box"))
		spec -= t->b[1] + t->b[3] + t->p[1] + t->p[3];
	if (spec >= 0)
		W = spec > mn ? spec : mn;
	else
		W = mx < avail ? mx : avail > mn ? avail : mn;
	if (kw(st, P_TABLE_LAYOUT, "fixed") && spec >= 0) {
		W = spec;
		fixed_columns(&g, W);
	} else {
		distribute(&g, W);
	}
	t->w = W;
	t->x = 0;
	t->y = 0;
	/* captions on top */
	for (c = t->first; c; c = c->next)
		if (c->role == R_CAPTION && !kw(c->style, P_CAPTION_SIDE,
						 "bottom")) {
			layout_cell(c, 0, y, W);
			y = c->y + c->h + c->p[2] + c->b[2] + c->m[2];
		}
	cap_top = y;
	/* cells at their widths, to know the row heights */
	for (i = 0; i < g.ncells; i++) {
		struct tcell *tc = &g.cells[i];
		float x = g.hs, h;
		int k;

		for (k = 0; k < tc->col; k++)
			x += g.cw[k] + g.hs;
		cell_hedges(tc->box, W);
		layout_cell(tc->box, x, 0, span_width(&g, tc));
		h = tc->box->h + tc->box->p[0] + tc->box->p[2] +
			tc->box->b[0] + tc->box->b[2];
		if (tc->rspan == 1 && h > g.rh[tc->row])
			g.rh[tc->row] = h;
	}
	for (r = 0; r < g.nrows; r++) {
		const struct cval *h = &g.rows[r]->style->v[P_HEIGHT];

		if (!h->kw && !h->u.len.pct && h->u.len.px > g.rh[r])
			g.rh[r] = h->u.len.px;
	}
	for (i = 0; i < g.ncells; i++) {
		struct tcell *tc = &g.cells[i];
		float h = tc->box->h + tc->box->p[0] + tc->box->p[2] +
			tc->box->b[0] + tc->box->b[2], have = 0;
		int k;

		if (tc->rspan == 1)
			continue;
		for (k = tc->row; k < tc->row + tc->rspan; k++)
			have += g.rh[k];
		have += g.vs * (float)(tc->rspan - 1);
		if (h > have)
			g.rh[tc->row + tc->rspan - 1] += h - have;
	}
	y += g.vs;
	for (r = 0; r < g.nrows; r++) {
		g.ry[r] = y;
		y += g.rh[r] + g.vs;
	}
	/* place cells: stretch to their rows, align their content */
	for (i = 0; i < g.ncells; i++) {
		struct tcell *tc = &g.cells[i];
		struct box *cb = tc->box;
		float rh = g.vs * (float)(tc->rspan - 1), inner, used;
		int k;

		for (k = tc->row; k < tc->row + tc->rspan; k++)
			rh += g.rh[k];
		translate(cb, 0, g.ry[tc->row] - (cb->y - cb->p[0] - cb->b[0]));
		inner = rh - cb->p[0] - cb->p[2] - cb->b[0] - cb->b[2];
		used = cb->h;
		cb->h = inner;
		valign(cb, inner - used);
	}
	/* rows and groups cover their cells */
	for (r = 0; r < g.nrows; r++) {
		struct box *row = g.rows[r];

		row->x = g.hs;
		row->w = W - 2 * g.hs;
		row->y = g.ry[r];
		row->h = g.rh[r];
		memset(row->p, 0, sizeof(row->p));
		memset(row->b, 0, sizeof(row->b));
	}
	for (c = t->first; c; c = c->next) {
		struct box *rw;
		float top = 1e30f, bot = -1e30f;

		if (c->role != R_ROWGROUP)
			continue;
		for (rw = c->first; rw; rw = rw->next) {
			if (rw->y < top)
				top = rw->y;
			if (rw->y + rw->h > bot)
				bot = rw->y + rw->h;
		}
		c->x = g.hs;
		c->w = W - 2 * g.hs;
		c->y = top < 1e29f ? top : y;
		c->h = bot > top ? bot - top : 0;
		memset(c->p, 0, sizeof(c->p));
		memset(c->b, 0, sizeof(c->b));
	}
	if (!g.nrows)
		y = cap_top;
	/* captions at the bottom */
	for (c = t->first; c; c = c->next)
		if (c->role == R_CAPTION && kw(c->style, P_CAPTION_SIDE,
					       "bottom")) {
			layout_cell(c, 0, y, W);
			y = c->y + c->h + c->p[2] + c->b[2] + c->m[2];
		}
	t->h = y;
	free_grid(&g);
}
