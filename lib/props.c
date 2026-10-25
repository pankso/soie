/*
 * CSS properties: value parsing, shorthands, computed values and their
 * serialization.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "css.h"

const struct propdef props[NPROPS] = {
#define PROP(id, name, inh, kind, flags, kw, init) \
	{ name, inh, kind, flags, kw, init },
#include "props.h"
#undef PROP
};

int prop_lookup(const char *name)
{
	int i;

	for (i = 0; i < NPROPS; i++)
		if (ascii_ieq(name, props[i].name))
			return i;
	return -1;
}

/* Index of word in a '|'-separated list, or -1. */
static int kw_index(const char *list, const char *word)
{
	int i = 0;

	while (*list) {
		const char *e = strchr(list, '|');
		size_t n = e ? (size_t)(e - list) : strlen(list), k;

		for (k = 0; k < n && word[k]; k++) {
			int c = word[k] >= 'A' && word[k] <= 'Z' ?
				word[k] + 32 : word[k];

			if (c != list[k])
				break;
		}
		if (k == n && !word[k])
			return i;
		if (!e)
			break;
		list = e + 1;
		i++;
	}
	return -1;
}

static void kw_name(const char *list, int i, struct buf *b)
{
	while (i-- > 0 && list)
		if ((list = strchr(list, '|')))
			list++;
	if (list) {
		const char *e = strchr(list, '|');

		buf_add(b, list, e ? (size_t)(e - list) : strlen(list));
	}
}

void sval_free(struct sval *v)
{
	slen_free(v->len);
	free(v->str);
	v->len = NULL;
	v->str = NULL;
}

void svec_free(struct svec *v)
{
	int i;

	for (i = 0; i < v->n; i++)
		sval_free(&v->v[i]);
	free(v->v);
	v->v = NULL;
	v->n = v->cap = 0;
}

static struct sval *svec_add(struct svec *v, int prop)
{
	struct sval *s;

	if (v->n == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 8;
		v->v = xrealloc(v->v, (size_t)v->cap * sizeof(*v->v));
	}
	s = &v->v[v->n++];
	memset(s, 0, sizeof(*s));
	s->prop = (short)prop;
	s->shorthand = -1;
	return s;
}

int has_var(const struct cv *v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if ((v[i].type == CT_FUNCTION && (ascii_ieq(v[i].str, "var") ||
		     ascii_ieq(v[i].str, "env"))) ||
		    ((v[i].type == CT_FUNCTION || v[i].type == CT_BLOCK) &&
		     has_var(v[i].kids.v, v[i].kids.n)))
			return 1;
	return 0;
}

static char *serialize_text(const struct cv *v, int n)
{
	struct buf b = { 0 };
	int i = 0;

	while (i < n && v[i].type == CT_WS)
		i++;
	while (n > i && v[n - 1].type == CT_WS)
		n--;
	css_serialize(&b, v + i, n - i);
	return b.s;
}

/* Longhand values */

static const char *const font_sizes =
	"xx-small|x-small|small|medium|large|x-large|xx-large|xxx-large|"
	"larger|smaller|math";

static int parse_family(struct cur *c, struct sval *sv)
{
	struct buf b = { 0 };
	const struct cv *v;
	int first = 1;

	buf_add(&b, "", 0);
	for (;;) {
		v = cur_next(c);
		if (!v)
			goto bad;
		if (!first)
			buf_add(&b, ", ", 2);
		first = 0;
		if (v->type == CT_STRING) {
			buf_putc(&b, '"');
			buf_add(&b, v->str, strlen(v->str));
			buf_putc(&b, '"');
		} else if (v->type == CT_IDENT) {
			int words = 0;

			for (;;) {
				if (words)
					buf_putc(&b, ' ');
				buf_add(&b, v->str, strlen(v->str));
				words++;
				v = cur_peek(c);
				if (!v || v->type != CT_IDENT)
					break;
				cur_next(c);
			}
		} else {
			goto bad;
		}
		v = cur_peek(c);
		if (!v)
			break;
		if (v->type != CT_COMMA)
			goto bad;
		cur_next(c);
	}
	sv->str = b.s;
	return 1;
bad:
	buf_free(&b);
	return 0;
}

static int parse_display(struct cur *c, struct sval *sv)
{
	static const char *const pairs[][3] = {
		{ "block", "flow", "block" }, { "inline", "flow", "inline" },
		{ "block", "flow-root", "flow-root" },
		{ "inline", "flow-root", "inline-block" },
		{ "block", "flex", "flex" }, { "inline", "flex", "inline-flex" },
		{ "block", "grid", "grid" }, { "inline", "grid", "inline-grid" },
		{ "block", "table", "table" },
		{ "inline", "table", "inline-table" },
		{ "block", "list-item", "list-item" },
		{ "list-item", "block", "list-item" },
		{ "list-item", "flow", "list-item" },
		{ "flow", "list-item", "list-item" },
		{ "inline", "list-item", "list-item" },
		{ "run-in", "flow", "inline" },
		{ NULL, NULL, NULL }
	};
	const struct cv *a = cur_next(c), *b;
	const char *name;
	int i;

	if (!a || a->type != CT_IDENT)
		return 0;
	name = a->str;
	b = cur_peek(c);
	if (b && b->type == CT_IDENT) {
		cur_next(c);
		for (i = 0; pairs[i][0]; i++)
			if ((ascii_ieq(a->str, pairs[i][0]) &&
			     ascii_ieq(b->str, pairs[i][1])) ||
			    (ascii_ieq(a->str, pairs[i][1]) &&
			     ascii_ieq(b->str, pairs[i][0])))
				break;
		if (!pairs[i][0])
			return 0;
		name = pairs[i][2];
	} else if (ascii_ieq(name, "flow")) {
		name = "block";
	} else if (ascii_ieq(name, "run-in")) {
		name = "inline";
	} else if (ascii_ieq(name, "-webkit-box") ||
		   ascii_ieq(name, "-webkit-flex")) {
		name = "flex";
	} else if (ascii_ieq(name, "-webkit-inline-box") ||
		   ascii_ieq(name, "-webkit-inline-flex")) {
		name = "inline-flex";
	}
	i = kw_index(props[P_DISPLAY].keywords, name);
	if (i < 0)
		return 0;
	sv->kw = (unsigned char)(i + 1);
	return 1;
}

static int parse_number(struct cur *c, struct sval *sv, int integer,
	int nonneg)
{
	const struct cv *v = cur_peek(c);
	struct slen *l;

	if (!v)
		return 0;
	if (v->type == CT_NUMBER) {
		if ((integer && !v->integer) || (nonneg && v->num < 0))
			return 0;
		sv->num = v->num;
		cur_next(c);
		return 1;
	}
	l = parse_length(c, L_NUMBER);
	if (!l)
		return 0;
	if (!slen_number(l, &sv->num)) {
		slen_free(l);
		return 0;
	}
	if (integer)
		sv->num = (double)(long)(sv->num + (sv->num < 0 ? -0.5 : 0.5));
	if (nonneg && sv->num < 0)
		sv->num = 0;
	slen_free(l);
	return 1;
}

/* Parse one longhand value at the cursor, consuming what it uses. */
static int parse_value_at(int prop, struct cur *c, struct sval *sv)
{
	const struct propdef *d = &props[prop];
	const struct cv *v = cur_peek(c);
	int i;

	if (!v)
		return 0;
	if (v->type == CT_IDENT && d->keywords[0] && d->kind != K_DISPLAY) {
		i = kw_index(d->keywords, v->str);
		if (i >= 0) {
			sv->kw = (unsigned char)(i + 1);
			cur_next(c);
			return 1;
		}
	}
	switch (d->kind) {
	case K_KW:
		return 0;
	case K_DISPLAY:
		return parse_display(c, sv);
	case K_LEN:
		sv->len = parse_length(c, d->flags);
		return sv->len != NULL;
	case K_BWIDTH:
		if (v->type == CT_IDENT) {
			i = kw_index("thin|medium|thick", v->str);
			if (i < 0)
				return 0;
			sv->kw = (unsigned char)(i + 1);
			cur_next(c);
			return 1;
		}
		sv->len = parse_length(c, L_NONNEG | L_NOPCT);
		return sv->len != NULL;
	case K_COLOR:
		return parse_color(c, &sv->color);
	case K_NUM:
		return parse_number(c, sv, 0, d->flags & L_NONNEG);
	case K_INT:
		return parse_number(c, sv, 1, 0);
	case K_ALPHA:
		if (v->type == CT_PERCENT) {
			sv->num = v->num / 100;
			cur_next(c);
			return 1;
		}
		return parse_number(c, sv, 0, 0);
	case K_FSIZE:
		if (v->type == CT_IDENT) {
			i = kw_index(font_sizes, v->str);
			if (i < 0)
				return 0;
			sv->kw = (unsigned char)(i + 1);
			cur_next(c);
			return 1;
		}
		sv->len = parse_length(c, L_NONNEG);
		return sv->len != NULL;
	case K_FWEIGHT:
		if (v->type == CT_IDENT) {
			i = kw_index("normal|bold|bolder|lighter", v->str);
			if (i < 0)
				return 0;
			sv->kw = (unsigned char)(i + 1);
			cur_next(c);
			return 1;
		}
		if (!parse_number(c, sv, 0, 0))
			return 0;
		return sv->num >= 1 && sv->num <= 1000;
	case K_LH:
		sv->len = parse_length(c, L_NONNEG | L_NUMBER);
		return sv->len != NULL;
	case K_FAMILY:
		return parse_family(c, sv);
	case K_LIST:
		if (v->type == CT_IDENT || v->type == CT_STRING) {
			struct buf b = { 0 };

			css_serialize(&b, v, 1);
			sv->str = b.s;
			cur_next(c);
			return 1;
		}
		return 0;
	case K_DECO: {
		int mask = 0, bit;

		if (cv_ident(v, "none")) {
			cur_next(c);
			sv->num = 0;
			return 1;
		}
		while ((v = cur_peek(c)) && v->type == CT_IDENT &&
		       (bit = kw_index("underline|overline|line-through|blink",
				       v->str)) >= 0 && !(mask & (1 << bit))) {
			mask |= 1 << bit;
			cur_next(c);
		}
		sv->num = mask;
		return mask != 0;
	}
	case K_RAW:
		sv->str = serialize_text(c->v + c->i, c->n - c->i);
		c->i = c->n;
		return *sv->str != '\0';
	}
	return 0;
}

int parse_longhand_value(int prop, const struct cvlist *value,
	struct sval *out)
{
	struct cur c;

	cur_init(&c, value);
	memset(out, 0, sizeof(*out));
	out->prop = (short)prop;
	out->shorthand = -1;
	if (!parse_value_at(prop, &c, out) || !cur_end(&c)) {
		sval_free(out);
		return 0;
	}
	return 1;
}

/* Shorthands */

enum {
	SH_MARGIN, SH_PADDING, SH_INSET, SH_BORDER_WIDTH, SH_BORDER_STYLE,
	SH_BORDER_COLOR, SH_BORDER_RADIUS, SH_BORDER, SH_BORDER_TOP,
	SH_BORDER_RIGHT, SH_BORDER_BOTTOM, SH_BORDER_LEFT, SH_OUTLINE,
	SH_OVERFLOW, SH_BACKGROUND, SH_FONT, SH_LIST_STYLE, SH_FLEX,
	SH_FLEX_FLOW, SH_GAP, SH_PLACE_ITEMS, SH_PLACE_CONTENT,
	SH_PLACE_SELF, SH_TEXT_DECORATION, SH_WHITE_SPACE, SH_TEXT_WRAP,
	SH_GRID_ROW, SH_GRID_COLUMN, SH_GRID_AREA, SH_MARGIN_INLINE,
	SH_MARGIN_BLOCK, SH_PADDING_INLINE, SH_PADDING_BLOCK,
	SH_INSET_INLINE, SH_INSET_BLOCK, SH_BORDER_INLINE, SH_BORDER_BLOCK,
	SH_ALL, NSHORTHANDS
};

static const char *const shorthand_names[NSHORTHANDS] = {
	"margin", "padding", "inset", "border-width", "border-style",
	"border-color", "border-radius", "border", "border-top",
	"border-right", "border-bottom", "border-left", "outline",
	"overflow", "background", "font", "list-style", "flex",
	"flex-flow", "gap", "place-items", "place-content", "place-self",
	"text-decoration", "white-space", "text-wrap", "grid-row",
	"grid-column", "grid-area", "margin-inline", "margin-block",
	"padding-inline", "padding-block", "inset-inline", "inset-block",
	"border-inline", "border-block", "all"
};

/* Logical and legacy names, for a horizontal left-to-right page. */
static const char *const aliases[][2] = {
	{ "margin-inline-start", "margin-left" },
	{ "margin-inline-end", "margin-right" },
	{ "margin-block-start", "margin-top" },
	{ "margin-block-end", "margin-bottom" },
	{ "padding-inline-start", "padding-left" },
	{ "padding-inline-end", "padding-right" },
	{ "padding-block-start", "padding-top" },
	{ "padding-block-end", "padding-bottom" },
	{ "inset-inline-start", "left" }, { "inset-inline-end", "right" },
	{ "inset-block-start", "top" }, { "inset-block-end", "bottom" },
	{ "inline-size", "width" }, { "block-size", "height" },
	{ "min-inline-size", "min-width" }, { "min-block-size", "min-height" },
	{ "max-inline-size", "max-width" }, { "max-block-size", "max-height" },
	{ "border-inline-start-width", "border-left-width" },
	{ "border-inline-end-width", "border-right-width" },
	{ "border-block-start-width", "border-top-width" },
	{ "border-block-end-width", "border-bottom-width" },
	{ "border-inline-start-style", "border-left-style" },
	{ "border-inline-end-style", "border-right-style" },
	{ "border-block-start-style", "border-top-style" },
	{ "border-block-end-style", "border-bottom-style" },
	{ "border-inline-start-color", "border-left-color" },
	{ "border-inline-end-color", "border-right-color" },
	{ "border-block-start-color", "border-top-color" },
	{ "border-block-end-color", "border-bottom-color" },
	{ "border-start-start-radius", "border-top-left-radius" },
	{ "border-start-end-radius", "border-top-right-radius" },
	{ "border-end-start-radius", "border-bottom-left-radius" },
	{ "border-end-end-radius", "border-bottom-right-radius" },
	{ "word-wrap", "overflow-wrap" },
	{ "grid-row-gap", "row-gap" }, { "grid-column-gap", "column-gap" },
	{ "-webkit-box-sizing", "box-sizing" },
	{ "-webkit-text-decoration-line", "text-decoration-line" },
	{ NULL, NULL }
};

static const char *const short_aliases[][2] = {
	{ "border-inline-start", "border-left" },
	{ "border-inline-end", "border-right" },
	{ "border-block-start", "border-top" },
	{ "border-block-end", "border-bottom" },
	{ "grid-gap", "gap" },
	{ NULL, NULL }
};

int shorthand_lookup(const char *name)
{
	int i;

	for (i = 0; short_aliases[i][0]; i++)
		if (ascii_ieq(name, short_aliases[i][0]))
			name = short_aliases[i][1];
	for (i = 0; i < NSHORTHANDS; i++)
		if (ascii_ieq(name, shorthand_names[i]))
			return i;
	return -1;
}

static const short box_props[][4] = {
	[SH_MARGIN] = { P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM,
		P_MARGIN_LEFT },
	[SH_PADDING] = { P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM,
		P_PADDING_LEFT },
	[SH_INSET] = { P_TOP, P_RIGHT, P_BOTTOM, P_LEFT },
	[SH_BORDER_WIDTH] = { P_BORDER_TOP_WIDTH, P_BORDER_RIGHT_WIDTH,
		P_BORDER_BOTTOM_WIDTH, P_BORDER_LEFT_WIDTH },
	[SH_BORDER_STYLE] = { P_BORDER_TOP_STYLE, P_BORDER_RIGHT_STYLE,
		P_BORDER_BOTTOM_STYLE, P_BORDER_LEFT_STYLE },
	[SH_BORDER_COLOR] = { P_BORDER_TOP_COLOR, P_BORDER_RIGHT_COLOR,
		P_BORDER_BOTTOM_COLOR, P_BORDER_LEFT_COLOR },
	[SH_BORDER_RADIUS] = { P_BORDER_TOP_LEFT_RADIUS,
		P_BORDER_TOP_RIGHT_RADIUS, P_BORDER_BOTTOM_RIGHT_RADIUS,
		P_BORDER_BOTTOM_LEFT_RADIUS },
};

/* The longhands a shorthand sets, -1 terminated. */
static int shorthand_longhands(int sh, int *out)
{
	static const short side[4][3] = {
		{ P_BORDER_TOP_WIDTH, P_BORDER_TOP_STYLE, P_BORDER_TOP_COLOR },
		{ P_BORDER_RIGHT_WIDTH, P_BORDER_RIGHT_STYLE,
			P_BORDER_RIGHT_COLOR },
		{ P_BORDER_BOTTOM_WIDTH, P_BORDER_BOTTOM_STYLE,
			P_BORDER_BOTTOM_COLOR },
		{ P_BORDER_LEFT_WIDTH, P_BORDER_LEFT_STYLE,
			P_BORDER_LEFT_COLOR },
	};
	int n = 0, i, j;

#define L(x) out[n++] = (x)
	switch (sh) {
	case SH_MARGIN: case SH_PADDING: case SH_INSET:
	case SH_BORDER_WIDTH: case SH_BORDER_STYLE: case SH_BORDER_COLOR:
	case SH_BORDER_RADIUS:
		for (i = 0; i < 4; i++)
			L(box_props[sh][i]);
		break;
	case SH_BORDER:
		for (i = 0; i < 4; i++)
			for (j = 0; j < 3; j++)
				L(side[i][j]);
		break;
	case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM:
	case SH_BORDER_LEFT:
		for (j = 0; j < 3; j++)
			L(side[sh - SH_BORDER_TOP][j]);
		break;
	case SH_BORDER_INLINE:
		for (j = 0; j < 3; j++)
			L(side[3][j]), L(side[1][j]);
		break;
	case SH_BORDER_BLOCK:
		for (j = 0; j < 3; j++)
			L(side[0][j]), L(side[2][j]);
		break;
	case SH_OUTLINE:
		L(P_OUTLINE_WIDTH), L(P_OUTLINE_STYLE), L(P_OUTLINE_COLOR);
		break;
	case SH_OVERFLOW: L(P_OVERFLOW_X), L(P_OVERFLOW_Y); break;
	case SH_BACKGROUND:
		L(P_BACKGROUND_COLOR), L(P_BACKGROUND_IMAGE);
		L(P_BACKGROUND_REPEAT), L(P_BACKGROUND_POSITION);
		L(P_BACKGROUND_SIZE), L(P_BACKGROUND_ATTACHMENT);
		L(P_BACKGROUND_CLIP), L(P_BACKGROUND_ORIGIN);
		break;
	case SH_FONT:
		L(P_FONT_STYLE), L(P_FONT_VARIANT_CAPS), L(P_FONT_WEIGHT);
		L(P_FONT_STRETCH), L(P_FONT_SIZE), L(P_LINE_HEIGHT);
		L(P_FONT_FAMILY);
		break;
	case SH_LIST_STYLE:
		L(P_LIST_STYLE_TYPE), L(P_LIST_STYLE_POSITION);
		L(P_LIST_STYLE_IMAGE);
		break;
	case SH_FLEX: L(P_FLEX_GROW), L(P_FLEX_SHRINK), L(P_FLEX_BASIS); break;
	case SH_FLEX_FLOW: L(P_FLEX_DIRECTION), L(P_FLEX_WRAP); break;
	case SH_GAP: L(P_ROW_GAP), L(P_COLUMN_GAP); break;
	case SH_PLACE_ITEMS: L(P_ALIGN_ITEMS), L(P_JUSTIFY_ITEMS); break;
	case SH_PLACE_CONTENT: L(P_ALIGN_CONTENT), L(P_JUSTIFY_CONTENT); break;
	case SH_PLACE_SELF: L(P_ALIGN_SELF), L(P_JUSTIFY_SELF); break;
	case SH_TEXT_DECORATION:
		L(P_TEXT_DECORATION_LINE), L(P_TEXT_DECORATION_STYLE);
		L(P_TEXT_DECORATION_COLOR), L(P_TEXT_DECORATION_THICKNESS);
		break;
	case SH_WHITE_SPACE:
		L(P_WHITE_SPACE_COLLAPSE), L(P_TEXT_WRAP_MODE);
		break;
	case SH_TEXT_WRAP: L(P_TEXT_WRAP_MODE), L(P_TEXT_WRAP_STYLE); break;
	case SH_GRID_ROW: L(P_GRID_ROW_START), L(P_GRID_ROW_END); break;
	case SH_GRID_COLUMN:
		L(P_GRID_COLUMN_START), L(P_GRID_COLUMN_END);
		break;
	case SH_GRID_AREA:
		L(P_GRID_ROW_START), L(P_GRID_COLUMN_START);
		L(P_GRID_ROW_END), L(P_GRID_COLUMN_END);
		break;
	case SH_MARGIN_INLINE: L(P_MARGIN_LEFT), L(P_MARGIN_RIGHT); break;
	case SH_MARGIN_BLOCK: L(P_MARGIN_TOP), L(P_MARGIN_BOTTOM); break;
	case SH_PADDING_INLINE: L(P_PADDING_LEFT), L(P_PADDING_RIGHT); break;
	case SH_PADDING_BLOCK: L(P_PADDING_TOP), L(P_PADDING_BOTTOM); break;
	case SH_INSET_INLINE: L(P_LEFT), L(P_RIGHT); break;
	case SH_INSET_BLOCK: L(P_TOP), L(P_BOTTOM); break;
	case SH_ALL:
		for (i = 0; i < NPROPS; i++)
			if (i != P_DIRECTION)
				L(i);
		break;
	}
#undef L
	out[n] = -1;
	return n;
}

/*
 * Shorthands find where each longhand's value lies (a token range),
 * then each longhand parses its own range. Missing ones get initial.
 */
struct range {
	int from, to;		/* from < 0: not given */
};

static void emit_initial(struct svec *out, int prop)
{
	svec_add(out, prop)->type = SV_INITIAL;
}

static void emit_range(int prop, const struct cur *src, struct range r,
	struct svec *out)
{
	struct sval *d = svec_add(out, prop);
	struct cur c;

	if (r.from < 0) {
		d->type = SV_INITIAL;
		return;
	}
	c.v = src->v;
	c.i = r.from;
	c.n = r.to;
	if (!parse_value_at(prop, &c, d) || !cur_end(&c)) {
		sval_free(d);
		memset(d, 0, sizeof(*d));
		d->prop = (short)prop;
		d->shorthand = -1;
		d->type = SV_INITIAL;
	}
}

/* Try to parse prop at c; on success the range it used. */
static int try_at(int prop, struct cur *c, struct range *r)
{
	struct cur save = *c;
	struct sval sv;
	int ok;

	memset(&sv, 0, sizeof(sv));
	cur_peek(c);
	r->from = c->i;
	ok = parse_value_at(prop, c, &sv);
	sval_free(&sv);
	if (!ok) {
		*c = save;
		r->from = -1;
		return 0;
	}
	r->to = c->i;
	return 1;
}

/* "a || b || c": each of the n longhands at most once, any order. */
static int any_order(struct cur *c, const int *lh, int n, struct range *r)
{
	int i, got = 0, progress = 1;

	for (i = 0; i < n; i++)
		r[i].from = -1;
	while (progress && !cur_end(c)) {
		progress = 0;
		for (i = 0; i < n && !cur_end(c); i++)
			if (r[i].from < 0 && try_at(lh[i], c, &r[i])) {
				got++;
				progress = 1;
			}
	}
	return got;
}

static int sh_box4(const short *lh, struct cur *c, struct svec *out,
	int radius)
{
	static const int map[4][4] = {
		{ 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 }
	};
	struct range r[4];
	const struct cv *v;
	int n = 0, i;

	while (n < 4 && (v = cur_peek(c))) {
		if (radius && v->type == CT_DELIM && v->delim == '/')
			break;
		if (!try_at(lh[0], c, &r[n]))
			return 0;
		n++;
	}
	if (radius && (v = cur_peek(c)) && v->type == CT_DELIM &&
	    v->delim == '/')
		c->i = c->n;	/* vertical radii are not supported */
	if (!n || !cur_end(c))
		return 0;
	for (i = 0; i < 4; i++)
		emit_range(lh[i], c, r[map[n - 1][i]], out);
	return 1;
}

/* One or two values; slash: separated by '/' (grid-row). */
static int sh_pair(int a, int b, struct cur *c, struct svec *out, int slash)
{
	struct range r[2];
	const struct cv *v;

	if (!try_at(a, c, &r[0]))
		return 0;
	r[1] = r[0];
	if (slash) {
		/* raw values: split at '/' by hand */
		int i, start = r[0].from;

		for (i = start; i < c->n; i++)
			if (c->v[i].type == CT_DELIM && c->v[i].delim == '/')
				break;
		r[0].to = i;
		if (i < c->n) {
			r[1].from = i + 1;
			r[1].to = c->n;
		} else {
			/* a lone custom ident is also the end */
			struct cur t;
			const struct cv *v;

			t.v = c->v;
			t.i = r[0].from;
			t.n = r[0].to;
			v = cur_next(&t);
			if (!(v && v->type == CT_IDENT && !cv_ident(v, "auto") &&
			      !cv_ident(v, "span") && cur_end(&t)))
				r[1].from = -1;
		}
		c->i = c->n;
	} else if ((v = cur_peek(c))) {
		if (!try_at(b, c, &r[1]) || !cur_end(c))
			return 0;
	}
	emit_range(a, c, r[0], out);
	emit_range(b, c, r[1], out);
	return 1;
}

static int sh_border(int sh, struct cur *c, struct svec *out)
{
	int lh[16], n = shorthand_longhands(sh, lh), i;
	int one[3];
	struct range r[3];

	if (sh == SH_OUTLINE) {
		one[0] = P_OUTLINE_WIDTH;
		one[1] = P_OUTLINE_STYLE;
		one[2] = P_OUTLINE_COLOR;
	} else {
		one[0] = P_BORDER_TOP_WIDTH;
		one[1] = P_BORDER_TOP_STYLE;
		one[2] = P_BORDER_TOP_COLOR;
	}
	if (!any_order(c, one, 3, r) || !cur_end(c))
		return 0;
	for (i = 0; i < n; i++) {
		int kind = props[lh[i]].kind;
		int slot = kind == K_BWIDTH ? 0 : kind == K_KW ? 1 : 2;

		emit_range(lh[i], c, r[slot], out);
	}
	return 1;
}

static int is_image(const struct cv *v)
{
	size_t n;

	if (!v)
		return 0;
	if (v->type == CT_URL)
		return 1;
	if (v->type != CT_FUNCTION)
		return 0;
	n = strlen(v->str);
	return ascii_ieq(v->str, "url") || ascii_ieq(v->str, "image-set") ||
		ascii_ieq(v->str, "-webkit-image-set") ||
		(n >= 8 && ascii_ieq(v->str + n - 8, "gradient"));
}

/*
 * background: per layer, the image, position [/ size], repeat,
 * attachment and boxes; the color only in the last layer. The layered
 * longhands keep their text, comma-separated like the declaration.
 */
static int is_box_kw(const struct cv *v)
{
	return cv_ident(v, "border-box") || cv_ident(v, "padding-box") ||
		cv_ident(v, "content-box") || cv_ident(v, "text");
}

static int sh_background(struct cur *c, struct svec *out)
{
	struct buf img = { 0 }, pos = { 0 }, size = { 0 }, rep = { 0 },
		clip = { 0 }, org = { 0 };
	struct color color;
	int have_color = 0, layer = 0, i, att = -1;
	const struct cv *v;
	int lh[16];

	buf_add(&img, "", 0);
	buf_add(&pos, "", 0);
	buf_add(&size, "", 0);
	buf_add(&rep, "", 0);
	buf_add(&clip, "", 0);
	buf_add(&org, "", 0);
	for (;;) {
		struct buf li = { 0 }, lp = { 0 }, ls = { 0 }, lr = { 0 };
		int boxes = 0, after_slash = 0;

		buf_add(&li, "", 0);
		buf_add(&lp, "", 0);
		buf_add(&ls, "", 0);
		buf_add(&lr, "", 0);
		while ((v = cur_peek(c)) && v->type != CT_COMMA) {
			struct cur save = *c;
			struct color col;

			if (is_image(v) || cv_ident(v, "none")) {
				css_serialize(&li, v, 1);
				cur_next(c);
			} else if (v->type == CT_IDENT && kw_index(
				   "repeat|repeat-x|repeat-y|no-repeat|space|"
				   "round", v->str) >= 0) {
				if (lr.len)
					buf_putc(&lr, ' ');
				css_serialize(&lr, v, 1);
				cur_next(c);
			} else if (v->type == CT_IDENT && kw_index(
				   "scroll|fixed|local", v->str) >= 0) {
				att = kw_index("scroll|fixed|local", v->str);
				cur_next(c);
			} else if (is_box_kw(v)) {
				/* first box: origin, second: clip */
				struct buf *bb = boxes++ ? &clip : &org;

				css_serialize(bb, v, 1);
				cur_next(c);
			} else if (v->type == CT_DELIM && v->delim == '/') {
				after_slash = 1;
				cur_next(c);
			} else if (v->type == CT_DIMENSION ||
				   v->type == CT_PERCENT ||
				   v->type == CT_NUMBER ||
				   cv_ident(v, "left") || cv_ident(v, "right") ||
				   cv_ident(v, "top") || cv_ident(v, "bottom") ||
				   cv_ident(v, "center") || cv_ident(v, "auto") ||
				   cv_ident(v, "cover") ||
				   cv_ident(v, "contain")) {
				struct buf *bb = after_slash ? &ls : &lp;

				if (bb->len)
					buf_putc(bb, ' ');
				css_serialize(bb, v, 1);
				cur_next(c);
			} else if (parse_color(c, &col)) {
				if (have_color)
					goto bad;
				color = col;
				have_color = 1;
			} else {
				*c = save;
				goto bad;
			}
		}
		if (layer) {
			buf_add(&img, ", ", 2);
			buf_add(&pos, ", ", 2);
			buf_add(&size, ", ", 2);
			buf_add(&rep, ", ", 2);
		}
		buf_add(&img, li.len ? li.s : "none", li.len ? li.len : 4);
		buf_add(&pos, lp.len ? lp.s : "0% 0%", lp.len ? lp.len : 5);
		buf_add(&size, ls.len ? ls.s : "auto", ls.len ? ls.len : 4);
		buf_add(&rep, lr.len ? lr.s : "repeat", lr.len ? lr.len : 6);
		buf_free(&li);
		buf_free(&lp);
		buf_free(&ls);
		buf_free(&lr);
		layer++;
		if (!(v = cur_next(c)))
			break;
		if (have_color)
			goto bad;	/* the color only in the last layer */
	}
	shorthand_longhands(SH_BACKGROUND, lh);
	for (i = 0; lh[i] >= 0; i++) {
		struct sval *d = svec_add(out, lh[i]);
		struct buf *src = lh[i] == P_BACKGROUND_IMAGE ? &img :
			lh[i] == P_BACKGROUND_POSITION ? &pos :
			lh[i] == P_BACKGROUND_SIZE ? &size :
			lh[i] == P_BACKGROUND_REPEAT ? &rep :
			lh[i] == P_BACKGROUND_CLIP ? &clip :
			lh[i] == P_BACKGROUND_ORIGIN ? &org : NULL;

		if (lh[i] == P_BACKGROUND_COLOR && have_color) {
			d->color = color;
		} else if (lh[i] == P_BACKGROUND_ATTACHMENT && att >= 0) {
			d->kw = (unsigned char)(att + 1);
		} else if (src && src->len) {
			d->str = xstrndup(src->s, src->len);
		} else {
			d->type = SV_INITIAL;
		}
	}
	buf_free(&img);
	buf_free(&pos);
	buf_free(&size);
	buf_free(&rep);
	buf_free(&clip);
	buf_free(&org);
	return 1;
bad:
	buf_free(&img);
	buf_free(&pos);
	buf_free(&size);
	buf_free(&rep);
	buf_free(&clip);
	buf_free(&org);
	return 0;
}

static int sh_font(struct cur *c, struct svec *out)
{
	static const int pre[4] = { P_FONT_STYLE, P_FONT_VARIANT_CAPS,
		P_FONT_WEIGHT, P_FONT_STRETCH };
	struct range r[7], size, lh, fam;
	const struct cv *v;
	int i, k, normals = 0;

	v = cur_peek(c);
	if (v && v->type == CT_IDENT && kw_index("caption|icon|menu|"
	    "message-box|small-caption|status-bar", v->str) >= 0) {
		int lhs[8];

		cur_next(c);
		if (!cur_end(c))
			return 0;
		/* system fonts: the initial font in the system family */
		shorthand_longhands(SH_FONT, lhs);
		for (i = 0; lhs[i] >= 0; i++) {
			if (lhs[i] == P_FONT_FAMILY)
				svec_add(out, lhs[i])->str = xstrndup(
					"system-ui", 9);
			else
				emit_initial(out, lhs[i]);
		}
		return 1;
	}
	for (i = 0; i < 4; i++)
		r[i].from = -1;
	for (k = 0; k < 4 && (v = cur_peek(c)); k++) {
		if (cv_ident(v, "normal")) {
			normals++;
			cur_next(c);
			continue;
		}
		for (i = 0; i < 4; i++) {
			/* font-variant: only small-caps here */
			if (i == 1 && !cv_ident(v, "small-caps"))
				continue;
			/* font-stretch: only keywords here */
			if (i == 3 && (v->type != CT_IDENT || kw_index(
			    "ultra-condensed|extra-condensed|condensed|"
			    "semi-condensed|semi-expanded|expanded|"
			    "extra-expanded|ultra-expanded", v->str) < 0))
				continue;
			/* font-weight: a number here must not be the size */
			if (r[i].from < 0 && try_at(pre[i], c, &r[i]))
				break;
		}
		if (i == 4)
			break;
	}
	(void)normals;
	if (!try_at(P_FONT_SIZE, c, &size))
		return 0;
	lh.from = -1;
	if ((v = cur_peek(c)) && v->type == CT_DELIM && v->delim == '/') {
		cur_next(c);
		if (!try_at(P_LINE_HEIGHT, c, &lh))
			return 0;
	}
	if (!try_at(P_FONT_FAMILY, c, &fam) || !cur_end(c))
		return 0;
	for (i = 0; i < 4; i++)
		emit_range(pre[i], c, r[i], out);
	emit_range(P_FONT_SIZE, c, size, out);
	emit_range(P_LINE_HEIGHT, c, lh, out);
	emit_range(P_FONT_FAMILY, c, fam, out);
	return 1;
}

static int sh_list_style(struct cur *c, struct svec *out)
{
	struct range type, pos, img;
	const struct cv *v;
	int nones = 0;

	type.from = pos.from = img.from = -1;
	while ((v = cur_peek(c))) {
		struct range *r;

		if (cv_ident(v, "none")) {
			nones++;
			cur_next(c);
			continue;
		}
		if (cv_ident(v, "inside") || cv_ident(v, "outside"))
			r = &pos;
		else if (is_image(v))
			r = &img;
		else if (v->type == CT_IDENT || v->type == CT_STRING)
			r = &type;
		else
			return 0;
		if (r->from >= 0)
			return 0;
		r->from = c->i;
		cur_next(c);
		r->to = c->i;
	}
	if (nones > 2 || (nones == 2 && (type.from >= 0 || img.from >= 0)) ||
	    (nones == 1 && type.from >= 0 && img.from >= 0))
		return 0;
	emit_range(P_LIST_STYLE_POSITION, c, pos, out);
	if (nones && type.from < 0) {
		svec_add(out, P_LIST_STYLE_TYPE)->str = xstrndup("none", 4);
		nones--;
	} else {
		emit_range(P_LIST_STYLE_TYPE, c, type, out);
	}
	emit_range(P_LIST_STYLE_IMAGE, c, img, out);
	return 1;
}

static void emit_number(struct svec *out, int prop, double v)
{
	svec_add(out, prop)->num = v;
}

static int sh_flex(struct cur *c, struct svec *out)
{
	const struct cv *v = cur_peek(c);
	struct range grow, shrink, basis;
	struct sval *d;

	if (cv_ident(v, "none") || cv_ident(v, "auto")) {
		int one = cv_ident(v, "auto");

		cur_next(c);
		if (!cur_end(c))
			return 0;
		emit_number(out, P_FLEX_GROW, one);
		emit_number(out, P_FLEX_SHRINK, one);
		svec_add(out, P_FLEX_BASIS)->kw = 1;	/* auto */
		return 1;
	}
	grow.from = shrink.from = basis.from = -1;
	while ((v = cur_peek(c))) {
		if (v->type == CT_NUMBER && grow.from < 0 && basis.from < 0 &&
		    v->num != 0) {
			try_at(P_FLEX_GROW, c, &grow);
			if ((v = cur_peek(c)) && v->type == CT_NUMBER)
				try_at(P_FLEX_SHRINK, c, &shrink);
		} else if (v->type == CT_NUMBER && grow.from < 0) {
			if (!try_at(P_FLEX_GROW, c, &grow))
				return 0;
			if ((v = cur_peek(c)) && v->type == CT_NUMBER)
				try_at(P_FLEX_SHRINK, c, &shrink);
		} else if (basis.from < 0 && try_at(P_FLEX_BASIS, c, &basis)) {
			;
		} else {
			return 0;
		}
	}
	if (grow.from < 0 && basis.from < 0)
		return 0;
	if (grow.from >= 0)
		emit_range(P_FLEX_GROW, c, grow, out);
	else
		emit_number(out, P_FLEX_GROW, 1);
	if (shrink.from >= 0)
		emit_range(P_FLEX_SHRINK, c, shrink, out);
	else
		emit_number(out, P_FLEX_SHRINK, 1);
	if (basis.from >= 0) {
		emit_range(P_FLEX_BASIS, c, basis, out);
	} else {
		/* flex: <number> means a basis of 0% */
		struct cvlist l;
		struct cur z;

		css_component_values("0%", 2, &l);
		cur_init(&z, &l);
		d = svec_add(out, P_FLEX_BASIS);
		d->len = parse_length(&z, 0);
		cvlist_free(&l);
	}
	return 1;
}

static int set_kw(struct svec *out, int prop, const char *word)
{
	int i = kw_index(props[prop].keywords, word);

	if (i < 0)
		return 0;
	svec_add(out, prop)->kw = (unsigned char)(i + 1);
	return 1;
}

static int sh_white_space(struct cur *c, struct svec *out)
{
	static const char *const map[][3] = {
		{ "normal", "collapse", "wrap" }, { "pre", "preserve", "nowrap" },
		{ "nowrap", "collapse", "nowrap" },
		{ "pre-wrap", "preserve", "wrap" },
		{ "pre-line", "preserve-breaks", "wrap" },
		{ "break-spaces", "break-spaces", "wrap" },
		{ NULL, NULL, NULL }
	};
	const struct cv *v = cur_next(c);
	int i;

	if (!v || v->type != CT_IDENT || !cur_end(c))
		return 0;
	for (i = 0; map[i][0]; i++)
		if (ascii_ieq(v->str, map[i][0])) {
			set_kw(out, P_WHITE_SPACE_COLLAPSE, map[i][1]);
			set_kw(out, P_TEXT_WRAP_MODE, map[i][2]);
			return 1;
		}
	return 0;
}

static int sh_text_wrap(struct cur *c, struct svec *out)
{
	int lh[2] = { P_TEXT_WRAP_MODE, P_TEXT_WRAP_STYLE };
	struct range r[2];

	if (!any_order(c, lh, 2, r) || !cur_end(c))
		return 0;
	emit_range(lh[0], c, r[0], out);
	emit_range(lh[1], c, r[1], out);
	return 1;
}

static int sh_grid_area(struct cur *c, struct svec *out)
{
	static const int lh[4] = { P_GRID_ROW_START, P_GRID_COLUMN_START,
		P_GRID_ROW_END, P_GRID_COLUMN_END };
	struct range r[4];
	int n = 0, i, start;

	cur_peek(c);
	start = c->i;
	for (i = start; i <= c->n && n < 4; i++)
		if (i == c->n || (c->v[i].type == CT_DELIM &&
				  c->v[i].delim == '/')) {
			r[n].from = start;
			r[n].to = i;
			n++;
			start = i + 1;
		}
	if (i < c->n)
		return 0;
	c->i = c->n;
	/* missing parts copy a custom ident: column-start from row-start,
	 * the ends from their starts (CSS Grid 8.4); else auto */
	for (i = n; i < 4; i++) {
		int from = i == 1 ? 0 : i - 2;
		struct cur t;
		const struct cv *v;

		t.v = c->v;
		t.i = r[from].from;
		t.n = r[from].from >= 0 ? r[from].to : 0;
		v = r[from].from >= 0 ? cur_next(&t) : NULL;
		if (v && v->type == CT_IDENT && !cv_ident(v, "auto") &&
		    !cv_ident(v, "span") && cur_end(&t))
			r[i] = r[from];
		else
			r[i].from = -1;
	}
	for (i = 0; i < 4; i++)
		emit_range(lh[i], c, r[i], out);
	return 1;
}

int parse_shorthand_value(int sh, const struct cvlist *value,
	struct svec *out)
{
	struct cur c;
	int start = out->n, ok = 0;

	cur_init(&c, value);
	switch (sh) {
	case SH_MARGIN: case SH_PADDING: case SH_INSET:
	case SH_BORDER_WIDTH: case SH_BORDER_STYLE: case SH_BORDER_COLOR:
		ok = sh_box4(box_props[sh], &c, out, 0);
		break;
	case SH_BORDER_RADIUS:
		ok = sh_box4(box_props[sh], &c, out, 1);
		break;
	case SH_BORDER: case SH_BORDER_TOP: case SH_BORDER_RIGHT:
	case SH_BORDER_BOTTOM: case SH_BORDER_LEFT: case SH_OUTLINE:
	case SH_BORDER_INLINE: case SH_BORDER_BLOCK:
		ok = sh_border(sh, &c, out);
		break;
	case SH_OVERFLOW:
		ok = sh_pair(P_OVERFLOW_X, P_OVERFLOW_Y, &c, out, 0);
		break;
	case SH_BACKGROUND:
		ok = sh_background(&c, out);
		break;
	case SH_FONT:
		ok = sh_font(&c, out);
		break;
	case SH_LIST_STYLE:
		ok = sh_list_style(&c, out);
		break;
	case SH_FLEX:
		ok = sh_flex(&c, out);
		break;
	case SH_FLEX_FLOW: {
		int lh[2] = { P_FLEX_DIRECTION, P_FLEX_WRAP };
		struct range r[2];

		ok = any_order(&c, lh, 2, r) && cur_end(&c);
		if (ok) {
			emit_range(lh[0], &c, r[0], out);
			emit_range(lh[1], &c, r[1], out);
		}
		break;
	}
	case SH_GAP:
		ok = sh_pair(P_ROW_GAP, P_COLUMN_GAP, &c, out, 0);
		break;
	case SH_PLACE_ITEMS:
	case SH_PLACE_CONTENT:
	case SH_PLACE_SELF: {
		/* raw values: one or two idents */
		static const int a[] = { P_ALIGN_ITEMS, P_ALIGN_CONTENT,
			P_ALIGN_SELF };
		static const int b[] = { P_JUSTIFY_ITEMS, P_JUSTIFY_CONTENT,
			P_JUSTIFY_SELF };
		const struct cv *v1 = cur_next(&c), *v2 = cur_next(&c);
		struct range r1, r2;

		if (!v1 || cur_next(&c))
			break;
		r1.from = (int)(v1 - c.v);
		r1.to = r1.from + 1;
		r2 = r1;
		if (v2) {
			r2.from = (int)(v2 - c.v);
			r2.to = r2.from + 1;
		}
		emit_range(a[sh - SH_PLACE_ITEMS], &c, r1, out);
		emit_range(b[sh - SH_PLACE_ITEMS], &c, r2, out);
		ok = 1;
		break;
	}
	case SH_TEXT_DECORATION: {
		int lh[4] = { P_TEXT_DECORATION_LINE, P_TEXT_DECORATION_STYLE,
			P_TEXT_DECORATION_COLOR, P_TEXT_DECORATION_THICKNESS };
		struct range r[4];
		int i;

		ok = any_order(&c, lh, 4, r) && cur_end(&c);
		for (i = 0; ok && i < 4; i++)
			emit_range(lh[i], &c, r[i], out);
		break;
	}
	case SH_WHITE_SPACE:
		ok = sh_white_space(&c, out);
		break;
	case SH_TEXT_WRAP:
		ok = sh_text_wrap(&c, out);
		break;
	case SH_GRID_ROW:
		ok = sh_pair(P_GRID_ROW_START, P_GRID_ROW_END, &c, out, 1);
		break;
	case SH_GRID_COLUMN:
		ok = sh_pair(P_GRID_COLUMN_START, P_GRID_COLUMN_END, &c, out,
			1);
		break;
	case SH_GRID_AREA:
		ok = sh_grid_area(&c, out);
		break;
	case SH_MARGIN_INLINE:
		ok = sh_pair(P_MARGIN_LEFT, P_MARGIN_RIGHT, &c, out, 0);
		break;
	case SH_MARGIN_BLOCK:
		ok = sh_pair(P_MARGIN_TOP, P_MARGIN_BOTTOM, &c, out, 0);
		break;
	case SH_PADDING_INLINE:
		ok = sh_pair(P_PADDING_LEFT, P_PADDING_RIGHT, &c, out, 0);
		break;
	case SH_PADDING_BLOCK:
		ok = sh_pair(P_PADDING_TOP, P_PADDING_BOTTOM, &c, out, 0);
		break;
	case SH_INSET_INLINE:
		ok = sh_pair(P_LEFT, P_RIGHT, &c, out, 0);
		break;
	case SH_INSET_BLOCK:
		ok = sh_pair(P_TOP, P_BOTTOM, &c, out, 0);
		break;
	case SH_ALL:
		break;	/* only CSS-wide keywords, see parse_declaration */
	}
	if (!ok) {
		while (out->n > start)
			sval_free(&out->v[--out->n]);
		return 0;
	}
	return 1;
}

static int wide_keyword(const struct cvlist *value)
{
	struct cur c;
	const struct cv *v;

	cur_init(&c, value);
	v = cur_next(&c);
	if (!v || v->type != CT_IDENT || !cur_end(&c))
		return 0;
	if (ascii_ieq(v->str, "inherit"))
		return SV_INHERIT;
	if (ascii_ieq(v->str, "initial"))
		return SV_INITIAL;
	if (ascii_ieq(v->str, "unset"))
		return SV_UNSET;
	if (ascii_ieq(v->str, "revert") || ascii_ieq(v->str, "revert-layer"))
		return SV_REVERT;
	return 0;
}

/* Parse a declaration into longhand values appended to out. */
int parse_declaration(const char *name, const struct cvlist *value,
	int important, struct svec *out)
{
	int prop, sh = -1, start = out->n, wide, i, lh[NPROPS + 1];

	for (i = 0; aliases[i][0]; i++)
		if (ascii_ieq(name, aliases[i][0])) {
			name = aliases[i][1];
			break;
		}
	prop = prop_lookup(name);
	if (prop < 0 && (sh = shorthand_lookup(name)) < 0)
		return 0;
	wide = wide_keyword(value);
	if (wide || has_var(value->v, value->n)) {
		char *text = wide ? NULL : serialize_text(value->v, value->n);

		if (prop >= 0) {
			lh[0] = prop;
			lh[1] = -1;
		} else {
			shorthand_longhands(sh, lh);
		}
		for (i = 0; lh[i] >= 0; i++) {
			struct sval *d = svec_add(out, lh[i]);

			d->type = (unsigned char)(wide ? wide : SV_PENDING);
			d->shorthand = (short)(prop >= 0 ? -1 : sh);
			if (text)
				d->str = xstrndup(text, strlen(text));
		}
		free(text);
	} else if (prop >= 0) {
		struct sval *d = svec_add(out, prop);

		if (!parse_longhand_value(prop, value, d)) {
			out->n--;
			return 0;
		}
	} else if (!parse_shorthand_value(sh, value, out)) {
		return 0;
	}
	for (i = start; i < out->n; i++)
		out->v[i].important = (unsigned char)important;
	return 1;
}

/* Computed values */

static const float font_size_px[] = {
	9.6f, 12, 13.333333f, 16, 19.2f, 24, 32, 48
};

static float parent_font_size(const struct cctx *c)
{
	return c->parent ? c->parent->v[P_FONT_SIZE].u.len.px : 16;
}

void compute_value(int prop, const struct sval *sv, struct cctx *c,
	struct cval *out)
{
	const struct propdef *d = &props[prop];

	memset(out, 0, sizeof(*out));
	if (sv->kw && d->kind != K_FSIZE && d->kind != K_FWEIGHT &&
	    d->kind != K_BWIDTH) {
		out->kw = sv->kw;
		return;
	}
	switch (d->kind) {
	case K_KW:
	case K_DISPLAY:
		out->kw = sv->kw;
		break;
	case K_LEN:
		if (sv->len) {
			slen_resolve(sv->len, &c->u, &out->u.len);
			if (out->u.len.calc)
				out->u.len.calc = db_calc(c->db,
					out->u.len.calc);
		}
		break;
	case K_BWIDTH:
		if (sv->kw) {
			static const float w[] = { 1, 3, 5 };

			out->u.len.px = w[sv->kw - 1];
		} else if (sv->len) {
			slen_resolve(sv->len, &c->u, &out->u.len);
		}
		/* Device pixels: round down widths of 1px and more. */
		if (out->u.len.px >= 1)
			out->u.len.px = (float)(int)out->u.len.px;
		break;
	case K_COLOR:
		if (sv->color.current && prop != P_COLOR) {
			out->u.color = c->self->v[P_COLOR].u.color;
		} else if (sv->color.current) {
			out->u.color = c->parent ?
				c->parent->v[P_COLOR].u.color :
				db_color(c->db, &sv->color);
			if (!c->parent) {
				struct color black;

				named_color("canvastext", &black);
				out->u.color = db_color(c->db, &black);
			}
		} else {
			out->u.color = db_color(c->db, &sv->color);
		}
		break;
	case K_NUM:
	case K_INT:
		out->u.num = sv->num;
		break;
	case K_ALPHA:
		out->u.num = sv->num < 0 ? 0 : sv->num > 1 ? 1 : sv->num;
		break;
	case K_FSIZE: {
		float pfs = parent_font_size(c);

		if (sv->kw) {
			int k = sv->kw - 1;

			if (k < 8)
				out->u.len.px = font_size_px[k];
			else if (k == 8)
				out->u.len.px = pfs * 1.2f;
			else if (k == 9)
				out->u.len.px = pfs / 1.2f;
			else
				out->u.len.px = pfs;
		} else if (sv->len) {
			struct ucontext u = c->u;
			struct len l;

			u.em = pfs;
			u.ex = u.ch = pfs / 2;
			slen_resolve(sv->len, &u, &l);
			out->u.len.px = l.px + l.pct * pfs / 100;
			if (l.calc) {
				out->u.len.px = calc_eval(l.calc, pfs);
				calc_release(l.calc);
			}
			if (out->u.len.px < 0)
				out->u.len.px = 0;
		}
		break;
	}
	case K_FWEIGHT: {
		double p = c->parent ? c->parent->v[P_FONT_WEIGHT].u.num : 400;

		switch (sv->kw) {
		case 0: out->u.num = sv->num; break;
		case 1: out->u.num = 400; break;
		case 2: out->u.num = 700; break;
		case 3:
			out->u.num = p < 350 ? 400 : p < 550 ? 700 :
				p < 900 ? 900 : p;
			break;
		case 4:
			out->u.num = p < 100 ? p : p < 550 ? 100 :
				p < 750 ? 400 : 700;
			break;
		}
		break;
	}
	case K_LH:
		if (sv->len) {
			struct len l;

			if (slen_resolve(sv->len, &c->u, &l) == 2) {
				out->flag = 1;
				out->u.num = l.px;
			} else {
				out->flag = 2;
				out->u.len.px = l.px + l.pct * c->u.em / 100;
				if (l.calc) {
					out->u.len.px = calc_eval(l.calc,
						c->u.em);
					calc_release(l.calc);
				}
			}
		}
		break;
	case K_FAMILY:
	case K_RAW:
	case K_LIST:
		out->u.str = db_string(c->db, sv->str, strlen(sv->str));
		break;
	case K_DECO:
		out->u.num = sv->num;
		break;
	}
}

void serialize_value(int prop, const struct cval *v, struct buf *b)
{
	const struct propdef *d = &props[prop];
	static const char *const deco[] = {
		"underline", "overline", "line-through", "blink"
	};
	int i, first = 1;

	buf_add(b, "", 0);
	if (v->kw) {
		kw_name(d->keywords, v->kw - 1, b);
		return;
	}
	switch (d->kind) {
	case K_LEN:
	case K_BWIDTH:
	case K_FSIZE:
		serialize_len(b, &v->u.len);
		break;
	case K_LH:
		if (v->flag == 1) {
			serialize_float(b, v->u.num);
		} else {
			serialize_float(b, v->u.len.px);
			buf_add(b, "px", 2);
		}
		break;
	case K_COLOR:
		if (v->u.color)
			serialize_color(b, v->u.color);
		break;
	case K_NUM:
	case K_INT:
	case K_ALPHA:
	case K_FWEIGHT:
		serialize_number(b, v->u.num);
		break;
	case K_FAMILY:
	case K_RAW:
	case K_LIST:
		if (v->u.str)
			buf_add(b, v->u.str, strlen(v->u.str));
		break;
	case K_DECO:
		if (!(int)v->u.num) {
			buf_add(b, "none", 4);
			break;
		}
		for (i = 0; i < 4; i++)
			if ((int)v->u.num & (1 << i)) {
				if (!first)
					buf_putc(b, ' ');
				buf_add(b, deco[i], strlen(deco[i]));
				first = 0;
			}
		break;
	}
}
