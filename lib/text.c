/*
 * Text measurement. Until FreeType and HarfBuzz arrive (phase 4) every
 * character is 0.5em wide, ascent 0.75em, descent 0.25em, and normal
 * line-height 1.25em: round numbers that keep layout tests readable.
 */
#include "layout.h"

static float font_size(const struct style *st)
{
	return st->v[P_FONT_SIZE].u.len.px;
}

void font_metrics(const struct style *st, struct fontm *m)
{
	float em = font_size(st);

	m->ascent = em * 0.75f;
	m->descent = em * 0.25f;
	m->normal_lh = em * 1.25f;
}

float text_width(const struct style *st, const char *s, size_t len)
{
	float em = font_size(st), w = 0, ls = 0, ws = 0;
	size_t i;

	if (!st->v[P_LETTER_SPACING].kw)
		ls = st->v[P_LETTER_SPACING].u.len.px;
	if (!st->v[P_WORD_SPACING].kw)
		ws = st->v[P_WORD_SPACING].u.len.px;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];

		if ((c & 0xc0) == 0x80)
			continue;	/* UTF-8 continuation byte */
		w += em * 0.5f + ls;
		if (c == ' ')
			w += ws;
	}
	return w;
}
