/*
 * Paint tests: render tests/paint/NAME.html at 320x240 with the stub
 * font and compare with NAME.png, pixel for pixel. With -u, write the
 * references instead (check them by eye before committing).
 * Usage: painttest [-u] FILE.html...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <unistd.h>
#include <soie/soie.h>

#define W 320
#define H 240

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *b;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	*len = fread(b, 1, (size_t)n, f);
	fclose(f);
	return b;
}

/* file:// URLs only: the test images. */
static int fetch(void *data, const char *url, char **buf, size_t *len)
{
	(void)data;
	if (strncmp(url, "file://", 7))
		return -1;
	*buf = slurp(url + 7, len);
	return *buf ? 0 : -1;
}

static int write_png(const char *path, unsigned char *px)
{
	FILE *f = fopen(path, "wb");
	png_structp p;
	png_infop i;
	int y;

	if (!f)
		return -1;
	p = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	i = png_create_info_struct(p);
	if (setjmp(png_jmpbuf(p))) {
		fclose(f);
		return -1;
	}
	png_init_io(p, f);
	png_set_IHDR(p, i, W, H, 8, PNG_COLOR_TYPE_RGB_ALPHA,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);
	png_write_info(p, i);
	for (y = 0; y < H; y++)
		png_write_row(p, px + y * W * 4);
	png_write_end(p, i);
	png_destroy_write_struct(&p, &i);
	fclose(f);
	return 0;
}

static unsigned char *read_png(const char *path)
{
	FILE *f = fopen(path, "rb");
	png_structp p;
	png_infop i;
	unsigned char *px;
	int y;

	if (!f)
		return NULL;
	p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	i = png_create_info_struct(p);
	if (setjmp(png_jmpbuf(p))) {
		fclose(f);
		return NULL;
	}
	png_init_io(p, f);
	png_read_info(p, i);
	if (png_get_image_width(p, i) != W || png_get_image_height(p, i) != H ||
	    png_get_color_type(p, i) != PNG_COLOR_TYPE_RGB_ALPHA) {
		fclose(f);
		return NULL;
	}
	px = malloc(W * H * 4);
	for (y = 0; y < H; y++)
		png_read_row(p, px + y * W * 4, NULL);
	png_destroy_read_struct(&p, &i, NULL);
	fclose(f);
	return px;
}

int main(int argc, char **argv)
{
	int i, update = 0, fail = 0, pass = 0;

	soie_fonts_stub(1);
	for (i = 1; i < argc; i++) {
		char ref[1024], out[1024];
		unsigned char *px, *want;
		size_t len, n;
		char *html;
		soie_node *doc;

		if (!strcmp(argv[i], "-u")) {
			update = 1;
			continue;
		}
		html = slurp(argv[i], &len);
		if (!html)
			continue;
		n = strlen(argv[i]) - 5;
		snprintf(ref, sizeof(ref), "%.*s.png", (int)n, argv[i]);
		snprintf(out, sizeof(out), "%.*s.out.png", (int)n, argv[i]);
		doc = soie_html_parse(html, len, NULL);
		{
			soie_style_options o;
			char base[2048], cwd[1024];

			memset(&o, 0, sizeof(o));
			if (!getcwd(cwd, sizeof(cwd)))
				cwd[0] = '\0';
			snprintf(base, sizeof(base), "file://%s/%s", cwd,
				argv[i]);
			o.base_url = base;
			o.fetch = fetch;
			soie_style_document(doc, &o);
		}
		soie_layout_document(doc, W, H);
		px = malloc(W * H * 4);
		soie_paint(doc, px, W, H, W * 4, 0, 0);
		if (update) {
			write_png(ref, px);
			printf("wrote %s\n", ref);
		} else if (!(want = read_png(ref)) ||
			   memcmp(want, px, W * H * 4)) {
			write_png(out, px);
			printf("FAIL %s (got %s)\n", argv[i], out);
			if (want) {
				int k;

				for (k = 0; k < W * H * 4 && want[k] == px[k]; k++)
					;
				printf("  first difference at pixel %d,%d\n",
					k / 4 % W, k / 4 / W);
			}
			fail++;
			free(want);
		} else {
			remove(out);
			pass++;
			free(want);
		}
		free(px);
		free(html);
		soie_node_free(doc);
	}
	if (!update)
		printf("paint: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
