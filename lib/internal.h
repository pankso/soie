/*
 * Internal structures shared by the library sources. Never installed.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
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
char *lowdup(const char *s);

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

/* What a document has besides its nodes */
struct docinfo {
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
	int anims;		/* elements with anim set */
	/* layout_touch() stamps, and the last layout's */
	unsigned long lstamp, lbuilt;
	struct damage *damage;	/* soie_layout_damage() */
	float zoom;		/* the style options', 1 */
	int shadows;		/* shadow roots made: 0, no shadow work */
	struct shadow_css *css;	/* adopted sheets of its shadow roots */
	struct shadow_css *sheets;	/* their <style> texts (cascade.c) */
	/* the custom states of its elements (soie_element_set_states()):
	 * few, found from the node's flag */
	struct cstates {
		struct soie_node *el;
		char *names;
	} *states;
	int nstates;
};

/* The text of a shadow root's adopted style sheets, or of its <style>
 * elements (each ended by a NUL, len in all), shared by the roots of a
 * document with the same text (every instance of a component): the
 * cascade parses it once, its rules have it as scope */
struct shadow_css {
	char *text;
	size_t len;
	unsigned refs;
	struct shadow_css *next;
};

/* list's entry of that text, held once more; let go of one */
struct shadow_css *css_hold(struct shadow_css **list, const char *text,
	size_t len);
void css_drop(struct shadow_css **list, struct shadow_css *c);

struct soie_node {
	unsigned char type;
	unsigned char ns;
	unsigned char name_owned;
	/* Element created by the parser as an HTML integration point
	 * (MathML annotation-xml with an HTML encoding). */
	unsigned char html_ip;
	unsigned char started;	/* <script>: already started */
	unsigned char dirty;	/* its subtree needs restyling */
	unsigned char vp;	/* its style has viewport units (vw...) */
	unsigned char sheet;	/* <link>: its style sheet came (1), failed (2) */
	unsigned char defined;	/* a custom element, upgraded: :defined */
	unsigned char states;	/* it has custom states: :state() */
	unsigned char anim;	/* bit 0: an animation of it began, for
				 * soie_animations() to tell scripts; the
				 * others: a hash of its animation-name */
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
			struct soie_node *shadow;	/* its shadow root */
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
		/* a document fragment: a shadow root has a host, and the
		 * CSS of its adopted style sheets, of its <style>
		 * elements when that is all it has (the cascade's) */
		struct {
			struct soie_node *host;
			struct shadow_css *css, *sheets;
			/* its <slot> elements in tree order, NULL ended,
			 * when slots_ok (slot_find()) */
			struct soie_node **slots;
			/* declared: made by the parser, not yet taken by
			 * a script (soie_shadow_claim()) */
			unsigned char closed, slots_ok, declared;
		} frag;
		/* a document's own (soie_document_new()), out of every
		 * node's union */
		struct docinfo *doc;
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
/* soie_layout_damage()'s tracking, freed with the document (damage.c) */
void damage_free(struct soie_node *doc);
/* Which style rules doc has: boxes of other rules are not kept */
unsigned long style_serial(const struct soie_node *doc);

static inline int is_html(const struct soie_node *n, int tag)
{
	return n->type == SOIE_ELEMENT && n->ns == SOIE_NS_HTML &&
		n->tag == tag;
}

/* The node above n: its parent, or its host when n is a shadow root */
static inline struct soie_node *node_above(const struct soie_node *n)
{
	return n->parent ? n->parent : n->type == SOIE_DOCUMENT_FRAGMENT ?
		n->u.frag.host : NULL;
}

/*
 * A walk of a subtree in tree order, shadow trees included: a host's
 * shadow root and its tree right after the host, before its children
 * (the style order). Without recursion: pages nest shadow trees as
 * deep as they like. scope: the shadow root of the tree of the node
 * walk_next() gave (NULL for the document's), as collect() wants it.
 */
struct walk {
	struct soie_node *n, *top;
	const struct soie_node *scope;
	const struct soie_node **outer;	/* the scopes of the hosts */
	size_t depth, cap;
};
/* top, the first node; scope: top's */
struct soie_node *walk_start(struct walk *w, struct soie_node *top,
	const struct soie_node *scope);
struct soie_node *walk_step(struct walk *w);
void walk_end(struct walk *w);

/* A child or a sibling here, the rest in walk_step(): every cascade
 * walks the whole page */
static inline struct soie_node *walk_next(struct walk *w)
{
	struct soie_node *n = w->n;

	if (n && (n->type != SOIE_ELEMENT || !n->u.el.shadow)) {
		if (n->first)
			return w->n = n->first;
		if (n != w->top && n->next)
			return w->n = n->next;
	}
	return walk_step(w);
}

/* Shadow trees (dom.c). The root of n's tree when it is a shadow
 * root, else NULL */
struct soie_node *shadow_scope(const struct soie_node *n);
/* What slot a light child of a host takes (its slot attribute, ""
 * for text); NULL for nodes no slot takes */
const char *slot_name(const struct soie_node *n);
/* The first <slot> of shadow root s by that name */
struct soie_node *slot_find(const struct soie_node *s, const char *name);
/* The slot light child n is assigned to, else NULL */
struct soie_node *slot_of(const struct soie_node *n);
/* May HTML element el have a shadow root (DOM attachShadow())? */
int shadow_host_ok(const struct soie_node *el);
/* The custom states of el, space separated (el->states set) */
const char *element_states(const struct soie_node *el);
/* n's parent in the flat tree: its slot for a light child of a host
 * (the host when it has none), the host for a shadow root's child */
struct soie_node *flat_parent(const struct soie_node *n);

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
