/*
 * <video> and <audio>: players from the media extension, their frames
 * and controls painted by the engine, their events sent to scripts.
 *
 * A player opens when the element is first played (a click, a script,
 * autoplay when the host allows it), never just because the element
 * is on the page: on old machines a page full of videos stays a page.
 * A script that attaches a MediaSource opens one at once: it feeds it.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"
#include "css.h"
#include "image.h"
#include "soie/ext.h"

#define BAR_H 32		/* the controls' height */

/* The host's request function, with the page as first party: what
 * the player fetches is the page's subresource (cookies, blocking). */
struct preq {
	soie_request_fn fn;
	void *data;
	char *first_party;
};

static int page_request(void *data, const soie_request *req,
	soie_response *res)
{
	const struct preq *q = data;
	soie_request r = *req;

	r.first_party = q->first_party;
	return q->fn(q->data, &r, res);
}

struct player {
	struct preq *req;	/* NULL: fed by a script */
	soie_node *el;
	const struct soie_ext_media *ext;
	void *p;
	soie_media_state last;	/* for events and repaints */
	double reported;	/* time of the last timeupdate */
};

struct media {
	struct player *v;
	int n, cap;
};

static int is_media(const soie_node *el)
{
	return el->type == SOIE_ELEMENT && el->ns == SOIE_NS_HTML &&
		(!strcmp(el->name, "video") || !strcmp(el->name, "audio"));
}

static struct player *find(const soie_node *el)
{
	struct media *m = el->doc->u.doc.media;
	int i;

	for (i = 0; m && i < m->n; i++)
		if (m->v[i].el == el)
			return &m->v[i];
	return NULL;
}

int soie_media_can_play(const char *type)
{
	const struct soie_ext_media *e;
	int i;

	for (i = 0; (e = (const void *)soie_ext_get(SOIE_EXT_MEDIA, i)); i++)
		if (e->can_play && e->can_play(e->base.data, type))
			return 1;
	return 0;
}

/* The source: src, else the first <source> whose type may play. */
static const char *source_of(const soie_node *el)
{
	const char *src = soie_element_get_attr(el, "src"), *t;
	const soie_node *c;

	if (src && *src)
		return src;
	for (c = el->first; c; c = c->next) {
		if (c->type != SOIE_ELEMENT || c->ns != SOIE_NS_HTML ||
		    strcmp(c->name, "source"))
			continue;
		src = soie_element_get_attr(c, "src");
		t = soie_element_get_attr(c, "type");
		if (src && *src && (!t || soie_media_can_play(t)))
			return src;
	}
	return NULL;
}

/* el's player h, from extension e, in its document's list */
static struct player *add_player(soie_node *el,
	const struct soie_ext_media *e, void *h, struct preq *q)
{
	soie_node *doc = el->doc;
	struct media *m;
	struct player *p;

	m = doc->u.doc.media;
	if (!m) {
		m = xmalloc(sizeof(*m));
		memset(m, 0, sizeof(*m));
		doc->u.doc.media = m;
	}
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 4;
		m->v = xrealloc(m->v, (size_t)m->cap * sizeof(*m->v));
	}
	p = &m->v[m->n++];
	memset(p, 0, sizeof(*p));
	p->req = q;
	p->el = el;
	p->ext = e;
	p->p = h;
	p->last.paused = 1;
	p->ext->volume(p->p, 1, soie_element_get_attr(el, "muted") != NULL);
	return p;
}

static struct player *open_player(soie_node *el)
{
	soie_node *doc = el->doc;
	const struct soie_ext_media *e;
	const soie_style_options *o = style_saved_options(doc);
	const char *src = source_of(el);
	struct preq *q;
	char *base, *url;
	void *h = NULL;
	int i;

	if (!src || !o || !o->request)
		return NULL;
	base = document_base(doc, o->base_url);
	url = soie_url_resolve(base, src);
	free(base);
	q = xmalloc(sizeof(*q));
	q->fn = o->request;
	q->data = o->request_data;
	q->first_party = xstrndup(o->base_url ? o->base_url : "about:blank",
		strlen(o->base_url ? o->base_url : "about:blank"));
	for (i = 0; !h && (e = (const void *)soie_ext_get(SOIE_EXT_MEDIA,
	     i)); i++)
		if (e->open)
			h = e->open(e->base.data, url, page_request, q);
	free(url);
	if (!h) {
		free(q->first_party);
		free(q);
		return NULL;
	}
	return add_player(el, (const struct soie_ext_media *)
		soie_ext_get(SOIE_EXT_MEDIA, i - 1), h, q);
}

static void close_player(struct player *p)
{
	/* close joins the player's threads: then its requests end */
	p->ext->close(p->p);
	if (p->req) {
		free(p->req->first_party);
		free(p->req);
	}
}

void media_free(soie_node *doc)
{
	struct media *m = doc->u.doc.media;
	int i;

	if (!m)
		return;
	for (i = 0; i < m->n; i++)
		close_player(&m->v[i]);
	free(m->v);
	free(m);
	doc->u.doc.media = NULL;
}

/* el leaves its document: its player, if any, stops. */
void media_forget(soie_node *el)
{
	struct media *m = el->doc->u.doc.media;
	int i;

	for (i = 0; m && i < m->n; i++)
		if (m->v[i].el == el) {
			close_player(&m->v[i]);
			memmove(&m->v[i], &m->v[i + 1], (size_t)(m->n - i - 1) *
				sizeof(*m->v));
			m->n--;
			return;
		}
}

int soie_media_control(soie_node *el, int op, double value)
{
	struct player *p;
	soie_media_state st;

	if (!is_media(el))
		return -1;
	if (op == SOIE_MEDIA_CLOSE) {
		media_forget(el);
		return 0;
	}
	p = find(el);
	if (!p && (op == SOIE_MEDIA_PLAY || op == SOIE_MEDIA_TOGGLE))
		p = open_player(el);
	if (!p)
		return -1;
	switch (op) {
	case SOIE_MEDIA_TOGGLE:
		p->ext->state(p->p, &st);
		if (st.paused || st.ended)
			p->ext->play(p->p);
		else
			p->ext->pause(p->p);
		break;
	case SOIE_MEDIA_PLAY:
		p->ext->play(p->p);
		break;
	case SOIE_MEDIA_PAUSE:
		p->ext->pause(p->p);
		break;
	case SOIE_MEDIA_SEEK:
		p->ext->seek(p->p, value);
		break;
	case SOIE_MEDIA_VOLUME:
		p->ext->volume(p->p, value, soie_element_get_attr(el, "muted") !=
			NULL);
		break;
	case SOIE_MEDIA_MUTE:
		p->ext->volume(p->p, 1, value != 0);
		break;
	}
	return 0;
}

int soie_media_get_state(soie_node *el, soie_media_state *st)
{
	struct player *p = is_media(el) ? find(el) : NULL;

	if (!p)
		return -1;
	memset(st, 0, sizeof(*st));
	p->ext->state(p->p, st);
	return 0;
}

/* Media Source Extensions */

#define SOURCE_HOOK(e, hook) SOIE_EXT_HOOK(e, struct soie_ext_media, hook)

int soie_media_source_supported(const char *type)
{
	const struct soie_ext_media *e;
	int (*can)(void *, const char *);
	int i;

	for (i = 0; (e = (const void *)soie_ext_get(SOIE_EXT_MEDIA, i)); i++)
		if ((can = SOURCE_HOOK(e, can_source)) &&
		    SOURCE_HOOK(e, source) && can(e->base.data, type))
			return 1;
	return 0;
}

int soie_media_source_open(soie_node *el)
{
	const struct soie_ext_media *e;
	void *(*open)(void *);
	void *h = NULL;
	int i;

	if (!is_media(el))
		return -1;
	media_forget(el);
	for (i = 0; !h && (e = (const void *)soie_ext_get(SOIE_EXT_MEDIA,
	     i)); i++)
		if ((open = SOURCE_HOOK(e, open_source)) && SOURCE_HOOK(e, source))
			h = open(e->base.data);
	if (!h)
		return -1;
	add_player(el, (const struct soie_ext_media *)
		soie_ext_get(SOIE_EXT_MEDIA, i - 1), h, NULL);
	return 0;
}

/* el's player when a script feeds it */
static struct player *source_of_el(const soie_node *el)
{
	struct player *p = is_media(el) ? find(el) : NULL;

	return p && !p->req ? p : NULL;
}

int soie_media_source_add(soie_node *el, const char *type)
{
	struct player *p = source_of_el(el);

	return p ? p->ext->add_buffer(p->p, type) : -1;
}

int soie_media_source_append(soie_node *el, int id, const void *bytes,
	size_t len, double offset, double start, double end)
{
	struct player *p = source_of_el(el);

	if (!p)
		return SOIE_SOURCE_ERROR;
	return p->ext->append(p->p, id, bytes, len, offset, start, end);
}

int soie_media_source_buffered(soie_node *el, int id, double *ranges,
	int max)
{
	struct player *p = source_of_el(el);

	return p ? p->ext->buffered(p->p, id, ranges, max) : 0;
}

int soie_media_source_control(soie_node *el, int op, int id, double a,
	double b)
{
	struct player *p = source_of_el(el);

	if (!p)
		return -1;
	p->ext->source(p->p, op, id, a, b);
	return 0;
}

int soie_media_click(soie_node *el, float x, float y)
{
	float bx, by, bw, bh;
	soie_media_state st;
	struct player *p;

	if (!is_media(el) || soie_element_rect(el, &bx, &by, &bw, &bh))
		return 0;
	p = find(el);
	if (soie_element_get_attr(el, "controls") && y >= by + bh - BAR_H &&
	    x > bx + BAR_H && p) {
		/* the seek bar */
		p->ext->state(p->p, &st);
		if (st.duration > 0 && bw > BAR_H + 16) {
			double f = (x - bx - BAR_H) / (bw - BAR_H - 8);

			f = f < 0 ? 0 : f > 1 ? 1 : f;
			p->ext->seek(p->p, f * st.duration);
		}
		return 1;
	}
	return soie_media_control(el, SOIE_MEDIA_TOGGLE, 0) == 0;
}

/* Events wait for the end of the tick: their listeners may open or
 * close players, the list changes under them. */
struct events {
	struct { soie_node *el; const char *type; } *v;
	int n, cap;
};

static void fire(struct events *q, soie_node *el, const char *type)
{
	if (q->n == q->cap) {
		q->cap = q->cap ? q->cap * 2 : 16;
		q->v = xrealloc(q->v, (size_t)q->cap * sizeof(*q->v));
	}
	q->v[q->n].el = el;
	q->v[q->n++].type = type;
}

/* Events and repaints from state changes. */
int soie_media_tick(soie_node *doc, int *repaint)
{
	struct media *m = doc->u.doc.media;
	struct events q = { NULL, 0, 0 };
	soie_media_state st;
	soie_event ev;
	struct player *p;
	int i, next = -1;

	*repaint = 0;
	for (i = 0; m && i < m->n; i++) {
		p = &m->v[i];
		memset(&st, 0, sizeof(st));
		p->ext->state(p->p, &st);
		if (st.changes != p->last.changes)
			*repaint = 1;
		if ((st.width != p->last.width || st.height != p->last.height) &&
		    !strcmp(p->el->name, "video"))
			doc->u.doc.gen++;	/* a new intrinsic size */
		if (st.duration != p->last.duration && st.duration > 0) {
			fire(&q, p->el, "durationchange");
			if (p->last.duration <= 0) {
				fire(&q, p->el, "loadedmetadata");
				fire(&q, p->el, "loadeddata");
				fire(&q, p->el, "canplay");
				fire(&q, p->el, "canplaythrough");
			}
		}
		if (st.paused != p->last.paused)
			fire(&q, p->el, st.paused ? "pause" : "playing");
		else if (st.waiting != p->last.waiting && !st.paused)
			fire(&q, p->el, st.waiting ? "waiting" : "playing");
		if (st.seeks != p->last.seeks)
			fire(&q, p->el, "seeked");
		if (st.time < p->reported || st.time >= p->reported + 0.25) {
			p->reported = st.time;
			fire(&q, p->el, "timeupdate");
		}
		if (st.ended && !p->last.ended) {
			if (st.time != p->reported)
				fire(&q, p->el, "timeupdate");
			p->reported = st.time;
			fire(&q, p->el, "ended");
		}
		if (st.error && !p->last.error)
			fire(&q, p->el, "error");
		p->last = st;
		if (!st.paused && !st.ended && !st.error)
			next = 20;
		else if (!p->req && !st.error && !st.ended &&
			 (next < 0 || next > 250))
			next = 250;	/* a script feeds it: data comes */
	}
	for (i = 0; i < q.n; i++) {
		memset(&ev, 0, sizeof(ev));
		ev.type = q.v[i].type;
		soie_script_event(doc, q.v[i].el, &ev);
	}
	free(q.v);
	return next;
}

/* Painting */

void media_size(const soie_node *el, float *w, float *h)
{
	struct player *p = find(el);
	const struct image *im;
	const char *poster;

	if (p && p->last.width > 0) {
		*w = (float)p->last.width;
		*h = (float)p->last.height;
	} else if ((poster = soie_element_get_attr(el, "poster")) &&
		   (im = image_get(el->doc, poster))) {
		*w = (float)im->w;
		*h = (float)im->h;
	}
}

static void triangle(struct canvas *c, float x, float y, float size,
	const unsigned char *rgba)
{
	float i;

	for (i = 0; i < size; i++) {
		float d = i < size / 2 ? i : size - i;

		fill_rect(c, x, y + i, d * 1.6f, 1, rgba);
	}
}

static void play_icon(struct canvas *c, float x, float y, float size,
	int paused)
{
	static const unsigned char white[4] = { 255, 255, 255, 230 };

	if (paused) {
		triangle(c, x + size * 0.3f, y + size * 0.2f, size * 0.6f,
			white);
	} else {
		fill_rect(c, x + size * 0.28f, y + size * 0.25f, size * 0.15f,
			size * 0.5f, white);
		fill_rect(c, x + size * 0.57f, y + size * 0.25f, size * 0.15f,
			size * 0.5f, white);
	}
}

void paint_media(struct canvas *c, const struct box *b)
{
	static const unsigned char black[4] = { 0, 0, 0, 255 };
	static const unsigned char shade[4] = { 0, 0, 0, 150 };
	static const unsigned char track[4] = { 255, 255, 255, 90 };
	static const unsigned char done[4] = { 255, 255, 255, 230 };
	const soie_node *el = b->node;
	struct player *p = find(el);
	soie_media_state st;
	struct image frame;
	const char *poster;
	const struct image *im = NULL;
	int video = !strcmp(el->name, "video"), shown = 0;
	float bar = b->h < BAR_H ? b->h : BAR_H;

	memset(&st, 0, sizeof(st));
	st.paused = 1;
	if (p) {
		memset(&st, 0, sizeof(st));
		p->ext->state(p->p, &st);
	}
	if (video) {
		memset(&frame, 0, sizeof(frame));
		if (p && p->ext->frame &&
		    (frame.rgba = (unsigned char *)p->ext->frame(p->p,
		     &frame.w, &frame.h)) && frame.w > 0 && frame.h > 0) {
			fill_rect(c, b->x, b->y, b->w, b->h, black);
			draw_fitted(c, b, &frame, NULL);
			shown = 1;
		} else if ((poster = soie_element_get_attr(el, "poster")) &&
			   (im = image_get(el->doc, poster))) {
			draw_fitted(c, b, im, NULL);
			shown = 1;
		}
		if (!shown)
			fill_rect(c, b->x, b->y, b->w, b->h, black);
		if (st.paused && !soie_element_get_attr(el, "controls")) {
			float s = b->w < b->h ? b->w : b->h;

			s = s > 64 ? 64 : s / 2;
			fill_rect(c, b->x + (b->w - s) / 2, b->y + (b->h - s) / 2,
				s, s, shade);
			play_icon(c, b->x + (b->w - s) / 2,
				b->y + (b->h - s) / 2, s, 1);
		}
	}
	if (!soie_element_get_attr(el, "controls"))
		return;
	/* the bar: play/pause, then the progress */
	{
		float y = b->y + b->h - bar, w = b->w - bar - 8;

		fill_rect(c, b->x, y, b->w, bar, video ? shade : black);
		play_icon(c, b->x, y, bar, st.paused || st.ended);
		if (w > 0) {
			fill_rect(c, b->x + bar, y + bar / 2 - 2, w, 4, track);
			if (st.duration > 0)
				fill_rect(c, b->x + bar, y + bar / 2 - 2,
					w * (float)(st.time / st.duration), 4,
					done);
		}
	}
}
