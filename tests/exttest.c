/*
 * Extension tests: the registry and its ABI checks, the image and
 * content hooks inside the engine, and the modules of ext/ (cookies,
 * block, webp when built), loaded the way hosts load them.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>
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

#define GET(url, fp) soie_cookies_get(url, fp, 0)
#define SET(url, line) soie_cookies_set(url, url, line, 0)

static void test_cookies(void)
{
	const char *u = "https://www.example.com/a/b";

	CHECK(soie_ext_load("ext/cookies.so") == 0);
	CHECK(GET(u, u) == NULL);
	SET(u, "a=1");
	STR(GET(u, u), "a=1");
	/* default path is the directory of the request: /a */
	STR(GET("https://www.example.com/a", NULL), "a=1");
	CHECK(GET("https://www.example.com/", NULL) == NULL);
	CHECK(GET("https://www.example.com/ab", NULL) == NULL);
	/* host-only without Domain */
	CHECK(GET("https://x.www.example.com/a/", NULL) == NULL);

	SET(u, "b=2; Path=/; Domain=example.com");
	STR(GET("https://x.example.com/", NULL), "b=2");
	STR(GET(u, NULL), "a=1; b=2");	/* longer path first */
	/* Domain that does not cover the host, public suffixes */
	SET(u, "c=3; Domain=other.com");
	SET(u, "d=4; Domain=com");
	SET("https://shop.bbc.co.uk/", "e=5; Domain=co.uk");
	SET("https://shop.bbc.co.uk/", "f=6; Domain=bbc.co.uk; Path=/");
	STR(GET("https://www.example.com/", NULL), "b=2");
	STR(GET("https://news.bbc.co.uk/", NULL), "f=6");
	CHECK(GET("https://other.co.uk/", NULL) == NULL);

	/* Secure and HttpOnly */
	SET(u, "s=1; Secure; Path=/");
	SET("http://www.example.com/", "t=1; Secure; Path=/");
	SET(u, "h=1; HttpOnly; Path=/");
	STR(GET("https://www.example.com/", NULL), "b=2; s=1; h=1");
	STR(GET("http://www.example.com/", NULL), "b=2; h=1");
	STR(soie_cookies_get("https://www.example.com/", NULL, 1),
		"b=2; s=1");
	soie_cookies_set(u, u, "h=2; Path=/", 1);	/* script vs HttpOnly */
	STR(GET("https://www.example.com/", NULL), "b=2; s=1; h=1");
	soie_cookies_set(u, u, "j=1; Path=/; HttpOnly", 1);
	CHECK(!strstr(GET("https://www.example.com/", NULL), "j="));

	/* prefixes */
	SET(u, "__Host-x=1; Path=/");
	SET(u, "__Host-y=1; Secure; Path=/; Domain=example.com");
	SET(u, "__Host-z=1; Secure; Path=/");
	SET(u, "__Secure-w=1");
	STR(GET("https://www.example.com/", NULL), "b=2; s=1; h=1; "
		"__Host-z=1");

	/* replacing, expiring */
	SET(u, "b=3; Path=/; Domain=example.com");
	SET(u, "s=; Path=/; Secure; Max-Age=0");
	SET(u, "h=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
	STR(GET("https://www.example.com/", NULL), "b=3; __Host-z=1");
	SET(u, "old=1; Path=/; Expires=Wed, 21 Oct 2015 07:28:00 GMT");
	SET(u, "new=1; Path=/; expires=Fri, 31-Dec-2100 23:59:59 GMT");
	STR(GET("https://www.example.com/", NULL), "b=3; __Host-z=1; new=1");

	/* third parties get nothing and set nothing */
	CHECK(soie_cookies_get("https://www.example.com/",
		"https://tracker.net/", 0) == NULL);
	soie_cookies_set("https://ads.net/", "https://www.example.com/",
		"t=1", 0);
	CHECK(GET("https://ads.net/", NULL) == NULL);
	STR(soie_cookies_get("https://img.example.com/",
		"https://www.example.com/x", 0), "b=3");
}

static void test_block(void)
{
	char path[] = "/tmp/soie-block-XXXXXX";
	soie_request rq;
	FILE *f;
	int fd = mkstemp(path);

	CHECK(fd >= 0);
	f = fdopen(fd, "w");
	fputs("# a hosts file\n0.0.0.0 ads.example.com\n"
		"127.0.0.1 localhost\ntracker.net # trailing\n", f);
	fclose(f);
	setenv("SOIE_BLOCK", path, 1);
	CHECK(soie_ext_load("ext/block.so") == 0);
	unlink(path);
	memset(&rq, 0, sizeof(rq));
	rq.first_party = "https://www.example.com/";
	rq.url = "https://ads.example.com/a.js";
	CHECK(soie_ext_blocked(&rq));
	rq.url = "https://x.ADS.example.com:443/a.js";
	CHECK(soie_ext_blocked(&rq));
	rq.url = "http://tracker.net";
	CHECK(soie_ext_blocked(&rq));
	rq.url = "https://example.com/ads.example.com";
	CHECK(!soie_ext_blocked(&rq));
	rq.url = "https://bads.example.com/";
	CHECK(!soie_ext_blocked(&rq));
	rq.url = "http://localhost/";
	CHECK(!soie_ext_blocked(&rq));
	/* what one asked for is never blocked */
	rq.url = "https://ads.example.com/";
	rq.first_party = NULL;
	CHECK(!soie_ext_blocked(&rq));
}

static void test_webp(void)
{
	const struct soie_ext_image *e;
	unsigned char *px;
	char *buf;
	size_t len;
	long n;
	int w = 0, h = 0, i;
	FILE *f;

	if (access("ext/webp.so", F_OK))
		return;	/* not built */
	CHECK(soie_ext_load("ext/webp.so") == 0);
	f = fopen("tests/paint/img/grad.webp", "rb");
	CHECK(f != NULL);
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	len = (size_t)n;
	buf = malloc(len);
	CHECK(fread(buf, 1, len, f) == len);
	fclose(f);
	for (i = 0; (e = (const void *)soie_ext_get(SOIE_EXT_IMAGE, i)); i++)
		if (!strcmp(e->base.name, "webp"))
			break;
	CHECK(e != NULL);
	if (e) {
		px = e->decode(e->base.data, (unsigned char *)buf, len, &w, &h);
		CHECK(px && w == 40 && h == 30);
		free(px);
		CHECK(!e->decode(e->base.data, (const unsigned char *)"FAKE",
			4, &w, &h));
	}
	free(buf);
}

/* Local files through the request interface, with Range support. */
static int ranges;

static int file_request(void *data, const soie_request *rq,
	soie_response *res)
{
	const char *r = rq->headers ? strstr(rq->headers, "Range: bytes=") :
		NULL;
	long long from = 0, to = -1, size;
	FILE *f;

	(void)data;
	memset(res, 0, sizeof(*res));
	if (strncmp(rq->url, "file://", 7) || !(f = fopen(rq->url + 7, "rb")))
		return -1;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (r && sscanf(r + 13, "%lld-%lld", &from, &to) >= 1) {
		ranges++;
		if (to < 0 || to >= size)
			to = size - 1;
	} else {
		to = size - 1;
	}
	if (from > to) {
		fclose(f);
		return -1;
	}
	res->len = (size_t)(to - from + 1);
	res->body = malloc(res->len + 1);
	fseek(f, from, SEEK_SET);
	res->len = fread(res->body, 1, res->len, f);
	fclose(f);
	res->status = r ? 206 : 200;
	res->url = strdup(rq->url);
	res->headers = malloc(64);
	snprintf(res->headers, 64, "content-range: bytes %lld-%lld/%lld\r\n",
		from, to, size);
	return 0;
}

static void wait_ms(int ms)
{
	struct timespec t;

	t.tv_sec = ms / 1000;
	t.tv_nsec = (long)(ms % 1000) * 1000000;
	nanosleep(&t, NULL);
}

static void test_media(void)
{
	const struct soie_ext_media *e;
	soie_media_state st;
	const unsigned char *px;
	char url[4200], cwd[4096], html[8400];
	void *p;
	int w = 0, h = 0, i, repaint;
	soie_node *doc, *v;
	soie_style_options so;
	float x, y, vw, vh;
	unsigned char *canvas;

	if (access("ext/media.so", F_OK))
		return;	/* not built */
	setenv("SOIE_AUDIO", "none", 1);	/* silent tests */
	CHECK(soie_ext_load("ext/media.so") == 0);
	e = (const void *)soie_ext_get(SOIE_EXT_MEDIA, 0);
	CHECK(e != NULL);
	if (!e || !getcwd(cwd, sizeof(cwd)))
		return;
	CHECK(e->can_play(NULL, "video/webm") && e->can_play(NULL, NULL));
	CHECK(!e->can_play(NULL, "application/x-mpegURL"));
	CHECK(soie_media_can_play("audio/ogg"));
	snprintf(url, sizeof(url), "file://%s/tests/media/clip.webm", cwd);
	p = e->open(NULL, url, file_request, NULL);
	CHECK(p != NULL);
	if (!p)
		return;
	for (i = 0; i < 200; i++) {
		e->state(p, &st);
		if (st.width || st.error)
			break;
		wait_ms(10);
	}
	CHECK(!st.error && st.width == 160 && st.height == 120);
	CHECK(st.duration > 1.9 && st.duration < 2.1 && st.paused);
	CHECK(ranges > 0);
	/* paused: the first frame shows */
	for (i = 0; i < 100 && !(px = e->frame(p, &w, &h)); i++)
		wait_ms(10);
	CHECK(px && w == 160 && h == 120);
	e->play(p);
	wait_ms(400);
	e->state(p, &st);
	CHECK(!st.paused && st.time > 0.2 && st.time < 1.5);
	e->seek(p, 1.5);
	for (i = 0; i < 300; i++) {
		e->state(p, &st);
		if (st.ended)
			break;
		wait_ms(10);
	}
	CHECK(st.ended && st.paused && st.time > 1.9);
	e->close(p);

	/* through the engine: an element, its size, its frames */
	snprintf(html, sizeof(html), "<body style=margin:0><video src=\"%s\">"
		"</video>", url);
	doc = soie_html_parse(html, strlen(html), NULL);
	memset(&so, 0, sizeof(so));
	so.width = 400;
	so.height = 300;
	so.request = file_request;
	soie_style_document(doc, &so);
	soie_layout_document(doc, 400, 300);
	v = soie_query(doc, "video", 1, (size_t *)&i)[0];
	CHECK(!soie_element_rect(v, &x, &y, &vw, &vh) && vw == 300);
	CHECK(soie_media_get_state(v, &st) == -1);	/* not opened yet */
	CHECK(soie_media_control(v, SOIE_MEDIA_PLAY, 0) == 0);
	for (i = 0; i < 200; i++) {
		soie_media_tick(doc, &repaint);
		soie_media_get_state(v, &st);
		if (st.width && st.time > 0.3)
			break;
		wait_ms(10);
	}
	soie_document_update(doc);
	CHECK(!soie_element_rect(v, &x, &y, &vw, &vh) && vw == 160 &&
		vh == 120);
	canvas = calloc(400 * 300, 4);
	soie_paint(doc, canvas, 400, 300, 400 * 4, 0, 0);
	/* testsrc: colored, not the black of an empty player */
	CHECK(canvas[(60 * 400 + 80) * 4] || canvas[(60 * 400 + 80) * 4 + 1] ||
		canvas[(60 * 400 + 80) * 4 + 2]);
	free(canvas);
	CHECK(soie_media_control(v, SOIE_MEDIA_PAUSE, 0) == 0);
	soie_media_get_state(v, &st);
	CHECK(st.paused);
	soie_node_free(doc);	/* closes the player */
}

int main(void)
{
	setenv("SOIE_COOKIES", "", 1);	/* memory only */
	soie_fonts_stub(1);
	test_registry();
	test_response();
	test_cookies();
	test_block();
	test_webp();
	test_media();
	printf("ext: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
