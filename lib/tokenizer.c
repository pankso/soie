/*
 * HTML tokenizer, after the WHATWG state machine (section 13.2.5).
 * Input is valid UTF-8 with normalized newlines. Character references
 * are decoded by charref() instead of their own states.
 */
#include <stdlib.h>
#include <string.h>
#include "parser.h"

#define EOF_C (-1)

enum {
	/* S_DATA .. S_PLAINTEXT come from parser.h */
	S_TAG_OPEN = S_PLAINTEXT + 1,
	S_END_TAG_OPEN,
	S_TAG_NAME,
	S_RCDATA_LT,
	S_RCDATA_END_TAG_OPEN,
	S_RCDATA_END_TAG_NAME,
	S_RAWTEXT_LT,
	S_RAWTEXT_END_TAG_OPEN,
	S_RAWTEXT_END_TAG_NAME,
	S_SCRIPT_LT,
	S_SCRIPT_END_TAG_OPEN,
	S_SCRIPT_END_TAG_NAME,
	S_SCRIPT_ESCAPE_START,
	S_SCRIPT_ESCAPE_START_DASH,
	S_SCRIPT_ESCAPED,
	S_SCRIPT_ESCAPED_DASH,
	S_SCRIPT_ESCAPED_DASH_DASH,
	S_SCRIPT_ESCAPED_LT,
	S_SCRIPT_ESCAPED_END_TAG_OPEN,
	S_SCRIPT_ESCAPED_END_TAG_NAME,
	S_SCRIPT_DOUBLE_ESCAPE_START,
	S_SCRIPT_DOUBLE_ESCAPED,
	S_SCRIPT_DOUBLE_ESCAPED_DASH,
	S_SCRIPT_DOUBLE_ESCAPED_DASH_DASH,
	S_SCRIPT_DOUBLE_ESCAPED_LT,
	S_SCRIPT_DOUBLE_ESCAPE_END,
	S_BEFORE_ATTR_NAME,
	S_ATTR_NAME,
	S_AFTER_ATTR_NAME,
	S_BEFORE_ATTR_VALUE,
	S_ATTR_VALUE_DQ,
	S_ATTR_VALUE_SQ,
	S_ATTR_VALUE_UNQ,
	S_AFTER_ATTR_VALUE_QUOTED,
	S_SELF_CLOSING_START_TAG,
	S_BOGUS_COMMENT,
	S_MARKUP_DECLARATION_OPEN,
	S_COMMENT_START,
	S_COMMENT_START_DASH,
	S_COMMENT,
	S_COMMENT_LT,
	S_COMMENT_LT_BANG,
	S_COMMENT_LT_BANG_DASH,
	S_COMMENT_LT_BANG_DASH_DASH,
	S_COMMENT_END_DASH,
	S_COMMENT_END,
	S_COMMENT_END_BANG,
	S_DOCTYPE,
	S_BEFORE_DOCTYPE_NAME,
	S_DOCTYPE_NAME,
	S_AFTER_DOCTYPE_NAME,
	S_AFTER_DOCTYPE_PUBLIC_KEYWORD,
	S_BEFORE_DOCTYPE_PUBLIC_ID,
	S_DOCTYPE_PUBLIC_ID_DQ,
	S_DOCTYPE_PUBLIC_ID_SQ,
	S_AFTER_DOCTYPE_PUBLIC_ID,
	S_BETWEEN_DOCTYPE_IDS,
	S_AFTER_DOCTYPE_SYSTEM_KEYWORD,
	S_BEFORE_DOCTYPE_SYSTEM_ID,
	S_DOCTYPE_SYSTEM_ID_DQ,
	S_DOCTYPE_SYSTEM_ID_SQ,
	S_AFTER_DOCTYPE_SYSTEM_ID,
	S_BOGUS_DOCTYPE,
	S_CDATA_SECTION,
	S_CDATA_SECTION_BRACKET,
	S_CDATA_SECTION_END
};

struct entity {
	const char *name;
	const char *value;
};

static const struct entity entities[] = {
#include "entities.inc"
};

extern const unsigned short cp1252_c1[32];

#define REPLACEMENT "\xef\xbf\xbd"

static int is_alpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_upper(int c)
{
	return c >= 'A' && c <= 'Z';
}

static int is_digit(int c)
{
	return c >= '0' && c <= '9';
}

static int is_alnum(int c)
{
	return is_alpha(c) || is_digit(c);
}

static int is_hex(int c)
{
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_ws(int c)
{
	return c == '\t' || c == '\n' || c == '\f' || c == ' ';
}

static int next(struct parser *p)
{
	if (p->pos < p->len)
		return (unsigned char)p->in[p->pos++];
	p->pos++;
	return EOF_C;
}

static void reconsume(struct parser *p, int state)
{
	p->pos--;
	p->state = state;
}

/* Case-insensitive match of an ASCII word at the input position. */
static int lookahead(struct parser *p, const char *word, int ci)
{
	size_t n = strlen(word), i;

	if (p->len - p->pos < n)
		return 0;
	for (i = 0; i < n; i++) {
		int c = (unsigned char)p->in[p->pos + i];

		if (ci && is_upper(c))
			c += 32;
		if (c != word[i])
			return 0;
	}
	p->pos += n;
	return 1;
}

/* Emission */

static void emit_char(struct parser *p, int c)
{
	buf_putc(&p->text, c);
}

static void emit_str(struct parser *p, const char *s)
{
	buf_add(&p->text, s, strlen(s));
}

static void flush_text(struct parser *p)
{
	struct token t;

	if (!p->text.len)
		return;
	memset(&t, 0, sizeof(t));
	t.type = TOK_CHARS;
	t.chars = p->text.s;
	t.nchars = p->text.len;
	tree_token(p, &t);
	buf_reset(&p->text);
}

static void emit_token(struct parser *p)
{
	struct token *t = &p->tok;

	flush_text(p);
	if (t->type == TOK_START || t->type == TOK_END) {
		t->tag = tag_lookup(t->name.s ? t->name.s : "", t->name.len);
		if (t->type == TOK_START) {
			buf_reset(&p->last_start);
			buf_add(&p->last_start, t->name.s, t->name.len);
		}
	}
	tree_token(p, t);
}

static void emit_eof(struct parser *p)
{
	flush_text(p);
	p->tok.type = TOK_EOF;
	tree_token(p, &p->tok);
	p->done = 1;
}

static void new_token(struct parser *p, int type)
{
	struct token *t = &p->tok;

	t->type = type;
	t->tag = TAG_UNKNOWN;
	buf_reset(&t->name);
	buf_reset(&t->data);
	buf_reset(&t->public_id);
	buf_reset(&t->system_id);
	buf_putc(&t->name, 0), t->name.len = 0;
	buf_putc(&t->data, 0), t->data.len = 0;
	t->has_name = t->has_public = t->has_system = 0;
	t->force_quirks = t->self_closing = 0;
	t->nattrs = 0;
}

static void new_attr(struct parser *p)
{
	struct token *t = &p->tok;

	if (t->nattrs == t->cap) {
		int old = t->cap;

		t->cap = t->cap ? t->cap * 2 : 8;
		t->attrs = xrealloc(t->attrs, t->cap * sizeof(*t->attrs));
		memset(t->attrs + old, 0, (t->cap - old) * sizeof(*t->attrs));
	}
	p->attr = &t->attrs[t->nattrs++];
	buf_reset(&p->attr->name);
	buf_reset(&p->attr->value);
	buf_putc(&p->attr->name, 0), p->attr->name.len = 0;
	buf_putc(&p->attr->value, 0), p->attr->value.len = 0;
}

/* Leaving the attribute name state: drop duplicates. */
static void check_attr(struct parser *p)
{
	struct token *t = &p->tok;
	int i;

	if (p->attr == &p->dropped)
		return;
	for (i = 0; i < t->nattrs - 1; i++)
		if (t->attrs[i].name.len == p->attr->name.len &&
		    !memcmp(t->attrs[i].name.s, p->attr->name.s,
			    p->attr->name.len)) {
			t->nattrs--;
			p->attr = &p->dropped;
			buf_reset(&p->dropped.value);
			return;
		}
}

static int appropriate_end_tag(struct parser *p)
{
	return p->last_start.len && p->tok.name.len == p->last_start.len &&
		!memcmp(p->tok.name.s, p->last_start.s, p->last_start.len);
}

/* Character references (13.2.5.77 to 13.2.5.84) */

static const char *entity_lookup(const char *s, size_t len)
{
	int lo = 0, hi = (int)(sizeof(entities) / sizeof(*entities)) - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const char *n = entities[mid].name;
		int c = strncmp(s, n, len);

		if (c == 0 && n[len] != '\0')
			c = -1;
		if (c == 0)
			return entities[mid].value;
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return NULL;
}

/* Called after '&'; appends the decoded text to out. */
static void charref(struct parser *p, struct buf *out, int in_attr)
{
	const char *s = p->in + p->pos, *val = NULL;
	size_t left = p->len - p->pos, i, run, start;
	unsigned long v = 0;
	int hex = 0;

	if (left && s[0] == '#') {
		i = 1;
		if (i < left && (s[i] == 'x' || s[i] == 'X')) {
			hex = 1;
			i++;
		}
		start = i;
		while (i < left && (hex ? is_hex(s[i]) : is_digit(s[i]))) {
			int d = is_digit(s[i]) ? s[i] - '0' :
				(s[i] | 32) - 'a' + 10;

			if (v <= 0x10ffff)
				v = v * (hex ? 16 : 10) + (unsigned long)d;
			i++;
		}
		if (i == start) {
			buf_putc(out, '&');
			return;
		}
		if (i < left && s[i] == ';')
			i++;
		p->pos += i;
		if (v == 0 || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
			v = 0xfffd;
		else if (v >= 0x80 && v <= 0x9f)
			v = cp1252_c1[v - 0x80];
		buf_put_utf8(out, v);
		return;
	}
	for (run = 0; run < left && run < 40 && is_alnum(s[run]); run++)
		;
	i = 0;
	if (run && run < left && s[run] == ';')
		val = entity_lookup(s, (i = run + 1));
	if (!val)
		for (i = run; i > 0; i--)
			if ((val = entity_lookup(s, i)))
				break;
	if (!val || (in_attr && s[i - 1] != ';' && i < left &&
		     (s[i] == '=' || is_alnum(s[i])))) {
		buf_putc(out, '&');
		return;
	}
	p->pos += i;
	buf_add(out, val, strlen(val));
}

/* Fast path for runs of ordinary characters in text states. */
static void text_run(struct parser *p, int amp)
{
	size_t start = p->pos;

	while (p->pos < p->len) {
		char c = p->in[p->pos];

		if (c == '<' || c == '\0' || (amp && c == '&'))
			break;
		p->pos++;
	}
	if (p->pos > start)
		buf_add(&p->text, p->in + start, p->pos - start);
}

/* RCDATA, RAWTEXT and script data end tag name states */
static void end_tag_name(struct parser *p, int c, int back)
{
	if (appropriate_end_tag(p)) {
		if (is_ws(c)) {
			p->state = S_BEFORE_ATTR_NAME;
			return;
		}
		if (c == '/') {
			p->state = S_SELF_CLOSING_START_TAG;
			return;
		}
		if (c == '>') {
			p->state = S_DATA;
			emit_token(p);
			return;
		}
	}
	if (is_alpha(c)) {
		buf_putc(&p->tok.name, is_upper(c) ? c + 32 : c);
		buf_putc(&p->tmp, c);
		return;
	}
	emit_str(p, "</");
	buf_add(&p->text, p->tmp.s, p->tmp.len);
	reconsume(p, back);
}

static void doctype_eof(struct parser *p)
{
	p->tok.force_quirks = 1;
	emit_token(p);
	emit_eof(p);
}

void tokenize(struct parser *p)
{
	struct token *t = &p->tok;
	struct buf *id;
	int c, q;

	while (!p->done) {
		switch (p->state) {
		case S_DATA:
			text_run(p, 1);
			c = next(p);
			if (c == '&')
				charref(p, &p->text, 0);
			else if (c == '<')
				p->state = S_TAG_OPEN;
			else if (c == 0)
				emit_char(p, 0);
			else if (c == EOF_C)
				emit_eof(p);
			break;
		case S_RCDATA:
		case S_RAWTEXT:
		case S_SCRIPT:
		case S_PLAINTEXT:
			text_run(p, p->state == S_RCDATA);
			c = next(p);
			if (c == '&')
				charref(p, &p->text, 0);
			else if (c == '<' && p->state != S_PLAINTEXT)
				p->state = p->state == S_RCDATA ? S_RCDATA_LT :
					p->state == S_RAWTEXT ? S_RAWTEXT_LT :
					S_SCRIPT_LT;
			else if (c == '<')
				emit_char(p, c);
			else if (c == 0)
				emit_str(p, REPLACEMENT);
			else if (c == EOF_C)
				emit_eof(p);
			break;
		case S_TAG_OPEN:
			c = next(p);
			if (c == '!') {
				p->state = S_MARKUP_DECLARATION_OPEN;
			} else if (c == '/') {
				p->state = S_END_TAG_OPEN;
			} else if (is_alpha(c)) {
				new_token(p, TOK_START);
				reconsume(p, S_TAG_NAME);
			} else if (c == '?') {
				/* Processing instructions are not
				 * implemented: see DECISIONS.md. */
				new_token(p, TOK_COMMENT);
				reconsume(p, S_BOGUS_COMMENT);
			} else if (c == EOF_C) {
				emit_char(p, '<');
				emit_eof(p);
			} else {
				emit_char(p, '<');
				reconsume(p, S_DATA);
			}
			break;
		case S_END_TAG_OPEN:
			c = next(p);
			if (is_alpha(c)) {
				new_token(p, TOK_END);
				reconsume(p, S_TAG_NAME);
			} else if (c == '>') {
				p->state = S_DATA;
			} else if (c == EOF_C) {
				emit_str(p, "</");
				emit_eof(p);
			} else {
				new_token(p, TOK_COMMENT);
				reconsume(p, S_BOGUS_COMMENT);
			}
			break;
		case S_TAG_NAME:
			c = next(p);
			if (is_ws(c))
				p->state = S_BEFORE_ATTR_NAME;
			else if (c == '/')
				p->state = S_SELF_CLOSING_START_TAG;
			else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == 0)
				buf_add(&t->name, REPLACEMENT, 3);
			else if (c == EOF_C)
				emit_eof(p);
			else
				buf_putc(&t->name, is_upper(c) ? c + 32 : c);
			break;
		case S_RCDATA_LT:
		case S_RAWTEXT_LT:
			q = p->state == S_RCDATA_LT ? S_RCDATA : S_RAWTEXT;
			c = next(p);
			if (c == '/') {
				buf_reset(&p->tmp);
				p->state = q == S_RCDATA ? S_RCDATA_END_TAG_OPEN :
					S_RAWTEXT_END_TAG_OPEN;
			} else {
				emit_char(p, '<');
				reconsume(p, q);
			}
			break;
		case S_RCDATA_END_TAG_OPEN:
		case S_RAWTEXT_END_TAG_OPEN:
		case S_SCRIPT_END_TAG_OPEN:
		case S_SCRIPT_ESCAPED_END_TAG_OPEN:
			q = p->state;
			c = next(p);
			if (is_alpha(c)) {
				new_token(p, TOK_END);
				reconsume(p, q + 1);	/* the name state */
			} else {
				emit_str(p, "</");
				reconsume(p,
					q == S_RCDATA_END_TAG_OPEN ? S_RCDATA :
					q == S_RAWTEXT_END_TAG_OPEN ? S_RAWTEXT :
					q == S_SCRIPT_END_TAG_OPEN ? S_SCRIPT :
					S_SCRIPT_ESCAPED);
			}
			break;
		case S_RCDATA_END_TAG_NAME:
			end_tag_name(p, next(p), S_RCDATA);
			break;
		case S_RAWTEXT_END_TAG_NAME:
			end_tag_name(p, next(p), S_RAWTEXT);
			break;
		case S_SCRIPT_END_TAG_NAME:
			end_tag_name(p, next(p), S_SCRIPT);
			break;
		case S_SCRIPT_ESCAPED_END_TAG_NAME:
			end_tag_name(p, next(p), S_SCRIPT_ESCAPED);
			break;
		case S_SCRIPT_LT:
			c = next(p);
			if (c == '/') {
				buf_reset(&p->tmp);
				p->state = S_SCRIPT_END_TAG_OPEN;
			} else if (c == '!') {
				p->state = S_SCRIPT_ESCAPE_START;
				emit_str(p, "<!");
			} else {
				emit_char(p, '<');
				reconsume(p, S_SCRIPT);
			}
			break;
		case S_SCRIPT_ESCAPE_START:
		case S_SCRIPT_ESCAPE_START_DASH:
			c = next(p);
			if (c == '-') {
				p->state = p->state == S_SCRIPT_ESCAPE_START ?
					S_SCRIPT_ESCAPE_START_DASH :
					S_SCRIPT_ESCAPED_DASH_DASH;
				emit_char(p, '-');
			} else {
				reconsume(p, S_SCRIPT);
			}
			break;
		case S_SCRIPT_ESCAPED:
		case S_SCRIPT_ESCAPED_DASH:
		case S_SCRIPT_ESCAPED_DASH_DASH:
		case S_SCRIPT_DOUBLE_ESCAPED:
		case S_SCRIPT_DOUBLE_ESCAPED_DASH:
		case S_SCRIPT_DOUBLE_ESCAPED_DASH_DASH:
			q = p->state >= S_SCRIPT_DOUBLE_ESCAPED;
			c = next(p);
			if (c == '-') {
				/* escaped -> dash -> dash dash */
				if (p->state != S_SCRIPT_ESCAPED_DASH_DASH &&
				    p->state != S_SCRIPT_DOUBLE_ESCAPED_DASH_DASH)
					p->state++;
				emit_char(p, '-');
			} else if (c == '<') {
				if (q) {
					p->state = S_SCRIPT_DOUBLE_ESCAPED_LT;
					emit_char(p, '<');
				} else {
					p->state = S_SCRIPT_ESCAPED_LT;
				}
			} else if (c == '>' &&
				   (p->state == S_SCRIPT_ESCAPED_DASH_DASH ||
				    p->state == S_SCRIPT_DOUBLE_ESCAPED_DASH_DASH)) {
				p->state = S_SCRIPT;
				emit_char(p, '>');
			} else if (c == EOF_C) {
				emit_eof(p);
			} else {
				p->state = q ? S_SCRIPT_DOUBLE_ESCAPED :
					S_SCRIPT_ESCAPED;
				if (c == 0)
					emit_str(p, REPLACEMENT);
				else
					emit_char(p, c);
			}
			break;
		case S_SCRIPT_ESCAPED_LT:
			c = next(p);
			if (c == '/') {
				buf_reset(&p->tmp);
				p->state = S_SCRIPT_ESCAPED_END_TAG_OPEN;
			} else if (is_alpha(c)) {
				buf_reset(&p->tmp);
				emit_char(p, '<');
				reconsume(p, S_SCRIPT_DOUBLE_ESCAPE_START);
			} else {
				emit_char(p, '<');
				reconsume(p, S_SCRIPT_ESCAPED);
			}
			break;
		case S_SCRIPT_DOUBLE_ESCAPE_START:
		case S_SCRIPT_DOUBLE_ESCAPE_END:
			q = p->state == S_SCRIPT_DOUBLE_ESCAPE_START;
			c = next(p);
			if (is_ws(c) || c == '/' || c == '>') {
				int script = p->tmp.len == 6 &&
					!memcmp(p->tmp.s, "script", 6);

				if (q)
					p->state = script ?
						S_SCRIPT_DOUBLE_ESCAPED :
						S_SCRIPT_ESCAPED;
				else
					p->state = script ? S_SCRIPT_ESCAPED :
						S_SCRIPT_DOUBLE_ESCAPED;
				emit_char(p, c);
			} else if (is_alpha(c)) {
				buf_putc(&p->tmp, is_upper(c) ? c + 32 : c);
				emit_char(p, c);
			} else {
				reconsume(p, q ? S_SCRIPT_ESCAPED :
					S_SCRIPT_DOUBLE_ESCAPED);
			}
			break;
		case S_SCRIPT_DOUBLE_ESCAPED_LT:
			c = next(p);
			if (c == '/') {
				buf_reset(&p->tmp);
				p->state = S_SCRIPT_DOUBLE_ESCAPE_END;
				emit_char(p, '/');
			} else {
				reconsume(p, S_SCRIPT_DOUBLE_ESCAPED);
			}
			break;
		case S_BEFORE_ATTR_NAME:
			c = next(p);
			if (is_ws(c))
				break;
			if (c == '/' || c == '>' || c == EOF_C) {
				reconsume(p, S_AFTER_ATTR_NAME);
			} else if (c == '=') {
				new_attr(p);
				buf_putc(&p->attr->name, c);
				p->state = S_ATTR_NAME;
			} else {
				new_attr(p);
				reconsume(p, S_ATTR_NAME);
			}
			break;
		case S_ATTR_NAME:
			c = next(p);
			if (is_ws(c) || c == '/' || c == '>' || c == EOF_C) {
				check_attr(p);
				reconsume(p, S_AFTER_ATTR_NAME);
			} else if (c == '=') {
				check_attr(p);
				p->state = S_BEFORE_ATTR_VALUE;
			} else if (c == 0) {
				buf_add(&p->attr->name, REPLACEMENT, 3);
			} else {
				buf_putc(&p->attr->name,
					is_upper(c) ? c + 32 : c);
			}
			break;
		case S_AFTER_ATTR_NAME:
			c = next(p);
			if (is_ws(c))
				break;
			if (c == '/') {
				p->state = S_SELF_CLOSING_START_TAG;
			} else if (c == '=') {
				p->state = S_BEFORE_ATTR_VALUE;
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				emit_eof(p);
			} else {
				new_attr(p);
				reconsume(p, S_ATTR_NAME);
			}
			break;
		case S_BEFORE_ATTR_VALUE:
			c = next(p);
			if (is_ws(c))
				break;
			if (c == '"') {
				p->state = S_ATTR_VALUE_DQ;
			} else if (c == '\'') {
				p->state = S_ATTR_VALUE_SQ;
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else {
				reconsume(p, S_ATTR_VALUE_UNQ);
			}
			break;
		case S_ATTR_VALUE_DQ:
		case S_ATTR_VALUE_SQ:
			q = p->state == S_ATTR_VALUE_DQ ? '"' : '\'';
			{
				size_t start = p->pos;

				while (p->pos < p->len && p->in[p->pos] != q &&
				       p->in[p->pos] != '&' &&
				       p->in[p->pos] != '\0')
					p->pos++;
				buf_add(&p->attr->value, p->in + start,
					p->pos - start);
			}
			c = next(p);
			if (c == q)
				p->state = S_AFTER_ATTR_VALUE_QUOTED;
			else if (c == '&')
				charref(p, &p->attr->value, 1);
			else if (c == 0)
				buf_add(&p->attr->value, REPLACEMENT, 3);
			else if (c == EOF_C)
				emit_eof(p);
			break;
		case S_ATTR_VALUE_UNQ:
			c = next(p);
			if (is_ws(c)) {
				p->state = S_BEFORE_ATTR_NAME;
			} else if (c == '&') {
				charref(p, &p->attr->value, 1);
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == 0) {
				buf_add(&p->attr->value, REPLACEMENT, 3);
			} else if (c == EOF_C) {
				emit_eof(p);
			} else {
				buf_putc(&p->attr->value, c);
			}
			break;
		case S_AFTER_ATTR_VALUE_QUOTED:
			c = next(p);
			if (is_ws(c)) {
				p->state = S_BEFORE_ATTR_NAME;
			} else if (c == '/') {
				p->state = S_SELF_CLOSING_START_TAG;
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				emit_eof(p);
			} else {
				reconsume(p, S_BEFORE_ATTR_NAME);
			}
			break;
		case S_SELF_CLOSING_START_TAG:
			c = next(p);
			if (c == '>') {
				t->self_closing = 1;
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				emit_eof(p);
			} else {
				reconsume(p, S_BEFORE_ATTR_NAME);
			}
			break;
		case S_BOGUS_COMMENT:
			c = next(p);
			if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				emit_token(p);
				emit_eof(p);
			} else if (c == 0) {
				buf_add(&t->data, REPLACEMENT, 3);
			} else {
				buf_putc(&t->data, c);
			}
			break;
		case S_MARKUP_DECLARATION_OPEN:
			if (lookahead(p, "--", 0)) {
				new_token(p, TOK_COMMENT);
				p->state = S_COMMENT_START;
			} else if (lookahead(p, "doctype", 1)) {
				p->state = S_DOCTYPE;
			} else if (lookahead(p, "[CDATA[", 0)) {
				if (tree_cdata_allowed(p)) {
					p->state = S_CDATA_SECTION;
				} else {
					new_token(p, TOK_COMMENT);
					buf_add(&t->data, "[CDATA[", 7);
					p->state = S_BOGUS_COMMENT;
				}
			} else {
				new_token(p, TOK_COMMENT);
				p->state = S_BOGUS_COMMENT;
			}
			break;
		case S_COMMENT_START:
			c = next(p);
			if (c == '-') {
				p->state = S_COMMENT_START_DASH;
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else {
				reconsume(p, S_COMMENT);
			}
			break;
		case S_COMMENT_START_DASH:
			c = next(p);
			if (c == '-') {
				p->state = S_COMMENT_END;
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				emit_token(p);
				emit_eof(p);
			} else {
				buf_putc(&t->data, '-');
				reconsume(p, S_COMMENT);
			}
			break;
		case S_COMMENT:
			{
				size_t start = p->pos;

				while (p->pos < p->len && p->in[p->pos] != '<' &&
				       p->in[p->pos] != '-' &&
				       p->in[p->pos] != '\0')
					p->pos++;
				buf_add(&t->data, p->in + start, p->pos - start);
			}
			c = next(p);
			if (c == '<') {
				buf_putc(&t->data, c);
				p->state = S_COMMENT_LT;
			} else if (c == '-') {
				p->state = S_COMMENT_END_DASH;
			} else if (c == 0) {
				buf_add(&t->data, REPLACEMENT, 3);
			} else if (c == EOF_C) {
				emit_token(p);
				emit_eof(p);
			}
			break;
		case S_COMMENT_LT:
			c = next(p);
			if (c == '!') {
				buf_putc(&t->data, c);
				p->state = S_COMMENT_LT_BANG;
			} else if (c == '<') {
				buf_putc(&t->data, c);
			} else {
				reconsume(p, S_COMMENT);
			}
			break;
		case S_COMMENT_LT_BANG:
			c = next(p);
			if (c == '-')
				p->state = S_COMMENT_LT_BANG_DASH;
			else
				reconsume(p, S_COMMENT);
			break;
		case S_COMMENT_LT_BANG_DASH:
			c = next(p);
			if (c == '-')
				p->state = S_COMMENT_LT_BANG_DASH_DASH;
			else
				reconsume(p, S_COMMENT_END_DASH);
			break;
		case S_COMMENT_LT_BANG_DASH_DASH:
			next(p);
			reconsume(p, S_COMMENT_END);
			break;
		case S_COMMENT_END_DASH:
			c = next(p);
			if (c == '-') {
				p->state = S_COMMENT_END;
			} else if (c == EOF_C) {
				emit_token(p);
				emit_eof(p);
			} else {
				buf_putc(&t->data, '-');
				reconsume(p, S_COMMENT);
			}
			break;
		case S_COMMENT_END:
			c = next(p);
			if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == '!') {
				p->state = S_COMMENT_END_BANG;
			} else if (c == '-') {
				buf_putc(&t->data, '-');
			} else if (c == EOF_C) {
				emit_token(p);
				emit_eof(p);
			} else {
				buf_add(&t->data, "--", 2);
				reconsume(p, S_COMMENT);
			}
			break;
		case S_COMMENT_END_BANG:
			c = next(p);
			if (c == '-') {
				buf_add(&t->data, "--!", 3);
				p->state = S_COMMENT_END_DASH;
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				emit_token(p);
				emit_eof(p);
			} else {
				buf_add(&t->data, "--!", 3);
				reconsume(p, S_COMMENT);
			}
			break;
		case S_DOCTYPE:
			c = next(p);
			if (is_ws(c)) {
				p->state = S_BEFORE_DOCTYPE_NAME;
			} else if (c == EOF_C) {
				new_token(p, TOK_DOCTYPE);
				doctype_eof(p);
			} else {
				reconsume(p, S_BEFORE_DOCTYPE_NAME);
			}
			break;
		case S_BEFORE_DOCTYPE_NAME:
			c = next(p);
			if (is_ws(c))
				break;
			new_token(p, TOK_DOCTYPE);
			if (c == '>') {
				t->force_quirks = 1;
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				doctype_eof(p);
			} else {
				t->has_name = 1;
				if (c == 0)
					buf_add(&t->name, REPLACEMENT, 3);
				else
					buf_putc(&t->name,
						is_upper(c) ? c + 32 : c);
				p->state = S_DOCTYPE_NAME;
			}
			break;
		case S_DOCTYPE_NAME:
			c = next(p);
			if (is_ws(c)) {
				p->state = S_AFTER_DOCTYPE_NAME;
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == 0) {
				buf_add(&t->name, REPLACEMENT, 3);
			} else if (c == EOF_C) {
				doctype_eof(p);
			} else {
				buf_putc(&t->name, is_upper(c) ? c + 32 : c);
			}
			break;
		case S_AFTER_DOCTYPE_NAME:
			c = next(p);
			if (is_ws(c))
				break;
			if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				doctype_eof(p);
			} else {
				p->pos--;
				if (lookahead(p, "public", 1)) {
					p->state = S_AFTER_DOCTYPE_PUBLIC_KEYWORD;
				} else if (lookahead(p, "system", 1)) {
					p->state = S_AFTER_DOCTYPE_SYSTEM_KEYWORD;
				} else {
					t->force_quirks = 1;
					p->state = S_BOGUS_DOCTYPE;
				}
			}
			break;
		case S_AFTER_DOCTYPE_PUBLIC_KEYWORD:
		case S_BEFORE_DOCTYPE_PUBLIC_ID:
		case S_AFTER_DOCTYPE_SYSTEM_KEYWORD:
		case S_BEFORE_DOCTYPE_SYSTEM_ID:
			q = p->state == S_AFTER_DOCTYPE_PUBLIC_KEYWORD ||
				p->state == S_BEFORE_DOCTYPE_PUBLIC_ID;
			c = next(p);
			if (is_ws(c)) {
				if (p->state == S_AFTER_DOCTYPE_PUBLIC_KEYWORD)
					p->state = S_BEFORE_DOCTYPE_PUBLIC_ID;
				else if (p->state == S_AFTER_DOCTYPE_SYSTEM_KEYWORD)
					p->state = S_BEFORE_DOCTYPE_SYSTEM_ID;
			} else if (c == '"' || c == '\'') {
				if (q) {
					t->has_public = 1;
					p->state = c == '"' ?
						S_DOCTYPE_PUBLIC_ID_DQ :
						S_DOCTYPE_PUBLIC_ID_SQ;
				} else {
					t->has_system = 1;
					p->state = c == '"' ?
						S_DOCTYPE_SYSTEM_ID_DQ :
						S_DOCTYPE_SYSTEM_ID_SQ;
				}
			} else if (c == '>') {
				t->force_quirks = 1;
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				doctype_eof(p);
			} else {
				t->force_quirks = 1;
				reconsume(p, S_BOGUS_DOCTYPE);
			}
			break;
		case S_DOCTYPE_PUBLIC_ID_DQ:
		case S_DOCTYPE_PUBLIC_ID_SQ:
		case S_DOCTYPE_SYSTEM_ID_DQ:
		case S_DOCTYPE_SYSTEM_ID_SQ:
			q = (p->state == S_DOCTYPE_PUBLIC_ID_DQ ||
			     p->state == S_DOCTYPE_SYSTEM_ID_DQ) ? '"' : '\'';
			id = p->state <= S_DOCTYPE_PUBLIC_ID_SQ ?
				&t->public_id : &t->system_id;
			c = next(p);
			if (c == q) {
				p->state = id == &t->public_id ?
					S_AFTER_DOCTYPE_PUBLIC_ID :
					S_AFTER_DOCTYPE_SYSTEM_ID;
			} else if (c == 0) {
				buf_add(id, REPLACEMENT, 3);
			} else if (c == '>') {
				t->force_quirks = 1;
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				doctype_eof(p);
			} else {
				buf_putc(id, c);
			}
			break;
		case S_AFTER_DOCTYPE_PUBLIC_ID:
		case S_BETWEEN_DOCTYPE_IDS:
			c = next(p);
			if (is_ws(c)) {
				p->state = S_BETWEEN_DOCTYPE_IDS;
			} else if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == '"' || c == '\'') {
				t->has_system = 1;
				p->state = c == '"' ? S_DOCTYPE_SYSTEM_ID_DQ :
					S_DOCTYPE_SYSTEM_ID_SQ;
			} else if (c == EOF_C) {
				doctype_eof(p);
			} else {
				t->force_quirks = 1;
				reconsume(p, S_BOGUS_DOCTYPE);
			}
			break;
		case S_AFTER_DOCTYPE_SYSTEM_ID:
			c = next(p);
			if (is_ws(c))
				break;
			if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				doctype_eof(p);
			} else {
				reconsume(p, S_BOGUS_DOCTYPE);
			}
			break;
		case S_BOGUS_DOCTYPE:
			c = next(p);
			if (c == '>') {
				p->state = S_DATA;
				emit_token(p);
			} else if (c == EOF_C) {
				emit_token(p);
				emit_eof(p);
			}
			break;
		case S_CDATA_SECTION:
			{
				size_t start = p->pos;

				while (p->pos < p->len && p->in[p->pos] != ']')
					p->pos++;
				buf_add(&p->text, p->in + start, p->pos - start);
			}
			c = next(p);
			if (c == ']')
				p->state = S_CDATA_SECTION_BRACKET;
			else if (c == EOF_C)
				emit_eof(p);
			break;
		case S_CDATA_SECTION_BRACKET:
			c = next(p);
			if (c == ']') {
				p->state = S_CDATA_SECTION_END;
			} else {
				emit_char(p, ']');
				reconsume(p, S_CDATA_SECTION);
			}
			break;
		case S_CDATA_SECTION_END:
			c = next(p);
			if (c == ']') {
				emit_char(p, ']');
			} else if (c == '>') {
				p->state = S_DATA;
			} else {
				emit_str(p, "]]");
				reconsume(p, S_CDATA_SECTION);
			}
			break;
		}
	}
}

void tokenizer_free(struct parser *p)
{
	int i;

	for (i = 0; i < p->tok.cap; i++) {
		buf_free(&p->tok.attrs[i].name);
		buf_free(&p->tok.attrs[i].value);
	}
	free(p->tok.attrs);
	buf_free(&p->tok.name);
	buf_free(&p->tok.data);
	buf_free(&p->tok.public_id);
	buf_free(&p->tok.system_id);
	buf_free(&p->dropped.name);
	buf_free(&p->dropped.value);
	buf_free(&p->text);
	buf_free(&p->tmp);
	buf_free(&p->last_start);
}
