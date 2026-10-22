/*
 * The script extension: QuickJS, and the natives the DOM is built on.
 *
 * C gives a small set of functions over Soie nodes (tree walking,
 * attributes, mutation, queries, layout, the host's services) on an
 * internal object; dom.js, run first in every context, builds the web
 * platform (Node, Element, events, timers, fetch, XHR...) on them.
 * Each node a script touches gets one wrapper object, kept until the
 * page goes away, so expandos and listeners survive.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <quickjs.h>

#include <soie/soie.h>
#include <soie/ext.h>

#define RUN_MS 5000		/* longest a script may run at once */
#define MEMORY_LIMIT (256 << 20)

extern const char dom_js[];

struct sctx {
	JSRuntime *rt;
	JSContext *js;
	soie_node *doc;
	const soie_script_host *host;
	char *url;
	JSValue api;
	JSValue proto_for, dispatch, tick;
	soie_node **wrapped;
	size_t nwrapped, capwrapped;
	double deadline;
	soie_node *written;	/* document.write: the last inserted node */
	char **map_from, **map_to;	/* the import map */
	int nmap;
};

static JSClassID node_class;

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

static struct sctx *ctx_of(JSContext *js)
{
	return JS_GetContextOpaque(js);
}

static void message(struct sctx *c, int kind, const char *text)
{
	if (c->host->message)
		c->host->message(c->host->data, kind, text);
}

static void report(struct sctx *c)
{
	JSValue e = JS_GetException(c->js), stack;
	const char *msg = JS_ToCString(c->js, e), *st = NULL;
	char *line;

	if (JS_IsError(c->js, e)) {
		stack = JS_GetPropertyStr(c->js, e, "stack");
		if (!JS_IsUndefined(stack))
			st = JS_ToCString(c->js, stack);
		JS_FreeValue(c->js, stack);
	}
	line = malloc(strlen(msg ? msg : "?") + strlen(st ? st : "") + 2);
	if (line) {
		sprintf(line, "%s\n%s", msg ? msg : "?", st ? st : "");
		message(c, SOIE_MSG_ERROR, line);
		free(line);
	}
	JS_FreeCString(c->js, msg);
	if (st)
		JS_FreeCString(c->js, st);
	JS_FreeValue(c->js, e);
}

/* After every entry into JS: promise jobs, errors reported. */
static void settle(struct sctx *c, JSValue v)
{
	JSContext *js;
	int r;

	if (JS_IsException(v))
		report(c);
	JS_FreeValue(c->js, v);
	while ((r = JS_ExecutePendingJob(c->rt, &js)) != 0)
		if (r < 0)
			report(c);
}

static int interrupt(JSRuntime *rt, void *opaque)
{
	struct sctx *c = opaque;

	(void)rt;
	return c->deadline && now_ms() > c->deadline;
}

/* Wrappers */

static JSValue wrap(struct sctx *c, soie_node *n)
{
	JSValue *v, proto, obj, args[3];
	const char *name = "";

	if (!n)
		return JS_NULL;
	if ((v = soie_node_user(n)))
		return JS_DupValue(c->js, *v);
	if (soie_node_get_type(n) == SOIE_ELEMENT)
		name = soie_element_name(n);
	args[0] = JS_NewInt32(c->js, soie_node_get_type(n));
	args[1] = JS_NewString(c->js, name);
	args[2] = JS_NewInt32(c->js, soie_node_get_type(n) == SOIE_ELEMENT ?
		(int)soie_element_ns(n) : 0);
	proto = JS_Call(c->js, c->proto_for, JS_UNDEFINED, 3, args);
	JS_FreeValue(c->js, args[1]);
	if (JS_IsException(proto))
		return proto;
	obj = JS_NewObjectProtoClass(c->js, proto, node_class);
	JS_FreeValue(c->js, proto);
	if (JS_IsException(obj))
		return obj;
	JS_SetOpaque(obj, n);
	v = malloc(sizeof(*v));
	if (!v)
		abort();
	*v = JS_DupValue(c->js, obj);
	soie_node_set_user(n, v);
	if (c->nwrapped == c->capwrapped) {
		c->capwrapped = c->capwrapped ? c->capwrapped * 2 : 256;
		c->wrapped = realloc(c->wrapped, c->capwrapped *
			sizeof(*c->wrapped));
		if (!c->wrapped)
			abort();
	}
	c->wrapped[c->nwrapped++] = n;
	return obj;
}

static soie_node *unwrap(JSValueConst v)
{
	return JS_GetOpaque(v, node_class);
}

/* Argument helpers: natives are only called by dom.js, which checks
 * what users pass; a wrong node here is a TypeError, not a crash. */
#define NODE(i, n) \
	soie_node *n = argc > (i) ? unwrap(argv[i]) : NULL; \
	if (!n) \
		return JS_ThrowTypeError(js, "not a node")
#define STR(i, s) \
	const char *s = argc > (i) ? JS_ToCString(js, argv[i]) : NULL; \
	if (!s) \
		return JS_EXCEPTION

static JSValue str_or_null(JSContext *js, const char *s)
{
	return s ? JS_NewString(js, s) : JS_NULL;
}

/* Natives: the tree */

static JSValue n_type(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	NODE(0, n);
	(void)this;
	return JS_NewInt32(js, soie_node_get_type(n));
}

static JSValue n_name(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	NODE(0, n);
	(void)this;
	switch (soie_node_get_type(n)) {
	case SOIE_ELEMENT:
		return JS_NewString(js, soie_element_name(n));
	case SOIE_DOCTYPE:
		return JS_NewString(js, soie_doctype_name(n));
	default:
		return JS_NewString(js, "");
	}
}

static JSValue n_ns(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	NODE(0, n);
	(void)this;
	return JS_NewInt32(js, soie_node_get_type(n) == SOIE_ELEMENT ?
		(int)soie_element_ns(n) : 0);
}

static JSValue n_rel(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv, int magic)
{
	soie_node *r = NULL;
	NODE(0, n);

	(void)this;
	switch (magic) {
	case 0: r = soie_node_parent(n); break;
	case 1: r = soie_node_first_child(n); break;
	case 2: r = soie_node_last_child(n); break;
	case 3: r = soie_node_next_sibling(n); break;
	case 4: r = soie_node_prev_sibling(n); break;
	case 5: r = soie_template_content(n); break;
	case 6: r = soie_document_element(n); break;
	}
	return wrap(ctx_of(js), r);
}

/* All the children at once: childNodes without a call per node. */
static JSValue n_children(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	JSValue a;
	soie_node *k;
	uint32_t i = 0;
	int elements = argc > 1 && JS_ToBool(js, argv[1]);
	NODE(0, n);

	(void)this;
	a = JS_NewArray(js);
	for (k = soie_node_first_child(n); k; k = soie_node_next_sibling(k))
		if (!elements || soie_node_get_type(k) == SOIE_ELEMENT)
			JS_SetPropertyUint32(js, a, i++, wrap(ctx_of(js), k));
	return a;
}

static JSValue n_data(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	const char *d;
	size_t len;
	NODE(0, n);

	(void)this;
	d = soie_node_data(n, &len);
	return d ? JS_NewStringLen(js, d, len) : JS_NULL;
}

/* textContent of an element or fragment, without wrapping its nodes */
static JSValue n_text(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	char *out = NULL;
	size_t len = 0, cap = 0, k;
	soie_node *top, *n;
	const char *d;
	JSValue r;
	NODE(0, t);

	(void)this;
	top = t;
	for (n = soie_node_first_child(top); n; ) {
		if (soie_node_get_type(n) == SOIE_TEXT) {
			d = soie_node_data(n, &k);
			if (len + k + 1 > cap) {
				cap = (len + k + 1) * 2;
				out = realloc(out, cap);
				if (!out)
					abort();
			}
			memcpy(out + len, d, k);
			len += k;
		}
		if (soie_node_get_type(n) == SOIE_ELEMENT &&
		    soie_node_first_child(n)) {
			n = soie_node_first_child(n);
			continue;
		}
		while (n != top && !soie_node_next_sibling(n))
			n = soie_node_parent(n);
		n = n == top ? NULL : soie_node_next_sibling(n);
	}
	r = JS_NewStringLen(js, out ? out : "", len);
	free(out);
	return r;
}

static JSValue n_set_data(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	size_t len;
	const char *s;
	NODE(0, n);

	(void)this;
	s = argc > 1 ? JS_ToCStringLen(js, &len, argv[1]) : NULL;
	if (!s)
		return JS_EXCEPTION;
	soie_node_set_data(n, s, len);
	JS_FreeCString(js, s);
	return JS_UNDEFINED;
}

static JSValue n_attrs(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	JSValue a, e;
	size_t i, k;
	NODE(0, n);

	(void)this;
	a = JS_NewArray(js);
	if (soie_node_get_type(n) != SOIE_ELEMENT)
		return a;
	k = soie_element_attr_count(n);
	for (i = 0; i < k; i++) {
		e = JS_NewArray(js);
		JS_SetPropertyUint32(js, e, 0,
			JS_NewString(js, soie_element_attr_name(n, i)));
		JS_SetPropertyUint32(js, e, 1,
			JS_NewString(js, soie_element_attr_value(n, i)));
		JS_SetPropertyUint32(js, e, 2,
			JS_NewInt32(js, soie_element_attr_ns(n, i)));
		JS_SetPropertyUint32(js, a, (uint32_t)i, e);
	}
	return a;
}

static JSValue n_get_attr(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	JSValue r;
	NODE(0, n);
	STR(1, name);

	(void)this;
	r = soie_node_get_type(n) == SOIE_ELEMENT ?
		str_or_null(js, soie_element_get_attr(n, name)) : JS_NULL;
	JS_FreeCString(js, name);
	return r;
}

static JSValue n_set_attr(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	const char *value;
	NODE(0, n);
	STR(1, name);

	(void)this;
	value = argc > 2 ? JS_ToCString(js, argv[2]) : NULL;
	if (!value) {
		JS_FreeCString(js, name);
		return JS_EXCEPTION;
	}
	soie_element_set_attr(n, name, value);
	JS_FreeCString(js, name);
	JS_FreeCString(js, value);
	return JS_UNDEFINED;
}

static JSValue n_remove_attr(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	NODE(0, n);
	STR(1, name);

	(void)this;
	soie_element_remove_attr(n, name);
	JS_FreeCString(js, name);
	return JS_UNDEFINED;
}

static int connected(soie_node *n)
{
	while (soie_node_parent(n))
		n = soie_node_parent(n);
	return soie_node_get_type(n) == SOIE_DOCUMENT;
}

/* Scripts inside nodes just inserted run, in tree order: inline ones
 * now, external ones from a task (api.later) unless sync, as
 * document.write's do. */
static void run_inserted(struct sctx *c, soie_node **v, size_t n, int sync)
{
	JSValue later = JS_GetPropertyStr(c->js, c->api, "later"), el, r;

	soie_node *k, *top;
	size_t i;

	for (i = 0; i < n; i++) {
		if (!connected(v[i]))
			continue;
		for (k = top = v[i]; k; ) {
			if (soie_node_get_type(k) == SOIE_ELEMENT &&
			    !strcmp(soie_element_name(k), "script")) {
				if (sync || !soie_element_get_attr(k, "src")) {
					soie_script_element(k);
				} else {
					el = wrap(c, k);
					r = JS_Call(c->js, later, JS_UNDEFINED, 1,
						&el);
					JS_FreeValue(c->js, r);
					JS_FreeValue(c->js, el);
				}
			}
			if (soie_node_first_child(k)) {
				k = soie_node_first_child(k);
				continue;
			}
			while (k != top && !soie_node_next_sibling(k))
				k = soie_node_parent(k);
			k = k == top ? NULL : soie_node_next_sibling(k);
		}
	}
	JS_FreeValue(c->js, later);
}

static JSValue n_run_script(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	NODE(0, el);

	(void)this;
	soie_script_element(el);
	return JS_UNDEFINED;
}

static JSValue n_insert(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	soie_node *ref = argc > 2 ? unwrap(argv[2]) : NULL, **v, *k;
	size_t nv = 0;
	int r;
	NODE(0, parent);
	NODE(1, child);

	(void)this;
	/* what gets inserted: the child, or a fragment's children */
	if (soie_node_get_type(child) == SOIE_DOCUMENT_FRAGMENT) {
		for (k = soie_node_first_child(child); k;
		     k = soie_node_next_sibling(k))
			nv++;
	} else {
		nv = 1;
	}
	v = malloc((nv + 1) * sizeof(*v));
	if (!v)
		abort();
	if (soie_node_get_type(child) == SOIE_DOCUMENT_FRAGMENT) {
		nv = 0;
		for (k = soie_node_first_child(child); k;
		     k = soie_node_next_sibling(k))
			v[nv++] = k;
	} else {
		v[0] = child;
	}
	r = soie_node_insert_before(parent, child, ref);
	if (!r)
		run_inserted(ctx_of(js), v, nv, 0);
	free(v);
	return JS_NewBool(js, r == 0);
}

static JSValue n_remove(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	NODE(0, n);

	(void)this;
	soie_node_remove(n);
	return JS_UNDEFINED;
}

static JSValue n_create(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv, int magic)
{
	struct sctx *c = ctx_of(js);
	soie_node *n = NULL;
	size_t len = 0;
	const char *s = NULL;
	int32_t ns = 0;

	(void)this;
	if (magic == 0) {
		if (argc < 2 || JS_ToInt32(js, &ns, argv[0]))
			return JS_EXCEPTION;
		s = JS_ToCString(js, argv[1]);
		if (!s)
			return JS_EXCEPTION;
		n = soie_element_new(c->doc, (soie_ns)ns, s);
	} else if (magic == 1 || magic == 2) {
		s = argc > 0 ? JS_ToCStringLen(js, &len, argv[0]) : NULL;
		if (!s)
			return JS_EXCEPTION;
		n = magic == 1 ? soie_text_new(c->doc, s, len) :
			soie_comment_new(c->doc, s, len);
	} else {
		n = soie_document_fragment_new(c->doc);
	}
	if (s)
		JS_FreeCString(js, s);
	return wrap(c, n);
}

static JSValue n_clone(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	soie_node *k;
	NODE(0, n);

	(void)this;
	if (soie_node_get_type(n) == SOIE_DOCUMENT)
		return JS_ThrowTypeError(js, "cannot clone a document");
	k = soie_node_clone(n, argc > 1 && JS_ToBool(js, argv[1]));
	return wrap(ctx_of(js), k);
}

static JSValue n_serialize(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	JSValue r;
	size_t len;
	char *s;
	NODE(0, n);

	(void)this;
	s = soie_node_serialize(n, argc > 1 && JS_ToBool(js, argv[1]), &len);
	r = JS_NewStringLen(js, s, len);
	free(s);
	return r;
}

static void skip_scripts(soie_node *top)
{
	soie_node *k;

	for (k = soie_node_first_child(top); k; ) {
		if (soie_node_get_type(k) == SOIE_ELEMENT &&
		    !strcmp(soie_element_name(k), "script"))
			soie_script_skip(k);
		if (soie_node_first_child(k)) {
			k = soie_node_first_child(k);
			continue;
		}
		while (k != top && !soie_node_next_sibling(k))
			k = soie_node_parent(k);
		k = k == top ? NULL : soie_node_next_sibling(k);
	}
}

static soie_node *parse_fragment(struct sctx *c, soie_node *context,
	const char *html, size_t len)
{
	soie_html_options o = { "UTF-8", 1 };
	soie_ns ns = SOIE_NS_HTML;
	const char *name = "body";

	if (context && soie_node_get_type(context) == SOIE_ELEMENT) {
		ns = soie_element_ns(context);
		name = soie_element_name(context);
	}
	return soie_html_parse_fragment(c->doc, ns, name, html, len, &o);
}

/* innerHTML and friends: scripts in the markup never run. */
static JSValue n_parse(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);
	soie_node *ctxnode = argc > 0 ? unwrap(argv[0]) : NULL, *f;
	size_t len;
	const char *s = argc > 1 ? JS_ToCStringLen(js, &len, argv[1]) : NULL;

	(void)this;
	if (!s)
		return JS_EXCEPTION;
	f = parse_fragment(c, ctxnode, s, len);
	JS_FreeCString(js, s);
	skip_scripts(f);
	return wrap(c, f);
}

/* document.write(): after the running script, whose scripts run. */
static JSValue n_write(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);
	soie_node *cur = soie_script_current(c->doc), *parent, *f, *ref, **v;
	soie_node *k;
	size_t len, nv = 0;
	const char *s = argc > 0 ? JS_ToCStringLen(js, &len, argv[0]) : NULL;

	(void)this;
	if (!s)
		return JS_EXCEPTION;
	if (cur && soie_node_parent(cur)) {
		parent = soie_node_parent(cur);
		if (!c->written || soie_node_parent(c->written) != parent)
			c->written = cur;
	} else {
		/* after loading: at the end of <body> */
		parent = soie_document_element(c->doc);
		for (k = parent ? soie_node_last_child(parent) : NULL; k;
		     k = soie_node_prev_sibling(k))
			if (soie_node_get_type(k) == SOIE_ELEMENT &&
			    !strcmp(soie_element_name(k), "body"))
				break;
		parent = k ? k : parent;
		c->written = parent ? soie_node_last_child(parent) : NULL;
	}
	if (!parent) {
		JS_FreeCString(js, s);
		return JS_UNDEFINED;
	}
	f = parse_fragment(c, parent, s, len);
	JS_FreeCString(js, s);
	for (k = soie_node_first_child(f); k; k = soie_node_next_sibling(k))
		nv++;
	v = malloc((nv + 1) * sizeof(*v));
	if (!v)
		abort();
	nv = 0;
	for (k = soie_node_first_child(f); k; k = soie_node_next_sibling(k))
		v[nv++] = k;
	ref = c->written ? soie_node_next_sibling(c->written) :
		soie_node_first_child(parent);
	soie_node_insert_before(parent, f, ref);
	soie_node_free(f);
	if (nv)
		c->written = v[nv - 1];
	run_inserted(ctx_of(js), v, nv, 1);
	free(v);
	return JS_UNDEFINED;
}

static JSValue n_matches(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	int m;
	NODE(0, n);
	STR(1, sel);

	(void)this;
	m = soie_element_matches(n, sel);
	JS_FreeCString(js, sel);
	if (m < 0)
		return JS_Throw(js, JS_NewError(js));
	return JS_NewBool(js, m);
}

static JSValue n_query(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	soie_node **v;
	size_t n, i;
	int32_t max = 0;
	JSValue a;
	NODE(0, root);
	STR(1, sel);

	(void)this;
	if (argc > 2)
		JS_ToInt32(js, &max, argv[2]);
	v = soie_query(root, sel, max > 0 ? (size_t)max : 0, &n);
	JS_FreeCString(js, sel);
	if (!v)
		return JS_Throw(js, JS_NewError(js));
	a = JS_NewArray(js);
	for (i = 0; i < n; i++)
		JS_SetPropertyUint32(js, a, (uint32_t)i, wrap(ctx_of(js), v[i]));
	free(v);
	return a;
}

/* Natives: layout */

static JSValue n_rect(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	float r[4];
	JSValue a;
	int i;
	NODE(0, n);

	(void)this;
	soie_document_update(ctx_of(js)->doc);
	if (soie_element_rect(n, &r[0], &r[1], &r[2], &r[3]))
		return JS_NULL;
	a = JS_NewArray(js);
	for (i = 0; i < 4; i++)
		JS_SetPropertyUint32(js, a, (uint32_t)i,
			JS_NewFloat64(js, r[i]));
	return a;
}

static JSValue n_style(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	const char *pseudo = NULL;
	JSValue r;
	char *v;
	NODE(0, n);
	STR(1, prop);

	(void)this;
	if (argc > 2 && JS_IsString(argv[2]))
		pseudo = JS_ToCString(js, argv[2]);
	soie_document_update(ctx_of(js)->doc);
	v = soie_computed_value(n, pseudo, prop);
	r = v ? JS_NewString(js, v) : JS_NewString(js, "");
	free(v);
	JS_FreeCString(js, prop);
	if (pseudo)
		JS_FreeCString(js, pseudo);
	return r;
}

static JSValue n_page_height(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);

	(void)this;
	(void)argc;
	(void)argv;
	soie_document_update(c->doc);
	return JS_NewFloat64(js, soie_layout_height(c->doc));
}

static JSValue n_viewport(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);
	int w = 1024, h = 768;
	float sx = 0, sy = 0;
	JSValue a;

	(void)this;
	(void)argc;
	(void)argv;
	if (c->host->viewport)
		c->host->viewport(c->host->data, &w, &h, &sx, &sy);
	a = JS_NewArray(js);
	JS_SetPropertyUint32(js, a, 0, JS_NewInt32(js, w));
	JS_SetPropertyUint32(js, a, 1, JS_NewInt32(js, h));
	JS_SetPropertyUint32(js, a, 2, JS_NewFloat64(js, sx));
	JS_SetPropertyUint32(js, a, 3, JS_NewFloat64(js, sy));
	return a;
}

static JSValue n_scroll(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);
	double x = 0, y = 0;

	(void)this;
	if (argc > 1) {
		JS_ToFloat64(js, &x, argv[0]);
		JS_ToFloat64(js, &y, argv[1]);
	}
	if (c->host->scroll)
		c->host->scroll(c->host->data, (float)x, (float)y);
	return JS_UNDEFINED;
}

static JSValue n_media(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	int m;
	STR(0, q);

	(void)this;
	m = soie_media_matches(ctx_of(js)->doc, q);
	JS_FreeCString(js, q);
	return JS_NewBool(js, m);
}

/* Natives: the host */

static JSValue n_resolve(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	JSValue r;
	char *u;
	STR(0, base);
	STR(1, ref);

	(void)this;
	u = soie_url_resolve(base, ref);
	r = str_or_null(js, u);
	free(u);
	JS_FreeCString(js, base);
	JS_FreeCString(js, ref);
	return r;
}

/* request(method, url, headers, body) -> {status, url, headers, body} */
static JSValue n_request(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);
	soie_request rq;
	soie_response res;
	const char *body = NULL, *headers = NULL;
	size_t blen = 0;
	JSValue o = JS_NULL;
	STR(0, method);
	STR(1, url);

	(void)this;
	if (argc > 2 && JS_IsString(argv[2]))
		headers = JS_ToCString(js, argv[2]);
	if (argc > 3 && JS_IsString(argv[3]))
		body = JS_ToCStringLen(js, &blen, argv[3]);
	memset(&rq, 0, sizeof(rq));
	rq.url = url;
	rq.method = method;
	rq.headers = headers;
	rq.body = body;
	rq.body_len = blen;
	rq.first_party = c->url;
	if (c->host->request && !c->host->request(c->host->data, &rq, &res)) {
		o = JS_NewObject(js);
		JS_SetPropertyStr(js, o, "status", JS_NewInt32(js, res.status));
		JS_SetPropertyStr(js, o, "url",
			JS_NewString(js, res.url ? res.url : url));
		JS_SetPropertyStr(js, o, "headers",
			JS_NewString(js, res.headers ? res.headers : ""));
		JS_SetPropertyStr(js, o, "body", JS_NewStringLen(js,
			res.body ? res.body : "", res.len));
		soie_response_free(&res);
	}
	JS_FreeCString(js, method);
	JS_FreeCString(js, url);
	if (headers)
		JS_FreeCString(js, headers);
	if (body)
		JS_FreeCString(js, body);
	return o;
}

static JSValue n_cookie(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);
	JSValue r;
	char *s;

	(void)this;
	if (argc > 0 && JS_IsString(argv[0])) {
		const char *v = JS_ToCString(js, argv[0]);

		if (!v)
			return JS_EXCEPTION;
		soie_cookies_set(c->url, c->url, v, 1);
		JS_FreeCString(js, v);
		return JS_UNDEFINED;
	}
	s = soie_cookies_get(c->url, c->url, 1);
	r = JS_NewString(js, s ? s : "");
	free(s);
	return r;
}

static JSValue n_navigate(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);
	STR(0, url);

	(void)this;
	if (c->host->navigate)
		c->host->navigate(c->host->data, url,
			argc > 1 && JS_ToBool(js, argv[1]));
	JS_FreeCString(js, url);
	return JS_UNDEFINED;
}

static JSValue n_message(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	int32_t kind = 0;
	STR(1, text);

	(void)this;
	JS_ToInt32(js, &kind, argv[0]);
	message(ctx_of(js), kind, text);
	JS_FreeCString(js, text);
	return JS_UNDEFINED;
}

static JSValue n_now(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	(void)this;
	(void)argc;
	(void)argv;
	return JS_NewFloat64(js, now_ms());
}

static JSValue n_current(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);

	(void)this;
	(void)argc;
	(void)argv;
	return wrap(c, soie_script_current(c->doc));
}

/* Audio and video elements */
static JSValue n_player(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	int32_t op = 0;
	double v = 0;
	NODE(0, el);

	(void)this;
	if (argc > 1)
		JS_ToInt32(js, &op, argv[1]);
	if (argc > 2)
		JS_ToFloat64(js, &v, argv[2]);
	return JS_NewBool(js, soie_media_control(el, op, v) == 0);
}

static JSValue n_player_state(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	soie_media_state st;
	JSValue o;
	NODE(0, el);

	(void)this;
	if (soie_media_get_state(el, &st))
		return JS_NULL;
	o = JS_NewObject(js);
	JS_SetPropertyStr(js, o, "duration", JS_NewFloat64(js, st.duration));
	JS_SetPropertyStr(js, o, "time", JS_NewFloat64(js, st.time));
	JS_SetPropertyStr(js, o, "paused", JS_NewBool(js, st.paused));
	JS_SetPropertyStr(js, o, "ended", JS_NewBool(js, st.ended));
	JS_SetPropertyStr(js, o, "width", JS_NewInt32(js, st.width));
	JS_SetPropertyStr(js, o, "height", JS_NewInt32(js, st.height));
	JS_SetPropertyStr(js, o, "error", JS_NewBool(js, st.error));
	return o;
}

static JSValue n_can_play(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	int r;
	STR(0, type);

	(void)this;
	r = soie_media_can_play(type);
	JS_FreeCString(js, type);
	return JS_NewBool(js, r);
}

/* Bytes from the kernel, for crypto.getRandomValues() */
static JSValue n_random(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	uint8_t buf[65536];
	int32_t n = 0;
	size_t got = 0;
	FILE *f;

	(void)this;
	if (argc < 1 || JS_ToInt32(js, &n, argv[0]) || n < 0 ||
	    n > (int32_t)sizeof(buf))
		return JS_ThrowRangeError(js, "bad length");
	f = fopen("/dev/urandom", "rb");
	if (f) {
		got = fread(buf, 1, (size_t)n, f);
		fclose(f);
	}
	if (got != (size_t)n)
		return JS_ThrowInternalError(js, "no random source");
	return JS_NewArrayBufferCopy(js, buf, (size_t)n);
}

/* An import map entry: a specifier, or a prefix ending with '/' */
static JSValue n_map_module(JSContext *js, JSValueConst this, int argc,
	JSValueConst *argv)
{
	struct sctx *c = ctx_of(js);
	STR(0, from);
	STR(1, to);

	(void)this;
	c->map_from = realloc(c->map_from, (size_t)(c->nmap + 1) *
		sizeof(char *));
	c->map_to = realloc(c->map_to, (size_t)(c->nmap + 1) *
		sizeof(char *));
	if (!c->map_from || !c->map_to)
		abort();
	c->map_from[c->nmap] = strdup(from);
	c->map_to[c->nmap] = strdup(to);
	c->nmap++;
	JS_FreeCString(js, from);
	JS_FreeCString(js, to);
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry natives[] = {
	JS_CFUNC_DEF("type", 1, n_type),
	JS_CFUNC_DEF("name", 1, n_name),
	JS_CFUNC_DEF("ns", 1, n_ns),
	JS_CFUNC_MAGIC_DEF("parent", 1, n_rel, 0),
	JS_CFUNC_MAGIC_DEF("first", 1, n_rel, 1),
	JS_CFUNC_MAGIC_DEF("last", 1, n_rel, 2),
	JS_CFUNC_MAGIC_DEF("next", 1, n_rel, 3),
	JS_CFUNC_MAGIC_DEF("prev", 1, n_rel, 4),
	JS_CFUNC_MAGIC_DEF("content", 1, n_rel, 5),
	JS_CFUNC_MAGIC_DEF("root", 1, n_rel, 6),
	JS_CFUNC_DEF("children", 2, n_children),
	JS_CFUNC_DEF("data", 1, n_data),
	JS_CFUNC_DEF("text", 1, n_text),
	JS_CFUNC_DEF("setData", 2, n_set_data),
	JS_CFUNC_DEF("attrs", 1, n_attrs),
	JS_CFUNC_DEF("getAttr", 2, n_get_attr),
	JS_CFUNC_DEF("setAttr", 3, n_set_attr),
	JS_CFUNC_DEF("removeAttr", 2, n_remove_attr),
	JS_CFUNC_DEF("insert", 3, n_insert),
	JS_CFUNC_DEF("runScript", 1, n_run_script),
	JS_CFUNC_DEF("remove", 1, n_remove),
	JS_CFUNC_MAGIC_DEF("createElement", 2, n_create, 0),
	JS_CFUNC_MAGIC_DEF("createText", 1, n_create, 1),
	JS_CFUNC_MAGIC_DEF("createComment", 1, n_create, 2),
	JS_CFUNC_MAGIC_DEF("createFragment", 0, n_create, 3),
	JS_CFUNC_DEF("clone", 2, n_clone),
	JS_CFUNC_DEF("serialize", 2, n_serialize),
	JS_CFUNC_DEF("parse", 2, n_parse),
	JS_CFUNC_DEF("write", 1, n_write),
	JS_CFUNC_DEF("matches", 2, n_matches),
	JS_CFUNC_DEF("query", 3, n_query),
	JS_CFUNC_DEF("rect", 1, n_rect),
	JS_CFUNC_DEF("style", 3, n_style),
	JS_CFUNC_DEF("pageHeight", 0, n_page_height),
	JS_CFUNC_DEF("viewport", 0, n_viewport),
	JS_CFUNC_DEF("scroll", 2, n_scroll),
	JS_CFUNC_DEF("media", 1, n_media),
	JS_CFUNC_DEF("resolve", 2, n_resolve),
	JS_CFUNC_DEF("request", 4, n_request),
	JS_CFUNC_DEF("cookie", 1, n_cookie),
	JS_CFUNC_DEF("navigate", 2, n_navigate),
	JS_CFUNC_DEF("message", 2, n_message),
	JS_CFUNC_DEF("now", 0, n_now),
	JS_CFUNC_DEF("currentScript", 0, n_current),
	JS_CFUNC_DEF("random", 1, n_random),
	JS_CFUNC_DEF("player", 3, n_player),
	JS_CFUNC_DEF("playerState", 1, n_player_state),
	JS_CFUNC_DEF("canPlay", 1, n_can_play),
	JS_CFUNC_DEF("mapModule", 2, n_map_module),
};

/* Modules: specifiers are URLs, fetched through the host. */

static char *mod_normalize(JSContext *js, const char *base, const char *name,
	void *opaque)
{
	struct sctx *c = opaque;
	char *u = NULL, *r;
	size_t len, best = 0;
	int i, k = -1;

	/* the import map: exact, else the longest "prefix/" */
	for (i = 0; i < c->nmap; i++) {
		len = strlen(c->map_from[i]);
		if (!strcmp(name, c->map_from[i])) {
			k = i;
			break;
		}
		if (len > best && c->map_from[i][len - 1] == '/' &&
		    !strncmp(name, c->map_from[i], len)) {
			best = len;
			k = i;
		}
	}
	if (k >= 0 && !strcmp(name, c->map_from[k])) {
		u = strdup(c->map_to[k]);
	} else if (k >= 0) {
		u = malloc(strlen(c->map_to[k]) + strlen(name) + 1);
		if (u)
			sprintf(u, "%s%s", c->map_to[k], name + best);
	} else {
		u = soie_url_resolve(base, name);
	}
	r = js_strdup(js, u ? u : name);
	free(u);
	return r;
}

static JSModuleDef *mod_load(JSContext *js, const char *name, void *opaque)
{
	struct sctx *c = opaque;
	soie_request rq;
	soie_response res;
	JSValue f;
	JSModuleDef *m;
	char *src;

	memset(&rq, 0, sizeof(rq));
	rq.url = name;
	rq.first_party = c->url;
	if (!c->host->request || c->host->request(c->host->data, &rq, &res) ||
	    res.status >= 400) {
		JS_ThrowReferenceError(js, "cannot load module %s", name);
		return NULL;
	}
	src = malloc(res.len + 1);
	if (!src)
		abort();
	memcpy(src, res.body, res.len);
	src[res.len] = 0;
	f = JS_Eval(js, src, res.len, name,
		JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	free(src);
	soie_response_free(&res);
	if (JS_IsException(f))
		return NULL;
	m = JS_VALUE_GET_PTR(f);
	JS_FreeValue(js, f);
	return m;
}

/* The hooks */

static void *start(void *data, soie_node *doc, const char *url,
	const soie_script_host *host)
{
	struct sctx *c = calloc(1, sizeof(*c));
	JSValue global, v;

	(void)data;
	if (!c)
		return NULL;
	c->doc = doc;
	c->host = host;
	c->url = strdup(url);
	c->rt = JS_NewRuntime();
	if (!c->url || !c->rt) {
		free(c->url);
		free(c);
		return NULL;
	}
	JS_SetMemoryLimit(c->rt, MEMORY_LIMIT);
	JS_SetInterruptHandler(c->rt, interrupt, c);
	JS_SetModuleLoaderFunc(c->rt, mod_normalize, mod_load, c);
	if (!node_class)
		JS_NewClassID(&node_class);
	{
		JSClassDef def = { "Node", NULL, NULL, NULL, NULL };

		JS_NewClass(c->rt, node_class, &def);
	}
	c->js = JS_NewContext(c->rt);
	JS_SetContextOpaque(c->js, c);
	c->api = JS_NewObject(c->js);
	JS_SetPropertyFunctionList(c->js, c->api, natives,
		sizeof(natives) / sizeof(natives[0]));
	JS_SetPropertyStr(c->js, c->api, "url", JS_NewString(c->js, url));
	global = JS_GetGlobalObject(c->js);
	JS_SetPropertyStr(c->js, global, "__soie", JS_DupValue(c->js,
		c->api));
	JS_FreeValue(c->js, global);
	c->deadline = now_ms() + RUN_MS;
	v = JS_Eval(c->js, dom_js, strlen(dom_js), "soie:dom.js",
		JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(v)) {
		report(c);
		JS_FreeValue(c->js, v);
		JS_FreeValue(c->js, c->api);
		JS_FreeContext(c->js);
		JS_FreeRuntime(c->rt);
		free(c->url);
		free(c);
		return NULL;
	}
	JS_FreeValue(c->js, v);
	c->proto_for = JS_GetPropertyStr(c->js, c->api, "protoFor");
	c->dispatch = JS_GetPropertyStr(c->js, c->api, "dispatch");
	c->tick = JS_GetPropertyStr(c->js, c->api, "tick");
	/* the document object */
	{
		JSValue set = JS_GetPropertyStr(c->js, c->api, "setDocument");
		JSValue d = wrap(c, doc);

		v = JS_Call(c->js, set, JS_UNDEFINED, 1, &d);
		settle(c, v);
		JS_FreeValue(c->js, d);
		JS_FreeValue(c->js, set);
	}
	c->deadline = 0;
	return c;
}

static void stop(void *ctx)
{
	struct sctx *c = ctx;
	JSValue *v;
	size_t i;

	for (i = 0; i < c->nwrapped; i++) {
		v = soie_node_user(c->wrapped[i]);
		soie_node_set_user(c->wrapped[i], NULL);
		JS_SetOpaque(*v, NULL);
		JS_FreeValue(c->js, *v);
		free(v);
	}
	free(c->wrapped);
	while (c->nmap--) {
		free(c->map_from[c->nmap]);
		free(c->map_to[c->nmap]);
	}
	free(c->map_from);
	free(c->map_to);
	JS_FreeValue(c->js, c->proto_for);
	JS_FreeValue(c->js, c->dispatch);
	JS_FreeValue(c->js, c->tick);
	JS_FreeValue(c->js, c->api);
	JS_FreeContext(c->js);
	JS_FreeRuntime(c->rt);
	free(c->url);
	free(c);
}

static void run(void *ctx, soie_node *el, const char *src, size_t len,
	const char *url, int module)
{
	struct sctx *c = ctx;
	char *code = malloc(len + 1);
	JSValue v;

	(void)el;
	if (!code)
		abort();
	memcpy(code, src, len);
	code[len] = 0;
	c->deadline = now_ms() + RUN_MS;
	c->written = NULL;
	v = JS_Eval(c->js, code, len, url, module ? JS_EVAL_TYPE_MODULE :
		JS_EVAL_TYPE_GLOBAL);
	free(code);
	settle(c, v);
	c->deadline = 0;
}

static int event(void *ctx, soie_node *target, const soie_event *ev)
{
	struct sctx *c = ctx;
	JSValue args[3], init, v;
	int prevented = 0;

	c->deadline = now_ms() + RUN_MS;
	init = JS_NewObject(c->js);
	JS_SetPropertyStr(c->js, init, "clientX", JS_NewFloat64(c->js, ev->x));
	JS_SetPropertyStr(c->js, init, "clientY", JS_NewFloat64(c->js, ev->y));
	JS_SetPropertyStr(c->js, init, "button", JS_NewInt32(c->js,
		ev->button));
	JS_SetPropertyStr(c->js, init, "mods", JS_NewInt32(c->js, ev->mods));
	if (ev->key)
		JS_SetPropertyStr(c->js, init, "key", JS_NewString(c->js,
			ev->key));
	args[0] = target ? wrap(c, target) : JS_NULL;
	args[1] = JS_NewString(c->js, ev->type);
	args[2] = init;
	v = JS_Call(c->js, c->dispatch, JS_UNDEFINED, 3, args);
	if (!JS_IsException(v))
		prevented = JS_ToBool(c->js, v);
	settle(c, v);
	JS_FreeValue(c->js, args[0]);
	JS_FreeValue(c->js, args[1]);
	JS_FreeValue(c->js, args[2]);
	c->deadline = 0;
	return prevented;
}

static int tick(void *ctx)
{
	struct sctx *c = ctx;
	JSValue v;
	double d = -1;

	c->deadline = now_ms() + RUN_MS;
	v = JS_Call(c->js, c->tick, JS_UNDEFINED, 0, NULL);
	if (!JS_IsException(v))
		JS_ToFloat64(c->js, &d, v);
	settle(c, v);
	c->deadline = 0;
	return d < 0 ? -1 : d > 1e9 ? 1000000000 : (int)d;
}

static struct soie_ext_script hooks = {
	{ SOIE_EXT_ABI, sizeof(hooks), SOIE_EXT_SCRIPT, "quickjs", NULL },
	start, stop, run, event, tick
};

int soie_ext_init(void)
{
	return soie_ext_register(&hooks.base);
}
