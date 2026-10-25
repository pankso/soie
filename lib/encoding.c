/*
 * Character encodings: sniffing (BOM, transport hint, <meta> prescan)
 * and decoding to UTF-8 with normalized newlines, which is all the
 * tokenizer ever sees.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>
#include "internal.h"

/* windows-1252 0x80-0x9f; also used by numeric character references */
const unsigned short cp1252_c1[32] = {
	0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
	0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
	0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
	0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178
};

static const unsigned short iso8859_2_high[96] = {
	0x00a0, 0x0104, 0x02d8, 0x0141, 0x00a4, 0x013d, 0x015a, 0x00a7,
	0x00a8, 0x0160, 0x015e, 0x0164, 0x0179, 0x00ad, 0x017d, 0x017b,
	0x00b0, 0x0105, 0x02db, 0x0142, 0x00b4, 0x013e, 0x015b, 0x02c7,
	0x00b8, 0x0161, 0x015f, 0x0165, 0x017a, 0x02dd, 0x017e, 0x017c,
	0x0154, 0x00c1, 0x00c2, 0x0102, 0x00c4, 0x0139, 0x0106, 0x00c7,
	0x010c, 0x00c9, 0x0118, 0x00cb, 0x011a, 0x00cd, 0x00ce, 0x010e,
	0x0110, 0x0143, 0x0147, 0x00d3, 0x00d4, 0x0150, 0x00d6, 0x00d7,
	0x0158, 0x016e, 0x00da, 0x0170, 0x00dc, 0x00dd, 0x0162, 0x00df,
	0x0155, 0x00e1, 0x00e2, 0x0103, 0x00e4, 0x013a, 0x0107, 0x00e7,
	0x010d, 0x00e9, 0x0119, 0x00eb, 0x011b, 0x00ed, 0x00ee, 0x010f,
	0x0111, 0x0144, 0x0148, 0x00f3, 0x00f4, 0x0151, 0x00f6, 0x00f7,
	0x0159, 0x016f, 0x00fa, 0x0171, 0x00fc, 0x00fd, 0x0163, 0x02d9
};

static const char *const labels[] = {
	"UTF-8",
	"unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8",
	"utf8", "x-unicode20utf8", NULL,
	"windows-1252",
	"ansi_x3.4-1968", "ascii", "cp1252", "cp819", "csisolatin1",
	"ibm819", "iso-8859-1", "iso-ir-100", "iso8859-1", "iso88591",
	"iso_8859-1", "iso_8859-1:1987", "l1", "latin1", "us-ascii",
	"windows-1252", "x-cp1252", "x-user-defined", NULL,
	"ISO-8859-15",
	"csisolatin9", "iso-8859-15", "iso8859-15", "iso885915",
	"iso_8859-15", "l9", "latin-9", NULL,
	"ISO-8859-2",
	"csisolatin2", "iso-8859-2", "iso-ir-101", "iso8859-2", "iso88592",
	"iso_8859-2", "iso_8859-2:1987", "l2", "latin2", NULL,
	"UTF-16LE",
	"csunicode", "iso-10646-ucs-2", "ucs-2", "unicode", "unicodefeff",
	"utf-16", "utf-16le", NULL,
	"UTF-16BE",
	"unicodefffe", "utf-16be", NULL,
	NULL
};

static int is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Canonical name for an encoding label, NULL if unsupported. */
static const char *charset_resolve(const char *s, size_t len)
{
	const char *const *l = labels, *name;
	char buf[32];
	size_t i;

	while (len && is_space((unsigned char)*s))
		s++, len--;
	while (len && is_space((unsigned char)s[len - 1]))
		len--;
	if (!len || len >= sizeof(buf))
		return NULL;
	for (i = 0; i < len; i++)
		buf[i] = (char)lower((unsigned char)s[i]);
	buf[len] = '\0';
	while (*l) {
		name = *l++;
		for (; *l; l++)
			if (!strcmp(*l, buf))
				return name;
		l++;
	}
	return NULL;
}

/* Prescan helpers, after the "prescan a byte stream" algorithm. */

struct scan {
	const unsigned char *s;
	size_t pos, len;
};

/* Read one attribute; returns 0 at '>' or the end. */
static int scan_attr(struct scan *p, struct buf *name, struct buf *value)
{
	int c, q;

	buf_reset(name);
	buf_reset(value);
	while (p->pos < p->len && (is_space(p->s[p->pos]) || p->s[p->pos] == '/'))
		p->pos++;
	if (p->pos >= p->len || p->s[p->pos] == '>')
		return 0;
	for (;;) {
		if (p->pos >= p->len)
			return 0;
		c = p->s[p->pos];
		if (c == '=' && name->len) {
			p->pos++;
			break;
		}
		if (is_space(c)) {
			while (p->pos < p->len && is_space(p->s[p->pos]))
				p->pos++;
			if (p->pos >= p->len || p->s[p->pos] != '=')
				return 1;
			p->pos++;
			break;
		}
		if (c == '/' || c == '>')
			return 1;
		buf_putc(name, lower(c));
		p->pos++;
	}
	while (p->pos < p->len && is_space(p->s[p->pos]))
		p->pos++;
	if (p->pos >= p->len)
		return 0;
	q = p->s[p->pos];
	if (q == '"' || q == '\'') {
		p->pos++;
		while (p->pos < p->len && p->s[p->pos] != q)
			buf_putc(value, lower(p->s[p->pos++]));
		p->pos++;
		return 1;
	}
	if (q == '>')
		return 1;
	while (p->pos < p->len && !is_space(p->s[p->pos]) && p->s[p->pos] != '>')
		buf_putc(value, lower(p->s[p->pos++]));
	return 1;
}

/* "Extract a character encoding from a meta element" */
static const char *charset_from_content(const char *s, size_t len)
{
	size_t i = 0, start;
	int q;

	for (;;) {
		for (; i + 7 <= len; i++)
			if (!memcmp(s + i, "charset", 7))
				break;
		if (i + 7 > len)
			return NULL;
		i += 7;
		while (i < len && is_space((unsigned char)s[i]))
			i++;
		if (i < len && s[i] == '=')
			break;
	}
	i++;
	while (i < len && is_space((unsigned char)s[i]))
		i++;
	if (i >= len)
		return NULL;
	q = s[i];
	if (q == '"' || q == '\'') {
		start = ++i;
		while (i < len && s[i] != q)
			i++;
		return i < len ? charset_resolve(s + start, i - start) : NULL;
	}
	start = i;
	while (i < len && !is_space((unsigned char)s[i]) && s[i] != ';')
		i++;
	return charset_resolve(s + start, i - start);
}

static const char *meta_charset(struct scan *p, struct buf *name,
	struct buf *value)
{
	const char *charset = NULL;
	int got_pragma = 0, need_pragma = -1, seen = 0;

	while (scan_attr(p, name, value)) {
		if (!strcmp(name->s, "http-equiv") && !(seen & 1)) {
			seen |= 1;
			if (!strcmp(value->s ? value->s : "", "content-type"))
				got_pragma = 1;
		} else if (!strcmp(name->s, "content") && !(seen & 2)) {
			seen |= 2;
			if (!charset && value->s) {
				charset = charset_from_content(value->s,
					value->len);
				if (charset)
					need_pragma = 1;
			}
		} else if (!strcmp(name->s, "charset") && !(seen & 4)) {
			seen |= 4;
			charset = value->s ?
				charset_resolve(value->s, value->len) : NULL;
			need_pragma = 0;
		}
	}
	if (need_pragma < 0 || (need_pragma == 1 && !got_pragma) || !charset)
		return NULL;
	if (!strncmp(charset, "UTF-16", 6))
		return "UTF-8";
	return charset;
}

static const char *prescan(const unsigned char *s, size_t len)
{
	struct scan p;
	struct buf name = { 0 }, value = { 0 };
	const char *charset = NULL;

	p.s = s;
	p.len = len > 1024 ? 1024 : len;
	p.pos = 0;
	while (p.pos < p.len && !charset) {
		const unsigned char *c = s + p.pos;
		size_t left = p.len - p.pos;

		if (left >= 4 && !memcmp(c, "<!--", 4)) {
			p.pos += 2;
			while (p.pos + 3 <= p.len &&
			       memcmp(s + p.pos, "-->", 3))
				p.pos++;
			p.pos += 3;
		} else if (left >= 6 && c[0] == '<' &&
			   lower(c[1]) == 'm' && lower(c[2]) == 'e' &&
			   lower(c[3]) == 't' && lower(c[4]) == 'a' &&
			   (is_space(c[5]) || c[5] == '/')) {
			p.pos += 6;
			charset = meta_charset(&p, &name, &value);
		} else if (left >= 2 && c[0] == '<' &&
			   ((lower(c[1]) >= 'a' && lower(c[1]) <= 'z') ||
			    (c[1] == '/' && left >= 3 &&
			     lower(c[2]) >= 'a' && lower(c[2]) <= 'z'))) {
			while (p.pos < p.len && !is_space(s[p.pos]) &&
			       s[p.pos] != '>')
				p.pos++;
			while (scan_attr(&p, &name, &value))
				;
			p.pos++;
		} else if (left >= 2 && c[0] == '<' &&
			   (c[1] == '!' || c[1] == '/' || c[1] == '?')) {
			while (p.pos < p.len && s[p.pos] != '>')
				p.pos++;
			p.pos++;
		} else {
			p.pos++;
		}
	}
	buf_free(&name);
	buf_free(&value);
	return charset;
}

/* Length of a valid UTF-8 sequence at s, 0 if invalid or truncated. */
static size_t utf8_seq(const unsigned char *s, size_t left)
{
	unsigned char lo = 0x80, hi = 0xbf;
	size_t need, i;

	if (s[0] < 0x80)
		return 1;
	if (s[0] >= 0xc2 && s[0] <= 0xdf)
		need = 1;
	else if (s[0] >= 0xe0 && s[0] <= 0xef)
		need = 2;
	else if (s[0] >= 0xf0 && s[0] <= 0xf4)
		need = 3;
	else
		return 0;
	if (s[0] == 0xe0)
		lo = 0xa0;
	else if (s[0] == 0xed)
		hi = 0x9f;
	else if (s[0] == 0xf0)
		lo = 0x90;
	else if (s[0] == 0xf4)
		hi = 0x8f;
	for (i = 1; i <= need; i++) {
		if (i >= left || s[i] < lo || s[i] > hi)
			return 0;
		lo = 0x80;
		hi = 0xbf;
	}
	return need + 1;
}

/* Invalid bytes of a broken sequence ("maximal subpart" length). */
static size_t utf8_bad(const unsigned char *s, size_t left)
{
	unsigned char lo = 0x80, hi = 0xbf;
	size_t need, i;

	if (s[0] >= 0xc2 && s[0] <= 0xdf)
		need = 1;
	else if (s[0] >= 0xe0 && s[0] <= 0xef)
		need = 2;
	else if (s[0] >= 0xf0 && s[0] <= 0xf4)
		need = 3;
	else
		return 1;
	if (s[0] == 0xe0)
		lo = 0xa0;
	else if (s[0] == 0xed)
		hi = 0x9f;
	else if (s[0] == 0xf0)
		lo = 0x90;
	else if (s[0] == 0xf4)
		hi = 0x8f;
	for (i = 1; i <= need && i < left; i++) {
		if (s[i] < lo || s[i] > hi)
			break;
		lo = 0x80;
		hi = 0xbf;
	}
	return i;
}

/* Valid UTF-8 in the first 64 KiB (a cut sequence at the end is ok). */
static int looks_utf8(const unsigned char *s, size_t len)
{
	size_t i = 0, n;

	if (len > 65536)
		len = 65536;
	while (i < len) {
		n = utf8_seq(s + i, len - i);
		if (!n)
			return len - i < 4 && len == 65536;
		i += n;
	}
	return 1;
}

const char *charset_sniff(const unsigned char *s, size_t len,
	const char *hint, size_t *bom)
{
	const char *cs;

	*bom = 0;
	if (len >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) {
		*bom = 3;
		return "UTF-8";
	}
	if (len >= 2 && s[0] == 0xfe && s[1] == 0xff) {
		*bom = 2;
		return "UTF-16BE";
	}
	if (len >= 2 && s[0] == 0xff && s[1] == 0xfe) {
		*bom = 2;
		return "UTF-16LE";
	}
	if (hint && (cs = charset_resolve(hint, strlen(hint))))
		return cs;
	if ((cs = prescan(s, len)))
		return cs;
	return looks_utf8(s, len) ? "UTF-8" : "windows-1252";
}

static void decode_utf8(const unsigned char *s, size_t len, struct buf *out)
{
	size_t i = 0, n, start;

	buf_grow(out, len);
	while (i < len) {
		start = i;
		while (i < len && (n = utf8_seq(s + i, len - i)))
			i += n;
		buf_add(out, (const char *)s + start, i - start);
		if (i < len) {
			buf_add(out, "\xef\xbf\xbd", 3);
			i += utf8_bad(s + i, len - i);
		}
	}
}

static void decode_utf16(const unsigned char *s, size_t len, int be,
	struct buf *out)
{
	size_t i;
	unsigned long u, v;

	for (i = 0; i + 1 < len; i += 2) {
		u = be ? (unsigned)s[i] << 8 | s[i + 1] :
			(unsigned)s[i + 1] << 8 | s[i];
		if (u >= 0xd800 && u <= 0xdbff && i + 3 < len) {
			v = be ? (unsigned)s[i + 2] << 8 | s[i + 3] :
				(unsigned)s[i + 3] << 8 | s[i + 2];
			if (v >= 0xdc00 && v <= 0xdfff) {
				u = 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00);
				i += 2;
			} else {
				u = 0xfffd;
			}
		} else if (u >= 0xd800 && u <= 0xdfff) {
			u = 0xfffd;
		}
		buf_put_utf8(out, u);
	}
	if (i < len)
		buf_add(out, "\xef\xbf\xbd", 3);
}

static void decode_single(const char *cs, const unsigned char *s, size_t len,
	struct buf *out)
{
	int latin2 = !strcmp(cs, "ISO-8859-2");
	int latin9 = !strcmp(cs, "ISO-8859-15");
	size_t i;
	unsigned long u;

	buf_grow(out, len + len / 2);
	for (i = 0; i < len; i++) {
		u = s[i];
		if (u >= 0x80 && u < 0xa0 && !latin2 && !latin9) {
			u = cp1252_c1[u - 0x80];
		} else if (u >= 0xa0 && latin2) {
			u = iso8859_2_high[u - 0xa0];
		} else if (latin9) {
			switch (u) {
			case 0xa4: u = 0x20ac; break;
			case 0xa6: u = 0x0160; break;
			case 0xa8: u = 0x0161; break;
			case 0xb4: u = 0x017d; break;
			case 0xb8: u = 0x017e; break;
			case 0xbc: u = 0x0152; break;
			case 0xbd: u = 0x0153; break;
			case 0xbe: u = 0x0178; break;
			}
		}
		buf_put_utf8(out, u);
	}
}

/* CR LF and lone CR become LF, in place. */
static void normalize_newlines(struct buf *b)
{
	size_t i, j;

	if (!b->s || !memchr(b->s, '\r', b->len))
		return;
	for (i = j = 0; i < b->len; i++) {
		if (b->s[i] == '\r') {
			b->s[j++] = '\n';
			if (i + 1 < b->len && b->s[i + 1] == '\n')
				i++;
		} else {
			b->s[j++] = b->s[i];
		}
	}
	b->len = j;
	b->s[j] = '\0';
}

void charset_decode(const char *cs, const unsigned char *s, size_t len,
	struct buf *out)
{
	buf_reset(out);
	if (!strcmp(cs, "UTF-8"))
		decode_utf8(s, len, out);
	else if (!strcmp(cs, "UTF-16LE"))
		decode_utf16(s, len, 0, out);
	else if (!strcmp(cs, "UTF-16BE"))
		decode_utf16(s, len, 1, out);
	else
		decode_single(cs, s, len, out);
	buf_putc(out, 0);	/* make sure out->s exists */
	out->len--;
	normalize_newlines(out);
}
