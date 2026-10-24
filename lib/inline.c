/*
 * Inline layout: white-space processing (CSS Text 3 4.1), line breaking
 * at spaces, line boxes and vertical alignment (CSS 2 10.8), text-align,
 * floats and absolute boxes met in the text.
 */
#include <stdlib.h>
#include <string.h>
#include "layout.h"

enum { I_TEXT, I_OPEN, I_CLOSE, I_ATOMIC, I_BREAK, I_FLOAT, I_ABS,
	I_MARKER };

struct item {
	int kind;
	struct box *box;
};

struct items {
	struct item *v;
	int n, cap;
};

static void add_item(struct items *it, int kind, struct box *b)
{
	if (it->n == it->cap) {
		it->cap = it->cap ? it->cap * 2 : 32;
		it->v = xrealloc(it->v, (size_t)it->cap * sizeof(*it->v));
	}
	it->v[it->n].kind = kind;
	it->v[it->n].box = b;
	it->n++;
}

static void collect(struct box *b, struct items *it)
{
	struct box *c;

	for (c = b->first; c; c = c->next) {
		if (c->flags & BF_ABS)
			add_item(it, I_ABS, c);
		else if (c->flags & BF_FLOAT)
			add_item(it, I_FLOAT, c);
		else if (c->type == B_TEXT)
			add_item(it, I_TEXT, c);
		else if (c->type == B_BREAK)
			add_item(it, I_BREAK, c);
		else if (c->type == B_INLINE && (c->flags & BF_OUTSIDE))
			add_item(it, I_MARKER, c);
		else if (c->type == B_INLINE) {
			add_item(it, I_OPEN, c);
			collect(c, it);
			add_item(it, I_CLOSE, c);
		} else {
			add_item(it, I_ATOMIC, c);
		}
	}
}

/* White-space processing, once per text box. */

static int ws_mode(const struct style *st)
{
	if (kw(st, P_WHITE_SPACE_COLLAPSE, "collapse") ||
	    kw(st, P_WHITE_SPACE_COLLAPSE, "discard"))
		return 'c';
	if (kw(st, P_WHITE_SPACE_COLLAPSE, "preserve-breaks"))
		return 'b';
	return 'p';
}

static int nowrap(const struct style *st)
{
	return kw(st, P_TEXT_WRAP_MODE, "nowrap");
}

static int is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void transform(const struct style *st, char *s, size_t n)
{
	int up = kw(st, P_TEXT_TRANSFORM, "uppercase");
	int low = kw(st, P_TEXT_TRANSFORM, "lowercase");
	int cap = kw(st, P_TEXT_TRANSFORM, "capitalize");
	size_t i;

	if (!up && !low && !cap)
		return;
	for (i = 0; i < n; i++) {
		int start = i == 0 || s[i - 1] == ' ';

		if ((up || (cap && start)) && s[i] >= 'a' && s[i] <= 'z')
			s[i] = (char)(s[i] - 32);
		else if (low && s[i] >= 'A' && s[i] <= 'Z')
			s[i] = (char)(s[i] + 32);
	}
}

/* prev: was the last character a collapsible space (or line start)? */
static void process_text(struct box *t, int *prev)
{
	int mode = ws_mode(t->style);
	struct buf b = { 0 };
	size_t i;

	buf_add(&b, "", 0);
	for (i = 0; i < t->len; i++) {
		int c = (unsigned char)t->text[i];

		if (mode == 'c' || (mode == 'b' && c != '\n')) {
			if (is_space(c)) {
				if (!*prev)
					buf_putc(&b, ' ');
				*prev = 1;
				continue;
			}
		} else if (mode == 'b' && c == '\n') {
			/* spaces before a preserved newline go away */
			while (b.len && b.s[b.len - 1] == ' ')
				b.len--;
			buf_putc(&b, '\n');
			*prev = 1;
			continue;
		} else if (c == '\r' || c == '\f') {
			c = ' ';
		}
		buf_putc(&b, c);
		*prev = c == '\n';
	}
	b.s[b.len] = '\0';
	transform(t->style, b.s, b.len);
	free(t->text);
	t->text = b.s;
	t->len = b.len;
	t->ws_done = 1;
}

static void process_items(struct items *it)
{
	int prev = 1, i;

	for (i = 0; i < it->n; i++) {
		struct item *x = &it->v[i];

		if (x->kind == I_TEXT) {
			if (!x->box->ws_done)
				process_text(x->box, &prev);
			else if (x->box->len)
				prev = x->box->text[x->box->len - 1] == ' ' ||
					x->box->text[x->box->len - 1] == '\n';
		} else if (x->kind == I_ATOMIC) {
			prev = 0;
		} else if (x->kind == I_BREAK) {
			prev = 1;
		}
	}
}

/* Line building */

struct openbox {
	struct box *box;
	float x;
};

struct ls {
	struct box *b;
	struct bfc *f;
	float x0, w0;		/* content box of the container */
	float y;		/* top of the current line */
	float lx, lr;		/* line edges, after floats */
	float pen;
	float strut_h;		/* line height of the container */
	int first;		/* first line? */
	int start;		/* first fragment of the line */
	int content;		/* the line has something visible */
	struct openbox open[64];
	int nopen;
	struct box *pending[32];	/* floats for the next line */
	int npending;
};

static struct frag *new_frag(struct box *b, int kind, struct box *of)
{
	struct frag *f;

	if (b->nfrags == b->capfrags) {
		b->capfrags = b->capfrags ? b->capfrags * 2 : 32;
		b->frags = xrealloc(b->frags,
			(size_t)b->capfrags * sizeof(*b->frags));
	}
	f = &b->frags[b->nfrags++];
	memset(f, 0, sizeof(*f));
	f->box = of;
	f->kind = (unsigned char)kind;
	return f;
}

static float line_height(const struct style *st, const struct fontm *m)
{
	const struct cval *v = &st->v[P_LINE_HEIGHT];

	if (v->kw)
		return m->normal_lh;
	if (v->flag == 1)
		return (float)v->u.num * st->v[P_FONT_SIZE].u.len.px;
	return v->u.len.px;
}

static void edges_at(struct ls *s)
{
	float indent = 0;

	s->lx = bfc_left(s->f, s->y, s->strut_h, s->x0);
	s->lr = bfc_right(s->f, s->y, s->strut_h, s->x0 + s->w0);
	if (s->first)
		indent = len_val(&s->b->style->v[P_TEXT_INDENT], s->w0);
	s->pen = s->lx + indent;
}

/* vertical-align of box against its parent's font (CSS 2 10.8.1). */
static float va_shift(const struct box *b, const struct style *parent,
	float lh)
{
	const struct style *st = b->style;
	const struct cval *v = &st->v[P_VERTICAL_ALIGN];
	float pem = parent->v[P_FONT_SIZE].u.len.px;

	if (!v->kw)
		return -len_val(v, lh);
	if (kw(st, P_VERTICAL_ALIGN, "sub"))
		return pem * 0.2f;
	if (kw(st, P_VERTICAL_ALIGN, "super"))
		return -pem * 0.4f;
	return 0;
}

/* Baseline shift of everything inside inline box b, vs the line's. */
static float total_shift(const struct box *b, const struct box *root)
{
	float s = 0;

	for (; b && b != root; b = b->parent)
		if (b->type == B_INLINE && b->parent) {
			struct fontm m;

			font_metrics(b->style, &m);
			s += va_shift(b, b->parent->style,
				line_height(b->style, &m));
		}
	return s;
}

/* Top-aligned and middle boxes need the font of their parent. */
static int va_kind(const struct style *st)
{
	if (kw(st, P_VERTICAL_ALIGN, "middle"))
		return 'm';
	if (kw(st, P_VERTICAL_ALIGN, "top") ||
	    kw(st, P_VERTICAL_ALIGN, "text-top"))
		return 't';
	if (kw(st, P_VERTICAL_ALIGN, "bottom") ||
	    kw(st, P_VERTICAL_ALIGN, "text-bottom"))
		return 'b';
	return 0;
}

/* Trailing collapsible spaces hang: drop them from the last texts. */
static void trim_end(struct ls *s)
{
	int i;

	for (i = s->b->nfrags - 1; i >= s->start; i--) {
		struct frag *f = &s->b->frags[i];

		if (f->kind == F_INLINE)
			continue;
		if (f->kind != F_TEXT)
			break;
		if (ws_mode(f->box->style) == 'p' &&
		    !kw(f->box->style, P_WHITE_SPACE_COLLAPSE, "preserve"))
			break;
		while (f->len && f->box->text[f->start + f->len - 1] == ' ') {
			float w = text_width(f->box->style, " ", 1);

			f->len--;
			f->w -= w;
			s->pen -= w;
			if (f->spaces)
				f->spaces--;
		}
		if (f->len)
			break;
		if (i == s->b->nfrags - 1)
			s->b->nfrags--;		/* nothing left of it */
	}
}

static void align(struct ls *s, struct line *l, int last)
{
	const struct style *st = s->b->style;
	float extra = s->lr - s->pen, off = 0;
	int i, spaces = 0;

	if (extra < 0)
		extra = 0;
	if (kw(st, P_TEXT_ALIGN, "right") || kw(st, P_TEXT_ALIGN, "end") ||
	    kw(st, P_TEXT_ALIGN, "-webkit-right"))
		off = extra;
	else if (kw(st, P_TEXT_ALIGN, "center") ||
		 kw(st, P_TEXT_ALIGN, "-webkit-center"))
		off = extra / 2;
	else if (kw(st, P_TEXT_ALIGN, "justify") && !last) {
		for (i = l->first; i < l->first + l->n; i++)
			spaces += s->b->frags[i].spaces;
		if (spaces)
			l->jspace = extra / (float)spaces;
	}
	if (!off && !l->jspace)
		return;
	for (i = l->first; i < l->first + l->n; i++) {
		struct frag *f = &s->b->frags[i];

		f->x += off;
		if (l->jspace && f->kind == F_TEXT) {
			f->w += f->spaces * l->jspace;
			off += f->spaces * l->jspace;
		}
	}
}

static void place_pending(struct ls *s);

/* Close the current line: vertical metrics, alignment, next line. */
static void finish_line(struct ls *s, int forced, int last)
{
	struct box *b = s->b;
	struct fontm m;
	struct line *l;
	float lh, half, top, bottom, base;
	int i, k;

	trim_end(s);
	/* inline boxes still open end at the line edge, if they began */
	for (k = 0; k < s->nopen; k++) {
		struct frag *f;

		if (s->open[k].x >= s->pen - 0.01f)
			continue;
		f = new_frag(b, F_INLINE, s->open[k].box);
		f->x = s->open[k].x;
		f->w = s->pen - s->open[k].x;
	}
	if (!s->content && !forced) {
		/* nothing but collapsed spaces: no line box */
		b->nfrags = s->start;
		for (k = 0; k < s->nopen; k++)
			s->open[k].x = s->lx;
		edges_at(s);
		return;
	}
	font_metrics(b->style, &m);
	lh = line_height(b->style, &m);
	half = (lh - m.ascent - m.descent) / 2;
	top = -(m.ascent + half);
	bottom = m.descent + half;
	/* first pass: extents relative to the baseline */
	for (i = s->start; i < b->nfrags; i++) {
		struct frag *f = &b->frags[i];
		float t, bt, shift;

		if (f->kind == F_ATOMIC) {
			struct box *a = f->box;
			int k2 = va_kind(a->style);

			shift = va_shift(a, a->parent->style, lh);
			t = shift - f->baseline;
			bt = t + f->h;
			if (k2 == 't') {
				t = top;
				bt = t + f->h;
			} else if (k2 == 'b') {
				bt = bottom;
				t = bt - f->h;
			} else if (k2 == 'm') {
				float x = a->parent->style->v[P_FONT_SIZE].u.len.px *
					0.25f;

				t = -x - f->h / 2;
				bt = t + f->h;
			}
			f->y = t;	/* margin box top vs baseline, for now */
		} else {
			const struct style *st = f->box->style;
			float flh, fh;

			font_metrics(st, &m);
			flh = line_height(st, &m);
			fh = (flh - m.ascent - m.descent) / 2;
			shift = total_shift(f->kind == F_INLINE ? f->box :
				f->box->parent, b);
			t = shift - m.ascent - fh;
			bt = shift + m.descent + fh;
			f->y = shift - m.ascent;
			f->h = m.ascent + m.descent;
			if (f->kind == F_INLINE) {
				f->y -= f->box->p[0] + f->box->b[0];
				f->h += f->box->p[0] + f->box->b[0] +
					f->box->p[2] + f->box->b[2];
			}
		}
		if (t < top)
			top = t;
		if (bt > bottom)
			bottom = bt;
	}
	base = s->y - top;
	if (b->nlines % 8 == 0)
		b->lines = xrealloc(b->lines, (size_t)(b->nlines + 8) *
			sizeof(*b->lines));
	l = &b->lines[b->nlines++];
	memset(l, 0, sizeof(*l));
	l->x = s->lx;
	l->y = s->y;
	l->w = s->lr - s->lx;
	l->h = bottom - top;
	l->baseline = base;
	l->first = s->start;
	l->n = b->nfrags - s->start;
	align(s, l, last || forced);
	for (i = s->start; i < b->nfrags; i++) {
		struct frag *f = &b->frags[i];

		f->baseline = base;
		f->y += base;
		if (f->kind == F_ATOMIC)
			translate(f->box, f->x, f->y);
	}
	/* outside markers stick to the first line */
	if (b->nlines == 1)
		b->baseline = base;
	b->last_baseline = base;
	s->y += l->h;
	s->first = 0;
	s->start = b->nfrags;
	s->content = 0;
	edges_at(s);
	for (k = 0; k < s->nopen; k++)
		s->open[k].x = s->pen;
	place_pending(s);
}

static void place_pending(struct ls *s)
{
	int i;

	for (i = 0; i < s->npending; i++)
		place_float(s->pending[i], s->f, s->y, s->x0, s->x0 + s->w0,
			s->w0);
	if (s->npending) {
		s->npending = 0;
		edges_at(s);
	}
}

/* Does w more fit on the line? Wraps (once) if not. */
static void need(struct ls *s, float w, int can_wrap)
{
	if (s->pen + w <= s->lr + 0.01f || !can_wrap)
		return;
	if (s->content) {
		finish_line(s, 0, 0);
		return;
	}
	/* an empty line beside floats: go below them if that helps */
	while (s->pen + w > s->lr + 0.01f &&
	       (s->lx > s->x0 || s->lr < s->x0 + s->w0)) {
		float next = bfc_next(s->f, s->y);

		if (next <= s->y)
			break;
		s->y = next;
		edges_at(s);
	}
}

static void add_text(struct ls *s, struct box *t, unsigned start,
	unsigned len, float w)
{
	struct frag *f;
	unsigned i;
	int sp = 0;

	for (i = 0; i < len; i++)
		if (t->text[start + i] == ' ')
			sp++;
	if (s->b->nfrags > s->start) {
		f = &s->b->frags[s->b->nfrags - 1];
		if (f->kind == F_TEXT && f->box == t &&
		    f->start + f->len == start) {
			f->len += len;
			f->w += w;
			f->spaces += sp;
			s->pen += w;
			return;
		}
	}
	f = new_frag(s->b, F_TEXT, t);
	f->start = start;
	f->len = len;
	f->x = s->pen;
	f->w = w;
	f->spaces = sp;
	s->pen += w;
}

static int break_anywhere(const struct style *st)
{
	return kw(st, P_OVERFLOW_WRAP, "anywhere") ||
		kw(st, P_OVERFLOW_WRAP, "break-word") ||
		kw(st, P_WORD_BREAK, "break-all") ||
		kw(st, P_WORD_BREAK, "break-word");
}

static size_t utf8_next(const char *s, size_t i, size_t n)
{
	i++;
	while (i < n && ((unsigned char)s[i] & 0xc0) == 0x80)
		i++;
	return i;
}

/* A word too long for any line: split it where it overflows. */
static void split_word(struct ls *s, struct box *t, size_t i, size_t j)
{
	const struct style *st = t->style;

	while (i < j) {
		size_t k = i, e;
		float w = 0;

		for (e = i; e < j; ) {
			size_t nx = utf8_next(t->text, e, j);
			float cw = text_width(st, t->text + e, nx - e);

			if (s->pen + w + cw > s->lr && k > i)
				break;
			w += cw;
			k = e = nx;
		}
		add_text(s, t, (unsigned)i, (unsigned)(k - i), w);
		s->content = 1;
		i = k;
		if (i < j)
			finish_line(s, 0, 0);
	}
}

static void place_text(struct ls *s, struct box *t)
{
	const struct style *st = t->style;
	int mode = ws_mode(st), wrap = !nowrap(st);
	size_t i = 0, n = t->len;

	while (i < n) {
		size_t j = i, word_end;
		float w, ww;

		if (t->text[i] == '\n') {
			finish_line(s, 1, 1);
			i++;
			continue;
		}
		/* a unit: non-spaces then spaces, up to a newline */
		while (j < n && t->text[j] != ' ' && t->text[j] != '\n')
			j++;
		word_end = j;
		while (j < n && t->text[j] == ' ')
			j++;
		if (!wrap) {
			while (j < n && t->text[j] != '\n')
				j++;
			word_end = j;
		}
		if (i == word_end && mode != 'p' && !s->content) {
			/* collapsible spaces at the start of a line */
			i = j;
			continue;
		}
		ww = text_width(st, t->text + i, word_end - i);
		w = ww + text_width(st, t->text + word_end, j - word_end);
		need(s, ww, wrap && word_end > i);
		if (!s->content && mode != 'p' && t->text[i] == ' ') {
			/* the wrap left this unit at a line start */
			while (i < j && t->text[i] == ' ')
				i++;
			if (i == j)
				continue;
			ww = text_width(st, t->text + i, word_end - i);
			w = ww + text_width(st, t->text + word_end,
				j - word_end);
		}
		if (s->pen + ww > s->lr + 0.01f && wrap && break_anywhere(st) &&
		    word_end > i) {
			split_word(s, t, i, word_end);
			i = word_end;
			if (j > word_end)
				add_text(s, t, (unsigned)i, (unsigned)(j - i),
					text_width(st, t->text + i, j - i));
			i = j;
			continue;
		}
		add_text(s, t, (unsigned)i, (unsigned)(j - i), w);
		if (word_end > i || mode == 'p')
			s->content = 1;
		i = j;
	}
}

static void place_marker(struct ls *s, struct box *m)
{
	struct box *t = m->first;
	struct frag *f;
	float w;
	int prev = 1;

	if (!t || t->type != B_TEXT)
		return;
	if (!t->ws_done)
		process_text(t, &prev);
	w = text_width(t->style, t->text, t->len);
	f = new_frag(s->b, F_TEXT, t);
	f->len = (unsigned)t->len;
	f->x = s->lx - w;
	f->w = w;
}

float layout_inline(struct box *b, struct bfc *f, float x, float y, float w,
	float cbh)
{
	struct items it = { NULL, 0, 0 };
	struct ls s;
	struct fontm m;
	int i;

	collect(b, &it);
	process_items(&it);
	memset(&s, 0, sizeof(s));
	s.b = b;
	s.f = f;
	s.x0 = x;
	s.w0 = w;
	s.y = y;
	s.first = 1;
	font_metrics(b->style, &m);
	s.strut_h = line_height(b->style, &m);
	free(b->lines);
	free(b->frags);
	b->lines = NULL;
	b->frags = NULL;
	b->nlines = b->nfrags = b->capfrags = 0;
	edges_at(&s);
	for (i = 0; i < it.n; i++) {
		struct box *c = it.v[i].box;

		switch (it.v[i].kind) {
		case I_TEXT:
			place_text(&s, c);
			break;
		case I_OPEN:
			box_edges(c, w);
			if (s.nopen < 64) {
				s.open[s.nopen].box = c;
				s.open[s.nopen].x = s.pen;
				s.nopen++;
			}
			s.pen += c->m[3] + c->b[3] + c->p[3];
			if (c->m[3] + c->b[3] + c->p[3] > 0)
				s.content = 1;
			break;
		case I_CLOSE: {
			struct frag *fr;
			float e = c->p[1] + c->b[1] + c->m[1];

			need(&s, e, 0);
			s.pen += e;
			if (e > 0 || (!c->first && c->b[3] + c->p[3] > 0))
				s.content = 1;
			if (s.nopen && s.open[s.nopen - 1].box == c) {
				fr = new_frag(b, F_INLINE, c);
				fr->x = s.open[s.nopen - 1].x;
				fr->w = s.pen - fr->x;
				s.nopen--;
			}
			break;
		}
		case I_ATOMIC: {
			struct frag *fr;
			float mw;

			layout_atomic(c, w, cbh);
			mw = c->w + c->p[1] + c->p[3] + c->b[1] + c->b[3] +
				c->m[1] + c->m[3];
			need(&s, mw, 1);
			fr = new_frag(b, F_ATOMIC, c);
			fr->x = s.pen;
			fr->w = mw;
			fr->h = c->h + c->p[0] + c->p[2] + c->b[0] + c->b[2] +
				c->m[0] + c->m[2];
			/* baseline from the margin box top */
			if (c->type == B_ATOMIC && c->nlines &&
			    kw(c->style, P_OVERFLOW_Y, "visible"))
				fr->baseline = c->last_baseline;
			else if (c->type == B_ATOMIC && !c->nlines &&
				 c->first && c->first->nlines)
				fr->baseline = c->first->last_baseline;
			else
				fr->baseline = fr->h;
			s.pen += mw;
			s.content = 1;
			break;
		}
		case I_BREAK:
			finish_line(&s, 1, 1);
			break;
		case I_FLOAT: {
			float mn, mx;

			box_sizes(c, &mn, &mx);
			if (!s.content || s.pen + mx <= s.lr) {
				float old = s.lx, pen = s.pen;

				place_float(c, f, s.y, x, x + w, w);
				edges_at(&s);
				s.pen = pen + s.lx - old;
				if (s.lx != old) {
					/* a new left float pushes the line */
					int k;

					for (k = s.start; k < b->nfrags; k++)
						b->frags[k].x += s.lx - old;
					for (k = 0; k < s.nopen; k++)
						s.open[k].x += s.lx - old;
				}
			} else if (s.npending < 32) {
				s.pending[s.npending++] = c;
			}
			break;
		}
		case I_ABS:
			box_edges(c, w);
			abs_static(c, s.pen, s.y);
			break;
		case I_MARKER:
			place_marker(&s, c);
			break;
		}
	}
	if (s.content || b->nfrags > s.start)
		finish_line(&s, 0, 1);
	place_pending(&s);
	free(it.v);
	return b->nlines ? s.y - y : 0;
}

/* Intrinsic widths of an inline formatting context. */
void inline_sizes(struct box *b, float *mn, float *mx)
{
	struct items it = { NULL, 0, 0 };
	float line = 0, cmin, cmax;
	int i;

	collect(b, &it);
	process_items(&it);
	*mn = *mx = 0;
	for (i = 0; i < it.n; i++) {
		struct box *c = it.v[i].box;

		switch (it.v[i].kind) {
		case I_TEXT: {
			const struct style *st = c->style;
			size_t k = 0, j;

			while (k < c->len) {
				float w;

				if (c->text[k] == '\n') {
					if (line > *mx)
						*mx = line;
					line = 0;
					k++;
					continue;
				}
				for (j = k; j < c->len && c->text[j] != ' ' &&
				     c->text[j] != '\n'; j++)
					;
				if (nowrap(st))
					while (j < c->len && c->text[j] != '\n')
						j++;
				w = text_width(st, c->text + k, j - k);
				if (w > *mn)
					*mn = w;
				while (j < c->len && c->text[j] == ' ')
					j++;
				line += text_width(st, c->text + k, j - k);
				k = j;
			}
			break;
		}
		case I_OPEN:
		case I_CLOSE:
			box_edges(c, 0);
			line += it.v[i].kind == I_OPEN ?
				c->m[3] + c->b[3] + c->p[3] :
				c->m[1] + c->b[1] + c->p[1];
			break;
		case I_ATOMIC:
		case I_FLOAT:
			box_sizes(c, &cmin, &cmax);
			if (cmin > *mn)
				*mn = cmin;
			line += cmax;
			break;
		case I_BREAK:
			if (line > *mx)
				*mx = line;
			line = 0;
			break;
		}
	}
	if (line > *mx)
		*mx = line;
	free(it.v);
}
