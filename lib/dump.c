/*
 * Tree dump in the html5lib tree-construction test format.
 */
#include <stdlib.h>
#include <string.h>
#include "internal.h"

struct dattr {
	char *name;
	const char *value;
};

static void indent(struct buf *b, int depth)
{
	int i;

	buf_add(b, "| ", 2);
	for (i = 0; i < depth; i++)
		buf_add(b, "  ", 2);
}

static int cmp_attr(const void *a, const void *b)
{
	return strcmp(((const struct dattr *)a)->name,
		((const struct dattr *)b)->name);
}

static const char *ns_prefix(int ns)
{
	switch (ns) {
	case SOIE_NS_SVG: return "svg";
	case SOIE_NS_MATHML: return "math";
	case SOIE_NS_XLINK: return "xlink";
	case SOIE_NS_XML: return "xml";
	case SOIE_NS_XMLNS: return "xmlns";
	}
	return NULL;
}

static void dump_attrs(struct buf *b, const soie_node *n, int depth)
{
	unsigned i, count = n->u.el.nattrs;
	struct dattr *a;

	if (!count)
		return;
	a = xmalloc(count * sizeof(*a));
	for (i = 0; i < count; i++) {
		const struct attr *at = &n->u.el.attrs[i];
		const char *pre = ns_prefix(at->ns);
		size_t len = strlen(at->name);

		if (pre) {
			size_t pl = strlen(pre);

			a[i].name = xmalloc(pl + len + 2);
			memcpy(a[i].name, pre, pl);
			a[i].name[pl] = ' ';
			memcpy(a[i].name + pl + 1, at->name, len + 1);
		} else {
			a[i].name = xstrndup(at->name, len);
		}
		a[i].value = at->value;
	}
	qsort(a, count, sizeof(*a), cmp_attr);
	for (i = 0; i < count; i++) {
		indent(b, depth);
		buf_add(b, a[i].name, strlen(a[i].name));
		buf_add(b, "=\"", 2);
		buf_add(b, a[i].value, strlen(a[i].value));
		buf_add(b, "\"\n", 2);
		free(a[i].name);
	}
	free(a);
}

/* One node's own lines, without its children. */
static void dump_node(struct buf *b, const soie_node *n, int depth)
{
	const char *pre;

	indent(b, depth);
	switch (n->type) {
	case SOIE_DOCTYPE:
		buf_add(b, "<!DOCTYPE ", 10);
		buf_add(b, n->name, strlen(n->name));
		if (*soie_doctype_public_id(n) || *soie_doctype_system_id(n)) {
			buf_add(b, " \"", 2);
			buf_add(b, n->u.doctype.public_id,
				strlen(n->u.doctype.public_id));
			buf_add(b, "\" \"", 3);
			buf_add(b, n->u.doctype.system_id,
				strlen(n->u.doctype.system_id));
			buf_putc(b, '"');
		}
		buf_add(b, ">\n", 2);
		return;
	case SOIE_TEXT:
		buf_putc(b, '"');
		buf_add(b, n->u.text.data, n->u.text.len);
		buf_add(b, "\"\n", 2);
		return;
	case SOIE_COMMENT:
		buf_add(b, "<!-- ", 5);
		buf_add(b, n->u.text.data, n->u.text.len);
		buf_add(b, " -->\n", 5);
		return;
	case SOIE_ELEMENT:
		buf_putc(b, '<');
		pre = n->ns == SOIE_NS_HTML ? NULL : ns_prefix(n->ns);
		if (pre) {
			buf_add(b, pre, strlen(pre));
			buf_putc(b, ' ');
		}
		buf_add(b, n->name, strlen(n->name));
		buf_add(b, ">\n", 2);
		dump_attrs(b, n, depth + 1);
		return;
	}
}

/* Pending work of the iterative walk: deep trees must not recurse. */
struct item {
	const soie_node *n;
	int depth;
	int content;		/* the "content" line of a template */
};

struct todo {
	struct item *items;
	size_t len, cap;
};

static void todo_push(struct todo *t, const soie_node *n, int depth,
	int content)
{
	if (t->len == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 64;
		t->items = xrealloc(t->items, t->cap * sizeof(*t->items));
	}
	t->items[t->len].n = n;
	t->items[t->len].depth = depth;
	t->items[t->len].content = content;
	t->len++;
}

/* Queue the children of n, last first, so they pop in order. */
static void todo_children(struct todo *t, const soie_node *n, int depth)
{
	const soie_node *c;

	for (c = n->last; c; c = c->prev)
		todo_push(t, c, depth, 0);
}

char *soie_node_dump(const soie_node *n, size_t *len)
{
	struct buf b = { 0 };
	struct todo t = { NULL, 0, 0 };
	struct item it;

	buf_add(&b, "", 0);
	if (n->type == SOIE_DOCUMENT || n->type == SOIE_DOCUMENT_FRAGMENT)
		todo_children(&t, n, 0);
	else
		todo_push(&t, n, 0, 0);
	while (t.len) {
		it = t.items[--t.len];
		if (it.content) {
			indent(&b, it.depth);
			buf_add(&b, "content\n", 8);
			todo_children(&t, it.n->u.el.content, it.depth + 1);
			continue;
		}
		dump_node(&b, it.n, it.depth);
		todo_children(&t, it.n, it.depth + 1);
		if (it.n->type == SOIE_ELEMENT && it.n->u.el.content)
			todo_push(&t, it.n, it.depth + 1, 1);
	}
	free(t.items);
	if (len)
		*len = b.len;
	return b.s;
}
