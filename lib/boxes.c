/*
 * The box tree: generation from the DOM and computed styles (CSS 2
 * chapter 9, CSS Display 3), anonymous boxes, generated content, list
 * markers. Also the layout dump used by tests.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"
#include "font.h"
#include "image.h"

/* Keyword test with a small cache: callers pass string literals, the
 * slot is a hash of the literal's address and prop. */
int kw(const struct style *st, int prop, const char *name)
{
	static struct {
		int prop;
		const char *name;
		int kw;
	} cache[256];
	unsigned i = ((unsigned)((uintptr_t)name >> 3) ^ (unsigned)prop *
		31u) & 255;
	const char *list;
	int k;

	if (cache[i].name == name && cache[i].prop == prop)
		return st->v[prop].kw == cache[i].kw;
	list = props[prop].keywords;
	for (k = 1; *list; k++) {
		const char *e = strchr(list, '|');
		size_t len = e ? (size_t)(e - list) : strlen(list);

		if (len == strlen(name) && !memcmp(list, name, len))
			break;
		if (!e) {
			k = -1;
			break;
		}
		list = e + 1;
	}
	cache[i].prop = prop;
	cache[i].name = name;
	cache[i].kw = k;
	return st->v[prop].kw == k;
}

struct box *box_new(int type, const struct style *st, soie_node *node)
{
	struct box *b = xmalloc(sizeof(*b));

	memset(b, 0, sizeof(*b));
	b->type = (unsigned char)type;
	b->style = st;
	b->node = node;
	b->iw = b->ih = -1;
	return b;
}

void box_append(struct box *parent, struct box *child)
{
	child->parent = parent;
	child->next = NULL;
	if (parent->last)
		parent->last->next = child;
	else
		parent->first = child;
	parent->last = child;
}

void box_free(struct box *b)
{
	struct box *c, *n;

	if (!b)
		return;
	for (c = b->first; c; c = n) {
		n = c->next;
		box_free(c);
	}
	free(b->text);
	free(b->lines);
	free(b->frags);
	free(b->absv);
	free(b);
}

static void forget(struct box *b)
{
	struct box *c;

	if (b->node && b->node->type == SOIE_ELEMENT &&
	    b->node->u.el.box == b)
		b->node->u.el.box = NULL;
	for (c = b->first; c; c = c->next)
		forget(c);
}

void box_drop(struct box *b)
{
	if (!b)
		return;
	forget(b);
	box_free(b);
}

static int block_level(const struct box *b)
{
	if (b->flags & (BF_FLOAT | BF_ABS))
		return 0;	/* out of flow: fine anywhere */
	return b->type == B_BLOCK ||
		(b->type == B_REPLACED && (b->flags & BF_BLOCKLEVEL));
}

static int is_replaced(const soie_node *n)
{
	if (n->ns == SOIE_NS_SVG)
		return n->tag == TAG_SVG;
	if (n->ns != SOIE_NS_HTML)
		return 0;
	switch (n->tag) {
	case TAG_IMG: case TAG_IFRAME: case TAG_EMBED: case TAG_OBJECT:
	case TAG_INPUT: case TAG_TEXTAREA: case TAG_SELECT:
		return 1;
	}
	return !strcmp(n->name, "video") || !strcmp(n->name, "canvas") ||
		!strcmp(n->name, "audio");
}

static float attr_num(const soie_node *n, const char *name, float dflt)
{
	const char *a = soie_element_get_attr(n, name);

	return a && *a >= '0' && *a <= '9' ? (float)atof(a) : dflt;
}

/* Intrinsic sizes of replaced elements, until images load (phase 6).
 * CSS px are zoomed (doc_zoom()), em ones are already. */
static void intrinsic(struct box *b, const soie_node *n)
{
	float em = b->style->v[P_FONT_SIZE].u.len.px, z = doc_zoom();
	const struct image *im;

	if (n->ns == SOIE_NS_SVG) {
		svg_size(n, em / z, &b->iw, &b->ih);
		if (!b->iw && !b->ih) {
			b->iw = 300;
			b->ih = 150;
		}
		b->iw *= z;
		b->ih *= z;
	} else if ((n->ns == SOIE_NS_HTML && (n->tag == TAG_IMG ||
		    n->tag == TAG_OBJECT || n->tag == TAG_EMBED ||
		    n->tag == TAG_INPUT)) && (im = image_for(n))) {
		b->iw = (float)im->w * z;
		b->ih = (float)im->h * z;
	} else if (n->ns == SOIE_NS_HTML && n->tag == TAG_IMG) {
		b->iw = attr_num(n, "width", 0) * z;
		b->ih = attr_num(n, "height", 0) * z;
	} else if (n->ns == SOIE_NS_HTML && n->tag == TAG_INPUT) {
		const char *t = soie_element_get_attr(n, "type");
		struct fontm fm;

		font_metrics(b->style, &fm);
		if (t && (ascii_ieq(t, "checkbox") || ascii_ieq(t, "radio"))) {
			b->iw = b->ih = 13 * z;
		} else if (t && (ascii_ieq(t, "submit") ||
				 ascii_ieq(t, "reset") ||
				 ascii_ieq(t, "button"))) {
			const char *v = soie_element_get_attr(n, "value");

			if (!v)
				v = ascii_ieq(t, "reset") ? "Reset" :
					ascii_ieq(t, "submit") ? "Submit" : "";
			b->iw = text_width(b->style, v, strlen(v));
			b->ih = fm.normal_lh;
		} else {
			b->iw = attr_num(n, "size", 20) * em * 0.5f;
			b->ih = fm.normal_lh;
		}
	} else if (n->ns == SOIE_NS_HTML && n->tag == TAG_TEXTAREA) {
		b->iw = attr_num(n, "cols", 20) * em * 0.5f + 4;
		b->ih = attr_num(n, "rows", 2) * em * 1.25f + 2;
	} else if (n->ns == SOIE_NS_HTML && n->tag == TAG_SELECT) {
		b->iw = em * 5;
		b->ih = em * 1.25f + 2;
	} else if (n->ns == SOIE_NS_HTML && !strcmp(n->name, "audio")) {
		b->iw = 300 * z;
		b->ih = 54 * z;
	} else {
		b->iw = 300;
		b->ih = 150;
		if (n->ns == SOIE_NS_HTML && !strcmp(n->name, "video"))
			media_size(n, &b->iw, &b->ih);
		b->iw = attr_num(n, "width", b->iw) * z;
		b->ih = attr_num(n, "height", b->ih) * z;
	}
}

static void roman(struct buf *b, int n, int upper);
static void alpha(struct buf *b, int n, int upper);

/*
 * CSS counters (CSS Lists 3, 4): the counters alive while the box tree
 * is built, in tree order. One made by counter-reset on an element is
 * seen by that element, its descendants and its later siblings with
 * theirs, until its parent ends: level is the depth of the element
 * that made it, counters deeper than an element die when it ends.
 */
static struct counter {
	char name[48];
	int value, level;
} *ctrs;
static int nctrs, capctrs, clevel, counters_seen;

static struct counter *counter_find(const char *name)
{
	int i;

	for (i = nctrs - 1; i >= 0; i--)
		if (!strcmp(ctrs[i].name, name))
			return &ctrs[i];
	return NULL;
}

static struct counter *counter_new(const char *name, int value)
{
	struct counter *k;
	int i;

	/* one of an earlier sibling, same name: this one takes its place */
	for (i = nctrs - 1; i >= 0 && ctrs[i].level >= clevel; i--)
		if (ctrs[i].level == clevel && !strcmp(ctrs[i].name, name)) {
			ctrs[i].value = value;
			return &ctrs[i];
		}
	if (nctrs == capctrs) {
		capctrs = capctrs ? capctrs * 2 : 16;
		ctrs = xrealloc(ctrs, (size_t)capctrs * sizeof(*ctrs));
	}
	k = &ctrs[nctrs++];
	snprintf(k->name, sizeof(k->name), "%s", name);
	k->value = value;
	k->level = clevel;
	return k;
}

/* counter-reset, counter-increment, counter-set: `name [integer]`
 * pairs; op 'r', 'i' or 's' */
static void counter_ops(const char *s, int op)
{
	if (!s || !strcmp(s, "none"))
		return;
	counters_seen = 1;
	while (*s) {
		char name[48];
		struct counter *k;
		size_t n = 0;
		int v = op == 'i', has = 0;
		char *end;

		while (*s == ' ')
			s++;
		while (*s && *s != ' ' && n < sizeof(name) - 1)
			name[n++] = *s++;
		name[n] = '\0';
		while (*s && *s != ' ')
			s++;
		while (*s == ' ')
			s++;
		if (*s == '-' || *s == '+' || (*s >= '0' && *s <= '9')) {
			long x = strtol(s, &end, 10);

			v = x > 1000000000 ? 1000000000 : x < -1000000000 ?
				-1000000000 : (int)x;
			has = 1;
			s = end;
		}
		(void)has;
		if (!n || !strcmp(name, "none"))
			continue;
		if (op == 'r') {
			counter_new(name, v);
			continue;
		}
		if (!(k = counter_find(name)))
			k = counter_new(name, 0);
		if (op == 'i')
			k->value = k->value > 1000000000 - v && v > 0 ?
				k->value : k->value + v;
		else
			k->value = v;
	}
}

static void counters_enter(const struct style *st)
{
	clevel++;
	counter_ops(st->v[P_COUNTER_RESET].u.str, 'r');
	counter_ops(st->v[P_COUNTER_INCREMENT].u.str, 'i');
	counter_ops(st->v[P_COUNTER_SET].u.str, 's');
}

static void counters_leave(void)
{
	while (nctrs && ctrs[nctrs - 1].level > clevel)
		nctrs--;
	clevel--;
}

/* A counter's value as its style writes it */
static void counter_text(struct buf *b, int n, const char *style)
{
	char num[16];

	if (style && (!strcmp(style, "none")))
		return;
	if (style && (!strcmp(style, "disc") || !strcmp(style, "circle") ||
	    !strcmp(style, "square"))) {
		buf_add(b, *style == 'd' ? "\xe2\x80\xa2" : *style == 'c' ?
			"\xe2\x97\xa6" : "\xe2\x96\xaa", 3);
	} else if (style && strstr(style, "roman") && n >= 1 && n <= 3999) {
		roman(b, n, *style == 'u');
	} else if (style && (strstr(style, "alpha") || strstr(style, "latin")) &&
	    n >= 1) {
		alpha(b, n, *style == 'u');
	} else {
		snprintf(num, sizeof(num), style && !strcmp(style,
			"decimal-leading-zero") ? "%02d" : "%d", n);
		buf_add(b, num, strlen(num));
	}
}

/* counter(name, style) and counters(name, "sep", style) */
static void counter_fn(struct buf *b, const struct cv *f, int all)
{
	struct cur k;
	const struct cv *v, *name, *sep = NULL, *style = NULL;
	int i, first = 1;

	cur_init(&k, &f->kids);
	name = cur_next(&k);
	if (!name || name->type != CT_IDENT)
		return;
	counters_seen = 1;
	while ((v = cur_next(&k))) {
		if (v->type == CT_STRING && all && !sep)
			sep = v;
		else if (v->type == CT_IDENT)
			style = v;
	}
	if (!all) {
		const struct counter *c = counter_find(name->str);

		counter_text(b, c ? c->value : 0, style ? style->str : NULL);
		return;
	}
	for (i = 0; i < nctrs; i++) {
		if (strcmp(ctrs[i].name, name->str))
			continue;
		if (!first && sep)
			buf_add(b, sep->str, strlen(sep->str));
		counter_text(b, ctrs[i].value, style ? style->str : NULL);
		first = 0;
	}
	if (first)
		counter_text(b, 0, style ? style->str : NULL);
}

/* Text of ::before/::after: strings, attr(), quotes, counters. */
static char *content_text(const char *content, const soie_node *el)
{
	struct cvlist l;
	struct buf b = { 0 };
	int i;

	buf_add(&b, "", 0);
	css_component_values(content, strlen(content), &l);
	for (i = 0; i < l.n; i++) {
		const struct cv *v = &l.v[i];

		if (v->type == CT_STRING) {
			buf_add(&b, v->str, strlen(v->str));
		} else if (cv_ident(v, "open-quote")) {
			buf_add(&b, "\xe2\x80\x9c", 3);
		} else if (cv_ident(v, "close-quote")) {
			buf_add(&b, "\xe2\x80\x9d", 3);
		} else if (cv_func(v, "attr") && v->kids.n &&
			   v->kids.v[0].type == CT_IDENT) {
			const char *a = soie_element_get_attr(el,
				v->kids.v[0].str);

			if (a)
				buf_add(&b, a, strlen(a));
		} else if (cv_func(v, "counter") || cv_func(v, "counters")) {
			counter_fn(&b, v, cv_func(v, "counters"));
		}
		/* url(): not yet */
	}
	cvlist_free(&l);
	return b.s;
}

static struct box *text_box(const struct style *st, const char *s,
	size_t len)
{
	struct box *t = box_new(B_TEXT, st, NULL);

	t->text = xstrndup(s, len);
	t->len = len;
	return t;
}

static void roman(struct buf *b, int n, int upper)
{
	static const int val[] = { 1000, 900, 500, 400, 100, 90, 50, 40,
		10, 9, 5, 4, 1 };
	static const char *const sym[] = { "m", "cm", "d", "cd", "c", "xc",
		"l", "xl", "x", "ix", "v", "iv", "i" };
	int i;
	size_t k;

	for (i = 0; i < 13; i++)
		while (n >= val[i]) {
			for (k = 0; sym[i][k]; k++)
				buf_putc(b, upper ? sym[i][k] - 32 : sym[i][k]);
			n -= val[i];
		}
}

static void alpha(struct buf *b, int n, int upper)
{
	char s[16];
	int i = 15;

	s[i] = '\0';
	while (n > 0 && i > 0) {
		n--;
		s[--i] = (char)((upper ? 'A' : 'a') + n % 26);
		n /= 26;
	}
	buf_add(b, s + i, strlen(s + i));
}

/* Ordinal of a list item: <ol start>, <li value>, reversed. */
static int ordinal(const soie_node *li)
{
	const soie_node *p = li->parent, *c;
	int n, step = 1;
	const char *a;

	if (p && is_html(p, TAG_OL) && soie_element_get_attr(p, "reversed")) {
		step = -1;
		n = 0;
		for (c = p->first; c; c = c->next)
			if (is_html(c, TAG_LI))
				n++;
		if ((a = soie_element_get_attr(p, "start")))
			n = atoi(a);
	} else {
		n = p && is_html(p, TAG_OL) &&
			(a = soie_element_get_attr(p, "start")) ? atoi(a) : 1;
	}
	for (c = p ? p->first : li; c && c != li; c = c->next)
		if (c->type == SOIE_ELEMENT && c->u.el.style &&
		    kw(c->u.el.style, P_DISPLAY, "list-item")) {
			if ((a = soie_element_get_attr(c, "value")))
				n = atoi(a);
			n += step;
		}
	if ((a = soie_element_get_attr(li, "value")))
		n = atoi(a);
	return n;
}

static struct box *marker(const soie_node *el, const struct style *st)
{
	const char *t = st->v[P_LIST_STYLE_TYPE].u.str;
	struct buf b = { 0 };
	struct box *m;

	if (!t || !strcmp(t, "none"))
		return NULL;
	buf_add(&b, "", 0);
	if (t[0] == '"') {
		struct cvlist l;

		css_component_values(t, strlen(t), &l);
		if (l.n && l.v[0].type == CT_STRING)
			buf_add(&b, l.v[0].str, strlen(l.v[0].str));
		cvlist_free(&l);
	} else if (!strcmp(t, "disc")) {
		buf_add(&b, "\xe2\x80\xa2 ", 4);
	} else if (!strcmp(t, "circle")) {
		buf_add(&b, "\xe2\x97\xa6 ", 4);
	} else if (!strcmp(t, "square")) {
		buf_add(&b, "\xe2\x96\xaa ", 4);
	} else if (!strcmp(t, "disclosure-closed")) {
		buf_add(&b, "\xe2\x96\xb8 ", 4);
	} else if (!strcmp(t, "disclosure-open")) {
		buf_add(&b, "\xe2\x96\xbe ", 4);
	} else {
		int n = ordinal(el);
		char num[16];

		/* roman numerals stop at 3999: decimal beyond, as browsers
		 * (a value of two billions was two million letters) */
		if ((!strcmp(t, "lower-roman") || !strcmp(t, "upper-roman")) &&
		    n >= 1 && n <= 3999)
			roman(&b, n, t[0] == 'u');
		else if (!strcmp(t, "lower-alpha") ||
			 !strcmp(t, "lower-latin") ||
			 !strcmp(t, "upper-alpha") || !strcmp(t, "upper-latin"))
			alpha(&b, n, t[0] == 'u');
		else {
			snprintf(num, sizeof(num), "%d", n);
			buf_add(&b, num, strlen(num));
		}
		buf_add(&b, ". ", 2);
	}
	m = box_new(B_INLINE, st, NULL);
	m->flags = BF_ANON | BF_PSEUDO;
	m->pseudo = 'm';
	if (kw(st, P_LIST_STYLE_POSITION, "outside"))
		m->flags |= BF_OUTSIDE;
	box_append(m, text_box(st, b.s, b.len));
	buf_free(&b);
	return m;
}

static int box_type(const struct style *st)
{
	if (kw(st, P_DISPLAY, "inline") || kw(st, P_DISPLAY, "ruby") ||
	    kw(st, P_DISPLAY, "ruby-text"))
		return B_INLINE;
	if (kw(st, P_DISPLAY, "inline-block") ||
	    kw(st, P_DISPLAY, "inline-flex") ||
	    kw(st, P_DISPLAY, "inline-grid") ||
	    kw(st, P_DISPLAY, "inline-table"))
		return B_ATOMIC;
	return B_BLOCK;	/* block, flow-root, list-item, flex, grid, table* */
}

static int table_role(const struct style *st)
{
	if (kw(st, P_DISPLAY, "table") || kw(st, P_DISPLAY, "inline-table"))
		return R_TABLE;
	if (kw(st, P_DISPLAY, "table-row-group") ||
	    kw(st, P_DISPLAY, "table-header-group") ||
	    kw(st, P_DISPLAY, "table-footer-group"))
		return R_ROWGROUP;
	if (kw(st, P_DISPLAY, "table-row"))
		return R_ROW;
	if (kw(st, P_DISPLAY, "table-cell"))
		return R_CELL;
	if (kw(st, P_DISPLAY, "table-caption"))
		return R_CAPTION;
	if (kw(st, P_DISPLAY, "table-column-group"))
		return R_COLGROUP;
	if (kw(st, P_DISPLAY, "table-column"))
		return R_COL;
	return R_NONE;
}

static void set_flags(struct box *b)
{
	const struct style *st = b->style;

	b->role = (unsigned char)table_role(st);
	if (!kw(st, P_FLOAT, "none"))
		b->flags |= BF_FLOAT;
	if (kw(st, P_POSITION, "absolute") || kw(st, P_POSITION, "fixed"))
		b->flags |= BF_ABS;
}

static void gen_children(struct box *parent, soie_node *el);

/*
 * Incremental layout: the previous tree, while the new one is built.
 * An element whose subtree did not change since keeps its box with
 * everything below, when that box makes no part of what is around:
 * a block-level or atomic box (its content starts lines of its own),
 * not a table part (fixed up with its siblings) nor a list item (its
 * number counts the ones before).
 */
static struct layout *old_tree, *new_tree;

void layout_touch(soie_node *n)
{
	soie_node *doc = n->doc;
	unsigned long built = doc->u.doc->lbuilt, s = ++doc->u.doc->lstamp;

	/* an ancestor changed since the last layout: so did its own */
	for (; n && n->lchg <= built; n = n->parent)
		n->lchg = s;
}

static void intrinsic(struct box *b, const soie_node *n);

/* The images and videos below came with the same sizes */
static int replaced_same(const struct box *b)
{
	const struct box *c;
	struct box t;

	if (b->type == B_REPLACED && b->node) {
		t = *b;
		intrinsic(&t, b->node);
		if (t.iw != b->iw || t.ih != b->ih)
			return 0;
	}
	for (c = b->first; c; c = c->next)
		if (!replaced_same(c))
			return 0;
	return 1;
}

/* The boxes below belong to the new tree now */
static void retag(struct box *b)
{
	struct box *c;

	if (b->node && b->node->type == SOIE_ELEMENT &&
	    b->node->u.el.box == b)
		b->node->u.el.box_tree = new_tree->serial;
	for (c = b->first; c; c = c->next)
		retag(c);
}

/* A list item numbered by its place among its siblings */
static int numbered(const struct style *st)
{
	const char *t = st->v[P_LIST_STYLE_TYPE].u.str;

	return t && strcmp(t, "none") && t[0] != '"' && strcmp(t, "disc") &&
		strcmp(t, "circle") && strcmp(t, "square") &&
		strncmp(t, "disclosure-", 11);
}

static struct box *kept_box(soie_node *n)
{
	struct box *b = n->u.el.box, *p, *c, *prev = NULL;

	if (!old_tree || old_tree->counters ||
	    n->u.el.box_tree != old_tree->serial || !b ||
	    b->node != n || b->style != n->u.el.style ||
	    n->lchg > old_tree->built)
		return NULL;
	if ((b->type != B_BLOCK && b->type != B_ATOMIC) || b->role ||
	    (b->flags & BF_PSEUDO) || (kw(b->style, P_DISPLAY, "list-item") &&
	    numbered(b->style)) ||
	    !replaced_same(b))
		return NULL;
	/* out of the old tree */
	if ((p = b->parent)) {
		for (c = p->first; c && c != b; c = c->next)
			prev = c;
		if (!c)
			return NULL;
		if (prev)
			prev->next = b->next;
		else
			p->first = b->next;
		if (p->last == b)
			p->last = prev;
	}
	b->parent = b->next = NULL;
	b->kept = 1;
	retag(b);
	return b;
}

/*
 * Box tree depth limit: layout is recursive. Deeper elements give their
 * content to the box at the limit, like display: contents (Blink caps
 * the DOM at 512 levels for the same reason).
 */
#define MAX_DEPTH 512
static int depth;

/* Past the limit: the text of the subtree, walked without recursion. */
static void gen_flat(struct box *parent, soie_node *el)
{
	soie_node *n = el->first;

	while (n && n != el) {
		if (n->type == SOIE_TEXT && n->u.text.len && n->parent->u.el.style &&
		    !kw(n->parent->u.el.style, P_DISPLAY, "none"))
			box_append(parent, text_box(n->parent->u.el.style,
				n->u.text.data, n->u.text.len));
		if (n->first && n->type == SOIE_ELEMENT && n->u.el.style &&
		    !kw(n->u.el.style, P_DISPLAY, "none")) {
			n = n->first;
			continue;
		}
		while (n != el && !n->next)
			n = n->parent;
		if (n != el)
			n = n->next;
	}
}

static struct box *gen_pseudo(soie_node *el, const struct style *st,
	int which)
{
	struct box *b;
	char *text;

	if (!st)
		return NULL;
	b = box_new(box_type(st), st, NULL);
	b->flags = BF_PSEUDO;
	b->pseudo = (unsigned char)which;
	set_flags(b);
	/* a pseudo-element counts as a child would (h2::before {
	 * counter-increment: section }) */
	counters_enter(st);
	text = content_text(st->v[P_CONTENT].u.str, el);
	counters_leave();
	if (*text)
		box_append(b, text_box(st, text, strlen(text)));
	free(text);
	return b;
}

static void gen_element(struct box *parent, soie_node *n);

static void gen_node(struct box *parent, soie_node *n)
{
	const struct style *st = n->type == SOIE_ELEMENT ? n->u.el.style :
		NULL;

	/* an element with a box (or display: contents) and its counters */
	if (st && !kw(st, P_DISPLAY, "none")) {
		counters_enter(st);
		gen_element(parent, n);
		counters_leave();
	} else {
		gen_element(parent, n);
	}
}

static void gen_element(struct box *parent, soie_node *n)
{
	const struct style *st;
	struct box *b, *m;

	if (n->type == SOIE_TEXT) {
		if (n->u.text.len)
			box_append(parent, text_box(parent->style ?
				parent->style : NULL, n->u.text.data,
				n->u.text.len));
		return;
	}
	if (n->type != SOIE_ELEMENT || !(st = n->u.el.style))
		return;
	if (kw(st, P_DISPLAY, "none"))
		return;
	if (depth >= MAX_DEPTH) {
		gen_flat(parent, n);
		return;
	}
	if (kw(st, P_DISPLAY, "contents")) {
		depth++;	/* no box, a level of recursion all the same */
		gen_children(parent, n);
		depth--;
		return;
	}
	if (is_html(n, TAG_BR)) {
		box_append(parent, b = box_new(B_BREAK, st, n));
		n->u.el.box = b;
		n->u.el.box_tree = new_tree->serial;
		return;
	}
	if ((b = kept_box(n))) {
		box_append(parent, b);
		return;
	}
	if (is_replaced(n)) {
		b = box_new(B_REPLACED, st, n);
		n->u.el.box = b;
		n->u.el.box_tree = new_tree->serial;
		if (box_type(st) == B_BLOCK)
			b->flags |= BF_BLOCKLEVEL;
		set_flags(b);
		intrinsic(b, n);
		box_append(parent, b);
		return;
	}
	b = box_new(box_type(st), st, n);
	n->u.el.box = b;
	n->u.el.box_tree = new_tree->serial;
	set_flags(b);
	if (kw(st, P_DISPLAY, "list-item") && (m = marker(n, st)))
		box_append(b, m);
	depth++;
	gen_children(b, n);
	depth--;
	box_append(parent, b);
}

/* A closed <details> shows its first <summary>, nothing else */
static soie_node *closed_summary(soie_node *el)
{
	soie_node *c;

	if (!is_html(el, TAG_DETAILS) || soie_element_get_attr(el, "open"))
		return NULL;
	for (c = el->first; c; c = c->next)
		if (is_html(c, TAG_SUMMARY))
			return c;
	return el;	/* none: nothing shown */
}

static void gen_children(struct box *parent, soie_node *el)
{
	soie_node *c, *only = closed_summary(el);
	struct box *p;

	if ((p = gen_pseudo(el, el->u.el.before, 'b')))
		box_append(parent, p);
	for (c = el->first; c; c = c->next) {
		if (only && c != only)
			continue;
		/* text takes the style of its parent element */
		if (c->type == SOIE_TEXT) {
			if (c->u.text.len)
				box_append(parent, text_box(el->u.el.style,
					c->u.text.data, c->u.text.len));
		} else {
			gen_node(parent, c);
		}
	}
	if ((p = gen_pseudo(el, el->u.el.after, 'a')))
		box_append(parent, p);
}

static int has_block_inside(const struct box *b)
{
	const struct box *c;

	for (c = b->first; c; c = c->next)
		if (block_level(c) || (c->type == B_INLINE &&
				       has_block_inside(c)))
			return 1;
	return 0;
}

static struct box *inline_clone(const struct box *b)
{
	struct box *c = box_new(B_INLINE, b->style, b->node);

	c->flags = b->flags;
	return c;
}

/*
 * Block-in-inline (CSS 2 9.2.1.1): an inline box holding a block is
 * split around it. Appends the pieces of b to out (a list via next).
 */
static void split_inline(struct box *b, struct box *out)
{
	struct box *cur = inline_clone(b), *c, *n;

	for (c = b->first; c; c = n) {
		n = c->next;
		if (block_level(c)) {
			box_append(out, cur);
			box_append(out, c);
			cur = inline_clone(b);
		} else if (c->type == B_INLINE && has_block_inside(c)) {
			struct box tmp, *p, *pn;

			memset(&tmp, 0, sizeof(tmp));
			split_inline(c, &tmp);
			for (p = tmp.first; p; p = pn) {
				pn = p->next;
				if (block_level(p)) {
					box_append(out, cur);
					box_append(out, p);
					cur = inline_clone(b);
				} else {
					box_append(cur, p);
				}
			}
		} else {
			box_append(cur, c);
		}
	}
	box_append(out, cur);
	b->first = b->last = NULL;
	box_drop(b);
}

static int collapsible_only(const struct box *b)
{
	const struct box *c;
	size_t i;

	if (b->type == B_TEXT) {
		if (!kw(b->style, P_WHITE_SPACE_COLLAPSE, "collapse"))
			return 0;
		for (i = 0; i < b->len; i++)
			if (b->text[i] != ' ' && b->text[i] != '\n' &&
			    b->text[i] != '\t' && b->text[i] != '\r' &&
			    b->text[i] != '\f')
				return 0;
		return 1;
	}
	if (b->type != B_INLINE || (b->flags & BF_PSEUDO))
		return 0;
	for (c = b->first; c; c = c->next)
		if (!collapsible_only(c))
			return 0;
	/* an empty span with padding or borders still makes a line */
	return 1;
}

static void fixup(struct styledb *db, struct box *b);

/* Flex and grid containers: their text becomes anonymous items. */
int is_flexgrid(const struct box *b)
{
	const struct style *st = b->style;

	return st && (b->type == B_BLOCK || b->type == B_ATOMIC) && !b->role &&
		(kw(st, P_DISPLAY, "flex") || kw(st, P_DISPLAY, "inline-flex") ||
		 kw(st, P_DISPLAY, "grid") || kw(st, P_DISPLAY, "inline-grid"));
}

/* Block containers below b, inline boxes crossed. */
static void fixup_inside(struct styledb *db, struct box *b)
{
	struct box *c;

	for (c = b->first; c; c = c->next) {
		c->parent = b;
		if (c->kept)
			continue;	/* fixed up in the old tree */
		if (c->type == B_BLOCK || c->type == B_ATOMIC)
			fixup(db, c);
		else if (c->type == B_INLINE)
			fixup_inside(db, c);
	}
}

/* Block containers: wrap inline runs next to blocks (CSS 2 9.2.1.1). */
static void fixup(struct styledb *db, struct box *b)
{
	struct box tmp, *c, *n, *run = NULL;
	int blocks = 0;

	if (b->role == R_COL || b->role == R_COLGROUP) {
		/* nothing in them but a column group's columns: they give
		 * the table's columns their widths (table.c) */
		struct box *keep = NULL, *kl = NULL;

		while (b->first) {
			c = b->first;
			b->first = c->next;
			c->next = NULL;
			if (b->role == R_COLGROUP && c->role == R_COL) {
				if (kl)
					kl->next = c;
				else
					keep = c;
				kl = c;
				while (c->first) {
					n = c->first;
					c->first = n->next;
					box_drop(n);
				}
				c->last = NULL;
			} else {
				box_drop(c);
			}
		}
		b->first = keep;
		b->last = kl;
		return;
	}
	table_fixup(db, b);
	if (b->role == R_TABLE || b->role == R_ROWGROUP || b->role == R_ROW) {
		/* table parts hold table parts only: no line boxes */
		fixup_inside(db, b);
		return;
	}
	/* split inlines that hold blocks */
	memset(&tmp, 0, sizeof(tmp));
	for (c = b->first; c; c = n) {
		n = c->next;
		if (c->type == B_INLINE && has_block_inside(c))
			split_inline(c, &tmp);
		else
			box_append(&tmp, c);
	}
	b->first = tmp.first;
	b->last = tmp.last;
	for (c = b->first; c; c = c->next) {
		c->parent = b;
		if (block_level(c))
			blocks = 1;
	}
	fixup_inside(db, b);
	if (!blocks && !is_flexgrid(b)) {
		b->flags |= BF_INLINE_KIDS;
		return;
	}
	/* anonymous blocks around inline runs; whitespace runs vanish */
	memset(&tmp, 0, sizeof(tmp));
	for (c = b->first; c; c = n) {
		n = c->next;
		if (block_level(c)) {
			run = NULL;
			box_append(&tmp, c);
			continue;
		}
		if (!run) {
			run = box_new(B_BLOCK, anon_style(db, b->style), NULL);
			run->flags = BF_ANON | BF_INLINE_KIDS;
			box_append(&tmp, run);
		}
		box_append(run, c);
	}
	b->first = b->last = NULL;
	for (c = tmp.first; c; c = n) {
		n = c->next;
		if ((c->flags & BF_ANON) && c->type == B_BLOCK) {
			struct box *k, *kn;
			int keep = 0;

			for (k = c->first; k; k = k->next)
				if (!(k->flags & (BF_FLOAT | BF_ABS)) &&
				    !collapsible_only(k))
					keep = 1;
			if (!keep) {
				/* floats and abs boxes between blocks need
				 * no wrapper; collapsed spaces go away */
				for (k = c->first; k; k = kn) {
					kn = k->next;
					if (k->flags & (BF_FLOAT | BF_ABS))
						box_append(b, k);
					else
						box_drop(k);
				}
				c->first = c->last = NULL;
				box_drop(c);
				continue;
			}
		}
		box_append(b, c);
	}
}

struct box *build_boxes(soie_node *doc)
{
	soie_node *root = soie_document_element(doc);
	struct box *b;

	if (!root || root->type != SOIE_ELEMENT || !root->u.el.style || !doc->u.doc->styles ||
	    kw(root->u.el.style, P_DISPLAY, "none"))
		return NULL;
	b = box_new(B_BLOCK, root->u.el.style, root);
	b->flags = BF_BFC;
	root->u.el.box = b;
	root->u.el.box_tree = new_tree->serial;
	depth = 1;
	gen_children(b, root);
	fixup(doc->u.doc->styles, b);
	return b;
}

/* Dump */

static void indent(struct buf *b, int depth)
{
	int i;

	buf_add(b, "| ", 2);
	for (i = 0; i < depth; i++)
		buf_add(b, "  ", 2);
}

static void num(struct buf *b, float v)
{
	serialize_float(b, v);
}

static void rect(struct buf *b, float x, float y, float w, float h)
{
	buf_putc(b, ' ');
	num(b, x);
	buf_putc(b, ',');
	num(b, y);
	buf_putc(b, ' ');
	num(b, w);
	buf_putc(b, 'x');
	num(b, h);
}

static void name(struct buf *b, const struct box *x)
{
	if (x->node) {
		buf_putc(b, '<');
		buf_add(b, x->node->name, strlen(x->node->name));
		buf_putc(b, '>');
	} else if (x->pseudo) {
		const char *p = x->pseudo == 'b' ? "::before" :
			x->pseudo == 'a' ? "::after" : "::marker";

		buf_add(b, p, strlen(p));
	} else {
		buf_add(b, "anonymous", 9);
	}
}

static void dump_box(struct buf *b, const struct box *x, int depth);

static void dump_quoted(struct buf *b, const char *s, size_t n)
{
	buf_putc(b, ' ');
	buf_putc(b, '"');
	buf_add(b, s, n);
	buf_putc(b, '"');
}

/* Out-of-flow boxes inside an inline formatting context. */
static void dump_oof(struct buf *b, const struct box *x, int depth)
{
	const struct box *c;

	for (c = x->first; c; c = c->next) {
		if (c->flags & (BF_FLOAT | BF_ABS))
			dump_box(b, c, depth);
		else if (c->type == B_INLINE)
			dump_oof(b, c, depth);
	}
}

static void dump_box(struct buf *b, const struct box *x, int depth)
{
	static const char *const kinds[] = { "block", "inline",
		"inline-block", "text", "br", "replaced" };
	const struct box *c;
	int i, j;

	indent(b, depth);
	buf_add(b, kinds[x->type], strlen(kinds[x->type]));
	if (x->flags & BF_FLOAT)
		buf_add(b, " float", 6);
	if (x->flags & BF_ABS)
		buf_add(b, " abs", 4);
	buf_putc(b, ' ');
	name(b, x);
	rect(b, x->x - x->p[3] - x->b[3], x->y - x->p[0] - x->b[0],
		x->w + x->p[1] + x->p[3] + x->b[1] + x->b[3],
		x->h + x->p[0] + x->p[2] + x->b[0] + x->b[2]);
	buf_putc(b, '\n');
	if (x->type == B_REPLACED)
		return;
	if (x->flags & BF_INLINE_KIDS) {
		for (i = 0; i < x->nlines; i++) {
			const struct line *l = &x->lines[i];

			indent(b, depth + 1);
			buf_add(b, "line", 4);
			rect(b, l->x, l->y, l->w, l->h);
			buf_putc(b, '\n');
			for (j = l->first; j < l->first + l->n; j++) {
				const struct frag *f = &x->frags[j];

				if (f->kind == F_ATOMIC) {
					dump_box(b, f->box, depth + 2);
					continue;
				}
				indent(b, depth + 2);
				if (f->kind == F_TEXT) {
					buf_add(b, "text", 4);
					dump_quoted(b, f->box->text + f->start,
						f->len);
				} else {
					buf_add(b, "inline ", 7);
					name(b, f->box);
				}
				rect(b, f->x, f->y, f->w, f->h);
				buf_putc(b, '\n');
			}
		}
		dump_oof(b, x, depth + 1);
		return;
	}
	for (c = x->first; c; c = c->next)
		dump_box(b, c, depth + 1);
}

char *layout_dump(const struct box *root, size_t *len)
{
	struct buf b = { 0 };

	buf_add(&b, "", 0);
	if (root)
		dump_box(&b, root, 0);
	if (len)
		*len = b.len;
	return b.s;
}

/* Entry points */

void layout_free(struct layout *l)
{
	box_free(l->root);
	free(l);
}

int soie_layout_document(soie_node *doc, int width, int height)
{
	static unsigned long serials;
	struct layout *l, *old = doc->u.doc->layout;

	if (doc->type != SOIE_DOCUMENT || !doc->u.doc->styles)
		return -1;
	l = xmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->width = width > 0 ? (float)width : 1024;
	l->height = height > 0 ? (float)height : 768;
	l->serial = ++serials;
	l->built = doc->u.doc->lstamp;
	l->rules = style_serial(doc);
	/* the text widths stay: their styles live as long as the rules
	 * (styledb_free() clears them), the fonts too (fonts_drop_*) */
	font_doc = doc;
	damage_before(doc);
	old_tree = old && old->rules == l->rules ? old : NULL;
	new_tree = l;
	nctrs = clevel = counters_seen = 0;
	l->root = build_boxes(doc);
	l->counters = counters_seen;
	old_tree = new_tree = NULL;
	if (old)
		layout_free(old);	/* what was not kept */
	doc->u.doc->layout = NULL;
	doc->u.doc->lbuilt = l->built;
	if (l->root)
		layout_root(l);
	doc->u.doc->layout = l;
	doc->u.doc->lay_w = (int)l->width;
	doc->u.doc->lay_h = (int)l->height;
	damage_after(doc);
	return 0;
}

float soie_layout_height(const soie_node *doc)
{
	return doc->type == SOIE_DOCUMENT && doc->u.doc->layout ?
		doc->u.doc->layout->doc_height : 0;
}

char *soie_layout_dump(const soie_node *doc, size_t *len)
{
	return layout_dump(doc->type == SOIE_DOCUMENT && doc->u.doc->layout ?
		doc->u.doc->layout->root : NULL, len);
}

/* Hit testing and geometry */

static soie_node *node_of(const struct box *b)
{
	for (; b; b = b->parent)
		if (b->node)
			return b->node;
	return NULL;
}

static int inside(float x, float y, float bx, float by, float bw, float bh)
{
	return x >= bx && y >= by && x < bx + bw && y < by + bh;
}

/*
 * Hit testing follows painting (paint.c): every positioned box is a
 * layer of its own, over the flow, by z-index then tree order; within
 * a layer, floats over blocks, the text and atomics of the lines.
 */
int positioned(const struct box *b)
{
	return b->style && !kw(b->style, P_POSITION, "static");
}

/* visibility: hidden and pointer-events: none are not hit, their
 * descendants may be (both inherit, a child can set them back) */
static int hittable(const struct style *st)
{
	return !st || (kw(st, P_VISIBILITY, "visible") &&
		!kw(st, P_POINTER_EVENTS, "none"));
}

static soie_node *hit(struct box *b, float x, float y, int depth);

/*
 * Boxes deeper than this are not hit: hit() recurses, a hostile page
 * nesting thousands of elements would run it out of stack.
 */
#define HIT_DEPTH 1024

/* Floats inside the inline boxes of b: the last one under x, y */
static soie_node *hit_in_inline(struct box *b, float x, float y, int depth)
{
	struct box *c;
	soie_node *r, *top = NULL;

	if (depth > HIT_DEPTH)
		return NULL;
	for (c = b->first; c; c = c->next)
		if (c->type == B_INLINE && !positioned(c)) {
			if ((r = hit_in_inline(c, x, y, depth + 1)))
				top = r;
		} else if ((c->flags & BF_FLOAT) && !positioned(c) &&
			   b->type == B_INLINE && (r = hit(c, x, y, depth + 1))) {
			top = r;
		}
	return top;
}

/* b's layer: b and its descendants, but the positioned ones */
static soie_node *hit(struct box *b, float x, float y, int depth)
{
	struct box *c, *some[32], **kids = some;
	int i, n = 0;
	soie_node *r = NULL;

	if (depth > HIT_DEPTH)
		return NULL;
	for (c = b->first; c; c = c->next)
		n++;
	if (n > 32)
		kids = xmalloc((size_t)n * sizeof(*kids));
	for (n = 0, c = b->first; c; c = c->next)
		kids[n++] = c;
	/* floats are over the rest */
	for (i = n - 1; i >= 0 && !r; i--)
		if ((kids[i]->flags & BF_FLOAT) && !positioned(kids[i]))
			r = hit(kids[i], x, y, depth + 1);
	if (!r && (b->flags & BF_INLINE_KIDS)) {
		const struct box *in = NULL;
		int deep = -1;

		r = hit_in_inline(b, x, y, depth + 1);
		/* text and atomics first: an inline box's frag comes after
		 * its content and spans it, an outer one after an inner */
		for (i = b->nfrags - 1; i >= 0 && !r; i--) {
			struct frag *f = &b->frags[i];

			if (f->kind == F_ATOMIC) {
				if (!positioned(f->box))
					r = hit(f->box, x, y, depth + 1);
			} else if (f->kind == F_TEXT &&
				   inside(x, y, f->x, f->y, f->w, f->h) &&
				   hittable(f->box->style)) {
				r = node_of(f->box->parent);
			}
		}
		/* then, between texts (padding, spaces), the innermost
		 * inline box there */
		for (i = 0; i < b->nfrags && !r; i++) {
			struct frag *f = &b->frags[i];
			const struct box *k;
			int d = 0;

			if (f->kind != F_INLINE ||
			    !inside(x, y, f->x, f->y, f->w, f->h) ||
			    !hittable(f->box->style))
				continue;
			for (k = f->box; k && k != b; k = k->parent)
				d++;
			if (d > deep) {
				deep = d;
				in = f->box;
			}
		}
		if (!r && in)
			r = node_of(in);
	} else if (!r) {
		for (i = n - 1; i >= 0 && !r; i--)
			if (!(kids[i]->flags & (BF_FLOAT | BF_ABS)) &&
			    !positioned(kids[i]))
				r = hit(kids[i], x, y, depth + 1);
	}
	if (kids != some)
		free(kids);
	if (r)
		return r;
	if (b->type != B_INLINE && b->node && hittable(b->style) &&
	    inside(x, y, b->x - b->p[3] - b->b[3], b->y - b->p[0] - b->b[0],
		   b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3],
		   b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2]))
		return b->node;
	return NULL;
}

/* A positioned box with a z-index, fixed, translucent, masked or
 * clipped by a shape */
int stacking_context(const struct box *b)
{
	const char *m, *k;

	if (!positioned(b))
		return 0;
	m = b->style->v[P_MASK_IMAGE].u.str;
	k = b->style->v[P_CLIP_PATH].u.str;
	return !b->style->v[P_Z_INDEX].kw ||
		kw(b->style, P_POSITION, "fixed") ||
		b->style->v[P_OPACITY].u.num < 1 ||
		(m && strcmp(m, "none")) || (k && strcmp(k, "none"));
}

struct layer {
	struct box *box;
	int z, order;
};

/* The positioned boxes of the context b is the root of: not those in
 * the contexts they make */
static void layers(struct box *b, struct layer **v, int *n, int *cap)
{
	struct box *c;

	for (c = b->first; c; c = c->next) {
		if (positioned(c)) {
			if (*n == *cap) {
				*cap = *cap ? *cap * 2 : 32;
				*v = xrealloc(*v, (size_t)*cap * sizeof(**v));
			}
			(*v)[*n].box = c;
			(*v)[*n].z = c->style->v[P_Z_INDEX].kw ? 0 :
				(int)c->style->v[P_Z_INDEX].u.num;
			(*v)[*n].order = *n;
			(*n)++;
			if (stacking_context(c))
				continue;
		}
		layers(c, v, n, cap);
	}
}

/* the top layer first: higher z-index, then later in the tree */
static int layer_cmp(const void *a, const void *b)
{
	const struct layer *x = a, *y = b;

	if (x->z != y->z)
		return x->z > y->z ? -1 : 1;
	return y->order - x->order;
}

/* A stacking context: its layers over its flow, but those with a
 * negative z-index, under it (CSS 2 Appendix E) */
static soie_node *hit_context(struct box *root, float x, float y)
{
	struct layer *v = NULL;
	int n = 0, cap = 0, i;
	soie_node *r = NULL;

	layers(root, &v, &n, &cap);
	qsort(v, (size_t)n, sizeof(*v), layer_cmp);
	for (i = 0; i < n && !r && v[i].z >= 0; i++)
		r = stacking_context(v[i].box) ? hit_context(v[i].box, x, y) :
			hit(v[i].box, x, y, 0);
	if (!r)
		r = hit(root, x, y, 0);
	for (; i < n && !r; i++)
		r = stacking_context(v[i].box) ? hit_context(v[i].box, x, y) :
			hit(v[i].box, x, y, 0);
	free(v);
	return r;
}

soie_node *soie_hit_test(soie_node *doc, float x, float y)
{
	struct box *root;

	if (doc->type != SOIE_DOCUMENT || !doc->u.doc->layout ||
	    !(root = doc->u.doc->layout->root))
		return NULL;
	return hit_context(root, x, y);
}

static struct box *find_box(struct box *b, const soie_node *el)
{
	struct box *c, *r;

	if (b->node == el)
		return b;
	for (c = b->first; c; c = c->next)
		if ((r = find_box(c, el)))
			return r;
	return NULL;
}

int soie_element_rect(const soie_node *el, float *x, float *y, float *w,
	float *h)
{
	const soie_node *doc = el->doc;
	struct box *b, *p;
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	int i;

	if (!doc->u.doc->layout || !doc->u.doc->layout->root)
		return -1;
	/* its box, kept by the layout that made it (none: no box) */
	if (el->type != SOIE_ELEMENT)
		b = find_box(doc->u.doc->layout->root, el);
	else if (el->u.el.box_tree != doc->u.doc->layout->serial)
		return -1;
	else if (!(b = el->u.el.box) || b->node != el)
		b = find_box(doc->u.doc->layout->root, el);
	if (!b)
		return -1;
	if (b->type != B_INLINE) {
		*x = b->x - b->p[3] - b->b[3];
		*y = b->y - b->p[0] - b->b[0];
		*w = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
		*h = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
		return 0;
	}
	for (p = b->parent; p && !(p->flags & BF_INLINE_KIDS); p = p->parent)
		;
	for (i = 0; p && i < p->nfrags; i++) {
		struct frag *f = &p->frags[i];
		const struct box *k;

		for (k = f->box; k && k != b && k != p; k = k->parent)
			;
		if (k != b)
			continue;
		if (f->x < x0)
			x0 = f->x;
		if (f->y < y0)
			y0 = f->y;
		if (f->x + f->w > x1)
			x1 = f->x + f->w;
		if (f->y + f->h > y1)
			y1 = f->y + f->h;
	}
	if (x1 < x0)
		return -1;
	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return 0;
}

/* Find: matches counted in layout order, the wanted one's rect kept */
struct find {
	const char *text;
	size_t len;
	int want, n;
	float r[4];
};

/* ASCII letters folded */
static int same(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];

		if (x >= 'A' && x <= 'Z')
			x += 32;
		if (y >= 'A' && y <= 'Z')
			y += 32;
		if (x != y)
			return 0;
	}
	return 1;
}

static void find_in(struct box *b, struct find *f);

/* Floats and positioned boxes of an inline formatting context, inside
 * its inline boxes too */
static void find_out_of_flow(struct box *b, struct find *f)
{
	struct box *c;

	for (c = b->first; c; c = c->next)
		if (c->flags & (BF_FLOAT | BF_ABS))
			find_in(c, f);
		else if (c->type == B_INLINE)
			find_out_of_flow(c, f);
}

static void find_in(struct box *b, struct find *f)
{
	struct box *c;
	int i;

	if (!(b->flags & BF_INLINE_KIDS)) {
		for (c = b->first; c; c = c->next)
			find_in(c, f);
		return;
	}
	for (i = 0; i < b->nfrags; i++) {
		struct frag *g = &b->frags[i];
		const char *s;
		size_t k = 0;

		if (g->kind == F_ATOMIC)
			find_in(g->box, f);
		if (g->kind != F_TEXT || !g->box->text)
			continue;
		s = g->box->text + g->start;
		while (k + f->len <= g->len) {
			if (!same(s + k, f->text, f->len)) {
				k++;
				continue;
			}
			if (f->n++ == f->want) {
				const struct style *st = g->box->style;

				f->r[0] = g->x + text_width(st, s, k);
				f->r[1] = g->y;
				f->r[2] = text_width(st, s + k, f->len);
				f->r[3] = g->h;
			}
			k += f->len;
		}
	}
	find_out_of_flow(b, f);
}

int soie_find(soie_node *doc, const char *text, int n, float *x, float *y,
	float *w, float *h)
{
	struct find f;

	if (doc->type != SOIE_DOCUMENT || !doc->u.doc->layout ||
	    !doc->u.doc->layout->root || !text || !*text)
		return 0;
	memset(&f, 0, sizeof(f));
	f.text = text;
	f.len = strlen(text);
	f.want = n;
	find_in(doc->u.doc->layout->root, &f);
	if (n >= 0 && n < f.n) {
		*x = f.r[0];
		*y = f.r[1];
		*w = f.r[2];
		*h = f.r[3];
	}
	return f.n;
}

/*
 * Selection: the text fragments in layout order, as find walks them,
 * numbered; a point stands for the nearest character of the nearest
 * fragment, and the selection is all between its two points.
 */
struct spos {
	int k;		/* fragment, by number */
	unsigned off;	/* byte in it */
};

enum { S_LOCATE, S_BLOCK, S_EMIT };

struct sel {
	int pass, k;
	const float *pt;
	/* locate: the nearest fragment of each point */
	float best[2];
	struct spos at[2];
	const struct frag *frag[2];
	const struct box *block[2];
	unsigned last_len;
	/* the range */
	struct spos lo, hi;
	const struct frag *lof, *hif;
	/* emit: rects, or the text */
	float *rects;
	int max, n;
	struct buf *text;
	const struct box *pblock;
	const struct frag *pf;
	int pline;
};

/* x of byte i in g, from its left: text_width() and what justify added
 * to its spaces */
static float sel_x(const struct frag *g, unsigned i)
{
	const struct style *st = g->box->style;
	const char *s = g->box->text + g->start;
	float x, extra;
	unsigned j;

	if (!i)
		return 0;
	if (i >= g->len)
		return g->w;
	x = text_width(st, s, i);
	if (g->spaces && (extra = (g->w - text_width(st, s, g->len)) /
	    (float)g->spaces) > 0)
		for (j = 0; j < i; j++)
			if (s[j] == ' ')
				x += extra;
	return x;
}

/* The character boundary of g nearest to page x */
static unsigned sel_off(const struct frag *g, float x)
{
	const char *s = g->box->text + g->start;
	unsigned i = 0, best = 0;
	float d, bd = 1e30f;

	x -= g->x;
	for (;;) {
		float cx = sel_x(g, i);

		d = cx > x ? cx - x : x - cx;
		if (d < bd) {
			bd = d;
			best = i;
		} else if (cx > x) {
			break;
		}
		if (i >= g->len)
			break;
		i++;
		while (i < g->len && ((unsigned char)s[i] & 0xc0) == 0x80)
			i++;
	}
	return best;
}

/* A word's bytes: letters, digits, _ and all of UTF-8 but ASCII */
static int word_char(char c)
{
	unsigned char u = (unsigned char)c;

	return u >= 0x80 || u == '_' || (u >= '0' && u <= '9') ||
		((u | 32) >= 'a' && (u | 32) <= 'z');
}

static int line_of(const struct box *b, int i)
{
	int l;

	for (l = 0; l < b->nlines; l++)
		if (i >= b->lines[l].first && i < b->lines[l].first +
		    b->lines[l].n)
			return l;
	return -1;
}

static void sel_frag(struct sel *s, const struct box *b, int i)
{
	const struct frag *g = &b->frags[i];
	unsigned a, e;
	int p;

	if (s->pass == S_LOCATE) {
		for (p = 0; p < 2; p++) {
			float x = s->pt[2 * p], y = s->pt[2 * p + 1], dx, dy;

			dx = x < g->x ? g->x - x : x > g->x + g->w ?
				x - g->x - g->w : 0;
			dy = y < g->y ? g->y - y : y > g->y + g->h ?
				y - g->y - g->h : 0;
			/* the nearest line first, then along it */
			if (dy * 10000 + dx < s->best[p]) {
				s->best[p] = dy * 10000 + dx;
				s->at[p].k = s->k;
				s->frag[p] = g;
				s->block[p] = b;
			}
		}
		s->last_len = g->len;
		return;
	}
	if (s->pass == S_BLOCK) {
		if (b == s->block[0] && s->k < s->lo.k) {
			s->lo.k = s->k;
			s->lo.off = 0;
		}
		if (b == s->block[1]) {
			s->hi.k = s->k;
			s->hi.off = g->len;
		}
		return;
	}
	if (s->k < s->lo.k || s->k > s->hi.k)
		return;
	a = s->k == s->lo.k ? s->lo.off : 0;
	e = s->k == s->hi.k ? s->hi.off : g->len;
	if (a >= e)
		return;
	if (!s->text) {
		if (s->n < s->max) {
			float x = sel_x(g, a);

			s->rects[s->n * 4] = g->x + x;
			s->rects[s->n * 4 + 1] = g->y;
			s->rects[s->n * 4 + 2] = sel_x(g, e) - x;
			s->rects[s->n * 4 + 3] = g->h;
		}
		s->n++;
		return;
	}
	p = line_of(b, i);
	if (s->pf) {
		struct buf *t = s->text;
		char last = t->len ? t->s[t->len - 1] : '\n';

		if (b == s->pblock && p == s->pline)
			;
		else if (b == s->pblock && s->pline >= 0 &&
			 !b->lines[s->pline].forced) {
			if (last != ' ')
				buf_putc(t, ' ');	/* a wrapped line */
		} else if (b != s->pblock && g->y < s->pf->y + s->pf->h &&
			   s->pf->y < g->y + g->h) {
			if (last != ' ' && last != '\n')
				buf_putc(t, ' ');	/* side by side */
		} else if (last != '\n' || b == s->pblock) {
			buf_putc(t, '\n');
		}
	}
	buf_add(s->text, g->box->text + g->start + a, e - a);
	s->pblock = b;
	s->pf = g;
	s->pline = p;
}

static void sel_in(struct box *b, struct sel *s);

static void sel_out_of_flow(struct box *b, struct sel *s)
{
	struct box *c;

	for (c = b->first; c; c = c->next)
		if (c->flags & (BF_FLOAT | BF_ABS))
			sel_in(c, s);
		else if (c->type == B_INLINE)
			sel_out_of_flow(c, s);
}

static void sel_in(struct box *b, struct sel *s)
{
	struct box *c;
	int i;

	if (!(b->flags & BF_INLINE_KIDS)) {
		for (c = b->first; c; c = c->next)
			sel_in(c, s);
		return;
	}
	for (i = 0; i < b->nfrags; i++) {
		struct frag *g = &b->frags[i];
		const struct box *p = g->box->parent;

		if (g->kind == F_ATOMIC)
			sel_in(g->box, s);
		/* list markers and generated content are not the page's
		 * text */
		if (g->kind != F_TEXT || !g->box->text || !g->len ||
		    (p && (p->flags & (BF_OUTSIDE | BF_PSEUDO))) ||
		    (g->box->style && !kw(g->box->style, P_VISIBILITY,
		    "visible")))
			continue;
		sel_frag(s, b, i);
		s->k++;
	}
	sel_out_of_flow(b, s);
}

/* The range between the points: 0 when it is empty */
static int sel_range(soie_node *doc, const float *pt, int unit,
	struct sel *s)
{
	struct box *root;
	int p;

	memset(s, 0, sizeof(*s));
	if (doc->type != SOIE_DOCUMENT || !doc->u.doc->layout ||
	    !(root = doc->u.doc->layout->root))
		return 0;
	s->pt = pt;
	s->best[0] = s->best[1] = 1e30f;
	sel_in(root, s);
	if (!s->k || !s->frag[0])
		return 0;
	if (unit == SOIE_SELECT_ALL) {
		s->lo.k = 0;
		s->lo.off = 0;
		s->hi.k = s->k - 1;
		s->hi.off = s->last_len;
		return 1;
	}
	for (p = 0; p < 2; p++)
		s->at[p].off = sel_off(s->frag[p], pt[2 * p]);
	p = s->at[1].k < s->at[0].k || (s->at[1].k == s->at[0].k &&
		s->at[1].off < s->at[0].off);
	s->lo = s->at[p];
	s->lof = s->frag[p];
	s->hi = s->at[!p];
	s->hif = s->frag[!p];
	if (unit == SOIE_SELECT_WORD) {
		const char *t = s->lof->box->text + s->lof->start;

		while (s->lo.off && word_char(t[s->lo.off - 1]))
			s->lo.off--;
		t = s->hif->box->text + s->hif->start;
		while (s->hi.off < s->hif->len && word_char(t[s->hi.off]))
			s->hi.off++;
		/* a word's UTF-8 bytes are word bytes: off stays on a
		 * character */
	} else if (unit == SOIE_SELECT_BLOCK) {
		const struct box *b0 = s->block[p], *b1 = s->block[!p];

		s->block[0] = b0;
		s->block[1] = b1;
		s->lo.k = s->k;
		s->pass = S_BLOCK;
		s->k = 0;
		sel_in(root, s);
	}
	return s->lo.k < s->hi.k || (s->lo.k == s->hi.k &&
		s->lo.off < s->hi.off);
}

int soie_select(soie_node *doc, const float *points, int unit, float *rects,
	int max)
{
	struct sel s;

	if (!sel_range(doc, points, unit, &s))
		return 0;
	s.pass = S_EMIT;
	s.k = 0;
	s.rects = rects;
	s.max = max;
	sel_in(doc->u.doc->layout->root, &s);
	return s.n;
}

char *soie_select_text(soie_node *doc, const float *points, int unit)
{
	struct sel s;
	struct buf t = { NULL, 0, 0 };

	if (!sel_range(doc, points, unit, &s))
		return NULL;
	s.pass = S_EMIT;
	s.k = 0;
	s.text = &t;
	s.pline = -1;
	sel_in(doc->u.doc->layout->root, &s);
	return t.s;
}

void soie_set_focus(soie_node *doc, soie_node *el, int caret)
{
	if (doc->type != SOIE_DOCUMENT)
		return;
	if (doc->u.doc->damage && (el != doc->u.doc->focus ||
	    caret != doc->u.doc->caret)) {
		damage_node(doc, doc->u.doc->focus);
		damage_node(doc, el);
	}
	doc->u.doc->focus = el;
	doc->u.doc->caret = caret;
}

soie_node *soie_get_focus(const soie_node *doc, int *caret)
{
	if (doc->type != SOIE_DOCUMENT)
		return NULL;
	if (caret)
		*caret = doc->u.doc->caret;
	return doc->u.doc->focus;
}
