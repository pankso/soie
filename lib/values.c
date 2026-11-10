/*
 * CSS values: numbers, colors (CSS Color 4), lengths and calc(), An+B.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "css.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif

/* Numbers */

/* Shortest form with at most 6 decimals: "0.533333", "112.2", "0". */
void serialize_number(struct buf *b, double v)
{
	char s[64];
	int n;

	if (v != v)
		v = 0;
	if (v > 1e15 || v < -1e15) {
		n = snprintf(s, sizeof(s), "%.6g", v);
	} else {
		/* the bias picks the upper value of decimal ties */
		n = snprintf(s, sizeof(s), "%.6f", v + 1e-7);
		while (n > 0 && s[n - 1] == '0')
			n--;
		if (n > 0 && s[n - 1] == '.')
			n--;
		if (n == 2 && s[0] == '-' && s[1] == '0')
			s[0] = '0', n = 1;
	}
	buf_add(b, s, (size_t)n);
}

/* Colors */

struct named {
	const char *name;
	unsigned rgb;
};

static const struct named named_colors[] = {
	{ "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 },
	{ "aqua", 0x00ffff }, { "aquamarine", 0x7fffd4 },
	{ "azure", 0xf0ffff }, { "beige", 0xf5f5dc },
	{ "bisque", 0xffe4c4 }, { "black", 0x000000 },
	{ "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff },
	{ "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a },
	{ "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 },
	{ "chartreuse", 0x7fff00 }, { "chocolate", 0xd2691e },
	{ "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed },
	{ "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c },
	{ "cyan", 0x00ffff }, { "darkblue", 0x00008b },
	{ "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b },
	{ "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 },
	{ "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b },
	{ "darkmagenta", 0x8b008b }, { "darkolivegreen", 0x556b2f },
	{ "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc },
	{ "darkred", 0x8b0000 }, { "darksalmon", 0xe9967a },
	{ "darkseagreen", 0x8fbc8f }, { "darkslateblue", 0x483d8b },
	{ "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f },
	{ "darkturquoise", 0x00ced1 }, { "darkviolet", 0x9400d3 },
	{ "deeppink", 0xff1493 }, { "deepskyblue", 0x00bfff },
	{ "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
	{ "dodgerblue", 0x1e90ff }, { "firebrick", 0xb22222 },
	{ "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 },
	{ "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc },
	{ "ghostwhite", 0xf8f8ff }, { "gold", 0xffd700 },
	{ "goldenrod", 0xdaa520 }, { "gray", 0x808080 },
	{ "green", 0x008000 }, { "greenyellow", 0xadff2f },
	{ "grey", 0x808080 }, { "honeydew", 0xf0fff0 },
	{ "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c },
	{ "indigo", 0x4b0082 }, { "ivory", 0xfffff0 },
	{ "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa },
	{ "lavenderblush", 0xfff0f5 }, { "lawngreen", 0x7cfc00 },
	{ "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 },
	{ "lightcoral", 0xf08080 }, { "lightcyan", 0xe0ffff },
	{ "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 },
	{ "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 },
	{ "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a },
	{ "lightseagreen", 0x20b2aa }, { "lightskyblue", 0x87cefa },
	{ "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 },
	{ "lightsteelblue", 0xb0c4de }, { "lightyellow", 0xffffe0 },
	{ "lime", 0x00ff00 }, { "limegreen", 0x32cd32 },
	{ "linen", 0xfaf0e6 }, { "magenta", 0xff00ff },
	{ "maroon", 0x800000 }, { "mediumaquamarine", 0x66cdaa },
	{ "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 },
	{ "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 },
	{ "mediumslateblue", 0x7b68ee }, { "mediumspringgreen", 0x00fa9a },
	{ "mediumturquoise", 0x48d1cc }, { "mediumvioletred", 0xc71585 },
	{ "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa },
	{ "mistyrose", 0xffe4e1 }, { "moccasin", 0xffe4b5 },
	{ "navajowhite", 0xffdead }, { "navy", 0x000080 },
	{ "oldlace", 0xfdf5e6 }, { "olive", 0x808000 },
	{ "olivedrab", 0x6b8e23 }, { "orange", 0xffa500 },
	{ "orangered", 0xff4500 }, { "orchid", 0xda70d6 },
	{ "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 },
	{ "paleturquoise", 0xafeeee }, { "palevioletred", 0xdb7093 },
	{ "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 },
	{ "peru", 0xcd853f }, { "pink", 0xffc0cb },
	{ "plum", 0xdda0dd }, { "powderblue", 0xb0e0e6 },
	{ "purple", 0x800080 }, { "rebeccapurple", 0x663399 },
	{ "red", 0xff0000 }, { "rosybrown", 0xbc8f8f },
	{ "royalblue", 0x4169e1 }, { "saddlebrown", 0x8b4513 },
	{ "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 },
	{ "seagreen", 0x2e8b57 }, { "seashell", 0xfff5ee },
	{ "sienna", 0xa0522d }, { "silver", 0xc0c0c0 },
	{ "skyblue", 0x87ceeb }, { "slateblue", 0x6a5acd },
	{ "slategray", 0x708090 }, { "slategrey", 0x708090 },
	{ "snow", 0xfffafa }, { "springgreen", 0x00ff7f },
	{ "steelblue", 0x4682b4 }, { "tan", 0xd2b48c },
	{ "teal", 0x008080 }, { "thistle", 0xd8bfd8 },
	{ "tomato", 0xff6347 }, { "turquoise", 0x40e0d0 },
	{ "violet", 0xee82ee }, { "wheat", 0xf5deb3 },
	{ "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 },
	{ "yellow", 0xffff00 }, { "yellowgreen", 0x9acd32 },
};

/* System colors, light scheme, close to what browsers use. */
static const struct named system_colors[] = {
	{ "accentcolor", 0x0075ff }, { "accentcolortext", 0xffffff },
	{ "activetext", 0xff0000 }, { "buttonborder", 0x767676 },
	{ "buttonface", 0xefefef }, { "buttontext", 0x000000 },
	{ "canvas", 0xffffff }, { "canvastext", 0x000000 },
	{ "field", 0xffffff }, { "fieldtext", 0x000000 },
	{ "graytext", 0x808080 }, { "highlight", 0x3399ff },
	{ "highlighttext", 0xffffff }, { "linktext", 0x0000ee },
	{ "mark", 0xffff00 }, { "marktext", 0x000000 },
	{ "selecteditem", 0x3399ff }, { "selecteditemtext", 0xffffff },
	{ "visitedtext", 0x551a8b },
};

static void set_rgb(struct color *c, unsigned rgb, float alpha)
{
	memset(c, 0, sizeof(*c));
	c->space = CS_RGB_LEGACY;
	c->c[0] = rgb >> 16 & 0xff;
	c->c[1] = rgb >> 8 & 0xff;
	c->c[2] = rgb & 0xff;
	c->alpha = alpha;
}

static int find_named(const struct named *t, int n, const char *name)
{
	int lo = 0, hi = n - 1;
	char low[32];
	size_t i;

	for (i = 0; name[i] && i < sizeof(low) - 1; i++)
		low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ?
			name[i] + 32 : name[i]);
	if (name[i])
		return -1;
	low[i] = '\0';
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(low, t[mid].name);

		if (!c)
			return mid;
		if (c < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return -1;
}

#define COUNT(a) ((int)(sizeof(a) / sizeof(*(a))))

int named_color(const char *name, struct color *out)
{
	int i = find_named(named_colors, COUNT(named_colors), name);

	if (i >= 0) {
		set_rgb(out, named_colors[i].rgb, 1);
		return 1;
	}
	i = find_named(system_colors, COUNT(system_colors), name);
	if (i >= 0) {
		set_rgb(out, system_colors[i].rgb, 1);
		return 1;
	}
	if (ascii_ieq(name, "transparent")) {
		set_rgb(out, 0, 0);
		return 1;
	}
	if (ascii_ieq(name, "currentcolor")) {
		set_rgb(out, 0, 1);
		out->current = 1;
		return 1;
	}
	return 0;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 32;
	return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int parse_hex(const char *s, struct color *out)
{
	size_t n = strlen(s), i;
	int d[8];

	if (n != 3 && n != 4 && n != 6 && n != 8)
		return 0;
	for (i = 0; i < n; i++)
		if ((d[i] = hexval((unsigned char)s[i])) < 0)
			return 0;
	memset(out, 0, sizeof(*out));
	out->space = CS_RGB_LEGACY;
	out->alpha = 1;
	if (n <= 4) {
		for (i = 0; i < 3; i++)
			out->c[i] = d[i] * 17;
		if (n == 4)
			out->alpha = d[3] * 17 / 255.0;
	} else {
		for (i = 0; i < 3; i++)
			out->c[i] = d[2 * i] * 16 + d[2 * i + 1];
		if (n == 8)
			out->alpha = (d[6] * 16 + d[7]) / 255.0;
	}
	return 1;
}

/* One argument of a color function. */
enum { A_NUMBER, A_PERCENT, A_ANGLE, A_NONE };

struct carg {
	int type;
	double v;
};

static int angle_deg(const struct cv *v, double *out)
{
	const char *u = v->str;

	if (ascii_ieq(u, "deg"))
		*out = v->num;
	else if (ascii_ieq(u, "grad"))
		*out = v->num * 0.9;
	else if (ascii_ieq(u, "rad"))
		*out = v->num * 180 / M_PI;
	else if (ascii_ieq(u, "turn"))
		*out = v->num * 360;
	else
		return 0;
	return 1;
}

/* Split arguments: up to 4 values, commas (legacy) or spaces + '/'. */
static int color_args(const struct cvlist *l, struct carg *a, int *n,
	int *legacy, int *has_alpha)
{
	struct cur c;
	const struct cv *v;
	int commas = 0, slash = 0, i = 0;

	*n = 0;
	*legacy = 0;
	*has_alpha = 0;
	cur_init(&c, l);
	while ((v = cur_next(&c))) {
		if (i == 4)
			return 0;
		if (v->type == CT_NUMBER) {
			a[i].type = A_NUMBER;
			a[i].v = v->num;
		} else if (v->type == CT_PERCENT) {
			a[i].type = A_PERCENT;
			a[i].v = v->num;
		} else if (v->type == CT_DIMENSION) {
			a[i].type = A_ANGLE;
			if (!angle_deg(v, &a[i].v))
				return 0;
		} else if (cv_ident(v, "none")) {
			a[i].type = A_NONE;
			a[i].v = 0;
		} else {
			return 0;
		}
		i++;
		v = cur_peek(&c);
		if (!v)
			break;
		if (v->type == CT_COMMA) {
			if (slash || (i > 1 && commas != i - 1))
				return 0;
			commas++;
			cur_next(&c);
		} else if (v->type == CT_DELIM && v->delim == '/') {
			if (commas || slash || i != 3)
				return 0;
			slash = 1;
			cur_next(&c);
		} else if (commas) {
			return 0;
		}
	}
	if (commas) {
		if (commas != i - 1 || (i != 3 && i != 4))
			return 0;
		*legacy = 1;
		*has_alpha = i == 4;
	} else {
		if (slash ? i != 4 : i != 3)
			return 0;
		*has_alpha = slash;
	}
	*n = i;
	return 1;
}

static double clampf(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int set_alpha(struct color *col, const struct carg *a, int has)
{
	if (!has) {
		col->alpha = 1;
		return 1;
	}
	if (a->type == A_ANGLE)
		return 0;
	if (a->type == A_NONE) {
		col->none |= 8;
		col->alpha = 0;
		return 1;
	}
	col->alpha = clampf(a->type == A_PERCENT ? a->v / 100 : a->v, 0, 1);
	return 1;
}

static double hue(const struct carg *a)
{
	double h = fmod(a->v, 360);

	return h < 0 ? h + 360 : h;
}

static void hsl_to_rgb(double h, double s, double l, double rgb[3])
{
	static const int n[3] = { 0, 8, 4 };
	int i;

	s /= 100;
	l /= 100;
	for (i = 0; i < 3; i++) {
		double k = fmod(n[i] + h / 30, 12);
		double a = s * (l < 1 - l ? l : 1 - l);
		double m = k - 3 < 9 - k ? k - 3 : 9 - k;

		if (m > 1)
			m = 1;
		if (m < -1)
			m = -1;
		rgb[i] = (l - a * m) * 255;
	}
}

static void hwb_to_rgb(double h, double w, double b, double rgb[3])
{
	int i;

	w /= 100;
	b /= 100;
	if (w + b >= 1) {
		double g = w / (w + b) * 255;

		rgb[0] = rgb[1] = rgb[2] = g;
		return;
	}
	hsl_to_rgb(h, 100, 50, rgb);
	for (i = 0; i < 3; i++)
		rgb[i] = rgb[i] * (1 - w - b) + w * 255;
}

static int rgb_func(const struct cvlist *l, struct color *col)
{
	struct carg a[4];
	int n, legacy, has_alpha, i, pct = -1;

	if (!color_args(l, a, &n, &legacy, &has_alpha))
		return 0;
	memset(col, 0, sizeof(*col));
	col->space = CS_RGB_LEGACY;
	for (i = 0; i < 3; i++) {
		if (a[i].type == A_ANGLE || (legacy && a[i].type == A_NONE))
			return 0;
		if (legacy && a[i].type != A_NONE) {
			if (pct < 0)
				pct = a[i].type == A_PERCENT;
			else if (pct != (a[i].type == A_PERCENT))
				return 0;
		}
		col->c[i] = a[i].type == A_PERCENT ?
			clampf(a[i].v * 2.55, 0, 255) : clampf(a[i].v, 0, 255);
	}
	if (legacy && has_alpha && a[3].type == A_NONE)
		return 0;
	return set_alpha(col, &a[3], has_alpha);
}

static int hsl_func(const struct cvlist *l, struct color *col, int hwb)
{
	struct carg a[4];
	int n, legacy, has_alpha, i;
	double h, x[2];

	if (!color_args(l, a, &n, &legacy, &has_alpha))
		return 0;
	if (hwb && legacy)
		return 0;
	if (a[0].type == A_PERCENT)
		return 0;
	for (i = 1; i < 3; i++) {
		if (a[i].type == A_ANGLE)
			return 0;
		if (legacy && a[i].type != A_PERCENT)
			return 0;
		x[i - 1] = a[i].type == A_NONE ? 0 : a[i].v;
	}
	if (legacy && (a[0].type == A_NONE ||
		       (has_alpha && a[3].type == A_NONE)))
		return 0;
	memset(col, 0, sizeof(*col));
	col->space = CS_RGB_LEGACY;
	h = a[0].type == A_NONE ? 0 : hue(&a[0]);
	if (hwb) {
		hwb_to_rgb(h, clampf(x[0], 0, 100), clampf(x[1], 0, 100),
			col->c);
	} else {
		hsl_to_rgb(h, clampf(x[0], 0, 100), clampf(x[1], 0, 100),
			col->c);
	}
	for (i = 0; i < 3; i++)
		col->c[i] = clampf(col->c[i], 0, 255);
	return set_alpha(col, &a[3], has_alpha);
}

/* lab(), lch(), oklab(), oklch(): percentage references per space. */
static int lab_func(const struct cvlist *l, struct color *col, int space)
{
	struct carg a[4];
	int n, legacy, has_alpha, i, polar = space == CS_LCH ||
		space == CS_OKLCH, ok = space == CS_OKLAB || space == CS_OKLCH;
	double ref[3];

	if (!color_args(l, a, &n, &legacy, &has_alpha) || legacy)
		return 0;
	ref[0] = ok ? 1 : 100;
	ref[1] = polar ? (ok ? 0.4 : 150) : (ok ? 0.4 : 125);
	ref[2] = ref[1];
	memset(col, 0, sizeof(*col));
	col->space = (unsigned char)space;
	for (i = 0; i < 3; i++) {
		if (a[i].type == A_NONE) {
			col->none |= (unsigned char)(1 << i);
			continue;
		}
		if (polar && i == 2) {
			if (a[i].type == A_PERCENT)
				return 0;
			col->c[i] = hue(&a[i]);
			continue;
		}
		if (a[i].type == A_ANGLE)
			return 0;
		col->c[i] = a[i].type == A_PERCENT ?
			a[i].v / 100 * ref[i] : a[i].v;
	}
	return set_alpha(col, &a[3], has_alpha);
}

static const char *const color_spaces[] = {
	"srgb", "srgb-linear", "display-p3", "a98-rgb", "prophoto-rgb",
	"rec2020", "xyz-d50", "xyz-d65", NULL
};

static int color_func(const struct cvlist *l, struct color *col)
{
	struct cur c;
	const struct cv *v;
	struct cvlist rest;
	struct carg a[4];
	int n, legacy, has_alpha, i, space = -1;

	cur_init(&c, l);
	v = cur_next(&c);
	if (!v || v->type != CT_IDENT)
		return 0;
	for (i = 0; color_spaces[i]; i++)
		if (ascii_ieq(v->str, color_spaces[i]))
			space = CS_SRGB + i;
	if (ascii_ieq(v->str, "xyz"))
		space = CS_XYZ_D65;
	if (space < 0)
		return 0;
	rest.v = (struct cv *)c.v + c.i;
	rest.n = c.n - c.i;
	rest.cap = 0;
	if (!color_args(&rest, a, &n, &legacy, &has_alpha) || legacy)
		return 0;
	memset(col, 0, sizeof(*col));
	col->space = (unsigned char)space;
	for (i = 0; i < 3; i++) {
		if (a[i].type == A_ANGLE)
			return 0;
		if (a[i].type == A_NONE)
			col->none |= (unsigned char)(1 << i);
		else
			col->c[i] = a[i].type == A_PERCENT ?
				a[i].v / 100 : a[i].v;
	}
	return set_alpha(col, &a[3], has_alpha);
}

int parse_color(struct cur *c, struct color *out)
{
	const struct cv *v = cur_peek(c);
	int ok = 0;

	if (!v)
		return 0;
	if (v->type == CT_IDENT)
		ok = named_color(v->str, out);
	else if (v->type == CT_HASH)
		ok = parse_hex(v->str, out);
	else if (cv_func(v, "rgb") || cv_func(v, "rgba"))
		ok = rgb_func(&v->kids, out);
	else if (cv_func(v, "hsl") || cv_func(v, "hsla"))
		ok = hsl_func(&v->kids, out, 0);
	else if (cv_func(v, "hwb"))
		ok = hsl_func(&v->kids, out, 1);
	else if (cv_func(v, "lab"))
		ok = lab_func(&v->kids, out, CS_LAB);
	else if (cv_func(v, "lch"))
		ok = lab_func(&v->kids, out, CS_LCH);
	else if (cv_func(v, "oklab"))
		ok = lab_func(&v->kids, out, CS_OKLAB);
	else if (cv_func(v, "oklch"))
		ok = lab_func(&v->kids, out, CS_OKLCH);
	else if (cv_func(v, "color"))
		ok = color_func(&v->kids, out);
	else if (cv_func(v, "light-dark")) {
		/* CSS Color 5: the one of the element's color scheme */
		struct cur in;
		struct color dark;

		cur_init(&in, &v->kids);
		ok = parse_color(&in, out) && cur_next(&in) &&
			parse_color(&in, &dark) && cur_end(&in);
		if (ok && color_dark)
			*out = dark;
	}
	if (ok)
		cur_next(c);
	return ok;
}

static void ser_comp(struct buf *b, const struct color *col, int i, double v)
{
	if (col->none & (1 << i))
		buf_add(b, "none", 4);
	else
		serialize_number(b, v);
}

void serialize_color(struct buf *b, const struct color *col)
{
	static const char *const fn[] = { "lab(", "lch(", "oklab(", "oklch(" };
	int i;

	if (col->current) {
		buf_add(b, "currentcolor", 12);
		return;
	}
	if (col->space == CS_RGB_LEGACY) {
		buf_add(b, col->alpha < 1 ? "rgba(" : "rgb(",
			col->alpha < 1 ? 5 : 4);
		for (i = 0; i < 3; i++) {
			if (i)
				buf_add(b, ", ", 2);
			serialize_number(b, col->c[i]);
		}
		if (col->alpha < 1) {
			buf_add(b, ", ", 2);
			serialize_number(b, col->alpha);
		}
		buf_putc(b, ')');
		return;
	}
	if (col->space >= CS_LAB && col->space <= CS_OKLCH) {
		const char *f = fn[col->space - CS_LAB];

		buf_add(b, f, strlen(f));
	} else {
		const char *s = color_spaces[col->space - CS_SRGB];

		buf_add(b, "color(", 6);
		buf_add(b, s, strlen(s));
		buf_putc(b, ' ');
	}
	for (i = 0; i < 3; i++) {
		if (i)
			buf_putc(b, ' ');
		ser_comp(b, col, i, col->c[i]);
	}
	if (col->alpha < 1 || (col->none & 8)) {
		buf_add(b, " / ", 3);
		ser_comp(b, col, 3, col->alpha);
	}
	buf_putc(b, ')');
}

/* Conversion to 8-bit sRGB, for painting. */

static double srgb_gamma(double x)
{
	double s = x < 0 ? -1 : 1;

	x *= s;
	return s * (x > 0.0031308 ? 1.055 * pow(x, 1 / 2.4) - 0.055 :
		12.92 * x);
}

static void mat3(const double m[9], const double in[3], double out[3])
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = m[3 * i] * in[0] + m[3 * i + 1] * in[1] +
			m[3 * i + 2] * in[2];
}

static const double xyz65_to_lrgb[9] = {
	3.2409699419045226, -1.537383177570094, -0.4986107602930034,
	-0.9692436362808796, 1.8759675015077202, 0.04155505740717559,
	0.05563007969699366, -0.20397695888897652, 1.0569715142428786
};

static const double d50_to_d65[9] = {
	0.955473421488075, -0.02309845494876471, 0.06325924320057072,
	-0.0283697093338637, 1.0099953980813041, 0.021041441191917323,
	0.012314014864481998, -0.020507649298898964, 1.330365926242124
};

void color_to_rgba(const struct color *col, unsigned char rgba[4])
{
	double c[3], lin[3], xyz[3], t;
	int i;

	for (i = 0; i < 3; i++)
		c[i] = col->none & (1 << i) ? 0 : col->c[i];
	switch (col->space) {
	case CS_RGB_LEGACY:
		for (i = 0; i < 3; i++)
			lin[i] = c[i] / 255;
		goto out_srgb;
	case CS_SRGB:
		lin[0] = c[0], lin[1] = c[1], lin[2] = c[2];
		goto out_srgb;
	case CS_LCH:
	case CS_OKLCH:
		t = c[2] * M_PI / 180;
		c[2] = c[1] * sin(t);
		c[1] = c[1] * cos(t);
		break;
	}
	switch (col->space) {
	case CS_LAB:
	case CS_LCH: {
		double fy = (c[0] + 16) / 116, fx = c[1] / 500 + fy,
			fz = fy - c[2] / 200, e = 216.0 / 24389, k = 24389.0 / 27;
		double d50[3];

		d50[0] = (fx * fx * fx > e ? fx * fx * fx : (116 * fx - 16) / k)
			* 0.3457 / 0.3585;
		d50[1] = c[0] > k * e ? fy * fy * fy : c[0] / k;
		d50[2] = (fz * fz * fz > e ? fz * fz * fz : (116 * fz - 16) / k)
			* (1 - 0.3457 - 0.3585) / 0.3585;
		mat3(d50_to_d65, d50, xyz);
		break;
	}
	case CS_OKLAB:
	case CS_OKLCH: {
		static const double lms_to_xyz[9] = {
			1.2268798758459243, -0.5578149944602171,
			0.2813910456659647, -0.0405757452148008,
			1.1122868032803170, -0.0717110580655164,
			-0.0763729366746601, -0.4214933324022432,
			1.5869240198367816
		};
		double lms[3];

		lms[0] = c[0] + 0.3963377773761749 * c[1] +
			0.2158037573099136 * c[2];
		lms[1] = c[0] - 0.1055613458156586 * c[1] -
			0.0638541728258133 * c[2];
		lms[2] = c[0] - 0.0894841775298119 * c[1] -
			1.2914855480194092 * c[2];
		for (i = 0; i < 3; i++)
			lms[i] = lms[i] * lms[i] * lms[i];
		mat3(lms_to_xyz, lms, xyz);
		break;
	}
	case CS_XYZ_D50:
		mat3(d50_to_d65, c, xyz);
		break;
	case CS_XYZ_D65:
		xyz[0] = c[0], xyz[1] = c[1], xyz[2] = c[2];
		break;
	case CS_SRGB_LINEAR:
		for (i = 0; i < 3; i++)
			lin[i] = srgb_gamma(c[i]);
		goto out_srgb;
	default:
		/* display-p3, a98-rgb, prophoto, rec2020: close enough to
		 * sRGB for an old screen. */
		lin[0] = c[0], lin[1] = c[1], lin[2] = c[2];
		goto out_srgb;
	}
	mat3(xyz65_to_lrgb, xyz, lin);
	for (i = 0; i < 3; i++)
		lin[i] = srgb_gamma(lin[i]);
out_srgb:
	for (i = 0; i < 3; i++)
		rgba[i] = (unsigned char)(clampf(lin[i], 0, 1) * 255 + 0.5);
	rgba[3] = (unsigned char)(clampf(col->alpha, 0, 1) * 255 + 0.5);
}

/* Lengths and calc() */

enum {
	U_PX, U_EM, U_REM, U_EX, U_CH, U_VW, U_VH, U_VMIN, U_VMAX, U_PCT,
	NUNITS
};

/* A calc() value: linear combination of units, or a number. */
struct lin {
	double k[NUNITS];
	double num;
	int is_num;		/* a plain number, no unit */
};

/* min(), max(), clamp() that cannot be folded at parse time. */
enum { K_LEAF, K_MIN, K_MAX, K_CLAMP };

struct calc {
	int op;
	struct lin leaf;	/* K_LEAF, specified units */
	float px, pct;		/* K_LEAF, computed */
	struct calc **args;
	int nargs;
};

struct slen {
	struct lin v;
	struct calc *tree;	/* NULL when v says it all */
	int keyword;		/* unused here: keywords live in props */
};

static const struct {
	const char *name;
	int unit;
	double scale;
} units[] = {
	{ "px", U_PX, 1 }, { "cm", U_PX, 96 / 2.54 },
	{ "mm", U_PX, 96 / 25.4 }, { "q", U_PX, 96 / 101.6 },
	{ "in", U_PX, 96 }, { "pt", U_PX, 96.0 / 72 },
	{ "pc", U_PX, 16 }, { "em", U_EM, 1 }, { "rem", U_REM, 1 },
	{ "ex", U_EX, 1 }, { "ch", U_CH, 1 }, { "cap", U_EM, 0.7 },
	{ "ic", U_EM, 1 }, { "lh", U_EM, 1.2 }, { "rlh", U_REM, 1.2 },
	{ "vw", U_VW, 1 }, { "vh", U_VH, 1 }, { "vi", U_VW, 1 },
	{ "vb", U_VH, 1 }, { "vmin", U_VMIN, 1 }, { "vmax", U_VMAX, 1 },
	{ "svw", U_VW, 1 }, { "svh", U_VH, 1 }, { "lvw", U_VW, 1 },
	{ "lvh", U_VH, 1 }, { "dvw", U_VW, 1 }, { "dvh", U_VH, 1 },
	{ "svmin", U_VMIN, 1 }, { "svmax", U_VMAX, 1 },
	{ "dvmin", U_VMIN, 1 }, { "dvmax", U_VMAX, 1 },
	{ "lvmin", U_VMIN, 1 }, { "lvmax", U_VMAX, 1 },
	{ NULL, 0, 0 }
};

static int lin_unit(const struct cv *v, struct lin *l)
{
	int i;

	memset(l, 0, sizeof(*l));
	for (i = 0; units[i].name; i++)
		if (ascii_ieq(v->str, units[i].name)) {
			l->k[units[i].unit] = v->num * units[i].scale;
			return 1;
		}
	return 0;
}

static int lin_is_abs(const struct lin *l)
{
	int i;

	for (i = 1; i < NUNITS; i++)
		if (l->k[i] != 0)
			return 0;
	return 1;
}

static void calc_free(struct calc *k)
{
	int i;

	if (!k)
		return;
	for (i = 0; i < k->nargs; i++)
		calc_free(k->args[i]);
	free(k->args);
	free(k);
}

/* calc() parsing: each node is a lin, or a tree when not foldable. */
struct cnode {
	struct lin v;
	struct calc *tree;	/* min/max/clamp left for later */
};

static int calc_sum(struct cur *c, struct cnode *out);

/* calc() parsers: on failure out owns nothing (out->tree is NULL), so
 * that a caller freeing it after does not free it twice */
static int sum_to_end(struct cur *c, struct cnode *out)
{
	if (!calc_sum(c, out))
		return 0;
	if (cur_end(c))
		return 1;
	calc_free(out->tree);
	out->tree = NULL;
	return 0;
}

static int calc_math_fn(const struct cv *f, struct cnode *out)
{
	struct cur c;
	struct cnode args[16];
	int n = 0, i, op, j;

	op = cv_func(f, "min") ? K_MIN : cv_func(f, "max") ? K_MAX :
		cv_func(f, "clamp") ? K_CLAMP : -1;
	cur_init(&c, &f->kids);
	if (op < 0) {
		if (!cv_func(f, "calc") && !cv_func(f, "-webkit-calc"))
			return 0;
		return sum_to_end(&c, out);
	}
	for (;;) {
		const struct cv *v;

		if (n == 16 || !calc_sum(&c, &args[n]))
			goto fail;
		n++;
		v = cur_next(&c);
		if (!v)
			break;
		if (v->type != CT_COMMA)
			goto fail;
	}
	if (op == K_CLAMP && n != 3)
		goto fail;
	for (i = 1; i < n; i++)
		if (args[i].v.is_num != args[0].v.is_num)
			goto fail;
	/* Fold when everything is absolute. */
	for (i = 0; i < n; i++)
		if (args[i].tree || !lin_is_abs(&args[i].v))
			break;
	memset(out, 0, sizeof(*out));
	if (i == n) {
		double r;
		int num = args[0].v.is_num;

#define VAL(x) (num ? (x).v.num : (x).v.k[U_PX])
		r = VAL(args[0]);
		if (op == K_CLAMP) {
			double lo = VAL(args[0]), val = VAL(args[1]),
				hi = VAL(args[2]);

			r = val < hi ? val : hi;
			r = r > lo ? r : lo;
		} else {
			for (j = 1; j < n; j++) {
				double x = VAL(args[j]);

				if (op == K_MIN ? x < r : x > r)
					r = x;
			}
		}
#undef VAL
		out->v.is_num = num;
		if (num)
			out->v.num = r;
		else
			out->v.k[U_PX] = r;
		return 1;
	}
	out->tree = xmalloc(sizeof(*out->tree));
	memset(out->tree, 0, sizeof(*out->tree));
	out->tree->op = op;
	out->tree->args = xmalloc((size_t)n * sizeof(*out->tree->args));
	out->tree->nargs = n;
	for (i = 0; i < n; i++) {
		if (args[i].tree) {
			out->tree->args[i] = args[i].tree;
		} else {
			struct calc *leaf = xmalloc(sizeof(*leaf));

			memset(leaf, 0, sizeof(*leaf));
			leaf->leaf = args[i].v;
			out->tree->args[i] = leaf;
		}
	}
	out->v.is_num = args[0].v.is_num;
	return 1;
fail:
	for (i = 0; i < n; i++)
		calc_free(args[i].tree);
	return 0;
}

static int calc_value(struct cur *c, struct cnode *out)
{
	const struct cv *v = cur_next(c);

	memset(out, 0, sizeof(*out));
	if (!v)
		return 0;
	switch (v->type) {
	case CT_NUMBER:
		out->v.is_num = 1;
		out->v.num = v->num;
		return 1;
	case CT_PERCENT:
		out->v.k[U_PCT] = v->num;
		return 1;
	case CT_DIMENSION:
		return lin_unit(v, &out->v);
	case CT_BLOCK:
		if (v->delim == '(') {
			struct cur in;

			cur_init(&in, &v->kids);
			return sum_to_end(&in, out);
		}
		return 0;
	case CT_FUNCTION:
		return calc_math_fn(v, out);
	case CT_IDENT:
		out->v.is_num = 1;
		if (ascii_ieq(v->str, "pi"))
			out->v.num = M_PI;
		else if (ascii_ieq(v->str, "e"))
			out->v.num = M_E;
		else
			return 0;
		return 1;
	}
	return 0;
}

static void lin_scale(struct lin *l, double f)
{
	int i;

	for (i = 0; i < NUNITS; i++)
		l->k[i] *= f;
	l->num *= f;
}

static int calc_product(struct cur *c, struct cnode *out)
{
	struct cnode rhs;
	const struct cv *v;

	if (!calc_value(c, out))
		return 0;
	while ((v = cur_peek(c)) && v->type == CT_DELIM &&
	       (v->delim == '*' || v->delim == '/')) {
		int div = v->delim == '/';

		cur_next(c);
		if (!calc_value(c, &rhs))
			goto fail;
		if (out->tree || rhs.tree) {
			calc_free(rhs.tree);
			goto fail;	/* min() * x: rare, unsupported */
		}
		if (div) {
			if (!rhs.v.is_num || rhs.v.num == 0)
				goto fail;
			lin_scale(&out->v, 1 / rhs.v.num);
		} else if (rhs.v.is_num) {
			lin_scale(&out->v, rhs.v.num);
		} else if (out->v.is_num) {
			double f = out->v.num;

			out->v = rhs.v;
			lin_scale(&out->v, f);
		} else {
			goto fail;
		}
	}
	return 1;
fail:
	calc_free(out->tree);
	out->tree = NULL;
	return 0;
}

static int calc_sum(struct cur *c, struct cnode *out)
{
	struct cnode rhs;
	const struct cv *v;
	int i;

	if (!calc_product(c, out))
		return 0;
	while ((v = cur_peek(c)) && v->type == CT_DELIM &&
	       (v->delim == '+' || v->delim == '-')) {
		double sign = v->delim == '-' ? -1 : 1;

		/* + and - need whitespace on both sides */
		if (c->i == 0 || c->v[c->i - 1].type != CT_WS ||
		    c->i + 1 >= c->n || c->v[c->i + 1].type != CT_WS)
			goto fail;
		cur_next(c);
		if (!calc_product(c, &rhs))
			goto fail;
		if (out->tree || rhs.tree || rhs.v.is_num != out->v.is_num) {
			calc_free(rhs.tree);
			goto fail;
		}
		for (i = 0; i < NUNITS; i++)
			out->v.k[i] += sign * rhs.v.k[i];
		out->v.num += sign * rhs.v.num;
	}
	return 1;
fail:
	calc_free(out->tree);
	out->tree = NULL;
	return 0;
}

struct slen *parse_length(struct cur *c, int flags)
{
	const struct cv *v = cur_peek(c);
	struct cnode n;
	struct slen *l;
	int ok = 0;

	if (!v)
		return NULL;
	memset(&n, 0, sizeof(n));
	if (v->type == CT_DIMENSION) {
		ok = lin_unit(v, &n.v);
	} else if (v->type == CT_PERCENT) {
		ok = !(flags & L_NOPCT);
		n.v.k[U_PCT] = v->num;
	} else if (v->type == CT_NUMBER) {
		if (flags & L_NUMBER) {
			ok = 1;
			n.v.is_num = 1;
			n.v.num = v->num;
		} else {
			ok = v->num == 0;	/* unitless zero */
		}
	} else if (v->type == CT_FUNCTION) {
		struct cur save = *c;

		ok = calc_math_fn(v, &n);
		if (ok && n.v.is_num && !(flags & L_NUMBER))
			ok = 0;
		if (ok && !n.v.is_num && (flags & L_NOPCT) &&
		    (n.v.k[U_PCT] != 0 || n.tree))
			ok = 0;
		*c = save;
		if (!ok)
			calc_free(n.tree);
		else if (flags & L_NONNEG)
			flags &= ~L_NONNEG;	/* calc() clamps instead */
	}
	if (!ok)
		return NULL;
	if ((flags & L_NONNEG) && !n.tree) {
		int i;

		for (i = 0; i < NUNITS; i++)
			if (n.v.k[i] < 0)
				return NULL;
		if (n.v.num < 0)
			return NULL;
	}
	cur_next(c);
	l = xmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->v = n.v;
	l->tree = n.tree;
	return l;
}

void slen_free(struct slen *l)
{
	if (l) {
		calc_free(l->tree);
		free(l);
	}
}

int vp_units_used;
THREAD_LOCAL int color_dark;

static void lin_resolve(const struct lin *l, const struct ucontext *u,
	float *px, float *pct)
{
	double vmin = u->vw < u->vh ? u->vw : u->vh;
	double vmax = u->vw > u->vh ? u->vw : u->vh;
	double z = u->zoom > 0 ? u->zoom : 1;

	if (l->k[U_VW] || l->k[U_VH] || l->k[U_VMIN] || l->k[U_VMAX])
		vp_units_used = 1;

	*px = (float)(l->k[U_PX] * z + l->k[U_EM] * u->em + l->k[U_REM] * u->rem +
		l->k[U_EX] * u->ex + l->k[U_CH] * u->ch +
		(l->k[U_VW] * u->vw + l->k[U_VH] * u->vh +
		 l->k[U_VMIN] * vmin + l->k[U_VMAX] * vmax) / 100);
	*pct = (float)l->k[U_PCT];
}

static struct calc *calc_resolve(const struct calc *k,
	const struct ucontext *u)
{
	struct calc *r = xmalloc(sizeof(*r));
	int i;

	memset(r, 0, sizeof(*r));
	r->op = k->op;
	if (k->op == K_LEAF) {
		lin_resolve(&k->leaf, u, &r->px, &r->pct);
		return r;
	}
	r->nargs = k->nargs;
	r->args = xmalloc((size_t)k->nargs * sizeof(*r->args));
	for (i = 0; i < k->nargs; i++)
		r->args[i] = calc_resolve(k->args[i], u);
	return r;
}

static int calc_has_pct(const struct calc *k)
{
	int i;

	if (k->op == K_LEAF)
		return k->pct != 0;
	for (i = 0; i < k->nargs; i++)
		if (calc_has_pct(k->args[i]))
			return 1;
	return 0;
}

float calc_eval(const struct calc *k, float ref)
{
	float r, x;
	int i;

	if (k->op == K_LEAF)
		return k->px + k->pct * ref / 100;
	if (k->op == K_CLAMP) {
		float lo = calc_eval(k->args[0], ref),
			v = calc_eval(k->args[1], ref),
			hi = calc_eval(k->args[2], ref);

		r = v < hi ? v : hi;
		return r > lo ? r : lo;
	}
	r = calc_eval(k->args[0], ref);
	for (i = 1; i < k->nargs; i++) {
		x = calc_eval(k->args[i], ref);
		if (k->op == K_MIN ? x < r : x > r)
			r = x;
	}
	return r;
}

/* Returns 1 for a length, 2 for a number (with L_NUMBER). */
int slen_resolve(const struct slen *l, const struct ucontext *u,
	struct len *out)
{
	memset(out, 0, sizeof(*out));
	if (l->v.is_num) {
		out->px = (float)l->v.num;
		return 2;
	}
	if (l->tree) {
		out->calc = calc_resolve(l->tree, u);
		out->px = calc_eval(out->calc, 0);
		/* without percentages it is just a length */
		if (!calc_has_pct(out->calc)) {
			calc_free(out->calc);
			out->calc = NULL;
		}
		return 1;
	}
	lin_resolve(&l->v, u, &out->px, &out->pct);
	return 1;
}

int slen_number(const struct slen *l, double *out)
{
	if (!l->v.is_num || l->tree)
		return 0;
	*out = l->v.num;
	return 1;
}

void calc_release(struct calc *k)
{
	calc_free(k);
}

static void ser_calc(struct buf *b, const struct calc *k)
{
	static const char *const fn[] = { "", "min(", "max(", "clamp(" };
	int i;

	if (k->op == K_LEAF) {
		struct len l;

		l.px = k->px;
		l.pct = k->pct;
		l.calc = NULL;
		serialize_len(b, &l);
		return;
	}
	buf_add(b, fn[k->op], strlen(fn[k->op]));
	for (i = 0; i < k->nargs; i++) {
		if (i)
			buf_add(b, ", ", 2);
		ser_calc(b, k->args[i]);
	}
	buf_putc(b, ')');
}

/* Lengths are floats: 4 decimals, like browsers show them. */
void serialize_float(struct buf *b, double v)
{
	double r = v < 0 ? -v : v;

	r = (double)(long long)(r * 10000 + 0.5) / 10000;
	serialize_number(b, v < 0 ? -r : r);
}

void serialize_len(struct buf *b, const struct len *l)
{
	if (l->calc) {
		ser_calc(b, l->calc);
		return;
	}
	if (l->pct != 0 && l->px != 0) {
		buf_add(b, "calc(", 5);
		serialize_float(b, l->pct);
		buf_add(b, l->px < 0 ? "% - " : "% + ", 4);
		serialize_float(b, l->px < 0 ? -l->px : l->px);
		buf_add(b, "px)", 3);
	} else if (l->pct != 0) {
		serialize_float(b, l->pct);
		buf_putc(b, '%');
	} else {
		serialize_float(b, l->px);
		buf_add(b, "px", 2);
	}
}

/* An+B (CSS Syntax 3, 6.2) */

static int parse_int_str(const char *s, int *out)
{
	long v = 0;
	int sign = 1;

	if (*s == '-' || *s == '+') {
		sign = *s == '-' ? -1 : 1;
		s++;
	}
	if (!*s)
		return 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return 0;
		if (v < 100000000)
			v = v * 10 + (*s - '0');
	}
	*out = (int)(sign * v);
	return 1;
}

/* Optional "+ B", "- B", or a signed integer B after the n part. */
static int anb_tail(struct cur *c, int *b)
{
	const struct cv *v = cur_peek(c);

	*b = 0;
	if (!v)
		return 1;
	if (v->type == CT_NUMBER && v->integer &&
	    (v->repr[0] == '+' || v->repr[0] == '-')) {
		cur_next(c);
		*b = (int)v->num;
		return 1;
	}
	if (v->type == CT_DELIM && (v->delim == '+' || v->delim == '-')) {
		int sign = v->delim == '-' ? -1 : 1;

		cur_next(c);
		v = cur_next(c);
		if (!v || v->type != CT_NUMBER || !v->integer ||
		    v->repr[0] == '+' || v->repr[0] == '-')
			return 0;
		*b = sign * (int)v->num;
		return 1;
	}
	return 1;
}

int parse_anb(struct cur *c, int *a, int *b)
{
	const struct cv *v = cur_next(c);
	const char *s;
	int plus = 0;

	if (!v)
		return 0;
	if (v->type == CT_DELIM && v->delim == '+') {
		/* "+n": no whitespace between + and n */
		if (c->i >= c->n || c->v[c->i].type != CT_IDENT)
			return 0;
		v = cur_next(c);
		plus = 1;
	}
	if (!plus && v->type == CT_NUMBER) {
		if (!v->integer)
			return 0;
		*a = 0;
		*b = (int)v->num;
		return 1;
	}
	if (!plus && v->type == CT_DIMENSION) {
		if (!v->integer)
			return 0;
		*a = (int)v->num;
		s = v->str;
	} else if (v->type == CT_IDENT) {
		s = v->str;
		if (!plus && (ascii_ieq(s, "odd") || ascii_ieq(s, "even"))) {
			*a = 2;
			*b = ascii_ieq(s, "odd");
			return 1;
		}
		if (s[0] == '-' && !plus) {
			*a = -1;
			s++;
		} else {
			*a = 1;
		}
	} else {
		return 0;
	}
	if (s[0] != 'n' && s[0] != 'N')
		return 0;
	if (!s[1])
		return anb_tail(c, b);
	if (s[1] != '-')
		return 0;
	if (!s[2]) {
		/* "n-" then a digits-only number */
		v = cur_next(c);
		if (!v || v->type != CT_NUMBER || !v->integer ||
		    v->repr[0] == '+' || v->repr[0] == '-')
			return 0;
		*b = -(int)v->num;
		return 1;
	}
	return parse_int_str(s + 1, b);
}
