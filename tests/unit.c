/*
 * Unit tests for the public API: DOM mutation, charsets, entities.
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

int main(void)
{
	test_dom();
	test_parse_api();
	test_charsets();
	test_entities();
	test_deep();
	printf("unit: %s (%d failures)\n", failures ? "FAIL" : "ok", failures);
	return failures != 0;
}
