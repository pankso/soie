/*
 * Selectors Level 4 subset: parsing, specificity and matching.
 */
#include <stdlib.h>
#include <string.h>
#include "css.h"

enum {
	PC_NONE,
	PC_ROOT, PC_EMPTY, PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD,
	PC_FIRST_OF_TYPE, PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE,
	PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE,
	PC_NOT, PC_IS, PC_WHERE, PC_HAS, PC_LANG, PC_DIR,
	PC_LINK, PC_VISITED, PC_CHECKED, PC_DISABLED, PC_ENABLED,
	PC_REQUIRED, PC_OPTIONAL, PC_READ_ONLY, PC_READ_WRITE,
	PC_PLACEHOLDER_SHOWN, PC_OPEN, PC_DEFINED, PC_VALID,
	PC_TRUE,	/* always matches: :scope... handled as root */
	PC_FALSE	/* states the engine does not have yet */
};

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
	{ "fullscreen", PC_FALSE }, { "host", PC_FALSE },
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

static char *lowdup(const char *s)
{
	size_t n = strlen(s), i;
	char *d = xstrndup(s, n);

	for (i = 0; i < n; i++)
		if (d[i] >= 'A' && d[i] <= 'Z')
			d[i] = (char)(d[i] + 32);
	return d;
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
int selector_match_relative(const struct complex *c, const soie_node *el,
	const struct sellist *parent);

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
	if (ascii_ieq(n, "host") || ascii_ieq(n, "host-context") ||
	    ascii_ieq(n, "state") || ascii_ieq(n, "active-view-transition-type")) {
		/* shadow DOM and custom states: valid, never matching */
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
				*pe = PE_OTHER;
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
			spec += SPEC_C;
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

static int word_in(const char *list, const char *w, int ci)
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
	int quirks = el->doc->u.doc.quirks == SOIE_QUIRKS;

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
			if (!parent || !matches_list(parent, el))
				return 0;
			break;
		}
	}
	return 1;
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
 * must be related to.
 */
static int match_from(const struct complex *c, int idx, const soie_node *el,
	const soie_node *anchor, const struct sellist *parent)
{
	const soie_node *n;
	int comb;

	if (!match_compound(&c->c[idx], el, parent))
		return 0;
	comb = c->c[idx].comb;
	if (idx == 0)
		return anchor ? related(anchor, el, comb) : 1;
	switch (comb) {
	case COMB_DESC:
		for (n = parent_el(el); n; n = parent_el(n))
			if (match_from(c, idx - 1, n, anchor, parent))
				return 1;
		return 0;
	case COMB_CHILD:
		n = parent_el(el);
		return n && match_from(c, idx - 1, n, anchor, parent);
	case COMB_NEXT:
		n = prev_el(el);
		return n && match_from(c, idx - 1, n, anchor, parent);
	case COMB_SUBSEQ:
		for (n = prev_el(el); n; n = prev_el(n))
			if (match_from(c, idx - 1, n, anchor, parent))
				return 1;
		return 0;
	}
	return 0;
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
int selector_match_relative(const struct complex *c, const soie_node *el,
	const struct sellist *parent)
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
