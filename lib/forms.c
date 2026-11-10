/*
 * Form controls, painted by the engine: text fields with their value,
 * placeholder and caret, checkboxes, radio buttons, push buttons and
 * selects. Their box (background, border) comes from the UA style.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "paint.h"

static const char *option_text(const soie_node *sel, struct buf *b)
{
	const soie_node *n = sel->first, *pick = NULL, *first = NULL;

	/* the selected option, else the first one */
	while (n && n != sel) {
		if (is_html(n, TAG_OPTION)) {
			if (!first)
				first = n;
			if (soie_element_get_attr(n, "selected")) {
				pick = n;
				break;
			}
		}
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n != sel && !n->next)
			n = n->parent;
		if (n != sel)
			n = n->next;
	}
	if (!pick)
		pick = first;
	if (pick) {
		const soie_node *t;

		for (t = pick->first; t; t = t->next)
			if (t->type == SOIE_TEXT)
				buf_add(b, t->u.text.data, t->u.text.len);
	}
	buf_add(b, "", 0);
	return b->s;
}

/* A number attribute of a control, dflt when missing or not one */
static float control_num(const soie_node *n, const char *name, float dflt)
{
	const char *a = soie_element_get_attr(n, name);
	char *e;
	float v;

	if (!a)
		return dflt;
	v = strtof(a, &e);
	return e == a ? dflt : v;
}

void paint_control(struct canvas *c, const struct box *b)
{
	const soie_node *n = b->node;
	const struct style *st = b->style;
	const char *type = soie_element_get_attr(n, "type"), *v;
	unsigned char fg[4];
	struct fontm m;
	struct buf text = { 0 };
	float base, x = b->x, tw;
	int focused = n->doc->u.doc->focus == n;

	color_to_rgba(st->v[P_COLOR].u.color, fg);
	font_metrics(st, &m);
	base = b->y + (b->h - m.ascent - m.descent) / 2 + m.ascent;
	if (n->tag == TAG_INPUT && type && (ascii_ieq(type, "checkbox") ||
					    ascii_ieq(type, "radio"))) {
		/* the colours of Firefox's and Chrome's, not the text's:
		 * a gray ring on white, the accent blue when checked */
		static const unsigned char ring[4] = { 118, 118, 118, 255 },
			white[4] = { 255, 255, 255, 255 },
			accent[4] = { 0, 117, 255, 255 };
		int checked = soie_element_get_attr(n, "checked") != NULL;
		float s = b->w < b->h ? b->w : b->h, x0 = b->x, y0 = b->y;

		if (ascii_ieq(type, "radio")) {
			fill_round(c, x0, y0, s, s, s / 2,
				checked ? accent : ring);
			fill_round(c, x0 + 1, y0 + 1, s - 2, s - 2,
				(s - 2) / 2, white);
			if (checked)
				fill_round(c, x0 + s * 0.25f, y0 + s * 0.25f,
					s * 0.5f, s * 0.5f, s * 0.25f, accent);
			return;
		}
		if (!checked) {
			fill_round(c, x0, y0, s, s, 2, ring);
			fill_round(c, x0 + 1, y0 + 1, s - 2, s - 2, 1.5f,
				white);
		} else {
			/* a tick, in fractions of the box */
			static const float tick[12] = { .20f, .52f, .40f, .70f,
				.78f, .28f, .88f, .40f, .40f, .88f, .10f, .62f };
			float p[12];
			int i;

			for (i = 0; i < 12; i += 2) {
				p[i] = x0 + tick[i] * s;
				p[i + 1] = y0 + tick[i + 1] * s;
			}
			fill_round(c, x0, y0, s, s, 2, accent);
			fill_polygon(c, p, 6, white);
		}
		return;
	}
	if (n->tag != TAG_INPUT && n->tag != TAG_SELECT &&
	    n->tag != TAG_TEXTAREA) {
		/* progress and meter: a rounded track, filled up to the
		 * value (none: nothing filled) */
		static const unsigned char track[4] = { 233, 233, 237, 255 },
			edge[4] = { 143, 143, 157, 255 },
			blue[4] = { 0, 96, 223, 255 },
			green[4] = { 23, 163, 74, 255 };
		float mn = n->name[0] == 'm' ? control_num(n, "min", 0) : 0;
		float mx = control_num(n, "max", 1);
		float val = control_num(n, "value", mn), f;

		f = mx > mn ? (val - mn) / (mx - mn) : 0;
		f = f < 0 ? 0 : f > 1 ? 1 : f;
		fill_round(c, b->x, b->y, b->w, b->h, b->h / 2, edge);
		fill_round(c, b->x + 1, b->y + 1, b->w - 2, b->h - 2,
			b->h / 2 - 1, track);
		if (f * b->w >= b->h)
			fill_round(c, b->x, b->y, f * b->w, b->h, b->h / 2,
				n->name[0] == 'm' ? green : blue);
		return;
	}
	if (n->tag == TAG_INPUT && type && ascii_ieq(type, "range")) {
		/* a track with its thumb where the value is */
		static const unsigned char track[4] = { 233, 233, 237, 255 },
			edge[4] = { 143, 143, 157, 255 },
			blue[4] = { 0, 96, 223, 255 },
			thumb[4] = { 103, 103, 116, 255 };
		float mn = control_num(n, "min", 0), mx = control_num(n, "max",
			100);
		float val = control_num(n, "value", (mn + mx) / 2), f;
		float th = b->h < 16 ? b->h : 16, ty = b->y + (b->h - 6) / 2;

		f = mx > mn ? (val - mn) / (mx - mn) : 0;
		f = f < 0 ? 0 : f > 1 ? 1 : f;
		fill_round(c, b->x, ty, b->w, 6, 3, edge);
		fill_round(c, b->x + 1, ty + 1, b->w - 2, 4, 2, track);
		fill_round(c, b->x, ty, th / 2 + f * (b->w - th), 6, 3, blue);
		fill_round(c, b->x + f * (b->w - th), b->y + (b->h - th) / 2,
			th, th, th / 2, thumb);
		return;
	}
	if (n->tag == TAG_INPUT && type && ascii_ieq(type, "color")) {
		/* the color it holds, black by default */
		static const unsigned char edge[4] = { 118, 118, 118, 255 };
		unsigned char col[4] = { 0, 0, 0, 255 };

		v = soie_element_get_attr(n, "value");
		if (v && *v == '#' && strlen(v) == 7) {
			unsigned long x24 = strtoul(v + 1, NULL, 16);

			col[0] = (unsigned char)(x24 >> 16);
			col[1] = (unsigned char)(x24 >> 8);
			col[2] = (unsigned char)x24;
		}
		fill_rect(c, b->x, b->y, b->w, b->h, edge);
		fill_rect(c, b->x + 1, b->y + 1, b->w - 2, b->h - 2, col);
		return;
	}
	if (n->tag == TAG_INPUT && type && ascii_ieq(type, "file")) {
		/* its button, then what is chosen: nothing here */
		static const unsigned char face[4] = { 233, 233, 237, 255 },
			edge[4] = { 143, 143, 157, 255 };
		static const char label[] = "Browse\xe2\x80\xa6";
		static const char none[] = "No file selected.";
		float bw = text_width(st, label, sizeof(label) - 1) + 10;

		fill_round(c, b->x, b->y, bw, b->h, 4, edge);
		fill_round(c, b->x + 1, b->y + 1, bw - 2, b->h - 2, 3, face);
		paint_text(c, st, label, sizeof(label) - 1, b->x + 5, base);
		paint_text(c, st, none, sizeof(none) - 1, b->x + bw + 5, base);
		return;
	}
	if (n->tag == TAG_SELECT && (soie_element_get_attr(n, "multiple") ||
	    control_num(n, "size", 1) > 1)) {
		/* a list box: its first options, one a row */
		const soie_node *o = n->first;
		float lh = m.normal_lh + 4, y = b->y;

		while (o && o != n && y + lh <= b->y + b->h + 1) {
			if (is_html(o, TAG_OPTION)) {
				const soie_node *t;

				text.len = 0;
				for (t = o->first; t; t = t->next)
					if (t->type == SOIE_TEXT)
						buf_add(&text, t->u.text.data,
							t->u.text.len);
				paint_text(c, st, text.s ? text.s : "", text.len,
					x + 4, y + 2 + m.ascent);
				y += lh;
			}
			if (o->first && !is_html(o, TAG_OPTION)) {
				o = o->first;
				continue;
			}
			while (o != n && !o->next)
				o = o->parent;
			if (o != n)
				o = o->next;
		}
		buf_free(&text);
		return;
	}
	if (n->tag == TAG_SELECT) {
		/* the chosen option and a chevron */
		float ax = b->x + b->w - 11, ay = b->y + b->h / 2 - 2;
		float p[12];

		option_text(n, &text);
		paint_text(c, st, text.s, text.len, x, base);
		p[0] = ax, p[1] = ay;
		p[2] = ax + 4, p[3] = ay + 4;
		p[4] = ax + 8, p[5] = ay;
		p[6] = ax + 8, p[7] = ay + 1.6f;
		p[8] = ax + 4, p[9] = ay + 5.6f;
		p[10] = ax, p[11] = ay + 1.6f;
		fill_polygon(c, p, 6, fg);
		buf_free(&text);
		return;
	}
	if (n->tag == TAG_TEXTAREA) {
		const soie_node *t;

		for (t = n->first; t; t = t->next)
			if (t->type == SOIE_TEXT)
				buf_add(&text, t->u.text.data, t->u.text.len);
		v = soie_element_get_attr(n, "value");
		if (v) {
			text.len = 0;
			buf_add(&text, v, strlen(v));
		}
		buf_add(&text, "", 0);
		base = b->y + m.ascent;
		{
			/* one line per newline, no wrapping yet */
			const char *s = text.s, *e;
			float lh = m.ascent + m.descent;

			while (s && *s && base < b->y + b->h + m.ascent) {
				e = strchr(s, '\n');
				paint_text(c, st, s, e ? (size_t)(e - s) :
					strlen(s), x, base);
				base += lh;
				s = e ? e + 1 : NULL;
			}
		}
		buf_free(&text);
		return;
	}
	if (type && (ascii_ieq(type, "submit") || ascii_ieq(type, "reset") ||
		     ascii_ieq(type, "button"))) {
		v = soie_element_get_attr(n, "value");
		if (!v)
			v = ascii_ieq(type, "reset") ? "Reset" :
				ascii_ieq(type, "submit") ? "Submit" : "";
		tw = text_width(st, v, strlen(v));
		paint_text(c, st, v, strlen(v), x + (b->w - tw) / 2, base);
		return;
	}
	v = soie_element_get_attr(n, "value");
	if (type && ascii_ieq(type, "password") && v) {
		size_t k, len = strlen(v);

		for (k = 0; k < len; k++)
			if (((unsigned char)v[k] & 0xc0) != 0x80)
				buf_add(&text, "\xe2\x80\xa2", 3);
		buf_add(&text, "", 0);
		v = text.s;
	}
	if ((!v || !*v) && !focused &&
	    (v = soie_element_get_attr(n, "placeholder"))) {
		/* the text's colour, faded as browsers do (Firefox 0.54) */
		float a = c->alpha;

		c->alpha *= 0.55f;
		paint_text(c, st, v, strlen(v), x, base);
		c->alpha = a;
		buf_free(&text);
		return;
	}
	if (!v)
		v = "";
	{
		int cx0 = c->cx1;
		int clip = (int)(b->x + b->w - c->ox + 1);

		if (clip < c->cx1)
			c->cx1 = clip;
		paint_text(c, st, v, strlen(v), x, base);
		if (focused) {
			int caret = n->doc->u.doc->caret;
			size_t len = strlen(v);

			if (v == text.s) {
				/* a password: the caret is in the value, one
				 * bullet (3 bytes) a character before it */
				const char *o = soie_element_get_attr(n, "value");
				int k, chars = 0;

				for (k = 0; o[k] && k < caret; k++)
					if (((unsigned char)o[k] & 0xc0) != 0x80)
						chars++;
				caret = caret < 0 ? -1 : chars * 3;
			}
			if (caret < 0 || (size_t)caret > len)
				caret = (int)len;
			tw = text_width(st, v, (size_t)caret);
			fill_rect(c, x + tw, base - m.ascent, 1,
				m.ascent + m.descent, fg);
		}
		c->cx1 = cx0;
	}
	buf_free(&text);
}
