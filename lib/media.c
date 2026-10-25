/*
 * Media queries level 4 and @supports conditions.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>
#include "css.h"

/* Three-valued logic of the spec: unknown is neither true nor false. */
#define UNKNOWN (-1)

static int is_delim(const struct cv *v, int c)
{
	return v && v->type == CT_DELIM && v->delim == c;
}

/* A <mf-value>: number, dimension, ratio or ident, at the cursor. */
struct mval {
	int type;		/* 0 none, 1 number/length in px, 2 ident */
	double v;
	const char *ident;
};

static int mf_value(struct cur *c, struct mval *out)
{
	const struct cv *v = cur_peek(c);
	struct slen *l;
	struct ucontext u = { 16, 16, 0, 0, 8, 8 };
	struct len len;

	memset(out, 0, sizeof(*out));
	if (!v)
		return 0;
	if (v->type == CT_IDENT) {
		out->type = 2;
		out->ident = v->str;
		cur_next(c);
		return 1;
	}
	if (v->type == CT_NUMBER) {
		cur_next(c);
		out->type = 1;
		out->v = v->num;
		if (is_delim(cur_peek(c), '/')) {
			const struct cv *d;

			cur_next(c);
			d = cur_next(c);
			if (!d || d->type != CT_NUMBER || d->num == 0)
				return 0;
			out->v /= d->num;
		}
		return 1;
	}
	if (v->type == CT_DIMENSION) {
		const char *s = v->str;

		if (ascii_ieq(s, "dppx") || ascii_ieq(s, "x")) {
			out->type = 1;
			out->v = v->num;
			cur_next(c);
			return 1;
		}
		if (ascii_ieq(s, "dpi") || ascii_ieq(s, "dpcm")) {
			out->type = 1;
			out->v = v->num / (ascii_ieq(s, "dpi") ? 96 : 96 / 2.54);
			cur_next(c);
			return 1;
		}
	}
	l = parse_length(c, L_NOPCT);
	if (!l)
		return 0;
	slen_resolve(l, &u, &len);
	if (len.calc)
		calc_release(len.calc);
	slen_free(l);
	out->type = 1;
	out->v = len.px;
	return 1;
}

static int cmp_op(struct cur *c, int *op)
{
	const struct cv *v = cur_peek(c);
	int eq = 0;

	if (!v || v->type != CT_DELIM)
		return 0;
	if (v->delim != '<' && v->delim != '>' && v->delim != '=')
		return 0;
	*op = v->delim;
	cur_next(c);
	if (*op != '=' && c->i < c->n && is_delim(&c->v[c->i], '=')) {
		c->i++;
		eq = 1;
	}
	if (eq)
		*op = *op == '<' ? 'l' : 'g';	/* <= >= */
	return 1;
}

static int compare(double a, int op, double b)
{
	switch (op) {
	case '<': return a < b;
	case '>': return a > b;
	case 'l': return a <= b;
	case 'g': return a >= b;
	}
	return a == b;
}

/* Value of a numeric feature, or -1 when not numeric/unknown. */
static int feature_num(const char *n, const struct env *e, double *v)
{
	if (ascii_ieq(n, "width") || ascii_ieq(n, "device-width"))
		*v = e->width;
	else if (ascii_ieq(n, "height") || ascii_ieq(n, "device-height"))
		*v = e->height;
	else if (ascii_ieq(n, "aspect-ratio") ||
		 ascii_ieq(n, "device-aspect-ratio"))
		*v = e->height ? e->width / e->height : 0;
	else if (ascii_ieq(n, "resolution") ||
		 ascii_ieq(n, "-webkit-device-pixel-ratio"))
		*v = 1;
	else if (ascii_ieq(n, "color"))
		*v = 8;
	else if (ascii_ieq(n, "color-index") || ascii_ieq(n, "monochrome") ||
		 ascii_ieq(n, "grid"))
		*v = 0;
	else
		return 0;
	return 1;
}

/* Value of a discrete feature. */
static const char *feature_ident(const char *n, const struct env *e)
{
	static const char *const fixed[][2] = {
		{ "hover", "hover" }, { "any-hover", "hover" },
		{ "pointer", "fine" }, { "any-pointer", "fine" },
		{ "prefers-reduced-motion", "no-preference" },
		{ "prefers-contrast", "no-preference" },
		{ "prefers-reduced-transparency", "no-preference" },
		{ "prefers-reduced-data", "no-preference" },
		{ "forced-colors", "none" }, { "inverted-colors", "none" },
		{ "display-mode", "browser" }, { "dynamic-range", "standard" },
		{ "video-dynamic-range", "standard" },
		{ "color-gamut", "srgb" }, { "overflow-block", "scroll" },
		{ "overflow-inline", "scroll" },
		{ NULL, NULL }
	};
	int i;

	if (ascii_ieq(n, "orientation"))
		return e->height >= e->width ? "portrait" : "landscape";
	if (ascii_ieq(n, "prefers-color-scheme"))
		return e->dark ? "dark" : "light";
	if (ascii_ieq(n, "scripting"))
		return e->scripting ? "enabled" : "none";
	if (ascii_ieq(n, "update"))
		return e->print ? "none" : "fast";
	for (i = 0; fixed[i][0]; i++)
		if (ascii_ieq(n, fixed[i][0]))
			return fixed[i][1];
	return NULL;
}

/* Boolean context: is the feature not zero / not none? */
static int feature_bool(const char *n, const struct env *e)
{
	double v;
	const char *s;

	if (feature_num(n, e, &v))
		return v != 0;
	if ((s = feature_ident(n, e)))
		return strcmp(s, "none") && strcmp(s, "no-preference");
	return UNKNOWN;
}

static int eval_feature(const struct cv *block, const struct env *e)
{
	struct cur c;
	const struct cv *v;
	struct mval a, b;
	int op, op2, prefix = 0;
	const char *name;
	double fv;

	cur_init(&c, &block->kids);
	v = cur_peek(&c);
	if (v && v->type == CT_IDENT && (c.i + 1 >= c.n ||
	    c.v[c.i + 1].type == CT_COLON || c.v[c.i + 1].type == CT_WS ||
	    c.v[c.i + 1].type == CT_DELIM)) {
		struct cur save = c;

		name = v->str;
		cur_next(&c);
		if (cur_end(&c))
			return feature_bool(name, e);
		if (cur_peek(&c)->type == CT_COLON) {
			cur_next(&c);
			if (!strncmp(name, "min-", 4) ||
			    !strncmp(name, "max-", 4)) {
				prefix = name[1];
				name += 4;
			}
			if (!mf_value(&c, &a) || !cur_end(&c))
				return UNKNOWN;
			if (a.type == 2) {
				const char *s = feature_ident(name, e);

				if (prefix || !s)
					return UNKNOWN;
				return ascii_ieq(s, a.ident);
			}
			if (!feature_num(name, e, &fv))
				return UNKNOWN;
			if (prefix == 'i')
				return fv >= a.v;
			if (prefix == 'a')
				return fv <= a.v;
			return fv == a.v;
		}
		/* (width > 600px) */
		if (cmp_op(&c, &op) && mf_value(&c, &a) && cur_end(&c) &&
		    a.type == 1 && feature_num(name, e, &fv))
			return compare(fv, op, a.v);
		c = save;
	}
	/* (600px < width) and (400px < width < 800px) */
	if (!mf_value(&c, &a) || a.type != 1 || !cmp_op(&c, &op))
		return UNKNOWN;
	v = cur_next(&c);
	if (!v || v->type != CT_IDENT || !feature_num(v->str, e, &fv))
		return UNKNOWN;
	if (!compare(a.v, op, fv)) {
		if (cur_end(&c))
			return 0;
		if (!cmp_op(&c, &op2) || !mf_value(&c, &b) || !cur_end(&c))
			return UNKNOWN;
		return 0;
	}
	if (cur_end(&c))
		return 1;
	if (!cmp_op(&c, &op2) || !mf_value(&c, &b) || b.type != 1 ||
	    !cur_end(&c))
		return UNKNOWN;
	return compare(fv, op2, b.v);
}

static int eval_condition(struct cur *c, const struct env *e, int supports);

static int eval_in_parens(struct cur *c, const struct env *e, int supports)
{
	const struct cv *v = cur_next(c);
	struct cur in;

	if (!v)
		return UNKNOWN;
	if (supports && v->type == CT_FUNCTION) {
		if (ascii_ieq(v->str, "selector")) {
			struct sellist *s = selector_parse(&v->kids, 0,
				v->kids.n, NULL, 0);

			if (s)
				sellist_free(s);
			return s != NULL;
		}
		return 0;	/* font-tech(), font-format()... */
	}
	if (v->type != CT_BLOCK || v->delim != '(')
		return UNKNOWN;
	cur_init(&in, &v->kids);
	{
		const struct cv *f = cur_peek(&in);

		if (f && (cv_ident(f, "not") || (f->type == CT_BLOCK &&
		    f->delim == '(') || (supports && f->type == CT_FUNCTION))) {
			int r = eval_condition(&in, e, supports);

			return cur_end(&in) ? r : UNKNOWN;
		}
	}
	if (supports) {
		/* (property: value) */
		const struct cv *n = cur_next(&in), *colon = cur_next(&in);
		struct cvlist val;
		struct svec out = { NULL, 0, 0 };
		int ok;

		if (!n || n->type != CT_IDENT || !colon ||
		    colon->type != CT_COLON)
			return 0;
		if (!strncmp(n->str, "--", 2))
			return 1;
		cur_peek(&in);
		val.v = (struct cv *)in.v + in.i;
		val.n = in.n - in.i;
		val.cap = 0;
		/* "!important" is allowed in the value */
		ok = parse_declaration(n->str, &val, 0, &out);
		svec_free(&out);
		return ok;
	}
	return eval_feature(v, e);
}

static int eval_condition(struct cur *c, const struct env *e, int supports)
{
	const struct cv *v = cur_peek(c);
	int r, x, op = 0;

	if (cv_ident(v, "not")) {
		cur_next(c);
		r = eval_in_parens(c, e, supports);
		return r == UNKNOWN ? UNKNOWN : !r;
	}
	r = eval_in_parens(c, e, supports);
	while ((v = cur_peek(c)) && (cv_ident(v, "and") || cv_ident(v, "or"))) {
		int this = cv_ident(v, "and") ? 'a' : 'o';

		if (op && op != this)
			return UNKNOWN;	/* mixing and/or needs parens */
		op = this;
		cur_next(c);
		x = eval_in_parens(c, e, supports);
		if (op == 'a')
			r = (r == 0 || x == 0) ? 0 :
				(r == UNKNOWN || x == UNKNOWN) ? UNKNOWN : 1;
		else
			r = (r == 1 || x == 1) ? 1 :
				(r == UNKNOWN || x == UNKNOWN) ? UNKNOWN : 0;
	}
	return r;
}

/* One media query in [c, end of c). */
static int eval_query(struct cur *c, const struct env *e)
{
	const struct cv *v = cur_peek(c), *w;
	struct cur look;
	int not = 0, r, x;

	if (!v)
		return 0;
	if (cv_ident(v, "not") || cv_ident(v, "only")) {
		look = *c;
		cur_next(&look);
		w = cur_peek(&look);
		/* "not (cond)" is a condition, "not screen" a type query */
		if (cv_ident(v, "not") && w && w->type == CT_BLOCK) {
			r = eval_condition(c, e, 0);
			return cur_end(c) && r == 1;
		}
		not = cv_ident(v, "not");
		*c = look;
		v = w;
		if (!v || v->type != CT_IDENT)
			return 0;
	}
	if (v->type == CT_IDENT) {
		cur_next(c);
		if (ascii_ieq(v->str, "all"))
			r = 1;
		else if (ascii_ieq(v->str, "screen"))
			r = !e->print;
		else if (ascii_ieq(v->str, "print"))
			r = e->print;
		else
			r = 0;
		if (cv_ident(cur_peek(c), "and")) {
			cur_next(c);
			x = eval_condition(c, e, 0);
			r = r && x == 1;
		}
		if (!cur_end(c))
			return 0;
		return not ? !r : r;
	}
	r = eval_condition(c, e, 0);
	return cur_end(c) && r == 1;
}

int media_match(const struct cvlist *l, int from, int to,
	const struct env *e)
{
	int i, start = from, any = 0;

	for (i = from; i < to && l->v[i].type == CT_WS; i++)
		;
	if (i == to)
		return 1;	/* empty list: all */
	for (i = from; i <= to; i++) {
		if (i == to || l->v[i].type == CT_COMMA) {
			struct cur c;

			c.v = l->v;
			c.i = start;
			c.n = i;
			if (eval_query(&c, e))
				any = 1;
			start = i + 1;
		}
	}
	return any;
}

int supports_match(const struct cvlist *l, int from, int to)
{
	struct cur c;
	struct env e;
	int r;

	memset(&e, 0, sizeof(e));
	c.v = l->v;
	c.i = from;
	c.n = to;
	r = eval_condition(&c, &e, 1);
	return cur_end(&c) && r == 1;
}
