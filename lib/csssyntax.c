/*
 * CSS Syntax Level 3: tokenizer, component values, rules and
 * declarations, after the current editor's draft (with nesting).
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "css.h"

/* Tokenizer */

struct ctok {
	const unsigned char *s;
	size_t pos, len;
};

#define CEOF (-1)

static int at(struct ctok *t, size_t k)
{
	return t->pos + k < t->len ? t->s[t->pos + k] : CEOF;
}

static int is_nl(int c)
{
	return c == '\n';
}

static int is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\n';
}

static int is_digit(int c)
{
	return c >= '0' && c <= '9';
}

static int is_hex(int c)
{
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_name_start(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
		c >= 0x80;
}

static int is_name(int c)
{
	return is_name_start(c) || is_digit(c) || c == '-';
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int ascii_ieq(const char *a, const char *b)
{
	while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
		a++, b++;
	return !*a && !*b;
}

/* A backslash before EOF is valid: it becomes U+FFFD. */
static int valid_escape(int c1, int c2)
{
	return c1 == '\\' && c2 != '\n';
}

static int starts_ident(int c1, int c2, int c3)
{
	if (c1 == '-')
		return is_name_start(c2) || c2 == '-' || valid_escape(c2, c3);
	if (is_name_start(c1))
		return 1;
	return valid_escape(c1, c2);
}

static int starts_number(int c1, int c2, int c3)
{
	if (c1 == '+' || c1 == '-')
		return is_digit(c2) || (c2 == '.' && is_digit(c3));
	if (c1 == '.')
		return is_digit(c2);
	return is_digit(c1);
}

/* Decode one UTF-8 sequence (input is valid UTF-8). */
static unsigned long utf8_get(struct ctok *t)
{
	unsigned long c = t->s[t->pos++];
	int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

	if (n)
		c &= 0x3f >> n;
	while (n-- && t->pos < t->len)
		c = c << 6 | (t->s[t->pos++] & 0x3f);
	return c;
}

/* After the backslash. */
static void consume_escape(struct ctok *t, struct buf *out)
{
	unsigned long v = 0;
	int i;

	if (t->pos >= t->len) {
		buf_add(out, "\xef\xbf\xbd", 3);
		return;
	}
	if (!is_hex(at(t, 0))) {
		buf_put_utf8(out, utf8_get(t));
		return;
	}
	for (i = 0; i < 6 && is_hex(at(t, 0)); i++) {
		int c = t->s[t->pos++];

		v = v * 16 + (unsigned long)(is_digit(c) ? c - '0' :
			lower(c) - 'a' + 10);
	}
	if (is_space(at(t, 0)))
		t->pos++;
	if (v == 0 || (v >= 0xd800 && v <= 0xdfff) || v > 0x10ffff)
		v = 0xfffd;
	buf_put_utf8(out, v);
}

static void consume_name(struct ctok *t, struct buf *out)
{
	for (;;) {
		int c = at(t, 0);

		if (is_name(c)) {
			if (c < 0x80)
				buf_putc(out, c), t->pos++;
			else
				buf_put_utf8(out, utf8_get(t));
		} else if (valid_escape(c, at(t, 1))) {
			t->pos++;
			consume_escape(t, out);
		} else {
			return;
		}
	}
}

/*
 * Numbers: all digits go in one mantissa, scaled once by a power of
 * ten, so that ".67" gives the double nearest to 0.67 (no strtod():
 * it depends on the locale of the host).
 */
static double pow10i(int e)
{
	double r = 1, b = 10;

	if (e > 400)
		e = 400;
	while (e) {
		if (e & 1)
			r *= b;
		b *= b;
		e >>= 1;
	}
	return r;
}

static void consume_number(struct ctok *t, struct cv *v)
{
	size_t start = t->pos;
	double mant = 0, sign = 1;
	int integer = 1, scale = 0, e = 0, esign = 1, nd = 0;

	if (at(t, 0) == '+' || at(t, 0) == '-') {
		if (at(t, 0) == '-')
			sign = -1;
		t->pos++;
	}
	while (is_digit(at(t, 0))) {
		if (nd < 19)
			mant = mant * 10 + (t->s[t->pos] - '0'), nd += mant > 0;
		else
			scale++;
		t->pos++;
	}
	if (at(t, 0) == '.' && is_digit(at(t, 1))) {
		t->pos++;
		while (is_digit(at(t, 0))) {
			if (nd < 19) {
				mant = mant * 10 + (t->s[t->pos] - '0');
				nd += mant > 0;
				scale--;
			}
			t->pos++;
		}
		integer = 0;
	}
	if ((at(t, 0) == 'e' || at(t, 0) == 'E') &&
	    (is_digit(at(t, 1)) || ((at(t, 1) == '+' || at(t, 1) == '-') &&
				     is_digit(at(t, 2))))) {
		t->pos++;
		if (at(t, 0) == '-' || at(t, 0) == '+') {
			if (at(t, 0) == '-')
				esign = -1;
			t->pos++;
		}
		while (is_digit(at(t, 0))) {
			if (e < 10000)
				e = e * 10 + (t->s[t->pos] - '0');
			t->pos++;
		}
		integer = 0;
	}
	e = esign * e + scale;
	v->num = sign * (e < 0 ? mant / pow10i(-e) : mant * pow10i(e));
	v->integer = (unsigned char)integer;
	v->repr = xstrndup((const char *)t->s + start, t->pos - start);
}

static void bad_url_remnants(struct ctok *t)
{
	for (;;) {
		int c = at(t, 0);

		if (c == CEOF)
			return;
		t->pos++;
		if (c == ')')
			return;
		if (valid_escape(c, at(t, 0))) {
			struct buf junk = { 0 };

			consume_escape(t, &junk);
			buf_free(&junk);
		}
	}
}

static void consume_url(struct ctok *t, struct cv *v)
{
	struct buf b = { 0 };

	buf_add(&b, "", 0);
	while (is_space(at(t, 0)))
		t->pos++;
	for (;;) {
		int c = at(t, 0);

		if (c == ')' || c == CEOF) {
			if (c == ')')
				t->pos++;
			break;
		}
		if (is_space(c)) {
			while (is_space(at(t, 0)))
				t->pos++;
			if (at(t, 0) == ')' || at(t, 0) == CEOF)
				continue;
			bad_url_remnants(t);
			v->type = CT_BAD_URL;
			break;
		}
		if (c == '"' || c == '\'' || c == '(' || c < 0x09 ||
		    c == 0x0b || (c >= 0x0e && c <= 0x1f) || c == 0x7f) {
			bad_url_remnants(t);
			v->type = CT_BAD_URL;
			break;
		}
		if (c == '\\') {
			if (valid_escape(c, at(t, 1))) {
				t->pos++;
				consume_escape(t, &b);
				continue;
			}
			bad_url_remnants(t);
			v->type = CT_BAD_URL;
			break;
		}
		buf_putc(&b, c);
		t->pos++;
	}
	if (v->type == CT_BAD_URL)
		buf_free(&b);
	else
		v->str = b.s;
}

static void consume_string(struct ctok *t, int q, struct cv *v)
{
	struct buf b = { 0 };

	buf_add(&b, "", 0);
	v->type = CT_STRING;
	for (;;) {
		int c = at(t, 0);

		if (c == CEOF)
			break;
		if (c == q) {
			t->pos++;
			break;
		}
		if (is_nl(c)) {
			v->type = CT_BAD_STRING;
			buf_free(&b);
			return;
		}
		t->pos++;
		if (c == '\\') {
			if (at(t, 0) == CEOF)
				continue;
			if (is_nl(at(t, 0))) {
				t->pos++;
				continue;
			}
			consume_escape(t, &b);
			continue;
		}
		buf_putc(&b, c);
	}
	v->str = b.s;
}

static void skip_comments(struct ctok *t)
{
	while (at(t, 0) == '/' && at(t, 1) == '*') {
		t->pos += 2;
		while (t->pos < t->len &&
		       !(at(t, 0) == '*' && at(t, 1) == '/'))
			t->pos++;
		t->pos = t->pos + 2 > t->len ? t->len : t->pos + 2;
	}
}

static char *take_name(struct ctok *t)
{
	struct buf b = { 0 };

	buf_add(&b, "", 0);
	consume_name(t, &b);
	return b.s;
}

static void consume_token(struct ctok *t, struct cv *v)
{
	int c;

	memset(v, 0, sizeof(*v));
	skip_comments(t);
	c = at(t, 0);
	if (c == CEOF) {
		v->type = CT_EOF;
		return;
	}
	if (is_space(c)) {
		while (is_space(at(t, 0)))
			t->pos++;
		v->type = CT_WS;
		return;
	}
	if (c == '"' || c == '\'') {
		t->pos++;
		consume_string(t, c, v);
		return;
	}
	if (c == '-' && at(t, 1) == '-' && at(t, 2) == '>') {
		t->pos += 3;
		v->type = CT_CDC;
		return;
	}
	if (starts_number(c, at(t, 1), at(t, 2))) {
		consume_number(t, v);
		if (starts_ident(at(t, 0), at(t, 1), at(t, 2))) {
			v->type = CT_DIMENSION;
			v->str = take_name(t);
		} else if (at(t, 0) == '%') {
			t->pos++;
			v->type = CT_PERCENT;
		} else {
			v->type = CT_NUMBER;
		}
		return;
	}
	if (starts_ident(c, at(t, 1), at(t, 2))) {
		v->str = take_name(t);
		if (at(t, 0) != '(') {
			v->type = CT_IDENT;
			return;
		}
		t->pos++;
		if (ascii_ieq(v->str, "url")) {
			size_t p = t->pos;

			while (is_space(at(t, 0)) && is_space(at(t, 1)))
				t->pos++;
			if (at(t, 0) == '"' || at(t, 0) == '\'' ||
			    (is_space(at(t, 0)) &&
			     (at(t, 1) == '"' || at(t, 1) == '\''))) {
				v->type = CT_FUNCTION;
				return;
			}
			t->pos = p;
			free(v->str);
			v->str = NULL;
			v->type = CT_URL;
			consume_url(t, v);
			return;
		}
		v->type = CT_FUNCTION;
		return;
	}
	t->pos++;
	switch (c) {
	case '#':
		if (is_name(at(t, 0)) || valid_escape(at(t, 0), at(t, 1))) {
			v->type = CT_HASH;
			v->id = (unsigned char)starts_ident(at(t, 0), at(t, 1),
				at(t, 2));
			v->str = take_name(t);
			return;
		}
		break;
	case '(': v->type = CT_LPAREN; return;
	case ')': v->type = CT_RPAREN; return;
	case '[': v->type = CT_LBRACKET; return;
	case ']': v->type = CT_RBRACKET; return;
	case '{': v->type = CT_LBRACE; return;
	case '}': v->type = CT_RBRACE; return;
	case ',': v->type = CT_COMMA; return;
	case ':': v->type = CT_COLON; return;
	case ';': v->type = CT_SEMI; return;
	case '<':
		if (at(t, 0) == '!' && at(t, 1) == '-' && at(t, 2) == '-') {
			t->pos += 3;
			v->type = CT_CDO;
			return;
		}
		break;
	case '@':
		if (starts_ident(at(t, 0), at(t, 1), at(t, 2))) {
			v->type = CT_AT;
			v->str = take_name(t);
			return;
		}
		break;
	}
	v->type = CT_DELIM;
	if (c >= 0x80) {
		t->pos--;
		utf8_get(t);
		c = '?';	/* non-ASCII delims cannot occur: name chars */
	}
	v->delim = (unsigned char)c;
}

/* Preprocess: CR LF, CR, FF to LF; NUL to U+FFFD. */
static char *preprocess(const char *s, size_t len, size_t *out_len)
{
	struct buf b = { 0 };
	size_t i;

	buf_grow(&b, len);
	for (i = 0; i < len; i++) {
		if (s[i] == '\r') {
			buf_putc(&b, '\n');
			if (i + 1 < len && s[i + 1] == '\n')
				i++;
		} else if (s[i] == '\f') {
			buf_putc(&b, '\n');
		} else if (s[i] == '\0') {
			buf_add(&b, "\xef\xbf\xbd", 3);
		} else {
			buf_putc(&b, s[i]);
		}
	}
	buf_add(&b, "", 0);
	*out_len = b.len;
	return b.s;
}

/* Token stream, fully tokenized up front. */
struct tstream {
	struct cv *t;
	int n, i, depth;
};

/*
 * Blocks and functions nest this deep at most: the parsers of rules,
 * selectors and values walk the tree recursively, a hostile sheet of
 * nested brackets would run them out of stack. Deeper contents are
 * dropped (real sheets stay under ten levels).
 */
#define CV_DEPTH 64

static void tokenize_all(const char *s, size_t len, struct tstream *ts)
{
	struct ctok t;
	size_t plen;
	char *p = preprocess(s, len, &plen);
	int cap = 0;

	t.s = (const unsigned char *)p;
	t.pos = 0;
	t.len = plen;
	ts->t = NULL;
	ts->n = ts->i = ts->depth = 0;
	for (;;) {
		if (ts->n == cap) {
			cap = cap ? cap * 2 : 64;
			ts->t = xrealloc(ts->t, (size_t)cap * sizeof(*ts->t));
		}
		consume_token(&t, &ts->t[ts->n]);
		if (ts->t[ts->n++].type == CT_EOF)
			break;
	}
	free(p);
}

/* Component values */

void cv_free(struct cv *v)
{
	free(v->str);
	free(v->repr);
	cvlist_free(&v->kids);
}

void cvlist_free(struct cvlist *l)
{
	int i;

	for (i = 0; i < l->n; i++)
		cv_free(&l->v[i]);
	free(l->v);
	l->v = NULL;
	l->n = l->cap = 0;
}

void cvlist_add(struct cvlist *l, const struct cv *v)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 4;
		l->v = xrealloc(l->v, (size_t)l->cap * sizeof(*l->v));
	}
	l->v[l->n++] = *v;
}

static const struct cv *ts_peek(struct tstream *ts)
{
	return &ts->t[ts->i];
}

/* Take the current token out of the stream (ownership moves). */
static struct cv ts_take(struct tstream *ts)
{
	struct cv v = ts->t[ts->i];

	if (v.type != CT_EOF) {
		memset(&ts->t[ts->i], 0, sizeof(v));
		ts->t[ts->i].type = CT_WS;	/* consumed placeholder */
		ts->i++;
	}
	return v;
}

static void consume_cv(struct tstream *ts, struct cv *out);

static int closing_of(int type)
{
	return type == CT_LBRACE ? CT_RBRACE : type == CT_LBRACKET ?
		CT_RBRACKET : CT_RPAREN;
}

/* Contents of a block or function up to the closing token. */
static void consume_until(struct tstream *ts, int close, struct cvlist *kids)
{
	struct cv v;

	for (;;) {
		int type = ts_peek(ts)->type;

		if (type == CT_EOF)
			return;
		if (type == close) {
			ts_take(ts);
			return;
		}
		consume_cv(ts, &v);
		cvlist_add(kids, &v);
	}
}

/* Too deep: skip what is inside, up to the closing token */
static void skip_until(struct tstream *ts)
{
	int level = 1;

	while (ts_peek(ts)->type != CT_EOF) {
		struct cv v = ts_take(ts);

		if (v.type == CT_LBRACE || v.type == CT_LBRACKET ||
		    v.type == CT_LPAREN || v.type == CT_FUNCTION)
			level++;
		else if ((v.type == CT_RBRACE || v.type == CT_RBRACKET ||
		    v.type == CT_RPAREN) && !--level)
			return;
		cv_free(&v);
	}
}

static void consume_kids(struct tstream *ts, int close, struct cvlist *kids)
{
	if (ts->depth >= CV_DEPTH) {
		skip_until(ts);
		return;
	}
	ts->depth++;
	consume_until(ts, close, kids);
	ts->depth--;
}

static void consume_cv(struct tstream *ts, struct cv *out)
{
	*out = ts_take(ts);
	if (out->type == CT_LBRACE || out->type == CT_LBRACKET ||
	    out->type == CT_LPAREN) {
		int close = closing_of(out->type);

		out->delim = out->type == CT_LBRACE ? '{' :
			out->type == CT_LBRACKET ? '[' : '(';
		out->type = CT_BLOCK;
		consume_kids(ts, close, &out->kids);
	} else if (out->type == CT_FUNCTION) {
		consume_kids(ts, CT_RPAREN, &out->kids);
	}
}

static void ts_free(struct tstream *ts)
{
	int i;

	for (i = 0; i < ts->n; i++)
		cv_free(&ts->t[i]);
	free(ts->t);
}

int css_component_values(const char *s, size_t len, struct cvlist *out)
{
	struct tstream ts;
	struct cv v;

	tokenize_all(s, len, &ts);
	memset(out, 0, sizeof(*out));
	while (ts_peek(&ts)->type != CT_EOF) {
		consume_cv(&ts, &v);
		cvlist_add(out, &v);
	}
	ts_free(&ts);
	return 0;
}

/* Cursors */

const struct cv *cur_peek(struct cur *c)
{
	while (c->i < c->n && c->v[c->i].type == CT_WS)
		c->i++;
	return c->i < c->n ? &c->v[c->i] : NULL;
}

const struct cv *cur_next(struct cur *c)
{
	const struct cv *v = cur_peek(c);

	if (v)
		c->i++;
	return v;
}

int cur_end(struct cur *c)
{
	return cur_peek(c) == NULL;
}

int cv_ident(const struct cv *v, const char *s)
{
	return v && v->type == CT_IDENT && ascii_ieq(v->str, s);
}

int cv_func(const struct cv *v, const char *s)
{
	return v && v->type == CT_FUNCTION && ascii_ieq(v->str, s);
}

/* Rules and declarations, over a component value list */

int css_raw_blocks;

struct pstate {
	struct cv *v;
	int i, n;
};

static struct cv *ps_peek(struct pstate *p)
{
	return p->i < p->n ? &p->v[p->i] : NULL;
}

static void crule_add_rule(struct crule *r, struct crule *child)
{
	if (r->nrules == r->caprules) {
		r->caprules = r->caprules ? r->caprules * 2 : 4;
		r->rules = xrealloc(r->rules,
			(size_t)r->caprules * sizeof(*r->rules));
	}
	r->rules[r->nrules++] = child;
}

static void crule_add_decl(struct crule *r, struct decl *d)
{
	if (r->ndecls == r->capdecls) {
		r->capdecls = r->capdecls ? r->capdecls * 2 : 8;
		r->decls = xrealloc(r->decls,
			(size_t)r->capdecls * sizeof(*r->decls));
	}
	r->decls[r->ndecls++] = *d;
}

void crule_free(struct crule *r)
{
	int i;

	free(r->name);
	cvlist_free(&r->prelude);
	cvlist_free(&r->block);
	for (i = 0; i < r->ndecls; i++) {
		free(r->decls[i].name);
		cvlist_free(&r->decls[i].value);
	}
	free(r->decls);
	for (i = 0; i < r->nrules; i++)
		crule_free(r->rules[i]);
	free(r->rules);
	free(r);
}

/* Steal component value i (it becomes whitespace in the source). */
static struct cv ps_take(struct pstate *p)
{
	struct cv v = p->v[p->i];

	memset(&p->v[p->i], 0, sizeof(v));
	p->v[p->i].type = CT_WS;
	p->i++;
	return v;
}

static int is_brace_block(const struct cv *v)
{
	return v && v->type == CT_BLOCK && v->delim == '{';
}

static struct crule *consume_at_rule(struct pstate *p, int nested)
{
	struct crule *r = xmalloc(sizeof(*r));
	struct cv *v;

	memset(r, 0, sizeof(*r));
	r->type = R_AT;
	v = ps_peek(p);
	r->name = v->str;
	v->str = NULL;
	p->i++;
	while ((v = ps_peek(p))) {
		if (v->type == CT_SEMI) {
			p->i++;
			break;
		}
		if (nested && v->type == CT_RBRACE)
			break;
		if (is_brace_block(v)) {
			struct cv b = ps_take(p);

			r->has_block = 1;
			r->block = b.kids;
			free(b.str);
			free(b.repr);
			break;
		}
		{
			struct cv x = ps_take(p);

			cvlist_add(&r->prelude, &x);
		}
	}
	return r;
}

static void bad_decl_remnants(struct pstate *p, int nested)
{
	struct cv *v;

	while ((v = ps_peek(p))) {
		if (v->type == CT_SEMI) {
			p->i++;
			return;
		}
		if (nested && v->type == CT_RBRACE)
			return;
		p->i++;
	}
}

static void trim_ws(struct cvlist *l)
{
	int start = 0, i;

	while (l->n && l->v[l->n - 1].type == CT_WS)
		cv_free(&l->v[--l->n]);
	while (start < l->n && l->v[start].type == CT_WS)
		start++;
	if (start) {
		for (i = 0; i < start; i++)
			cv_free(&l->v[i]);
		memmove(l->v, l->v + start, (size_t)(l->n - start) * sizeof(*l->v));
		l->n -= start;
	}
}

/*
 * Returns 1 and fills d. On failure returns 0 and leaves the input
 * untouched, so that the caller can read it again as a nested rule.
 */
static int consume_declaration(struct pstate *p, struct decl *d, int nested)
{
	int ni = p->i, i = p->i + 1, vs, e, last, prev, j;
	int has_block = 0, others = 0;

	memset(d, 0, sizeof(*d));
	if (ni >= p->n || p->v[ni].type != CT_IDENT)
		return 0;
	while (i < p->n && p->v[i].type == CT_WS)
		i++;
	if (i >= p->n || p->v[i].type != CT_COLON)
		return 0;
	vs = ++i;
	for (e = vs; e < p->n && p->v[e].type != CT_SEMI &&
	     !(nested && p->v[e].type == CT_RBRACE); e++)
		;
	/* !important */
	for (last = e - 1; last >= vs && p->v[last].type == CT_WS; last--)
		;
	for (prev = last - 1; prev >= vs && p->v[prev].type == CT_WS; prev--)
		;
	if (prev >= vs && p->v[last].type == CT_IDENT &&
	    ascii_ieq(p->v[last].str, "important") &&
	    p->v[prev].type == CT_DELIM && p->v[prev].delim == '!') {
		d->important = 1;
		last = prev - 1;
	}
	if (strncmp(p->v[ni].str, "--", 2)) {
		for (j = vs; j <= last; j++) {
			if (is_brace_block(&p->v[j]))
				has_block = 1;
			else if (p->v[j].type != CT_WS)
				others = 1;
		}
		if (has_block && others)
			return 0;
	}
	d->name = p->v[ni].str;
	p->v[ni].str = NULL;
	for (p->i = vs; p->i <= last; ) {
		struct cv x = ps_take(p);

		cvlist_add(&d->value, &x);
	}
	p->i = e < p->n && p->v[e].type == CT_SEMI ? e + 1 : e;
	trim_ws(&d->value);
	return 1;
}

static void consume_block_contents(struct pstate *p, struct crule *into);

static struct crule *consume_qualified(struct pstate *p, int nested,
	int stop_semi)
{
	struct crule *r = xmalloc(sizeof(*r));
	struct cv *v;
	int j, k;

	memset(r, 0, sizeof(*r));
	r->type = R_QUALIFIED;
	while ((v = ps_peek(p))) {
		if (stop_semi && v->type == CT_SEMI) {
			crule_free(r);
			return NULL;
		}
		if (nested && v->type == CT_RBRACE) {
			crule_free(r);
			return NULL;
		}
		if (is_brace_block(v)) {
			struct cv b = ps_take(p);
			struct pstate bp;

			/* "--foo:" preludes are bad declarations */
			for (j = 0; j < r->prelude.n &&
			     r->prelude.v[j].type == CT_WS; j++)
				;
			for (k = j + 1; k < r->prelude.n &&
			     r->prelude.v[k].type == CT_WS; k++)
				;
			if (j < r->prelude.n &&
			    r->prelude.v[j].type == CT_IDENT &&
			    !strncmp(r->prelude.v[j].str, "--", 2) &&
			    k < r->prelude.n &&
			    r->prelude.v[k].type == CT_COLON) {
				cv_free(&b);
				crule_free(r);
				bad_decl_remnants(p, nested);
				return NULL;
			}
			r->has_block = 1;
			if (css_raw_blocks) {
				r->block = b.kids;
				free(b.str);
				free(b.repr);
				return r;
			}
			bp.v = b.kids.v;
			bp.n = b.kids.n;
			bp.i = 0;
			consume_block_contents(&bp, r);
			cv_free(&b);
			return r;
		}
		{
			struct cv x = ps_take(p);

			cvlist_add(&r->prelude, &x);
		}
	}
	crule_free(r);
	return NULL;
}

/*
 * Declarations after a nested rule go into a "nested declarations"
 * rule, so that they keep their place in the cascade order.
 */
static void add_nested_decl(struct crule *into, struct decl *d)
{
	struct crule *last;

	if (!into->nrules) {
		crule_add_decl(into, d);
		return;
	}
	last = into->rules[into->nrules - 1];
	if (!last->nested_decls) {
		last = xmalloc(sizeof(*last));
		memset(last, 0, sizeof(*last));
		last->type = R_QUALIFIED;
		last->nested_decls = 1;
		last->has_block = 1;
		crule_add_rule(into, last);
	}
	crule_add_decl(last, d);
}

static void consume_block_contents(struct pstate *p, struct crule *into)
{
	struct cv *v;

	while ((v = ps_peek(p))) {
		/* a stray '}' (style="a}b") is junk: blocks are closed */
		if (v->type == CT_WS || v->type == CT_SEMI ||
		    v->type == CT_RBRACE) {
			p->i++;
		} else if (v->type == CT_AT) {
			crule_add_rule(into, consume_at_rule(p, 1));
		} else {
			int mark = p->i;
			struct decl d;
			struct crule *r;

			if (consume_declaration(p, &d, 1)) {
				add_nested_decl(into, &d);
				continue;
			}
			r = consume_qualified(p, 1, 1);
			if (r)
				crule_add_rule(into, r);
			else if (p->i == mark)
				bad_decl_remnants(p, 1);
		}
	}
}

void css_parse_block(struct cvlist *block, struct crule *into, int nested)
{
	struct pstate p;

	(void)nested;
	p.v = block->v;
	p.n = block->n;
	p.i = 0;
	consume_block_contents(&p, into);
}

struct crule **css_parse_stylesheet(const char *s, size_t len, int *n)
{
	struct cvlist l;
	struct pstate p;
	struct crule tmp, *r;
	struct cv *v;

	css_component_values(s, len, &l);
	memset(&tmp, 0, sizeof(tmp));
	p.v = l.v;
	p.n = l.n;
	p.i = 0;
	while ((v = ps_peek(&p))) {
		if (v->type == CT_WS || v->type == CT_CDO || v->type == CT_CDC) {
			p.i++;
		} else if (v->type == CT_AT) {
			crule_add_rule(&tmp, consume_at_rule(&p, 0));
		} else {
			r = consume_qualified(&p, 0, 0);
			if (r)
				crule_add_rule(&tmp, r);
		}
	}
	cvlist_free(&l);
	*n = tmp.nrules;
	return tmp.rules;
}

/*
 * Entry points of the css-parsing-tests functions: 'S' stylesheet,
 * 'L' rule list, 'B' block contents, 'R' one rule, 'D' one declaration.
 * Returns a container rule, or NULL with *err set.
 */
struct crule *css_parse_test(const char *s, size_t len, int mode,
	const char **err)
{
	struct cvlist l;
	struct pstate p;
	struct crule *c = xmalloc(sizeof(*c)), *r;
	struct cv *v;
	struct decl d;

	memset(c, 0, sizeof(*c));
	*err = NULL;
	css_component_values(s, len, &l);
	p.v = l.v;
	p.n = l.n;
	p.i = 0;
	if (mode == 'B') {
		consume_block_contents(&p, c);
	} else if (mode == 'S' || mode == 'L') {
		while ((v = ps_peek(&p))) {
			if (v->type == CT_WS || (mode == 'S' &&
			    (v->type == CT_CDO || v->type == CT_CDC))) {
				p.i++;
			} else if (v->type == CT_AT) {
				crule_add_rule(c, consume_at_rule(&p, 0));
			} else if ((r = consume_qualified(&p, 0, 0))) {
				crule_add_rule(c, r);
			}
		}
	} else {
		while ((v = ps_peek(&p)) && v->type == CT_WS)
			p.i++;
		if (!v) {
			*err = "empty";
		} else if (mode == 'D') {
			if (consume_declaration(&p, &d, 0))
				crule_add_decl(c, &d);
			else
				*err = "invalid";
		} else if (v->type == CT_AT) {
			crule_add_rule(c, consume_at_rule(&p, 0));
		} else if ((r = consume_qualified(&p, 0, 0))) {
			crule_add_rule(c, r);
		} else {
			*err = "invalid";
		}
		while (!*err && (v = ps_peek(&p)) && v->type == CT_WS)
			p.i++;
		if (!*err && mode == 'R' && ps_peek(&p))
			*err = "extra-input";
	}
	cvlist_free(&l);
	if (*err) {
		crule_free(c);
		return NULL;
	}
	return c;
}

/* Serialization, close enough to round-trip through the tokenizer. */

static void ser_ident(struct buf *b, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	char hex[8];

	if (*p == '-' && !p[1]) {
		buf_add(b, "\\-", 2);
		return;
	}
	for (; *p; p++) {
		int first = p == (const unsigned char *)s;

		if (is_name(*p) && !(is_digit(*p) && (first ||
		    (p == (const unsigned char *)s + 1 && s[0] == '-')))) {
			buf_putc(b, *p);
		} else if (*p < 0x20 || *p == 0x7f || is_digit(*p)) {
			int n = snprintf(hex, sizeof(hex), "\\%x ", *p);

			buf_add(b, hex, (size_t)n);
		} else {
			buf_putc(b, '\\');
			buf_putc(b, *p);
		}
	}
}

static void ser_string(struct buf *b, const char *s)
{
	char hex[8];

	buf_putc(b, '"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') {
			buf_putc(b, '\\');
			buf_putc(b, *s);
		} else if ((unsigned char)*s < 0x20 || *s == 0x7f) {
			int n = snprintf(hex, sizeof(hex), "\\%x ",
				(unsigned char)*s);

			buf_add(b, hex, (size_t)n);
		} else {
			buf_putc(b, *s);
		}
	}
	buf_putc(b, '"');
}

void css_serialize(struct buf *b, const struct cv *v, int n)
{
	int i;

	buf_add(b, "", 0);
	for (i = 0; i < n; i++, v++) {
		switch (v->type) {
		case CT_IDENT:
			ser_ident(b, v->str);
			break;
		case CT_FUNCTION:
			ser_ident(b, v->str);
			buf_putc(b, '(');
			css_serialize(b, v->kids.v, v->kids.n);
			buf_putc(b, ')');
			break;
		case CT_AT:
			buf_putc(b, '@');
			ser_ident(b, v->str);
			break;
		case CT_HASH:
			buf_putc(b, '#');
			buf_add(b, v->str, strlen(v->str));
			break;
		case CT_STRING:
			ser_string(b, v->str);
			break;
		case CT_URL:
			buf_add(b, "url(", 4);
			ser_string(b, v->str);
			buf_putc(b, ')');
			break;
		case CT_DELIM:
			buf_putc(b, v->delim);
			break;
		case CT_NUMBER:
			buf_add(b, v->repr, strlen(v->repr));
			break;
		case CT_PERCENT:
			buf_add(b, v->repr, strlen(v->repr));
			buf_putc(b, '%');
			break;
		case CT_DIMENSION:
			buf_add(b, v->repr, strlen(v->repr));
			ser_ident(b, v->str);
			break;
		case CT_WS:
			buf_putc(b, ' ');
			break;
		case CT_CDO: buf_add(b, "<!--", 4); break;
		case CT_CDC: buf_add(b, "-->", 3); break;
		case CT_COLON: buf_putc(b, ':'); break;
		case CT_SEMI: buf_putc(b, ';'); break;
		case CT_COMMA: buf_putc(b, ','); break;
		case CT_RBRACKET: buf_putc(b, ']'); break;
		case CT_RPAREN: buf_putc(b, ')'); break;
		case CT_RBRACE: buf_putc(b, '}'); break;
		case CT_BLOCK:
			buf_putc(b, v->delim);
			css_serialize(b, v->kids.v, v->kids.n);
			buf_putc(b, v->delim == '{' ? '}' :
				v->delim == '[' ? ']' : ')');
			break;
		}
	}
}
