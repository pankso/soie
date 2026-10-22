/*
 * Internal structures shared by the library sources. Never installed.
 */
#ifndef SOIE_INTERNAL_H
#define SOIE_INTERNAL_H

#include <stddef.h>
#include "soie/soie.h"

/* Growable byte buffer, always NUL-terminated once used. */
struct buf {
	char *s;
	size_t len, cap;
};

void buf_grow(struct buf *b, size_t extra);
void buf_add(struct buf *b, const char *s, size_t len);
void buf_putc(struct buf *b, int c);
void buf_put_utf8(struct buf *b, unsigned long cp);
void buf_reset(struct buf *b);
void buf_free(struct buf *b);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrndup(const char *s, size_t len);

/* Element name atoms, from tags.h. */
enum {
	TAG_UNKNOWN = 0,
#define TAG(id, name) TAG_##id,
#include "tags.h"
#undef TAG
	TAG_COUNT
};

int tag_lookup(const char *name, size_t len);
const char *tag_name(int tag);

struct attr {
	char *name;
	char *value;
	unsigned char ns;
};

struct style;
struct styledb;
struct layout;

struct soie_node {
	unsigned char type;
	unsigned char ns;
	unsigned char name_owned;
	/* Element created by the parser as an HTML integration point
	 * (MathML annotation-xml with an HTML encoding). */
	unsigned char html_ip;
	unsigned short tag;
	struct soie_node *doc;
	struct soie_node *parent, *first, *last, *prev, *next;
	const char *name;	/* element local name, doctype name */
	union {
		struct {
			struct attr *attrs;
			unsigned nattrs, cap;
			struct soie_node *content;	/* <template> */
			const struct style *style;	/* computed */
			const struct style *before, *after;
		} el;
		struct {
			char *data;
			size_t len, cap;
		} text;
		struct {
			char *public_id, *system_id;
		} doctype;
		struct {
			unsigned char quirks;
			const char *charset;
			struct styledb *styles;
			struct layout *layout;
		} doc;
	} u;
};

/* Raw constructors and helpers used by the parser. */
struct soie_node *node_new(struct soie_node *doc, int type);
struct soie_node *element_new_tag(struct soie_node *doc, int ns, int tag,
	const char *name, size_t len);
void element_add_attr(struct soie_node *el, int ns, const char *name,
	const char *value, size_t vlen);
struct attr *element_find_attr(const struct soie_node *el, const char *name);
void text_append(struct soie_node *t, const char *s, size_t len);
void node_insert(struct soie_node *parent, struct soie_node *child,
	struct soie_node *ref);
void node_detach(struct soie_node *n);

static inline int is_html(const struct soie_node *n, int tag)
{
	return n->type == SOIE_ELEMENT && n->ns == SOIE_NS_HTML &&
		n->tag == tag;
}

void styledb_free(struct styledb *db);
void layout_free(struct layout *l);
void fonts_drop_document(const void *doc);

/* Encoding: decode bytes to UTF-8 with normalized newlines. */
const char *charset_sniff(const unsigned char *s, size_t len,
	const char *hint, size_t *bom);
void charset_decode(const char *charset, const unsigned char *s,
	size_t len, struct buf *out);

#endif
