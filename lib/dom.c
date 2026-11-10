/*
 * DOM nodes: creation, navigation, attributes and tree mutation.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "soie/ext.h"

#define STR(x) #x
#define XSTR(x) STR(x)

const char *soie_version(void)
{
	return XSTR(SOIE_VERSION_MAJOR) "." XSTR(SOIE_VERSION_MINOR) "."
		XSTR(SOIE_VERSION_PATCH);
}

/* Memory */

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p) {
		fputs("soie: out of memory\n", stderr);
		abort();
	}
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p) {
		fputs("soie: out of memory\n", stderr);
		abort();
	}
	return p;
}

char *xstrndup(const char *s, size_t len)
{
	char *d = xmalloc(len + 1);

	memcpy(d, s, len);
	d[len] = '\0';
	return d;
}

/* A copy in ASCII lowercase */
char *lowdup(const char *s)
{
	size_t n = strlen(s), i;
	char *d = xstrndup(s, n);

	for (i = 0; i < n; i++)
		if (d[i] >= 'A' && d[i] <= 'Z')
			d[i] = (char)(d[i] + 32);
	return d;
}

/* Buffers */

void buf_grow(struct buf *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap)
		return;
	if (extra > (size_t)-1 / 2 - b->len) {
		fputs("soie: buffer too large\n", stderr);
		abort();
	}
	if (!b->cap)
		b->cap = 32;
	while (b->len + extra + 1 > b->cap)
		b->cap *= 2;
	b->s = xrealloc(b->s, b->cap);
}

void buf_add(struct buf *b, const char *s, size_t len)
{
	buf_grow(b, len);
	memcpy(b->s + b->len, s, len);
	b->len += len;
	b->s[b->len] = '\0';
}

void buf_putc(struct buf *b, int c)
{
	buf_grow(b, 1);
	b->s[b->len++] = (char)c;
	b->s[b->len] = '\0';
}

void buf_put_utf8(struct buf *b, unsigned long cp)
{
	char u[4];

	if (cp < 0x80) {
		buf_putc(b, (int)cp);
	} else if (cp < 0x800) {
		u[0] = (char)(0xc0 | cp >> 6);
		u[1] = (char)(0x80 | (cp & 0x3f));
		buf_add(b, u, 2);
	} else if (cp < 0x10000) {
		u[0] = (char)(0xe0 | cp >> 12);
		u[1] = (char)(0x80 | (cp >> 6 & 0x3f));
		u[2] = (char)(0x80 | (cp & 0x3f));
		buf_add(b, u, 3);
	} else {
		u[0] = (char)(0xf0 | cp >> 18);
		u[1] = (char)(0x80 | (cp >> 12 & 0x3f));
		u[2] = (char)(0x80 | (cp >> 6 & 0x3f));
		u[3] = (char)(0x80 | (cp & 0x3f));
		buf_add(b, u, 4);
	}
}

void buf_reset(struct buf *b)
{
	b->len = 0;
	if (b->s)
		b->s[0] = '\0';
}

void buf_free(struct buf *b)
{
	free(b->s);
	b->s = NULL;
	b->len = b->cap = 0;
}

/* Atoms */

static const char *const tag_names[] = {
	"",
#define TAG(id, name) name,
#include "tags.h"
#undef TAG
};

int tag_lookup(const char *name, size_t len)
{
	int lo = 1, hi = TAG_COUNT - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const char *t = tag_names[mid];
		int c = strncmp(name, t, len);

		if (c == 0 && t[len] != '\0')
			c = -1;
		if (c == 0)
			return mid;
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return TAG_UNKNOWN;
}

const char *tag_name(int tag)
{
	return tag > 0 && tag < TAG_COUNT ? tag_names[tag] : "";
}

/* Construction */

struct soie_node *node_new(struct soie_node *doc, int type)
{
	struct soie_node *n = xmalloc(sizeof(*n));

	memset(n, 0, sizeof(*n));
	n->type = (unsigned char)type;
	n->doc = doc ? doc : n;
	n->name = "";
	if (type == SOIE_DOCUMENT) {
		n->u.doc = xmalloc(sizeof(*n->u.doc));
		memset(n->u.doc, 0, sizeof(*n->u.doc));
	}
	if (doc) {
		n->anext = doc->u.doc->all;
		if (n->anext)
			n->anext->aprev = n;
		doc->u.doc->all = n;
	}
	return n;
}

soie_node *soie_document_new(void)
{
	soie_node *d = node_new(NULL, SOIE_DOCUMENT);

	d->u.doc->charset = "UTF-8";
	return d;
}

soie_node *soie_document_fragment_new(soie_node *doc)
{
	return node_new(doc, SOIE_DOCUMENT_FRAGMENT);
}

struct soie_node *element_new_tag(struct soie_node *doc, int ns, int tag,
	const char *name, size_t len)
{
	struct soie_node *n = node_new(doc, SOIE_ELEMENT);
	const char *atom = tag_name(tag);

	n->ns = (unsigned char)ns;
	n->tag = (unsigned short)tag;
	if (tag && strlen(atom) == len && !memcmp(atom, name, len)) {
		n->name = atom;
	} else {
		n->name = xstrndup(name, len);
		n->name_owned = 1;
	}
	if (ns == SOIE_NS_HTML && tag == TAG_TEMPLATE)
		n->u.el.content = soie_document_fragment_new(doc);
	return n;
}

soie_node *soie_element_new(soie_node *doc, soie_ns ns, const char *name)
{
	size_t len = strlen(name), i;
	char low[32];
	int tag = TAG_UNKNOWN;

	if (len < sizeof(low)) {
		for (i = 0; i < len; i++)
			low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ?
				name[i] + 32 : name[i]);
		tag = tag_lookup(low, len);
	}
	return element_new_tag(doc, ns, tag, name, len);
}

static soie_node *chardata_new(soie_node *doc, int type, const char *s,
	size_t len)
{
	soie_node *n = node_new(doc, type);

	text_append(n, s, len);
	return n;
}

soie_node *soie_text_new(soie_node *doc, const char *s, size_t len)
{
	return chardata_new(doc, SOIE_TEXT, s, len);
}

soie_node *soie_comment_new(soie_node *doc, const char *s, size_t len)
{
	return chardata_new(doc, SOIE_COMMENT, s, len);
}

void text_append(struct soie_node *t, const char *s, size_t len)
{
	if (t->u.text.len + len + 1 > t->u.text.cap) {
		size_t cap = t->u.text.cap ? t->u.text.cap : 16;

		while (t->u.text.len + len + 1 > cap)
			cap *= 2;
		t->u.text.data = xrealloc(t->u.text.data, cap);
		t->u.text.cap = cap;
	}
	memcpy(t->u.text.data + t->u.text.len, s, len);
	t->u.text.len += len;
	t->u.text.data[t->u.text.len] = '\0';
}

/* Navigation */

soie_node_type soie_node_get_type(const soie_node *n)
{
	return (soie_node_type)n->type;
}

soie_node *soie_node_document(const soie_node *n) { return n->doc; }
soie_node *soie_node_parent(const soie_node *n) { return n->parent; }
soie_node *soie_node_first_child(const soie_node *n) { return n->first; }
soie_node *soie_node_last_child(const soie_node *n) { return n->last; }
soie_node *soie_node_next_sibling(const soie_node *n) { return n->next; }
soie_node *soie_node_prev_sibling(const soie_node *n) { return n->prev; }

soie_quirks soie_document_quirks(const soie_node *doc)
{
	return doc->type == SOIE_DOCUMENT ?
		(soie_quirks)doc->u.doc->quirks : SOIE_NO_QUIRKS;
}

const char *soie_document_charset(const soie_node *doc)
{
	return doc->type == SOIE_DOCUMENT ? doc->u.doc->charset : "";
}

soie_node *soie_document_element(const soie_node *doc)
{
	soie_node *c;

	for (c = doc->first; c; c = c->next)
		if (c->type == SOIE_ELEMENT)
			return c;
	return NULL;
}

const char *soie_doctype_name(const soie_node *n)
{
	return n->type == SOIE_DOCTYPE ? n->name : "";
}

const char *soie_doctype_public_id(const soie_node *n)
{
	return n->type == SOIE_DOCTYPE && n->u.doctype.public_id ?
		n->u.doctype.public_id : "";
}

const char *soie_doctype_system_id(const soie_node *n)
{
	return n->type == SOIE_DOCTYPE && n->u.doctype.system_id ?
		n->u.doctype.system_id : "";
}

/* Elements */

const char *soie_element_name(const soie_node *n)
{
	return n->type == SOIE_ELEMENT ? n->name : "";
}

soie_ns soie_element_ns(const soie_node *n)
{
	return n->type == SOIE_ELEMENT ? (soie_ns)n->ns : SOIE_NS_NONE;
}

size_t soie_element_attr_count(const soie_node *n)
{
	return n->type == SOIE_ELEMENT ? n->u.el.nattrs : 0;
}

const char *soie_element_attr_name(const soie_node *n, size_t i)
{
	return i < soie_element_attr_count(n) ? n->u.el.attrs[i].name : NULL;
}

soie_ns soie_element_attr_ns(const soie_node *n, size_t i)
{
	return i < soie_element_attr_count(n) ?
		(soie_ns)n->u.el.attrs[i].ns : SOIE_NS_NONE;
}

const char *soie_element_attr_value(const soie_node *n, size_t i)
{
	return i < soie_element_attr_count(n) ? n->u.el.attrs[i].value : NULL;
}

struct attr *element_find_attr(const struct soie_node *el, const char *name)
{
	unsigned i;

	if (el->type != SOIE_ELEMENT)
		return NULL;
	for (i = 0; i < el->u.el.nattrs; i++)
		if (el->u.el.attrs[i].ns == SOIE_NS_NONE &&
		    !strcmp(el->u.el.attrs[i].name, name))
			return &el->u.el.attrs[i];
	return NULL;
}

void element_add_attr(struct soie_node *el, int ns, const char *name,
	const char *value, size_t vlen)
{
	struct attr *a;

	if (el->u.el.nattrs == el->u.el.cap) {
		el->u.el.cap = el->u.el.cap ? el->u.el.cap * 2 : 4;
		el->u.el.attrs = xrealloc(el->u.el.attrs,
			el->u.el.cap * sizeof(*a));
	}
	a = &el->u.el.attrs[el->u.el.nattrs++];
	a->name = xstrndup(name, strlen(name));
	a->value = xstrndup(value, vlen);
	a->ns = (unsigned char)ns;
}

const char *soie_element_get_attr(const soie_node *n, const char *name)
{
	struct attr *a = element_find_attr(n, name);

	return a ? a->value : NULL;
}

static void slot_attr(soie_node *n, const char *name);

int soie_element_set_attr(soie_node *n, const char *name, const char *value)
{
	struct attr *a;

	if (n->type != SOIE_ELEMENT)
		return -1;
	a = element_find_attr(n, name);
	if (a && !strcmp(a->value, value))
		return 0;	/* the same: nothing to restyle or relay */
	layout_touch(n);
	if (a) {
		char *old = a->value;

		a->value = xstrndup(value, strlen(value));
		n->doc->u.doc->gen++;
		style_changed_attr(n, CH_ATTR, name, old);
		slot_attr(n, name);
		free(old);
		return 0;
	}
	element_add_attr(n, SOIE_NS_NONE, name, value, strlen(value));
	n->doc->u.doc->gen++;
	style_changed_attr(n, CH_ATTR, name, NULL);
	slot_attr(n, name);
	return 0;
}

int soie_element_remove_attr(soie_node *n, const char *name)
{
	struct attr *a = element_find_attr(n, name);
	char *aname, *old;
	size_t i;

	if (!a)
		return -1;
	i = (size_t)(a - n->u.el.attrs);
	aname = a->name;	/* name may be it: freed after */
	old = a->value;
	memmove(a, a + 1, (n->u.el.nattrs - i - 1) * sizeof(*a));
	n->u.el.nattrs--;
	n->doc->u.doc->gen++;
	layout_touch(n);
	style_changed_attr(n, CH_ATTR, aname, old);
	slot_attr(n, aname);
	free(aname);
	free(old);
	return 0;
}

soie_node *soie_template_content(const soie_node *n)
{
	return n->type == SOIE_ELEMENT ? n->u.el.content : NULL;
}

/* Shadow trees */

soie_node *soie_attach_shadow(soie_node *el, int open)
{
	soie_node *r;

	if (el->type != SOIE_ELEMENT || el->u.el.shadow)
		return NULL;
	r = soie_document_fragment_new(el->doc);
	r->u.frag.host = el;
	r->u.frag.closed = !open;
	el->u.el.shadow = r;
	el->doc->u.doc->shadows++;
	el->doc->u.doc->gen++;
	/* its light children leave the boxes, its tree is styled */
	layout_touch(el);
	style_changed(el, CH_ATTR);
	return r;
}

soie_node *soie_shadow_root(const soie_node *el)
{
	return el->type == SOIE_ELEMENT ? el->u.el.shadow : NULL;
}

soie_node *soie_shadow_host(const soie_node *n)
{
	return n->type == SOIE_DOCUMENT_FRAGMENT ? n->u.frag.host : NULL;
}

int soie_shadow_open(const soie_node *root)
{
	return root->type == SOIE_DOCUMENT_FRAGMENT && root->u.frag.host &&
		!root->u.frag.closed;
}

/* doc's shared text css, held once more */
static struct shadow_css *css_hold(soie_node *doc, const char *css)
{
	struct shadow_css **p, *c;

	for (p = &doc->u.doc->css; (c = *p); p = &c->next)
		if (!strcmp(c->text, css)) {
			c->refs++;
			return c;
		}
	c = xmalloc(sizeof(*c));
	c->text = xstrndup(css, strlen(css));
	c->refs = 1;
	c->next = NULL;
	*p = c;
	return c;
}

static void css_drop(soie_node *doc, struct shadow_css *c)
{
	struct shadow_css **p;

	if (!c || --c->refs)
		return;
	for (p = &doc->u.doc->css; *p != c; p = &(*p)->next)
		;
	*p = c->next;
	free(c->text);
	free(c);
}

void soie_shadow_css(soie_node *root, const char *css)
{
	struct shadow_css *old;

	if (!soie_shadow_host(root))
		return;
	old = root->u.frag.css;
	root->u.frag.css = css && *css ? css_hold(root->doc, css) : NULL;
	css_drop(root->doc, old);
	/* other rules if a new text (sheet_signature() sees it), other
	 * rules for its tree anyway */
	root->doc->u.doc->gen++;
	style_changed(root->u.frag.host, CH_ATTR);
}

int shadow_host_ok(const struct soie_node *el)
{
	static const char *const reserved[] = { "annotation-xml",
		"color-profile", "font-face", "font-face-src",
		"font-face-uri", "font-face-format", "font-face-name",
		"missing-glyph", NULL };
	const char *s;
	int i;

	if (el->type != SOIE_ELEMENT || el->ns != SOIE_NS_HTML)
		return 0;
	switch (el->tag) {
	case TAG_ARTICLE: case TAG_ASIDE: case TAG_BLOCKQUOTE: case TAG_BODY:
	case TAG_DIV: case TAG_FOOTER: case TAG_H1: case TAG_H2: case TAG_H3:
	case TAG_H4: case TAG_H5: case TAG_H6: case TAG_HEADER: case TAG_MAIN:
	case TAG_NAV: case TAG_P: case TAG_SECTION: case TAG_SPAN:
		return 1;
	}
	/* a valid custom element name */
	s = el->name;
	if (*s < 'a' || *s > 'z' || !strchr(s, '-'))
		return 0;
	for (; *s; s++)
		if (*s >= 'A' && *s <= 'Z')
			return 0;
	for (i = 0; reserved[i]; i++)
		if (!strcmp(el->name, reserved[i]))
			return 0;
	return 1;
}

soie_node *soie_assigned_slot(const soie_node *n)
{
	return n->doc->u.doc->shadows ? slot_of(n) : NULL;
}

struct soie_node *shadow_scope(const struct soie_node *n)
{
	while (n->parent)
		n = n->parent;
	return n->type == SOIE_DOCUMENT_FRAGMENT && n->u.frag.host ?
		(struct soie_node *)n : NULL;
}

const char *slot_name(const struct soie_node *n)
{
	const char *s;

	if (n->type == SOIE_TEXT)
		return "";
	if (n->type != SOIE_ELEMENT)
		return NULL;
	return (s = soie_element_get_attr(n, "slot")) ? s : "";
}

/* The slots of shadow root s, in tree order, NULL ended: kept on s
 * until one comes, goes or changes its name (slots_moved()), not
 * looked for in the whole tree at each light child */
static struct soie_node **slots_of(const struct soie_node *s)
{
	struct soie_node *r = (struct soie_node *)s, *n = r->first;
	size_t k = 0, cap = 0;

	if (r->u.frag.slots_ok)
		return r->u.frag.slots;
	free(r->u.frag.slots);
	r->u.frag.slots = NULL;
	while (n) {
		if (is_html(n, TAG_SLOT)) {
			if (k + 1 >= cap) {
				cap = cap ? cap * 2 : 4;
				r->u.frag.slots = xrealloc(r->u.frag.slots,
					cap * sizeof(*r->u.frag.slots));
			}
			r->u.frag.slots[k++] = n;
		}
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n != r && !n->next)
			n = n->parent;
		n = n == r ? NULL : n->next;
	}
	if (k)
		r->u.frag.slots[k] = NULL;
	r->u.frag.slots_ok = 1;
	return r->u.frag.slots;
}

struct soie_node *slot_find(const struct soie_node *s, const char *name)
{
	struct soie_node **v = slots_of(s);
	const char *a;

	for (; v && *v; v++)
		if (!strcmp((a = soie_element_get_attr(*v, "name")) ? a : "",
		    name))
			return *v;
	return NULL;
}

struct soie_node *slot_of(const struct soie_node *n)
{
	const struct soie_node *h = n->parent;
	const char *name;

	if (!h || h->type != SOIE_ELEMENT || !h->u.el.shadow ||
	    !(name = slot_name(n)))
		return NULL;
	return slot_find(h->u.el.shadow, name);
}

struct soie_node *flat_parent(const struct soie_node *n)
{
	struct soie_node *p = n->parent, *s;

	if (!p)
		return node_above(n);
	if (p->type == SOIE_ELEMENT && p->u.el.shadow)
		return (s = slot_of(n)) ? s : p;
	if (p->type == SOIE_DOCUMENT_FRAGMENT && p->u.frag.host)
		return p->u.frag.host;
	return p;
}

/* Which nodes the slots of host's shadow tree take changed: their
 * boxes are built again */
static void slots_touch(soie_node *host)
{
	soie_node **v = slots_of(host->u.el.shadow);

	for (; v && *v; v++)
		layout_touch(*v);
}

/* The slots of host's shadow tree changed: its children may go to
 * another one, inherit from it */
static void slots_moved(soie_node *host)
{
	soie_node *c;

	host->u.el.shadow->u.frag.slots_ok = 0;
	slots_touch(host);
	for (c = host->first; c; c = c->next)
		if (c->type == SOIE_ELEMENT)
			style_changed(c, CH_ATTR);
}

/* n comes into parent p or leaves it: a light child of a host, or a
 * slot (or a subtree with one) in a shadow tree, changes what slots
 * take */
static void slots_changed(soie_node *p, soie_node *n)
{
	soie_node *k = n, *s;

	if (p->type == SOIE_ELEMENT && p->u.el.shadow)
		slots_touch(p);
	if (n->type != SOIE_ELEMENT || !(s = shadow_scope(p)))
		return;
	while (k) {
		if (is_html(k, TAG_SLOT)) {
			slots_moved(s->u.frag.host);
			return;
		}
		if (k->first) {
			k = k->first;
			continue;
		}
		while (k != n && !k->next)
			k = k->parent;
		k = k == n ? NULL : k->next;
	}
}

/* An attribute of n changed: slot of a light child, name of a slot */
static void slot_attr(soie_node *n, const char *name)
{
	soie_node *s;

	if (!n->doc->u.doc->shadows)
		return;
	if (!strcmp(name, "slot") && n->parent &&
	    n->parent->type == SOIE_ELEMENT && n->parent->u.el.shadow)
		slots_touch(n->parent);
	else if (!strcmp(name, "name") && is_html(n, TAG_SLOT) &&
		 (s = shadow_scope(n)))
		slots_moved(s->u.frag.host);
}

/* Character data */

const char *soie_node_data(const soie_node *n, size_t *len)
{
	if (n->type != SOIE_TEXT && n->type != SOIE_COMMENT) {
		if (len)
			*len = 0;
		return NULL;
	}
	if (len)
		*len = n->u.text.len;
	return n->u.text.data ? n->u.text.data : "";
}

int soie_node_set_data(soie_node *n, const char *s, size_t len)
{
	if (n->type != SOIE_TEXT && n->type != SOIE_COMMENT)
		return -1;
	n->u.text.len = 0;
	text_append(n, s, len);
	n->doc->u.doc->gen++;
	layout_touch(n);
	if (n->type == SOIE_TEXT)
		style_changed(n, CH_INSERT);	/* :empty */
	return 0;
}

/* One node, without children. */
static soie_node *clone_one(const soie_node *n, soie_node *doc)
{
	soie_node *c;
	unsigned i;

	switch (n->type) {
	case SOIE_ELEMENT:
		c = element_new_tag(doc, n->ns, n->tag, n->name,
			strlen(n->name));
		c->html_ip = n->html_ip;
		for (i = 0; i < n->u.el.nattrs; i++)
			element_add_attr(c, n->u.el.attrs[i].ns,
				n->u.el.attrs[i].name, n->u.el.attrs[i].value,
				strlen(n->u.el.attrs[i].value));
		return c;
	case SOIE_TEXT:
	case SOIE_COMMENT:
		return chardata_new(doc, n->type, n->u.text.data,
			n->u.text.len);
	case SOIE_DOCTYPE:
		c = node_new(doc, SOIE_DOCTYPE);
		c->name = xstrndup(n->name, strlen(n->name));
		c->name_owned = 1;
		c->u.doctype.public_id = xstrndup(n->u.doctype.public_id,
			strlen(n->u.doctype.public_id));
		c->u.doctype.system_id = xstrndup(n->u.doctype.system_id,
			strlen(n->u.doctype.system_id));
		return c;
	case SOIE_DOCUMENT_FRAGMENT:
		return soie_document_fragment_new(doc);
	default:
		return NULL;
	}
}

soie_node *soie_node_clone(const soie_node *n, int deep)
{
	soie_node *root = clone_one(n, n->doc), *into = root, *c;
	const soie_node *s;

	if (!root || !deep)
		return root;
	if (n->type == SOIE_ELEMENT && n->u.el.content) {
		/* a <template>: its content, not its (absent) children */
		soie_node_free(root->u.el.content);
		root->u.el.content = soie_node_clone(n->u.el.content, 1);
	}
	for (s = n->first; s; ) {
		c = clone_one(s, n->doc);
		if (c) {
			node_insert(into, c, NULL);
			if (s->type == SOIE_ELEMENT && s->u.el.content) {
				soie_node_free(c->u.el.content);
				c->u.el.content =
					soie_node_clone(s->u.el.content, 1);
			}
		}
		if (s->first && c) {
			into = c;
			s = s->first;
			continue;
		}
		while (s != n && !s->next) {
			s = s->parent;
			into = into->parent;
		}
		s = s == n ? NULL : s->next;
	}
	return root;
}

/* One node from document from to document to: the list of all nodes,
 * the styles of from, a player. */
static void move_one(soie_node *n, soie_node *from, soie_node *to)
{
	if (n->dirty)
		style_forget(n);
	if (n->type == SOIE_ELEMENT) {
		if (from->u.doc->media)
			media_forget(n);
		n->u.el.style = n->u.el.before = n->u.el.after = NULL;
		if (from->u.doc->focus == n)
			from->u.doc->focus = NULL;
	}
	if (n->aprev)
		n->aprev->anext = n->anext;
	else
		from->u.doc->all = n->anext;
	if (n->anext)
		n->anext->aprev = n->aprev;
	n->aprev = NULL;
	n->anext = to->u.doc->all;
	if (n->anext)
		n->anext->aprev = n;
	to->u.doc->all = n;
	n->doc = to;
}

/* n's subtree, template contents and shadow trees included */
static void move_tree(soie_node *n, soie_node *from, soie_node *to)
{
	soie_node *s = n;
	struct shadow_css *c;

	while (s) {
		move_one(s, from, to);
		if (s->type == SOIE_ELEMENT && s->u.el.content)
			move_tree(s->u.el.content, from, to);
		if (s->type == SOIE_ELEMENT && s->u.el.shadow) {
			move_tree(s->u.el.shadow, from, to);
			to->u.doc->shadows++;
		}
		if (s->type == SOIE_DOCUMENT_FRAGMENT && s->u.frag.css) {
			/* to's own text */
			c = s->u.frag.css;
			s->u.frag.css = css_hold(to, c->text);
			css_drop(from, c);
		}
		if (s->first) {
			s = s->first;
			continue;
		}
		while (s != n && !s->next)
			s = s->parent;
		s = s == n ? NULL : s->next;
	}
}

int soie_node_adopt(soie_node *doc, soie_node *n)
{
	soie_node *from = n->doc;

	/* a shadow root goes with its host only */
	if (doc->type != SOIE_DOCUMENT || n->type == SOIE_DOCUMENT ||
	    soie_shadow_host(n))
		return -1;
	node_detach(n);
	if (from == doc)
		return 0;
	move_tree(n, from, doc);
	from->u.doc->gen++;
	doc->u.doc->gen++;
	return 0;
}

void soie_node_set_user(soie_node *n, void *p) { n->user = p; }
void *soie_node_user(const soie_node *n) { return n->user; }

unsigned long soie_document_generation(const soie_node *doc)
{
	return doc->doc->u.doc->gen;
}

/* Mutation */

void node_detach(struct soie_node *n)
{
	struct soie_node *p = n->parent;

	if (!p)
		return;
	style_changed(n, CH_REMOVE);
	layout_touch(p);
	if (n->prev)
		n->prev->next = n->next;
	else
		p->first = n->next;
	if (n->next)
		n->next->prev = n->prev;
	else
		p->last = n->prev;
	n->parent = n->prev = n->next = NULL;
	n->doc->u.doc->gen++;
	/* out: the slots kept (slots_of()) are those of the tree left */
	if (n->doc->u.doc->shadows)
		slots_changed(p, n);
}

/* Insert without checks; ref is NULL to append. */
void node_insert(struct soie_node *parent, struct soie_node *child,
	struct soie_node *ref)
{
	node_detach(child);
	child->parent = parent;
	child->next = ref;
	child->prev = ref ? ref->prev : parent->last;
	if (child->prev)
		child->prev->next = child;
	else
		parent->first = child;
	if (ref)
		ref->prev = child;
	else
		parent->last = child;
	parent->doc->u.doc->gen++;
	layout_touch(parent);
	if (parent->doc->u.doc->shadows)
		slots_changed(parent, child);
	style_changed(child, CH_INSERT);
}

int soie_node_insert_before(soie_node *parent, soie_node *child,
	soie_node *ref)
{
	soie_node *a;

	if (parent->type != SOIE_DOCUMENT &&
	    parent->type != SOIE_DOCUMENT_FRAGMENT &&
	    parent->type != SOIE_ELEMENT)
		return -1;
	if (child->type == SOIE_DOCUMENT || child->doc != parent->doc ||
	    (ref && ref->parent != parent) || child == ref)
		return -1;
	/* hosts too: a host in its own shadow tree is a cycle */
	for (a = parent; a; a = node_above(a))
		if (a == child)
			return -1;
	if (child->type == SOIE_DOCUMENT_FRAGMENT) {
		while (child->first)
			node_insert(parent, child->first, ref);
		return 0;
	}
	node_insert(parent, child, ref);
	return 0;
}

int soie_node_append(soie_node *parent, soie_node *child)
{
	return soie_node_insert_before(parent, child, NULL);
}

void soie_node_remove(soie_node *n)
{
	node_detach(n);
}

static void node_free_one(soie_node *n)
{
	soie_node *a;
	unsigned i;

	if (n->dirty)
		style_forget(n);
	switch (n->type) {
	case SOIE_ELEMENT:
		for (i = 0; i < n->u.el.nattrs; i++) {
			free(n->u.el.attrs[i].name);
			free(n->u.el.attrs[i].value);
		}
		free(n->u.el.attrs);
		if (n->u.el.content)
			soie_node_free(n->u.el.content);
		if (n->u.el.shadow) {
			/* kept if a script holds a node of it: no host then */
			n->u.el.shadow->u.frag.host = NULL;
			soie_node_free(n->u.el.shadow);
		}
		break;
	case SOIE_DOCUMENT_FRAGMENT:
		if (n->u.frag.host)
			n->u.frag.host->u.el.shadow = NULL;
		css_drop(n->doc, n->u.frag.css);
		free(n->u.frag.slots);
		break;
	case SOIE_TEXT:
	case SOIE_COMMENT:
		free(n->u.text.data);
		break;
	case SOIE_DOCTYPE:
		free(n->u.doctype.public_id);
		free(n->u.doctype.system_id);
		break;
	case SOIE_DOCUMENT:
		soie_script_stop(n);
		media_free(n);
		if (n->u.doc->layout)
			layout_free(n->u.doc->layout);
		damage_free(n);
		if (n->u.doc->styles)
			styledb_free(n->u.doc->styles);
		fonts_drop_document(n);
		if (n->u.doc->images)
			images_free(n->u.doc->images);
		free(n->u.doc->dirty);
		n->u.doc->dirty = NULL;
		n->u.doc->ndirty = 0;
		/* every node left, attached or not; template contents
		 * and shadow roots are on the list too */
		for (a = n->u.doc->all; a; a = a->anext)
			if (a->type == SOIE_ELEMENT)
				a->u.el.content = a->u.el.shadow = NULL;
			else if (a->type == SOIE_DOCUMENT_FRAGMENT)
				a->u.frag.host = NULL;
		while (n->u.doc->all)
			node_free_one(n->u.doc->all);
		break;
	default:
		break;
	}
	if (n->doc != n) {
		a = n->doc;
		if (n->aprev)
			n->aprev->anext = n->anext;
		else
			a->u.doc->all = n->anext;
		if (n->anext)
			n->anext->aprev = n->aprev;
	}
	if (n->name_owned)
		free((char *)n->name);
	if (n->type == SOIE_DOCUMENT)
		free(n->u.doc);
	free(n);
}

/* Has a node of n's subtree, shadow trees included, a user pointer? */
static int held(const soie_node *n)
{
	const soie_node *c;

	for (c = n; c; ) {
		if (c->user || (c->type == SOIE_ELEMENT && c->u.el.shadow &&
		    held(c->u.el.shadow)))
			return 1;
		if (c->first) {
			c = c->first;
			continue;
		}
		while (c != n && !c->next)
			c = c->parent;
		c = c == n ? NULL : c->next;
	}
	return 0;
}

/* Iterative, so that deeply nested trees do not blow the stack. A
 * subtree where a node has a user pointer (a script's wrapper) is only
 * taken out: the document frees it, after its host let go. */
void soie_node_free(soie_node *n)
{
	soie_node *c = n, *p;

	if (n->type != SOIE_DOCUMENT && held(n)) {
		node_detach(n);
		return;
	}

	/* a document's scripts let go of their wrappers before its nodes
	 * go: they look at every node they wrapped */
	if (n->type == SOIE_DOCUMENT) {
		soie_script_stop(n);
		/* nothing restyles: no walk up a deep tree per node */
		n->u.doc->restyle_all = 1;
	}
	node_detach(n);
	while (c) {
		while (c->first)
			c = c->first;
		p = c->parent;
		node_detach(c);
		node_free_one(c);
		c = p;
	}
}
