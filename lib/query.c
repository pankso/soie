/*
 * DOM helpers for script engines: selector queries and HTML
 * serialization (what querySelector() and innerHTML need).
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "css.h"

static struct sellist *parse_selectors(const char *s)
{
	struct cvlist l;
	struct sellist *sel;

	css_component_values(s, strlen(s), &l);
	sel = l.n ? selector_parse(&l, 0, l.n, NULL, 0) : NULL;
	cvlist_free(&l);
	return sel;
}

int soie_element_matches(const soie_node *el, const char *selectors)
{
	struct sellist *sel = parse_selectors(selectors);
	int m;

	if (!sel)
		return -1;
	m = el->type == SOIE_ELEMENT && sellist_match(sel, el);
	sellist_free(sel);
	return m;
}

soie_node **soie_query(soie_node *root, const char *selectors, size_t max,
	size_t *count)
{
	struct sellist *sel = parse_selectors(selectors);
	soie_node **v, *n;
	size_t k = 0, cap = 8;

	*count = 0;
	if (!sel)
		return NULL;
	v = xmalloc(cap * sizeof(*v));
	for (n = root->first; n && (!max || k < max); ) {
		if (n->type == SOIE_ELEMENT && sellist_match(sel, n)) {
			if (k + 1 == cap) {
				cap *= 2;
				v = xrealloc(v, cap * sizeof(*v));
			}
			v[k++] = n;
		}
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n && n != root && !n->next)
			n = n->parent;
		n = n && n != root ? n->next : NULL;
	}
	v[k] = NULL;
	*count = k;
	sellist_free(sel);
	return v;
}

/* HTML fragment serialization (HTML 13.3) */

static int is_void(const soie_node *el)
{
	static const char *const names[] = { "area", "base", "basefont",
		"bgsound", "br", "col", "embed", "frame", "hr", "img", "input",
		"keygen", "link", "meta", "param", "source", "track", "wbr",
		NULL };
	int i;

	if (el->ns != SOIE_NS_HTML)
		return 0;
	for (i = 0; names[i]; i++)
		if (!strcmp(el->name, names[i]))
			return 1;
	return 0;
}

static int raw_text_parent(const soie_node *p)
{
	static const char *const names[] = { "style", "script", "xmp",
		"iframe", "noembed", "noframes", "plaintext", NULL };
	int i;

	if (!p || p->type != SOIE_ELEMENT || p->ns != SOIE_NS_HTML)
		return 0;
	for (i = 0; names[i]; i++)
		if (!strcmp(p->name, names[i]))
			return 1;
	return 0;
}

static void escape(struct buf *b, const char *s, size_t len, int attr)
{
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == '&')
			buf_add(b, "&amp;", 5);
		else if (c == 0xc2 && i + 1 < len &&
			 (unsigned char)s[i + 1] == 0xa0) {
			buf_add(b, "&nbsp;", 6);
			i++;
		} else if (c == '"' && attr)
			buf_add(b, "&quot;", 6);
		else if (c == '<')
			buf_add(b, "&lt;", 4);
		else if (c == '>')
			buf_add(b, "&gt;", 4);
		else
			buf_putc(b, c);
	}
}

static void attr_name(struct buf *b, const struct attr *a)
{
	switch (a->ns) {
	case SOIE_NS_XML:
		buf_add(b, "xml:", 4);
		break;
	case SOIE_NS_XMLNS:
		if (strcmp(a->name, "xmlns"))
			buf_add(b, "xmlns:", 6);
		break;
	case SOIE_NS_XLINK:
		buf_add(b, "xlink:", 6);
		break;
	}
	buf_add(b, a->name, strlen(a->name));
}

static void open_tag(struct buf *b, const soie_node *n)
{
	unsigned i;

	buf_putc(b, '<');
	buf_add(b, n->name, strlen(n->name));
	for (i = 0; i < n->u.el.nattrs; i++) {
		const struct attr *a = &n->u.el.attrs[i];

		buf_putc(b, ' ');
		attr_name(b, a);
		buf_add(b, "=\"", 2);
		escape(b, a->value, strlen(a->value), 1);
		buf_putc(b, '"');
	}
	buf_putc(b, '>');
}

static void close_tag(struct buf *b, const soie_node *n)
{
	buf_add(b, "</", 2);
	buf_add(b, n->name, strlen(n->name));
	buf_putc(b, '>');
}

static void serialize_leaf(struct buf *b, const soie_node *n)
{
	switch (n->type) {
	case SOIE_TEXT:
		if (raw_text_parent(n->parent))
			buf_add(b, n->u.text.data, n->u.text.len);
		else
			escape(b, n->u.text.data, n->u.text.len, 0);
		break;
	case SOIE_COMMENT:
		buf_add(b, "<!--", 4);
		buf_add(b, n->u.text.data, n->u.text.len);
		buf_add(b, "-->", 3);
		break;
	case SOIE_DOCTYPE:
		buf_add(b, "<!DOCTYPE ", 10);
		buf_add(b, n->name, strlen(n->name));
		buf_putc(b, '>');
		break;
	default:
		break;
	}
}

/*
 * The children of top (of its content for a <template>): iterative,
 * recursing only into template contents, which hang off the tree.
 */
static void serialize_children(struct buf *b, const soie_node *top)
{
	const soie_node *n;

	if (top->type == SOIE_ELEMENT && top->u.el.content)
		top = top->u.el.content;
	for (n = top->first; n; ) {
		if (n->type != SOIE_ELEMENT) {
			serialize_leaf(b, n);
		} else {
			open_tag(b, n);
			if (n->u.el.content) {
				serialize_children(b, n);
				close_tag(b, n);
			} else if (n->first && !is_void(n)) {
				n = n->first;
				continue;
			} else if (!is_void(n)) {
				close_tag(b, n);
			}
		}
		while (!n->next) {
			n = n->parent;
			if (!n || n == top)
				return;
			close_tag(b, n);
		}
		n = n->next;
	}
}

char *soie_node_serialize(const soie_node *n, int outer, size_t *len)
{
	struct buf b = { 0 };

	buf_add(&b, "", 0);
	if (!outer || n->type == SOIE_DOCUMENT ||
	    n->type == SOIE_DOCUMENT_FRAGMENT) {
		serialize_children(&b, n);
	} else if (n->type == SOIE_ELEMENT) {
		open_tag(&b, n);
		if (!is_void(n)) {
			serialize_children(&b, n);
			close_tag(&b, n);
		}
	} else {
		serialize_leaf(&b, n);
	}
	if (len)
		*len = b.len;
	return b.s;
}
