/*
 * The cascade: style sheet collection (<style>, <link>, @import),
 * layers, rule index, matching, custom properties, computed styles and
 * their sharing between elements.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "css.h"
#include "font.h"
#include "image.h"

/* Lines of lib/ua.css (C99 string literals are short). */
extern const char *const ua_css[];

enum { O_UA, O_USER, O_AUTHOR };

/* Interned strings, colors and styles, freed with the document. */

struct istr {
	struct istr *next;
	unsigned hash;
	size_t len;
	char s[1];
};

struct icolor {
	struct icolor *next;
	struct color c;
};

/*
 * Custom properties of an element: the ones it sets, sorted by name
 * pointer, over its parent's set. Interned, so shared when equal.
 */
struct vars {
	struct vars *next;
	unsigned hash;
	const struct vars *parent;
	int depth;
	int n;
	const char **name;	/* interned */
	const char **value;	/* interned text, NULL: removed */
};

static int vars_find(const struct vars *v, const char *name)
{
	int lo = 0, hi = v->n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (v->name[mid] == name)
			return mid;
		if ((size_t)v->name[mid] < (size_t)name)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* Value of name in v or its ancestors; *found tells unset apart. */
static const char *vars_get(const struct vars *v, const char *name,
	int *found)
{
	int i;

	for (; v; v = v->parent)
		if ((i = vars_find(v, name)) >= 0) {
			*found = 1;
			return v->value[i];
		}
	*found = 0;
	return NULL;
}

struct layer {
	char *name;
	struct layer *parent, *first, *last, *next;
	unsigned rank;
};

struct cdecl {
	char *name;
	char *value;
	int important;
};

struct srule {
	struct sellist *sel;
	struct svec decls;
	struct cdecl *custom;
	int ncustom;
	unsigned order;
	struct layer *layer;
	unsigned char origin;
};

struct entry {
	struct srule *r;
	short sel;
	unsigned char pseudo;
	unsigned char multi;	/* in several buckets: matched once */
	unsigned spec;
};

struct evec {
	struct entry *v;
	int n, cap;
};

struct bent {
	struct bent *next;
	char *key;
	struct evec e;
};

#define NBUCKETS 1024

struct bucket {
	struct bent *t[NBUCKETS];
};

struct reg {
	const char *name;
	const char *initial;	/* interned text, or NULL */
	int inherits;
};

struct styledb {
	struct env env;
	struct ucontext vp;		/* viewport units only */
	struct istr *strs[NBUCKETS];
	struct icolor *colors[256];
	struct style *styles[NBUCKETS];
	struct vars *varsets[256];
	struct calc **calcs;
	int ncalcs, capcalcs;
	struct srule **rules;
	int nrules, caprules;
	struct sellist **sels;
	int nsels, capsels;
	struct bucket ids, classes, tags;
	struct bucket attrs;	/* by attribute name, when nothing else */
	struct evec root;	/* :root and nothing else */
	struct evec universal;
	struct layer roots[3];
	unsigned order;
	int anon_layers;
	struct reg *regs;
	int nregs, noninherit;
	char **fontfaces;		/* @font-face blocks, for later */
	char **fontbases;		/* their sheets' URLs, for src */
	int nfontfaces;
	struct sval initial[NPROPS];
	struct {
		const struct style *parent, *style;
	} *anon;			/* anonymous box styles, by parent */
	int nanon;
	const soie_style_options *opts;
	const char *base;		/* document base URL */
	const char *doc_charset;
	int quirks;
	/* What the rules were built from, to reuse them on a restyle */
	unsigned sig;
	unsigned long serial;	/* which rules: boxes keep their styles */
	int sel_flags;		/* SF_*: what changes can reach */
	int incomplete;		/* a sheet or font comes later: */
	char **pending;		/* their URLs, asked again at a restyle */
	int npending;
	struct sellist **anchors;	/* selectors with a deep :has() */
	int nanchors, capanchors;
	float rem;		/* the root's font size */
	int vp_used;		/* a computed value has viewport units */
	soie_style_options saved;	/* a copy, for soie_document_update */
	/* The media queries the rules were chosen by, and their answers:
	 * another viewport keeps the rules when none changes */
	struct mq {
		struct cvlist q;
		int match;
	} *mqs;
	int nmqs, capmqs;
};

static unsigned hash_bytes(const void *p, size_t n, unsigned h)
{
	const unsigned char *s = p;

	while (n--)
		h = (h ^ *s++) * 16777619u;
	return h;
}

/* The same, 8 bytes at a time: for a whole struct style (3 KB, hashed
 * for each element) */
static unsigned hash_words(const void *p, size_t n, unsigned h)
{
	const unsigned char *s = p;
	uint64_t w, x = h;

	for (; n >= 8; n -= 8, s += 8) {
		memcpy(&w, s, 8);
		x = (x ^ w) * 0x100000001b3ULL;
		x ^= x >> 29;
	}
	return hash_bytes(s, n, (unsigned)(x ^ x >> 32));
}

const char *db_string(struct styledb *db, const char *s, size_t len)
{
	unsigned h = hash_bytes(s, len, 2166136261u);
	struct istr **b = &db->strs[h % NBUCKETS], *e;

	for (e = *b; e; e = e->next)
		if (e->hash == h && e->len == len && !memcmp(e->s, s, len))
			return e->s;
	e = xmalloc(sizeof(*e) + len);
	e->hash = h;
	e->len = len;
	memcpy(e->s, s, len);
	e->s[len] = '\0';
	e->next = *b;
	*b = e;
	return e->s;
}

const struct color *db_color(struct styledb *db, const struct color *c)
{
	struct color k = *c;
	unsigned h;
	struct icolor **b, *e;

	k.pad = 0;
	h = hash_bytes(&k, sizeof(k), 2166136261u);
	b = &db->colors[h % 256];
	for (e = *b; e; e = e->next)
		if (!memcmp(&e->c, &k, sizeof(k)))
			return &e->c;
	e = xmalloc(sizeof(*e));
	e->c = k;
	e->next = *b;
	*b = e;
	return &e->c;
}

struct calc *db_calc(struct styledb *db, struct calc *k)
{
	if (db->ncalcs == db->capcalcs) {
		db->capcalcs = db->capcalcs ? db->capcalcs * 2 : 16;
		db->calcs = xrealloc(db->calcs,
			(size_t)db->capcalcs * sizeof(*db->calcs));
	}
	db->calcs[db->ncalcs++] = k;
	return k;
}

static void layer_free(struct layer *l)
{
	struct layer *c = l->first, *n;

	for (; c; c = n) {
		n = c->next;
		layer_free(c);
		free(c);
	}
	free(l->name);
}

static void bucket_free(struct bucket *b)
{
	int i;
	struct bent *e, *n;

	for (i = 0; i < NBUCKETS; i++)
		for (e = b->t[i]; e; e = n) {
			n = e->next;
			free(e->key);
			free(e->e.v);
			free(e);
		}
}

static int media_check(struct styledb *db, const struct cvlist *l, int from,
	int to);

void styledb_free(struct styledb *db)
{
	int i, j;

	/* text widths and font chains are cached by style and family:
	 * these go with the rules */
	font_cache_clear();

	for (i = 0; i < db->nmqs; i++)
		cvlist_free(&db->mqs[i].q);
	free(db->mqs);
	for (i = 0; i < NBUCKETS; i++) {
		struct istr *s, *sn;
		struct style *st, *stn;

		for (s = db->strs[i]; s; s = sn) {
			sn = s->next;
			free(s);
		}
		for (st = db->styles[i]; st; st = stn) {
			stn = st->chain;
			free(st);
		}
	}
	for (i = 0; i < 256; i++) {
		struct icolor *c, *cn;
		struct vars *v, *vn;

		for (c = db->colors[i]; c; c = cn) {
			cn = c->next;
			free(c);
		}
		for (v = db->varsets[i]; v; v = vn) {
			vn = v->next;
			free(v->name);
			free(v->value);
			free(v);
		}
	}
	for (i = 0; i < db->npending; i++)
		free(db->pending[i]);
	free(db->pending);
	free((char *)db->saved.base_url);
	free((char *)db->saved.user_css);
	free(db->anchors);
	for (i = 0; i < db->ncalcs; i++)
		calc_release(db->calcs[i]);
	free(db->calcs);
	for (i = 0; i < db->nrules; i++) {
		struct srule *r = db->rules[i];

		svec_free(&r->decls);
		for (j = 0; j < r->ncustom; j++) {
			free(r->custom[j].name);
			free(r->custom[j].value);
		}
		free(r->custom);
		free(r);
	}
	free(db->rules);
	for (i = 0; i < db->nsels; i++)
		sellist_free(db->sels[i]);
	free(db->sels);
	bucket_free(&db->ids);
	bucket_free(&db->classes);
	bucket_free(&db->tags);
	bucket_free(&db->attrs);
	free(db->root.v);
	free(db->universal.v);
	for (i = 0; i < 3; i++)
		layer_free(&db->roots[i]);
	free(db->regs);
	for (i = 0; i < db->nfontfaces; i++) {
		free(db->fontfaces[i]);
		free(db->fontbases[i]);
	}
	free(db->fontfaces);
	free(db->fontbases);
	for (i = 0; i < NPROPS; i++)
		sval_free(&db->initial[i]);
	free(db->anon);
	free(db);
}

/* Layers */

static struct layer *layer_child(struct layer *p, const char *name,
	size_t len)
{
	struct layer *c;

	for (c = p->first; c; c = c->next)
		if (strlen(c->name) == len && !memcmp(c->name, name, len))
			return c;
	c = xmalloc(sizeof(*c));
	memset(c, 0, sizeof(*c));
	c->name = xstrndup(name, len);
	c->parent = p;
	if (p->last)
		p->last->next = c;
	else
		p->first = c;
	p->last = c;
	return c;
}

/* "a.b.c" below p */
static struct layer *layer_path(struct layer *p, const char *name)
{
	const char *dot;

	while ((dot = strchr(name, '.'))) {
		p = layer_child(p, name, (size_t)(dot - name));
		name = dot + 1;
	}
	return layer_child(p, name, strlen(name));
}

/* Post-order ranks: sub-layers before their parent; unlayered last. */
static void layer_rank(struct layer *l, unsigned *next)
{
	struct layer *c;

	for (c = l->first; c; c = c->next)
		layer_rank(c, next);
	l->rank = (*next)++;
}

/* Layer name from a prelude: idents joined by dots. */
static int layer_name(const struct cv *v, int n, int *i, struct buf *b)
{
	buf_reset(b);
	while (*i < n && v[*i].type == CT_WS)
		(*i)++;
	if (*i >= n || v[*i].type != CT_IDENT)
		return 0;
	for (;;) {
		buf_add(b, v[*i].str, strlen(v[*i].str));
		(*i)++;
		if (*i + 1 < n && v[*i].type == CT_DELIM && v[*i].delim == '.' &&
		    v[*i + 1].type == CT_IDENT) {
			buf_putc(b, '.');
			(*i)++;
			continue;
		}
		return 1;
	}
}

/* Rule index */

static void evec_add(struct evec *v, const struct entry *e)
{
	if (v->n == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 8;
		v->v = xrealloc(v->v, (size_t)v->cap * sizeof(*v->v));
	}
	v->v[v->n++] = *e;
}

static struct evec *bucket_get(struct bucket *b, const char *key, int add)
{
	unsigned h = hash_bytes(key, strlen(key), 2166136261u) % NBUCKETS;
	struct bent *e;

	for (e = b->t[h]; e; e = e->next)
		if (!strcmp(e->key, key))
			return &e->e;
	if (!add)
		return NULL;
	e = xmalloc(sizeof(*e));
	memset(e, 0, sizeof(*e));
	e->key = xstrndup(key, strlen(key));
	e->next = b->t[h];
	b->t[h] = e;
	return &e->e;
}

/*
 * The bucket of a compound and its key: its id, else a class, else its
 * tag, else an attribute name; else for :root the root's list. NULL
 * when it has none of them.
 */
static struct bucket *compound_key(struct styledb *db,
	const struct compound *c, const char **key, struct evec **root)
{
	struct bucket *b = NULL;
	const char *attr = NULL;
	int j, is_root = 0;

	for (j = 0; j < c->n; j++) {
		const struct simple *s = &c->s[j];

		if (s->type == SEL_ID) {
			*key = s->value;
			return &db->ids;
		}
		if (s->type == SEL_CLASS && b != &db->classes) {
			b = &db->classes;
			*key = s->value;
		} else if (s->type == SEL_TYPE && !b) {
			b = &db->tags;
			*key = s->name;
		} else if (s->type == SEL_ATTR && !attr) {
			attr = s->name;
		} else if (s->type == SEL_PSEUDO && s->pseudo == PC_ROOT) {
			is_root = 1;
		}
	}
	/* [data-x] alone: only for elements with that attribute */
	if (!b && attr) {
		b = &db->attrs;
		*key = attr;
	}
	if (!b && is_root)
		*root = &db->root;
	return b;
}

static void add_entry(struct styledb *db, struct bucket *b, const char *key,
	const struct entry *e)
{
	char *low = db->quirks && b != &db->tags && b != &db->attrs ?
		lowdup(key) : NULL;

	evec_add(bucket_get(b, low ? low : key, 1), e);
	free(low);
}

/* A compound with an :is() or :where() (GitHub's and YouTube's sheets
 * are full of them): in the buckets of its arguments, when each has
 * one */
static int index_is(struct styledb *db, const struct compound *c,
	struct entry *e)
{
	const struct sellist *a;
	struct evec *root = NULL;
	const char *key;
	int i, j;

	for (j = 0; j < c->n; j++) {
		if (c->s[j].type != SEL_PSEUDO || (c->s[j].pseudo != PC_IS &&
		    c->s[j].pseudo != PC_WHERE) || !(a = c->s[j].args) ||
		    !a->n || a->n > 16)
			continue;
		for (i = 0; i < a->n; i++)
			if (a->c[i].relative || !compound_key(db,
			    &a->c[i].c[a->c[i].n - 1], &key, &root))
				break;
		if (i < a->n)
			continue;
		e->multi = a->n > 1;
		for (i = 0; i < a->n; i++)
			add_entry(db, compound_key(db, &a->c[i].c[a->c[i].n - 1],
				&key, &root), key, e);
		return 1;
	}
	return 0;
}

/* A state the engine does not have (PC_FALSE): never matches */
static int never(const struct compound *c)
{
	int j;

	for (j = 0; j < c->n; j++)
		if (c->s[j].type == SEL_PSEUDO && c->s[j].pseudo == PC_FALSE)
			return 1;
	return 0;
}

static void index_rule(struct styledb *db, struct srule *r)
{
	int i;

	for (i = 0; i < r->sel->n; i++) {
		const struct complex *x = &r->sel->c[i];
		const struct compound *c = &x->c[x->n - 1];
		struct entry e;
		struct bucket *b;
		struct evec *root = NULL;
		const char *key = NULL;

		e.r = r;
		e.sel = (short)i;
		e.pseudo = (unsigned char)x->pseudo_element;
		e.multi = 0;
		e.spec = x->spec;
		if (never(c))
			continue;
		if ((b = compound_key(db, c, &key, &root)))
			add_entry(db, b, key, &e);
		else if (!index_is(db, c, &e))
			evec_add(root ? root : &db->universal, &e);
	}
}

/* Style sheet loading */

struct pctx {
	int origin;
	struct layer *layer;
	const struct sellist *parent;
	const char *base;	/* URL of the sheet */
	int depth;		/* @import nesting */
};

static void process_rules(struct styledb *db, struct crule **rules, int n,
	const struct pctx *pc);
static void load_fonts(struct styledb *db, const void *doc);

/* Make url() values absolute against the sheet's URL. */
static void resolve_urls(struct cv *v, int n, const char *base)
{
	int i;
	char *abs;

	for (i = 0; i < n; i++) {
		if (v[i].type == CT_URL && v[i].str) {
			abs = soie_url_resolve(base, v[i].str);
			free(v[i].str);
			v[i].str = abs;
		} else if (v[i].type == CT_FUNCTION &&
			   (ascii_ieq(v[i].str, "url") ||
			    ascii_ieq(v[i].str, "src"))) {
			int k;

			for (k = 0; k < v[i].kids.n; k++)
				if (v[i].kids.v[k].type == CT_STRING) {
					abs = soie_url_resolve(base,
						v[i].kids.v[k].str);
					free(v[i].kids.v[k].str);
					v[i].kids.v[k].str = abs;
					break;
				}
		} else if (v[i].type == CT_FUNCTION || v[i].type == CT_BLOCK) {
			resolve_urls(v[i].kids.v, v[i].kids.n, base);
		}
	}
}

static int has_url(const struct cv *v, int n)
{
	int i;

	/* url(a.png), url('a.png') (a function), src("a.woff") */
	for (i = 0; i < n; i++)
		if (v[i].type == CT_URL || (v[i].type == CT_FUNCTION &&
		    (ascii_ieq(v[i].str, "url") || ascii_ieq(v[i].str, "src"))) ||
		    ((v[i].type == CT_FUNCTION || v[i].type == CT_BLOCK) &&
		    has_url(v[i].kids.v, v[i].kids.n)))
			return 1;
	return 0;
}

static struct srule *new_rule(struct styledb *db, const struct sellist *sel,
	const struct pctx *pc)
{
	struct srule *r = xmalloc(sizeof(*r));

	memset(r, 0, sizeof(*r));
	r->sel = (struct sellist *)sel;
	r->order = db->order++;
	r->layer = pc->layer;
	r->origin = (unsigned char)pc->origin;
	return r;
}

/* Declarations of a block into rule r; returns how many it got. */
static int add_decls(struct srule *r, struct decl *d, int n, const char *base)
{
	int i, got = 0;

	for (i = 0; i < n; i++) {
		/* against the sheet that has them, custom properties too */
		if (base && has_url(d[i].value.v, d[i].value.n))
			resolve_urls(d[i].value.v, d[i].value.n, base);
		if (!strncmp(d[i].name, "--", 2)) {
			struct buf b = { 0 };

			css_serialize(&b, d[i].value.v, d[i].value.n);
			r->custom = xrealloc(r->custom, (size_t)(r->ncustom + 1)
				* sizeof(*r->custom));
			r->custom[r->ncustom].name = xstrndup(d[i].name,
				strlen(d[i].name));
			r->custom[r->ncustom].value = b.s;
			r->custom[r->ncustom].important = d[i].important;
			r->ncustom++;
			got++;
			continue;
		}
		got += parse_declaration(d[i].name, &d[i].value,
			d[i].important, &r->decls);
	}
	return got;
}

static void add_rule(struct styledb *db, struct srule *r)
{
	if (!r->decls.n && !r->ncustom) {
		svec_free(&r->decls);
		free(r);
		return;
	}
	if (db->nrules == db->caprules) {
		db->caprules = db->caprules ? db->caprules * 2 : 64;
		db->rules = xrealloc(db->rules,
			(size_t)db->caprules * sizeof(*db->rules));
	}
	db->rules[db->nrules++] = r;
	index_rule(db, r);
}

static void keep_sel(struct styledb *db, struct sellist *s)
{
	int f;

	if (db->nsels == db->capsels) {
		db->capsels = db->capsels ? db->capsels * 2 : 64;
		db->sels = xrealloc(db->sels,
			(size_t)db->capsels * sizeof(*db->sels));
	}
	db->sels[db->nsels++] = s;
	f = sellist_flags(s);
	db->sel_flags |= f;
	if (f & (SF_HAS << SF_DEEP)) {
		if (db->nanchors == db->capanchors) {
			db->capanchors = db->capanchors ?
				db->capanchors * 2 : 16;
			db->anchors = xrealloc(db->anchors,
				(size_t)db->capanchors * sizeof(*db->anchors));
		}
		db->anchors[db->nanchors++] = s;
	}
}

static void load_sheet(struct styledb *db, const char *css, size_t len,
	const struct pctx *pc)
{
	struct crule **rules;
	int n, i;

	rules = css_parse_stylesheet(css, len, &n);
	process_rules(db, rules, n, pc);
	for (i = 0; i < n; i++)
		crule_free(rules[i]);
	free(rules);
}

/* Bytes of a sheet to UTF-8: BOM, @charset, else the document's. */
static void decode_css(struct styledb *db, const char *s, size_t len,
	struct buf *out)
{
	const char *cs = db->doc_charset ? db->doc_charset : "UTF-8";
	size_t bom = 0;

	if (len >= 10 && !memcmp(s, "@charset \"", 10)) {
		const char *e = memchr(s + 10, '"', len - 10);

		if (e && e + 1 < s + len && e[1] == ';') {
			char label[32];
			size_t n = (size_t)(e - s - 10);

			if (n < sizeof(label)) {
				memcpy(label, s + 10, n);
				label[n] = '\0';
				cs = charset_sniff((const unsigned char *)"",
					0, label, &bom);
				if (!strncmp(cs, "UTF-16", 6))
					cs = "UTF-8";
			}
		}
	}
	if (len >= 3 && !memcmp(s, "\xef\xbb\xbf", 3)) {
		cs = "UTF-8";
		bom = 3;
	} else if (len >= 2 && (!memcmp(s, "\xfe\xff", 2) ||
				!memcmp(s, "\xff\xfe", 2))) {
		cs = s[0] == '\xfe' ? "UTF-16BE" : "UTF-16LE";
		bom = 2;
	} else {
		bom = 0;
	}
	charset_decode(cs, (const unsigned char *)s + bom, len - bom, out);
}

/* url comes later: the rules are built again when it is there */
static void pending_add(struct styledb *db, const char *url)
{
	db->incomplete = 1;
	db->pending = xrealloc(db->pending, (size_t)(db->npending + 1) *
		sizeof(*db->pending));
	db->pending[db->npending++] = xstrndup(url, strlen(url));
}

/* Has a sheet or font that was on its way come (or failed)? */
static int pending_changed(const struct styledb *db,
	const soie_style_options *o)
{
	char *buf;
	size_t len;
	int i, r;

	for (i = 0; i < db->npending; i++) {
		if (!o || !o->fetch)
			return 1;
		buf = NULL;
		r = o->fetch(o->fetch_data, db->pending[i], &buf, &len);
		free(buf);
		if (r != SOIE_FETCH_LATER)
			return 1;
	}
	return 0;
}

/* 0 when the sheet is in, SOIE_FETCH_LATER, -1 on failure */
static int fetch_sheet(struct styledb *db, const char *url,
	const struct pctx *pc)
{
	const soie_style_options *o = db->opts;
	struct pctx sub = *pc;
	struct buf text = { 0 };
	char *buf = NULL;
	size_t len = 0;
	int r;

	if (!o || !o->fetch || pc->depth > 16)
		return -1;
	r = o->fetch(o->fetch_data, url, &buf, &len);
	if (r == SOIE_FETCH_LATER)
		pending_add(db, url);
	if (r || !buf)
		return r ? r : -1;
	decode_css(db, buf, len, &text);
	free(buf);
	sub.base = url;
	sub.depth++;
	sub.parent = NULL;
	load_sheet(db, text.s, text.len, &sub);
	buf_free(&text);
	return 0;
}

static void at_import(struct styledb *db, struct crule *r,
	const struct pctx *pc)
{
	const struct cv *v = r->prelude.v;
	int n = r->prelude.n, i = 0;
	const char *href = NULL;
	struct pctx sub = *pc;
	struct buf name = { 0 };
	char *url;

	while (i < n && v[i].type == CT_WS)
		i++;
	if (i < n && (v[i].type == CT_STRING || v[i].type == CT_URL))
		href = v[i].str;
	else if (i < n && v[i].type == CT_FUNCTION &&
		 ascii_ieq(v[i].str, "url") && v[i].kids.n &&
		 v[i].kids.v[0].type == CT_STRING)
		href = v[i].kids.v[0].str;
	if (!href)
		return;
	for (i++; i < n && v[i].type == CT_WS; i++)
		;
	if (i < n && v[i].type == CT_IDENT && ascii_ieq(v[i].str, "layer")) {
		char anon[24];

		snprintf(anon, sizeof(anon), "\x01%d", db->anon_layers++);
		sub.layer = layer_path(pc->layer, anon);
		i++;
	} else if (i < n && v[i].type == CT_FUNCTION &&
		   ascii_ieq(v[i].str, "layer")) {
		int k = 0;

		if (layer_name(v[i].kids.v, v[i].kids.n, &k, &name))
			sub.layer = layer_path(pc->layer, name.s);
		i++;
	}
	buf_free(&name);
	for (; i < n && v[i].type == CT_WS; i++)
		;
	if (i < n && v[i].type == CT_FUNCTION &&
	    ascii_ieq(v[i].str, "supports")) {
		if (!supports_match(&v[i].kids, 0, v[i].kids.n))
			return;
		i++;
	}
	if (!media_check(db, &r->prelude, i, n))
		return;
	url = soie_url_resolve(pc->base, href);
	fetch_sheet(db, url, &sub);
	free(url);
}

static void at_property(struct styledb *db, struct crule *r)
{
	struct crule tmp;
	const struct cv *v = r->prelude.v;
	int i, n = r->prelude.n;
	struct reg g;

	for (i = 0; i < n && v[i].type == CT_WS; i++)
		;
	if (i >= n || v[i].type != CT_IDENT || strncmp(v[i].str, "--", 2) ||
	    !r->has_block)
		return;
	memset(&g, 0, sizeof(g));
	g.name = db_string(db, v[i].str, strlen(v[i].str));
	g.inherits = 1;
	memset(&tmp, 0, sizeof(tmp));
	css_parse_block(&r->block, &tmp, 1);
	for (i = 0; i < tmp.ndecls; i++) {
		struct decl *d = &tmp.decls[i];

		if (ascii_ieq(d->name, "inherits")) {
			g.inherits = d->value.n && d->value.v[0].type ==
				CT_IDENT && ascii_ieq(d->value.v[0].str, "true");
		} else if (ascii_ieq(d->name, "initial-value")) {
			struct buf b = { 0 };

			css_serialize(&b, d->value.v, d->value.n);
			g.initial = db_string(db, b.s, b.len);
			buf_free(&b);
		}
	}
	for (i = 0; i < tmp.ndecls; i++) {
		free(tmp.decls[i].name);
		cvlist_free(&tmp.decls[i].value);
	}
	free(tmp.decls);
	for (i = 0; i < tmp.nrules; i++)
		crule_free(tmp.rules[i]);
	free(tmp.rules);
	db->regs = xrealloc(db->regs, (size_t)(db->nregs + 1) *
		sizeof(*db->regs));
	db->regs[db->nregs++] = g;
	if (!g.inherits)
		db->noninherit = 1;
}

/* @media, @supports, @layer with a block: contents in context. */
static void conditional_block(struct styledb *db, struct crule *r,
	const struct pctx *pc)
{
	struct crule tmp;
	int i;

	memset(&tmp, 0, sizeof(tmp));
	css_parse_block(&r->block, &tmp, 1);
	if (pc->parent && tmp.ndecls) {
		/* nested: declarations apply to the parent selector */
		struct srule *nr = new_rule(db, pc->parent, pc);

		add_decls(nr, tmp.decls, tmp.ndecls, pc->base);
		add_rule(db, nr);
	}
	process_rules(db, tmp.rules, tmp.nrules, pc);
	for (i = 0; i < tmp.ndecls; i++) {
		free(tmp.decls[i].name);
		cvlist_free(&tmp.decls[i].value);
	}
	free(tmp.decls);
	for (i = 0; i < tmp.nrules; i++)
		crule_free(tmp.rules[i]);
	free(tmp.rules);
}

static void at_rule(struct styledb *db, struct crule *r,
	const struct pctx *pc)
{
	const char *n = r->name;
	struct pctx sub = *pc;

	if (ascii_ieq(n, "media")) {
		if (r->has_block && media_check(db, &r->prelude, 0,
						r->prelude.n))
			conditional_block(db, r, pc);
	} else if (ascii_ieq(n, "supports")) {
		if (r->has_block && supports_match(&r->prelude, 0,
						   r->prelude.n))
			conditional_block(db, r, pc);
	} else if (ascii_ieq(n, "layer")) {
		struct buf name = { 0 };
		int i = 0;

		if (!r->has_block) {
			/* @layer a, b; only declares the order */
			while (layer_name(r->prelude.v, r->prelude.n, &i, &name)) {
				layer_path(pc->layer, name.s);
				while (i < r->prelude.n &&
				       r->prelude.v[i].type != CT_COMMA)
					i++;
				i++;
			}
		} else {
			if (layer_name(r->prelude.v, r->prelude.n, &i, &name)) {
				sub.layer = layer_path(pc->layer, name.s);
			} else {
				char anon[24];

				snprintf(anon, sizeof(anon), "\x01%d",
					db->anon_layers++);
				sub.layer = layer_path(pc->layer, anon);
			}
			conditional_block(db, r, &sub);
		}
		buf_free(&name);
	} else if (ascii_ieq(n, "import")) {
		if (!pc->parent)
			at_import(db, r, pc);
	} else if (ascii_ieq(n, "property")) {
		at_property(db, r);
	} else if (ascii_ieq(n, "font-face") && r->has_block) {
		struct buf b = { 0 };

		css_serialize(&b, r->block.v, r->block.n);
		db->fontfaces = xrealloc(db->fontfaces,
			(size_t)(db->nfontfaces + 1) * sizeof(*db->fontfaces));
		db->fontbases = xrealloc(db->fontbases,
			(size_t)(db->nfontfaces + 1) * sizeof(*db->fontbases));
		db->fontbases[db->nfontfaces] = pc->base ?
			xstrndup(pc->base, strlen(pc->base)) : NULL;
		db->fontfaces[db->nfontfaces++] = b.s;
	}
	/* @container, @scope, @keyframes, @page...: not supported yet */
}

static void process_rules(struct styledb *db, struct crule **rules, int n,
	const struct pctx *pc)
{
	int i;

	for (i = 0; i < n; i++) {
		struct crule *r = rules[i];
		struct sellist *sel;
		struct srule *sr;
		struct pctx sub;

		if (r->type == R_AT) {
			at_rule(db, r, pc);
			continue;
		}
		if (r->nested_decls) {
			if (!pc->parent)
				continue;
			sr = new_rule(db, pc->parent, pc);
			add_decls(sr, r->decls, r->ndecls, pc->base);
			add_rule(db, sr);
			continue;
		}
		sel = selector_parse(&r->prelude, 0, r->prelude.n, pc->parent,
			0);
		if (!sel)
			continue;
		keep_sel(db, sel);
		sr = new_rule(db, sel, pc);
		add_decls(sr, r->decls, r->ndecls, pc->base);
		add_rule(db, sr);
		if (r->nrules) {
			sub = *pc;
			sub.parent = sel;
			process_rules(db, r->rules, r->nrules, &sub);
		}
	}
}

/* Text content of an element (the <style> sheet). */
static void text_content(const soie_node *el, struct buf *b)
{
	const soie_node *c;

	for (c = el->first; c; c = c->next)
		if (c->type == SOIE_TEXT)
			buf_add(b, c->u.text.data, c->u.text.len);
}

static void cv_copy(struct cv *d, const struct cv *s)
{
	int i;

	*d = *s;
	d->str = s->str ? xstrndup(s->str, strlen(s->str)) : NULL;
	d->repr = s->repr ? xstrndup(s->repr, strlen(s->repr)) : NULL;
	memset(&d->kids, 0, sizeof(d->kids));
	for (i = 0; i < s->kids.n; i++) {
		struct cv c;

		cv_copy(&c, &s->kids.v[i]);
		cvlist_add(&d->kids, &c);
	}
}

/* media_match(), the query and its answer kept */
static int media_check(struct styledb *db, const struct cvlist *l, int from,
	int to)
{
	struct mq *m;
	int i;

	if (db->nmqs == db->capmqs) {
		db->capmqs = db->capmqs ? db->capmqs * 2 : 16;
		db->mqs = xrealloc(db->mqs, (size_t)db->capmqs *
			sizeof(*db->mqs));
	}
	m = &db->mqs[db->nmqs++];
	memset(m, 0, sizeof(*m));
	for (i = from; i < to; i++) {
		struct cv c;

		cv_copy(&c, &l->v[i]);
		cvlist_add(&m->q, &c);
	}
	m->match = media_match(&m->q, 0, m->q.n, &db->env);
	return m->match;
}

/* The environment of opts; 1 when the rules of db hold in it too. */
static int env_of(const struct styledb *db, const soie_style_options *opts,
	struct env *e)
{
	int i;

	float z = opts && opts->zoom > 0 ? opts->zoom : 1;

	/* media queries see CSS px: the viewport zoomed */
	e->width = (opts && opts->width > 0 ? (float)opts->width : 1024) / z;
	e->height = (opts && opts->height > 0 ? (float)opts->height : 768) / z;
	e->dark = opts ? opts->dark : 0;
	e->print = opts ? opts->print : 0;
	e->scripting = opts ? opts->scripting : 0;
	if (!db)
		return 0;
	for (i = 0; i < db->nmqs; i++)
		if (media_match(&db->mqs[i].q, 0, db->mqs[i].q.n, e) !=
		    db->mqs[i].match)
			return 0;
	return 1;
}

static int media_attr(struct styledb *db, const soie_node *el)
{
	const char *m = soie_element_get_attr(el, "media");
	struct cvlist l;
	int r;

	if (!m)
		return 1;
	css_component_values(m, strlen(m), &l);
	r = media_check(db, &l, 0, l.n);
	cvlist_free(&l);
	return r;
}

static int svg_style(const soie_node *n)
{
	return n->type == SOIE_ELEMENT && n->ns == SOIE_NS_SVG &&
		!strcmp(n->name, "style");
}

static void collect_sheets(struct styledb *db, soie_node *doc,
	const char *base)
{
	soie_node *n = doc->first;
	struct pctx pc;

	memset(&pc, 0, sizeof(pc));
	pc.origin = O_AUTHOR;
	pc.layer = &db->roots[O_AUTHOR];
	pc.base = base;
	while (n) {
		/* an SVG's <style> is a sheet of the page as well */
		if (n->type == SOIE_ELEMENT && (n->ns == SOIE_NS_HTML ||
		    svg_style(n))) {
			if ((n->tag == TAG_STYLE || n->ns == SOIE_NS_SVG) &&
			    media_attr(db, n)) {
				const char *t = soie_element_get_attr(n, "type");
				struct buf b = { 0 };

				if (!t || !*t || ascii_ieq(t, "text/css")) {
					text_content(n, &b);
					buf_add(&b, "", 0);
					load_sheet(db, b.s, b.len, &pc);
				}
				buf_free(&b);
			} else if (n->tag == TAG_LINK) {
				const char *rel = soie_element_get_attr(n, "rel");
				const char *href = soie_element_get_attr(n,
					"href");

				if (href && *href && rel && word_in(rel,
				    "stylesheet", 1) && !word_in(rel,
				    "alternate", 1) && !soie_element_get_attr(n,
				    "disabled") && media_attr(db, n)) {
					char *url = soie_url_resolve(base, href);
					int r = fetch_sheet(db, url, &pc);

					n->sheet = r == SOIE_FETCH_LATER ? 0 :
						r ? 2 : 1;
					free(url);
				} else {
					n->sheet = 1;	/* nothing to wait for */
				}
			}
		}
		/* tree order, skipping template contents */
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n && !n->next)
			n = n->parent;
		if (n)
			n = n->next;
	}
}

/* Presentational hints (HTML 15.3), as declarations. */

static void hint(struct svec *out, const char *prop, const char *value)
{
	struct cvlist l;

	css_component_values(value, strlen(value), &l);
	parse_declaration(prop, &l, 0, out);
	cvlist_free(&l);
}

/* "rules for parsing dimension values": 100 -> 100px, 50% -> 50% */
static int dimension(const char *s, char *out, size_t size, int pct)
{
	const char *p = s;
	size_t n;

	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	s = p;
	while (*p >= '0' && *p <= '9')
		p++;
	if (p == s)
		return 0;
	if (*p == '.' && p[1] >= '0' && p[1] <= '9')
		for (p++; *p >= '0' && *p <= '9'; p++)
			;
	n = (size_t)(p - s);
	if (n + 3 > size)
		return 0;
	memcpy(out, s, n);
	if (*p == '%' && pct)
		strcpy(out + n, "%");
	else
		strcpy(out + n, "px");
	return 1;
}

static void legacy_color(struct svec *out, const char *prop, const char *v)
{
	char buf[16];
	size_t n = strlen(v), i;
	struct color c;

	if (!n)
		return;
	if (named_color(v, &c) || v[0] == '#') {
		hint(out, prop, v);
		return;
	}
	for (i = 0; i < n && i < 6; i++)
		if (!((v[i] >= '0' && v[i] <= '9') ||
		      ((v[i] | 32) >= 'a' && (v[i] | 32) <= 'f')))
			return;
	if (n != 3 && n != 6)
		return;
	buf[0] = '#';
	memcpy(buf + 1, v, n);
	buf[n + 1] = '\0';
	hint(out, prop, buf);
}

static const soie_node *ancestor_table(const soie_node *el)
{
	for (el = el->parent; el && el->type == SOIE_ELEMENT; el = el->parent)
		if (is_html(el, TAG_TABLE))
			return el;
	return NULL;
}

/* SVG presentation attributes (SVG 2, 6.6): hints as well. A bare
 * number is a length in user units, px here. */
static void svg_hints(const soie_node *el, struct svec *out)
{
	static const char *const names[] = { "fill", "stroke",
		"stroke-width", "stroke-linejoin", "stroke-linecap",
		"stroke-miterlimit", "fill-rule", "fill-opacity",
		"stroke-opacity", "opacity", "display", "visibility", "color",
		"font-family", "font-weight", "font-style", "text-anchor",
		NULL };
	const char *a;
	char d[48];
	int i;

	for (i = 0; names[i]; i++)
		if ((a = soie_element_get_attr(el, names[i])) && *a)
			hint(out, names[i], a);
	if ((a = soie_element_get_attr(el, "font-size")) && *a) {
		const char *e = a + strlen(a) - 1;

		if (((*e >= '0' && *e <= '9') || *e == '.') &&
		    dimension(a, d, sizeof(d), 0))
			hint(out, "font-size", d);
		else
			hint(out, "font-size", a);
	}
}

static void hints(const soie_node *el, struct svec *out)
{
	const char *a;
	char d[48];
	int tag = el->ns == SOIE_NS_HTML ? el->tag : -1;

	if (el->ns == SOIE_NS_SVG) {
		svg_hints(el, out);
		return;
	}

	if (tag < 0)
		return;
	if ((a = soie_element_get_attr(el, "bgcolor")) &&
	    (tag == TAG_BODY || tag == TAG_TABLE || tag == TAG_TR ||
	     tag == TAG_TD || tag == TAG_TH || tag == TAG_THEAD ||
	     tag == TAG_TBODY || tag == TAG_TFOOT))
		legacy_color(out, "background-color", a);
	if (tag == TAG_BODY && (a = soie_element_get_attr(el, "text")))
		legacy_color(out, "color", a);
	if (tag == TAG_FONT) {
		if ((a = soie_element_get_attr(el, "color")))
			legacy_color(out, "color", a);
		if ((a = soie_element_get_attr(el, "face")))
			hint(out, "font-family", a);
		if ((a = soie_element_get_attr(el, "size"))) {
			static const char *const sizes[] = { "x-small",
				"small", "medium", "large", "x-large",
				"xx-large", "xxx-large" };
			int rel = *a == '+' || *a == '-', v = atoi(a);

			if (rel)
				v = 3 + v;
			if (*a)
				hint(out, "font-size", sizes[v < 1 ? 0 :
					v > 7 ? 6 : v - 1]);
		}
	}
	if ((a = soie_element_get_attr(el, "align"))) {
		if (tag == TAG_DIV || tag == TAG_P || tag == TAG_H1 ||
		    tag == TAG_H2 || tag == TAG_H3 || tag == TAG_H4 ||
		    tag == TAG_H5 || tag == TAG_H6 || tag == TAG_CAPTION ||
		    tag == TAG_TD || tag == TAG_TH || tag == TAG_TR ||
		    tag == TAG_THEAD || tag == TAG_TBODY || tag == TAG_TFOOT) {
			if (ascii_ieq(a, "center") || ascii_ieq(a, "middle"))
				hint(out, "text-align", tag == TAG_DIV ||
					tag == TAG_TD || tag == TAG_TH ?
					"-webkit-center" : "center");
			else if (ascii_ieq(a, "left") || ascii_ieq(a, "right") ||
				 ascii_ieq(a, "justify"))
				hint(out, "text-align", a);
		} else if (tag == TAG_TABLE || tag == TAG_IMG ||
			   tag == TAG_IFRAME || tag == TAG_OBJECT ||
			   tag == TAG_EMBED || tag == TAG_INPUT) {
			if (ascii_ieq(a, "left") || ascii_ieq(a, "right"))
				hint(out, "float", a);
			else if (tag == TAG_TABLE && ascii_ieq(a, "center"))
				hint(out, "margin-inline", "auto");
		}
	}
	if ((a = soie_element_get_attr(el, "valign")) &&
	    (tag == TAG_TD || tag == TAG_TH || tag == TAG_TR ||
	     tag == TAG_THEAD || tag == TAG_TBODY || tag == TAG_TFOOT))
		hint(out, "vertical-align", a);
	if (tag == TAG_TABLE || tag == TAG_TD || tag == TAG_TH ||
	    tag == TAG_IMG || tag == TAG_IFRAME || tag == TAG_OBJECT ||
	    tag == TAG_EMBED || tag == TAG_HR || tag == TAG_COL ||
	    (tag == TAG_INPUT && (a = soie_element_get_attr(el, "type")) &&
	     ascii_ieq(a, "image"))) {
		if ((a = soie_element_get_attr(el, "width")) &&
		    dimension(a, d, sizeof(d), 1) && *a != '0')
			hint(out, "width", d);
		if (tag != TAG_HR && (a = soie_element_get_attr(el, "height")) &&
		    dimension(a, d, sizeof(d), 1) && *a != '0')
			hint(out, "height", d);
	}
	if ((tag == TAG_TD || tag == TAG_TH) &&
	    soie_element_get_attr(el, "nowrap"))
		hint(out, "white-space", "nowrap");
	if (tag == TAG_TABLE) {
		if ((a = soie_element_get_attr(el, "cellspacing")) &&
		    dimension(a, d, sizeof(d), 0))
			hint(out, "border-spacing", d);
		if ((a = soie_element_get_attr(el, "border"))) {
			int w = *a ? atoi(a) : 1;

			if (w > 0) {
				snprintf(d, sizeof(d), "%dpx", w);
				hint(out, "border-width", d);
				hint(out, "border-style", "outset");
				hint(out, "border-color", "gray");
			}
		}
	}
	if (tag == TAG_TD || tag == TAG_TH) {
		const soie_node *t = ancestor_table(el);

		if (t && (a = soie_element_get_attr(t, "cellpadding")) &&
		    dimension(a, d, sizeof(d), 0))
			hint(out, "padding", d);
		if (t && (a = soie_element_get_attr(t, "border")) &&
		    (!*a || atoi(a) > 0)) {
			hint(out, "border-width", "1px");
			hint(out, "border-style", "inset");
			hint(out, "border-color", "gray");
		}
	}
	if (tag == TAG_IMG || tag == TAG_OBJECT) {
		if ((a = soie_element_get_attr(el, "border")) &&
		    dimension(a, d, sizeof(d), 0)) {
			hint(out, "border-width", d);
			hint(out, "border-style", "solid");
		}
		if ((a = soie_element_get_attr(el, "hspace")) &&
		    dimension(a, d, sizeof(d), 0))
			hint(out, "margin-inline", d);
		if ((a = soie_element_get_attr(el, "vspace")) &&
		    dimension(a, d, sizeof(d), 0))
			hint(out, "margin-block", d);
	}
	if (tag == TAG_HR) {
		if ((a = soie_element_get_attr(el, "size")) &&
		    dimension(a, d, sizeof(d), 0))
			hint(out, "height", d);
		if ((a = soie_element_get_attr(el, "color"))) {
			legacy_color(out, "border-color", a);
			legacy_color(out, "background-color", a);
		}
		if (soie_element_get_attr(el, "noshade")) {
			hint(out, "border-style", "solid");
			hint(out, "border-color", "gray");
			hint(out, "background-color", "gray");
		}
	}
	if ((tag == TAG_UL || tag == TAG_OL || tag == TAG_LI) &&
	    (a = soie_element_get_attr(el, "type"))) {
		static const char *const types[][2] = {
			{ "1", "decimal" }, { "a", "lower-alpha" },
			{ "A", "upper-alpha" }, { "i", "lower-roman" },
			{ "I", "upper-roman" }, { "disc", "disc" },
			{ "circle", "circle" }, { "square", "square" },
			{ NULL, NULL }
		};
		int i;

		for (i = 0; types[i][0]; i++)
			if (!strcmp(a, types[i][0]) || (i > 4 &&
			    ascii_ieq(a, types[i][0])))
				hint(out, "list-style-type", types[i][1]);
	}
}

/* The cascade for one element */

typedef unsigned long long ckey;

static ckey make_key(int origin, int important, int inl, unsigned rank,
	unsigned spec, unsigned order)
{
	unsigned a = spec >> 20, b = spec >> 10 & 1023, c = spec & 1023;
	unsigned oi = important ? (unsigned)(5 - origin) : (unsigned)origin;

	if (a > 511) a = 511;
	if (b > 511) b = 511;
	if (c > 511) c = 511;
	if (rank > 16383)
		rank = 16383;
	if (important)
		rank = 16383 - rank;
	if (order > 524287)
		order = 524287;
	return (ckey)oi << 61 | (ckey)(inl ? 1 : 0) << 60 |
		(ckey)rank << 46 | (ckey)(a << 18 | b << 9 | c) << 19 | order;
}

struct win {
	ckey key;
	const struct sval *sv;
};

struct cwin {
	const char *name;	/* interned */
	const char *value;	/* rule text, not interned */
	ckey key;
};

struct cascade {
	struct win best[NPROPS];
	struct cwin *custom;
	int ncustom, capcustom;
};

static void cascade_decls(struct cascade *cs, const struct svec *d,
	ckey (*keyf)(const struct sval *, const void *), const void *data)
{
	int i;

	for (i = 0; i < d->n; i++) {
		const struct sval *sv = &d->v[i];
		ckey k = keyf(sv, data);
		struct win *w = &cs->best[sv->prop];

		if (!w->sv || k >= w->key) {
			w->sv = sv;
			w->key = k;
		}
	}
}

struct rkey {
	const struct srule *r;
	unsigned spec;
	int inl;
};

static ckey rule_key(const struct sval *sv, const void *data)
{
	const struct rkey *k = data;

	return make_key(k->r ? k->r->origin : O_AUTHOR, sv->important, k->inl,
		k->r ? k->r->layer->rank : 0, k->spec, k->r ? k->r->order : 0);
}

static void cascade_custom(struct styledb *db, struct cascade *cs,
	const char *name, const char *value, ckey k)
{
	const char *n = db_string(db, name, strlen(name));
	int i;

	for (i = 0; i < cs->ncustom; i++)
		if (cs->custom[i].name == n) {
			if (k >= cs->custom[i].key) {
				cs->custom[i].value = value;
				cs->custom[i].key = k;
			}
			return;
		}
	if (cs->ncustom == cs->capcustom) {
		cs->capcustom = cs->capcustom ? cs->capcustom * 2 : 8;
		cs->custom = xrealloc(cs->custom, (size_t)cs->capcustom *
			sizeof(*cs->custom));
	}
	cs->custom[cs->ncustom].name = n;
	cs->custom[cs->ncustom].value = value;
	cs->custom[cs->ncustom].key = k;
	cs->ncustom++;
}

/* Custom properties and var() substitution */

/*
 * The custom properties an element sets, being computed: sorted by name
 * pointer (names are interned), over base, the parent's set. A value
 * with var() waits in raw until it is resolved.
 */
struct vtmp {
	struct styledb *db;
	const struct vars *base;
	int n, cap;
	const char **name;
	const char **value;	/* NULL: guaranteed-invalid */
	char **raw;
	unsigned char *state;	/* 0 raw, 1 resolving, 2 done */
};

static const struct reg *registered(struct styledb *db, const char *name)
{
	int i;

	for (i = db->nregs - 1; i >= 0; i--)
		if (db->regs[i].name == name)
			return &db->regs[i];
	return NULL;
}

/* Index of name, or -(insertion point) - 1. */
static int vt_find(const struct vtmp *t, const char *name)
{
	int lo = 0, hi = t->n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (t->name[mid] == name)
			return mid;
		if ((size_t)t->name[mid] < (size_t)name)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -lo - 1;
}

static void vt_put(struct vtmp *t, const char *name, const char *value,
	char *raw)
{
	int i = vt_find(t, name);

	if (i < 0) {
		i = -i - 1;
		if (t->n == t->cap) {
			t->cap = t->cap ? t->cap * 2 : 16;
			t->name = xrealloc(t->name, (size_t)t->cap *
				sizeof(*t->name));
			t->value = xrealloc(t->value, (size_t)t->cap *
				sizeof(*t->value));
			t->raw = xrealloc(t->raw, (size_t)t->cap *
				sizeof(*t->raw));
			t->state = xrealloc(t->state, (size_t)t->cap);
		}
		memmove(t->name + i + 1, t->name + i,
			(size_t)(t->n - i) * sizeof(*t->name));
		memmove(t->value + i + 1, t->value + i,
			(size_t)(t->n - i) * sizeof(*t->value));
		memmove(t->raw + i + 1, t->raw + i,
			(size_t)(t->n - i) * sizeof(*t->raw));
		memmove(t->state + i + 1, t->state + i, (size_t)(t->n - i));
		t->n++;
		t->name[i] = name;
	} else {
		free(t->raw[i]);
	}
	t->value[i] = value;
	t->raw[i] = raw;
	t->state[i] = raw ? 0 : 2;
}

static const char *vtmp_resolve(struct vtmp *t, const char *name);

/* Append v[0..n) to out with var() replaced; 0 if invalid. */
static int substitute(struct vtmp *t, const struct cv *v, int n,
	struct buf *out, int depth)
{
	int i;

	if (depth > 32)
		return 0;
	for (i = 0; i < n; i++) {
		const struct cv *x = &v[i];

		if (x->type == CT_FUNCTION && (ascii_ieq(x->str, "var") ||
		    ascii_ieq(x->str, "env"))) {
			const struct cv *k = x->kids.v;
			int kn = x->kids.n, j = 0, comma;
			const char *val = NULL;

			while (j < kn && k[j].type == CT_WS)
				j++;
			if (j >= kn || k[j].type != CT_IDENT)
				return 0;
			if (ascii_ieq(x->str, "var")) {
				if (strncmp(k[j].str, "--", 2))
					return 0;
				val = vtmp_resolve(t, db_string(t->db,
					k[j].str, strlen(k[j].str)));
			}
			for (comma = j + 1; comma < kn &&
			     k[comma].type != CT_COMMA; comma++)
				;
			if (val) {
				buf_putc(out, ' ');
				buf_add(out, val, strlen(val));
				buf_putc(out, ' ');
			} else if (comma < kn) {
				if (!substitute(t, k + comma + 1,
						kn - comma - 1, out, depth + 1))
					return 0;
			} else {
				return 0;
			}
			continue;
		}
		if (x->type == CT_FUNCTION || x->type == CT_BLOCK) {
			struct cv shell = *x;
			struct buf tmp = { 0 };

			/* the opening, the contents, the closing */
			shell.kids.n = 0;
			css_serialize(&tmp, &shell, 1);
			buf_add(out, tmp.s, tmp.len - 1);
			buf_free(&tmp);
			if (!substitute(t, x->kids.v, x->kids.n, out, depth + 1))
				return 0;
			buf_putc(out, x->type == CT_FUNCTION ? ')' :
				x->delim == '{' ? '}' : x->delim == '[' ? ']' :
				')');
			continue;
		}
		css_serialize(out, x, 1);
	}
	return 1;
}

static char *substitute_text(struct vtmp *t, const char *text)
{
	struct cvlist l;
	struct buf b = { 0 };
	int ok;

	css_component_values(text, strlen(text), &l);
	buf_add(&b, "", 0);
	ok = substitute(t, l.v, l.n, &b, 0);
	cvlist_free(&l);
	if (!ok) {
		buf_free(&b);
		return NULL;
	}
	return b.s;
}

static const char *vtmp_resolve(struct vtmp *t, const char *name)
{
	int i = vt_find(t, name), found;
	const struct reg *g;
	const char *v;

	if (i < 0) {
		v = vars_get(t->base, name, &found);
		if (v)
			return v;
		g = registered(t->db, name);
		return g ? g->initial : NULL;
	}
	if (t->state[i] == 1) {
		/* a cycle: every property in it is invalid */
		t->value[i] = NULL;
		t->state[i] = 2;
		return NULL;
	}
	if (t->state[i] == 0) {
		char *r;

		t->state[i] = 1;
		r = substitute_text(t, t->raw[i]);
		i = vt_find(t, name);
		if (t->state[i] == 2) {		/* invalidated by a cycle */
			free(r);
		} else {
			t->value[i] = r ? db_string(t->db, r, strlen(r)) :
				NULL;
			free(r);
			t->state[i] = 2;
		}
		free(t->raw[i]);
		t->raw[i] = NULL;
	}
	if (!t->value[i]) {
		g = registered(t->db, name);
		return g ? g->initial : NULL;
	}
	return t->value[i];
}

static void vtmp_free(struct vtmp *t)
{
	int i;

	for (i = 0; i < t->n; i++)
		free(t->raw[i]);
	free(t->name);
	free(t->value);
	free(t->raw);
	free(t->state);
}

/* A chain deeper than this is merged, to keep look-ups short. */
#define VARS_DEPTH 8

static const struct vars *intern_vars(struct styledb *db, struct vtmp *t)
{
	const struct vars *base = t->base, *p;
	unsigned h = 2166136261u;
	struct vars **b, *v;
	int i;

	if (!t->n)
		return base;
	if (base && base->depth >= VARS_DEPTH) {
		/* flatten: everything the chain holds becomes ours */
		for (p = base; p; p = p->parent)
			for (i = 0; i < p->n; i++)
				if (vt_find(t, p->name[i]) < 0)
					vt_put(t, p->name[i], p->value[i],
						NULL);
		base = NULL;
	}
	h = hash_bytes(&base, sizeof(base), h);
	for (i = 0; i < t->n; i++) {
		h = hash_bytes(&t->name[i], sizeof(t->name[i]), h);
		h = hash_bytes(&t->value[i], sizeof(t->value[i]), h);
	}
	b = &db->varsets[h % 256];
	for (v = *b; v; v = v->next) {
		int same = v->hash == h && v->n == t->n && v->parent == base;

		for (i = 0; same && i < t->n; i++)
			same = v->name[i] == t->name[i] &&
				v->value[i] == t->value[i];
		if (same)
			return v;
	}
	v = xmalloc(sizeof(*v));
	v->hash = h;
	v->parent = base;
	v->depth = base ? base->depth + 1 : 0;
	v->n = t->n;
	v->name = xmalloc((size_t)t->n * sizeof(*v->name));
	v->value = xmalloc((size_t)t->n * sizeof(*v->value));
	memcpy(v->name, t->name, (size_t)t->n * sizeof(*v->name));
	memcpy(v->value, t->value, (size_t)t->n * sizeof(*v->value));
	v->next = *b;
	*b = v;
	return v;
}

static int wide_text(const char *v)
{
	while (*v == ' ')
		v++;
	if (ascii_ieq(v, "initial"))
		return SV_INITIAL;
	if (ascii_ieq(v, "inherit"))
		return SV_INHERIT;
	if (ascii_ieq(v, "unset") || ascii_ieq(v, "revert") ||
	    ascii_ieq(v, "revert-layer"))
		return SV_UNSET;
	return 0;
}

/*
 * Custom properties of an element. Returns the interned set; t is left
 * ready for var() look-ups by the longhands.
 */
static const struct vars *compute_custom(struct styledb *db,
	struct cascade *cs, const struct style *parent, struct vtmp *t)
{
	const struct vars *pv = parent ? parent->vars : NULL;
	int i, found;

	memset(t, 0, sizeof(*t));
	t->db = db;
	t->base = pv;
	/* registered with inherits: false: back to the initial value */
	for (i = 0; db->noninherit && i < db->nregs; i++)
		if (!db->regs[i].inherits &&
		    vars_get(pv, db->regs[i].name, &found) && found)
			vt_put(t, db->regs[i].name, NULL, NULL);
	for (i = 0; i < cs->ncustom; i++) {
		const char *name = cs->custom[i].name, *v = cs->custom[i].value;
		const struct reg *g = registered(db, name);
		const char *old = vars_get(pv, name, &found);
		int w = wide_text(v);

		if (w == SV_INITIAL || (w == SV_UNSET && g && !g->inherits)) {
			if (old)
				vt_put(t, name, NULL, NULL);
		} else if (w) {
			/* inherit/unset: keep the parent's value */
		} else if (strstr(v, "var(")) {
			vt_put(t, name, NULL, xstrndup(v, strlen(v)));
		} else {
			const char *iv = db_string(db, v, strlen(v));

			if (iv != old || vt_find(t, name) >= 0)
				vt_put(t, name, iv, NULL);
		}
	}
	for (i = 0; i < t->n; i++)
		if (t->state[i] == 0)
			vtmp_resolve(t, t->name[i]);
	return intern_vars(db, t);
}

/* Longhands */

static const struct style *intern_style(struct styledb *db,
	struct style *s)
{
	unsigned h = hash_words(s->v, sizeof(s->v), 2166136261u);
	struct style **b, *e;

	h = hash_bytes(&s->vars, sizeof(s->vars), h);
	b = &db->styles[h % NBUCKETS];
	for (e = *b; e; e = e->chain)
		if (e->hash == h && e->vars == s->vars &&
		    !memcmp(e->v, s->v, sizeof(s->v)))
			return e;
	e = xmalloc(sizeof(*e));
	*e = *s;
	e->hash = h;
	e->chain = *b;
	*b = e;
	return e;
}

/* The keyword number of name in prop's list, 0 when absent */
static int prop_kw(int prop, const char *name)
{
	const char *list = props[prop].keywords;
	int i = 0;
	size_t n = strlen(name);

	while (*list) {
		const char *e = strchr(list, '|');
		size_t k = e ? (size_t)(e - list) : strlen(list);

		if (k == n && !memcmp(list, name, n))
			return i + 1;
		if (!e)
			break;
		list = e + 1;
		i++;
	}
	return 0;
}

static int display_kw(const char *name)
{
	return prop_kw(P_DISPLAY, name);
}

static int kw_is(const struct cval *v, int prop, const char *name)
{
	const char *list = props[prop].keywords;
	int i = v->kw - 1;

	if (!v->kw)
		return 0;
	while (i-- > 0 && list)
		if ((list = strchr(list, '|')))
			list++;
	return list && !strncmp(list, name, strlen(name)) &&
		(list[strlen(name)] == '|' || !list[strlen(name)]);
}

/* CSS Display 2.7: blockification. */
static void blockify(struct cval *d)
{
	static const char *const map[][2] = {
		{ "inline", "block" }, { "inline-block", "block" },
		{ "inline-flex", "flex" }, { "inline-grid", "grid" },
		{ "inline-table", "table" }, { "table-row-group", "block" },
		{ "table-header-group", "block" },
		{ "table-footer-group", "block" }, { "table-row", "block" },
		{ "table-cell", "block" }, { "table-column-group", "block" },
		{ "table-column", "block" }, { "table-caption", "block" },
		{ "ruby", "block" }, { "ruby-text", "block" },
		{ NULL, NULL }
	};
	int i;

	for (i = 0; map[i][0]; i++)
		if (kw_is(d, P_DISPLAY, map[i][0])) {
			d->kw = (unsigned char)display_kw(map[i][1]);
			return;
		}
}

static void fixups(struct style *s, const struct style *parent, int root)
{
	static const short sides[4][2] = {
		{ P_BORDER_TOP_STYLE, P_BORDER_TOP_WIDTH },
		{ P_BORDER_RIGHT_STYLE, P_BORDER_RIGHT_WIDTH },
		{ P_BORDER_BOTTOM_STYLE, P_BORDER_BOTTOM_WIDTH },
		{ P_BORDER_LEFT_STYLE, P_BORDER_LEFT_WIDTH },
	};
	struct cval *pos = &s->v[P_POSITION];
	int i, absolute = kw_is(pos, P_POSITION, "absolute") ||
		kw_is(pos, P_POSITION, "fixed");

	/* the old WebKit box: vertical, a block (the line clamp's, as
	 * browsers lay it out now), else a flex row */
	if (kw_is(&s->v[P_DISPLAY], P_DISPLAY, "-webkit-box") ||
	    kw_is(&s->v[P_DISPLAY], P_DISPLAY, "-webkit-inline-box")) {
		int v = kw_is(&s->v[P_WEBKIT_BOX_ORIENT], P_WEBKIT_BOX_ORIENT,
			"vertical") || kw_is(&s->v[P_WEBKIT_BOX_ORIENT],
			P_WEBKIT_BOX_ORIENT, "block-axis");
		int in = kw_is(&s->v[P_DISPLAY], P_DISPLAY,
			"-webkit-inline-box");

		s->v[P_DISPLAY].kw = (unsigned char)display_kw(v ? (in ?
			"inline-block" : "flow-root") : in ? "inline-flex" :
			"flex");
	}
	if (absolute)
		s->v[P_FLOAT].kw = 1;	/* none */
	if (root || absolute || !kw_is(&s->v[P_FLOAT], P_FLOAT, "none") ||
	    (parent && (kw_is(&parent->v[P_DISPLAY], P_DISPLAY, "flex") ||
			kw_is(&parent->v[P_DISPLAY], P_DISPLAY, "inline-flex") ||
			kw_is(&parent->v[P_DISPLAY], P_DISPLAY, "grid") ||
			kw_is(&parent->v[P_DISPLAY], P_DISPLAY, "inline-grid"))))
		blockify(&s->v[P_DISPLAY]);
	if (root && kw_is(&s->v[P_DISPLAY], P_DISPLAY, "contents"))
		s->v[P_DISPLAY].kw = (unsigned char)display_kw("block");
	for (i = 0; i < 4; i++)
		if (kw_is(&s->v[sides[i][0]], sides[i][0], "none") ||
		    kw_is(&s->v[sides[i][0]], sides[i][0], "hidden"))
			memset(&s->v[sides[i][1]], 0, sizeof(struct cval));
	if (kw_is(&s->v[P_OUTLINE_STYLE], P_OUTLINE_STYLE, "none"))
		memset(&s->v[P_OUTLINE_WIDTH], 0, sizeof(struct cval));
	/* CSS Overflow 3: one axis scrolls or hides, then visible in the
	 * other is auto and clip is hidden (overflow-y: hidden alone makes
	 * a scroll container, flex items of which may shrink to nothing) */
	for (i = 0; i < 2; i++) {
		int ap = i ? P_OVERFLOW_Y : P_OVERFLOW_X;
		int bp = i ? P_OVERFLOW_X : P_OVERFLOW_Y;
		struct cval *a = &s->v[ap];
		const struct cval *b = &s->v[bp];

		if (kw_is(b, bp, "visible") || kw_is(b, bp, "clip"))
			continue;
		if (kw_is(a, ap, "visible"))
			a->kw = (unsigned char)prop_kw(ap, "auto");
		else if (kw_is(a, ap, "clip"))
			a->kw = (unsigned char)prop_kw(ap, "hidden");
	}
}

/* A pending (var()) value made concrete, or 0 if invalid. */
static int resolve_pending(struct vtmp *t, const struct sval *sv,
	struct sval *out)
{
	char *text = substitute_text(t, sv->str);
	struct cvlist l;
	struct svec v = { NULL, 0, 0 };
	int ok = 0, i, w;

	memset(out, 0, sizeof(*out));
	if (!text)
		return 0;
	w = wide_text(text);
	if (w) {
		free(text);
		out->type = (unsigned char)w;
		out->prop = sv->prop;
		return 1;
	}
	css_component_values(text, strlen(text), &l);
	free(text);
	if (sv->shorthand < 0) {
		ok = parse_longhand_value(sv->prop, &l, out);
	} else if (parse_shorthand_value(sv->shorthand, &l, &v)) {
		for (i = 0; i < v.n; i++)
			if (v.v[i].prop == sv->prop) {
				*out = v.v[i];
				memset(&v.v[i], 0, sizeof(v.v[i]));
				ok = 1;
			}
		svec_free(&v);
	}
	cvlist_free(&l);
	return ok;
}

static const struct style *compute_style(struct styledb *db,
	struct cascade *cs, const struct style *parent, int root, float rem)
{
	struct style s;
	struct cctx c;
	struct vtmp t;
	int i;

	memset(&s, 0, sizeof(s));
	s.vars = compute_custom(db, cs, parent, &t);
	c.db = db;
	c.parent = parent;
	c.self = &s;
	c.is_root = root;
	c.u = db->vp;
	c.u.em = parent ? parent->v[P_FONT_SIZE].u.len.px : 16 * db->vp.zoom;
	c.u.ex = c.u.ch = c.u.em / 2;
	c.u.rem = rem;
	for (i = 0; i < NPROPS; i++) {
		const struct sval *sv = cs->best[i].sv;
		struct sval tmp;
		int mode, used = 0;

		mode = sv ? sv->type : (props[i].inherited ? SV_INHERIT :
			SV_INITIAL);
		if (mode == SV_PENDING) {
			used = resolve_pending(&t, sv, &tmp);
			if (used) {
				sv = &tmp;
				mode = tmp.type;
			} else {
				mode = SV_UNSET;
			}
		}
		if (mode == SV_UNSET || mode == SV_REVERT)
			mode = props[i].inherited ? SV_INHERIT : SV_INITIAL;
		if (mode == SV_INHERIT && parent)
			s.v[i] = parent->v[i];
		else if (mode == SV_INHERIT || mode == SV_INITIAL)
			compute_value(i, &db->initial[i], &c, &s.v[i]);
		else
			compute_value(i, sv, &c, &s.v[i]);
		if (used)
			sval_free(&tmp);
		if (i == P_FONT_SIZE) {
			c.u.em = s.v[P_FONT_SIZE].u.len.px;
			c.u.ex = c.u.ch = c.u.em / 2;
			if (root)
				c.u.rem = c.u.em;
		}
	}
	vtmp_free(&t);
	fixups(&s, parent, root);
	return intern_style(db, &s);
}

/* e matched already (an :is() rule found in two buckets) */
static int in_evec(const struct evec *v, const struct entry *e)
{
	int i;

	for (i = 0; i < v->n; i++)
		if (v->v[i].r == e->r && v->v[i].sel == e->sel)
			return 1;
	return 0;
}

static void collect(struct styledb *db, const soie_node *el,
	struct evec *out)
{
	const char *a;
	struct evec *e;
	char *low;
	int i, k;

#define ADD(vec) do { \
	for (i = 0; (vec) && i < (vec)->n; i++) \
		if (selector_match((vec)->v[i].r->sel, (vec)->v[i].sel, el) && \
		    (!(vec)->v[i].multi || !in_evec(out, &(vec)->v[i]))) \
			evec_add(out, &(vec)->v[i]); \
} while (0)
	if ((a = soie_element_get_attr(el, "id")) && *a) {
		low = db->quirks ? lowdup(a) : NULL;
		e = bucket_get(&db->ids, low ? low : a, 0);
		ADD(e);
		free(low);
	}
	if ((a = soie_element_get_attr(el, "class"))) {
		char *copy = db->quirks ? lowdup(a) : xstrndup(a, strlen(a));
		char *p = copy, *w;

		while (*p) {
			while (*p == ' ' || *p == '\t' || *p == '\n' ||
			       *p == '\f' || *p == '\r')
				p++;
			w = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
			       *p != '\f' && *p != '\r')
				p++;
			if (p > w) {
				char save = *p;

				*p = '\0';
				e = bucket_get(&db->classes, w, 0);
				ADD(e);
				*p = save;
			}
		}
		free(copy);
	}
	low = lowdup(el->name);
	e = bucket_get(&db->tags, low, 0);
	free(low);
	ADD(e);
	for (k = 0; k < (int)el->u.el.nattrs; k++)
		if (el->u.el.attrs[k].ns == SOIE_NS_NONE) {
			e = bucket_get(&db->attrs, el->u.el.attrs[k].name, 0);
			ADD(e);
		}
	if (el->parent && el->parent->type == SOIE_DOCUMENT)
		ADD(&db->root);
	ADD(&db->universal);
#undef ADD
}

static void style_element(struct styledb *db, soie_node *el,
	const struct style *parent, float rem)
{
	struct evec m = { NULL, 0, 0 };
	struct cascade *cs = xmalloc(sizeof(*cs));
	struct svec hv = { NULL, 0, 0 };
	struct srule inl;
	struct rkey k;
	int i, j, pseudo, has[3] = { 1, 0, 0 };
	const char *style_attr;
	int root = el->parent && el->parent->type == SOIE_DOCUMENT;

	collect(db, el, &m);
	/* ::before and ::after only exist when content is set */
	for (i = 0; i < m.n; i++) {
		int p = m.v[i].pseudo == PE_BEFORE ? 1 :
			m.v[i].pseudo == PE_AFTER ? 2 : 0;

		for (j = 0; p && !has[p] && j < m.v[i].r->decls.n; j++)
			if (m.v[i].r->decls.v[j].prop == P_CONTENT)
				has[p] = 1;
	}
	hints(el, &hv);
	memset(&inl, 0, sizeof(inl));
	if ((style_attr = soie_element_get_attr(el, "style"))) {
		struct cvlist l;
		struct crule tmp;

		memset(&tmp, 0, sizeof(tmp));
		css_component_values(style_attr, strlen(style_attr), &l);
		css_parse_block(&l, &tmp, 1);
		add_decls(&inl, tmp.decls, tmp.ndecls, db->base);
		cvlist_free(&l);
		for (i = 0; i < tmp.ndecls; i++) {
			free(tmp.decls[i].name);
			cvlist_free(&tmp.decls[i].value);
		}
		free(tmp.decls);
		for (i = 0; i < tmp.nrules; i++)
			crule_free(tmp.rules[i]);
		free(tmp.rules);
	}
	for (pseudo = 0; pseudo < 3; pseudo++) {
		const struct style *st, *base = pseudo ? el->u.el.style : parent;
		int pe = pseudo == 0 ? PE_NONE : pseudo == 1 ? PE_BEFORE :
			PE_AFTER;

		if (!has[pseudo])
			continue;
		memset(cs, 0, sizeof(*cs));
		if (!pseudo) {
			k.r = NULL;
			k.spec = 0;
			k.inl = 0;
			cascade_decls(cs, &hv, rule_key, &k);
		}
		for (i = 0; i < m.n; i++) {
			const struct srule *r = m.v[i].r;

			if (m.v[i].pseudo != pe)
				continue;
			k.r = r;
			k.spec = m.v[i].spec;
			k.inl = 0;
			cascade_decls(cs, &r->decls, rule_key, &k);
			for (j = 0; j < r->ncustom; j++)
				cascade_custom(db, cs, r->custom[j].name,
					r->custom[j].value, make_key(r->origin,
					r->custom[j].important, 0,
					r->layer->rank, m.v[i].spec, r->order));
		}
		if (!pseudo) {
			k.r = NULL;
			k.spec = 0;
			k.inl = 1;
			cascade_decls(cs, &inl.decls, rule_key, &k);
			for (j = 0; j < inl.ncustom; j++)
				cascade_custom(db, cs, inl.custom[j].name,
					inl.custom[j].value, make_key(O_AUTHOR,
					inl.custom[j].important, 1, 0, 0, 0));
		}
		st = compute_style(db, cs, base, root && !pseudo, rem);
		free(cs->custom);
		if (pseudo) {
			/* no box without content */
			const struct cval *c = &st->v[P_CONTENT];

			if (!c->u.str || !strcmp(c->u.str, "normal") ||
			    !strcmp(c->u.str, "none") ||
			    kw_is(&st->v[P_DISPLAY], P_DISPLAY, "none"))
				st = NULL;
		}
		if (pseudo == 0)
			el->u.el.style = st;
		else if (pseudo == 1)
			el->u.el.before = st;
		else
			el->u.el.after = st;
	}
	free(cs);
	free(m.v);
	svec_free(&hv);
	svec_free(&inl.decls);
	for (j = 0; j < inl.ncustom; j++) {
		free(inl.custom[j].name);
		free(inl.custom[j].value);
	}
	free(inl.custom);
}

/* @font-face (CSS Fonts 4 4): fetch the first usable src. */
static void load_font_face(struct styledb *db, const void *doc,
	const char *block, const char *base)
{
	struct cvlist l;
	struct crule tmp;
	struct buf fam = { 0 };
	int i, r, weight = 0, italic = 0;
	const struct decl *src = NULL;

	memset(&tmp, 0, sizeof(tmp));
	css_component_values(block, strlen(block), &l);
	css_parse_block(&l, &tmp, 1);
	buf_add(&fam, "", 0);
	for (i = 0; i < tmp.ndecls; i++) {
		const struct decl *d = &tmp.decls[i];
		const struct cv *v = d->value.n ? &d->value.v[0] : NULL;

		if (ascii_ieq(d->name, "font-family") && v) {
			if (v->type == CT_STRING)
				buf_add(&fam, v->str, strlen(v->str));
			else
				css_serialize(&fam, d->value.v, d->value.n);
		} else if (ascii_ieq(d->name, "src")) {
			src = d;
		} else if (ascii_ieq(d->name, "font-weight") && v) {
			weight = v->type == CT_NUMBER ? (int)v->num :
				cv_ident(v, "bold") ? 700 : 400;
		} else if (ascii_ieq(d->name, "font-style") && v) {
			italic = !cv_ident(v, "normal");
		}
	}
	for (i = 0; src && fam.len && i < src->value.n; i++) {
		const struct cv *v = &src->value.v[i], *f;
		const char *url = NULL;
		char *buf = NULL, *abs;
		size_t len = 0;
		int k;

		if (v->type == CT_URL)
			url = v->str;
		else if (cv_func(v, "url") && v->kids.n &&
			 v->kids.v[0].type == CT_STRING)
			url = v->kids.v[0].str;
		if (!url)
			continue;
		/* skip formats FreeType cannot read here (woff2: brotli) */
		for (k = i + 1; k < src->value.n &&
		     src->value.v[k].type != CT_COMMA; k++) {
			f = &src->value.v[k];
			if (cv_func(f, "format") && f->kids.n &&
			    f->kids.v[0].str && (ascii_ieq(f->kids.v[0].str,
				"woff2") || ascii_ieq(f->kids.v[0].str,
				"embedded-opentype") ||
				ascii_ieq(f->kids.v[0].str, "svg")))
				url = NULL;
		}
		if (!url || !db->opts || !db->opts->fetch)
			continue;
		/* relative to the sheet, as every url() of it */
		abs = base ? soie_url_resolve(base, url) : NULL;
		if (abs)
			url = abs;
		r = db->opts->fetch(db->opts->fetch_data, url, &buf, &len);
		if (r == SOIE_FETCH_LATER)
			pending_add(db, url);
		free(abs);
		if (r || !buf)
			continue;
		if (font_add_memory(doc, fam.s, (unsigned char *)buf, len,
				    weight, italic))
			break;
	}
	buf_free(&fam);
	cvlist_free(&l);
	for (i = 0; i < tmp.ndecls; i++) {
		free(tmp.decls[i].name);
		cvlist_free(&tmp.decls[i].value);
	}
	free(tmp.decls);
	for (i = 0; i < tmp.nrules; i++)
		crule_free(tmp.rules[i]);
	free(tmp.rules);
}

static void load_fonts(struct styledb *db, const void *doc)
{
	int i;

	for (i = 0; i < db->nfontfaces; i++)
		load_font_face(db, doc, db->fontfaces[i], db->fontbases[i]);
}

/*
 * Style of an anonymous block box: inherited properties from parent,
 * initial values for the others, display: block.
 */
const struct style *anon_style(struct styledb *db, const struct style *parent)
{
	struct style s;
	struct cctx c;
	int i;

	for (i = 0; i < db->nanon; i++)
		if (db->anon[i].parent == parent)
			return db->anon[i].style;
	memset(&s, 0, sizeof(s));
	s.vars = parent ? parent->vars : NULL;
	memset(&c, 0, sizeof(c));
	c.db = db;
	c.parent = parent;
	c.self = &s;
	c.u = db->vp;
	c.u.em = parent ? parent->v[P_FONT_SIZE].u.len.px : 16 * db->vp.zoom;
	c.u.ex = c.u.ch = c.u.em / 2;
	c.u.rem = 16 * db->vp.zoom;
	for (i = 0; i < NPROPS; i++) {
		if (props[i].inherited && parent)
			s.v[i] = parent->v[i];
		else
			compute_value(i, &db->initial[i], &c, &s.v[i]);
	}
	s.v[P_DISPLAY].kw = (unsigned char)display_kw("block");
	fixups(&s, parent, 0);
	db->anon = xrealloc(db->anon, (size_t)(db->nanon + 1) *
		sizeof(*db->anon));
	db->anon[db->nanon].parent = parent;
	db->anon[db->nanon].style = intern_style(db, &s);
	return db->anon[db->nanon++].style;
}

/* Entry points */

static void parse_initial(struct styledb *db)
{
	int i;

	for (i = 0; i < NPROPS; i++) {
		struct cvlist l;
		const char *s = props[i].initial;

		css_component_values(s, strlen(s), &l);
		if (!parse_longhand_value(i, &l, &db->initial[i]))
			memset(&db->initial[i], 0, sizeof(db->initial[i]));
		db->initial[i].prop = (short)i;
		cvlist_free(&l);
	}
}

char *document_base(soie_node *doc, const char *base)
{
	soie_node *n = doc->first;

	while (n) {
		if (is_html(n, TAG_BASE)) {
			const char *href = soie_element_get_attr(n, "href");

			if (href)
				return soie_url_resolve(base, href);
		}
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n && !n->next)
			n = n->parent;
		if (n)
			n = n->next;
	}
	return base ? xstrndup(base, strlen(base)) : NULL;
}

/*
 * Incremental restyling. Changes mark elements: SUBTREE when their
 * whole subtree must be restyled (the changed element, siblings a
 * deep selector looks at), SELF when only their own match may have
 * changed (an ancestor for a subject :has(), a sibling for a subject
 * :first-child or +): restyled alone, their subtree follows only when
 * their style changed. Past MAX_DIRTY marks, or for a deep :has(),
 * everything restyles.
 */
#define MAX_DIRTY 512
enum { SELF = 1, SUBTREE = 2 };

static int connected(const soie_node *n)
{
	while (n->parent)
		n = n->parent;
	return n->type == SOIE_DOCUMENT;
}

static void mark(soie_node *n, int how)
{
	soie_node *doc = n->doc;

	if (n->type != SOIE_ELEMENT || n->dirty >= how ||
	    doc->u.doc->restyle_all)
		return;
	if (!n->dirty) {
		if (doc->u.doc->ndirty == MAX_DIRTY) {
			doc->u.doc->restyle_all = 1;
			return;
		}
		if (doc->u.doc->ndirty == doc->u.doc->capdirty) {
			doc->u.doc->capdirty = doc->u.doc->capdirty ?
				doc->u.doc->capdirty * 2 : 32;
			doc->u.doc->dirty = xrealloc(doc->u.doc->dirty,
				(size_t)doc->u.doc->capdirty *
				sizeof(soie_node *));
		}
		doc->u.doc->dirty[doc->u.doc->ndirty++] = n;
	}
	n->dirty = (unsigned char)how;
}

/* Siblings of n in one direction, as the flags say. */
static void mark_siblings(soie_node *n, int f, int bit, int next)
{
	int how = f & (bit << SF_DEEP) ? SUBTREE : f & bit ? SELF : 0;
	soie_node *k;

	if (!how)
		return;
	for (k = next ? n->next : n->prev; k; k = next ? k->next : k->prev)
		mark(k, how);
}

/* n changed: an attribute (name, its value was old), else n came or
 * goes */
void style_changed_attr(soie_node *n, int kind, const char *name,
	const char *old)
{
	soie_node *doc = n->doc, *a, *k;
	const struct styledb *db = doc->u.doc->styles;
	int f, i;

	if (!db || doc->u.doc->restyle_all || !n->parent || !connected(n))
		return;
	f = db->sel_flags;
	if (f & (SF_HAS << SF_DEEP))
		for (i = 0; i < db->nanchors; i++) {
			const struct sellist *l = db->anchors[i];
			int sib;

			/* a :has() that cannot see the change (a :has(dialog)
			 * and a div's class) needs nothing: YouTube has one
			 * at the root */
			if (!sellist_has_may_change(l, n, kind == CH_ATTR ?
			    name : NULL, old))
				continue;
			sib = sellist_has_sibling(l);
			for (a = n; a && a->type == SOIE_ELEMENT; a = a->parent) {
				soie_node *p;

				if (a != n && sellist_has_anchor(l, a)) {
					mark(a, SUBTREE);
					for (k = a->next; k; k = k->next)
						mark(k, SUBTREE);
				}
				/* :has(+ .x), :has(~ .x): the anchor is
				 * before */
				for (p = sib ? a->prev : NULL; p; p = p->prev)
					if (p->type == SOIE_ELEMENT &&
					    sellist_has_anchor(l, p)) {
						mark(p, SUBTREE);
						for (k = p->next; k; k = k->next)
							mark(k, SUBTREE);
					}
			}
		}
	if (f & SF_HAS)
		for (a = n->parent; a && a->type == SOIE_ELEMENT; a = a->parent) {
			mark(a, SELF);
			if (f & SF_HAS_SIB)
				mark_siblings(a, SF_FORWARD, SF_FORWARD, 0);
		}
	if (kind != CH_ATTR && (f & (SF_EMPTY | SF_EMPTY << SF_DEEP)))
		mark(n->parent, f & (SF_EMPTY << SF_DEEP) ? SUBTREE : SELF);
	if (n->type != SOIE_ELEMENT)
		return;		/* text: only :empty and :has() see it */
	if (kind != CH_REMOVE)
		mark(n, SUBTREE);
	if (f & SF_HAS_SIB)
		mark_siblings(n, SF_FORWARD, SF_FORWARD, 0);
	mark_siblings(n, f, SF_FORWARD, 1);
	mark_siblings(n, f, SF_BACKWARD, 0);
}

void style_changed(soie_node *n, int kind)
{
	style_changed_attr(n, kind, NULL, NULL);
}

/* A node freed while marked */
void style_forget(soie_node *n)
{
	soie_node *doc = n->doc;
	int i;

	for (i = 0; i < doc->u.doc->ndirty; i++)
		if (doc->u.doc->dirty[i] == n) {
			doc->u.doc->dirty[i] =
				doc->u.doc->dirty[--doc->u.doc->ndirty];
			break;
		}
	n->dirty = 0;
}

static void clear_marks(soie_node *doc)
{
	int i;

	for (i = 0; i < doc->u.doc->ndirty; i++)
		doc->u.doc->dirty[i]->dirty = 0;
	doc->u.doc->ndirty = 0;
	doc->u.doc->restyle_all = 0;
}

static void style_one(struct styledb *db, soie_node *n)
{
	const struct style *ps = n->parent && n->parent->type ==
		SOIE_ELEMENT ? n->parent->u.el.style : NULL;
	const struct style *st = n->u.el.style, *b = n->u.el.before,
		*a = n->u.el.after;

	int vp = vp_units_used;

	n->u.el.before = n->u.el.after = NULL;
	vp_units_used = 0;
	style_element(db, n, ps, db->rem);
	n->vp = (unsigned char)vp_units_used;
	db->vp_used |= vp_units_used;
	vp_units_used |= vp;
	/* styles are interned: the same pointer is the same style */
	if (st != n->u.el.style || b != n->u.el.before || a != n->u.el.after)
		layout_touch(n);
	if (n->parent && n->parent->type == SOIE_DOCUMENT)
		db->rem = n->u.el.style->v[P_FONT_SIZE].u.len.px;
}

/* Elements in tree order from top (top itself unless skip_top), each
 * after its parent. */
static void style_tree(struct styledb *db, soie_node *top, int skip_top)
{
	soie_node *n = top;

	while (n) {
		if (n->type == SOIE_ELEMENT && (n != top || !skip_top))
			style_one(db, n);
		if (n->first && n->type == SOIE_ELEMENT) {
			n = n->first;
			continue;
		}
		while (n && n != top && !n->next)
			n = n->parent;
		if (!n || n == top)
			break;
		n = n->next;
	}
}

/* Elements whose style has viewport units, for another viewport */
static void mark_viewport(soie_node *doc)
{
	soie_node *n = doc->first;

	while (n) {
		if (n->type == SOIE_ELEMENT && n->vp)
			mark(n, SELF);
		if (n->first && n->type == SOIE_ELEMENT) {
			n = n->first;
			continue;
		}
		while (n && !n->next)
			n = n->parent;
		if (n)
			n = n->next;
	}
}

/* The marked elements, unless an ancestor's subtree covers them.
 * Ancestors first: their subtree may cover more marks. */
static void style_marked(struct styledb *db, soie_node *doc)
{
	const struct style *st, *before, *after;
	soie_node *n, *a;
	int i, depth, maxd = 0, d;

	for (i = 0; i < doc->u.doc->ndirty; i++) {
		for (d = 0, a = doc->u.doc->dirty[i]; a->parent; a = a->parent)
			d++;
		if (d > maxd)
			maxd = d;
	}
	for (depth = 0; depth <= maxd; depth++)
		for (i = 0; i < doc->u.doc->ndirty; i++) {
			n = doc->u.doc->dirty[i];
			if (!n->dirty || !connected(n))
				continue;
			for (d = 0, a = n; a->parent; a = a->parent)
				d++;
			if (d != depth)
				continue;
			for (a = n->parent; a && a->type == SOIE_ELEMENT;
			     a = a->parent)
				if (a->dirty == SUBTREE)
					break;
			if (a && a->type == SOIE_ELEMENT)
				continue;
			if (n->dirty == SELF) {
				st = n->u.el.style;
				before = n->u.el.before;
				after = n->u.el.after;
				style_one(db, n);
				if (st == n->u.el.style &&
				    before == n->u.el.before &&
				    after == n->u.el.after)
					continue;
				n->dirty = SUBTREE;	/* covers the marks below */
				style_tree(db, n, 1);
			} else {
				style_tree(db, n, 0);
			}
		}
}

static unsigned hash_str(const char *s, unsigned h)
{
	return s ? hash_bytes(s, strlen(s) + 1, h) : hash_bytes("", 0, h ^ 1);
}

/*
 * A hash of everything the rules come from: options, the base URL, and
 * the <style>, <link> and <base> elements. When it has not changed, a
 * restyle (after a script changed the tree) keeps the parsed rules
 * and only runs the cascade again.
 */
static unsigned sheet_signature(soie_node *doc, const soie_style_options *o)
{
	static const char *const attrs[] = { "href", "rel", "media", "type",
		"disabled", NULL };
	unsigned h = 2166136261u;
	soie_node *n = doc->first;
	struct buf b = { 0 };
	int i;

	if (o) {
		int v[3];

		/* not the viewport's size: media_check() answers for it */
		v[0] = o->dark;
		v[1] = o->print;
		v[2] = o->scripting;
		h = hash_bytes(v, sizeof(v), h);
		h = hash_bytes(&o->fetch, sizeof(o->fetch), h);
		h = hash_bytes(&o->fetch_data, sizeof(o->fetch_data), h);
		h = hash_str(o->base_url, h);
		h = hash_str(o->user_css, h);
	}
	h = hash_bytes(&doc->u.doc->quirks, 1, h);
	while (n) {
		if (n->type == SOIE_ELEMENT && ((n->ns == SOIE_NS_HTML &&
		    (n->tag == TAG_STYLE || n->tag == TAG_LINK ||
		     n->tag == TAG_BASE)) || svg_style(n))) {
			h = hash_bytes(&n, sizeof(n), h);
			for (i = 0; attrs[i]; i++)
				h = hash_str(soie_element_get_attr(n,
					attrs[i]), h);
			if (n->tag == TAG_STYLE || n->ns == SOIE_NS_SVG) {
				b.len = 0;
				text_content(n, &b);
				h = hash_bytes(b.s ? b.s : "", b.len, h);
			}
		}
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n && !n->next)
			n = n->parent;
		if (n)
			n = n->next;
	}
	buf_free(&b);
	return h;
}

static void save_options(struct styledb *db, const soie_style_options *o)
{
	char *base = NULL, *css = NULL;

	if (o && o->base_url)
		base = xstrndup(o->base_url, strlen(o->base_url));
	if (o && o->user_css)
		css = xstrndup(o->user_css, strlen(o->user_css));
	/* o may point into the previous db: copy the strings first */
	free((char *)db->saved.base_url);
	free((char *)db->saved.user_css);
	if (o)
		db->saved = *o;
	else
		memset(&db->saved, 0, sizeof(db->saved));
	db->saved.base_url = base;
	db->saved.user_css = css;
}

static unsigned long serials;

unsigned long style_serial(const soie_node *doc)
{
	return doc->u.doc->styles ? doc->u.doc->styles->serial : 0;
}

int soie_style_document(soie_node *doc, const soie_style_options *opts)
{
	struct styledb *db;
	struct pctx pc;
	soie_node *n;
	char *base;
	unsigned next, sig;
	int i, reuse = 0, resized = 0;
	struct env env;
	float zoom = opts && opts->zoom > 0 ? opts->zoom : 1;

	if (doc->type != SOIE_DOCUMENT)
		return -1;
	doc->u.doc->zoom = zoom;
	sig = sheet_signature(doc, opts);
	db = doc->u.doc->styles;
	if (db && db->sig == sig && env_of(db, opts, &env) &&
	    !(db->incomplete && pending_changed(db, opts))) {
		/* same rules: only the cascade again, all of it when the
		 * viewport (its units) changed */
		resized = env.width != db->env.width ||
			env.height != db->env.height;
		/* another zoom: every length is another */
		if (zoom != db->vp.zoom)
			doc->u.doc->restyle_all = 1;
		db->env = env;
		db->vp.zoom = zoom;
		db->vp.vw = env.width * zoom;	/* device px */
		db->vp.vh = env.height * zoom;
		base = document_base(doc, opts ? opts->base_url : NULL);
		db->opts = opts;
		db->base = base;
		images_setup(doc, opts ? opts->fetch : NULL,
			opts ? opts->fetch_data : NULL, base);
		reuse = 1;
		goto cascade;
	}
	if (doc->u.doc->layout) {
		/* boxes point to the old styles; with the same rules they
		 * stay for the next layout to keep */
		layout_free(doc->u.doc->layout);
		doc->u.doc->layout = NULL;
	}
	if (db)
		styledb_free(db);
	db = xmalloc(sizeof(*db));
	memset(db, 0, sizeof(*db));
	db->serial = ++serials;
	doc->u.doc->styles = db;
	db->sig = sig;
	db->opts = opts;
	env_of(NULL, opts, &db->env);
	db->vp.zoom = zoom;
	db->vp.vw = db->env.width * zoom;
	db->vp.vh = db->env.height * zoom;
	db->doc_charset = doc->u.doc->charset;
	db->quirks = doc->u.doc->quirks == SOIE_QUIRKS;
	for (i = 0; i < 3; i++)
		db->roots[i].name = NULL;
	parse_initial(db);

	memset(&pc, 0, sizeof(pc));
	pc.origin = O_UA;
	pc.layer = &db->roots[O_UA];
	{
		struct buf ua = { 0 };

		for (i = 0; ua_css[i]; i++)
			buf_add(&ua, ua_css[i], strlen(ua_css[i]));
		load_sheet(db, ua.s, ua.len, &pc);
		buf_free(&ua);
	}
	if (db->quirks) {
		/* HTML 15.3.6: quirks mode tables reset the inherited text */
		static const char q[] = "table { font-weight: initial; "
			"font-style: initial; font-variant-caps: initial; "
			"font-size: initial; line-height: initial; "
			"white-space: initial; text-align: initial }";

		load_sheet(db, q, sizeof(q) - 1, &pc);
	}
	if (opts && opts->user_css) {
		pc.origin = O_USER;
		pc.layer = &db->roots[O_USER];
		load_sheet(db, opts->user_css, strlen(opts->user_css), &pc);
	}
	base = document_base(doc, opts ? opts->base_url : NULL);
	db->base = base;
	collect_sheets(db, doc, base);
	fonts_drop_document(doc);
	load_fonts(db, doc);
	images_setup(doc, opts ? opts->fetch : NULL,
		opts ? opts->fetch_data : NULL, base);
	for (i = 0; i < 3; i++) {
		next = 1;
		layer_rank(&db->roots[i], &next);
	}

cascade:
	/* another size changes only the styles with viewport units: they
	 * are restyled alone, their subtrees when their style changed (a
	 * window resized lays the page out again, mostly without a
	 * cascade) */
	if (resized && !doc->u.doc->restyle_all) {
		if (db->vp_used)
			mark_viewport(doc);
		resized = 0;
	}
	if (reuse && !resized && !doc->u.doc->restyle_all && doc->u.doc->ndirty &&
	    !(soie_document_element(doc) &&
	      soie_document_element(doc)->dirty == SUBTREE)) {
		/* only what changed */
		style_marked(db, doc);
	} else if (!reuse || resized || doc->u.doc->ndirty ||
		   doc->u.doc->restyle_all) {
		/* every element, in tree order, after its parent */
		db->rem = 16 * zoom;
		for (n = doc->first; n; n = n->next)
			style_tree(db, n, 0);
	}
	clear_marks(doc);
	save_options(db, opts);
	doc->u.doc->styled_gen = doc->u.doc->gen;
	db->opts = NULL;
	db->base = NULL;
	free(base);
	return 0;
}

const soie_style_options *style_saved_options(const soie_node *doc)
{
	const struct styledb *db = doc->doc->u.doc->styles;

	return db ? &db->saved : NULL;
}

int soie_media_matches(const soie_node *doc, const char *query)
{
	const struct styledb *db = doc->doc->u.doc->styles;
	struct env e;
	struct cvlist l;
	int m;

	if (db) {
		e = db->env;
	} else {
		memset(&e, 0, sizeof(e));
		e.width = 1024;
		e.height = 768;
	}
	css_component_values(query, strlen(query), &l);
	m = media_match(&l, 0, l.n, &e);
	cvlist_free(&l);
	return m;
}

int soie_document_update(soie_node *doc)
{
	struct styledb *db = doc->u.doc->styles;
	int done = 0;

	if (doc->type != SOIE_DOCUMENT || !db)
		return 0;
	if (doc->u.doc->gen != doc->u.doc->styled_gen) {
		/* a copy: a new style sheet frees db, and db->saved with it */
		soie_style_options o = db->saved;
		char *base = o.base_url ? xstrndup(o.base_url,
			strlen(o.base_url)) : NULL;
		char *css = o.user_css ? xstrndup(o.user_css,
			strlen(o.user_css)) : NULL;

		o.base_url = base;
		o.user_css = css;
		soie_style_document(doc, &o);
		free(base);
		free(css);
		done = 1;
	}
	if (doc->u.doc->lay_w && (done || !doc->u.doc->layout)) {
		soie_layout_document(doc, doc->u.doc->lay_w, doc->u.doc->lay_h);
		done = 1;
	}
	return done;
}

static const struct style *pick(const soie_node *el, const char *pseudo)
{
	if (el->type != SOIE_ELEMENT)
		return NULL;
	if (!pseudo || !*pseudo)
		return el->u.el.style;
	while (*pseudo == ':')
		pseudo++;
	if (ascii_ieq(pseudo, "before"))
		return el->u.el.before;
	if (ascii_ieq(pseudo, "after"))
		return el->u.el.after;
	return NULL;
}

static int value_of(const struct style *st, const char *prop, struct buf *b)
{
	int p, i;

	if (!st)
		return 0;
	if (!strncmp(prop, "--", 2)) {
		const struct vars *v;

		/* names are interned per document: compare strings here */
		for (v = st->vars; v; v = v->parent)
			for (i = 0; i < v->n; i++)
				if (!strcmp(v->name[i], prop)) {
					if (!v->value[i])
						return 0;
					buf_add(b, v->value[i],
						strlen(v->value[i]));
					return 1;
				}
		return 0;
	}
	p = prop_lookup(prop);
	if (p < 0)
		return 0;
	serialize_value(p, &st->v[p], b);
	return 1;
}

int soie_link_sheet(const soie_node *el)
{
	if (!el || el->type != SOIE_ELEMENT || el->tag != TAG_LINK)
		return 0;
	return el->sheet == 2 ? -1 : el->sheet;
}

char *soie_computed_value(const soie_node *el, const char *pseudo,
	const char *property)
{
	struct buf b = { 0 };

	buf_add(&b, "", 0);
	if (!value_of(pick(el, pseudo), property, &b)) {
		buf_free(&b);
		return NULL;
	}
	return b.s;
}

/* For dump.c: "@prop: value" lines of the listed properties. */
void dump_style_lines(struct buf *b, const soie_node *el, const char *props,
	int depth, void (*indent)(struct buf *, int))
{
	static const char *const pseudos[] = { NULL, "before", "after" };
	int k;

	for (k = 0; k < 3; k++) {
		const struct style *st = pick(el, pseudos[k]);
		const char *p = props;
		int d = depth;

		if (!st)
			continue;
		if (k) {
			indent(b, depth);
			buf_add(b, "::", 2);
			buf_add(b, pseudos[k], strlen(pseudos[k]));
			buf_putc(b, '\n');
			d++;
		}
		while (*p) {
			const char *e = strchr(p, ',');
			size_t n = e ? (size_t)(e - p) : strlen(p);
			char name[64];

			if (n && n < sizeof(name)) {
				memcpy(name, p, n);
				name[n] = '\0';
				indent(b, d);
				buf_putc(b, '@');
				buf_add(b, name, n);
				buf_add(b, ": ", 2);
				if (!value_of(st, name, b))
					buf_add(b, "(none)", 6);
				buf_putc(b, '\n');
			}
			p = e ? e + 1 : p + n;
		}
	}
}
