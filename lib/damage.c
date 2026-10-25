/*
 * Damage: what a layout changed in the painting, for a host that paints
 * only that (soie_layout_damage()). Before the layout the old box tree
 * is summed up per element: a signature of what its boxes paint
 * themselves (rects, style, text, the state of a control or an image)
 * and where, alone and with its subtree; after it, the new tree is, and
 * an element whose signature changed, came or went gives its rects, old
 * and new: its subtree's when its style changed (opacity, clipping and
 * stacking reach below). Asked once, the document keeps track.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "layout.h"

#define NDAMAGE 32

struct damage {
	float r[NDAMAGE][4];	/* x0 y0 x1 y1, page coordinates */
	int n, all;
	unsigned long media;	/* players: painted from their state */
};

/* An element's boxes: the signature of their own painting, its style,
 * their extent alone (e) and with what is below (s) */
struct dent {
	const soie_node *n;
	uint64_t sig;
	const struct style *st;
	float e[4], s[4];
	int seen;
};

struct table {
	struct dent *v;
	size_t n, cap;
};

/* The old tree's, between damage_before() and damage_after() */
static struct table before;
static float before_w, before_h;
static int before_ok;

static void none(float *r)
{
	r[0] = r[1] = 1e30f;
	r[2] = r[3] = -1e30f;
}

static void unite(float *r, float x0, float y0, float x1, float y1)
{
	if (x0 < r[0])
		r[0] = x0;
	if (y0 < r[1])
		r[1] = y0;
	if (x1 > r[2])
		r[2] = x1;
	if (y1 > r[3])
		r[3] = y1;
}

static uint64_t mix(uint64_t h, const void *p, size_t n)
{
	const unsigned char *b = p;
	size_t i;

	for (i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ULL;
	return h;
}

#define MIX(h, v) ((h) = mix((h), &(v), sizeof(v)))

static size_t slot(const struct table *t, const soie_node *n)
{
	size_t i = ((uintptr_t)n >> 4) * 0x9e3779b97f4a7c15ULL & (t->cap - 1);

	while (t->v[i].n && t->v[i].n != n)
		i = (i + 1) & (t->cap - 1);
	return i;
}

/* Boxes of elements below b: the table is made big enough once, its
 * entries are not moved while walking holds them */
static size_t count(const struct box *b)
{
	const struct box *c;
	size_t n = b->node && b->node->type == SOIE_ELEMENT;

	for (c = b->first; c; c = c->next)
		n += count(c);
	return n;
}

static void table_make(struct table *t, const struct box *root)
{
	size_t n = count(root) * 2 + 1;

	t->cap = 1024;
	while (t->cap < n)
		t->cap *= 2;
	t->n = 0;
	t->v = calloc(t->cap, sizeof(*t->v));
	if (!t->v)
		abort();
}

static struct dent *entry(struct table *t, const soie_node *n)
{
	size_t i = slot(t, n);

	if (!t->v[i].n) {
		t->v[i].n = n;
		t->v[i].sig = 0xcbf29ce484222325ULL;
		none(t->v[i].e);
		none(t->v[i].s);
		t->n++;
	}
	return &t->v[i];
}

static const struct dent *find(const struct table *t, const soie_node *n)
{
	size_t i;

	if (!t->cap)
		return NULL;
	i = slot(t, n);
	return t->v[i].n ? &t->v[i] : NULL;
}

static void table_free(struct table *t)
{
	free(t->v);
	memset(t, 0, sizeof(*t));
}

/* Painted from the DOM below them or their state, not their boxes:
 * images, SVG, form controls; players change all the time */
static int dom_painted(const soie_node *n)
{
	static const char *const names[] = { "img", "svg", "input",
		"textarea", "select", "button", "object", "embed", "iframe",
		"canvas", "progress", "meter" };
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (!strcmp(n->name, names[i]))
			return 1;
	return 0;
}

static void walk(struct table *t, const struct box *b, struct dent *d,
	unsigned long media, float *sub)
{
	const struct style *st = b->style;
	float s[4], x0, y0, x1, y1, o;
	const struct box *c;
	int i;

	none(s);
	if (b->node && b->node->type == SOIE_ELEMENT) {
		d = entry(t, b->node);
		if (!(b->flags & BF_PSEUDO))
			d->st = st;
		if (dom_painted(b->node))
			MIX(d->sig, b->node->lchg);
		if (!strcmp(b->node->name, "video") ||
		    !strcmp(b->node->name, "audio"))
			MIX(d->sig, media);
	}
	if (d) {
		MIX(d->sig, b->type);
		MIX(d->sig, b->flags);
		MIX(d->sig, b->pseudo);
		MIX(d->sig, st);
		MIX(d->sig, b->x);
		MIX(d->sig, b->y);
		MIX(d->sig, b->w);
		MIX(d->sig, b->h);
		d->sig = mix(d->sig, b->b, sizeof(b->b));
		d->sig = mix(d->sig, b->p, sizeof(b->p));
		MIX(d->sig, b->nlines);
		/* the border box, and an outline around it */
		o = 0;
		if (st)
			o = st->v[P_OUTLINE_WIDTH].u.len.px +
				(st->v[P_OUTLINE_OFFSET].u.len.px > 0 ?
				st->v[P_OUTLINE_OFFSET].u.len.px : 0);
		x0 = b->x - b->p[3] - b->b[3] - o;
		y0 = b->y - b->p[0] - b->b[0] - o;
		x1 = b->x + b->w + b->p[1] + b->b[1] + o;
		y1 = b->y + b->h + b->p[2] + b->b[2] + o;
		unite(d->e, x0, y0, x1, y1);
		unite(s, x0, y0, x1, y1);
		for (i = 0; i < b->nfrags; i++) {
			const struct frag *f = &b->frags[i];
			const struct style *fs = f->box->style;
			float em = fs ? fs->v[P_FONT_SIZE].u.len.px : 16;

			MIX(d->sig, f->kind);
			MIX(d->sig, f->x);
			MIX(d->sig, f->y);
			MIX(d->sig, f->w);
			MIX(d->sig, f->h);
			MIX(d->sig, f->baseline);
			MIX(d->sig, fs);
			if (f->kind == F_TEXT && f->box->text)
				d->sig = mix(d->sig, f->box->text + f->start,
					f->len);
			/* glyphs reach out of their frag, as when painted */
			unite(d->e, f->x - em, f->baseline - 2 * em,
				f->x + f->w + em, f->baseline + em);
			unite(d->e, f->x, f->y, f->x + f->w, f->y + f->h);
			unite(s, f->x - em, f->baseline - 2 * em,
				f->x + f->w + em, f->baseline + em);
			unite(s, f->x, f->y, f->x + f->w, f->y + f->h);
		}
		for (i = 0; i < b->nlines; i++)
			MIX(d->sig, b->lines[i].jspace);
	}
	for (c = b->first; c; c = c->next)
		walk(t, c, d, media, s);
	if (b->node && b->node->type == SOIE_ELEMENT)
		unite(d->s, s[0], s[1], s[2], s[3]);
	unite(sub, s[0], s[1], s[2], s[3]);
}

static void add(struct damage *dm, const float *r)
{
	int i;

	if (dm->all || r[0] >= r[2] || r[1] >= r[3])
		return;
	if (dm->n == NDAMAGE) {
		/* too many: their union */
		for (i = 1; i < NDAMAGE; i++)
			unite(dm->r[0], dm->r[i][0], dm->r[i][1], dm->r[i][2],
				dm->r[i][3]);
		dm->n = 1;
	}
	memcpy(dm->r[dm->n++], r, sizeof(*dm->r));
}

/* The root and body paint the canvas: their style is everywhere */
static int canvas_element(const soie_node *n)
{
	return is_html(n, TAG_HTML) || is_html(n, TAG_BODY);
}

void damage_before(soie_node *doc)
{
	const struct layout *l = doc->u.doc.layout;
	float s[4];

	before_ok = 0;
	if (!doc->u.doc.damage || !l || !l->root) {
		if (doc->u.doc.damage)
			doc->u.doc.damage->all = 1;
		return;
	}
	none(s);
	table_make(&before, l->root);
	walk(&before, l->root, NULL, doc->u.doc.damage->media, s);
	before_w = l->width;
	before_h = l->height;
	before_ok = 1;
}

void damage_after(soie_node *doc)
{
	struct damage *dm = doc->u.doc.damage;
	const struct layout *l = doc->u.doc.layout;
	struct table now = { NULL, 0, 0 };
	struct dent *o;
	size_t i;
	float s[4];

	if (!dm || !before_ok || !l || !l->root || l->width != before_w ||
	    l->height != before_h) {
		if (dm)
			dm->all = 1;
		table_free(&before);
		return;
	}
	dm->media++;
	none(s);
	table_make(&now, l->root);
	walk(&now, l->root, NULL, dm->media, s);
	for (i = 0; i < now.cap && !dm->all; i++) {
		const struct dent *e = &now.v[i];

		if (!e->n)
			continue;
		if (!(o = (struct dent *)find(&before, e->n))) {
			add(dm, e->e);
			continue;
		}
		o->seen = 1;
		if (o->st != e->st) {
			if (canvas_element(e->n))
				dm->all = 1;
			add(dm, o->s);
			add(dm, e->s);
		} else if (o->sig != e->sig) {
			add(dm, o->e);
			add(dm, e->e);
		}
	}
	for (i = 0; i < before.cap && !dm->all; i++)
		if (before.v[i].n && !before.v[i].seen)
			add(dm, before.v[i].e);
	table_free(&before);
	table_free(&now);
}

/* An element's box painted again (the focus moved, its caret) */
void damage_node(soie_node *doc, const soie_node *el)
{
	struct damage *dm = doc->u.doc.damage;
	const struct layout *l = doc->u.doc.layout;
	struct table t = { NULL, 0, 0 };
	const struct dent *e;
	float s[4];

	if (!dm || !el || !l || !l->root)
		return;
	none(s);
	table_make(&t, l->root);
	walk(&t, l->root, NULL, dm->media, s);
	if ((e = find(&t, el)))
		add(dm, e->s);
	table_free(&t);
}

void damage_free(soie_node *doc)
{
	free(doc->u.doc.damage);
	doc->u.doc.damage = NULL;
}

int soie_layout_damage(soie_node *doc, float *rects, int max)
{
	struct damage *dm;
	int i, n;

	if (doc->type != SOIE_DOCUMENT)
		return -1;
	if (!(dm = doc->u.doc.damage)) {
		/* asked the first time: tracked from now on */
		dm = calloc(1, sizeof(*dm));
		if (!dm)
			return -1;
		doc->u.doc.damage = dm;
		return -1;
	}
	/* more than asked: their union */
	if (!dm->all && dm->n > max && max > 0) {
		for (i = 1; i < dm->n; i++)
			unite(dm->r[0], dm->r[i][0], dm->r[i][1], dm->r[i][2],
				dm->r[i][3]);
		dm->n = 1;
	}
	n = dm->all || dm->n > max ? -1 : dm->n;
	for (i = 0; i < n; i++) {
		rects[i * 4] = dm->r[i][0];
		rects[i * 4 + 1] = dm->r[i][1];
		rects[i * 4 + 2] = dm->r[i][2] - dm->r[i][0];
		rects[i * 4 + 3] = dm->r[i][3] - dm->r[i][1];
	}
	dm->n = 0;
	dm->all = 0;
	return n;
}
