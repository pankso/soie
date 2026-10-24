/*
 * Layout internals: the box tree, line boxes and text measurement.
 * Never installed.
 */
#ifndef SOIE_INTERNAL_LAYOUT_H
#define SOIE_INTERNAL_LAYOUT_H

#include "css.h"

/* Text measurement (text.c): a stub until FreeType arrives. */
struct fontm {
	float ascent, descent;	/* above and below the baseline */
	float normal_lh;	/* line-height: normal */
};

void font_metrics(const struct style *st, struct fontm *m);
float text_width(const struct style *st, const char *s, size_t len);

enum {
	B_BLOCK,		/* block container, block-level */
	B_INLINE,		/* non-replaced inline box */
	B_ATOMIC,		/* inline-block: block container, inline-level */
	B_TEXT,
	B_BREAK,		/* <br> */
	B_REPLACED		/* img and friends; block or inline-level */
};

/* Table roles (CSS 2 17.2) */
enum { R_NONE, R_TABLE, R_CAPTION, R_ROWGROUP, R_ROW, R_CELL, R_COLGROUP,
	R_COL };

/* Flags */
#define BF_ANON		1	/* anonymous box */
#define BF_INLINE_KIDS	2	/* block container with inline content */
#define BF_BFC		4	/* establishes a block formatting context */
#define BF_FLOAT	8
#define BF_ABS		16	/* absolutely positioned (or fixed) */
#define BF_BLOCKLEVEL	32	/* replaced: block-level */
#define BF_OUTSIDE	64	/* list marker outside the content */
#define BF_PSEUDO	128	/* ::before, ::after or ::marker */

/* One piece of a line: text, an atomic box, an inline box edge. */
enum { F_TEXT, F_ATOMIC, F_INLINE };

struct frag {
	struct box *box;
	unsigned char kind;
	unsigned start, len;	/* text range in box->text */
	float x, y, w, h;	/* border box for atomics */
	float baseline;
	int spaces;		/* justification: spaces in the text */
};

struct line {
	float x, y, w, h;
	float baseline;
	float jspace;		/* text-align: justify, added per space */
	int first, n;		/* fragments */
};

struct box {
	unsigned char type;
	unsigned char flags;
	unsigned char pseudo;	/* 'b'efore, 'a'fter, 'm'arker */
	unsigned char ws_done;	/* white-space processing done */
	unsigned char role;	/* table part, R_* */
	const struct style *style;
	soie_node *node;	/* NULL for anonymous boxes */
	struct box *parent, *first, *last, *next;
	char *text;		/* B_TEXT after white-space processing */
	size_t len;
	/* geometry: content box, page coordinates */
	float x, y, w, h;
	float m[4], b[4], p[4];	/* top right bottom left */
	float iw, ih;		/* replaced: intrinsic size, -1 if none */
	/* inline formatting context of a block container */
	struct line *lines;
	int nlines;
	struct frag *frags;
	int nfrags, capfrags;
	float baseline;		/* first baseline, for inline-blocks */
	float last_baseline;
	/* content_sizes(), computed once while the box lives: flex and
	 * grid ask again at each level */
	float cmin, cmax;
	unsigned char sized;
	/* Incremental layout: a box whose element did not change since
	 * the last layout is kept, with its subtree (kept: from the old
	 * tree, fixed up); laid out alone again with the same width and
	 * containing block height (lw, lcbh, lok), it only moves. */
	unsigned char kept, lok;
	float lw, lcbh, lh;
	float lx, ly;		/* where its content was, translate() too */
	unsigned abs_pass;	/* in the absolute boxes of that layout */
	float sx, sy;		/* an absolute box's static position */
	/* the absolute boxes found laying it out: when it only moves,
	 * they are laid out again (their containing block may have a new
	 * height, flex stretches after) */
	struct box **absv;
	int nabsv;
};

struct layout {
	struct box *root;
	float width, height;	/* viewport */
	float doc_height;	/* bottom of everything laid out */
	unsigned long serial;	/* this tree, for soie_node box_tree */
	unsigned long built;	/* the document's lstamp when built */
	unsigned long rules;	/* style_serial() of its styles */
};

/* boxes.c */
struct box *box_new(int type, const struct style *st, soie_node *node);
void box_append(struct box *parent, struct box *child);
void box_free(struct box *b);
struct box *build_boxes(soie_node *doc);
int kw(const struct style *st, int prop, const char *name);

/* block.c */
struct bfc;
void layout_root(struct layout *l);
void translate(struct box *b, float dx, float dy);
/* The same for a box moved with the flow it is in (static positions) */
void translate_flow(struct box *b, float dx, float dy);
void box_sizes(struct box *b, float *min, float *max);
void layout_atomic(struct box *b, float cbw, float cbh);
void place_float(struct box *b, struct bfc *f, float y, float x0, float x1,
	float cbw);
float bfc_left(const struct bfc *f, float y, float h, float x0);
float bfc_right(const struct bfc *f, float y, float h, float x1);
float bfc_next(const struct bfc *f, float y);
void abs_static(struct box *b, float x, float y);
void box_edges(struct box *b, float cbw);
void content_sizes(struct box *b, float *min, float *max);
void layout_cell(struct box *c, float x, float y, float w);
void rel_offset(const struct box *b, float cbw, float *dx, float *dy);

/* inline.c */
/* cbh: b's content height when definite (percentage heights of its
 * atomics), else -1 */
float layout_inline(struct box *b, struct bfc *f, float x, float y,
	float w, float cbh);
void inline_sizes(struct box *b, float *min, float *max);

char *layout_dump(const struct box *root, size_t *len);

/* flex.c, grid.c */
int is_flexgrid(const struct box *b);
float layout_flex(struct box *b, float cbh);
float layout_grid(struct box *b, float cbh);
void flex_sizes(struct box *b, float *min, float *max);
void grid_sizes(struct box *b, float *min, float *max);
void layout_item(struct box *b, float x, float y, float w, float h);
float box_spec_w(const struct box *b, int prop, float cbw);
float box_spec_h(const struct box *b, int prop, float cbh);
float box_clamp_w(const struct box *b, float w, float cbw);
float box_clamp_h(const struct box *b, float h, float cbh);

/* table.c */
void table_fixup(struct styledb *db, struct box *b);
void layout_table(struct box *t, float avail);

/* Used values helpers (block.c) */
float len_val(const struct cval *v, float ref);
int is_auto(const struct style *st, int prop);

#endif
