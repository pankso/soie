/*
 * The box tree: generation from the DOM and computed styles (CSS 2
 * chapter 9, CSS Display 3), anonymous boxes, generated content, list
 * markers. Also the layout dump used by tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"
#include "font.h"
#include "image.h"

/* Keyword test with a small cache: callers pass string literals. */
int kw(const struct style *st, int prop, const char *name)
{
	static struct {
		int prop;
		const char *name;
		int kw;
	} cache[128];
	static int n;
	const char *list;
	int i, k;

	for (i = 0; i < n; i++)
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
	if (n < 128) {
		cache[n].prop = prop;
		cache[n].name = name;
		cache[n].kw = k;
		n++;
	}
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
	free(b);
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

/* Intrinsic sizes of replaced elements, until images load (phase 6). */
static void intrinsic(struct box *b, const soie_node *n)
{
	float em = b->style->v[P_FONT_SIZE].u.len.px;
	const struct image *im;

	if (n->ns == SOIE_NS_SVG) {
		svg_size(n, &b->iw, &b->ih);
		if (!b->iw && !b->ih) {
			b->iw = 300;
			b->ih = 150;
		}
	} else if ((n->ns == SOIE_NS_HTML && (n->tag == TAG_IMG ||
		    n->tag == TAG_OBJECT || n->tag == TAG_EMBED ||
		    n->tag == TAG_INPUT)) && (im = image_for(n))) {
		b->iw = (float)im->w;
		b->ih = (float)im->h;
	} else if (n->ns == SOIE_NS_HTML && n->tag == TAG_IMG) {
		b->iw = attr_num(n, "width", 0);
		b->ih = attr_num(n, "height", 0);
	} else if (n->ns == SOIE_NS_HTML && n->tag == TAG_INPUT) {
		const char *t = soie_element_get_attr(n, "type");
		struct fontm fm;

		font_metrics(b->style, &fm);
		if (t && (ascii_ieq(t, "checkbox") || ascii_ieq(t, "radio"))) {
			b->iw = b->ih = 13;
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
		b->iw = 300;
		b->ih = 54;
	} else {
		b->iw = 300;
		b->ih = 150;
		if (n->ns == SOIE_NS_HTML && !strcmp(n->name, "video"))
			media_size(n, &b->iw, &b->ih);
		b->iw = attr_num(n, "width", b->iw);
		b->ih = attr_num(n, "height", b->ih);
	}
}

/* Text of ::before/::after: strings, attr(), quotes. */
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
		}
		/* counter(), url(): not yet */
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

		if (!strcmp(t, "lower-roman") || !strcmp(t, "upper-roman"))
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
	text = content_text(st->v[P_CONTENT].u.str, el);
	if (*text)
		box_append(b, text_box(st, text, strlen(text)));
	free(text);
	return b;
}

static void gen_node(struct box *parent, soie_node *n)
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
		gen_children(parent, n);
		return;
	}
	if (is_html(n, TAG_BR)) {
		box_append(parent, box_new(B_BREAK, st, n));
		return;
	}
	if (is_replaced(n)) {
		b = box_new(B_REPLACED, st, n);
		if (box_type(st) == B_BLOCK)
			b->flags |= BF_BLOCKLEVEL;
		set_flags(b);
		intrinsic(b, n);
		box_append(parent, b);
		return;
	}
	b = box_new(box_type(st), st, n);
	set_flags(b);
	if (kw(st, P_DISPLAY, "list-item") && (m = marker(n, st)))
		box_append(b, m);
	depth++;
	gen_children(b, n);
	depth--;
	box_append(parent, b);
}

static void gen_children(struct box *parent, soie_node *el)
{
	soie_node *c;
	struct box *p;

	if ((p = gen_pseudo(el, el->u.el.before, 'b')))
		box_append(parent, p);
	for (c = el->first; c; c = c->next) {
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
	box_free(b);
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
		while (b->first) {
			c = b->first;
			b->first = c->next;
			box_free(c);
		}
		b->last = NULL;
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
						box_free(k);
				}
				c->first = c->last = NULL;
				box_free(c);
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

	if (!root || root->type != SOIE_ELEMENT || !root->u.el.style || !doc->u.doc.styles ||
	    kw(root->u.el.style, P_DISPLAY, "none"))
		return NULL;
	b = box_new(B_BLOCK, root->u.el.style, root);
	b->flags = BF_BFC;
	depth = 1;
	gen_children(b, root);
	fixup(doc->u.doc.styles, b);
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
	struct layout *l;

	if (doc->type != SOIE_DOCUMENT || !doc->u.doc.styles)
		return -1;
	if (doc->u.doc.layout)
		layout_free(doc->u.doc.layout);
	l = xmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->width = width > 0 ? (float)width : 1024;
	l->height = height > 0 ? (float)height : 768;
	font_doc = doc;
	font_cache_clear();
	l->root = build_boxes(doc);
	if (l->root)
		layout_root(l);
	doc->u.doc.layout = l;
	doc->u.doc.lay_w = (int)l->width;
	doc->u.doc.lay_h = (int)l->height;
	return 0;
}

float soie_layout_height(const soie_node *doc)
{
	return doc->type == SOIE_DOCUMENT && doc->u.doc.layout ?
		doc->u.doc.layout->doc_height : 0;
}

char *soie_layout_dump(const soie_node *doc, size_t *len)
{
	return layout_dump(doc->type == SOIE_DOCUMENT && doc->u.doc.layout ?
		doc->u.doc.layout->root : NULL, len);
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

static soie_node *hit(struct box *b, float x, float y)
{
	struct box *c, *kids[4096];
	int i, n = 0;
	soie_node *r;

	for (c = b->first; c && n < 4096; c = c->next)
		kids[n++] = c;
	/* floats and positioned children are over the rest */
	for (i = n - 1; i >= 0; i--)
		if ((kids[i]->flags & (BF_FLOAT | BF_ABS)) &&
		    (r = hit(kids[i], x, y)))
			return r;
	if (b->flags & BF_INLINE_KIDS) {
		for (i = b->nfrags - 1; i >= 0; i--) {
			struct frag *f = &b->frags[i];

			if (f->kind == F_ATOMIC) {
				if ((r = hit(f->box, x, y)))
					return r;
			} else if (inside(x, y, f->x, f->y, f->w, f->h)) {
				return node_of(f->kind == F_TEXT ?
					f->box->parent : f->box);
			}
		}
	} else {
		for (i = n - 1; i >= 0; i--)
			if (!(kids[i]->flags & (BF_FLOAT | BF_ABS)) &&
			    (r = hit(kids[i], x, y)))
				return r;
	}
	if (b->type != B_INLINE && b->node &&
	    inside(x, y, b->x - b->p[3] - b->b[3], b->y - b->p[0] - b->b[0],
		   b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3],
		   b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2]))
		return b->node;
	return NULL;
}

soie_node *soie_hit_test(soie_node *doc, float x, float y)
{
	if (doc->type != SOIE_DOCUMENT || !doc->u.doc.layout ||
	    !doc->u.doc.layout->root)
		return NULL;
	return hit(doc->u.doc.layout->root, x, y);
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

	if (!doc->u.doc.layout || !doc->u.doc.layout->root ||
	    !(b = find_box(doc->u.doc.layout->root, el)))
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

void soie_set_focus(soie_node *doc, soie_node *el, int caret)
{
	if (doc->type != SOIE_DOCUMENT)
		return;
	doc->u.doc.focus = el;
	doc->u.doc.caret = caret;
}
