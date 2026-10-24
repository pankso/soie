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
struct images;
struct script;
struct media;

struct soie_node {
	unsigned char type;
	unsigned char ns;
	unsigned char name_owned;
	/* Element created by the parser as an HTML integration point
	 * (MathML annotation-xml with an HTML encoding). */
	unsigned char html_ip;
	unsigned char started;	/* <script>: already started */
	unsigned char dirty;	/* its subtree needs restyling */
	unsigned short tag;
	/* when it or its subtree last changed what its boxes would be
	 * (layout_touch()); a layout built later keeps them */
	unsigned long lchg;
	struct soie_node *doc;
	struct soie_node *parent, *first, *last, *prev, *next;
	/* Every node of a document, attached or not, so that freeing the
	 * document frees nodes a script took out of the tree. */
	struct soie_node *anext, *aprev;
	void *user;		/* soie_node_set_user() */
	const char *name;	/* element local name, doctype name */
	union {
		struct {
			struct attr *attrs;
			unsigned nattrs, cap;
			struct soie_node *content;	/* <template> */
			const struct style *style;	/* computed */
			const struct style *before, *after;
			struct box *box;	/* its box in layout box_tree */
			unsigned long box_tree;
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
			struct images *images;
			struct soie_node *focus;	/* form control */
			int caret;
			struct soie_node *all;	/* anext list */
			unsigned long gen;	/* bumped by every change */
			/* what soie_document_update() redoes */
			unsigned long styled_gen;
			/* subtrees to restyle, or all of them */
			struct soie_node **dirty;
			int ndirty, capdirty, restyle_all;
			struct script *script;	/* soie_script_start() */
			struct media *media;	/* audio and video players */
			int lay_w, lay_h;
			/* layout_touch() stamps, and the last layout's */
			unsigned long lstamp, lbuilt;
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
/* n or something below it changed what boxes it gives (boxes.c) */
void layout_touch(struct soie_node *n);
/* Which style rules doc has: boxes of other rules are not kept */
unsigned long style_serial(const struct soie_node *doc);

static inline int is_html(const struct soie_node *n, int tag)
{
	return n->type == SOIE_ELEMENT && n->ns == SOIE_NS_HTML &&
		n->tag == tag;
}

void styledb_free(struct styledb *db);
void layout_free(struct layout *l);
void fonts_drop_document(const void *doc);
void images_free(struct images *im);
/* A change for incremental restyling (cascade.c) */
enum { CH_ATTR, CH_INSERT, CH_REMOVE };
void style_changed(soie_node *n, int kind);
/* CH_ATTR with the attribute's name: :has() looks at fewer changes */
void style_changed_attr(soie_node *n, int kind, const char *name,
	const char *old);
void style_forget(soie_node *n);

/* The options of the last cascade (player.c) */
const soie_style_options *style_saved_options(const soie_node *doc);
/* Players (player.c) */
void media_free(soie_node *doc);
void media_forget(soie_node *el);
void media_size(const soie_node *el, float *w, float *h);

/* The URL <base href> gives, else base; malloc()ed. */
char *document_base(soie_node *doc, const char *base);
unsigned char *ext_decode_image(const unsigned char *d, size_t len,
	int *w, int *h);

/* Encoding: decode bytes to UTF-8 with normalized newlines. */
const char *charset_sniff(const unsigned char *s, size_t len,
	const char *hint, size_t *bom);
void charset_decode(const char *charset, const unsigned char *s,
	size_t len, struct buf *out);

#endif
