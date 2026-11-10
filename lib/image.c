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

/* An image by URL (resolved against the base): fetched once. */
const struct image *image_get(soie_node *doc, const char *url)
{
	struct images *im = doc->u.doc->images;
	struct image *x;
	char *abs, *buf = NULL;
	size_t len = 0;
	int i, r;

	if (!im || !url || !*url)
		return NULL;
	abs = soie_url_resolve(im->base, url);
	for (i = 0; i < im->n; i++)
		if (!strcmp(im->v[i].url, abs)) {
			free(abs);
			x = &im->v[i];
			if (x->failed == LATER)
				goto fetch;	/* arrived by now? */
			return x->failed ? NULL : x;
		}
	if (im->n == im->cap) {
		im->cap = im->cap ? im->cap * 2 : 16;
		im->v = xrealloc(im->v, (size_t)im->cap * sizeof(*im->v));
	}
	x = &im->v[im->n++];
	memset(x, 0, sizeof(*x));
	x->url = abs;
fetch:
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
	return x->failed ? NULL : x;
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
		al = al * mask_at(c, x, y) / 255;
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
	p[0] = (unsigned char)((s[r] * al + p[0] * (255 - al)) / 255);
	p[1] = (unsigned char)((s[1] * al + p[1] * (255 - al)) / 255);
	p[2] = (unsigned char)((s[2 - r] * al + p[2] * (255 - al)) / 255);
	p[3] = (unsigned char)(al + p[3] * (255 - al) / 255);
}

/* Image into the page rect (x, y, w, h), bilinear, clipped. */
void draw_image(struct canvas *c, const struct image *im, float x, float y,
	float w, float h, const unsigned char *current,
	const struct style *st)
{
	const unsigned char *px;
	int i0, j0, i1, j1, i, j;

	if (w <= 0 || h <= 0)
		return;
	if (im->svg) {
		svg_render(c, im->svg, x, y, w, h, current, st, 1);
		return;
	}
	if (!im->w || !im->h)
		return;
	i0 = (int)floorf(x - c->ox);
	j0 = (int)floorf(y - c->oy);
	i1 = (int)ceilf(x + w - c->ox);
	j1 = (int)ceilf(y + h - c->oy);
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
	for (j = j0; j < j1; j++) {
		float sy = ((float)j + c->oy + 0.5f - y) * (float)im->h / h - 0.5f;
		int y0 = (int)floorf(sy), y1 = y0 + 1;
		float fy = sy - (float)y0;

		/* the edges' neighbours: a shrunk image's first row maps
		 * rows before its own, which do not exist */
		y0 = y0 < 0 ? 0 : y0 >= im->h ? im->h - 1 : y0;
		y1 = y1 < 0 ? 0 : y1 >= im->h ? im->h - 1 : y1;
		for (i = i0; i < i1; i++) {
			float sx = ((float)i + c->ox + 0.5f - x) * (float)im->w /
				w - 0.5f;
			int x0 = (int)floorf(sx), x1 = x0 + 1, k;
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
			blend_px(c, i, j, o, 1);
		}
	}
}

/* object-fit (CSS Images 3) inside the content box. */
/* An image in a replaced box, as object-fit says, clipped to it. */
void draw_fitted(struct canvas *c, const struct box *b,
	const struct image *im, const unsigned char *cur)
{
	const struct style *st = b->style;
	float x = b->x, y = b->y, w = b->w, h = b->h, iw, ih, s;
	int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
	int a, bb;

	/* natural size in CSS px, zoomed */
	iw = (float)im->w * doc_zoom();
	ih = (float)im->h * doc_zoom();
	if (kw(st, P_OBJECT_FIT, "contain") || kw(st, P_OBJECT_FIT, "cover") ||
	    (kw(st, P_OBJECT_FIT, "scale-down") && (iw > w || ih > h))) {
		s = w / iw < h / ih ? w / iw : h / ih;
		if (kw(st, P_OBJECT_FIT, "cover"))
			s = w / iw > h / ih ? w / iw : h / ih;
		iw *= s;
		ih *= s;
	} else if (!kw(st, P_OBJECT_FIT, "none") &&
		   !kw(st, P_OBJECT_FIT, "scale-down")) {
		iw = w;		/* fill */
		ih = h;
	}
	a = (int)floorf(x - c->ox);
	bb = (int)floorf(y - c->oy);
	if (a > c->cx0)
		c->cx0 = a;
	if (bb > c->cy0)
		c->cy0 = bb;
	if ((int)ceilf(x + w - c->ox) < c->cx1)
		c->cx1 = (int)ceilf(x + w - c->ox);
	if ((int)ceilf(y + h - c->oy) < c->cy1)
		c->cy1 = (int)ceilf(y + h - c->oy);
	draw_image(c, im, x + (w - iw) / 2, y + (h - ih) / 2, iw, ih, cur,
		st);
	c->cx0 = cx0;
	c->cy0 = cy0;
	c->cx1 = cx1;
	c->cy1 = cy1;
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
	if (isnan(t) || t <= s[0].pos) {
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

/* One tile of gradient g at (x, y), w x h, inside the canvas clip */
static void gradient_draw(struct canvas *c, const struct gradient *g,
	float x, float y, float w, float h)
{
	int i0 = (int)floorf(x - c->ox + 0.5f), i1 = (int)floorf(x + w -
		c->ox + 0.5f);
	int j0 = (int)floorf(y - c->oy + 0.5f), j1 = (int)floorf(y + h -
		c->oy + 0.5f);
	float half = g->len / 2;
	int i, j;

	if (i0 < c->cx0)
		i0 = c->cx0;
	if (j0 < c->cy0)
		j0 = c->cy0;
	if (i1 > c->cx1)
		i1 = c->cx1;
	if (j1 > c->cy1)
		j1 = c->cy1;
	for (j = j0; j < j1; j++) {
		float py = (float)j + c->oy + 0.5f - y;
		unsigned char col[4], last[4] = { 0, 0, 0, 0 };
		float tl = NAN;

		for (i = i0; i < i1; i++) {
			float px = (float)i + c->ox + 0.5f - x, t;

			if (g->kind == 'l') {
				t = (px - w / 2) * g->dx + (py - h / 2) * g->dy +
					half;
			} else if (g->kind == 'r') {
				float ex = px - g->cx, ey = py - g->cy;

				if (g->rx <= 0 || g->ry <= 0)
					t = 1e30f;
				else
					t = sqrtf(ex * ex / (g->rx * g->rx) +
						ey * ey / (g->ry * g->ry)) * g->rx;
			} else {
				t = atan2f(px - g->cx, g->cy - py) /
					(2 * (float)M_PI) - g->from;
				t -= floorf(t);
			}
			/* the same place on the line: the same color (the
			 * rows of a horizontal gradient) */
			if (t != tl) {
				stop_color(g, t, col);
				memcpy(last, col, 4);
				tl = t;
			}
			blend_px(c, i, j, last, 1);
		}
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

/* Along one axis: the first tile's position and how many, for an area
 * at a of size as, tiles of size *ts placed at off, painted from c0 to
 * c1. mode: 'r'epeat, 'n'o-repeat, 's'pace, rou'N'd (the size was
 * rounded by the caller). *gap is what space puts between tiles. */
static int tiles(int mode, float a, float as, float ts, float off, float c0,
	float c1, float *first, float *gap)
{
	float p = a + off, n;

	*gap = 0;
	*first = p;
	if (mode == 'n' || ts <= 0)
		return 1;
	if (mode == 's') {
		int k = (int)floorf(as / ts + 0.001f);

		if (k < 2)	/* one tile: where the position says */
			return 1;
		*gap = (as - (float)k * ts) / (float)(k - 1);
		p = a;
	}
	/* back to the first tile that reaches c0, without counting */
	n = ceilf((p - c0) / (ts + *gap));
	p -= n * (ts + *gap);
	*first = p;
	n = ceilf((c1 - p) / (ts + *gap));
	return n < 1 ? 1 : n > 100000 ? 100000 : (int)n;
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

	if (!img || !strcmp(img, "none"))
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
		float iw = 0, ih = 0, ox = 0, oy = 0, fx, fy, gx, gy;
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
			/* clip to its box */
			int a0 = (int)floorf(kx - c->ox + 0.5f);
			int b0 = (int)floorf(ky - c->oy + 0.5f);
			int a1 = (int)floorf(kx + kw_ - c->ox + 0.5f);
			int b1 = (int)floorf(ky + kh - c->oy + 0.5f);

			c->cx0 = a0 > cx0 ? a0 : cx0;
			c->cy0 = b0 > cy0 ? b0 : cy0;
			c->cx1 = a1 < cx1 ? a1 : cx1;
			c->cy1 = b1 < cy1 ? b1 : cy1;
			/* and what is painted of it, for the tiles to walk */
			kx = c->ox + (float)c->cx0;
			ky = c->oy + (float)c->cy0;
			kw_ = (float)(c->cx1 - c->cx0);
			kh = (float)(c->cy1 - c->cy0);
		}
		if (kw_ > 0 && kh > 0) {
			nx = tiles(mx, ax, aw, iw, ox, kx, kx + kw_, &fx, &gx);
			ny = tiles(my, ay, ah, ih, oy, ky, ky + kh, &fy, &gy);
			for (iy = 0; iy < ny; iy++)
				for (ix = 0; ix < nx; ix++) {
					float tx = fx + (float)ix * (iw + gx);
					float ty = fy + (float)iy * (ih + gy);

					if (grad)
						gradient_draw(c, &g, tx, ty, iw, ih);
					else
						draw_image(c, im, tx, ty, iw, ih,
							cur, st);
				}
		}
		if (grad)
			free(g.s);
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
