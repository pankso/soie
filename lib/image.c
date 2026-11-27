/*
 * Images: PNG (libpng), JPEG (libjpeg), GIF (first frame) and SVG
 * decoding, the per-document cache, drawing with bilinear scaling,
 * object-fit, and CSS backgrounds: url() images, linear and radial
 * gradients, background-size, -position and -repeat.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <png.h>		/* first: libpng 1.2 wants its own setjmp.h */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include "image.h"

#define LATER 2		/* image->failed: asked, not there yet */
#define LARGE (256 << 10)	/* decoded later than this many bytes */
#define BUDGET (32 << 20)	/* large images decoded at once */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* PNG */

struct mem {
	const unsigned char *p;
	size_t len, pos;
};

static void png_mem_read(png_structp png, png_bytep out, png_size_t n)
{
	struct mem *m = png_get_io_ptr(png);

	if (m->pos + n > m->len)
		png_error(png, "truncated");
	memcpy(out, m->p + m->pos, n);
	m->pos += n;
}

/* libpng must not print: errors jump back, warnings vanish. */
static void png_quiet_error(png_structp png, png_const_charp msg)
{
	(void)msg;
	longjmp(png_jmpbuf(png), 1);
}

static void png_quiet_warning(png_structp png, png_const_charp msg)
{
	(void)png;
	(void)msg;
}

/* size_only: read the header, im->w and im->h, no pixels */
static int decode_png(const unsigned char *d, size_t len, struct image *im,
	int size_only)
{
	png_structp png;
	png_infop info;
	struct mem m;
	png_bytep *rows = NULL;
	int y;

	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
		png_quiet_error, png_quiet_warning);
	if (!png)
		return 0;
	info = png_create_info_struct(png);
	if (!info || setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, NULL);
		free(rows);
		free(im->rgba);
		im->rgba = NULL;
		return 0;
	}
	m.p = d;
	m.len = len;
	m.pos = 0;
	png_set_read_fn(png, &m, png_mem_read);
	png_read_info(png, info);
	im->w = (int)png_get_image_width(png, info);
	im->h = (int)png_get_image_height(png, info);
	if (im->w <= 0 || im->h <= 0 || im->w > 16384 || im->h > 16384)
		png_error(png, "size");
	if (size_only) {
		png_destroy_read_struct(&png, &info, NULL);
		return 1;
	}
	png_set_expand(png);
	png_set_strip_16(png);
	png_set_gray_to_rgb(png);
	png_set_filler(png, 0xff, PNG_FILLER_AFTER);
	png_set_interlace_handling(png);
	png_read_update_info(png, info);
	im->rgba = xmalloc((size_t)im->w * (size_t)im->h * 4);
	rows = xmalloc((size_t)im->h * sizeof(*rows));
	for (y = 0; y < im->h; y++)
		rows[y] = im->rgba + (size_t)y * (size_t)im->w * 4;
	png_read_image(png, rows);
	png_destroy_read_struct(&png, &info, NULL);
	free(rows);
	return 1;
}

/* JPEG */

struct jerr {
	struct jpeg_error_mgr mgr;
	jmp_buf jmp;
};

static void jpeg_fail(j_common_ptr c)
{
	longjmp(((struct jerr *)c->err)->jmp, 1);
}

static void jpeg_quiet(j_common_ptr c, int level)
{
	(void)c;
	(void)level;
}

static int decode_jpeg(const unsigned char *d, size_t len, struct image *im,
	int size_only)
{
	struct jpeg_decompress_struct j;
	struct jerr e;
	unsigned char *row = NULL;

	j.err = jpeg_std_error(&e.mgr);
	e.mgr.error_exit = jpeg_fail;
	e.mgr.emit_message = jpeg_quiet;
	if (setjmp(e.jmp)) {
		jpeg_destroy_decompress(&j);
		free(row);
		free(im->rgba);
		im->rgba = NULL;
		return 0;
	}
	jpeg_create_decompress(&j);
	jpeg_mem_src(&j, (unsigned char *)d, (unsigned long)len);
	jpeg_read_header(&j, TRUE);
	j.out_color_space = JCS_RGB;
	if (size_only) {
		/* what jpeg_start_decompress() would give; CMYK may not
		 * convert: decoded at once to know */
		jpeg_calc_output_dimensions(&j);
		im->w = (int)j.output_width;
		im->h = (int)j.output_height;
		size_only = im->w > 0 && im->h > 0 && im->w <= 16384 &&
			im->h <= 16384 && j.output_components == 3 &&
			j.jpeg_color_space != JCS_CMYK &&
			j.jpeg_color_space != JCS_YCCK;
		jpeg_destroy_decompress(&j);
		return size_only;
	}
	jpeg_start_decompress(&j);
	im->w = (int)j.output_width;
	im->h = (int)j.output_height;
	if (im->w > 16384 || im->h > 16384 || j.output_components != 3)
		longjmp(e.jmp, 1);
	im->rgba = xmalloc((size_t)im->w * (size_t)im->h * 4);
	row = xmalloc((size_t)im->w * 3);
	while (j.output_scanline < j.output_height) {
		unsigned char *o = im->rgba + (size_t)j.output_scanline *
			(size_t)im->w * 4;
		int x;

		jpeg_read_scanlines(&j, &row, 1);
		for (x = 0; x < im->w; x++) {
			o[x * 4] = row[x * 3];
			o[x * 4 + 1] = row[x * 3 + 1];
			o[x * 4 + 2] = row[x * 3 + 2];
			o[x * 4 + 3] = 255;
		}
	}
	jpeg_finish_decompress(&j);
	jpeg_destroy_decompress(&j);
	free(row);
	return 1;
}

/* GIF, first frame (GIF89a, LZW) */

static int decode_gif(const unsigned char *d, size_t len, struct image *im)
{
	const unsigned char *gct = NULL, *pal;
	size_t p = 13;
	int gsize = 0, trans = -1, lsize, x0, y0, fw, fh, inter, ncol;
	unsigned char *idx;

	if (len < 13)
		return 0;
	im->w = d[6] | d[7] << 8;
	im->h = d[8] | d[9] << 8;
	if (!im->w || !im->h || im->w > 16384 || im->h > 16384)
		return 0;
	if (d[10] & 0x80) {
		gsize = 2 << (d[10] & 7);
		gct = d + 13;
		p += (size_t)gsize * 3;
	}
	for (;;) {
		if (p >= len)
			return 0;
		if (d[p] == 0x21) {	/* extension */
			if (p + 1 < len && d[p + 1] == 0xf9 && p + 7 < len &&
			    (d[p + 3] & 1))
				trans = d[p + 6];
			p += 2;
			while (p < len && d[p])
				p += (size_t)d[p] + 1;
			p++;
		} else if (d[p] == 0x2c) {
			break;
		} else {
			return 0;
		}
	}
	if (p + 10 > len)
		return 0;
	x0 = d[p + 1] | d[p + 2] << 8;
	y0 = d[p + 3] | d[p + 4] << 8;
	fw = d[p + 5] | d[p + 6] << 8;
	fh = d[p + 7] | d[p + 8] << 8;
	inter = d[p + 9] & 0x40;
	pal = gct;
	ncol = gsize;
	if (d[p + 9] & 0x80) {
		lsize = 2 << (d[p + 9] & 7);
		pal = d + p + 10;
		ncol = lsize;
		p += (size_t)lsize * 3;
	}
	p += 10;
	if (!pal || p >= len)
		return 0;
	/* a frame no larger than the screen's limit, nor than what the
	 * index arithmetic below (size_t) can hold */
	if (!fw || !fh || fw > 16384 || fh > 16384)
		return 0;
	idx = calloc((size_t)fw * (size_t)fh + 1, 1);
	if (!idx)
		abort();
	{
		/* LZW over the data sub-blocks */
		int minc = d[p++], clear = 1 << minc, eoi = clear + 1, next,
			csize, prev = -1;
		size_t out = 0, total = (size_t)fw * (size_t)fh;
		/* on the stack: decoders may run in several threads */
		unsigned short pre[4096];
		unsigned char suf[4096], stack[4097];
		unsigned bits = 0, nbits = 0;
		size_t blk = 0;

		if (minc < 2 || minc > 11) {
			free(idx);
			return 0;
		}
		next = eoi + 1;
		csize = minc + 1;
		while (out < total) {
			int code, c, sp = 0, first;

			while (nbits < (unsigned)csize) {
				if (!blk) {
					if (p >= len || !d[p])
						goto done;
					blk = d[p++];
				}
				if (p >= len)
					goto done;
				bits |= (unsigned)d[p++] << nbits;
				nbits += 8;
				blk--;
			}
			code = (int)(bits & ((1u << csize) - 1));
			bits >>= csize;
			nbits -= (unsigned)csize;
			if (code == clear) {
				next = eoi + 1;
				csize = minc + 1;
				prev = -1;
				continue;
			}
			if (code == eoi)
				break;
			if (code > next || (code == next && prev < 0))
				break;		/* corrupt */
			/* decode code, or prev for KwKwK (code == next) */
			c = code == next ? prev : code;
			while (c > eoi && sp < 4096) {
				stack[sp++] = suf[c];
				c = pre[c];
			}
			first = c;
			stack[sp++] = (unsigned char)c;
			while (sp && out < total)
				idx[out++] = stack[--sp];
			if (code == next && out < total)
				idx[out++] = (unsigned char)first;
			if (prev >= 0 && next < 4096) {
				pre[next] = (unsigned short)prev;
				suf[next] = (unsigned char)first;
				next++;
				if (next == (1 << csize) && csize < 12)
					csize++;
			}
			prev = code;
		}
	}
done:
	im->rgba = calloc((size_t)im->w * (size_t)im->h * 4, 1);
	if (!im->rgba)
		abort();
	{
		int row = 0, pass = 0, k;
		static const int start[4] = { 0, 4, 2, 1 },
			step[4] = { 8, 8, 4, 2 };

		for (k = 0; k < fh; k++) {
			int y = inter ? row : k, x;

			if (inter) {
				row += step[pass];
				while (row >= fh && pass < 3)
					row = start[++pass];
			}
			for (x = 0; x < fw; x++) {
				int ci = idx[(size_t)k * (size_t)fw + (size_t)x];
				int X = x0 + x, Y = y0 + y;
				unsigned char *o;

				if (X >= im->w || Y >= im->h || ci == trans ||
				    ci >= ncol)
					continue;
				o = im->rgba + ((size_t)Y * (size_t)im->w +
					(size_t)X) * 4;
				o[0] = pal[ci * 3];
				o[1] = pal[ci * 3 + 1];
				o[2] = pal[ci * 3 + 2];
				o[3] = 255;
			}
		}
	}
	free(idx);
	return 1;
}

/* SVG files: parsed by our HTML parser, foreign content does it. */
static int decode_svg(const unsigned char *d, size_t len, struct image *im)
{
	soie_node *doc = soie_html_parse((const char *)d, len, NULL);
	soie_node *n = doc;

	while (n) {
		if (n->type == SOIE_ELEMENT && n->ns == SOIE_NS_SVG &&
		    n->tag == TAG_SVG)
			break;
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n && !n->next)
			n = n->parent;
		if (n)
			n = n->next;
	}
	if (!n) {
		soie_node_free(doc);
		return 0;
	}
	im->svgdoc = doc;
	im->svg = n;
	{
		float w, h;

		svg_size(n, 16, &w, &h);
		im->w = (int)(w + 0.5f);
		im->h = (int)(h + 0.5f);
	}
	return 1;
}

int decode_image(const unsigned char *d, size_t len, struct image *im)
{
	size_t i;

	if (len > 8 && !memcmp(d, "\x89PNG", 4))
		return decode_png(d, len, im, 0);
	if (len > 3 && d[0] == 0xff && d[1] == 0xd8)
		return decode_jpeg(d, len, im, 0);
	if (len > 6 && !memcmp(d, "GIF8", 4))
		return decode_gif(d, len, im);
	for (i = 0; i < len && i < 512; i++)
		if (d[i] == '<')
			return decode_svg(d, len, im);
		else if (d[i] < 9)
			break;	/* binary */
	/* WebP, AVIF...: the image extensions' job */
	im->rgba = ext_decode_image(d, len, &im->w, &im->h);
	return im->rgba != NULL;
}

/* The size of a PNG or JPEG from its header, without decoding it */
static int header_size(const unsigned char *d, size_t len, struct image *im)
{
	if (len > 8 && !memcmp(d, "\x89PNG", 4))
		return decode_png(d, len, im, 1);
	if (len > 3 && d[0] == 0xff && d[1] == 0xd8)
		return decode_jpeg(d, len, im, 1);
	return 0;
}

/* Cache */

void images_free(struct images *im)
{
	int i;

	for (i = 0; i < im->n; i++) {
		free(im->v[i].url);
		free(im->v[i].rgba);
		free(im->v[i].data);
		if (im->v[i].svgdoc)
			soie_node_free(im->v[i].svgdoc);
	}
	free(im->v);
	free(im->base);
	free(im);
}

void images_setup(soie_node *doc, soie_fetch_fn fetch, void *data,
	const char *base)
{
	struct images *im = doc->u.doc->images;

	if (!im) {
		im = xmalloc(sizeof(*im));
		memset(im, 0, sizeof(*im));
		doc->u.doc->images = im;
	}
	im->fetch = fetch;
	im->data = data;
	free(im->base);
	im->base = base ? xstrndup(base, strlen(base)) : NULL;
}

/* Ask the host for x's bytes: its size, its pixels when small */
static void image_fetch(struct images *im, struct image *x)
{
	char *buf = NULL;
	size_t len = 0;
	int r;

	x->failed = 0;
	r = im->fetch ? im->fetch(im->data, x->url, &buf, &len) : -1;
	if (r == SOIE_FETCH_LATER) {
		x->failed = LATER;
	} else if (r || !buf) {
		x->failed = 1;
	} else if (header_size((unsigned char *)buf, len, x)) {
		/* the size for the layout now, the pixels when painted */
		x->data = (unsigned char *)buf;
		x->len = len;
		buf = NULL;
	} else if (!decode_image((unsigned char *)buf, len, x)) {
		x->failed = 1;
	}
	if (x->rgba && (size_t)x->w * (size_t)x->h * 4 > LARGE) {
		/* keep the bytes, decode again when painted */
		free(x->rgba);
		x->rgba = NULL;
		x->data = (unsigned char *)buf;
		x->len = len;
		buf = NULL;
	}
	x->owner = im;
	free(buf);
}

/* An image by URL (resolved against the base): fetched once. One on
 * its way (SOIE_FETCH_LATER) is asked again by images_retry() only:
 * a paint that took it when it came would show it in the rect it
 * paints and nowhere else. */
const struct image *image_get(soie_node *doc, const char *url)
{
	struct images *im = doc->u.doc->images;
	struct image *x;
	char *abs;
	int i;

	if (!im || !url || !*url)
		return NULL;
	abs = soie_url_resolve(im->base, url);
	for (i = 0; i < im->n; i++)
		if (!strcmp(im->v[i].url, abs)) {
			free(abs);
			return im->v[i].failed ? NULL : &im->v[i];
		}
	if (im->n == im->cap) {
		im->cap = im->cap ? im->cap * 2 : 16;
		im->v = xrealloc(im->v, (size_t)im->cap * sizeof(*im->v));
	}
	x = &im->v[im->n++];
	memset(x, 0, sizeof(*x));
	x->url = abs;
	image_fetch(im, x);
	return x->failed ? NULL : x;
}

/* The images that were on their way are asked for again: those that
 * came are fresh, until the boxes showing them are in the damage
 * (damage_images()). Returns how many are. */
int images_retry(soie_node *doc)
{
	struct images *im = doc->u.doc->images;
	int i;

	if (!im)
		return 0;
	for (i = 0; i < im->n; i++) {
		if (im->v[i].failed != LATER)
			continue;
		image_fetch(im, &im->v[i]);
		if (!im->v[i].failed) {
			im->v[i].fresh = 1;
			im->nfresh++;
		}
	}
	return im->nfresh;
}

void images_shown(soie_node *doc)
{
	struct images *im = doc->u.doc->images;
	int i;

	for (i = 0; im && i < im->n; i++)
		im->v[i].fresh = 0;
	if (im)
		im->nfresh = 0;
}

static int image_fresh(soie_node *doc, const char *url)
{
	struct images *im = doc->u.doc->images;
	char *abs;
	int i, r = 0;

	if (!url || !*url)
		return 0;
	abs = soie_url_resolve(im->base, url);
	for (i = 0; i < im->n && !r; i++)
		r = im->v[i].fresh && !strcmp(im->v[i].url, abs);
	free(abs);
	return r;
}

/* One of the images of a background-image or mask-image value */
static int layers_fresh(soie_node *doc, const char *img)
{
	struct cvlist li;
	int i, r = 0;

	if (!img || !strcmp(img, "none"))
		return 0;
	css_component_values(img, strlen(img), &li);
	for (i = 0; i < li.n && !r; i++) {
		const struct cv *v = &li.v[i];

		if (v->type == CT_URL)
			r = image_fresh(doc, v->str);
		else if (cv_func(v, "url") && v->kids.n &&
			 v->kids.v[0].type == CT_STRING)
			r = image_fresh(doc, v->kids.v[0].str);
	}
	cvlist_free(&li);
	return r;
}

/* b shows a fresh image: its own, a poster, a background or mask
 * layer */
int box_image_fresh(const struct box *b)
{
	static const char *const attrs[] = { "src", "data", "poster" };
	soie_node *doc = b->node->doc;
	struct images *im = doc->u.doc->images;
	size_t i;

	if (!im || !im->nfresh || !b->style)
		return 0;
	if (b->type == B_REPLACED)
		for (i = 0; i < sizeof(attrs) / sizeof(*attrs); i++)
			if (image_fresh(doc, soie_element_get_attr(b->node,
			    attrs[i])))
				return 1;
	return layers_fresh(doc, b->style->v[P_BACKGROUND_IMAGE].u.str) ||
		layers_fresh(doc, b->style->v[P_MASK_IMAGE].u.str);
}

/*
 * The pixels of an image about to be painted: PNGs and JPEGs are
 * decoded now. Small ones stay decoded; of the large ones, the least
 * recently painted are dropped to stay within the budget: a page of big
 * photos costs what a screenful of them does.
 */
static const unsigned char *image_pixels(struct image *x)
{
	struct images *im = x->owner;
	struct image tmp;
	size_t size = (size_t)x->w * (size_t)x->h * 4;
	int i, old;

	x->used = im ? ++im->tick : 0;
	if (x->rgba || !x->data || !im)
		return x->rgba;
	memset(&tmp, 0, sizeof(tmp));
	if (!decode_image(x->data, x->len, &tmp) || tmp.w != x->w ||
	    tmp.h != x->h) {
		/* broken past its header: not tried again */
		free(tmp.rgba);
		free(x->data);
		x->data = NULL;
		return NULL;
	}
	x->rgba = tmp.rgba;
	if (size <= LARGE) {
		free(x->data);
		x->data = NULL;
		return x->rgba;
	}
	im->pixels += size;
	while (im->pixels > BUDGET) {
		old = -1;
		for (i = 0; i < im->n; i++)
			if (im->v[i].rgba && im->v[i].data && &im->v[i] != x &&
			    (old < 0 || im->v[i].used < im->v[old].used))
				old = i;
		if (old < 0)
			break;
		free(im->v[old].rgba);
		im->v[old].rgba = NULL;
		im->pixels -= (size_t)im->v[old].w * (size_t)im->v[old].h * 4;
	}
	return x->rgba;
}

/* The image of an <img>, <input type=image>, <object> or <embed>. */
const struct image *image_for(const soie_node *el)
{
	const char *src = soie_element_get_attr(el, "src");

	if (!src && is_html(el, TAG_OBJECT))
		src = soie_element_get_attr(el, "data");
	return src ? image_get(el->doc, src) : NULL;
}

/* Drawing */

static void blend_px(struct canvas *c, int x, int y, const unsigned char *s,
	float a)
{
	unsigned char *p;
	int al, r = c->bgr ? 2 : 0;

	if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1)
		return;
	al = (int)(s[3] * a * c->alpha + 0.5f);
	if (c->mask)
		al = div255(al * mask_at(c, x, y));
	if (al <= 0)
		return;
	p = c->px + (size_t)y * (size_t)c->stride + (size_t)x * 4;
	if (al >= 255) {
		p[0] = s[r];
		p[1] = s[1];
		p[2] = s[2 - r];
		p[3] = 255;
		return;
	}
	p[0] = (unsigned char)div255(s[r] * al + p[0] * (255 - al));
	p[1] = (unsigned char)div255(s[1] * al + p[1] * (255 - al));
	p[2] = (unsigned char)div255(s[2 - r] * al + p[2] * (255 - al));
	p[3] = (unsigned char)(al + div255(p[3] * (255 - al)));
}

/* An opaque image in the canvas' byte order at its own size, pixel
 * (0, 0) at canvas pixel (dx, dy): rows i0..i1 of j0..j1 are copied
 * where no mask weighs them, what bilinear sampling at whole pixels
 * would store one by one. */
static void copy_rows(struct canvas *c, const struct image *im,
	const unsigned char *px, int dx, int dy, int i0, int j0, int i1,
	int j1)
{
	int i, j, k, m;

	for (j = j0; j < j1; j++) {
		const unsigned char *s = px + ((size_t)(j - dy) *
			(size_t)im->w + (size_t)(i0 - dx)) * 4;
		unsigned char *d = c->px + (size_t)j * (size_t)c->stride +
			(size_t)i0 * 4;

		if (!c->mask) {
			memcpy(d, s, (size_t)(i1 - i0) * 4);
			continue;
		}
		for (i = i0; i < i1; i = k) {
			/* a run under a full mask, or a pixel on its edge */
			for (k = i; k < i1 && mask_at(c, k, j) == 255; k++)
				;
			if (k > i) {
				memcpy(d + (size_t)(i - i0) * 4,
					s + (size_t)(i - i0) * 4,
					(size_t)(k - i) * 4);
				continue;
			}
			k = i + 1;
			m = im->bgr ? 2 : 0;
			if (mask_at(c, i, j)) {
				const unsigned char *q = s + (size_t)(i - i0) * 4;
				unsigned char o[4];

				o[0] = q[m];
				o[1] = q[1];
				o[2] = q[2 - m];
				o[3] = q[3];
				blend_px(c, i, j, o, 1);
			}
		}
	}
}

/* Image into the page rect (x, y, w, h), bilinear, clipped. */
void draw_image(struct canvas *c, const struct image *im, float x, float y,
	float w, float h, const unsigned char *current,
	const struct style *st)
{
	const unsigned char *px;
	int i0, j0, i1, j1, i, j;

	if (w <= 0 || h <= 0 || c->cover)
		return;
	if (im->svg) {
		svg_render(c, im->svg, x, y, w, h, current, st, 1);
		return;
	}
	if (!im->w || !im->h)
		return;
	i0 = ifloor(x - c->ox);
	j0 = ifloor(y - c->oy);
	i1 = iceil(x + w - c->ox);
	j1 = iceil(y + h - c->oy);
	if (i0 < c->cx0)
		i0 = c->cx0;
	if (j0 < c->cy0)
		j0 = c->cy0;
	if (i1 > c->cx1)
		i1 = c->cx1;
	if (j1 > c->cy1)
		j1 = c->cy1;
	if (i0 >= i1 || j0 >= j1 || !(px = image_pixels((struct image *)im)))
		return;
	if (im->opaque && !im->bgr == !c->bgr && c->alpha == 1 &&
	    w == (float)im->w && h == (float)im->h &&
	    x - c->ox == floorf(x - c->ox) && y - c->oy == floorf(y - c->oy)) {
		copy_rows(c, im, px, (int)(x - c->ox), (int)(y - c->oy), i0, j0,
			i1, j1);
		return;
	}
	for (j = j0; j < j1; j++) {
		float sy = ((float)j + c->oy + 0.5f - y) * (float)im->h / h - 0.5f;
		int y0 = ifloor(sy), y1 = y0 + 1;
		float fy = sy - (float)y0;

		/* the edges' neighbours: a shrunk image's first row maps
		 * rows before its own, which do not exist */
		y0 = y0 < 0 ? 0 : y0 >= im->h ? im->h - 1 : y0;
		y1 = y1 < 0 ? 0 : y1 >= im->h ? im->h - 1 : y1;
		for (i = i0; i < i1; i++) {
			float sx = ((float)i + c->ox + 0.5f - x) * (float)im->w /
				w - 0.5f;
			int x0 = ifloor(sx), x1 = x0 + 1, k;
			float fx = sx - (float)x0;
			unsigned char o[4];
			const unsigned char *a, *b, *cc, *d;

			x0 = x0 < 0 ? 0 : x0 >= im->w ? im->w - 1 : x0;
			x1 = x1 < 0 ? 0 : x1 >= im->w ? im->w - 1 : x1;
			if (fx < 0)
				fx = 0;
			if (fy < 0)
				fy = 0;
			a = px + ((size_t)y0 * (size_t)im->w + (size_t)x0) * 4;
			b = px + ((size_t)y0 * (size_t)im->w + (size_t)x1) * 4;
			cc = px + ((size_t)y1 * (size_t)im->w + (size_t)x0) * 4;
			d = px + ((size_t)y1 * (size_t)im->w + (size_t)x1) * 4;
			for (k = 0; k < 4; k++)
				o[k] = (unsigned char)((a[k] * (1 - fx) +
					b[k] * fx) * (1 - fy) + (cc[k] *
					(1 - fx) + d[k] * fx) * fy + 0.5f);
			if (im->bgr) {
				k = o[0];
				o[0] = o[2];
				o[2] = (unsigned char)k;
			}
			blend_px(c, i, j, o, 1);
		}
	}
}

/* object-fit (CSS Images 3): where content of natural size iw x ih
 * (zoomed px) goes in b's content box. */
void fit_rect(const struct box *b, float iw, float ih, float *x, float *y,
	float *w, float *h)
{
	const struct style *st = b->style;
	float s;

	if (kw(st, P_OBJECT_FIT, "contain") || kw(st, P_OBJECT_FIT, "cover") ||
	    (kw(st, P_OBJECT_FIT, "scale-down") && (iw > b->w || ih > b->h))) {
		s = b->w / iw < b->h / ih ? b->w / iw : b->h / ih;
		if (kw(st, P_OBJECT_FIT, "cover"))
			s = b->w / iw > b->h / ih ? b->w / iw : b->h / ih;
		iw *= s;
		ih *= s;
	} else if (!kw(st, P_OBJECT_FIT, "none") &&
		   !kw(st, P_OBJECT_FIT, "scale-down")) {
		iw = b->w;	/* fill */
		ih = b->h;
	}
	*x = b->x + (b->w - iw) / 2;
	*y = b->y + (b->h - ih) / 2;
	*w = iw;
	*h = ih;
}

/* An image at (x, y, w, h), clipped to b's content box. */
void draw_placed(struct canvas *c, const struct box *b,
	const struct image *im, float x, float y, float w, float h,
	const unsigned char *cur)
{
	int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
	int a, bb;

	a = ifloor(b->x - c->ox);
	bb = ifloor(b->y - c->oy);
	if (a > c->cx0)
		c->cx0 = a;
	if (bb > c->cy0)
		c->cy0 = bb;
	if (iceil(b->x + b->w - c->ox) < c->cx1)
		c->cx1 = iceil(b->x + b->w - c->ox);
	if (iceil(b->y + b->h - c->oy) < c->cy1)
		c->cy1 = iceil(b->y + b->h - c->oy);
	draw_image(c, im, x, y, w, h, cur, b->style);
	c->cx0 = cx0;
	c->cy0 = cy0;
	c->cx1 = cx1;
	c->cy1 = cy1;
}

/* An image in a replaced box, as object-fit says, clipped to it. */
void draw_fitted(struct canvas *c, const struct box *b,
	const struct image *im, const unsigned char *cur)
{
	float x, y, w, h;

	/* natural size in CSS px, zoomed */
	fit_rect(b, (float)im->w * doc_zoom(), (float)im->h * doc_zoom(), &x,
		&y, &w, &h);
	draw_placed(c, b, im, x, y, w, h, cur);
}

void soie_document_frames(soie_node *doc, soie_frame_fn fn, void *data)
{
	if (!doc || doc->type != SOIE_DOCUMENT)
		return;
	doc->u.doc->frame_fn = fn;
	doc->u.doc->frame_data = data;
}

/* An iframe: the pixels of its page, from the host, on whole pixels at
 * the content box's corner and clipped to it, as a video frame */
static void paint_frame(struct canvas *c, const struct box *b)
{
	const struct docinfo *d = b->node->doc->u.doc;
	float box[4];
	struct image im;

	/* not in what this paint covers: not asked (an iframe out of view
	 * is not loaded) */
	if (!d->frame_fn || b->x - c->ox >= (float)c->cx1 ||
	    b->x + b->w - c->ox <= (float)c->cx0 ||
	    b->y - c->oy >= (float)c->cy1 || b->y + b->h - c->oy <= (float)c->cy0)
		return;
	box[0] = b->x;
	box[1] = b->y;
	box[2] = b->w;
	box[3] = b->h;
	memset(&im, 0, sizeof(im));
	im.rgba = (unsigned char *)d->frame_fn(d->frame_data,
		(soie_node *)b->node, box, c->bgr, &im.w, &im.h);
	if (!im.rgba || im.w <= 0 || im.h <= 0)
		return;
	im.opaque = 1;
	im.bgr = c->bgr;
	draw_placed(c, b, &im, roundf(b->x), roundf(b->y), (float)im.w,
		(float)im.h, NULL);
}

void paint_replaced(struct canvas *c, const struct box *b)
{
	const struct style *st = b->style;
	const struct image *im = NULL;
	unsigned char cur[4];

	if (!b->node)
		return;
	if (b->node->ns == SOIE_NS_HTML && (b->node->tag == TAG_INPUT ||
	    b->node->tag == TAG_TEXTAREA || b->node->tag == TAG_SELECT)) {
		const char *t = soie_element_get_attr(b->node, "type");

		if (b->node->tag != TAG_INPUT || !t || !ascii_ieq(t, "image")) {
			paint_control(c, b);
			return;
		}
	}
	if (b->node->ns == SOIE_NS_HTML && (!strcmp(b->node->name, "progress") ||
	    !strcmp(b->node->name, "meter"))) {
		paint_control(c, b);
		return;
	}
	if (b->node->ns == SOIE_NS_HTML && (!strcmp(b->node->name, "video") ||
	    !strcmp(b->node->name, "audio"))) {
		paint_media(c, b);
		return;
	}
	if (is_html(b->node, TAG_IFRAME)) {
		paint_frame(c, b);
		return;
	}
	color_to_rgba(st->v[P_COLOR].u.color, cur);
	if (b->node->ns == SOIE_NS_SVG) {
		svg_render(c, b->node, b->x, b->y, b->w, b->h, cur, st, 0);
		return;
	}
	im = image_for(b->node);
	if (im)
		draw_fitted(c, b, im, cur);
}

/* Raw values: a <length-percentage> at the cursor, with its units and
 * calc(), for the element of style st. The caller releases l->calc. */
static int raw_lenp(struct cur *c, const struct style *st, struct len *l)
{
	struct slen *sl = parse_length(c, 0);
	struct ucontext u;

	if (!sl)
		return 0;
	doc_units(st, &u);
	slen_resolve(sl, &u, l);
	slen_free(sl);
	return 1;
}

static float len_at(const struct len *l, float ref)
{
	return l->calc ? calc_eval(l->calc, ref) : l->px + l->pct * ref / 100;
}

static int raw_len(struct cur *c, const struct style *st, float ref,
	float *out)
{
	struct len l;

	if (!raw_lenp(c, st, &l))
		return 0;
	*out = len_at(&l, ref);
	if (l.calc)
		calc_release(l.calc);
	return 1;
}

/*
 * A <position> (CSS Values 4): one to four values, keywords and
 * offsets from them, up to a comma or the end. rw and rh are what the
 * percentages are of (the area less the image); the result is from
 * the area's top left corner.
 */
static int parse_position(struct cur *c, const struct style *st, float rw,
	float rh, float *ox, float *oy)
{
	struct {
		char kw;	/* l r t b c, 0: a length */
		struct len l;
	} it[4];
	const struct cv *v;
	int n = 0, i, ok = 1;
	float x = rw / 2, y = rh / 2;

	memset(it, 0, sizeof(it));
	while (n < 4 && (v = cur_peek(c)) && v->type != CT_COMMA) {
		if (v->type == CT_IDENT) {
			it[n].kw = cv_ident(v, "left") ? 'l' :
				cv_ident(v, "right") ? 'r' :
				cv_ident(v, "top") ? 't' :
				cv_ident(v, "bottom") ? 'b' :
				cv_ident(v, "center") ? 'c' : 0;
			if (!it[n].kw)
				break;
			cur_next(c);
		} else if (!raw_lenp(c, st, &it[n].l)) {
			break;
		}
		n++;
	}
	if (!n)
		return 0;
	if (n <= 2) {
		/* x then y, unless the keywords say the other way */
		int swap = it[0].kw == 't' || it[0].kw == 'b' ||
			(n == 2 && (it[1].kw == 'l' || it[1].kw == 'r'));
		int a = swap ? 1 : 0, b = swap ? 0 : 1;

		if (a < n) {
			if (!it[a].kw)
				x = len_at(&it[a].l, rw);
			else if (it[a].kw == 'l')
				x = 0;
			else if (it[a].kw == 'r')
				x = rw;
			else if (it[a].kw != 'c')
				ok = 0;
		}
		if (b < n) {
			if (!it[b].kw)
				y = len_at(&it[b].l, rh);
			else if (it[b].kw == 't')
				y = 0;
			else if (it[b].kw == 'b')
				y = rh;
			else if (it[b].kw != 'c')
				ok = 0;
		}
	} else {
		/* three or four: each keyword may have an offset after it */
		for (i = 0; i < n; i++) {
			float off = 0;
			char k = it[i].kw;

			if (!k) {
				ok = 0;
				break;
			}
			if (i + 1 < n && !it[i + 1].kw && k != 'c') {
				off = len_at(&it[i + 1].l, k == 'l' || k == 'r' ?
					rw : rh);
				i++;
			}
			if (k == 'l')
				x = off;
			else if (k == 'r')
				x = rw - off;
			else if (k == 't')
				y = off;
			else if (k == 'b')
				y = rh - off;
		}
	}
	for (i = 0; i < n; i++)
		if (it[i].l.calc)
			calc_release(it[i].l.calc);
	if (!ok)
		return 0;
	*ox = x;
	*oy = y;
	return 1;
}

/* Gradients (CSS Images 3 and 4) */

struct stop {
	float c[4];		/* premultiplied, 0..255 */
	float pos;		/* px along the line, turns for a conic; NAN:
				 * to be spread */
	float hint;		/* where the mix with the stop before is even,
				 * NAN for the middle */
};

struct gradient {
	char kind;		/* 'l'inear, 'r'adial, 'c'onic */
	char repeating;
	struct stop *s;
	int n;
	float dx, dy, len;	/* linear: the line's direction and length */
	float cx, cy;		/* radial, conic: the centre, in the tile */
	float rx, ry;		/* radial */
	float from;		/* conic: the start angle, turns */
	/* its table (grad_table()), NULL without one */
	unsigned char *tab;	/* a cell: the color as the canvas orders
				 * it, its alpha times the group's */
	unsigned char *state;	/* a cell: GRAD_EVEN..., 0 not known yet */
	unsigned char *have;	/* a cell: 1 when its color is in tab */
	int ncell, first;	/* first: the cell of the first stop */
	double scale;		/* cells per pixel of the line; radial: per
				 * unit of the squared distance in radii */
	float step;		/* the shortest cell, px */
};

#define MAX_STOPS 1024

static int angle_of(const struct cv *v, float *deg)
{
	if (!v)
		return 0;
	if (v->type == CT_NUMBER && v->num == 0) {
		*deg = 0;
		return 1;
	}
	if (v->type != CT_DIMENSION)
		return 0;
	if (ascii_ieq(v->str, "deg"))
		*deg = (float)v->num;
	else if (ascii_ieq(v->str, "rad"))
		*deg = (float)(v->num * 180 / M_PI);
	else if (ascii_ieq(v->str, "turn"))
		*deg = (float)v->num * 360;
	else if (ascii_ieq(v->str, "grad"))
		*deg = (float)v->num * 0.9f;
	else
		return 0;
	return 1;
}

/* A stop position: a length along the line of len px, a percentage of
 * it; an angle or a percentage of a turn for a conic */
static int stop_pos(struct cur *c, const struct style *st,
	const struct gradient *g, float *out)
{
	const struct cv *v = cur_peek(c);
	float deg;

	if (!v || v->type == CT_COMMA)
		return 0;
	if (g->kind != 'c')
		return raw_len(c, st, g->len, out);
	if (v->type == CT_PERCENT)
		*out = (float)v->num / 100;
	else if (angle_of(v, &deg))
		*out = deg / 360;
	else
		return 0;
	cur_next(c);
	return 1;
}

static int parse_stops(struct cur *c, const struct style *st,
	struct gradient *g, const unsigned char *current)
{
	struct stop *s = NULL;
	int n = 0, cap = 0, i, j, k;
	float hint = NAN, end = g->kind == 'c' ? 1 : g->len;

	while (!cur_end(c) && n < MAX_STOPS) {
		const struct cv *v;
		struct color col;
		unsigned char rgba[4];
		float p;
		int npos = 0;

		if (n + 2 > cap) {
			cap = cap ? cap * 2 : 8;
			s = xrealloc(s, (size_t)cap * sizeof(*s));
		}
		if (!parse_color(c, &col)) {
			/* a position alone: a hint for the next stop */
			if (!n || !isnan(hint) || !stop_pos(c, st, g, &hint))
				goto bad;
			v = cur_next(c);
			if (!v || v->type != CT_COMMA)
				goto bad;
			continue;
		}
		if (col.current)
			memcpy(rgba, current, 4);
		else
			color_to_rgba(&col, rgba);
		for (k = 0; k < 3; k++)
			s[n].c[k] = rgba[k] * rgba[3] / 255.f;
		s[n].c[3] = rgba[3];
		s[n].pos = NAN;
		s[n].hint = hint;
		hint = NAN;
		while (npos < 2 && stop_pos(c, st, g, &p)) {
			if (npos) {	/* a second position: the color again */
				s[n + 1] = s[n];
				s[n + 1].hint = NAN;
				n++;
			}
			s[n].pos = p;
			npos++;
		}
		n++;
		v = cur_next(c);
		if (v && v->type != CT_COMMA)
			goto bad;
	}
	if (n < 1 || !isnan(hint))
		goto bad;
	if (isnan(s[0].pos))
		s[0].pos = 0;
	if (isnan(s[n - 1].pos))
		s[n - 1].pos = end;
	for (i = 1; i < n; i++) {
		float a, b;

		if (!isnan(s[i].pos)) {
			if (s[i].pos < s[i - 1].pos)
				s[i].pos = s[i - 1].pos;
			continue;
		}
		for (j = i; isnan(s[j].pos); j++)
			;
		a = s[i - 1].pos;
		b = s[j].pos < a ? a : s[j].pos;
		for (k = i; k < j; k++)
			s[k].pos = a + (b - a) * (float)(k - i + 1) /
				(float)(j - i + 1);
		/* i goes on: s[j] is checked against s[j - 1] next */
		i = j - 1;
	}
	for (i = 1; i < n; i++)
		if (!isnan(s[i].hint)) {
			float d = s[i].pos - s[i - 1].pos;

			s[i].hint = d > 0 ? (s[i].hint - s[i - 1].pos) / d : NAN;
		}
	g->s = s;
	g->n = n;
	return 1;
bad:
	free(s);
	return 0;
}

static void stop_color(const struct gradient *g, float t, unsigned char *out)
{
	const struct stop *s = g->s;
	const float *a, *b = NULL;
	float f = 0, o[4];
	int i, k, n = g->n;

	if (g->repeating) {
		float period = s[n - 1].pos - s[0].pos;

		if (period <= 0) {
			t = s[n - 1].pos;
		} else {
			t = fmodf(t - s[0].pos, period);
			if (t < 0)
				t += period;
			t += s[0].pos;
		}
	}
	if (t != t || t <= s[0].pos) {	/* NaN: isnan() is a call at -Os */
		a = s[0].c;
	} else if (t >= s[n - 1].pos) {
		a = s[n - 1].c;
	} else {
		for (i = 1; i < n - 1 && t > s[i].pos; i++)
			;
		a = s[i - 1].c;
		b = s[i].c;
		f = (t - s[i - 1].pos) / (s[i].pos - s[i - 1].pos);
		if (!isnan(s[i].hint)) {
			float h = s[i].hint;

			f = h <= 0 ? 1 : h >= 1 ? 0 : powf(f, logf(0.5f) /
				logf(h));
		}
	}
	for (k = 0; k < 4; k++)
		o[k] = b ? a[k] * (1 - f) + b[k] * f : a[k];
	if (o[3] <= 0) {
		memset(out, 0, 4);
		return;
	}
	for (k = 0; k < 3; k++) {
		float v = o[k] * 255 / o[3];

		out[k] = (unsigned char)(v > 255 ? 255 : v + 0.5f);
	}
	out[3] = (unsigned char)(o[3] + 0.5f);
}

/* Skip `in <color space> [<hue> hue]`: mixed in sRGB all the same */
static int skip_in(struct cur *c)
{
	const struct cv *v = cur_peek(c);

	if (!cv_ident(v, "in"))
		return 0;
	while ((v = cur_peek(c)) && v->type == CT_IDENT)
		cur_next(c);
	return 1;
}

/* The size of a radial gradient: keywords, a radius, two radii */
static void radial_size(struct gradient *g, float w, float h, int circle,
	int sized, const char *ext, const struct len *r)
{
	float l = g->cx, rt = w - g->cx, t = g->cy, bt = h - g->cy;
	float sx, sy;
	int corner, closest;

	if (sized == 1) {
		g->rx = g->ry = len_at(&r[0], w);
		return;
	}
	if (sized == 2) {
		g->rx = len_at(&r[0], w);
		g->ry = len_at(&r[1], h);
		return;
	}
	l = fabsf(l);
	rt = fabsf(rt);
	t = fabsf(t);
	bt = fabsf(bt);
	closest = ext && !strncmp(ext, "closest", 7);
	corner = !ext || strstr(ext, "corner") != NULL;
	sx = closest ? fminf(l, rt) : fmaxf(l, rt);
	sy = closest ? fminf(t, bt) : fmaxf(t, bt);
	if (circle) {
		if (corner)
			g->rx = g->ry = sqrtf(sx * sx + sy * sy);
		else
			g->rx = g->ry = closest ? fminf(sx, sy) : fmaxf(sx, sy);
	} else {
		/* an ellipse through the corner keeps the sides' ratio */
		g->rx = corner ? sx * 1.41421356f : sx;
		g->ry = corner ? sy * 1.41421356f : sy;
	}
}

/* What comes before the stops, for an image of w x h */
static int gradient_parse(const struct cv *f, const struct style *st,
	float w, float h, const unsigned char *current, struct gradient *g)
{
	const char *name = f->str;
	struct cur cu;
	const struct cv *v;
	float deg = 180;
	int config = 0;

	memset(g, 0, sizeof(*g));
	if (!strncmp(name, "-webkit-", 8))
		name += 8;
	if (!strncmp(name, "repeating-", 10)) {
		g->repeating = 1;
		name += 10;
	}
	g->kind = ascii_ieq(name, "linear-gradient") ? 'l' :
		ascii_ieq(name, "radial-gradient") ? 'r' :
		ascii_ieq(name, "conic-gradient") ? 'c' : 0;
	if (!g->kind)
		return 0;
	cur_init(&cu, &f->kids);
	g->cx = w / 2;
	g->cy = h / 2;
	if (g->kind == 'l') {
		int legacy = !strncmp(f->str, "-webkit-", 8);

		config = skip_in(&cu);
		v = cur_peek(&cu);
		if (angle_of(v, &deg) && v->type == CT_DIMENSION) {
			/* the prefixed form counts from the right, the other
			 * way round */
			if (legacy)
				deg = 90 - deg;
			cur_next(&cu);
			config = 1;
		} else if (cv_ident(v, "to") || (legacy && v &&
		    v->type == CT_IDENT && (cv_ident(v, "top") ||
		    cv_ident(v, "bottom") || cv_ident(v, "left") ||
		    cv_ident(v, "right")))) {
			int top = 0, bottom = 0, left = 0, right = 0;

			if (cv_ident(v, "to"))
				cur_next(&cu);
			while ((v = cur_peek(&cu)) && v->type == CT_IDENT &&
			       !cv_ident(v, "in")) {
				top |= cv_ident(v, "top");
				bottom |= cv_ident(v, "bottom");
				left |= cv_ident(v, "left");
				right |= cv_ident(v, "right");
				cur_next(&cu);
			}
			if (legacy) {	/* it names where it starts */
				int x = top;

				top = bottom;
				bottom = x;
				x = left;
				left = right;
				right = x;
			}
			deg = top ? 0 : bottom ? 180 : left ? 270 : 90;
			if ((top || bottom) && (left || right) && w > 0 &&
			    h > 0) {
				/* to a corner: the 50 % line joins the two
				 * other corners */
				float a = atanf(h / w) * 180 / (float)M_PI;

				deg = top ? (right ? a : 360 - a) :
					(right ? 180 - a : 180 + a);
			}
			config = 1;
		}
		config |= skip_in(&cu);
		{
			float r = deg * (float)M_PI / 180;

			g->dx = sinf(r);
			g->dy = -cosf(r);
			g->len = fabsf(w * g->dx) + fabsf(h * g->dy);
		}
	} else if (g->kind == 'r') {
		struct cur t = cu;
		struct color col;
		struct len r[2];
		const char *ext = NULL;
		int circle = 0, ellipse = 0, sized = 0;

		memset(r, 0, sizeof(r));
		config = skip_in(&cu);
		t = cu;
		if (!config && parse_color(&t, &col)) {
			/* stops right away */
		} else {
			while ((v = cur_peek(&cu)) && v->type != CT_COMMA) {
				if (cv_ident(v, "circle")) {
					circle = 1;
				} else if (cv_ident(v, "ellipse")) {
					ellipse = 1;
				} else if (v->type == CT_IDENT && (strstr(v->str,
				    "-side") || strstr(v->str, "-corner"))) {
					ext = v->str;
				} else if (cv_ident(v, "at")) {
					cur_next(&cu);
					if (!parse_position(&cu, st, w, h, &g->cx,
					    &g->cy))
						break;
					continue;
				} else if (cv_ident(v, "in")) {
					skip_in(&cu);
					continue;
				} else if (sized < 2 && raw_lenp(&cu, st,
				    &r[sized])) {
					sized++;
					continue;
				} else {
					break;
				}
				cur_next(&cu);
			}
			config = 1;
		}
		if (sized == 1 && !ellipse)
			circle = 1;
		radial_size(g, w, h, circle && !ellipse, sized, ext, r);
		if (r[0].calc)
			calc_release(r[0].calc);
		if (r[1].calc)
			calc_release(r[1].calc);
		g->len = g->rx;
	} else {
		struct cur t;
		struct color col;

		config = skip_in(&cu);
		t = cu;
		if (!config && parse_color(&t, &col)) {
			/* stops right away */
		} else {
			while ((v = cur_peek(&cu)) && v->type != CT_COMMA) {
				if (cv_ident(v, "from")) {
					cur_next(&cu);
					if (angle_of(cur_peek(&cu), &deg))
						g->from = deg / 360;
					else
						break;
				} else if (cv_ident(v, "at")) {
					cur_next(&cu);
					if (!parse_position(&cu, st, w, h, &g->cx,
					    &g->cy))
						break;
					continue;
				} else if (cv_ident(v, "in")) {
					skip_in(&cu);
					continue;
				} else {
					break;
				}
				cur_next(&cu);
			}
			config = 1;
		}
	}
	if (config) {
		v = cur_next(&cu);
		if (!v || v->type != CT_COMMA)
			return 0;
	}
	return parse_stops(&cu, st, g, current);
}

/* Where the pixel centre (px, py) of a tile of w x h is on the line */
static float grad_t(const struct gradient *g, float px, float py, float w,
	float h)
{
	float t;

	if (g->kind == 'l') {
		t = (px - w / 2) * g->dx + (py - h / 2) * g->dy + g->len / 2;
	} else if (g->kind == 'r') {
		float ex = px - g->cx, ey = py - g->cy;

		if (g->rx <= 0 || g->ry <= 0)
			t = 1e30f;
		else
			t = sqrtf(ex * ex / (g->rx * g->rx) +
				ey * ey / (g->ry * g->ry)) * g->rx;
	} else {
		t = atan2f(px - g->cx, g->cy - py) / (2 * (float)M_PI) -
			g->from;
		t -= floorf(t);
	}
	return t;
}

/*
 * The table of a gradient: its colors along the line, so that a pixel
 * is an index and a store or an integer blend instead of stop_color()
 * and its floats. Cells are an eighth of a pixel long or more,
 * GRAD_CELLS at most; a linear gradient's are even along the line, a
 * radial one's along the square of the distance (no square root per
 * pixel).
 *
 * A cell holds the color at its start. Its pixels take it when the
 * cell is even: no stop in it, where the mix turns, the same alpha at
 * its two ends and colors one level apart at most. They are then
 * within one level of what stop_color() gives, and stay so under any
 * number of layers: with the same alpha, a level in the color or in
 * what is below is a level at most once blended; a level of alpha
 * would add up. The pixels of the other cells are computed as before:
 * a hard stop is as sharp as it was, at the same pixels, and a steep
 * gradient is what it was. The index is fixed point and may be one
 * cell away from where the floats of the exact path put a pixel: the
 * two cells around count too.
 *
 * A pixel of the page gets the same color whatever is painted of it
 * (the whole page, the band a scroll brought in, a rectangle): bands
 * are compared with the whole byte for byte. So nothing here looks at
 * the canvas' clip, origin or size: the table's cells and what each
 * is depend on the gradient alone, a pixel's index on its place in
 * the tile. Cells are filled when a pixel first falls in them: a band
 * of a tall gradient computes the few it crosses.
 */
#define GRAD_CELLS 4096
#define GRAD_BIAS (1 << 30)	/* keeps the fixed point index positive */
/* what a cell is */
#define GRAD_EVEN 1		/* its pixels take its color */
#define GRAD_UNEVEN 2		/* its colors differ: they are computed */
#define GRAD_STOP 3		/* a stop in it or next to it: computed */

/* Where t is in the table, in cells */
static double grad_cell(const struct gradient *g, double t)
{
	if (g->kind == 'r')
		return t / g->rx * (t / g->rx) * g->scale;
	return (t - g->s[0].pos) * g->scale + g->first;
}

/* Color b for a: the same alpha, a level at most between the colors */
static int colors_even(const unsigned char *a, const unsigned char *b)
{
	if (a[3] != b[3])
		return 0;
	/* nothing is painted with no alpha: any color will do */
	return !a[3] || (abs(a[0] - b[0]) <= 1 && abs(a[1] - b[1]) <= 1 &&
		abs(a[2] - b[2]) <= 1);
}

/* The color at t as the table keeps it: in the canvas' order, its
 * alpha times the group's as blend_px() does */
static void grad_color(const struct gradient *g, const struct canvas *c,
	float t, unsigned char *e)
{
	unsigned char col[4];
	int r = c->bgr ? 2 : 0;

	stop_color(g, t, col);
	e[0] = col[r];
	e[1] = col[1];
	e[2] = col[2 - r];
	e[3] = (unsigned char)(int)((float)col[3] * c->alpha + 0.5f);
}

/* The table of g, empty; none (g->tab stays NULL) for the gradients
 * it cannot be right for */
static void grad_table(struct gradient *g)
{
	float p0 = g->s[0].pos, p1 = g->s[g->n - 1].pos, range = p1 - p0;
	int n, i, k, m;

	if (!(range >= 0) || !(fabsf(p0) < 1e6f) || !(p1 < 1e6f))
		return;
	if (g->kind == 'l' && !g->repeating) {
		/* three cells before the first stop and after the last
		 * one: the outer ones are all that is beyond */
		float step = range > GRAD_CELLS * 0.125f ? range / GRAD_CELLS :
			0.125f;

		g->scale = 1 / step;
		g->first = 3;
		n = (int)(range * g->scale) + 7;
	} else if (g->kind == 'l') {
		/* a period is a power of two of cells: the index wraps */
		for (n = 8; (float)n < range * 4 && n < GRAD_CELLS; n *= 2)
			;
		if ((float)n > range * 8)
			return;
		g->scale = n / range;
	} else if (g->kind == 'r' && !g->repeating && g->rx > 0 &&
		   g->ry > 0 && p1 >= 1) {
		/* from the centre to the last stop, where a cell is the
		 * shortest: p1 / 2n; then two cells and all the rest */
		n = p1 * 4 < GRAD_CELLS ? (int)(p1 * 4) : GRAD_CELLS;
		g->scale = n / ((double)p1 / g->rx * ((double)p1 / g->rx));
		g->step = p1 / (float)(2 * n);
		n += 3;
	} else {
		return;
	}
	if (g->kind == 'l')
		g->step = (float)(1 / g->scale);
	g->ncell = n;
	g->tab = xmalloc((size_t)n * 6 + 5);
	g->state = g->tab + (size_t)n * 4 + 4;
	g->have = g->state + n;
	memset(g->state, 0, (size_t)n * 2 + 1);
	/* a stop: its cell (two when it is where they meet) and the
	 * ones around */
	for (i = 0; i < g->n; i++) {
		double u = g->s[i].pos < 0 && g->kind == 'r' ? -9 :
			grad_cell(g, g->s[i].pos);

		if (!(u >= -4 && u <= n + 4))
			continue;
		m = (int)floor(u);
		for (k = m - 2; k <= m + 1; k++)
			if (g->repeating)
				g->state[k & (n - 1)] = GRAD_STOP;
			else if (k >= 0 && k < n)
				g->state[k] = GRAD_STOP;
	}
	/* the outer cells are not a stop's: see grad_state() */
	if (!g->repeating) {
		g->state[n - 1] = 0;
		if (g->kind == 'l')
			g->state[0] = 0;
	}
}

/* The color at the start of cell k, computed the first time */
static const unsigned char *grad_edge(const struct gradient *g,
	const struct canvas *c, int k)
{
	unsigned char *e = g->tab + (size_t)k * 4;

	if (!g->have[k]) {
		grad_color(g, c, g->kind == 'r' ?
			g->rx * sqrtf((float)(k / g->scale)) :
			g->s[0].pos + (float)((k - g->first) / g->scale), e);
		g->have[k] = 1;
	}
	return e;
}

/* What cell k is, found when a pixel first falls in it: from its
 * color and those of the cell before, of the next one and of that
 * one's end */
static int grad_state(const struct gradient *g, const struct canvas *c,
	int k)
{
	const unsigned char *e = grad_edge(g, c, k);
	int n = g->ncell, s;

	if (!g->repeating && (k == n - 1 || (k == 0 && g->kind == 'l')))
		/* beyond the stops: the first or the last color, for sure */
		s = GRAD_EVEN;
	else if (k == 0 || k == n - 1)
		s = GRAD_UNEVEN;
	else
		s = colors_even(e, grad_edge(g, c, k - 1)) &&
			colors_even(e, grad_edge(g, c, k + 1)) &&
			colors_even(e, grad_edge(g, c, k + 2)) ? GRAD_EVEN :
			GRAD_UNEVEN;
	g->state[k] = (unsigned char)s;
	return s;
}

/* The cell of the fixed point index v */
static inline int grad_index(const struct gradient *g, uint64_t v)
{
	int k = (int)(v >> 32);

	if (g->repeating)
		return k & (g->ncell - 1);
	k -= GRAD_BIAS;
	return k < 0 ? 0 : k >= g->ncell ? g->ncell - 1 : k;
}

/* The rows of a tile, each pixel's color computed: every gradient
 * without a table */
static void grad_exact(struct canvas *c, const struct gradient *g, float x,
	float y, float w, float h, int i0, int j0, int i1, int j1)
{
	int i, j;

	for (j = j0; j < j1; j++) {
		float py = (float)j + c->oy + 0.5f - y;
		unsigned char col[4] = { 0, 0, 0, 0 };
		float tl = NAN;

		for (i = i0; i < i1; i++) {
			float px = (float)i + c->ox + 0.5f - x;
			float t = grad_t(g, px, py, w, h);

			/* the same place on the line: the same color (the
			 * rows of a horizontal gradient) */
			if (t != tl) {
				stop_color(g, t, col);
				tl = t;
			}
			blend_px(c, i, j, col, 1);
		}
	}
}

/* Pixel p (i, j of the canvas) takes the table's color e */
static inline void grad_put(const struct canvas *c, unsigned char *p,
	const unsigned char *e, int i, int j)
{
	int al = e[3], na;

	if (c->mask)
		al = div255(al * mask_at(c, i, j));
	if (al == 255) {
		memcpy(p, e, 4);
	} else if (al > 0) {
		na = 255 - al;
		p[0] = (unsigned char)div255(e[0] * al + p[0] * na);
		p[1] = (unsigned char)div255(e[1] * al + p[1] * na);
		p[2] = (unsigned char)div255(e[2] * al + p[2] * na);
		p[3] = (unsigned char)(al + div255(p[3] * na));
	}
}

/*
 * Row j of a tile from the table. A pixel's cell is v >> 32; v grows
 * by d from a pixel to the next, and d by dd: nothing for a linear
 * gradient, while a radial one's index is a square, whose second
 * difference is constant. Unsigned 64 bits: they wrap, and on a 32-bit
 * machine the index is the high word as it is.
 */
static void grad_row(struct canvas *c, const struct gradient *g, float x,
	float y, float w, float h, int j, int i0, int i1, uint64_t v,
	uint64_t d, uint64_t dd)
{
	unsigned char *p = c->px + (size_t)j * (size_t)c->stride +
		(size_t)i0 * 4;
	int i;

	for (i = i0; i < i1; i++, p += 4, v += d, d += dd) {
		int k = grad_index(g, v), s = g->state[k];

		if (!s)
			s = grad_state(g, c, k);
		if (s == GRAD_EVEN)
			grad_put(c, p, g->tab + (size_t)k * 4, i, j);
		else
			grad_exact(c, g, x, y, w, h, i, j, i + 1, j + 1);
	}
}

/* One tile of gradient g at (x, y), w x h, inside the canvas clip */
static void gradient_draw(struct canvas *c, const struct gradient *g,
	float x, float y, float w, float h)
{
	int i0 = ifloor(x - c->ox + 0.5f), i1 = ifloor(x + w -
		c->ox + 0.5f);
	int j0 = ifloor(y - c->oy + 0.5f), j1 = ifloor(y + h -
		c->oy + 0.5f);
	double a = 0, bx, by, u, far, col, lx;
	int64_t n0;
	int i, j, k, s, flat;

	if (i0 < c->cx0)
		i0 = c->cx0;
	if (j0 < c->cy0)
		j0 = c->cy0;
	if (i1 > c->cx1)
		i1 = c->cx1;
	if (j1 > c->cy1)
		j1 = c->cy1;
	if (i0 >= i1 || j0 >= j1)
		return;
	/* The exact path's floats are good to 2^-21 or so of the largest
	 * number they meet: the table's index must stay within a cell of
	 * them. Far down a page (32768 px for cells of 1/8) the tile is
	 * exact. */
	far = fmax(fabsf(x) + w, fabsf(y) + h);
	if (g->kind == 'r')
		far = fmax(far, fmax(fabsf(g->cx), fabsf(g->cy)));
	if (!g->tab || !(far < g->step * 262144.)) {
		grad_exact(c, g, x, y, w, h, i0, j0, i1, j1);
		return;
	}
	/* the index of the pixel whose centre is at (lx, ly) in the
	 * tile, in cells: a + lx bx + ly by for a linear gradient,
	 * bx (lx - cx)^2 + by (ly - cy)^2 for a radial one; small enough
	 * at the tile's corners for the fixed point, so in all of it */
	if (g->kind == 'l') {
		bx = g->dx * g->scale;
		by = g->dy * g->scale;
		a = grad_cell(g, g->len / 2 - (double)w / 2 * g->dx -
			(double)h / 2 * g->dy);
		u = fabs(a) + (fabs(bx) + fabs(by)) * 2 + fabs(bx) * w +
			fabs(by) * h;
	} else {
		double ex = fmax(fabs(g->cx), fabs(w - g->cx)) + 2;
		double ey = fmax(fabs(g->cy), fabs(h - g->cy)) + 2;

		bx = g->scale / ((double)g->rx * g->rx);
		by = g->scale / ((double)g->ry * g->ry);
		u = bx * ex * ex + by * ey * ey;
	}
	if (!(u < 1e8)) {
		grad_exact(c, g, x, y, w, h, i0, j0, i1, j1);
		return;
	}
	/* a vertical gradient: a row is one place on the line */
	flat = g->kind == 'l' && fabs(bx) * (w + 2) < 1. / 64;
	/* Rows start from the tile's first column of pixels, not from the
	 * clip's: the index of column i0 is what n0 additions from there
	 * give, and a pixel has the same whatever the clip. lx: that
	 * column's centre in the tile. */
	col = i0 + (double)c->ox - floor(x);
	n0 = (int64_t)floor(col);
	lx = floor(x) + (col - floor(col)) + 0.5 - x;
	for (j = j0; j < j1; j++) {
		unsigned char *p = c->px + (size_t)j * (size_t)c->stride +
			(size_t)i0 * 4;
		double ly = j + (double)c->oy + 0.5 - y;
		uint64_t v, v0, d, dd = 0;
		unsigned char e0[4], e1[4];
		const unsigned char *e;

		if (g->kind == 'l') {
			u = a + lx * bx + ly * by;
			d = (uint64_t)(int64_t)(bx * 4294967296.);
		} else {
			double ex = lx - g->cx, ey = ly - g->cy;

			u = bx * ex * ex + by * ey * ey;
			d = (uint64_t)(int64_t)(bx * (2 * ex + 1) * 4294967296.);
			dd = (uint64_t)(bx * 2 * 4294967296.);
		}
		v0 = (uint64_t)((u + GRAD_BIAS) * 4294967296.);
		v = v0 + (uint64_t)n0 * d + (uint64_t)(n0 * (n0 - 1) / 2) * dd;
		d += (uint64_t)n0 * dd;
		if (!flat) {
			grad_row(c, g, x, y, w, h, j, i0, i1, v, d, dd);
			continue;
		}
		/* the row's cell: the first column's */
		k = grad_index(g, v0);
		if (!(s = g->state[k]))
			s = grad_state(g, c, k);
		e = g->tab + (size_t)k * 4;
		if (s == GRAD_UNEVEN) {
			/* the colors half a cell before and after, further
			 * than the exact path's floats are from here: the
			 * same, and the row has one */
			float t = g->s[0].pos + (float)((u - g->first) /
				g->scale);

			grad_color(g, c, t - g->step / 2, e0);
			grad_color(g, c, t + g->step / 2, e1);
			if (colors_even(e0, e1)) {
				e = e0;
				s = GRAD_EVEN;
			}
		}
		if (s != GRAD_EVEN)
			grad_row(c, g, x, y, w, h, j, i0, i1, v, d, dd);
		else if (e[3] == 255 && !c->mask)
			for (i = i0; i < i1; i++, p += 4)
				memcpy(p, e, 4);
		else if (e[3])
			for (i = i0; i < i1; i++, p += 4)
				grad_put(c, p, e, i, j);
	}
}

static int nth_layer(const struct cvlist *l, int k, struct cur *out)
{
	int i, start = 0, n = 0;

	for (i = 0; i <= l->n; i++)
		if (i == l->n || l->v[i].type == CT_COMMA) {
			if (n == k) {
				out->v = l->v;
				out->i = start;
				out->n = i;
				return 1;
			}
			n++;
			start = i + 1;
		}
	return 0;
}

/* The value of layer k of a list that repeats when it is shorter */
static int layer_value(const struct cvlist *l, int k, struct cur *out)
{
	int n = 1, i;

	for (i = 0; i < l->n; i++)
		if (l->v[i].type == CT_COMMA)
			n++;
	return nth_layer(l, k % n, out);
}

/* A box of b by its keyword: border-box (the rectangle given),
 * padding-box (no keyword too, when padding is set), content-box */
static void box_of(const struct box *b, const struct cv *v, int padding,
	float *x, float *y, float *w, float *h)
{
	float e[4] = { 0, 0, 0, 0 };
	int i;

	if (v)
		padding = cv_ident(v, "padding-box");
	if (padding || cv_ident(v, "content-box"))
		for (i = 0; i < 4; i++)
			e[i] = b->b[i] + (!padding ? b->p[i] : 0);
	*x += e[3];
	*y += e[0];
	*w -= e[1] + e[3];
	*h -= e[0] + e[2];
	if (*w < 0)
		*w = 0;
	if (*h < 0)
		*h = 0;
}

/* Along one axis: the tiles to paint and how many, for an area at a
 * of size as, tiles of size *ts placed at off, painted from c0 to c1:
 * *org is where tile 0 is and *k0 the number of the first one to
 * paint, so that a tile is at the same place whatever is painted
 * (counted from the first one painted, in floats, bands of a page
 * did not always agree with the whole). mode: 'r'epeat, 'n'o-repeat,
 * 's'pace, rou'N'd (the size was rounded by the caller). *gap is what
 * space puts between tiles. */
static int tiles(int mode, float a, float as, float ts, float off, float c0,
	float c1, double *org, double *k0, float *gap)
{
	double p = (double)a + off, n, step;

	*gap = 0;
	*org = p;
	*k0 = 0;
	if (mode == 'n' || ts <= 0)
		return 1;
	if (mode == 's') {
		int k = ifloor(as / ts + 0.001f);

		if (k < 2)	/* one tile: where the position says */
			return 1;
		*gap = (as - (float)k * ts) / (float)(k - 1);
		p = a;
	}
	/* back to the first tile that reaches c0, without counting; in
	 * doubles: ten billion pixels away a float no longer tells one
	 * tile from the next, and the tiles ran over the whole distance */
	step = (double)ts + *gap;
	n = ceil((p - c0) / step);
	*org = p;
	*k0 = -n;
	p -= n * step;
	n = ceil((c1 - p) / step);
	if (!(n >= 1))
		return 1;
	/* no more than what is painted can hold */
	step = (c1 - c0) / step + 2;
	return n > step ? (int)step : n > 100000 ? 100000 : (int)n;
}

/*
 * The image layers of property img (background-image, mask-image) of
 * b, whose border box is (x, y, w, h): each in its positioning area
 * (background-origin), at its size and position, repeated, clipped to
 * its background-clip box. With canvas set (the root's background, CSS
 * 2 14.2) they are clipped by the canvas alone.
 */
static void paint_layers(struct canvas *c, const struct box *b, int img_p,
	float x, float y, float w, float h, int canvas)
{
	const struct style *st = b->style;
	const char *img = st->v[img_p].u.str;
	int mask = img_p == P_MASK_IMAGE;
	int pos_p = mask ? P_MASK_POSITION : P_BACKGROUND_POSITION;
	int size_p = mask ? P_MASK_SIZE : P_BACKGROUND_SIZE;
	int rep_p = mask ? P_MASK_REPEAT : P_BACKGROUND_REPEAT;
	struct cvlist li, lp, ls, lr, lo, lc;
	unsigned char cur[4];
	int layer, nlayers = 1, i;
	int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;

	if (!img || !strcmp(img, "none") || c->cover)
		return;
	css_component_values(img, strlen(img), &li);
	css_component_values(st->v[pos_p].u.str, strlen(st->v[pos_p].u.str),
		&lp);
	css_component_values(st->v[size_p].u.str, strlen(st->v[size_p].u.str),
		&ls);
	css_component_values(st->v[rep_p].u.str, strlen(st->v[rep_p].u.str),
		&lr);
	memset(&lo, 0, sizeof(lo));
	memset(&lc, 0, sizeof(lc));
	if (!mask) {
		const char *o = st->v[P_BACKGROUND_ORIGIN].u.str;
		const char *k = st->v[P_BACKGROUND_CLIP].u.str;

		css_component_values(o, strlen(o), &lo);
		css_component_values(k, strlen(k), &lc);
	}
	color_to_rgba(st->v[P_COLOR].u.color, cur);
	for (i = 0; i < li.n; i++)
		if (li.v[i].type == CT_COMMA)
			nlayers++;
	/* the first layer is on top: paint from the last */
	for (layer = nlayers - 1; layer >= 0; layer--) {
		struct cur ci, cp, cs, cr, co;
		const struct cv *v, *a, *bb;
		const struct image *im = NULL;
		struct gradient g;
		float ax = x, ay = y, aw = w, ah = h;	/* positioning area */
		float kx = x, ky = y, kw_ = w, kh = h;	/* clip box */
		float iw = 0, ih = 0, ox = 0, oy = 0, gx, gy;
		double fx, fy, kx0, ky0;
		int mx = 'r', my = 'r', nx, ny, ix, iy, grad = 0, sized = 0;

		if (!nth_layer(&li, layer, &ci) || !(v = cur_peek(&ci)))
			continue;
		if (v->type == CT_FUNCTION && strstr(v->str, "gradient"))
			grad = 1;
		else if (v->type == CT_URL)
			im = image_get(paint_doc, v->str);
		else if (cv_func(v, "url") && v->kids.n &&
			 v->kids.v[0].type == CT_STRING)
			im = image_get(paint_doc, v->kids.v[0].str);
		if (!grad && (!im || !im->w || !im->h))
			continue;
		if (!mask) {
			a = layer_value(&lo, layer, &co) ? cur_peek(&co) : NULL;
			box_of(b, a, 1, &ax, &ay, &aw, &ah);
			if (layer_value(&lc, layer, &co) && (a = cur_peek(&co)))
				box_of(b, a, 0, &kx, &ky, &kw_, &kh);
		}
		if (im) {
			iw = (float)im->w * doc_zoom();
			ih = (float)im->h * doc_zoom();
		}
		/* background-size */
		a = bb = NULL;
		if (layer_value(&ls, layer, &cs)) {
			struct cur t;
			float nw = -1, nh = -1;

			a = cur_peek(&cs);
			if (cv_ident(a, "cover") || cv_ident(a, "contain")) {
				if (im) {
					float s = aw / iw, k = ah / ih;

					s = cv_ident(a, "cover") ? (s > k ? s : k) :
						(s < k ? s : k);
					iw *= s;
					ih *= s;
					sized = 1;
				}
			} else if (a) {
				if (cv_ident(a, "auto"))
					cur_next(&cs);
				else if (!raw_len(&cs, st, aw, &nw))
					nw = -1;
				t = cs;
				bb = cur_peek(&t);
				if (bb && !cv_ident(bb, "auto") &&
				    !raw_len(&cs, st, ah, &nh))
					nh = -1;
				if (nw >= 0 && nh >= 0) {
					iw = nw;
					ih = nh;
				} else if (nw >= 0) {
					ih = im && iw > 0 ? nw * ih / iw : ah;
					iw = nw;
				} else if (nh >= 0) {
					iw = im && ih > 0 ? nh * iw / ih : aw;
					ih = nh;
				}
				sized = nw >= 0 || nh >= 0;
			}
		}
		if (grad && !sized) {
			iw = aw;
			ih = ah;
		}
		if (layer_value(&lr, layer, &cr)) {
			static const struct {
				const char *n;
				char x, y;
			} modes[] = { { "repeat", 'r', 'r' }, { "no-repeat", 'n',
				'n' }, { "repeat-x", 'r', 'n' }, { "repeat-y", 'n',
				'r' }, { "space", 's', 's' }, { "round", 'N',
				'N' } };
			unsigned k;

			v = cur_next(&cr);
			for (k = 0; k < sizeof(modes) / sizeof(*modes); k++)
				if (cv_ident(v, modes[k].n)) {
					mx = modes[k].x;
					my = modes[k].y;
				}
			v = cur_next(&cr);
			for (k = 0; v && k < sizeof(modes) / sizeof(*modes); k++)
				if (cv_ident(v, modes[k].n))
					my = modes[k].y;
		}
		/* round: a whole number of tiles, the other size follows
		 * when it was automatic */
		if (mx == 'N' && iw > 0 && aw > 0) {
			float n = floorf(aw / iw + 0.5f), nw = aw / (n < 1 ? 1 : n);

			if (my != 'N' && im && !bb)
				ih = ih * nw / iw;
			iw = nw;
			mx = 'r';
		}
		if (my == 'N' && ih > 0 && ah > 0) {
			float n = floorf(ah / ih + 0.5f), nh = ah / (n < 1 ? 1 : n);

			ih = nh;
			my = 'r';
		}
		if (iw < 0.5f || ih < 0.5f)
			continue;
		if (layer_value(&lp, layer, &cp))
			parse_position(&cp, st, aw - iw, ah - ih, &ox, &oy);
		if (grad && !gradient_parse(cur_peek(&ci), st, iw, ih, cur, &g))
			continue;
		if (canvas) {
			kx = c->ox + (float)cx0;
			ky = c->oy + (float)cy0;
			kw_ = (float)(cx1 - cx0);
			kh = (float)(cy1 - cy0);
		} else {
			/* clip to its box: bounded as floats first, a box ten
			 * billion pixels away does not fit an int (its tiles
			 * were then painted all the way to it) */
			float fa0 = floorf(kx - c->ox + 0.5f);
			float fb0 = floorf(ky - c->oy + 0.5f);
			float fa1 = floorf(kx + kw_ - c->ox + 0.5f);
			float fb1 = floorf(ky + kh - c->oy + 0.5f);

			c->cx0 = fa0 > (float)cx0 ? fa0 >= (float)cx1 ? cx1 :
				(int)fa0 : cx0;
			c->cy0 = fb0 > (float)cy0 ? fb0 >= (float)cy1 ? cy1 :
				(int)fb0 : cy0;
			c->cx1 = fa1 < (float)cx1 ? fa1 <= (float)cx0 ? cx0 :
				(int)fa1 : cx1;
			c->cy1 = fb1 < (float)cy1 ? fb1 <= (float)cy0 ? cy0 :
				(int)fb1 : cy1;
			/* and what is painted of it, for the tiles to walk */
			kx = c->ox + (float)c->cx0;
			ky = c->oy + (float)c->cy0;
			kw_ = (float)(c->cx1 - c->cx0);
			kh = (float)(c->cy1 - c->cy0);
		}
		if (kw_ > 0 && kh > 0) {
			nx = tiles(mx, ax, aw, iw, ox, kx, kx + kw_, &fx, &kx0,
				&gx);
			ny = tiles(my, ay, ah, ih, oy, ky, ky + kh, &fy, &ky0,
				&gy);
			if (grad)
				grad_table(&g);
			for (iy = 0; iy < ny; iy++)
				for (ix = 0; ix < nx; ix++) {
					float tx = (float)(fx + (kx0 + ix) *
						((double)iw + gx));
					float ty = (float)(fy + (ky0 + iy) *
						((double)ih + gy));

					if (grad)
						gradient_draw(c, &g, tx, ty, iw, ih);
					else
						draw_image(c, im, tx, ty, iw, ih,
							cur, st);
				}
		}
		if (grad) {
			free(g.s);
			free(g.tab);
		}
		c->cx0 = cx0;
		c->cy0 = cy0;
		c->cx1 = cx1;
		c->cy1 = cy1;
	}
	cvlist_free(&li);
	cvlist_free(&lp);
	cvlist_free(&ls);
	cvlist_free(&lr);
	cvlist_free(&lo);
	cvlist_free(&lc);
}

void paint_layers_of(struct canvas *c, const struct box *b, int img_p,
	float x, float y, float w, float h)
{
	paint_layers(c, b, img_p, x, y, w, h, 0);
}

void paint_bg_image(struct canvas *c, const struct box *b, float x, float y,
	float w, float h)
{
	paint_layers(c, b, P_BACKGROUND_IMAGE, x, y, w, h, 0);
}

/* The root's (or the body's) background images over the whole canvas,
 * placed by that element's box */
void paint_bg_canvas(struct canvas *c, const struct box *b)
{
	paint_layers(c, b, P_BACKGROUND_IMAGE, b->x - b->p[3] - b->b[3],
		b->y - b->p[0] - b->b[0], b->w + b->p[1] + b->p[3] + b->b[1] +
		b->b[3], b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2], 1);
}
