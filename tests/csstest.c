/*
 * Driver for the css-parsing-tests corpus (see tests/css-parsing.py).
 * Usage: csstest FUNCTION < inputs, where each input is its byte length
 * on a line, then the bytes. Prints one JSON result per line.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "css.h"

static void jstr(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\')
			printf("\\%c", c);
		else if (c < 0x20)
			printf("\\u%04x", c);
		else
			putchar(c);
	}
	putchar('"');
}

static void jnum(const struct cv *v)
{
	jstr(v->repr);
	printf(", %.17g, \"%s\"", v->num, v->integer ? "integer" : "number");
}

static void jlist(const struct cv *v, int n);

static void jcv(const struct cv *v)
{
	char d[2] = { 0, 0 };

	switch (v->type) {
	case CT_IDENT: printf("[\"ident\", "); jstr(v->str); putchar(']'); break;
	case CT_AT: printf("[\"at-keyword\", "); jstr(v->str); putchar(']'); break;
	case CT_HASH:
		printf("[\"hash\", ");
		jstr(v->str);
		printf(", \"%s\"]", v->id ? "id" : "unrestricted");
		break;
	case CT_STRING: printf("[\"string\", "); jstr(v->str); putchar(']'); break;
	case CT_BAD_STRING: printf("[\"error\", \"bad-string\"]"); break;
	case CT_URL: printf("[\"url\", "); jstr(v->str); putchar(']'); break;
	case CT_BAD_URL: printf("[\"error\", \"bad-url\"]"); break;
	case CT_DELIM: d[0] = (char)v->delim; jstr(d); break;
	case CT_NUMBER: printf("[\"number\", "); jnum(v); putchar(']'); break;
	case CT_PERCENT: printf("[\"percentage\", "); jnum(v); putchar(']'); break;
	case CT_DIMENSION:
		printf("[\"dimension\", ");
		jnum(v);
		printf(", ");
		jstr(v->str);
		putchar(']');
		break;
	case CT_WS: printf("\" \""); break;
	case CT_CDO: printf("\"<!--\""); break;
	case CT_CDC: printf("\"-->\""); break;
	case CT_COLON: printf("\":\""); break;
	case CT_SEMI: printf("\";\""); break;
	case CT_COMMA: printf("\",\""); break;
	case CT_RBRACKET: printf("[\"error\", \"]\"]"); break;
	case CT_RPAREN: printf("[\"error\", \")\"]"); break;
	case CT_RBRACE: printf("[\"error\", \"}\"]"); break;
	case CT_FUNCTION:
		printf("[\"function\", ");
		jstr(v->str);
		if (v->kids.n)
			printf(", ");
		jlist(v->kids.v, v->kids.n);
		putchar(']');
		break;
	case CT_BLOCK:
		printf("[\"%c%c\"", v->delim, v->delim == '{' ? '}' :
			v->delim == '[' ? ']' : ')');
		if (v->kids.n)
			printf(", ");
		jlist(v->kids.v, v->kids.n);
		putchar(']');
		break;
	}
}

static void jlist(const struct cv *v, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (i)
			printf(", ");
		jcv(&v[i]);
	}
}

static void jdecl(const struct decl *d)
{
	printf("[\"declaration\", ");
	jstr(d->name);
	printf(", [");
	jlist(d->value.v, d->value.n);
	printf("], %s]", d->important ? "true" : "false");
}

static void jrule(const struct crule *r);

/* Items of a block: declarations, then rules (nested declarations
 * rules are flattened back in place). */
static void jitems(const struct crule *c)
{
	int i, j, first = 1;

	for (i = 0; i < c->ndecls; i++, first = 0) {
		if (!first)
			printf(", ");
		jdecl(&c->decls[i]);
	}
	for (i = 0; i < c->nrules; i++) {
		if (c->rules[i]->nested_decls) {
			for (j = 0; j < c->rules[i]->ndecls; j++, first = 0) {
				if (!first)
					printf(", ");
				jdecl(&c->rules[i]->decls[j]);
			}
			continue;
		}
		if (!first)
			printf(", ");
		first = 0;
		jrule(c->rules[i]);
	}
}

static void jrule(const struct crule *r)
{
	if (r->type == R_AT) {
		printf("[\"at-rule\", ");
		jstr(r->name);
		printf(", [");
		jlist(r->prelude.v, r->prelude.n);
		printf("], ");
		if (r->has_block) {
			putchar('[');
			jlist(r->block.v, r->block.n);
			putchar(']');
		} else {
			printf("null");
		}
		putchar(']');
		return;
	}
	printf("[\"qualified rule\", [");
	jlist(r->prelude.v, r->prelude.n);
	printf("], [");
	jlist(r->block.v, r->block.n);
	printf("]]");
}

static void run(const char *fn, const char *s, size_t len)
{
	struct cvlist l;
	struct crule *c;
	const char *err;
	int mode = 0, i;

	if (!strcmp(fn, "component_value_list") ||
	    !strcmp(fn, "one_component_value")) {
		css_component_values(s, len, &l);
		if (fn[0] == 'c') {
			putchar('[');
			jlist(l.v, l.n);
			putchar(']');
		} else {
			int first = -1, count = 0;

			for (i = 0; i < l.n; i++)
				if (l.v[i].type != CT_WS) {
					if (first < 0)
						first = i;
					count++;
				}
			if (!count)
				printf("[\"error\", \"empty\"]");
			else if (count > 1)
				printf("[\"error\", \"extra-input\"]");
			else
				jcv(&l.v[first]);
		}
		cvlist_free(&l);
		putchar('\n');
		return;
	}
	if (!strcmp(fn, "stylesheet"))
		mode = 'S';
	else if (!strcmp(fn, "rule_list"))
		mode = 'L';
	else if (!strcmp(fn, "blocks_contents") ||
		 !strcmp(fn, "declaration_list"))
		mode = 'B';
	else if (!strcmp(fn, "one_rule"))
		mode = 'R';
	else if (!strcmp(fn, "one_declaration"))
		mode = 'D';
	c = css_parse_test(s, len, mode, &err);
	if (!c) {
		printf("[\"error\", \"%s\"]\n", err);
		return;
	}
	if (mode == 'R')
		jrule(c->rules[0]);
	else if (mode == 'D')
		jdecl(&c->decls[0]);
	else {
		putchar('[');
		jitems(c);
		putchar(']');
	}
	putchar('\n');
	crule_free(c);
}

int main(int argc, char **argv)
{
	size_t len;
	char *buf;

	if (argc != 2)
		return 2;
	css_raw_blocks = 1;
	while (scanf("%zu", &len) == 1) {
		getchar();
		buf = malloc(len + 1);
		if (fread(buf, 1, len, stdin) != len)
			return 1;
		run(argv[1], buf, len);
		free(buf);
		fflush(stdout);
	}
	return 0;
}
