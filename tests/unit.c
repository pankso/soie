/*
 * Unit tests for the public API: DOM mutation, charsets, entities.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <soie/soie.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			#cond); \
		failures++; \
	} \
} while (0)

/* Dump of a parse. */
static char *parse_dump(const char *html, size_t len, const char *charset)
{
	soie_html_options opts = { NULL, 0 };
	soie_node *doc;
	char *d;

	opts.charset = charset;
	doc = soie_html_parse(html, len, &opts);
	d = soie_node_dump(doc, NULL);
	soie_node_free(doc);
	return d;
}

/* The first text node in document order. */
static const char *first_text(soie_node *n)
{
	soie_node *c;
	const char *s;

	if (soie_node_get_type(n) == SOIE_TEXT)
		return soie_node_data(n, NULL);
	for (c = soie_node_first_child(n); c; c = soie_node_next_sibling(c))
		if ((s = first_text(c)))
			return s;
	return NULL;
}

static int text_is(const char *html, size_t len, const char *charset,
	const char *expect, const char *expect_charset)
{
	soie_html_options opts = { NULL, 0 };
	soie_node *doc;
	const char *t;
	int ok;

	opts.charset = charset;
	doc = soie_html_parse(html, len, &opts);
	t = first_text(doc);
	ok = t && !strcmp(t, expect) &&
		!strcmp(soie_document_charset(doc), expect_charset);
	if (!ok)
		printf("  got \"%s\" in %s\n", t ? t : "(null)",
			soie_document_charset(doc));
	soie_node_free(doc);
	return ok;
}

#define TEXT_IS(s, cs, e, ecs) text_is(s, sizeof(s) - 1, cs, e, ecs)

static void test_dom(void)
{
	soie_node *doc = soie_document_new();
	soie_node *html = soie_element_new(doc, SOIE_NS_HTML, "html");
	soie_node *body = soie_element_new(doc, SOIE_NS_HTML, "body");
	soie_node *a = soie_element_new(doc, SOIE_NS_HTML, "a");
	soie_node *t = soie_text_new(doc, "hello", 5);
	soie_node *other = soie_document_new();
	soie_node *alien = soie_element_new(other, SOIE_NS_HTML, "p");
	char *d;

	CHECK(soie_node_append(doc, html) == 0);
	CHECK(soie_node_append(html, body) == 0);
	CHECK(soie_node_append(body, t) == 0);
	CHECK(soie_node_insert_before(body, a, t) == 0);
	CHECK(soie_node_first_child(body) == a);
	CHECK(soie_node_next_sibling(a) == t);
	CHECK(soie_node_parent(t) == body);
	CHECK(soie_document_element(doc) == html);

	/* hierarchy errors */
	CHECK(soie_node_append(t, a) == -1);
	CHECK(soie_node_append(body, html) == -1);
	CHECK(soie_node_append(body, alien) == -1);
	CHECK(soie_node_insert_before(html, a, t) == -1);

	/* attributes */
	CHECK(soie_element_set_attr(a, "href", "/x") == 0);
	CHECK(soie_element_set_attr(a, "id", "l") == 0);
	CHECK(soie_element_set_attr(a, "href", "/y") == 0);
	CHECK(soie_element_attr_count(a) == 2);
	CHECK(!strcmp(soie_element_get_attr(a, "href"), "/y"));
	CHECK(soie_element_remove_attr(a, "href") == 0);
	CHECK(soie_element_remove_attr(a, "href") == -1);
	CHECK(soie_element_get_attr(a, "href") == NULL);
	CHECK(!strcmp(soie_element_attr_name(a, 0), "id"));

	/* moving and text */
	CHECK(soie_node_append(body, a) == 0);
	CHECK(soie_node_last_child(body) == a);
	CHECK(soie_node_set_data(t, "bye", 3) == 0);
	d = soie_node_dump(doc, NULL);
	CHECK(!strcmp(d, "| <html>\n|   <body>\n|     \"bye\"\n"
		"|     <a>\n|       id=\"l\"\n"));
	free(d);

	/* remove, then free the detached node ourselves */
	soie_node_remove(a);
	CHECK(soie_node_parent(a) == NULL);
	CHECK(soie_node_last_child(body) == t);
	soie_node_free(a);

	/* fragments move their children */
	{
		soie_node *f = soie_document_fragment_new(doc);
		soie_node *x = soie_element_new(doc, SOIE_NS_HTML, "i");
		soie_node *y = soie_element_new(doc, SOIE_NS_HTML, "u");

		soie_node_append(f, x);
		soie_node_append(f, y);
		CHECK(soie_node_append(body, f) == 0);
		CHECK(soie_node_first_child(f) == NULL);
		CHECK(soie_node_last_child(body) == y);
		soie_node_free(f);
	}
	soie_node_free(doc);
	soie_node_free(alien);
	soie_node_free(other);
}

static void test_parse_api(void)
{
	soie_node *doc, *frag, *n;
	const char *html = "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\">"
		"<title>T</title><p class=a>x<svg viewbox='0 0 1 1'>";

	doc = soie_html_parse(html, strlen(html), NULL);
	CHECK(soie_document_quirks(doc) == SOIE_NO_QUIRKS);
	n = soie_node_first_child(doc);
	CHECK(soie_node_get_type(n) == SOIE_DOCTYPE);
	CHECK(!strcmp(soie_doctype_name(n), "html"));
	CHECK(!strcmp(soie_doctype_public_id(n), "-//W3C//DTD HTML 4.01//EN"));
	CHECK(!strcmp(soie_doctype_system_id(n), ""));
	n = soie_node_last_child(soie_document_element(doc));	/* body */
	n = soie_node_first_child(n);				/* p */
	CHECK(!strcmp(soie_element_get_attr(n, "class"), "a"));
	n = soie_node_last_child(n);				/* svg */
	CHECK(soie_element_ns(n) == SOIE_NS_SVG);
	CHECK(!strcmp(soie_element_attr_name(n, 0), "viewBox"));
	soie_node_free(doc);

	html = "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">";
	doc = soie_html_parse(html, strlen(html), NULL);
	CHECK(soie_document_quirks(doc) == SOIE_QUIRKS);
	soie_node_free(doc);
	doc = soie_html_parse("x", 1, NULL);
	CHECK(soie_document_quirks(doc) == SOIE_QUIRKS);
	soie_node_free(doc);

	doc = soie_document_new();
	frag = soie_html_parse_fragment(doc, SOIE_NS_HTML, "tr",
		"<td>a<td>b", 10, NULL);
	n = soie_node_first_child(frag);
	CHECK(n && !strcmp(soie_element_name(n), "td"));
	CHECK(n && soie_node_next_sibling(n) &&
		!strcmp(soie_element_name(soie_node_next_sibling(n)), "td"));
	soie_node_free(frag);
	soie_node_free(doc);

	/* template contents live in a fragment */
	doc = soie_html_parse("<template><b>x</b></template>", 29, NULL);
	n = soie_node_first_child(soie_node_first_child(
		soie_document_element(doc)));
	CHECK(n && !strcmp(soie_element_name(n), "template"));
	CHECK(n && soie_node_first_child(n) == NULL);
	CHECK(n && soie_node_get_type(soie_template_content(n)) ==
		SOIE_DOCUMENT_FRAGMENT);
	soie_node_free(doc);
}

static void test_charsets(void)
{
	/* BOM wins over everything */
	CHECK(TEXT_IS("\xef\xbb\xbf\xc3\xa9", "iso-8859-1",
		"\xc3\xa9", "UTF-8"));
	CHECK(TEXT_IS("\xff\xfe\xe9\x00\x3d\xd8\x00\xde", NULL,
		"\xc3\xa9\xf0\x9f\x98\x80", "UTF-16LE"));
	CHECK(TEXT_IS("\xfe\xff\x00\xe9", NULL, "\xc3\xa9", "UTF-16BE"));
	/* transport charset */
	CHECK(TEXT_IS("\xe9\x80", "latin1", "\xc3\xa9\xe2\x82\xac",
		"windows-1252"));
	CHECK(TEXT_IS("\xa4", "ISO-8859-15", "\xe2\x82\xac", "ISO-8859-15"));
	CHECK(TEXT_IS("\xb1", " Latin2 ", "\xc4\x85", "ISO-8859-2"));
	/* meta prescan */
	CHECK(TEXT_IS("<meta charset=\"windows-1252\">\xe9", NULL,
		"\xc3\xa9", "windows-1252"));
	CHECK(TEXT_IS("<!-- <meta charset=latin2> --><meta http-equiv="
		"Content-Type content='text/html; charset=iso-8859-15'>\xa4", NULL, "\xe2\x82\xac", "ISO-8859-15"));
	/* content charset without http-equiv is ignored */
	CHECK(TEXT_IS("<meta content='text/html; charset=latin2'>\xc3\xa9",
		NULL, "\xc3\xa9", "UTF-8"));
	/* meta utf-16 means utf-8 */
	CHECK(TEXT_IS("<meta charset=utf-16>\xc3\xa9", NULL,
		"\xc3\xa9", "UTF-8"));
	/* no declaration: valid UTF-8 or windows-1252 */
	CHECK(TEXT_IS("caf\xc3\xa9", NULL, "caf\xc3\xa9", "UTF-8"));
	CHECK(TEXT_IS("caf\xe9", NULL, "caf\xc3\xa9", "windows-1252"));
	/* invalid UTF-8: one U+FFFD per maximal subpart */
	CHECK(TEXT_IS("a\xe2\x82z\xc0\xafz\xed\xa0\x80", "utf-8",
		"a\xef\xbf\xbdz\xef\xbf\xbd\xef\xbf\xbdz"
		"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd", "UTF-8"));
	/* newlines */
	CHECK(TEXT_IS("a\r\nb\rc", NULL, "a\nb\nc", "UTF-8"));
}

static void test_entities(void)
{
	CHECK(TEXT_IS("&amp;&lt&notin;&notit;&#x1F600;&#128;&#0;&fjlig;",
		NULL, "&<\xe2\x88\x89\xc2\xacit;\xf0\x9f\x98\x80"
		"\xe2\x82\xac\xef\xbf\xbd" "fj", "UTF-8"));
	{
		static const char a[] = "<a href='?a=1&copy=2&amp;b=&copy'>";
		char *d = parse_dump(a, sizeof(a) - 1, NULL);

		CHECK(strstr(d, "href=\"?a=1&copy=2&b=\xc2\xa9\"") != NULL);
		free(d);
	}
}

static void test_url(void)
{
	/* RFC 3986 5.4, plus the WHATWG tolerances */
	static const char *const cases[][2] = {
		{ "g", "http://a/b/c/g" }, { "./g", "http://a/b/c/g" },
		{ "g/", "http://a/b/c/g/" }, { "/g", "http://a/g" },
		{ "//g", "http://g" }, { "?y", "http://a/b/c/d;p?y" },
		{ "g?y", "http://a/b/c/g?y" }, { "#s", "http://a/b/c/d;p?q#s" },
		{ "g#s", "http://a/b/c/g#s" }, { "../g", "http://a/b/g" },
		{ "../..", "http://a/" }, { "../../g", "http://a/g" },
		{ "../../../g", "http://a/g" }, { "", "http://a/b/c/d;p?q" },
		{ ".", "http://a/b/c/" }, { "..", "http://a/b/" },
		{ "g;x=1/../y", "http://a/b/c/y" }, { "http:g", "http://a/b/c/g" },
		{ "https:x/./y", "https:x/y" }, { " \tg\n ", "http://a/b/c/g" },
		{ "\\g", "http://a/g" }, { "mailto:me@x", "mailto:me@x" },
		{ NULL, NULL }
	};
	int i;

	for (i = 0; cases[i][0]; i++) {
		char *r = soie_url_resolve("http://a/b/c/d;p?q", cases[i][0]);

		if (strcmp(r, cases[i][1]))
			printf("  url \"%s\": got %s, expected %s\n",
				cases[i][0], r, cases[i][1]);
		CHECK(!strcmp(r, cases[i][1]));
		free(r);
	}
}

static void test_style_api(void)
{
	soie_node *doc;
	char *v;
	static const char html[] = "<!DOCTYPE html><style>p{--x: 3px;"
		"margin-top:var(--x)}</style><p id=p>t";

	doc = soie_html_parse(html, sizeof(html) - 1, NULL);
	CHECK(soie_style_document(doc, NULL) == 0);
	{
		soie_node *p = soie_node_first_child(soie_node_last_child(
			soie_document_element(doc)));

		v = soie_computed_value(p, NULL, "margin-top");
		CHECK(v && !strcmp(v, "3px"));
		free(v);
		v = soie_computed_value(p, NULL, "--x");
		CHECK(v && !strcmp(v, "3px"));
		free(v);
		CHECK(soie_computed_value(p, NULL, "no-such-property") == NULL);
		CHECK(soie_computed_value(p, "before", "color") == NULL);
	}
	/* a second run starts from scratch */
	CHECK(soie_style_document(doc, NULL) == 0);
	soie_node_free(doc);
}

/* Another viewport: rules kept when the media queries say the same,
 * chosen again when one changes, viewport units computed again. */
static void test_resize(void)
{
	static const char html[] = "<!DOCTYPE html><style>"
		"p{color:blue;width:50vw}"
		"@media (min-width:800px){p{color:red}}</style>"
		"<style media='(max-width:500px)'>p{margin-top:7px}</style>"
		"<p id=p>t";
	static const struct { int w; const char *color, *width, *mt; } want[] = {
		{ 1000, "rgb(255, 0, 0)", "500px", "16px" },
		{ 900, "rgb(255, 0, 0)", "450px", "16px" },
		{ 600, "rgb(0, 0, 255)", "300px", "16px" },
		{ 400, "rgb(0, 0, 255)", "200px", "7px" },
		{ 1000, "rgb(255, 0, 0)", "500px", "16px" },
	};
	soie_style_options o;
	soie_node *doc, *p;
	char *v;
	size_t i, n;
	soie_node **q;

	doc = soie_html_parse(html, sizeof(html) - 1, NULL);
	q = soie_query(doc, "#p", 1, &n);
	p = q && n ? q[0] : NULL;
	free(q);
	CHECK(p != NULL);
	memset(&o, 0, sizeof(o));
	for (i = 0; p && i < sizeof(want) / sizeof(*want); i++) {
		o.width = want[i].w;
		o.height = 600;
		CHECK(soie_style_document(doc, &o) == 0);
		v = soie_computed_value(p, NULL, "color");
		CHECK(v && !strcmp(v, want[i].color));
		free(v);
		v = soie_computed_value(p, NULL, "width");
		CHECK(v && !strcmp(v, want[i].width));
		free(v);
		v = soie_computed_value(p, NULL, "margin-top");
		CHECK(v && !strcmp(v, want[i].mt));
		free(v);
	}
	soie_node_free(doc);
}

/* A resize skips the cascade when no computed value has viewport
 * units; units met later, by a restyle of what a script changed, count
 * as well. */
static void test_resize_late_vw(void)
{
	static const char html[] = "<!DOCTYPE html><style>"
		"p{width:200px}.v{width:calc(10px + 10vw)}</style><p id=p>t";
	soie_style_options o;
	soie_node *doc, *p;
	char *v;
	size_t n;
	soie_node **q;

	doc = soie_html_parse(html, sizeof(html) - 1, NULL);
	q = soie_query(doc, "#p", 1, &n);
	p = q && n ? q[0] : NULL;
	free(q);
	CHECK(p != NULL);
	if (!p)
		return;
	memset(&o, 0, sizeof(o));
	o.width = 1000;
	o.height = 600;
	CHECK(soie_style_document(doc, &o) == 0);
	soie_element_set_attr(p, "class", "v");
	CHECK(soie_style_document(doc, &o) == 0);
	v = soie_computed_value(p, NULL, "width");
	CHECK(v && !strcmp(v, "110px"));
	free(v);
	o.width = 500;
	CHECK(soie_style_document(doc, &o) == 0);
	v = soie_computed_value(p, NULL, "width");
	CHECK(v && !strcmp(v, "60px"));
	free(v);
	soie_node_free(doc);
}

/* Invalid calc()s with a tree inside (min() of a percentage cannot
 * fold): parsing them failed freeing the tree twice (YouTube's CSS). */
static void test_bad_calc(void)
{
	static const char html[] = "<!DOCTYPE html><style>p{"
		"width:calc(min(1px, 2%) + );"
		"width:calc(min(1px, 2%) +3px);"
		"width:calc(max(1px, 2%) 4px);"
		"width:calc((min(1px, 2%)) 4px);"
		"width:calc(min(1px, 2%) * min(3%, 4px));"
		"width:clamp(1px, calc(min(1px, 2%) + ), 3px);"
		"height:calc(min(10px, 20%))}</style><p id=p>t";
	soie_node *doc = soie_html_parse(html, sizeof(html) - 1, NULL);

	CHECK(soie_style_document(doc, NULL) == 0);
	soie_node_free(doc);
}

/* What script engines use: queries, serialization, clones, updates. */
static void test_script_api(void)
{
	static const char html[] = "<!DOCTYPE html><style>.w{width:50px}"
		"</style><div id=a class=x><p>1</p><p class=x>2 &amp; "
		"<br>&nbsp;</p><template><i>t</i></template></div>";
	soie_node *doc = soie_html_parse(html, sizeof(html) - 1, NULL);
	soie_node *a, *c, **v;
	size_t n;
	char *s;
	float x, y, w, h;
	unsigned long gen;
	soie_style_options so;

	v = soie_query(doc, ".x", 0, &n);
	CHECK(v && n == 2 && !v[2]);
	a = v ? v[0] : NULL;
	free(v);
	CHECK(a && !strcmp(soie_element_get_attr(a, "id"), "a"));
	v = soie_query(doc, "p", 1, &n);
	CHECK(v && n == 1);
	free(v);
	CHECK(soie_query(doc, "p[", 0, &n) == NULL);
	CHECK(soie_element_matches(a, "body > div.x") == 1);
	CHECK(soie_element_matches(a, "p") == 0);
	CHECK(soie_element_matches(a, "!!") == -1);

	s = soie_node_serialize(a, 0, NULL);
	CHECK(!strcmp(s, "<p>1</p><p class=\"x\">2 &amp; <br>&nbsp;</p>"
		"<template><i>t</i></template>"));
	free(s);
	c = soie_node_clone(a, 1);
	s = soie_node_serialize(c, 1, NULL);
	CHECK(!strncmp(s, "<div id=\"a\" class=\"x\"><p>1</p>", 30));
	CHECK(strstr(s, "<template><i>t</i></template></div>") != NULL);
	free(s);
	CHECK(soie_node_parent(c) == NULL);
	soie_node_remove(a);	/* detached: still the document's */

	/* restyle and relayout after a change */
	soie_node_append(soie_node_last_child(soie_document_element(doc)), a);
	memset(&so, 0, sizeof(so));
	so.width = 300;
	so.height = 200;
	CHECK(soie_style_document(doc, &so) == 0);
	CHECK(soie_layout_document(doc, 300, 200) == 0);
	CHECK(soie_document_update(doc) == 0);
	CHECK(!soie_element_rect(a, &x, &y, &w, &h) && w == 284);
	gen = soie_document_generation(doc);
	soie_element_set_attr(a, "class", "w");
	CHECK(soie_document_generation(doc) != gen);
	CHECK(soie_document_update(doc) == 1);
	CHECK(!soie_element_rect(a, &x, &y, &w, &h) && w == 50);
	CHECK(soie_media_matches(doc, "(max-width: 400px)") == 1);
	CHECK(soie_media_matches(doc, "print") == 0);

	/* a new style sheet: the update must keep the options it had */
	{
		soie_node *st = soie_element_new(doc, SOIE_NS_HTML, "style");
		const char css[] = ".w{width:70px}";

		soie_node_append(st, soie_text_new(doc, css, sizeof(css) - 1));
		soie_node_append(soie_document_element(doc), st);
	}
	CHECK(soie_document_update(doc) == 1);
	CHECK(!soie_element_rect(a, &x, &y, &w, &h) && w == 70);
	CHECK(soie_media_matches(doc, "(max-width: 400px)") == 1);
	soie_node_free(doc);
}

static void test_select(void)
{
	static const char html[] = "<style>body{margin:0;font:16px/20px x}"
		"p{margin:0}</style><p>hello world</p>"
		"<p>second line<br>third</p><ul><li>item</ul>";
	soie_node *doc = soie_html_parse(html, sizeof(html) - 1, NULL);
	soie_style_options so;
	float r[8];
	char *t;
	/* 8 px a character, 20 px a line */
	const float in[] = { 17, 10, 35, 10 }, word[] = { 50, 10, 50, 10 },
		para[] = { 5, 30, 5, 30 }, across[] = { 16, 50, 48, 10 },
		none[] = { 17, 10, 17, 10 }, below[] = { 5, 1000, 5, 1000 };

	memset(&so, 0, sizeof(so));
	so.width = 400;
	so.height = 300;
	CHECK(soie_style_document(doc, &so) == 0);
	CHECK(soie_layout_document(doc, 400, 300) == 0);
	CHECK(soie_select(doc, in, SOIE_SELECT_CHAR, r, 2) == 1 &&
		r[0] == 16 && r[2] == 16);
	t = soie_select_text(doc, in, SOIE_SELECT_CHAR);
	CHECK(t && !strcmp(t, "ll"));
	free(t);
	t = soie_select_text(doc, word, SOIE_SELECT_WORD);
	CHECK(t && !strcmp(t, "world"));
	free(t);
	t = soie_select_text(doc, para, SOIE_SELECT_BLOCK);
	CHECK(t && !strcmp(t, "second line\nthird"));
	free(t);
	t = soie_select_text(doc, across, SOIE_SELECT_CHAR);
	CHECK(t && !strcmp(t, "world\nsecond line\nth"));
	free(t);
	/* no list marker */
	t = soie_select_text(doc, none, SOIE_SELECT_ALL);
	CHECK(t && !strcmp(t, "hello world\nsecond line\nthird\nitem"));
	free(t);
	CHECK(soie_select(doc, none, SOIE_SELECT_CHAR, r, 2) == 0);
	CHECK(soie_select_text(doc, none, SOIE_SELECT_CHAR) == NULL);
	t = soie_select_text(doc, below, SOIE_SELECT_BLOCK);
	CHECK(t && !strcmp(t, "item"));
	free(t);
	soie_node_free(doc);
}

static void test_deep(void)
{
	/* Deep nesting must not exhaust the stack when freeing. */
	size_t n = 100000, i;
	char *s = malloc(n * 5);
	soie_node *doc;

	for (i = 0; i < n; i++)
		memcpy(s + i * 5, "<div>", 5);
	doc = soie_html_parse(s, n * 5, NULL);
	CHECK(doc != NULL);
	soie_node_free(doc);
	free(s);
}

/* head, then unit n times, then tail: a hostile page */
static char *repeat(const char *head, const char *unit, size_t n,
	const char *tail, size_t *len)
{
	size_t h = strlen(head), u = strlen(unit), t = strlen(tail), i;
	char *s = malloc(h + u * n + t + 1);

	memcpy(s, head, h);
	for (i = 0; i < n; i++)
		memcpy(s + h + i * u, unit, u);
	memcpy(s + h + u * n, tail, t + 1);
	*len = h + u * n + t;
	return s;
}

/* Deep nesting of a page must not exhaust the stack anywhere else:
 * CSS blocks and functions, SVG groups, the hit test. */
static void test_hostile_depth(void)
{
	static const char *const pages[][3] = {
		{ "<style>", "a{", "</style><p>t" },
		{ "<p style=\"width:calc(", "(", "1px\">t" },
		{ "<style>", ":not(", "p{color:red}</style><p>t" },
		{ "<svg width=20 height=20>", "<g>", "<rect width=9 height=9>" },
		{ "", "<div>", "t" },
	};
	unsigned char px[20 * 20 * 4];
	size_t i, len;

	for (i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
		char *s = repeat(pages[i][0], pages[i][1], i < 4 ? 200000 :
			5000, pages[i][2], &len);
		soie_node *doc = soie_html_parse(s, len, NULL);

		CHECK(soie_style_document(doc, NULL) == 0);
		soie_layout_document(doc, 20, 20);
		soie_paint(doc, px, 20, 20, 20 * 4, 0, 0);
		soie_hit_test(doc, 5, 5);
		soie_node_free(doc);
		free(s);
	}
}

int main(void)
{
	soie_fonts_stub(1);
	test_dom();
	test_parse_api();
	test_charsets();
	test_entities();
	test_url();
	test_style_api();
	test_resize();
	test_resize_late_vw();
	test_bad_calc();
	test_script_api();
	test_select();
	test_deep();
	test_hostile_depth();
	printf("unit: %s (%d failures)\n", failures ? "FAIL" : "ok", failures);
	return failures != 0;
}
