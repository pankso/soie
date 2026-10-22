/*
 * Form controls, painted by the engine: text fields with their value,
 * placeholder and caret, checkboxes, radio buttons, push buttons and
 * selects. Their box (background, border) comes from the UA style.
 */
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

void paint_control(struct canvas *c, const struct box *b)
{
	const soie_node *n = b->node;
	const struct style *st = b->style;
	const char *type = soie_element_get_attr(n, "type"), *v;
	unsigned char fg[4], gray[4] = { 117, 117, 117, 255 };
	struct fontm m;
	struct buf text = { 0 };
	float base, x = b->x, tw;
	int focused = n->doc->u.doc.focus == n;

	color_to_rgba(st->v[P_COLOR].u.color, fg);
	font_metrics(st, &m);
	base = b->y + (b->h - m.ascent - m.descent) / 2 + m.ascent;
	if (n->tag == TAG_INPUT && type && (ascii_ieq(type, "checkbox") ||
					    ascii_ieq(type, "radio"))) {
		int checked = soie_element_get_attr(n, "checked") != NULL;
		unsigned char white[4] = { 255, 255, 255, 255 };
		float s = b->w < b->h ? b->w : b->h;

		fill_rect(c, b->x, b->y, s, s, fg);
		fill_rect(c, b->x + 1, b->y + 1, s - 2, s - 2, white);
		if (checked)
			fill_rect(c, b->x + 3, b->y + 3, s - 6, s - 6, fg);
		return;
	}
	if (n->tag == TAG_SELECT) {
		option_text(n, &text);
		paint_text(c, st, text.s, text.len, x, base);
		/* the drop-down arrow */
		fill_rect(c, b->x + b->w - 8, base - m.ascent / 2, 6, 2, fg);
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
		paint_text(c, st, v, strlen(v), x, base);
		(void)gray;
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
			int caret = n->doc->u.doc.caret;
			size_t len = strlen(v);

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
