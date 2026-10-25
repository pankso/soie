/*
 * HTML parser state shared by the tokenizer and the tree builder.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_PARSER_H
#define SOIE_PARSER_H

#include "internal.h"

enum {
	TOK_DOCTYPE,
	TOK_START,
	TOK_END,
	TOK_COMMENT,
	TOK_CHARS,
	TOK_EOF
};

struct tok_attr {
	struct buf name, value;
};

struct token {
	int type;
	int tag;			/* atom of name */
	struct buf name;		/* tag or doctype name */
	struct buf data;		/* comment data */
	struct buf public_id, system_id;
	unsigned char has_name, has_public, has_system, force_quirks;
	unsigned char self_closing;
	struct tok_attr *attrs;		/* buffers are reused between tags */
	int nattrs, cap;
	const char *chars;		/* TOK_CHARS, may contain NUL */
	size_t nchars;
};

/* Tokenizer states the tree builder switches to. */
enum {
	S_DATA,
	S_RCDATA,
	S_RAWTEXT,
	S_SCRIPT,
	S_PLAINTEXT
};

struct parser {
	/* Tokenizer */
	const char *in;
	size_t len, pos;
	int state;
	struct token tok;
	struct tok_attr *attr;		/* attribute being read */
	struct tok_attr dropped;	/* sink for duplicate attributes */
	struct buf text;		/* pending character data */
	struct buf tmp;			/* temporary buffer of the spec */
	struct buf last_start;		/* for "appropriate end tag" */
	int done;

	/* Tree builder */
	soie_node *doc;
	soie_node *root;		/* fragment parsing: the html node */
	soie_node *context;		/* fragment parsing context */
	int mode, orig_mode;
	soie_node **stack;
	int nstack, capstack;
	unsigned open[TAG_COUNT];	/* HTML elements on the stack, by tag */
	soie_node **afe;		/* active formatting, NULL = marker */
	int nafe, capafe;
	int *tmodes;			/* template insertion modes */
	int ntmodes, captmodes;
	soie_node *head, *form;
	int scripting, frameset_ok, foster, skip_lf;
	soie_node **orphans;		/* nodes left out of the tree */
	int norphans, caporphans;
	struct buf pending;		/* "in table text" characters */
	int pending_dirty;		/* pending has non-whitespace */
};

void tokenize(struct parser *p);
void tokenizer_free(struct parser *p);
void tree_token(struct parser *p, struct token *t);
int tree_cdata_allowed(struct parser *p);

#endif
