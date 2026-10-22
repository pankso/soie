/*
 * Images: PNG (libpng), JPEG (libjpeg), GIF (first frame) and SVG
 * decoding, the per-document cache, drawing with bilinear scaling,
 * object-fit, and CSS backgrounds: url() images, linear and radial
 * gradients, background-size, -position and -repeat.
 */
#include <png.h>		/* first: libpng 1.2 wants its own setjmp.h */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include "image.h"

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

static int decode_png(const unsigned char *d, size_t len, struct image *im)
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

static int decode_jpeg(const unsigned char *d, size_t len, struct image *im)
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
	idx = calloc((size_t)fw * (size_t)fh + 1, 1);
	if (!idx)
		abort();
	{
		/* LZW over the data sub-blocks */
		int minc = d[p++], clear = 1 << minc, eoi = clear + 1, next,
			csize, prev = -1, out = 0, total = fw * fh;
		static unsigned short pre[4096];
		static unsigned char suf[4096], stack[4097];
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
				int ci = idx[k * fw + x], X = x0 + x, Y = y0 + y;
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

		svg_size(n, &w, &h);
		im->w = (int)(w + 0.5f);
		im->h = (int)(h + 0.5f);
	}
	return 1;
}

int decode_image(const unsigned char *d, size_t len, struct image *im)
{
	size_t i;

	if (len > 8 && !memcmp(d, "\x89PNG", 4))
		return decode_png(d, len, im);
	if (len > 3 && d[0] == 0xff && d[1] == 0xd8)
		return decode_jpeg(d, len, im);
	if (len > 6 && !memcmp(d, "GIF8", 4))
		return decode_gif(d, len, im);
	for (i = 0; i < len && i < 512; i++)
		if (d[i] == '<')
			return decode_svg(d, len, im);
	return 0;	/* WebP, AVIF...: the image extension's job */
}

/* Cache */

void images_free(struct images *im)
{
	int i;

	for (i = 0; i < im->n; i++) {
		free(im->v[i].url);
		free(im->v[i].rgba);
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
	struct images *im = doc->u.doc.images;

	if (!im) {
		im = xmalloc(sizeof(*im));
		memset(im, 0, sizeof(*im));
		doc->u.doc.images = im;
	}
	im->fetch = fetch;
	im->data = data;
	free(im->base);
	im->base = base ? xstrndup(base, strlen(base)) : NULL;
}

/* An image by URL (resolved against the base): fetched once. */
const struct image *image_get(soie_node *doc, const char *url)
{
	struct images *im = doc->u.doc.images;
	struct image *x;
	char *abs, *buf = NULL;
	size_t len = 0;
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
	if (!im->fetch || im->fetch(im->data, abs, &buf, &len) || !buf ||
	    !decode_image((unsigned char *)buf, len, x))
		x->failed = 1;
	free(buf);
	return x->failed ? NULL : x;
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
	int al;

	if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1)
		return;
	al = (int)(s[3] * a * c->alpha + 0.5f);
	if (al <= 0)
		return;
	p = c->px + (size_t)y * (size_t)c->stride + (size_t)x * 4;
	if (al >= 255) {
		memcpy(p, s, 3);
		p[3] = 255;
		return;
	}
	p[0] = (unsigned char)((s[0] * al + p[0] * (255 - al)) / 255);
	p[1] = (unsigned char)((s[1] * al + p[1] * (255 - al)) / 255);
	p[2] = (unsigned char)((s[2] * al + p[2] * (255 - al)) / 255);
	p[3] = (unsigned char)(al + p[3] * (255 - al) / 255);
}

/* Image into the page rect (x, y, w, h), bilinear, clipped. */
void draw_image(struct canvas *c, const struct image *im, float x, float y,
	float w, float h, const unsigned char *current)
{
	int i0, j0, i1, j1, i, j;

	if (w <= 0 || h <= 0)
		return;
	if (im->svg) {
		svg_render(c, im->svg, x, y, w, h, current);
		return;
	}
	if (!im->rgba || !im->w || !im->h)
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
	for (j = j0; j < j1; j++) {
		float sy = ((float)j + c->oy + 0.5f - y) * (float)im->h / h - 0.5f;
		int y0 = (int)floorf(sy), y1 = y0 + 1;
		float fy = sy - (float)y0;

		if (y0 < 0)
			y0 = 0;
		if (y1 >= im->h)
			y1 = im->h - 1;
		if (y0 >= im->h)
			y0 = im->h - 1;
		for (i = i0; i < i1; i++) {
			float sx = ((float)i + c->ox + 0.5f - x) * (float)im->w /
				w - 0.5f;
			int x0 = (int)floorf(sx), x1 = x0 + 1, k;
			float fx = sx - (float)x0;
			unsigned char o[4];
			const unsigned char *a, *b, *cc, *d;

			if (x0 < 0)
				x0 = 0;
			if (x1 >= im->w)
				x1 = im->w - 1;
			if (x0 >= im->w)
				x0 = im->w - 1;
			if (fx < 0)
				fx = 0;
			if (fy < 0)
				fy = 0;
			a = im->rgba + ((size_t)y0 * (size_t)im->w + (size_t)x0) * 4;
			b = im->rgba + ((size_t)y0 * (size_t)im->w + (size_t)x1) * 4;
			cc = im->rgba + ((size_t)y1 * (size_t)im->w + (size_t)x0) * 4;
			d = im->rgba + ((size_t)y1 * (size_t)im->w + (size_t)x1) * 4;
			for (k = 0; k < 4; k++)
				o[k] = (unsigned char)((a[k] * (1 - fx) +
					b[k] * fx) * (1 - fy) + (cc[k] *
					(1 - fx) + d[k] * fx) * fy + 0.5f);
			blend_px(c, i, j, o, 1);
		}
	}
}

/* object-fit (CSS Images 3) inside the content box. */
void paint_replaced(struct canvas *c, const struct box *b)
{
	const struct style *st = b->style;
	const struct image *im = NULL;
	unsigned char cur[4];
	float x = b->x, y = b->y, w = b->w, h = b->h, iw, ih, s;

	if (!b->node)
		return;
	color_to_rgba(st->v[P_COLOR].u.color, cur);
	if (b->node->ns == SOIE_NS_SVG) {
		svg_render(c, b->node, x, y, w, h, cur);
		return;
	}
	im = image_for(b->node);
	if (!im)
		return;
	iw = (float)im->w;
	ih = (float)im->h;
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
	{
		int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
		int a = (int)floorf(x - c->ox), bb = (int)floorf(y - c->oy);

		if (a > c->cx0)
			c->cx0 = a;
		if (bb > c->cy0)
			c->cy0 = bb;
		if ((int)ceilf(x + w - c->ox) < c->cx1)
			c->cx1 = (int)ceilf(x + w - c->ox);
		if ((int)ceilf(y + h - c->oy) < c->cy1)
			c->cy1 = (int)ceilf(y + h - c->oy);
		draw_image(c, im, x + (w - iw) / 2, y + (h - ih) / 2, iw, ih,
			cur);
		c->cx0 = cx0;
		c->cy0 = cy0;
		c->cx1 = cx1;
		c->cy1 = cy1;
	}
}

/* Gradients */

struct stop {
	unsigned char rgba[4];
	float pos;		/* 0..1, -1: auto */
};

static int parse_stops(struct cur *c, struct stop *s, int max, float len)
{
	int n = 0, i, j;

	while (n < max && !cur_end(c)) {
		const struct cv *v;
		struct color col;

		if (!parse_color(c, &col))
			return 0;
		color_to_rgba(&col, s[n].rgba);
		s[n].pos = -1;
		v = cur_peek(c);
		if (v && (v->type == CT_PERCENT || v->type == CT_DIMENSION ||
			  v->type == CT_NUMBER)) {
			s[n].pos = v->type == CT_PERCENT ? (float)v->num / 100 :
				len > 0 ? (float)v->num / len : 0;
			cur_next(c);
			v = cur_peek(c);
			if (v && (v->type == CT_PERCENT ||
				  v->type == CT_DIMENSION)) {
				/* a second position: a hard stop */
				s[n + 1 < max ? n + 1 : n] = s[n];
				n++;
				s[n].pos = v->type == CT_PERCENT ?
					(float)v->num / 100 : len > 0 ?
					(float)v->num / len : 0;
				cur_next(c);
			}
		}
		n++;
		v = cur_next(c);
		if (v && v->type != CT_COMMA)
			return 0;
	}
	if (n < 1)
		return 0;
	if (s[0].pos < 0)
		s[0].pos = 0;
	if (s[n - 1].pos < 0)
		s[n - 1].pos = 1;
	for (i = 1; i < n; i++) {
		if (s[i].pos >= 0) {
			if (s[i].pos < s[i - 1].pos)
				s[i].pos = s[i - 1].pos;
			continue;
		}
		for (j = i; j < n && s[j].pos < 0; j++)
			;
		{
			float a = s[i - 1].pos, b = s[j].pos;
			int k;

			for (k = i; k < j; k++)
				s[k].pos = a + (b - a) * (float)(k - i + 1) /
					(float)(j - i + 1);
		}
	}
	return n;
}

static void stop_color(const struct stop *s, int n, float t,
	unsigned char *out)
{
	int i, k;

	if (t <= s[0].pos) {
		memcpy(out, s[0].rgba, 4);
		return;
	}
	for (i = 1; i < n; i++)
		if (t <= s[i].pos) {
			float d = s[i].pos - s[i - 1].pos;
			float f = d > 0 ? (t - s[i - 1].pos) / d : 1;

			for (k = 0; k < 4; k++)
				out[k] = (unsigned char)(s[i - 1].rgba[k] *
					(1 - f) + s[i].rgba[k] * f + 0.5f);
			return;
		}
	memcpy(out, s[n - 1].rgba, 4);
}

static void gradient(struct canvas *c, const struct cv *f, float x, float y,
	float w, float h)
{
	struct cur cu;
	struct stop s[32];
	int radial = strstr(f->str, "radial") != NULL, n, i, j;
	float angle = 180, len, cx = x + w / 2, cy = y + h / 2, rx, ry;
	const struct cv *v;

	cur_init(&cu, &f->kids);
	v = cur_peek(&cu);
	if (!radial && v && v->type == CT_DIMENSION) {
		angle = (float)v->num;
		if (ascii_ieq(v->str, "rad"))
			angle = angle * 180 / (float)M_PI;
		else if (ascii_ieq(v->str, "turn"))
			angle *= 360;
		else if (ascii_ieq(v->str, "grad"))
			angle *= 0.9f;
		cur_next(&cu);
		cur_next(&cu);	/* comma */
	} else if (!radial && cv_ident(v, "to")) {
		int top = 0, bottom = 0, left = 0, right = 0;

		cur_next(&cu);
		while ((v = cur_peek(&cu)) && v->type == CT_IDENT) {
			top |= cv_ident(v, "top");
			bottom |= cv_ident(v, "bottom");
			left |= cv_ident(v, "left");
			right |= cv_ident(v, "right");
			cur_next(&cu);
		}
		cur_next(&cu);
		angle = top ? 0 : bottom ? 180 : 90;
		if (left && !top && !bottom)
			angle = 270;
		if ((top || bottom) && (left || right)) {
			float a = atanf(w / h) * 180 / (float)M_PI;

			angle = top ? (right ? a : 360 - a) :
				(right ? 180 - a : 180 + a);
		}
	} else if (radial) {
		/* shape and position are skipped: circle at the center */
		int k = cu.i, comma = -1;

		for (; k < cu.n; k++)
			if (cu.v[k].type == CT_COMMA) {
				comma = k;
				break;
			}
		if (comma >= 0) {
			int has_color = 0;
			struct cur t = cu;
			struct color col;

			has_color = parse_color(&t, &col);
			if (!has_color)
				cu.i = comma + 1;
		}
	}
	{
		float r = angle * (float)M_PI / 180;

		len = fabsf(w * sinf(r)) + fabsf(h * cosf(r));
		rx = sqrtf(w * w + h * h) / 2;
		ry = rx;
		n = parse_stops(&cu, s, 32, radial ? rx : len);
		if (n <= 0)
			return;
		for (j = (int)floorf(y - c->oy); j < (int)ceilf(y + h - c->oy);
		     j++) {
			if (j < c->cy0 || j >= c->cy1)
				continue;
			for (i = (int)floorf(x - c->ox);
			     i < (int)ceilf(x + w - c->ox); i++) {
				float px = (float)i + c->ox + 0.5f - cx;
				float py = (float)j + c->oy + 0.5f - cy, t;
				unsigned char col[4];

				if (i < c->cx0 || i >= c->cx1)
					continue;
				if (radial)
					t = sqrtf(px * px / (rx * rx) +
						py * py / (ry * ry));
				else
					t = (px * sinf(r) - py * cosf(r)) / len +
						0.5f;
				stop_color(s, n, t, col);
				blend_px(c, i, j, col, 1);
			}
		}
	}
}

/* background-position/size from their text, for one layer. */
static float bg_len(const struct cv *v, float ref, float dflt)
{
	if (!v)
		return dflt;
	if (v->type == CT_PERCENT)
		return (float)v->num * ref / 100;
	if (v->type == CT_DIMENSION || v->type == CT_NUMBER)
		return (float)v->num;	/* px; em and others approximated */
	return dflt;
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

void paint_bg_image(struct canvas *c, const struct box *b, float x, float y,
	float w, float h)
{
	const struct style *st = b->style;
	const char *img = st->v[P_BACKGROUND_IMAGE].u.str;
	struct cvlist li, lp, ls, lr;
	unsigned char cur[4];
	int layer, nlayers = 0, i;
	int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;

	if (!img || !strcmp(img, "none"))
		return;
	css_component_values(img, strlen(img), &li);
	css_component_values(st->v[P_BACKGROUND_POSITION].u.str,
		strlen(st->v[P_BACKGROUND_POSITION].u.str), &lp);
	css_component_values(st->v[P_BACKGROUND_SIZE].u.str,
		strlen(st->v[P_BACKGROUND_SIZE].u.str), &ls);
	css_component_values(st->v[P_BACKGROUND_REPEAT].u.str,
		strlen(st->v[P_BACKGROUND_REPEAT].u.str), &lr);
	color_to_rgba(st->v[P_COLOR].u.color, cur);
	for (i = 0; i < li.n; i++)
		if (li.v[i].type == CT_COMMA)
			nlayers++;
	nlayers++;
	/* clip to the border box */
	{
		int a = (int)floorf(x - c->ox), bb = (int)floorf(y - c->oy);

		if (a > c->cx0)
			c->cx0 = a;
		if (bb > c->cy0)
			c->cy0 = bb;
		if ((int)ceilf(x + w - c->ox) < c->cx1)
			c->cx1 = (int)ceilf(x + w - c->ox);
		if ((int)ceilf(y + h - c->oy) < c->cy1)
			c->cy1 = (int)ceilf(y + h - c->oy);
	}
	/* the first layer is on top: paint from the last */
	for (layer = nlayers - 1; layer >= 0; layer--) {
		struct cur ci, cp, cs, cr;
		const struct cv *v;
		const struct image *im = NULL;
		float iw, ih, px, py, ox, oy, sx, sy;
		int rx = 1, ry = 1;

		if (!nth_layer(&li, layer, &ci) || !(v = cur_peek(&ci)))
			continue;
		if (v->type == CT_FUNCTION && strstr(v->str, "gradient")) {
			gradient(c, v, x, y, w, h);
			continue;
		}
		if (v->type == CT_URL)
			im = image_get(paint_doc, v->str);
		else if (cv_func(v, "url") && v->kids.n &&
			 v->kids.v[0].type == CT_STRING)
			im = image_get(paint_doc, v->kids.v[0].str);
		if (!im || !im->w || !im->h)
			continue;
		iw = (float)im->w;
		ih = (float)im->h;
		/* background-size */
		if (nth_layer(&ls, layer < 0 ? 0 : layer, &cs) ||
		    nth_layer(&ls, 0, &cs)) {
			const struct cv *a = cur_next(&cs), *bb = cur_next(&cs);

			if (cv_ident(a, "cover") || cv_ident(a, "contain")) {
				float s = w / iw, t = h / ih;

				s = cv_ident(a, "cover") ? (s > t ? s : t) :
					(s < t ? s : t);
				iw *= s;
				ih *= s;
			} else if (a && !cv_ident(a, "auto")) {
				float nw = bg_len(a, w, iw);
				float nh = bb && !cv_ident(bb, "auto") ?
					bg_len(bb, h, ih) : nw * ih / iw;

				iw = nw;
				ih = nh;
			} else if (bb && !cv_ident(bb, "auto")) {
				float nh = bg_len(bb, h, ih);

				iw = iw * nh / ih;
				ih = nh;
			}
		}
		/* background-position: keywords, lengths, percentages */
		ox = oy = 0;
		if (nth_layer(&lp, layer, &cp) || nth_layer(&lp, 0, &cp)) {
			int axis = 0;

			while ((v = cur_next(&cp)) && axis < 2) {
				float *o = axis ? &oy : &ox;
				float room = axis ? h - ih : w - iw;

				if (cv_ident(v, "left") || cv_ident(v, "top")) {
					if (cv_ident(v, "top"))
						oy = 0, axis = 1;
					else
						ox = 0;
				} else if (cv_ident(v, "right")) {
					ox = w - iw;
				} else if (cv_ident(v, "bottom")) {
					oy = h - ih;
					axis = 1;
				} else if (cv_ident(v, "center")) {
					*o = room / 2;
				} else {
					*o = v->type == CT_PERCENT ?
						(float)v->num * room / 100 :
						bg_len(v, room, 0);
				}
				axis++;
			}
		}
		if (nth_layer(&lr, layer, &cr) || nth_layer(&lr, 0, &cr)) {
			v = cur_next(&cr);
			if (cv_ident(v, "no-repeat"))
				rx = ry = 0;
			else if (cv_ident(v, "repeat-x"))
				ry = 0;
			else if (cv_ident(v, "repeat-y"))
				rx = 0;
			v = cur_next(&cr);
			if (cv_ident(v, "no-repeat"))
				ry = 0;
		}
		if (iw < 1 || ih < 1)
			continue;
		px = x + ox;
		py = y + oy;
		if (rx)
			while (px > x)
				px -= iw;
		if (ry)
			while (py > y)
				py -= ih;
		for (sy = py; sy < y + h && (ry || sy == py); sy += ih)
			for (sx = px; sx < x + w && (rx || sx == px); sx += iw)
				draw_image(c, im, sx, sy, iw, ih, cur);
	}
	c->cx0 = cx0;
	c->cy0 = cy0;
	c->cx1 = cx1;
	c->cy1 = cy1;
	cvlist_free(&li);
	cvlist_free(&lp);
	cvlist_free(&ls);
	cvlist_free(&lr);
}
