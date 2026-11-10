/*
 * CSS internals: tokens, component values, rules, selectors, values
 * and computed styles. Never installed.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_CSS_H
#define SOIE_CSS_H

#include "internal.h"

/* Tokens and component values (CSS Syntax 3) */

enum {
	CT_EOF,
	CT_IDENT,
	CT_FUNCTION,		/* also a function component value */
	CT_AT,
	CT_HASH,
	CT_STRING,
	CT_BAD_STRING,
	CT_URL,
	CT_BAD_URL,
	CT_DELIM,
	CT_NUMBER,
	CT_PERCENT,
	CT_DIMENSION,
	CT_WS,
	CT_CDO,
	CT_CDC,
	CT_COLON,
	CT_SEMI,
	CT_COMMA,
	CT_LBRACKET,
	CT_RBRACKET,
	CT_LPAREN,
	CT_RPAREN,
	CT_LBRACE,
	CT_RBRACE,
	CT_BLOCK		/* component value: [] () {} block */
};

struct cv;

struct cvlist {
	struct cv *v;
	int n, cap;
};

struct cv {
	unsigned char type;
	unsigned char integer;	/* number types: integer flag */
	unsigned char id;	/* hash: is an identifier */
	unsigned char delim;	/* delim char, block opening char */
	double num;
	char *str;		/* ident, function name, unit, string... */
	char *repr;		/* numbers: source text */
	struct cvlist kids;	/* function arguments, block contents */
};

struct decl {
	char *name;
	struct cvlist value;
	int important;
};

enum { R_QUALIFIED, R_AT };

struct crule {
	int type;
	char *name;			/* at-rule name */
	struct cvlist prelude;
	int has_block;
	struct cvlist block;		/* raw block, for at-rules */
	struct decl *decls;		/* parsed block contents */
	int ndecls, capdecls;
	struct crule **rules;
	int nrules, caprules;
	int nested_decls;		/* declarations after nested rules */
};

/* csstok.c / cssparse.c */
void cv_free(struct cv *v);
void cvlist_free(struct cvlist *l);
void cvlist_add(struct cvlist *l, const struct cv *v);
int css_component_values(const char *s, size_t len, struct cvlist *out);
struct crule **css_parse_stylesheet(const char *s, size_t len, int *n);
extern int css_raw_blocks;	/* tests: keep qualified rule blocks raw */
void css_parse_block(struct cvlist *block, struct crule *into, int nested);
void crule_free(struct crule *r);
struct crule *css_parse_test(const char *s, size_t len, int mode,
	const char **err);
void css_serialize(struct buf *b, const struct cv *v, int n);

/* Cursor over a component value list, skipping nothing by itself. */
struct cur {
	const struct cv *v;
	int i, n;
};

static inline void cur_init(struct cur *c, const struct cvlist *l)
{
	c->v = l->v;
	c->i = 0;
	c->n = l->n;
}

const struct cv *cur_peek(struct cur *c);	/* skips whitespace */
const struct cv *cur_next(struct cur *c);	/* skips whitespace */
int cur_end(struct cur *c);			/* only whitespace left */
int cv_ident(const struct cv *v, const char *s);	/* ASCII ci */
int cv_func(const struct cv *v, const char *s);
int ascii_ieq(const char *a, const char *b);

/* Selectors (selector.c) */

enum {
	SEL_TYPE,		/* name */
	SEL_UNIVERSAL,
	SEL_ID,
	SEL_CLASS,
	SEL_ATTR,
	SEL_PSEUDO,		/* pseudo-class, id in .pseudo */
	SEL_PSEUDO_ELEMENT,
	SEL_NEST		/* & : matches the parent rule's selector */
};

/* Combinator on the left of a compound. */
enum { COMB_NONE, COMB_DESC, COMB_CHILD, COMB_NEXT, COMB_SUBSEQ };

struct sellist;

struct simple {
	unsigned char type;
	unsigned char pseudo;	/* PC_* or PE_* */
	unsigned char op;	/* attribute operator */
	unsigned char ci;	/* attribute value case-insensitive */
	int a, b;		/* An+B */
	char *name;		/* lowercase for type and attribute names */
	char *value;
	struct sellist *args;	/* :is() :not() :has() :nth-*(of S)
				 * :host() ::slotted() */
};

struct compound {
	struct simple *s;
	int n;
	int comb;
};

struct complex {
	struct compound *c;	/* left to right */
	int n;
	unsigned spec;		/* a << 20 | b << 10 | c */
	int pseudo_element;	/* PE_NONE or the rightmost one */
	int relative;		/* :has() argument: starts with comb */
};

struct sellist {
	struct complex *c;
	int n;
	const struct sellist *parent;	/* what & stands for */
};

enum { PE_NONE, PE_BEFORE, PE_AFTER, PE_MARKER, PE_OTHER, PE_SLOTTED };

/* Pseudo-classes (the cascade indexes rules by some) */
enum {
	PC_NONE,
	PC_ROOT, PC_EMPTY, PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD,
	PC_FIRST_OF_TYPE, PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE,
	PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE,
	PC_NOT, PC_IS, PC_WHERE, PC_HAS, PC_LANG, PC_DIR,
	PC_LINK, PC_VISITED, PC_CHECKED, PC_DISABLED, PC_ENABLED,
	PC_REQUIRED, PC_OPTIONAL, PC_READ_ONLY, PC_READ_WRITE,
	PC_PLACEHOLDER_SHOWN, PC_OPEN, PC_DEFINED, PC_VALID,
	PC_HOST, PC_HOST_CONTEXT,	/* :host, :host(S), :host-context(S) */
	PC_TRUE,	/* always matches: :scope... handled as root */
	PC_FALSE	/* states the engine does not have yet */
};

struct sellist *selector_parse(const struct cvlist *l, int from, int to,
	const struct sellist *parent, int forgiving);
void sellist_free(struct sellist *s);
int selector_match(const struct sellist *l, int i, const soie_node *el);
int word_in(const char *list, const char *w, int ci);
int sellist_match(const struct sellist *l, const soie_node *el);
void selector_scope(const soie_node *el);
/* The shadow root whose rules are matched (cascade.c), else NULL: its
 * host matches :host, ::slotted() sees its slots */
void selector_shadow(const soie_node *root);
/* Selector i of l: has its subject :host? does el match its
 * ::slotted() argument (the rest is matched from el's slot)? */
int selector_has_host(const struct sellist *l, int i);
int selector_slotted(const struct sellist *l, int i, const soie_node *el);
unsigned sellist_max_spec(const struct sellist *s, const soie_node *el);
/*
 * What a change can affect beyond the changed element's subtree. The
 * low bits are dependencies of the selector's subject itself (restyle
 * the sibling or ancestor alone, its subtree only if its style
 * changed); the same bits << SF_DEEP come from compounds further left
 * (`li:first-child a`): their whole subtree depends on them.
 */
#define SF_FORWARD 1	/* later siblings: +, ~, :first-child, :nth-child */
#define SF_BACKWARD 2	/* earlier siblings: :last-child, :nth-last-* */
#define SF_EMPTY 4	/* the parent: :empty */
#define SF_HAS 8	/* ancestors: :has() */
#define SF_HAS_SIB 16	/* their earlier siblings: :has(+ x), :has(~ x) */
#define SF_DEEP 8	/* shift */
int sellist_flags(const struct sellist *s);
int sellist_has_anchor(const struct sellist *l, const soie_node *el);
int sellist_has_may_change(const struct sellist *l, const soie_node *n,
	const char *name, const char *old);
int sellist_has_sibling(const struct sellist *l);

/* Values (values.c) */

enum {
	CS_SRGB, CS_SRGB_LINEAR, CS_DISPLAY_P3, CS_A98, CS_PROPHOTO,
	CS_REC2020, CS_XYZ_D50, CS_XYZ_D65, CS_LAB, CS_LCH, CS_OKLAB,
	CS_OKLCH, CS_HSL, CS_HWB, CS_RGB_LEGACY
};

struct color {
	double c[3];
	double alpha;
	unsigned char space;
	unsigned char none;	/* bit per missing component, 8 = alpha */
	unsigned char current;	/* currentcolor */
	unsigned char pad;
};

/* A length-percentage computed to px + % of a reference. */
struct calc;

struct len {
	float px, pct;
	struct calc *calc;	/* min()/max()/clamp() with % */
};

/* Context to resolve relative units. */
struct ucontext {
	float em, rem, vw, vh;
	float ex, ch;
	float zoom;		/* device px a CSS px (absolute units), 0: 1 */
};
/* Set when a length was resolved with viewport units (vw, vh...) */
extern int vp_units_used;

int parse_color(struct cur *c, struct color *out);
void serialize_color(struct buf *b, const struct color *col);
void color_to_rgba(const struct color *col, unsigned char rgba[4]);
int named_color(const char *name, struct color *out);

/* Length-percentage: specified form, before unit resolution. */
struct slen;
struct slen *parse_length(struct cur *c, int flags);
void slen_free(struct slen *l);
int slen_resolve(const struct slen *l, const struct ucontext *u,
	struct len *out);
void doc_units(const struct style *st, struct ucontext *u);
float calc_eval(const struct calc *k, float pct_ref);
void calc_release(struct calc *k);
int slen_number(const struct slen *l, double *out);
void serialize_len(struct buf *b, const struct len *l);
void serialize_number(struct buf *b, double v);
void serialize_float(struct buf *b, double v);	/* 4 decimals */
int parse_anb(struct cur *c, int *a, int *b);

#define L_NONNEG 1
#define L_NOPCT 2
#define L_UNITLESS0 4	/* plain 0 allowed (always, except in calc) */
#define L_NUMBER 8	/* a bare number is accepted (line-height) */

/* Properties (props.c) */

enum {
	K_KW, K_LEN, K_COLOR, K_NUM, K_INT, K_ALPHA, K_FSIZE, K_FWEIGHT,
	K_LH, K_FAMILY, K_RAW, K_BWIDTH, K_LIST, K_DECO, K_DISPLAY
};

#define BORDER_STYLES "none|hidden|dotted|dashed|solid|double|groove|" \
	"ridge|inset|outset"

enum {
#define PROP(id, name, inh, kind, flags, kw, init) P_##id,
#include "props.h"
#undef PROP
	NPROPS
};

struct propdef {
	const char *name;
	unsigned char inherited, kind, flags;
	const char *keywords, *initial;
};

extern const struct propdef props[NPROPS];

/* Specified value of one longhand. */
enum { SV_VALUE, SV_INHERIT, SV_INITIAL, SV_UNSET, SV_REVERT, SV_PENDING };

struct sval {
	short prop;
	short shorthand;	/* SV_PENDING: shorthand id, or -1 */
	unsigned char type;
	unsigned char important;
	unsigned char kw;	/* 1 + keyword index, 0 if a value */
	unsigned char flag;
	double num;
	struct slen *len;
	struct color color;
	char *str;		/* raw text, family list, pending text */
};

struct svec {
	struct sval *v;
	int n, cap;
};

/* A computed value: 24 bytes, so that styles stay small. */
struct cval {
	unsigned char kw;	/* 1 + keyword index, 0 if a value */
	unsigned char flag;	/* K_LH: 1 number, 2 length */
	unsigned char pad[6];
	union {
		struct len len;
		double num;
		const struct color *color;
		const char *str;
	} u;
};

struct styledb;

struct style {
	struct style *chain;		/* intern table bucket */
	unsigned hash;
	const struct vars *vars;	/* custom properties */
	struct cval v[NPROPS];
};

/* Everything a value needs to be computed. */
struct cctx {
	struct styledb *db;
	const struct style *parent;	/* NULL for the root */
	struct style *self;		/* values computed so far */
	struct ucontext u;		/* em is the parent's until font-size */
	int is_root;
};

int prop_lookup(const char *name);
int shorthand_lookup(const char *name);
int parse_declaration(const char *name, const struct cvlist *value,
	int important, struct svec *out);
int parse_shorthand_value(int sh, const struct cvlist *value,
	struct svec *out);
int parse_longhand_value(int prop, const struct cvlist *value,
	struct sval *out);
void svec_free(struct svec *v);
void sval_free(struct sval *v);
void compute_value(int prop, const struct sval *sv, struct cctx *c,
	struct cval *out);
void serialize_value(int prop, const struct cval *v, struct buf *b);
int has_var(const struct cv *v, int n);

/* Interning (cascade.c) */
const struct style *anon_style(struct styledb *db, const struct style *parent);
const char *db_string(struct styledb *db, const char *s, size_t len);
const struct color *db_color(struct styledb *db, const struct color *c);
struct calc *db_calc(struct styledb *db, struct calc *k);

/* Media queries and @supports (media.c) */
struct env {
	float width, height;
	int dark, print, scripting;
};

int media_match(const struct cvlist *l, int from, int to,
	const struct env *e);
int supports_match(const struct cvlist *l, int from, int to);

#endif
