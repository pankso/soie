/*
 * Selectors Level 4 subset: parsing, specificity and matching.
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

#if defined(__GNUC__)
#define THREAD_LOCAL __thread	/* workers match selectors too */
#else
#define THREAD_LOCAL
#endif

static const struct {
	const char *name;
	int id;
} pseudo_classes[] = {
	{ "active", PC_FALSE }, { "any-link", PC_LINK },
	{ "autofill", PC_FALSE }, { "checked", PC_CHECKED },
	{ "default", PC_FALSE }, { "defined", PC_DEFINED },
	{ "disabled", PC_DISABLED }, { "empty", PC_EMPTY },
	{ "enabled", PC_ENABLED }, { "first-child", PC_FIRST_CHILD },
	{ "first-of-type", PC_FIRST_OF_TYPE }, { "focus", PC_FALSE },
	{ "focus-visible", PC_FALSE }, { "focus-within", PC_FALSE },
	{ "fullscreen", PC_FALSE }, { "host", PC_HOST },
	{ "hover", PC_FALSE },
	{ "in-range", PC_VALID }, { "indeterminate", PC_FALSE },
	{ "invalid", PC_FALSE }, { "last-child", PC_LAST_CHILD },
	{ "last-of-type", PC_LAST_OF_TYPE }, { "link", PC_LINK },
	{ "modal", PC_FALSE }, { "only-child", PC_ONLY_CHILD },
	{ "only-of-type", PC_ONLY_OF_TYPE }, { "open", PC_OPEN },
	{ "optional", PC_OPTIONAL }, { "out-of-range", PC_FALSE },
	{ "paused", PC_FALSE }, { "placeholder-shown", PC_PLACEHOLDER_SHOWN },
	{ "playing", PC_FALSE }, { "popover-open", PC_FALSE },
	{ "read-only", PC_READ_ONLY }, { "read-write", PC_READ_WRITE },
	{ "required", PC_REQUIRED }, { "root", PC_ROOT },
	{ "scope", PC_ROOT }, { "target", PC_FALSE },
	{ "target-within", PC_FALSE }, { "user-invalid", PC_FALSE },
	{ "user-valid", PC_FALSE }, { "valid", PC_VALID },
	{ "visited", PC_VISITED },
	{ NULL, 0 }
};

static const struct {
	const char *name;
	int id;
} pseudo_elements[] = {
	{ "after", PE_AFTER }, { "backdrop", PE_OTHER },
	{ "before", PE_BEFORE }, { "checkmark", PE_OTHER },
	{ "cue", PE_OTHER }, { "details-content", PE_OTHER },
	{ "file-selector-button", PE_OTHER },
	{ "first-letter", PE_OTHER }, { "first-line", PE_OTHER },
	{ "grammar-error", PE_OTHER }, { "marker", PE_MARKER },
	{ "picker-icon", PE_OTHER }, { "placeholder", PE_OTHER },
	{ "scroll-marker", PE_OTHER }, { "scroll-marker-group", PE_OTHER },
	{ "selection", PE_OTHER }, { "spelling-error", PE_OTHER },
	{ "target-text", PE_OTHER }, { "view-transition", PE_OTHER },
	{ NULL, 0 }
};

enum { OP_EXISTS, OP_EQ, OP_INCLUDES, OP_DASH, OP_PREFIX, OP_SUFFIX,
	OP_SUBSTRING };

/* Parsing */

struct sp {
	const struct cv *v;
	int i, n;
	const struct sellist *parent;
	int nest;		/* top level of a nested rule: implicit & */
	int bad;
};

static const struct cv *sp_peek(struct sp *p)
{
	return p->i < p->n ? &p->v[p->i] : NULL;
}

static int is_delim(const struct cv *v, int c)
{
	return v && v->type == CT_DELIM && v->delim == c;
}

static void simple_free(struct simple *s)
{
	free(s->name);
	free(s->value);
	if (s->args)
		sellist_free(s->args);
}

static void complex_free(struct complex *c)
{
	int i, j;

	for (i = 0; i < c->n; i++) {
		for (j = 0; j < c->c[i].n; j++)
			simple_free(&c->c[i].s[j]);
		free(c->c[i].s);
	}
	free(c->c);
}

void sellist_free(struct sellist *s)
{
	int i;

	for (i = 0; i < s->n; i++)
		complex_free(&s->c[i]);
	free(s->c);
	free(s);
}

static void add_simple(struct compound *c, const struct simple *s)
{
	c->s = xrealloc(c->s, (size_t)(c->n + 1) * sizeof(*c->s));
	c->s[c->n++] = *s;
}

static struct sellist *parse_list(const struct cvlist *l, int from, int to,
	const struct sellist *parent, int forgiving, int relative, int nest);
static int selector_match_relative(const struct complex *c,
	const soie_node *el, const struct sellist *parent);

static int parse_attr(const struct cv *block, struct simple *s)
{
	struct cur c;
	const struct cv *v;

	cur_init(&c, &block->kids);
	v = cur_next(&c);
	if (is_delim(v, '|') || (v && v->type == CT_IDENT &&
	    c.i < c.n && is_delim(&c.v[c.i], '|') &&
	    !(c.i + 1 < c.n && is_delim(&c.v[c.i + 1], '='))))
		v = is_delim(v, '|') ? cur_next(&c) : (c.i++, cur_next(&c));
	else if (is_delim(v, '*') && is_delim(cur_peek(&c), '|'))
		cur_next(&c), v = cur_next(&c);
	if (!v || v->type != CT_IDENT)
		return 0;
	s->type = SEL_ATTR;
	s->name = lowdup(v->str);
	v = cur_next(&c);
	if (!v) {
		s->op = OP_EXISTS;
		return 1;
	}
	if (is_delim(v, '=')) {
		s->op = OP_EQ;
	} else if (v->type == CT_DELIM && c.i < c.n &&
		   is_delim(&c.v[c.i], '=')) {
		switch (v->delim) {
		case '~': s->op = OP_INCLUDES; break;
		case '|': s->op = OP_DASH; break;
		case '^': s->op = OP_PREFIX; break;
		case '$': s->op = OP_SUFFIX; break;
		case '*': s->op = OP_SUBSTRING; break;
		default: return 0;
		}
		c.i++;
	} else {
		return 0;
	}
	v = cur_next(&c);
	if (!v || (v->type != CT_IDENT && v->type != CT_STRING))
		return 0;
	s->value = xstrndup(v->str, strlen(v->str));
	v = cur_next(&c);
	if (v) {
		if (cv_ident(v, "i"))
			s->ci = 1;
		else if (!cv_ident(v, "s"))
			return 0;
		if (cur_next(&c))
			return 0;
	}
	return 1;
}

/* What :scope is while a query runs (query.c), else the root */
static THREAD_LOCAL const soie_node *sel_scope;

void selector_scope(const soie_node *el)
{
	sel_scope = el;
}

/* The shadow root of the rules the cascade matches, NULL for the
 * document's: :host is its host, the parent of its tree's top
 * elements for the combinators */
static THREAD_LOCAL const soie_node *sel_shadow;

void selector_shadow(const soie_node *root)
{
	sel_shadow = root;
}

static int find_pseudo_class(const char *name)
{
	int i;

	for (i = 0; pseudo_classes[i].name; i++)
		if (ascii_ieq(name, pseudo_classes[i].name))
			return pseudo_classes[i].id;
	return PC_NONE;
}

static int find_pseudo_element(const char *name)
{
	int i;

	for (i = 0; pseudo_elements[i].name; i++)
		if (ascii_ieq(name, pseudo_elements[i].name))
			return pseudo_elements[i].id;
	return PE_NONE;
}

static int parse_pseudo_fn(struct sp *p, const struct cv *f,
	struct simple *s)
{
	const char *n = f->str;
	struct cur c;

	s->type = SEL_PSEUDO;
	if (ascii_ieq(n, "not") || ascii_ieq(n, "is") ||
	    ascii_ieq(n, "where") || ascii_ieq(n, "matches") ||
	    ascii_ieq(n, "-webkit-any") || ascii_ieq(n, "has")) {
		int has = ascii_ieq(n, "has"), not = ascii_ieq(n, "not");

		s->pseudo = (unsigned char)(not ? PC_NOT : has ? PC_HAS :
			ascii_ieq(n, "where") ? PC_WHERE : PC_IS);
		s->args = parse_list(&f->kids, 0, f->kids.n, p->parent,
			!not && !has, has, 0);
		return s->args != NULL;
	}
	if (ascii_ieq(n, "nth-child") || ascii_ieq(n, "nth-last-child") ||
	    ascii_ieq(n, "nth-of-type") || ascii_ieq(n, "nth-last-of-type")) {
		s->pseudo = (unsigned char)(ascii_ieq(n, "nth-child") ?
			PC_NTH_CHILD : ascii_ieq(n, "nth-last-child") ?
			PC_NTH_LAST_CHILD : ascii_ieq(n, "nth-of-type") ?
			PC_NTH_OF_TYPE : PC_NTH_LAST_OF_TYPE);
		cur_init(&c, &f->kids);
		if (!parse_anb(&c, &s->a, &s->b))
			return 0;
		if (cur_end(&c))
			return 1;
		if (s->pseudo > PC_NTH_LAST_CHILD || !cv_ident(cur_next(&c),
							     "of"))
			return 0;
		cur_peek(&c);
		s->args = parse_list(&f->kids, c.i, f->kids.n, p->parent, 0, 0,
			0);
		return s->args != NULL;
	}
	if (ascii_ieq(n, "host") || ascii_ieq(n, "host-context")) {
		/* a compound selector, of the host or of its ancestors */
		s->pseudo = (unsigned char)(ascii_ieq(n, "host") ? PC_HOST :
			PC_HOST_CONTEXT);
		s->args = parse_list(&f->kids, 0, f->kids.n, NULL, 0, 0, 0);
		return s->args && s->args->n == 1 && s->args->c[0].n == 1;
	}
	if (ascii_ieq(n, "state") ||
	    ascii_ieq(n, "active-view-transition-type")) {
		/* custom states: valid, never matching */
		s->pseudo = PC_FALSE;
		return 1;
	}
	if (ascii_ieq(n, "lang") || ascii_ieq(n, "dir")) {
		const struct cv *v;

		s->pseudo = (unsigned char)(ascii_ieq(n, "lang") ? PC_LANG :
			PC_DIR);
		cur_init(&c, &f->kids);
		v = cur_next(&c);
		if (!v || (v->type != CT_IDENT && v->type != CT_STRING))
			return 0;
		s->value = lowdup(v->str);
		return 1;	/* only the first lang() range is used */
	}
	return 0;
}

/* One compound selector; returns 0 if there is none here. */
static int parse_compound(struct sp *p, struct compound *c, int *pe)
{
	const struct cv *v;
	struct simple s;
	int any = 0;

	memset(c, 0, sizeof(*c));
	*pe = PE_NONE;
	while ((v = sp_peek(p))) {
		memset(&s, 0, sizeof(s));
		if (*pe != PE_NONE && !(v->type == CT_COLON))
			break;
		if (v->type == CT_IDENT && !any) {
			p->i++;
			if (is_delim(sp_peek(p), '|')) {
				/* ns|type: namespaces are ignored */
				p->i++;
				v = sp_peek(p);
				if (!v || (v->type != CT_IDENT &&
					   !is_delim(v, '*')))
					goto bad;
				p->i++;
				if (is_delim(v, '*')) {
					s.type = SEL_UNIVERSAL;
					add_simple(c, &s);
					any = 1;
					continue;
				}
			}
			s.type = SEL_TYPE;
			s.name = lowdup(v->str);
			s.value = xstrndup(v->str, strlen(v->str));
		} else if (is_delim(v, '*') && !any) {
			p->i++;
			if (is_delim(sp_peek(p), '|')) {
				p->i++;
				v = sp_peek(p);
				if (v && v->type == CT_IDENT) {
					p->i++;
					s.type = SEL_TYPE;
					s.name = lowdup(v->str);
					s.value = xstrndup(v->str,
						strlen(v->str));
					add_simple(c, &s);
					any = 1;
					continue;
				}
				if (!is_delim(v, '*'))
					goto bad;
				p->i++;
			}
			s.type = SEL_UNIVERSAL;
		} else if (is_delim(v, '&')) {
			p->i++;
			if (!p->parent)
				goto bad;
			s.type = SEL_NEST;
		} else if (v->type == CT_HASH && v->id) {
			p->i++;
			s.type = SEL_ID;
			s.value = xstrndup(v->str, strlen(v->str));
		} else if (is_delim(v, '.')) {
			p->i++;
			v = sp_peek(p);
			if (!v || v->type != CT_IDENT)
				goto bad;
			p->i++;
			s.type = SEL_CLASS;
			s.value = xstrndup(v->str, strlen(v->str));
		} else if (v->type == CT_BLOCK && v->delim == '[') {
			p->i++;
			if (!parse_attr(v, &s)) {
				simple_free(&s);
				goto bad;
			}
		} else if (v->type == CT_COLON) {
			int element = 0;

			p->i++;
			v = sp_peek(p);
			if (v && v->type == CT_COLON) {
				element = 1;
				p->i++;
				v = sp_peek(p);
			}
			if (!v)
				goto bad;
			p->i++;
			if (v->type == CT_IDENT) {
				int id = find_pseudo_element(v->str);

				if (element || (id != PE_NONE &&
				    (id == PE_BEFORE || id == PE_AFTER ||
				     ascii_ieq(v->str, "first-line") ||
				     ascii_ieq(v->str, "first-letter")))) {
					if (id == PE_NONE || *pe != PE_NONE)
						goto bad;
					s.type = SEL_PSEUDO_ELEMENT;
					s.pseudo = (unsigned char)id;
					*pe = id;
				} else {
					id = find_pseudo_class(v->str);
					if (id == PC_NONE)
						goto bad;
					s.type = SEL_PSEUDO;
					s.pseudo = (unsigned char)id;
					s.op = ascii_ieq(v->str, "scope");
				}
			} else if (v->type == CT_FUNCTION && element) {
				/* ::slotted(), ::part(), ::highlight()... */
				if (*pe != PE_NONE || !(ascii_ieq(v->str,
				    "slotted") || ascii_ieq(v->str, "part") ||
				    ascii_ieq(v->str, "highlight") ||
				    ascii_ieq(v->str, "picker") ||
				    !strncmp(v->str, "view-transition", 15)))
					goto bad;
				s.type = SEL_PSEUDO_ELEMENT;
				s.pseudo = PE_OTHER;
				if (ascii_ieq(v->str, "slotted")) {
					/* a compound selector of the
					 * slotted element */
					s.pseudo = PE_SLOTTED;
					s.args = parse_list(&v->kids, 0,
						v->kids.n, NULL, 0, 0, 0);
					if (!s.args || s.args->n != 1 ||
					    s.args->c[0].n != 1) {
						simple_free(&s);
						goto bad;
					}
				}
				*pe = s.pseudo;
			} else if (v->type == CT_FUNCTION && !element) {
				if (*pe != PE_NONE ||
				    !parse_pseudo_fn(p, v, &s)) {
					simple_free(&s);
					goto bad;
				}
			} else {
				goto bad;
			}
		} else {
			break;
		}
		add_simple(c, &s);
		any = 1;
	}
	return any;
bad:
	p->bad = 1;
	return any;
}

/* Specificity, given the element only for & and :is() maxima. */
#define SPEC_A (1u << 20)
#define SPEC_B (1u << 10)
#define SPEC_C 1u

static unsigned list_max_spec(const struct sellist *l)
{
	unsigned m = 0;
	int i;

	for (i = 0; l && i < l->n; i++)
		if (l->c[i].spec > m)
			m = l->c[i].spec;
	return m;
}

static unsigned compound_spec(const struct compound *c,
	const struct sellist *parent)
{
	unsigned spec = 0;
	int i;

	for (i = 0; i < c->n; i++) {
		const struct simple *s = &c->s[i];

		switch (s->type) {
		case SEL_ID:
			spec += SPEC_A;
			break;
		case SEL_CLASS:
		case SEL_ATTR:
			spec += SPEC_B;
			break;
		case SEL_TYPE:
		case SEL_PSEUDO_ELEMENT:
			spec += SPEC_C + list_max_spec(s->args);
			break;
		case SEL_NEST:
			spec += list_max_spec(parent);
			break;
		case SEL_PSEUDO:
			if (s->pseudo == PC_WHERE)
				break;
			if (s->pseudo == PC_IS || s->pseudo == PC_NOT ||
			    s->pseudo == PC_HAS)
				spec += list_max_spec(s->args);
			else if (s->pseudo == PC_HOST ||
				 s->pseudo == PC_HOST_CONTEXT)
				spec += SPEC_B + list_max_spec(s->args);
			else if (s->pseudo == PC_NTH_CHILD ||
				 s->pseudo == PC_NTH_LAST_CHILD)
				spec += SPEC_B + list_max_spec(s->args);
			else
				spec += SPEC_B;
			break;
		}
	}
	return spec;
}

static int has_nest(const struct complex *c)
{
	int i, j;

	for (i = 0; i < c->n; i++)
		for (j = 0; j < c->c[i].n; j++)
			if (c->c[i].s[j].type == SEL_NEST)
				return 1;
	return 0;
}

static int combinator(struct sp *p, int *comb)
{
	const struct cv *v;
	int ws = 0;

	*comb = COMB_NONE;
	while ((v = sp_peek(p)) && v->type == CT_WS) {
		ws = 1;
		p->i++;
	}
	if (is_delim(v, '>'))
		*comb = COMB_CHILD;
	else if (is_delim(v, '+'))
		*comb = COMB_NEXT;
	else if (is_delim(v, '~'))
		*comb = COMB_SUBSEQ;
	if (*comb != COMB_NONE) {
		p->i++;
		while ((v = sp_peek(p)) && v->type == CT_WS)
			p->i++;
		return 1;
	}
	if (ws && v && v->type != CT_COMMA) {
		*comb = COMB_DESC;
		return 1;
	}
	return 0;
}

static int parse_complex(struct sp *p, struct complex *x, int relative)
{
	struct compound c;
	int comb = COMB_NONE, pe, lead;
	const struct cv *v;

	memset(x, 0, sizeof(*x));
	while ((v = sp_peek(p)) && v->type == CT_WS)
		p->i++;
	/* A leading combinator: relative (:has, nesting) selectors. */
	lead = combinator(p, &comb) && comb != COMB_DESC;
	if (lead && !relative && !p->nest)
		return 0;
	if (!lead)
		comb = relative ? COMB_DESC : COMB_NONE;
	x->relative = relative;
	for (;;) {
		if (!parse_compound(p, &c, &pe) || p->bad) {
			int i;

			for (i = 0; i < c.n; i++)
				simple_free(&c.s[i]);
			free(c.s);
			return 0;
		}
		c.comb = comb;
		x->c = xrealloc(x->c, (size_t)(x->n + 1) * sizeof(*x->c));
		x->c[x->n++] = c;
		if (pe != PE_NONE)
			x->pseudo_element = pe;
		if (!combinator(p, &comb))
			break;
		if (pe != PE_NONE)
			return 0;
	}
	while ((v = sp_peek(p)) && v->type == CT_WS)
		p->i++;
	/* Nested rule without &: it means "& " (or "& >" etc.). */
	if (p->nest && (lead || !has_nest(x))) {
		struct compound nest;
		struct simple s;

		memset(&nest, 0, sizeof(nest));
		memset(&s, 0, sizeof(s));
		s.type = SEL_NEST;
		add_simple(&nest, &s);
		if (!lead)
			x->c[0].comb = COMB_DESC;
		x->c = xrealloc(x->c, (size_t)(x->n + 1) * sizeof(*x->c));
		memmove(x->c + 1, x->c, (size_t)x->n * sizeof(*x->c));
		x->c[0] = nest;
		x->n++;
	}
	return 1;
}

static struct sellist *parse_list(const struct cvlist *l, int from, int to,
	const struct sellist *parent, int forgiving, int relative, int nest)
{
	struct sellist *s = xmalloc(sizeof(*s));
	struct sp p;
	struct complex x;
	const struct cv *v;
	int i;

	memset(s, 0, sizeof(*s));
	s->parent = parent;
	p.v = l->v;
	p.n = to;
	p.i = from;
	p.parent = parent;
	p.nest = nest && parent;
	for (;;) {
		p.bad = 0;
		if (parse_complex(&p, &x, relative) &&
		    (!(v = sp_peek(&p)) || v->type == CT_COMMA)) {
			for (i = 0; i < x.n; i++)
				x.spec += compound_spec(&x.c[i], parent);
			s->c = xrealloc(s->c, (size_t)(s->n + 1) * sizeof(*s->c));
			s->c[s->n++] = x;
		} else {
			complex_free(&x);
			if (!forgiving) {
				sellist_free(s);
				return NULL;
			}
			/* forgiving lists drop the bad item */
			while ((v = sp_peek(&p)) && v->type != CT_COMMA)
				p.i++;
		}
		if (!(v = sp_peek(&p)))
			break;
		p.i++;	/* comma */
	}
	if (!s->n && !forgiving) {
		sellist_free(s);
		return NULL;
	}
	return s;
}

struct sellist *selector_parse(const struct cvlist *l, int from, int to,
	const struct sellist *parent, int forgiving)
{
	return parse_list(l, from, to, parent, forgiving, 0, 1);
}

/* Dependencies, for incremental restyling */

static int compound_flags(const struct compound *c)
{
	const struct simple *s;
	int f = 0, k, i;

	for (k = 0; k < c->n; k++) {
		s = &c->s[k];
		if (s->type != SEL_PSEUDO)
			continue;
		switch (s->pseudo) {
		case PC_FIRST_CHILD: case PC_FIRST_OF_TYPE:
		case PC_NTH_CHILD: case PC_NTH_OF_TYPE:
			f |= SF_FORWARD;
			break;
		case PC_LAST_CHILD: case PC_LAST_OF_TYPE:
		case PC_NTH_LAST_CHILD: case PC_NTH_LAST_OF_TYPE:
			f |= SF_BACKWARD;
			break;
		case PC_ONLY_CHILD: case PC_ONLY_OF_TYPE:
			f |= SF_FORWARD | SF_BACKWARD;
			break;
		case PC_EMPTY:
			f |= SF_EMPTY;
			break;
		case PC_HAS:
			f |= SF_HAS;
			for (i = 0; s->args && i < s->args->n; i++)
				if (s->args->c[i].n && (s->args->c[i].c[0].comb ==
				    COMB_NEXT || s->args->c[i].c[0].comb ==
				    COMB_SUBSEQ))
					f |= SF_HAS_SIB;
			continue;	/* what is inside :has() is
					 * reached through its ancestors */
		}
		/* :is(), :not(), :where(), :nth-child(of S): as here */
		if (s->args)
			f |= sellist_flags(s->args);
	}
	return f;
}

int sellist_flags(const struct sellist *l)
{
	const struct compound *c;
	int f = 0, g, i, j;

	for (i = 0; l && i < l->n; i++)
		for (j = 0; j < l->c[i].n; j++) {
			c = &l->c[i].c[j];
			g = compound_flags(c);
			if (c->comb == COMB_NEXT || c->comb == COMB_SUBSEQ)
				g |= SF_FORWARD;
			/* the subject is the last compound; further left,
			 * everything is deep */
			if (j == l->c[i].n - 1)
				f |= g;
			else
				f |= (g & 0xff) << SF_DEEP | (g & ~0xff);
		}
	return f;
}

/* Matching */

static soie_node *prev_el(const soie_node *n)
{
	for (n = n->prev; n; n = n->prev)
		if (n->type == SOIE_ELEMENT)
			return (soie_node *)n;
	return NULL;
}

static soie_node *next_el(const soie_node *n)
{
	for (n = n->next; n; n = n->next)
		if (n->type == SOIE_ELEMENT)
			return (soie_node *)n;
	return NULL;
}

static soie_node *parent_el(const soie_node *n)
{
	return n->parent && n->parent->type == SOIE_ELEMENT ? n->parent : NULL;
}

/* Is el the host of the rules' shadow tree? */
static int is_host(const soie_node *el)
{
	return sel_shadow && el->u.el.shadow == sel_shadow;
}

/* Does compound c hold :host, maybe in :is() or :where()? Nothing
 * else of a shadow tree's rules sees its host */
static int has_host(const struct compound *c)
{
	const struct sellist *a;
	int i, j;

	for (i = 0; i < c->n; i++) {
		if (c->s[i].type != SEL_PSEUDO)
			continue;
		if (c->s[i].pseudo == PC_HOST ||
		    c->s[i].pseudo == PC_HOST_CONTEXT)
			return 1;
		if ((c->s[i].pseudo == PC_IS || c->s[i].pseudo == PC_WHERE) &&
		    (a = c->s[i].args))
			for (j = 0; j < a->n; j++)
				if (has_host(&a->c[j].c[a->c[j].n - 1]))
					return 1;
	}
	return 0;
}

static int matches_list(const struct sellist *l, const soie_node *el);

/* Above el, a top element of the rules' shadow tree, its host for
 * compound i of c if it holds :host (`:host > .x`, `:host .x`). Only
 * called at the top of a fragment: match_rec() itself reads no
 * thread-local, it is on every page's hot path */
static const soie_node *host_up(const soie_node *el,
	const struct complex *c, int i)
{
	return el->parent == sel_shadow && has_host(&c->c[i]) ?
		sel_shadow->u.frag.host : NULL;
}

static int top_of_fragment(const soie_node *el)
{
	return el->parent && el->parent->type == SOIE_DOCUMENT_FRAGMENT;
}

int selector_has_host(const struct sellist *l, int i)
{
	return has_host(&l->c[i].c[l->c[i].n - 1]);
}

/* An argument of :host(), :host-context(), ::slotted(): el is matched
 * from outside the shadow tree */
static int matches_outside(const struct sellist *l, const soie_node *el)
{
	const soie_node *saved = sel_shadow;
	int r;

	sel_shadow = NULL;
	r = matches_list(l, el);
	sel_shadow = saved;
	return r;
}

/* ::slotted(x) of selector i of l: does el match x? */
int selector_slotted(const struct sellist *l, int i, const soie_node *el)
{
	const struct compound *c = &l->c[i].c[l->c[i].n - 1];
	int j;

	for (j = 0; j < c->n; j++)
		if (c->s[j].type == SEL_PSEUDO_ELEMENT &&
		    c->s[j].pseudo == PE_SLOTTED)
			return matches_outside(c->s[j].args, el);
	return 0;
}

static const char *attr(const soie_node *el, const char *name)
{
	struct attr *a = element_find_attr(el, name);

	return a ? a->value : NULL;
}

static int lower_c(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int str_eq(const char *a, const char *b, int ci)
{
	return ci ? ascii_ieq(a, b) : !strcmp(a, b);
}

static int str_prefix(const char *s, const char *p, int ci)
{
	for (; *p; s++, p++)
		if (ci ? lower_c((unsigned char)*s) !=
		    lower_c((unsigned char)*p) : *s != *p)
			return 0;
	return 1;
}

static int is_ws(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* Whether the whitespace-separated list has the word w */
int word_in(const char *list, const char *w, int ci)
{
	size_t n = strlen(w);

	if (!n)
		return 0;
	while (*list) {
		const char *e;

		while (is_ws((unsigned char)*list))
			list++;
		for (e = list; *e && !is_ws((unsigned char)*e); e++)
			;
		if ((size_t)(e - list) == n && str_prefix(list, w, ci))
			return 1;
		list = e;
	}
	return 0;
}

/* HTML attributes whose values match case-insensitively. */
static int ci_attr(const char *name)
{
	static const char *const names[] = {
		"accept", "accept-charset", "align", "alink", "axis",
		"bgcolor", "charset", "checked", "clear", "codetype", "color",
		"compact", "declare", "defer", "dir", "direction", "disabled",
		"enctype", "face", "frame", "hreflang", "http-equiv", "lang",
		"language", "link", "media", "method", "multiple", "nohref",
		"noresize", "noshade", "nowrap", "readonly", "rel", "rev",
		"rules", "scope", "scrolling", "selected", "shape", "target",
		"text", "type", "valign", "valuetype", "vlink", NULL
	};
	int i;

	for (i = 0; names[i]; i++)
		if (!strcmp(name, names[i]))
			return 1;
	return 0;
}

static int match_attr(const struct simple *s, const soie_node *el)
{
	const char *v = attr(el, s->name);
	size_t vl, sl;
	int ci;

	if (!v)
		return 0;
	if (s->op == OP_EXISTS)
		return 1;
	ci = s->ci || (el->ns == SOIE_NS_HTML && ci_attr(s->name));
	vl = strlen(v);
	sl = strlen(s->value);
	switch (s->op) {
	case OP_EQ:
		return str_eq(v, s->value, ci);
	case OP_INCLUDES:
		return word_in(v, s->value, ci);
	case OP_DASH:
		return str_prefix(v, s->value, ci) &&
			(v[sl] == '\0' || v[sl] == '-');
	case OP_PREFIX:
		return sl && str_prefix(v, s->value, ci);
	case OP_SUFFIX:
		return sl && vl >= sl && str_prefix(v + vl - sl, s->value, ci);
	case OP_SUBSTRING:
		if (!sl)
			return 0;
		for (; vl >= sl; v++, vl--)
			if (str_prefix(v, s->value, ci))
				return 1;
		return 0;
	}
	return 0;
}

static int same_type(const soie_node *a, const soie_node *b)
{
	return a->ns == b->ns && !strcmp(a->name, b->name);
}

static int matches_list(const struct sellist *l, const soie_node *el);

/*
 * & : does el match the parent rule's list? The last answers are kept
 * while one selector is matched (nest_depth: cleared when the outermost
 * match starts, nothing changes meanwhile): `&:is(&)` asked twice at
 * each level of nesting, 2^n times in all.
 */
#define NEST_MEMO 16
static THREAD_LOCAL struct {
	const struct sellist *l;
	const soie_node *el;
	int r;
} nest_memo[NEST_MEMO];
static THREAD_LOCAL int nest_n, nest_depth;

static int nest_match(const struct sellist *l, const soie_node *el)
{
	int i, r;

	/* only while match_from() runs: it empties the memo first; a
	 * compound matched from elsewhere would read answers of another
	 * time */
	if (!nest_depth)
		return matches_list(l, el);
	for (i = 0; i < NEST_MEMO && i < nest_n; i++)
		if (nest_memo[i].l == l && nest_memo[i].el == el)
			return nest_memo[i].r;
	r = matches_list(l, el);
	i = nest_n++ % NEST_MEMO;
	nest_memo[i].l = l;
	nest_memo[i].el = el;
	nest_memo[i].r = r;
	return r;
}

/* Position (1-based) among siblings, from the end if last. */
static int nth_index(const soie_node *el, int last, int of_type,
	const struct sellist *of)
{
	const soie_node *n;
	int i = 1;

	for (n = last ? next_el(el) : prev_el(el); n;
	     n = last ? next_el(n) : prev_el(n))
		if ((!of_type || same_type(n, el)) &&
		    (!of || matches_list(of, n)))
			i++;
	return i;
}

static int anb_match(int a, int b, int i)
{
	if (a == 0)
		return i == b;
	return (i - b) % a == 0 && (i - b) / a >= 0;
}

static int is_html_el(const soie_node *el, int tag)
{
	return el->ns == SOIE_NS_HTML && el->tag == tag;
}

static int form_control(const soie_node *el)
{
	return el->ns == SOIE_NS_HTML &&
		(el->tag == TAG_BUTTON || el->tag == TAG_INPUT ||
		 el->tag == TAG_SELECT || el->tag == TAG_TEXTAREA ||
		 el->tag == TAG_OPTGROUP || el->tag == TAG_OPTION ||
		 el->tag == TAG_FIELDSET);
}

static int lang_match(const soie_node *el, const char *range)
{
	const char *l = NULL;
	size_t n = strlen(range);

	for (; el && el->type == SOIE_ELEMENT; el = el->parent) {
		struct attr *a = NULL;
		unsigned i;

		for (i = 0; i < el->u.el.nattrs; i++)
			if (!strcmp(el->u.el.attrs[i].name, "lang") &&
			    (el->u.el.attrs[i].ns == SOIE_NS_NONE ||
			     el->u.el.attrs[i].ns == SOIE_NS_XML))
				a = &el->u.el.attrs[i];
		if (a) {
			l = a->value;
			break;
		}
	}
	if (!l)
		return 0;
	return str_prefix(l, range, 1) && (l[n] == '\0' || l[n] == '-');
}

static int match_pseudo(const struct simple *s, const soie_node *el)
{
	const soie_node *n;
	const char *t;
	int i;

	switch (s->pseudo) {
	case PC_ROOT:
		/* :scope in a query from an element is that element */
		if (s->op && sel_scope)
			return el == sel_scope;
		return el->parent && el->parent->type == SOIE_DOCUMENT;
	case PC_EMPTY:
		for (n = el->first; n; n = n->next)
			if (n->type == SOIE_ELEMENT ||
			    (n->type == SOIE_TEXT && n->u.text.len))
				return 0;
		return 1;
	case PC_FIRST_CHILD:
		return !prev_el(el) && parent_el(el);
	case PC_LAST_CHILD:
		return !next_el(el) && parent_el(el);
	case PC_ONLY_CHILD:
		return !prev_el(el) && !next_el(el) && parent_el(el);
	case PC_FIRST_OF_TYPE:
		return nth_index(el, 0, 1, NULL) == 1;
	case PC_LAST_OF_TYPE:
		return nth_index(el, 1, 1, NULL) == 1;
	case PC_ONLY_OF_TYPE:
		return nth_index(el, 0, 1, NULL) == 1 &&
			nth_index(el, 1, 1, NULL) == 1;
	case PC_NTH_CHILD:
	case PC_NTH_LAST_CHILD:
		if (s->args && !matches_list(s->args, el))
			return 0;
		return anb_match(s->a, s->b, nth_index(el,
			s->pseudo == PC_NTH_LAST_CHILD, 0, s->args));
	case PC_NTH_OF_TYPE:
	case PC_NTH_LAST_OF_TYPE:
		return anb_match(s->a, s->b, nth_index(el,
			s->pseudo == PC_NTH_LAST_OF_TYPE, 1, NULL));
	case PC_NOT:
		return !matches_list(s->args, el);
	case PC_IS:
	case PC_WHERE:
		return matches_list(s->args, el);
	case PC_HAS:
		for (i = 0; i < s->args->n; i++)
			if (selector_match_relative(&s->args->c[i], el,
						    s->args->parent))
				return 1;
		return 0;
	case PC_LANG:
		return lang_match(el, s->value);
	case PC_DIR:
		for (n = el; n && n->type == SOIE_ELEMENT; n = n->parent)
			if ((t = attr(n, "dir")) && (ascii_ieq(t, "rtl") ||
						     ascii_ieq(t, "ltr")))
				return ascii_ieq(t, s->value);
		return !strcmp(s->value, "ltr");
	case PC_LINK:
		return (is_html_el(el, TAG_A) || is_html_el(el, TAG_AREA)) &&
			attr(el, "href");
	case PC_VISITED:
		return 0;
	case PC_CHECKED:
		if (is_html_el(el, TAG_INPUT)) {
			t = attr(el, "type");
			return t && (ascii_ieq(t, "checkbox") ||
				     ascii_ieq(t, "radio")) &&
				attr(el, "checked");
		}
		return is_html_el(el, TAG_OPTION) && attr(el, "selected");
	case PC_DISABLED:
		return form_control(el) && attr(el, "disabled");
	case PC_ENABLED:
		return form_control(el) && !attr(el, "disabled");
	case PC_REQUIRED:
	case PC_OPTIONAL:
		if (!is_html_el(el, TAG_INPUT) && !is_html_el(el, TAG_SELECT) &&
		    !is_html_el(el, TAG_TEXTAREA))
			return 0;
		return (attr(el, "required") != NULL) ==
			(s->pseudo == PC_REQUIRED);
	case PC_READ_WRITE:
	case PC_READ_ONLY:
		i = (is_html_el(el, TAG_INPUT) ||
		     is_html_el(el, TAG_TEXTAREA)) &&
			!attr(el, "readonly") && !attr(el, "disabled");
		if (!i && (t = attr(el, "contenteditable")))
			i = !ascii_ieq(t, "false");
		return i == (s->pseudo == PC_READ_WRITE);
	case PC_PLACEHOLDER_SHOWN:
		return (is_html_el(el, TAG_INPUT) ||
			is_html_el(el, TAG_TEXTAREA)) &&
			attr(el, "placeholder") &&
			(!(t = attr(el, "value")) || !*t);
	case PC_OPEN:
		return attr(el, "open") != NULL;
	case PC_HOST:
		return is_host(el) && (!s->args ||
			matches_outside(s->args, el));
	case PC_HOST_CONTEXT:
		if (!is_host(el))
			return 0;
		/* the host or an ancestor, through outer hosts */
		for (n = el; n; n = node_above(n))
			if (n->type == SOIE_ELEMENT &&
			    matches_outside(s->args, n))
				return 1;
		return 0;
	case PC_DEFINED:
	case PC_TRUE:
		return 1;
	case PC_VALID:
		return form_control(el);
	}
	return 0;
}

static int match_compound(const struct compound *c, const soie_node *el,
	const struct sellist *parent)
{
	int i;
	const char *v;
	int quirks = el->doc->u.doc->quirks == SOIE_QUIRKS;

	for (i = 0; i < c->n; i++) {
		const struct simple *s = &c->s[i];

		switch (s->type) {
		case SEL_TYPE:
			if (el->ns == SOIE_NS_HTML ? strcmp(el->name, s->name) :
			    strcmp(el->name, s->value))
				return 0;
			break;
		case SEL_UNIVERSAL:
		case SEL_PSEUDO_ELEMENT:
			break;
		case SEL_ID:
			if (!(v = attr(el, "id")) || !str_eq(v, s->value, quirks))
				return 0;
			break;
		case SEL_CLASS:
			if (!(v = attr(el, "class")) ||
			    !word_in(v, s->value, quirks))
				return 0;
			break;
		case SEL_ATTR:
			if (!match_attr(s, el))
				return 0;
			break;
		case SEL_PSEUDO:
			if (!match_pseudo(s, el))
				return 0;
			break;
		case SEL_NEST:
			if (!parent || !nest_match(parent, el))
				return 0;
			break;
		}
	}
	return 1;
}

/*
 * Incremental restyling: could a change at n change what the :has()
 * arguments of l match? A change of attribute name on n (from old to
 * its value now), else n inserted or removed. No only when sure:
 * - an attribute: no compound of the argument tests it (a class or an
 *   id: one of the old or new values; a state pseudo-class: the
 *   attribute it reads);
 * - a subtree: with descendant and child combinators a new match has
 *   its subject in that subtree (an element's ancestors do not move),
 *   so no element of it has what the rightmost compound requires;
 *   sibling combinators and positions (:nth-child(), :empty) may
 *   change for others: yes.
 */
static int subtree_has_key(const struct compound *c, const soie_node *n)
{
	const struct simple *k = NULL;
	const soie_node *e = n;
	const char *v;
	int i, quirks = n->doc->u.doc->quirks == SOIE_QUIRKS;

	for (i = 0; i < c->n && !k; i++)
		if (c->s[i].type == SEL_TYPE || c->s[i].type == SEL_ID ||
		    c->s[i].type == SEL_CLASS || c->s[i].type == SEL_ATTR)
			k = &c->s[i];
	if (!k)
		return 1;
	while (e) {
		if (e->type == SOIE_ELEMENT) {
			switch (k->type) {
			case SEL_TYPE:
				if (!strcmp(e->name, e->ns == SOIE_NS_HTML ?
				    k->name : k->value))
					return 1;
				break;
			case SEL_ID:
				if ((v = attr(e, "id")) && str_eq(v, k->value,
				    quirks))
					return 1;
				break;
			case SEL_CLASS:
				if ((v = attr(e, "class")) && word_in(v, k->value,
				    quirks))
					return 1;
				break;
			default:
				if (attr(e, k->name))
					return 1;
			}
		}
		if (e->first) {
			e = e->first;
			continue;
		}
		while (e != n && !e->next)
			e = e->parent;
		if (e == n)
			break;
		e = e->next;
	}
	return 0;
}

/* Does a pseudo-class read attribute name? 1 when unsure. */
static int pseudo_reads(int pc, const char *name)
{
	static const struct {
		int pc;
		const char *names;
	} reads[] = {
		{ PC_LINK, " href " }, { PC_VISITED, " href " },
		{ PC_CHECKED, " checked selected type " },
		{ PC_DISABLED, " disabled " }, { PC_ENABLED, " disabled " },
		{ PC_REQUIRED, " required " }, { PC_OPTIONAL, " required " },
		{ PC_READ_ONLY, " readonly contenteditable disabled type " },
		{ PC_READ_WRITE, " readonly contenteditable disabled type " },
		{ PC_PLACEHOLDER_SHOWN, " placeholder value type " },
		{ PC_OPEN, " open " }, { PC_LANG, " lang xml:lang " },
		{ PC_DIR, " dir " }, { PC_DEFINED, " is " }
	};
	char w[64];
	size_t i;

	if (pc >= PC_ROOT && pc <= PC_NTH_LAST_OF_TYPE)
		return 0;	/* positions, not attributes */
	if (pc == PC_TRUE || pc == PC_FALSE)
		return 0;
	if (strlen(name) > sizeof(w) - 3)
		return 1;
	snprintf(w, sizeof(w), " %s ", name);
	for (i = 0; i < sizeof(reads) / sizeof(*reads); i++)
		if (reads[i].pc == pc)
			return strstr(reads[i].names, w) != NULL;
	return 1;
}

/* Is a value of class (or id) w in list (old or new)? */
static int value_in(const char *w, const char *list, int words)
{
	if (!list || !w)
		return 0;
	return words ? word_in(list, w, 0) : !strcmp(list, w);
}

static int arg_sees_attr(const struct sellist *l, const soie_node *n,
	const char *name, const char *old)
{
	const char *now = attr(n, name);
	const struct simple *s;
	int i, j, k;

	for (i = 0; i < l->n; i++)
		for (j = 0; j < l->c[i].n; j++)
			for (k = 0; k < l->c[i].c[j].n; k++) {
				s = &l->c[i].c[j].s[k];
				if (s->args && arg_sees_attr(s->args, n, name, old))
					return 1;
				switch (s->type) {
				case SEL_ATTR:
					if (ascii_ieq(s->name, name))
						return 1;
					break;
				case SEL_CLASS:
					if (!strcmp(name, "class") &&
					    (value_in(s->value, old, 1) ||
					     value_in(s->value, now, 1)))
						return 1;
					break;
				case SEL_ID:
					if (!strcmp(name, "id") &&
					    (value_in(s->value, old, 0) ||
					     value_in(s->value, now, 0)))
						return 1;
					break;
				case SEL_PSEUDO:
					if (s->pseudo != PC_NOT &&
					    s->pseudo != PC_IS &&
					    s->pseudo != PC_WHERE &&
					    s->pseudo != PC_HAS &&
					    pseudo_reads(s->pseudo, name))
						return 1;
					break;
				case SEL_NEST:
					return 1;
				}
			}
	return 0;
}

/* Positions anywhere in l (sibling combinators, :nth-*, :empty...) */
static int has_positions(const struct sellist *l)
{
	const struct compound *c;
	int i, j, k;

	for (i = 0; i < l->n; i++)
		for (j = 0; j < l->c[i].n; j++) {
			c = &l->c[i].c[j];
			if (c->comb == COMB_NEXT || c->comb == COMB_SUBSEQ)
				return 1;
			for (k = 0; k < c->n; k++) {
				if (c->s[k].type == SEL_NEST)
					return 1;
				if (c->s[k].type == SEL_PSEUDO &&
				    c->s[k].pseudo >= PC_ROOT &&
				    c->s[k].pseudo <= PC_NTH_LAST_OF_TYPE)
					return 1;
				if (c->s[k].args && has_positions(c->s[k].args))
					return 1;
			}
		}
	return 0;
}

static int arg_sees(const struct sellist *l, const soie_node *n,
	const char *name, const char *old)
{
	int i;

	if (name)
		return n->type == SOIE_ELEMENT &&
			arg_sees_attr(l, n, name, old);
	if (has_positions(l))
		return 1;
	if (n->type != SOIE_ELEMENT)
		return 0;	/* text: only :empty, a position */
	for (i = 0; i < l->n; i++)
		if (!l->c[i].n || subtree_has_key(&l->c[i].c[l->c[i].n - 1], n))
			return 1;
	return 0;
}

int sellist_has_may_change(const struct sellist *l, const soie_node *n,
	const char *name, const char *old)
{
	const struct simple *s;
	int i, j, k;

	for (i = 0; l && i < l->n; i++)
		for (j = 0; j < l->c[i].n; j++)
			for (k = 0; k < l->c[i].c[j].n; k++) {
				s = &l->c[i].c[j].s[k];
				if (!s->args)
					continue;
				if (s->type == SEL_PSEUDO && s->pseudo == PC_HAS ?
				    arg_sees(s->args, n, name, old) :
				    sellist_has_may_change(s->args, n, name, old))
					return 1;
			}
	return 0;
}

/* Has l a :has() whose argument starts with + or ~? Its anchor is
 * then a previous sibling of what changes, not an ancestor. */
int sellist_has_sibling(const struct sellist *l)
{
	const struct simple *s;
	int i, j, k, m;

	for (i = 0; l && i < l->n; i++)
		for (j = 0; j < l->c[i].n; j++)
			for (k = 0; k < l->c[i].c[j].n; k++) {
				s = &l->c[i].c[j].s[k];
				if (!s->args)
					continue;
				if (s->type == SEL_PSEUDO && s->pseudo == PC_HAS)
					for (m = 0; m < s->args->n; m++)
						if (s->args->c[m].n &&
						    (s->args->c[m].c[0].comb ==
						     COMB_NEXT ||
						     s->args->c[m].c[0].comb ==
						     COMB_SUBSEQ))
							return 1;
				if (sellist_has_sibling(s->args))
					return 1;
			}
	return 0;
}

/*
 * Incremental restyling: is el the anchor of a :has() left of a
 * subject (`.wrap:has(.label) .input`)? The compound is matched alone
 * and without its :has(), which can only say yes too often: a change
 * below el may change what el's subtree and later siblings match.
 */
static int compound_has(const struct compound *c)
{
	int k;

	for (k = 0; k < c->n; k++)
		if (c->s[k].type == SEL_PSEUDO && c->s[k].pseudo == PC_HAS)
			return 1;
	return 0;
}

int sellist_has_anchor(const struct sellist *l, const soie_node *el)
{
	const struct compound *c;
	struct compound one;
	int i, j, k, ok;

	for (i = 0; l && i < l->n; i++)
		for (j = 0; j < l->c[i].n; j++) {
			c = &l->c[i].c[j];
			for (k = 0; k < c->n; k++)
				if (c->s[k].args && c->s[k].pseudo != PC_HAS &&
				    sellist_has_anchor(c->s[k].args, el))
					return 1;
			if (j == l->c[i].n - 1 || !compound_has(c))
				continue;
			ok = 1;
			for (k = 0; k < c->n && ok; k++) {
				if (c->s[k].type == SEL_PSEUDO &&
				    c->s[k].pseudo == PC_HAS)
					continue;
				one.s = &c->s[k];
				one.n = 1;
				one.comb = COMB_NONE;
				ok = match_compound(&one, el, l->parent);
			}
			if (ok)
				return 1;
		}
	return 0;
}

/* Does the relation comb hold between el (right) and x (left)? */
static int related(const soie_node *x, const soie_node *el, int comb)
{
	const soie_node *n;

	switch (comb) {
	case COMB_DESC:
		for (n = el->parent; n; n = n->parent)
			if (n == x)
				return 1;
		return 0;
	case COMB_CHILD:
		return el->parent == x;
	case COMB_NEXT:
		return prev_el(el) == x;
	case COMB_SUBSEQ:
		for (n = prev_el(el); n; n = prev_el(n))
			if (n == x)
				return 1;
		return 0;
	}
	return 0;
}

/*
 * Match compounds [0, idx] with el as the subject of compound idx.
 * anchor: for relative selectors, the :has() element compound 0
 * must be related to. Returns 1 for a match, 0 when el does not match
 * but another candidate may, -1 when no ancestor further up can
 * either: the ancestors left to try are fewer of the same. Without
 * it `a b c d e` tried every ancestor at every level, depth^4 (50
 * nested divs and six descendant combinators: over a second).
 */
static int match_rec(const struct complex *c, int idx, const soie_node *el,
	const soie_node *anchor, const struct sellist *parent)
{
	const soie_node *n, *p;
	int comb, r;

	if (!match_compound(&c->c[idx], el, parent))
		return 0;
	comb = c->c[idx].comb;
	if (idx == 0)
		return anchor ? related(anchor, el, comb) : 1;
	switch (comb) {
	case COMB_DESC:
		for (n = el; (p = parent_el(n)); )
			if ((r = match_rec(c, idx - 1, n = p, anchor, parent)))
				return r;
		/* past the top of a shadow tree, its host */
		n = top_of_fragment(n) ? host_up(n, c, idx - 1) : NULL;
		return n ? match_rec(c, idx - 1, n, anchor, parent) : -1;
	case COMB_CHILD:
		if (!(n = parent_el(el)) && top_of_fragment(el))
			n = host_up(el, c, idx - 1);
		return n ? match_rec(c, idx - 1, n, anchor, parent) : -1;
	case COMB_NEXT:
		n = prev_el(el);
		return n ? match_rec(c, idx - 1, n, anchor, parent) : 0;
	case COMB_SUBSEQ:
		for (n = prev_el(el); n; n = prev_el(n))
			if ((r = match_rec(c, idx - 1, n, anchor, parent)))
				return r;
		return 0;
	}
	return 0;
}

static int match_from(const struct complex *c, int idx, const soie_node *el,
	const soie_node *anchor, const struct sellist *parent)
{
	int r;

	if (!nest_depth++)
		nest_n = 0;
	r = match_rec(c, idx, el, anchor, parent) > 0;
	nest_depth--;
	return r;
}

int selector_match(const struct sellist *l, int i, const soie_node *el)
{
	return match_from(&l->c[i], l->c[i].n - 1, el, NULL, l->parent);
}

static int matches_list(const struct sellist *l, const soie_node *el)
{
	int i;

	for (i = 0; i < l->n; i++)
		if (match_from(&l->c[i], l->c[i].n - 1, el, NULL, l->parent))
			return 1;
	return 0;
}

/* :has(): look for a subject among descendants or later siblings. */
static int selector_match_relative(const struct complex *c,
	const soie_node *el, const struct sellist *parent)
{
	const soie_node *start, *n, *stop;
	int sibling = c->c[0].comb == COMB_NEXT ||
		c->c[0].comb == COMB_SUBSEQ;

	start = sibling ? next_el(el) : el->first;
	stop = sibling ? el->parent : el;
	for (n = start; n && n != stop; ) {
		if (n->type == SOIE_ELEMENT &&
		    match_from(c, c->n - 1, n, el, parent))
			return 1;
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n != stop && !n->next)
			n = n->parent;
		if (n == stop)
			break;
		n = n->next;
	}
	return 0;
}

int sellist_match(const struct sellist *l, const soie_node *el)
{
	return matches_list(l, el);
}

/* Specificity of the most specific selector of l matching el. */
unsigned sellist_max_spec(const struct sellist *l, const soie_node *el)
{
	unsigned m = 0;
	int i;

	for (i = 0; i < l->n; i++)
		if (l->c[i].spec > m && match_from(&l->c[i], l->c[i].n - 1,
						   el, NULL, l->parent))
			m = l->c[i].spec;
	return m;
}
