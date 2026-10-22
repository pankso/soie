/*
 * Running a document's scripts through the script extension: which
 * <script> elements run, in what order, fetched how. The engine
 * itself (QuickJS or another) lives in the extension.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"
#include "soie/ext.h"

struct script {
	const struct soie_ext_script *ext;
	void *ctx;
	soie_script_host host;
	char *url;
	soie_node *current;
	soie_node **later;	/* defer, async and module scripts */
	size_t nlater, caplater;
	int loading;		/* still in soie_script_start() */
};

static int is_script(const soie_node *n)
{
	return n->type == SOIE_ELEMENT && (n->ns == SOIE_NS_HTML ||
		n->ns == SOIE_NS_SVG) && !strcmp(n->name, "script");
}

/* 1 classic, 2 module, 0 not for us (JSON blocks, templates...) */
static int script_kind(const soie_node *el)
{
	static const char *const js[] = { "text/javascript",
		"application/javascript", "text/ecmascript",
		"application/ecmascript", "application/x-javascript",
		"application/x-ecmascript", "text/x-javascript",
		"text/x-ecmascript", "text/jscript", "text/livescript",
		"text/javascript1.0", "text/javascript1.1",
		"text/javascript1.2", "text/javascript1.3",
		"text/javascript1.4", "text/javascript1.5", NULL };
	const char *t = soie_element_get_attr(el, "type");
	const char *lang = soie_element_get_attr(el, "language");
	char buf[64];
	size_t n;
	int i;

	if (!t && lang && *lang) {
		snprintf(buf, sizeof(buf), "text/%s", lang);
		t = buf;
	} else if (!t || !*t) {
		return 1;
	}
	while (*t == ' ' || *t == '\t' || *t == '\n')
		t++;
	n = strlen(t);
	while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\n'))
		n--;
	if (n == 6 && !strncasecmp(t, "module", 6))
		return 2;
	for (i = 0; js[i]; i++)
		if (strlen(js[i]) == n && !strncasecmp(t, js[i], n))
			return 1;
	return 0;
}

static int connected(const soie_node *n)
{
	while (n->parent)
		n = n->parent;
	return n->type == SOIE_DOCUMENT;
}

static void fire(struct script *s, soie_node *target, const char *type)
{
	soie_event ev;

	if (!s->ext->event)
		return;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	s->ext->event(s->ctx, target, &ev);
}

static void execute(struct script *s, soie_node *el, int module)
{
	const char *src = soie_element_get_attr(el, "src");
	soie_node *prev = s->current;

	if (!s->ext->run)
		return;
	s->current = module ? NULL : el;	/* currentScript */
	if (src) {
		soie_request rq;
		soie_response res;
		char *base = document_base(el->doc, s->url);
		char *url = soie_url_resolve(base, src);

		free(base);
		memset(&rq, 0, sizeof(rq));
		rq.url = url;
		rq.first_party = s->url;
		if (*src && s->host.request &&
		    !s->host.request(s->host.data, &rq, &res) &&
		    res.status < 400) {
			struct buf b = { 0 };
			const char *cs;
			size_t bom;

			cs = charset_sniff((unsigned char *)res.body, res.len,
				NULL, &bom);
			charset_decode(cs, (unsigned char *)res.body + bom,
				res.len - bom, &b);
			soie_response_free(&res);
			s->ext->run(s->ctx, el, b.s ? b.s : "", b.len, url,
				module);
			buf_free(&b);
			fire(s, el, "load");
		} else {
			fire(s, el, "error");
		}
		free(url);
	} else {
		struct buf b = { 0 };
		const soie_node *c;

		for (c = el->first; c; c = c->next)
			if (c->type == SOIE_TEXT)
				buf_add(&b, c->u.text.data, c->u.text.len);
		s->ext->run(s->ctx, el, b.s ? b.s : "", b.len, s->url, module);
		buf_free(&b);
	}
	s->current = prev;
}

/* "Prepare the script element", without the parser's blocking. */
static int prepare(struct script *s, soie_node *el, int parser)
{
	int kind;

	if (el->started || !connected(el))
		return 0;
	kind = script_kind(el);
	if (!kind || (kind == 1 && soie_element_get_attr(el, "nomodule")))
		return 0;
	if (!soie_element_get_attr(el, "src") && !el->first)
		return 0;	/* empty: may get text later */
	el->started = 1;
	if (parser && s->loading && (kind == 2 ||
	    (soie_element_get_attr(el, "src") &&
	     (soie_element_get_attr(el, "defer") ||
	      soie_element_get_attr(el, "async"))))) {
		if (s->nlater == s->caplater) {
			s->caplater = s->caplater ? s->caplater * 2 : 16;
			s->later = xrealloc(s->later, s->caplater *
				sizeof(*s->later));
		}
		s->later[s->nlater++] = el;
		return 1;
	}
	execute(s, el, kind == 2);
	return 1;
}

int soie_script_start(soie_node *doc, const char *url,
	const soie_script_host *host)
{
	const struct soie_ext_script *e;
	struct script *s;
	soie_node **v = NULL, *n;
	size_t nv = 0, cap = 0, i;

	if (doc->type != SOIE_DOCUMENT || doc->u.doc.script)
		return -1;
	e = (const void *)soie_ext_get(SOIE_EXT_SCRIPT, 0);
	if (!e || !SOIE_EXT_HOOK(e, struct soie_ext_script, start))
		return -1;
	s = xmalloc(sizeof(*s));
	memset(s, 0, sizeof(*s));
	s->ext = e;
	s->host = *host;
	s->url = xstrndup(url ? url : "about:blank",
		strlen(url ? url : "about:blank"));
	s->ctx = e->start(e->base.data, doc, s->url, &s->host);
	if (!s->ctx) {
		free(s->url);
		free(s);
		return -1;
	}
	doc->u.doc.script = s;
	s->loading = 1;
	/* the parser's scripts, in tree order, as they are now */
	for (n = doc->first; n; ) {
		if (is_script(n)) {
			if (nv == cap) {
				cap = cap ? cap * 2 : 32;
				v = xrealloc(v, cap * sizeof(*v));
			}
			v[nv++] = n;
		}
		if (n->first) {
			n = n->first;
			continue;
		}
		while (n && !n->next)
			n = n->parent;
		if (n)
			n = n->next;
	}
	for (i = 0; i < nv && doc->u.doc.script == s; i++)
		prepare(s, v[i], 1);
	free(v);
	s->loading = 0;
	/* parsing is over: "interactive", then deferred scripts */
	if (doc->u.doc.script == s)
		fire(s, doc, "readystatechange");
	for (i = 0; i < s->nlater && doc->u.doc.script == s; i++)
		execute(s, s->later[i], script_kind(s->later[i]) == 2);
	free(s->later);
	s->later = NULL;
	s->nlater = s->caplater = 0;
	if (doc->u.doc.script == s) {
		fire(s, doc, "DOMContentLoaded");
		fire(s, NULL, "load");
	}
	return 0;
}

int soie_script_element(soie_node *el)
{
	struct script *s = el->doc->u.doc.script;

	if (!s || !is_script(el))
		return 0;
	return prepare(s, el, 0);
}

void soie_script_skip(soie_node *el)
{
	el->started = 1;
}

soie_node *soie_script_current(const soie_node *doc)
{
	return doc->u.doc.script ? doc->u.doc.script->current : NULL;
}

int soie_script_event(soie_node *doc, soie_node *target,
	const soie_event *ev)
{
	struct script *s = doc->u.doc.script;

	if (!s || !s->ext->event)
		return 0;
	return s->ext->event(s->ctx, target, ev) != 0;
}

int soie_script_tick(soie_node *doc)
{
	struct script *s = doc->u.doc.script;

	if (!s || !s->ext->tick)
		return -1;
	return s->ext->tick(s->ctx);
}

void soie_script_stop(soie_node *doc)
{
	struct script *s = doc->u.doc.script;

	if (!s)
		return;
	doc->u.doc.script = NULL;
	if (s->ext->stop)
		s->ext->stop(s->ctx);
	free(s->later);
	free(s->url);
	free(s);
}
