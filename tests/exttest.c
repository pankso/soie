/*
 * Extension tests: the registry and its ABI checks, the image and
 * content hooks inside the engine.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <soie/soie.h>
#include <soie/ext.h>

static int failures, checks;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, \
			#cond); \
	} \
} while (0)

static void check_str(int line, const char *got, const char *want)
{
	checks++;
	if ((got == NULL) != (want == NULL) ||
	    (got && strcmp(got, want))) {
		failures++;
		fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__,
			line, got ? got : "(null)", want ? want : "(null)");
	}
}
#define STR(got, want) do { \
	char *s_ = (got); \
	check_str(__LINE__, s_, want); \
	free(s_); \
} while (0)

/* A fake image format: "FAKE" then w and h bytes, red pixels. */
static unsigned char *fake_decode(void *data, const unsigned char *b,
	size_t len, int *w, int *h)
{
	unsigned char *px;
	int i;

	(*(int *)data)++;
	if (len < 6 || memcmp(b, "FAKE", 4))
		return NULL;
	*w = b[4];
	*h = b[5];
	px = malloc((size_t)*w * (size_t)*h * 4);
	for (i = 0; i < *w * *h; i++)
		memcpy(px + i * 4, "\xff\0\0\xff", 4);
	return px;
}

static int fake_calls;
static struct soie_ext_image fake = {
	{ SOIE_EXT_ABI, sizeof(fake), SOIE_EXT_IMAGE, "fake", &fake_calls },
	fake_decode
};

/* Built against an older ABI: no decode hook within its size. */
static int old_calls;
static struct soie_ext_image old = {
	{ SOIE_EXT_ABI, sizeof(struct soie_ext), SOIE_EXT_IMAGE, "old",
	  &old_calls },
	fake_decode
};

static struct soie_ext_image wrong = {
	{ (SOIE_EXT_ABI_MAJOR + 1) << 16, sizeof(wrong), SOIE_EXT_IMAGE,
	  "wrong", NULL },
	fake_decode
};

static char *my_css(void *data, const char *url)
{
	(void)data;
	return url && strstr(url, "example") ? strdup("p { color: red }") :
		NULL;
}

static void my_document(void *data, soie_node *doc, const char *url)
{
	soie_node *el = soie_document_element(doc);

	(void)data;
	(void)url;
	soie_element_set_attr(el, "data-seen", "yes");
}

static struct soie_ext_content content = {
	{ SOIE_EXT_ABI, sizeof(content), SOIE_EXT_CONTENT, "content", NULL },
	my_css, my_document
};

static int fetch(void *data, const char *url, char **buf, size_t *len)
{
	(void)data;
	if (strcmp(url, "http://example.org/a.fake"))
		return -1;
	*buf = malloc(6);
	memcpy(*buf, "FAKE\x07\x05", 6);
	*len = 6;
	return 0;
}

static void test_registry(void)
{
	soie_style_options so;
	soie_node *doc, *img;
	float x, y, w, h;
	const char *html = "<p>a<img id=i src=a.fake>";
	char *css;

	CHECK(soie_ext_register(&wrong.base) == -1);
	CHECK(soie_ext_register(NULL) == -1);
	CHECK(soie_ext_register(&old.base) == 0);
	CHECK(soie_ext_register(&fake.base) == 0);
	CHECK(soie_ext_get(SOIE_EXT_IMAGE, 0) == &old.base);
	CHECK(soie_ext_get(SOIE_EXT_IMAGE, 1) == &fake.base);
	CHECK(soie_ext_get(SOIE_EXT_IMAGE, 2) == NULL);
	CHECK(soie_ext_get(SOIE_EXT_SCRIPT, 0) == NULL);

	/* an <img> in a format only the extension knows */
	doc = soie_html_parse(html, strlen(html), NULL);
	memset(&so, 0, sizeof(so));
	so.base_url = "http://example.org/";
	so.fetch = fetch;
	soie_style_document(doc, &so);
	soie_layout_document(doc, 800, 600);
	img = soie_node_last_child(soie_node_first_child(
		soie_node_last_child(soie_document_element(doc))));
	CHECK(img && !strcmp(soie_element_name(img), "img"));
	CHECK(!soie_element_rect(img, &x, &y, &w, &h));
	CHECK(w == 7 && h == 5);
	CHECK(fake_calls == 1 && old_calls == 0);
	soie_node_free(doc);

	/* content hooks */
	CHECK(soie_ext_register(&content.base) == 0);
	css = soie_ext_user_css("a { color: blue }", "http://example.org/");
	STR(css, "a { color: blue }\np { color: red }");
	CHECK(soie_ext_user_css(NULL, "http://other.org/") == NULL);
	doc = soie_html_parse(html, strlen(html), NULL);
	soie_ext_document(doc, "http://example.org/");
	CHECK(!strcmp(soie_element_get_attr(soie_document_element(doc),
		"data-seen"), "yes"));
	soie_node_free(doc);
}

static void test_response(void)
{
	soie_response r;

	memset(&r, 0, sizeof(r));
	r.headers = strdup("content-type: text/html; charset=utf-8\r\n"
		"set-cookie: a=1\r\nset-cookie: b=2\r\n");
	STR(soie_response_header(&r, "Content-Type"),
		"text/html; charset=utf-8");
	STR(soie_response_header(&r, "set-cookie"), "a=1");
	CHECK(soie_response_header(&r, "content") == NULL);
	soie_response_free(&r);
	CHECK(r.headers == NULL);
}

int main(void)
{
	setenv("SOIE_COOKIES", "", 1);	/* memory only */
	soie_fonts_stub(1);
	test_registry();
	test_response();
	printf("ext: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
