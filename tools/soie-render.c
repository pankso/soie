/*
 * soie-render: render a page with libsoie, from the command line:
 * DOM, computed styles, box tree or PNG.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <png.h>
#include <soie/soie.h>
#include <soie/ext.h>

static void usage(void)
{
	fputs("Usage: soie-render [options] FILE|-\n"
	      "  -d          dump the DOM tree (default)\n"
	      "  -c CHARSET  charset from the transport, skips sniffing\n"
	      "  -f CONTEXT  parse as a fragment inside element CONTEXT\n"
	      "  -s          scripting flag on (<noscript> as raw text)\n"
	      "  -p PROPS    run the cascade, dump these computed properties\n"
	      "              (comma separated, e.g. display,color)\n"
	      "  -l          run cascade and layout, dump the box tree\n"
	      "  -o FILE.png render the viewport (or -a: whole page) to PNG\n"
	      "  -w WIDTH    viewport width (1024), -H HEIGHT (768)\n"
	      "  -D          prefers-color-scheme: dark\n"
	      "  -S          stub font (0.5em per character), as the tests\n"
	      "  -b URL      base URL (default: the file's file:// URL)\n"
	      "  -j          run the page's scripts (and timers, 2 s at most)\n"
	      "  -T MS       the scripts' time, in milliseconds (2000)\n"
	      "  -x MODULE   load an extension module (.so), may repeat\n"
	      "  -X          load the installed extensions\n"
	      "  -V          print the library version\n", stderr);
	exit(2);
}

static char *read_file(FILE *f, size_t *len);

static int write_png(const char *path, const unsigned char *rgba, int w,
	int h)
{
	FILE *f = fopen(path, "wb");
	png_structp png;
	png_infop info;
	int y;

	if (!f)
		return -1;
	png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info = png ? png_create_info_struct(png) : NULL;
	if (!info || setjmp(png_jmpbuf(png))) {
		fclose(f);
		return -1;
	}
	png_init_io(png, f);
	png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
		PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (y = 0; y < h; y++)
		png_write_row(png, (png_bytep)(rgba + (size_t)y * (size_t)w * 4));
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	fclose(f);
	return 0;
}

static char *read_all(const char *path, size_t *len)
{
	FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
	char *buf;

	if (!f) {
		perror(path);
		exit(1);
	}
	buf = read_file(f, len);
	if (!buf) {
		perror(path);
		exit(1);
	}
	if (f != stdin)
		fclose(f);
	return buf;
}

/* Fetch callback: local files only (file:// URLs). */
static int fetch(void *data, const char *url, char **buf, size_t *len)
{
	FILE *f;

	(void)data;
	if (strncmp(url, "file://", 7))
		return -1;
	f = fopen(url + 7, "rb");
	if (!f)
		return -1;
	*buf = read_file(f, len);
	fclose(f);
	return *buf ? 0 : -1;
}

/* Scripts: the same files through the request interface; messages
 * and navigations go to stderr. */
static int request(void *data, const soie_request *rq, soie_response *res)
{
	char *buf;
	size_t len;

	memset(res, 0, sizeof(*res));
	if (fetch(data, rq->url, &buf, &len))
		return -1;
	res->status = 200;
	res->url = strdup(rq->url);
	res->headers = strdup("");
	res->body = buf ? buf : malloc(1);
	res->len = len;
	return 0;
}

static void message(void *data, int kind, const char *text)
{
	static const char *const names[] = { "log", "warn", "error",
		"alert" };

	(void)data;
	fprintf(stderr, "%s: %s\n", names[kind & 3], text);
}

static void navigate(void *data, const char *url, int replace)
{
	(void)data;
	fprintf(stderr, "navigate%s: %s\n", replace ? " (replace)" : "", url);
}

static int vp_w = 1024, vp_h = 768;
static float vp_y;

static void viewport(void *data, int *w, int *h, float *sx, float *sy)
{
	(void)data;
	*w = vp_w;
	*h = vp_h;
	*sx = 0;
	*sy = vp_y;
}

static void scroll(void *data, float x, float y)
{
	(void)data;
	(void)x;
	vp_y = y;
}

/* Run the page's scripts, then its timers for up to ms. */
static void run_scripts(soie_node *doc, const char *url, int ms)
{
	static soie_script_host host = { NULL, request, navigate, message,
		viewport, scroll };
	struct timespec t0, t;
	int d, spent;

	if (soie_script_start(doc, url, &host)) {
		fputs("soie-render: no script extension (-x ext/script.so)\n",
			stderr);
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		int m, repaint;

		/* media events first: timers their listeners set count */
		m = soie_media_tick(doc, &repaint);
		d = soie_script_tick(doc);
		if (m >= 0 && (d < 0 || m < d))
			d = m;
		if (d < 0)
			break;
		clock_gettime(CLOCK_MONOTONIC, &t);
		spent = (int)((t.tv_sec - t0.tv_sec) * 1000 +
			(t.tv_nsec - t0.tv_nsec) / 1000000);
		if (spent + d > ms)
			break;
		if (d > 0) {
			t.tv_sec = d / 1000;
			t.tv_nsec = (long)(d % 1000) * 1000000;
			nanosleep(&t, NULL);
		}
	}
}

static char *read_file(FILE *f, size_t *len)
{
	char *buf = NULL;
	size_t cap = 0, n;

	*len = 0;
	do {
		if (*len + 65536 > cap) {
			cap = cap ? cap * 2 : 65536;
			buf = realloc(buf, cap);
			if (!buf) {
				perror("realloc");
				exit(1);
			}
		}
		n = fread(buf + *len, 1, cap - *len, f);
		*len += n;
	} while (n);
	if (ferror(f)) {
		free(buf);
		return NULL;
	}
	return buf;
}

int main(int argc, char **argv)
{
	soie_html_options opts = { NULL, 0 };
	soie_style_options so;
	const char *context = NULL, *props = NULL, *base = NULL;
	int layout = 0, all = 0, scripts = 0, script_ms = 2000;
	const char *png = NULL;
	char cwd[4096], file_url[8192];
	soie_node *doc, *root;
	char *buf, *dump, *user_css = NULL;
	size_t len;
	int i;

	memset(&so, 0, sizeof(so));
	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-d"))
			;
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			opts.charset = argv[++i];
		else if (!strcmp(argv[i], "-f") && i + 1 < argc)
			context = argv[++i];
		else if (!strcmp(argv[i], "-s"))
			opts.scripting = so.scripting = 1;
		else if (!strcmp(argv[i], "-l"))
			layout = 1;
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			png = argv[++i];
		else if (!strcmp(argv[i], "-a"))
			all = 1;
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			props = argv[++i];
		else if (!strcmp(argv[i], "-w") && i + 1 < argc)
			so.width = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-H") && i + 1 < argc)
			so.height = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-D"))
			so.dark = 1;
		else if (!strcmp(argv[i], "-S"))
			soie_fonts_stub(1);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			base = argv[++i];
		else if (!strcmp(argv[i], "-T") && i + 1 < argc)
			script_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-j"))
			scripts = opts.scripting = so.scripting = 1;
		else if (!strcmp(argv[i], "-x") && i + 1 < argc) {
			if (soie_ext_load(argv[++i])) {
				fprintf(stderr, "soie-render: cannot load %s\n",
					argv[i]);
				return 1;
			}
		} else if (!strcmp(argv[i], "-X"))
			soie_ext_load_dir(SOIE_EXT_DIR);
		else if (!strcmp(argv[i], "-V")) {
			printf("soie %s\n", soie_version());
			return 0;
		} else
			usage();
	}
	if (i != argc - 1)
		usage();
	buf = read_all(argv[i], &len);
	if (context) {
		doc = soie_document_new();
		root = soie_html_parse_fragment(doc, SOIE_NS_HTML, context,
			buf, len, &opts);
	} else {
		doc = soie_html_parse(buf, len, &opts);
		root = doc;
	}
	if (!base && strcmp(argv[i], "-")) {
		if (argv[i][0] == '/')
			snprintf(file_url, sizeof(file_url), "file://%s", argv[i]);
		else if (getcwd(cwd, sizeof(cwd)))
			snprintf(file_url, sizeof(file_url), "file://%s/%s", cwd,
				argv[i]);
		base = file_url;
	}
	so.base_url = base;
	so.fetch = fetch;
	if (root == doc)
		soie_ext_document(doc, base);
	if (scripts && root == doc) {
		so.user_css = user_css = soie_ext_user_css(NULL, base);
		so.request = request;	/* media players */
		vp_w = so.width > 0 ? so.width : 1024;
		vp_h = so.height > 0 ? so.height : 768;
		soie_style_document(doc, &so);
		soie_layout_document(doc, so.width, so.height);
		run_scripts(doc, base ? base : "about:blank", script_ms);
	}
	if (props || layout || png) {
		if (!user_css)
			so.user_css = user_css = soie_ext_user_css(NULL, base);
		soie_style_document(doc, &so);
		if (png) {
			int w = so.width > 0 ? so.width : 1024;
			int h = so.height > 0 ? so.height : 768;
			unsigned char *px;

			soie_layout_document(doc, w, h);
			if (all && soie_layout_height(doc) > h)
				h = (int)(soie_layout_height(doc) + 0.99f);
			px = malloc((size_t)w * (size_t)h * 4);
			soie_paint(doc, px, w, h, w * 4, 0, 0);
			if (write_png(png, px, w, h)) {
				perror(png);
				return 1;
			}
			free(px);
			dump = calloc(1, 1);
			len = 0;
		} else if (layout) {
			soie_layout_document(doc, so.width, so.height);
			dump = soie_layout_dump(doc, &len);
		} else {
			dump = soie_node_dump_style(root, props, &len);
		}
	} else {
		dump = soie_node_dump(root, &len);
	}
	fwrite(dump, 1, len, stdout);
	free(dump);
	if (root != doc)
		soie_node_free(root);
	soie_node_free(doc);
	free(buf);
	free(user_css);
	return 0;
}
