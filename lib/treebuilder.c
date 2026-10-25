/*
 * HTML tree construction, after the WHATWG algorithm (section 13.2.6),
 * and the parser entry points.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "parser.h"

enum {
	M_INITIAL,
	M_BEFORE_HTML,
	M_BEFORE_HEAD,
	M_IN_HEAD,
	M_IN_HEAD_NOSCRIPT,
	M_AFTER_HEAD,
	M_IN_BODY,
	M_TEXT,
	M_IN_TABLE,
	M_IN_TABLE_TEXT,
	M_IN_CAPTION,
	M_IN_COLUMN_GROUP,
	M_IN_TABLE_BODY,
	M_IN_ROW,
	M_IN_CELL,
	M_IN_TEMPLATE,
	M_AFTER_BODY,
	M_IN_FRAMESET,
	M_AFTER_FRAMESET,
	M_AFTER_AFTER_BODY,
	M_AFTER_AFTER_FRAMESET
};

/* Scopes of "has an element in scope" */
enum { SCOPE_DEFAULT, SCOPE_LIST_ITEM, SCOPE_BUTTON, SCOPE_TABLE };

/* Pseudo tag matching any of h1-h6 in scope checks. */
#define TAG_HEADING (-1)

static void process(struct parser *p, struct token *t);
static void m_in_head(struct parser *p, struct token *t);
static void m_in_body(struct parser *p, struct token *t);
static void m_in_table(struct parser *p, struct token *t);
static void m_in_table_body(struct parser *p, struct token *t);
static void m_in_row(struct parser *p, struct token *t);
static void m_in_template(struct parser *p, struct token *t);

/* Small predicates */

static int is_ws(int c)
{
	return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int strcaseeq(const char *a, const char *b)
{
	while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
		a++, b++;
	return lower((unsigned char)*a) == lower((unsigned char)*b);
}

static int is_start(const struct token *t, int tag)
{
	return t->type == TOK_START && t->tag == tag;
}

static int is_end(const struct token *t, int tag)
{
	return t->type == TOK_END && t->tag == tag;
}

static int tag_in(int tag, const short *list)
{
	for (; *list; list++)
		if (*list == tag)
			return 1;
	return 0;
}

#define IN(tag, ...) tag_in(tag, (const short[]){ __VA_ARGS__, 0 })

/* Character token of whitespace only (tokens are split by class). */
static int ws_chars(const struct token *t)
{
	return t->type == TOK_CHARS && is_ws((unsigned char)t->chars[0]);
}

static int nul_chars(const struct token *t)
{
	return t->type == TOK_CHARS && t->chars[0] == '\0';
}

static const char *tok_attr(const struct token *t, const char *name)
{
	int i;

	for (i = 0; i < t->nattrs; i++)
		if (!strcmp(t->attrs[i].name.s, name))
			return t->attrs[i].value.s;
	return NULL;
}

static int is_special(const soie_node *n)
{
	if (n->ns == SOIE_NS_HTML)
		return IN(n->tag, TAG_ADDRESS, TAG_APPLET, TAG_AREA,
			TAG_ARTICLE, TAG_ASIDE, TAG_BASE, TAG_BASEFONT,
			TAG_BGSOUND, TAG_BLOCKQUOTE, TAG_BODY, TAG_BR,
			TAG_BUTTON, TAG_CAPTION, TAG_CENTER, TAG_COL,
			TAG_COLGROUP, TAG_DD, TAG_DETAILS, TAG_DIR, TAG_DIV,
			TAG_DL, TAG_DT, TAG_EMBED, TAG_FIELDSET,
			TAG_FIGCAPTION, TAG_FIGURE, TAG_FOOTER, TAG_FORM,
			TAG_FRAME, TAG_FRAMESET, TAG_H1, TAG_H2, TAG_H3,
			TAG_H4, TAG_H5, TAG_H6, TAG_HEAD, TAG_HEADER,
			TAG_HGROUP, TAG_HR, TAG_HTML, TAG_IFRAME, TAG_IMG,
			TAG_INPUT, TAG_KEYGEN, TAG_LI, TAG_LINK, TAG_LISTING,
			TAG_MAIN, TAG_MARQUEE, TAG_MENU, TAG_META, TAG_NAV,
			TAG_NOEMBED, TAG_NOFRAMES, TAG_NOSCRIPT, TAG_OBJECT,
			TAG_OL, TAG_P, TAG_PARAM, TAG_PLAINTEXT, TAG_PRE,
			TAG_SCRIPT, TAG_SEARCH, TAG_SECTION, TAG_SELECT,
			TAG_SOURCE, TAG_STYLE, TAG_SUMMARY, TAG_TABLE,
			TAG_TBODY, TAG_TD, TAG_TEMPLATE, TAG_TEXTAREA,
			TAG_TFOOT, TAG_TH, TAG_THEAD, TAG_TITLE, TAG_TR,
			TAG_TRACK, TAG_UL, TAG_WBR, TAG_XMP);
	if (n->ns == SOIE_NS_MATHML)
		return IN(n->tag, TAG_MI, TAG_MO, TAG_MN, TAG_MS, TAG_MTEXT,
			TAG_ANNOTATION_XML);
	if (n->ns == SOIE_NS_SVG)
		return IN(n->tag, TAG_FOREIGNOBJECT, TAG_DESC, TAG_TITLE);
	return 0;
}

static int is_mathml_text_ip(const soie_node *n)
{
	return n->ns == SOIE_NS_MATHML &&
		IN(n->tag, TAG_MI, TAG_MO, TAG_MN, TAG_MS, TAG_MTEXT);
}

static int is_html_ip(const soie_node *n)
{
	if (n->ns == SOIE_NS_MATHML)
		return n->html_ip;
	return n->ns == SOIE_NS_SVG &&
		IN(n->tag, TAG_FOREIGNOBJECT, TAG_DESC, TAG_TITLE);
}

/* Stack of open elements */

static soie_node *current(struct parser *p)
{
	return p->nstack ? p->stack[p->nstack - 1] : NULL;
}

static soie_node *adjusted_current(struct parser *p)
{
	return p->context && p->nstack == 1 ? p->context : current(p);
}

/*
 * Every change to the stack goes through these functions, which keep
 * p->open up to date: scope checks for elements that are not open
 * then cost nothing, instead of a walk down a deep stack.
 */
static void count(struct parser *p, const soie_node *n, int d)
{
	if (n->ns == SOIE_NS_HTML)
		p->open[n->tag] += (unsigned)d;
}

static void push(struct parser *p, soie_node *n)
{
	if (p->nstack == p->capstack) {
		p->capstack = p->capstack ? p->capstack * 2 : 32;
		p->stack = xrealloc(p->stack, p->capstack * sizeof(*p->stack));
	}
	p->stack[p->nstack++] = n;
	count(p, n, 1);
}

static void pop(struct parser *p)
{
	if (p->nstack)
		count(p, p->stack[--p->nstack], -1);
}

/* Pop until only n elements are left. */
static void pop_to(struct parser *p, int n)
{
	while (p->nstack > n)
		pop(p);
}

static int stack_index(struct parser *p, const soie_node *n)
{
	int i;

	for (i = p->nstack - 1; i >= 0; i--)
		if (p->stack[i] == n)
			return i;
	return -1;
}

static void stack_remove_at(struct parser *p, int i)
{
	count(p, p->stack[i], -1);
	memmove(p->stack + i, p->stack + i + 1,
		(p->nstack - i - 1) * sizeof(*p->stack));
	p->nstack--;
}

static void stack_insert_at(struct parser *p, int i, soie_node *n)
{
	push(p, n);
	memmove(p->stack + i + 1, p->stack + i,
		(p->nstack - i - 1) * sizeof(*p->stack));
	p->stack[i] = n;
}

static void stack_replace_at(struct parser *p, int i, soie_node *n)
{
	count(p, p->stack[i], -1);
	p->stack[i] = n;
	count(p, n, 1);
}

static int has_template(struct parser *p)
{
	return p->open[TAG_TEMPLATE] != 0;
}

static int scope_boundary(const soie_node *n, int scope)
{
	if (scope == SCOPE_TABLE)
		return n->ns == SOIE_NS_HTML &&
			IN(n->tag, TAG_HTML, TAG_TABLE, TAG_TEMPLATE);
	if (n->ns == SOIE_NS_HTML) {
		if (IN(n->tag, TAG_APPLET, TAG_CAPTION, TAG_HTML, TAG_TABLE,
		       TAG_TD, TAG_TH, TAG_MARQUEE, TAG_OBJECT, TAG_SELECT,
		       TAG_TEMPLATE))
			return 1;
		if (scope == SCOPE_LIST_ITEM)
			return n->tag == TAG_OL || n->tag == TAG_UL;
		if (scope == SCOPE_BUTTON)
			return n->tag == TAG_BUTTON;
		return 0;
	}
	if (n->ns == SOIE_NS_MATHML)
		return IN(n->tag, TAG_MI, TAG_MO, TAG_MN, TAG_MS, TAG_MTEXT,
			TAG_ANNOTATION_XML);
	if (n->ns == SOIE_NS_SVG)
		return IN(n->tag, TAG_FOREIGNOBJECT, TAG_DESC, TAG_TITLE);
	return 0;
}

/* Has an HTML element with this tag (or this node, if el) in scope. */
static int in_scope(struct parser *p, int tag, const soie_node *el, int scope)
{
	int i;

	if (tag == TAG_HEADING ? !(p->open[TAG_H1] | p->open[TAG_H2] |
	    p->open[TAG_H3] | p->open[TAG_H4] | p->open[TAG_H5] |
	    p->open[TAG_H6]) : !el && !p->open[tag])
		return 0;
	for (i = p->nstack - 1; i >= 0; i--) {
		soie_node *n = p->stack[i];

		if (el ? n == el : n->ns == SOIE_NS_HTML &&
		    (tag == TAG_HEADING ? IN(n->tag, TAG_H1, TAG_H2, TAG_H3,
			TAG_H4, TAG_H5, TAG_H6) : n->tag == tag))
			return 1;
		if (scope_boundary(n, scope))
			return 0;
	}
	return 0;
}

static void generate_implied(struct parser *p, int except)
{
	soie_node *n;

	while ((n = current(p)) && n->ns == SOIE_NS_HTML &&
	       n->tag != except &&
	       IN(n->tag, TAG_DD, TAG_DT, TAG_LI, TAG_OPTGROUP, TAG_OPTION,
		  TAG_P, TAG_RB, TAG_RP, TAG_RT, TAG_RTC))
		pop(p);
}

static void generate_implied_thoroughly(struct parser *p)
{
	soie_node *n;

	while ((n = current(p)) && n->ns == SOIE_NS_HTML &&
	       IN(n->tag, TAG_CAPTION, TAG_COLGROUP, TAG_DD, TAG_DT, TAG_LI,
		  TAG_OPTGROUP, TAG_OPTION, TAG_P, TAG_RB, TAG_RP, TAG_RT,
		  TAG_RTC, TAG_TBODY, TAG_TD, TAG_TFOOT, TAG_TH, TAG_THEAD,
		  TAG_TR))
		pop(p);
}

/* Pop until an HTML element with this tag (or a heading) is popped. */
static void pop_until(struct parser *p, int tag)
{
	soie_node *n;

	while ((n = current(p))) {
		pop(p);
		if (n->ns == SOIE_NS_HTML && (tag == TAG_HEADING ?
		    IN(n->tag, TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6) :
		    n->tag == tag))
			return;
	}
}

static void close_p(struct parser *p)
{
	generate_implied(p, TAG_P);
	pop_until(p, TAG_P);
}

static void clear_to_context(struct parser *p, const short *tags)
{
	soie_node *n;

	while ((n = current(p)) &&
	       !(n->ns == SOIE_NS_HTML && tag_in(n->tag, tags)))
		pop(p);
}

#define CLEAR_TO(p, ...) clear_to_context(p, (const short[]){ __VA_ARGS__, 0 })

/* Template insertion modes */

static void push_tmode(struct parser *p, int mode)
{
	if (p->ntmodes == p->captmodes) {
		p->captmodes = p->captmodes ? p->captmodes * 2 : 8;
		p->tmodes = xrealloc(p->tmodes,
			p->captmodes * sizeof(*p->tmodes));
	}
	p->tmodes[p->ntmodes++] = mode;
}

static void reset_insertion_mode(struct parser *p)
{
	int i, last;
	soie_node *n;

	for (i = p->nstack - 1; i >= 0; i--) {
		n = p->stack[i];
		last = i == 0;
		if (last && p->context)
			n = p->context;
		if (n->ns == SOIE_NS_HTML) {
			switch (n->tag) {
			case TAG_TD:
			case TAG_TH:
				if (!last) {
					p->mode = M_IN_CELL;
					return;
				}
				break;
			case TAG_TR:
				p->mode = M_IN_ROW;
				return;
			case TAG_TBODY:
			case TAG_THEAD:
			case TAG_TFOOT:
				p->mode = M_IN_TABLE_BODY;
				return;
			case TAG_CAPTION:
				p->mode = M_IN_CAPTION;
				return;
			case TAG_COLGROUP:
				p->mode = M_IN_COLUMN_GROUP;
				return;
			case TAG_TABLE:
				p->mode = M_IN_TABLE;
				return;
			case TAG_TEMPLATE:
				p->mode = p->ntmodes ?
					p->tmodes[p->ntmodes - 1] : M_IN_BODY;
				return;
			case TAG_HEAD:
				if (!last) {
					p->mode = M_IN_HEAD;
					return;
				}
				break;
			case TAG_BODY:
				p->mode = M_IN_BODY;
				return;
			case TAG_FRAMESET:
				p->mode = M_IN_FRAMESET;
				return;
			case TAG_HTML:
				p->mode = p->head ? M_AFTER_HEAD :
					M_BEFORE_HEAD;
				return;
			}
		}
		if (last) {
			p->mode = M_IN_BODY;
			return;
		}
	}
}

/* Node creation and insertion (13.2.6.1) */

struct name_fix {
	const char *from, *to;
};

static const struct name_fix svg_tags[] = {
	{ "altglyph", "altGlyph" }, { "altglyphdef", "altGlyphDef" },
	{ "altglyphitem", "altGlyphItem" }, { "animatecolor", "animateColor" },
	{ "animatemotion", "animateMotion" },
	{ "animatetransform", "animateTransform" },
	{ "clippath", "clipPath" }, { "feblend", "feBlend" },
	{ "fecolormatrix", "feColorMatrix" },
	{ "fecomponenttransfer", "feComponentTransfer" },
	{ "fecomposite", "feComposite" },
	{ "feconvolvematrix", "feConvolveMatrix" },
	{ "fediffuselighting", "feDiffuseLighting" },
	{ "fedisplacementmap", "feDisplacementMap" },
	{ "fedistantlight", "feDistantLight" },
	{ "fedropshadow", "feDropShadow" }, { "feflood", "feFlood" },
	{ "fefunca", "feFuncA" }, { "fefuncb", "feFuncB" },
	{ "fefuncg", "feFuncG" }, { "fefuncr", "feFuncR" },
	{ "fegaussianblur", "feGaussianBlur" }, { "feimage", "feImage" },
	{ "femerge", "feMerge" }, { "femergenode", "feMergeNode" },
	{ "femorphology", "feMorphology" }, { "feoffset", "feOffset" },
	{ "fepointlight", "fePointLight" },
	{ "fespecularlighting", "feSpecularLighting" },
	{ "fespotlight", "feSpotLight" }, { "fetile", "feTile" },
	{ "feturbulence", "feTurbulence" },
	{ "foreignobject", "foreignObject" }, { "glyphref", "glyphRef" },
	{ "lineargradient", "linearGradient" },
	{ "radialgradient", "radialGradient" }, { "textpath", "textPath" },
	{ NULL, NULL }
};

static const struct name_fix svg_attrs[] = {
	{ "attributename", "attributeName" },
	{ "attributetype", "attributeType" },
	{ "basefrequency", "baseFrequency" }, { "baseprofile", "baseProfile" },
	{ "calcmode", "calcMode" }, { "clippathunits", "clipPathUnits" },
	{ "diffuseconstant", "diffuseConstant" }, { "edgemode", "edgeMode" },
	{ "filterunits", "filterUnits" }, { "glyphref", "glyphRef" },
	{ "gradienttransform", "gradientTransform" },
	{ "gradientunits", "gradientUnits" },
	{ "kernelmatrix", "kernelMatrix" },
	{ "kernelunitlength", "kernelUnitLength" },
	{ "keypoints", "keyPoints" }, { "keysplines", "keySplines" },
	{ "keytimes", "keyTimes" }, { "lengthadjust", "lengthAdjust" },
	{ "limitingconeangle", "limitingConeAngle" },
	{ "markerheight", "markerHeight" }, { "markerunits", "markerUnits" },
	{ "markerwidth", "markerWidth" },
	{ "maskcontentunits", "maskContentUnits" },
	{ "maskunits", "maskUnits" }, { "numoctaves", "numOctaves" },
	{ "pathlength", "pathLength" },
	{ "patterncontentunits", "patternContentUnits" },
	{ "patterntransform", "patternTransform" },
	{ "patternunits", "patternUnits" }, { "pointsatx", "pointsAtX" },
	{ "pointsaty", "pointsAtY" }, { "pointsatz", "pointsAtZ" },
	{ "preservealpha", "preserveAlpha" },
	{ "preserveaspectratio", "preserveAspectRatio" },
	{ "primitiveunits", "primitiveUnits" }, { "refx", "refX" },
	{ "refy", "refY" }, { "repeatcount", "repeatCount" },
	{ "repeatdur", "repeatDur" },
	{ "requiredextensions", "requiredExtensions" },
	{ "requiredfeatures", "requiredFeatures" },
	{ "specularconstant", "specularConstant" },
	{ "specularexponent", "specularExponent" },
	{ "spreadmethod", "spreadMethod" }, { "startoffset", "startOffset" },
	{ "stddeviation", "stdDeviation" }, { "stitchtiles", "stitchTiles" },
	{ "surfacescale", "surfaceScale" },
	{ "systemlanguage", "systemLanguage" },
	{ "tablevalues", "tableValues" }, { "targetx", "targetX" },
	{ "targety", "targetY" }, { "textlength", "textLength" },
	{ "viewbox", "viewBox" }, { "viewtarget", "viewTarget" },
	{ "xchannelselector", "xChannelSelector" },
	{ "ychannelselector", "yChannelSelector" },
	{ "zoomandpan", "zoomAndPan" },
	{ NULL, NULL }
};

static const char *fix_name(const struct name_fix *f, const char *name)
{
	for (; f->from; f++)
		if (!strcmp(f->from, name))
			return f->to;
	return name;
}

/* "adjust foreign attributes": prefixed names get a namespace. */
static const char *foreign_attr(const char *name, int *ns)
{
	if (!strncmp(name, "xlink:", 6) &&
	    (!strcmp(name + 6, "actuate") || !strcmp(name + 6, "arcrole") ||
	     !strcmp(name + 6, "href") || !strcmp(name + 6, "role") ||
	     !strcmp(name + 6, "show") || !strcmp(name + 6, "title") ||
	     !strcmp(name + 6, "type"))) {
		*ns = SOIE_NS_XLINK;
		return name + 6;
	}
	if (!strcmp(name, "xml:lang") || !strcmp(name, "xml:space")) {
		*ns = SOIE_NS_XML;
		return name + 4;
	}
	if (!strcmp(name, "xmlns")) {
		*ns = SOIE_NS_XMLNS;
		return name;
	}
	if (!strcmp(name, "xmlns:xlink")) {
		*ns = SOIE_NS_XMLNS;
		return name + 6;
	}
	*ns = SOIE_NS_NONE;
	return name;
}

/* "create an element for a token"; t may be NULL for a bare tag. */
static soie_node *create_element(struct parser *p, struct token *t, int ns,
	int tag)
{
	const char *name = tag_name(tag);
	soie_node *el;
	int i, ans;

	if (t) {
		name = t->name.s;
		if (ns == SOIE_NS_SVG)
			name = fix_name(svg_tags, name);
	}
	el = element_new_tag(p->doc, ns, t ? t->tag : tag, name, strlen(name));
	for (i = 0; t && i < t->nattrs; i++) {
		const char *an = t->attrs[i].name.s;

		ans = SOIE_NS_NONE;
		if (ns == SOIE_NS_MATHML && !strcmp(an, "definitionurl"))
			an = "definitionURL";
		else if (ns == SOIE_NS_SVG)
			an = fix_name(svg_attrs, an);
		if (ns != SOIE_NS_HTML)
			an = foreign_attr(an, &ans);
		element_add_attr(el, ans, an, t->attrs[i].value.s,
			t->attrs[i].value.len);
	}
	if (ns == SOIE_NS_MATHML && el->tag == TAG_ANNOTATION_XML && t) {
		const char *enc = tok_attr(t, "encoding");

		el->html_ip = enc && (strcaseeq(enc, "text/html") ||
			strcaseeq(enc, "application/xhtml+xml"));
	}
	return el;
}

static soie_node *clone_element(struct parser *p, soie_node *el)
{
	soie_node *c = element_new_tag(p->doc, el->ns, el->tag, el->name,
		strlen(el->name));
	unsigned i;

	for (i = 0; i < el->u.el.nattrs; i++)
		element_add_attr(c, el->u.el.attrs[i].ns,
			el->u.el.attrs[i].name, el->u.el.attrs[i].value,
			strlen(el->u.el.attrs[i].value));
	return c;
}

/* "appropriate place for inserting a node" */
static void insertion_place(struct parser *p, soie_node *target,
	soie_node **parent, soie_node **ref)
{
	int i, ti = -1, tbi = -1;

	if (!target)
		target = current(p);
	*parent = target;
	*ref = NULL;
	if (p->foster && target->ns == SOIE_NS_HTML &&
	    IN(target->tag, TAG_TABLE, TAG_TBODY, TAG_TFOOT, TAG_THEAD,
	       TAG_TR)) {
		for (i = p->nstack - 1; i >= 0; i--) {
			if (ti < 0 && is_html(p->stack[i], TAG_TEMPLATE))
				ti = i;
			if (tbi < 0 && is_html(p->stack[i], TAG_TABLE))
				tbi = i;
		}
		if (ti >= 0 && (tbi < 0 || ti > tbi)) {
			*parent = p->stack[ti];
		} else if (tbi < 0) {
			*parent = p->stack[0];
		} else if (p->stack[tbi]->parent) {
			*parent = p->stack[tbi]->parent;
			*ref = p->stack[tbi];
		} else {
			*parent = p->stack[tbi - 1];
		}
	}
	if (is_html(*parent, TAG_TEMPLATE)) {
		*parent = (*parent)->u.el.content;
		*ref = NULL;
	}
}

/*
 * Remember a node the parser leaves out of the tree, maybe for a while:
 * those still without a parent at the end of the parse are freed.
 */
static void orphan(struct parser *p, soie_node *n)
{
	int i;

	for (i = 0; i < p->norphans; i++)
		if (p->orphans[i] == n)
			return;
	if (p->norphans == p->caporphans) {
		p->caporphans = p->caporphans ? p->caporphans * 2 : 8;
		p->orphans = xrealloc(p->orphans,
			p->caporphans * sizeof(*p->orphans));
	}
	p->orphans[p->norphans++] = n;
}

static void insert_node(struct parser *p, soie_node *n)
{
	soie_node *parent, *ref;

	insertion_place(p, NULL, &parent, &ref);
	if (parent->type == SOIE_DOCUMENT && n->type == SOIE_ELEMENT &&
	    soie_document_element(parent)) {
		orphan(p, n);
		return;
	}
	node_insert(parent, n, ref);
}

static soie_node *insert_element(struct parser *p, soie_node *el)
{
	insert_node(p, el);
	push(p, el);
	return el;
}

static soie_node *insert_html(struct parser *p, struct token *t)
{
	return insert_element(p, create_element(p, t, SOIE_NS_HTML, 0));
}

static soie_node *insert_tag(struct parser *p, int tag)
{
	return insert_element(p, create_element(p, NULL, SOIE_NS_HTML, tag));
}

static void insert_foreign(struct parser *p, struct token *t, int ns)
{
	insert_element(p, create_element(p, t, ns, 0));
	if (t->self_closing)
		pop(p);
}

/* Insert a comment; parent overrides the insertion place. */
static void insert_comment(struct parser *p, struct token *t,
	soie_node *parent)
{
	soie_node *c = soie_comment_new(p->doc, t->data.s, t->data.len);

	if (parent)
		node_insert(parent, c, NULL);
	else
		insert_node(p, c);
}

static void insert_chars(struct parser *p, const char *s, size_t len)
{
	soie_node *parent, *ref, *prev;

	insertion_place(p, NULL, &parent, &ref);
	if (parent->type == SOIE_DOCUMENT)
		return;
	prev = ref ? ref->prev : parent->last;
	if (prev && prev->type == SOIE_TEXT)
		text_append(prev, s, len);
	else
		node_insert(parent, soie_text_new(p->doc, s, len), ref);
}

/* List of active formatting elements */

static void afe_insert_at(struct parser *p, int i, soie_node *n)
{
	if (p->nafe == p->capafe) {
		p->capafe = p->capafe ? p->capafe * 2 : 16;
		p->afe = xrealloc(p->afe, p->capafe * sizeof(*p->afe));
	}
	memmove(p->afe + i + 1, p->afe + i, (p->nafe - i) * sizeof(*p->afe));
	p->afe[i] = n;
	p->nafe++;
}

static void afe_remove_at(struct parser *p, int i)
{
	memmove(p->afe + i, p->afe + i + 1,
		(p->nafe - i - 1) * sizeof(*p->afe));
	p->nafe--;
}

static int afe_index(struct parser *p, const soie_node *n)
{
	int i;

	for (i = p->nafe - 1; i >= 0; i--)
		if (p->afe[i] == n)
			return i;
	return -1;
}

static int same_attrs(const soie_node *a, const soie_node *b)
{
	unsigned i, j;

	if (a->u.el.nattrs != b->u.el.nattrs)
		return 0;
	for (i = 0; i < a->u.el.nattrs; i++) {
		for (j = 0; j < b->u.el.nattrs; j++)
			if (a->u.el.attrs[i].ns == b->u.el.attrs[j].ns &&
			    !strcmp(a->u.el.attrs[i].name,
				    b->u.el.attrs[j].name) &&
			    !strcmp(a->u.el.attrs[i].value,
				    b->u.el.attrs[j].value))
				break;
		if (j == b->u.el.nattrs)
			return 0;
	}
	return 1;
}

/* Push onto the list, with the Noah's Ark clause. */
static void afe_push(struct parser *p, soie_node *el)
{
	int i, count = 0, first = -1;

	for (i = p->nafe - 1; i >= 0 && p->afe[i]; i--) {
		soie_node *n = p->afe[i];

		if (n->ns == el->ns && !strcmp(n->name, el->name) &&
		    same_attrs(n, el)) {
			count++;
			first = i;
		}
	}
	if (count >= 3)
		afe_remove_at(p, first);
	afe_insert_at(p, p->nafe, el);
}

static void afe_marker(struct parser *p)
{
	afe_insert_at(p, p->nafe, NULL);
}

static void afe_clear_to_marker(struct parser *p)
{
	while (p->nafe) {
		soie_node *n = p->afe[--p->nafe];

		if (!n)
			break;
	}
}

static void reconstruct_afe(struct parser *p)
{
	int i;

	if (!p->nafe)
		return;
	i = p->nafe - 1;
	if (!p->afe[i] || stack_index(p, p->afe[i]) >= 0)
		return;
	while (i > 0) {
		i--;
		if (!p->afe[i] || stack_index(p, p->afe[i]) >= 0) {
			i++;
			break;
		}
	}
	for (; i < p->nafe; i++)
		p->afe[i] = insert_element(p, clone_element(p, p->afe[i]));
}

/* Last formatting element with this tag after the last marker. */
static soie_node *afe_find(struct parser *p, int tag)
{
	int i;

	for (i = p->nafe - 1; i >= 0 && p->afe[i]; i--)
		if (is_html(p->afe[i], tag))
			return p->afe[i];
	return NULL;
}

static int same_name(const soie_node *n, const struct token *t)
{
	return n->ns == SOIE_NS_HTML && (t->tag ? n->tag == t->tag :
		!n->tag && !strcmp(n->name, t->name.s));
}

/* "any other end tag" in body */
static void any_other_end_tag(struct parser *p, struct token *t)
{
	int i;

	for (i = p->nstack - 1; i >= 0; i--) {
		soie_node *n = p->stack[i];

		if (same_name(n, t)) {
			generate_implied(p, t->tag ? t->tag : -2);
			pop_to(p, i);
			return;
		}
		if (is_special(n))
			return;
	}
}

static int is_ancestor(const soie_node *a, const soie_node *n)
{
	for (; n; n = n->parent)
		if (n == a)
			return 1;
	return 0;
}

/* The adoption agency algorithm */
static void adoption_agency(struct parser *p, struct token *t)
{
	soie_node *cur = current(p), *fe, *furthest, *common, *node, *last;
	soie_node *parent, *ref, *el;
	int outer, inner, fi, si, i, ni, ai, bookmark;

	if (cur && same_name(cur, t) && afe_index(p, cur) < 0) {
		pop(p);
		return;
	}
	for (outer = 0; outer < 8; outer++) {
		fe = afe_find(p, t->tag);
		if (!fe) {
			any_other_end_tag(p, t);
			return;
		}
		si = stack_index(p, fe);
		if (si < 0) {
			afe_remove_at(p, afe_index(p, fe));
			return;
		}
		if (!in_scope(p, 0, fe, SCOPE_DEFAULT))
			return;
		furthest = NULL;
		for (i = si + 1; i < p->nstack; i++)
			if (is_special(p->stack[i])) {
				furthest = p->stack[i];
				break;
			}
		if (!furthest) {
			pop_to(p, si);
			afe_remove_at(p, afe_index(p, fe));
			return;
		}
		common = p->stack[si - 1];
		bookmark = afe_index(p, fe);
		node = last = furthest;
		ni = stack_index(p, furthest);
		for (inner = 1;; inner++) {
			node = p->stack[--ni];
			if (node == fe)
				break;
			ai = afe_index(p, node);
			if (inner > 3 && ai >= 0) {
				afe_remove_at(p, ai);
				if (ai < bookmark)
					bookmark--;
				ai = -1;
			}
			if (ai < 0) {
				stack_remove_at(p, ni);
				continue;
			}
			el = clone_element(p, node);
			p->afe[ai] = el;
			stack_replace_at(p, ni, el);
			node = el;
			if (last == furthest)
				bookmark = ai + 1;
			node_insert(node, last, NULL);
			last = node;
		}
		insertion_place(p, common, &parent, &ref);
		if (!is_ancestor(last, parent) &&
		    !(parent->type == SOIE_DOCUMENT &&
		      soie_document_element(parent)) &&
		    (!ref || ref->parent == parent))
			node_insert(parent, last, ref);
		else if (!last->parent)
			orphan(p, last);
		el = clone_element(p, fe);
		while (furthest->first)
			node_insert(el, furthest->first, NULL);
		node_insert(furthest, el, NULL);
		fi = afe_index(p, fe);
		afe_remove_at(p, fi);
		if (fi < bookmark)
			bookmark--;
		afe_insert_at(p, bookmark, el);
		stack_remove_at(p, stack_index(p, fe));
		stack_insert_at(p, stack_index(p, furthest) + 1, el);
	}
}

/* Shared steps */

static void ack(struct token *t)
{
	t->self_closing = 0;
}

static void raw_text(struct parser *p, struct token *t, int state)
{
	insert_html(p, t);
	p->state = state;
	p->orig_mode = p->mode;
	p->mode = M_TEXT;
}

/* Feed a buffer of characters, split by class, to a mode function. */
static void feed_chars(struct parser *p, const char *s, size_t n,
	void (*fn)(struct parser *, struct token *))
{
	struct token seg;
	size_t i = 0, j;

	memset(&seg, 0, sizeof(seg));
	seg.type = TOK_CHARS;
	while (i < n) {
		int cls = s[i] == '\0' ? 1 : is_ws((unsigned char)s[i]) ? 2 : 0;

		for (j = i + 1; j < n; j++) {
			int c = s[j] == '\0' ? 1 :
				is_ws((unsigned char)s[j]) ? 2 : 0;

			if (c != cls)
				break;
		}
		seg.chars = s + i;
		seg.nchars = j - i;
		fn(p, &seg);
		i = j;
	}
}

/* Insertion modes (13.2.6.4) */

static const char *const quirks_prefixes[] = {
	"+//silmaril//dtd html pro v0r11 19970101//",
	"-//as//dtd html 3.0 aswedit + extensions//",
	"-//advasoft ltd//dtd html 3.0 aswedit + extensions//",
	"-//ietf//dtd html 2.0 level 1//",
	"-//ietf//dtd html 2.0 level 2//",
	"-//ietf//dtd html 2.0 strict level 1//",
	"-//ietf//dtd html 2.0 strict level 2//",
	"-//ietf//dtd html 2.0 strict//",
	"-//ietf//dtd html 2.0//",
	"-//ietf//dtd html 2.1e//",
	"-//ietf//dtd html 3.0//",
	"-//ietf//dtd html 3.2 final//",
	"-//ietf//dtd html 3.2//",
	"-//ietf//dtd html 3//",
	"-//ietf//dtd html level 0//",
	"-//ietf//dtd html level 1//",
	"-//ietf//dtd html level 2//",
	"-//ietf//dtd html level 3//",
	"-//ietf//dtd html strict level 0//",
	"-//ietf//dtd html strict level 1//",
	"-//ietf//dtd html strict level 2//",
	"-//ietf//dtd html strict level 3//",
	"-//ietf//dtd html strict//",
	"-//ietf//dtd html//",
	"-//metrius//dtd metrius presentational//",
	"-//microsoft//dtd internet explorer 2.0 html strict//",
	"-//microsoft//dtd internet explorer 2.0 html//",
	"-//microsoft//dtd internet explorer 2.0 tables//",
	"-//microsoft//dtd internet explorer 3.0 html strict//",
	"-//microsoft//dtd internet explorer 3.0 html//",
	"-//microsoft//dtd internet explorer 3.0 tables//",
	"-//netscape comm. corp.//dtd html//",
	"-//netscape comm. corp.//dtd strict html//",
	"-//o'reilly and associates//dtd html 2.0//",
	"-//o'reilly and associates//dtd html extended 1.0//",
	"-//o'reilly and associates//dtd html extended relaxed 1.0//",
	"-//sq//dtd html 2.0 hotmetal + extensions//",
	"-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//",
	"-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//",
	"-//spyglass//dtd html 2.0 extended//",
	"-//sun microsystems corp.//dtd hotjava html//",
	"-//sun microsystems corp.//dtd hotjava strict html//",
	"-//w3c//dtd html 3 1995-03-24//",
	"-//w3c//dtd html 3.2 draft//",
	"-//w3c//dtd html 3.2 final//",
	"-//w3c//dtd html 3.2//",
	"-//w3c//dtd html 3.2s draft//",
	"-//w3c//dtd html 4.0 frameset//",
	"-//w3c//dtd html 4.0 transitional//",
	"-//w3c//dtd html experimental 19960712//",
	"-//w3c//dtd html experimental 970421//",
	"-//w3c//dtd w3 html//",
	"-//w3o//dtd w3 html 3.0//",
	"-//webtechs//dtd mozilla html 2.0//",
	"-//webtechs//dtd mozilla html//",
	NULL
};

static int prefix_ci(const char *s, const char *prefix)
{
	for (; *prefix; s++, prefix++)
		if (lower((unsigned char)*s) != *prefix)
			return 0;
	return 1;
}

static int doctype_quirks(const struct token *t)
{
	const char *pub = t->has_public ? t->public_id.s : "";
	const char *sys = t->has_system ? t->system_id.s : "";
	const char *const *q;
	int no_sys = !t->has_system;

	if (!pub)
		pub = "";
	if (!sys)
		sys = "";
	if (t->force_quirks || !t->has_name || strcmp(t->name.s, "html"))
		return SOIE_QUIRKS;
	if (t->has_public && (strcaseeq(pub, "-//W3O//DTD W3 HTML Strict 3.0//EN//") ||
	    strcaseeq(pub, "-/W3C/DTD HTML 4.0 Transitional/EN") ||
	    strcaseeq(pub, "HTML")))
		return SOIE_QUIRKS;
	if (t->has_system && strcaseeq(sys,
	    "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"))
		return SOIE_QUIRKS;
	if (t->has_public) {
		for (q = quirks_prefixes; *q; q++)
			if (prefix_ci(pub, *q))
				return SOIE_QUIRKS;
		if (prefix_ci(pub, "-//w3c//dtd html 4.01 frameset//") ||
		    prefix_ci(pub, "-//w3c//dtd html 4.01 transitional//"))
			return no_sys ? SOIE_QUIRKS : SOIE_LIMITED_QUIRKS;
		if (prefix_ci(pub, "-//w3c//dtd xhtml 1.0 frameset//") ||
		    prefix_ci(pub, "-//w3c//dtd xhtml 1.0 transitional//"))
			return SOIE_LIMITED_QUIRKS;
	}
	return SOIE_NO_QUIRKS;
}

static void m_initial(struct parser *p, struct token *t)
{
	soie_node *d;

	if (ws_chars(t))
		return;
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, p->doc);
		return;
	}
	if (t->type == TOK_DOCTYPE) {
		d = node_new(p->doc, SOIE_DOCTYPE);
		d->name = xstrndup(t->name.s, t->name.len);
		d->name_owned = 1;
		d->u.doctype.public_id = xstrndup(t->public_id.s ?
			t->public_id.s : "", t->public_id.len);
		d->u.doctype.system_id = xstrndup(t->system_id.s ?
			t->system_id.s : "", t->system_id.len);
		node_insert(p->doc, d, NULL);
		p->doc->u.doc.quirks = (unsigned char)doctype_quirks(t);
		p->mode = M_BEFORE_HTML;
		return;
	}
	p->doc->u.doc.quirks = SOIE_QUIRKS;
	p->mode = M_BEFORE_HTML;
	process(p, t);
}

static void m_before_html(struct parser *p, struct token *t)
{
	soie_node *el;

	if (t->type == TOK_DOCTYPE || ws_chars(t))
		return;
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, p->doc);
		return;
	}
	if (is_start(t, TAG_HTML)) {
		el = create_element(p, t, SOIE_NS_HTML, 0);
		node_insert(p->doc, el, NULL);
		push(p, el);
		p->mode = M_BEFORE_HEAD;
		return;
	}
	if (t->type == TOK_END && !IN(t->tag, TAG_HEAD, TAG_BODY, TAG_HTML,
				      TAG_BR))
		return;
	el = create_element(p, NULL, SOIE_NS_HTML, TAG_HTML);
	node_insert(p->doc, el, NULL);
	push(p, el);
	p->mode = M_BEFORE_HEAD;
	process(p, t);
}

static void m_before_head(struct parser *p, struct token *t)
{
	if (ws_chars(t) || t->type == TOK_DOCTYPE)
		return;
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, NULL);
		return;
	}
	if (is_start(t, TAG_HTML)) {
		m_in_body(p, t);
		return;
	}
	if (is_start(t, TAG_HEAD)) {
		p->head = insert_html(p, t);
		p->mode = M_IN_HEAD;
		return;
	}
	if (t->type == TOK_END && !IN(t->tag, TAG_HEAD, TAG_BODY, TAG_HTML,
				      TAG_BR))
		return;
	p->head = insert_tag(p, TAG_HEAD);
	p->mode = M_IN_HEAD;
	process(p, t);
}

static void m_in_head(struct parser *p, struct token *t)
{
	if (ws_chars(t)) {
		insert_chars(p, t->chars, t->nchars);
		return;
	}
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, NULL);
		return;
	}
	if (t->type == TOK_DOCTYPE)
		return;
	if (t->type == TOK_START) {
		switch (t->tag) {
		case TAG_HTML:
			m_in_body(p, t);
			return;
		case TAG_BASE:
		case TAG_BASEFONT:
		case TAG_BGSOUND:
		case TAG_LINK:
		case TAG_META:
			insert_html(p, t);
			pop(p);
			ack(t);
			return;
		case TAG_TITLE:
			raw_text(p, t, S_RCDATA);
			return;
		case TAG_NOSCRIPT:
			if (!p->scripting) {
				insert_html(p, t);
				p->mode = M_IN_HEAD_NOSCRIPT;
				return;
			}
			/* fall through */
		case TAG_NOFRAMES:
		case TAG_STYLE:
			raw_text(p, t, S_RAWTEXT);
			return;
		case TAG_SCRIPT:
			raw_text(p, t, S_SCRIPT);
			return;
		case TAG_TEMPLATE:
			insert_html(p, t);
			afe_marker(p);
			p->frameset_ok = 0;
			p->mode = M_IN_TEMPLATE;
			push_tmode(p, M_IN_TEMPLATE);
			return;
		case TAG_HEAD:
			return;
		}
	}
	if (is_end(t, TAG_HEAD)) {
		pop(p);
		p->mode = M_AFTER_HEAD;
		return;
	}
	if (is_end(t, TAG_TEMPLATE)) {
		if (!has_template(p))
			return;
		generate_implied_thoroughly(p);
		pop_until(p, TAG_TEMPLATE);
		afe_clear_to_marker(p);
		p->ntmodes--;
		reset_insertion_mode(p);
		return;
	}
	if (t->type == TOK_END && !IN(t->tag, TAG_BODY, TAG_HTML, TAG_BR))
		return;
	pop(p);
	p->mode = M_AFTER_HEAD;
	process(p, t);
}

static void m_in_head_noscript(struct parser *p, struct token *t)
{
	if (t->type == TOK_DOCTYPE)
		return;
	if (is_start(t, TAG_HTML)) {
		m_in_body(p, t);
		return;
	}
	if (is_end(t, TAG_NOSCRIPT)) {
		pop(p);
		p->mode = M_IN_HEAD;
		return;
	}
	if (ws_chars(t) || t->type == TOK_COMMENT ||
	    (t->type == TOK_START && IN(t->tag, TAG_BASEFONT, TAG_BGSOUND,
		TAG_LINK, TAG_META, TAG_NOFRAMES, TAG_STYLE))) {
		m_in_head(p, t);
		return;
	}
	if ((t->type == TOK_START && IN(t->tag, TAG_HEAD, TAG_NOSCRIPT)) ||
	    (t->type == TOK_END && t->tag != TAG_BR))
		return;
	pop(p);
	p->mode = M_IN_HEAD;
	process(p, t);
}

static void m_after_head(struct parser *p, struct token *t)
{
	if (ws_chars(t)) {
		insert_chars(p, t->chars, t->nchars);
		return;
	}
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, NULL);
		return;
	}
	if (t->type == TOK_DOCTYPE)
		return;
	if (t->type == TOK_START) {
		switch (t->tag) {
		case TAG_HTML:
			m_in_body(p, t);
			return;
		case TAG_BODY:
			insert_html(p, t);
			p->frameset_ok = 0;
			p->mode = M_IN_BODY;
			return;
		case TAG_FRAMESET:
			insert_html(p, t);
			p->mode = M_IN_FRAMESET;
			return;
		case TAG_BASE:
		case TAG_BASEFONT:
		case TAG_BGSOUND:
		case TAG_LINK:
		case TAG_META:
		case TAG_NOFRAMES:
		case TAG_SCRIPT:
		case TAG_STYLE:
		case TAG_TEMPLATE:
		case TAG_TITLE:
			push(p, p->head);
			m_in_head(p, t);
			{
				int i = stack_index(p, p->head);

				if (i >= 0)
					stack_remove_at(p, i);
			}
			return;
		case TAG_HEAD:
			return;
		}
	}
	if (is_end(t, TAG_TEMPLATE)) {
		m_in_head(p, t);
		return;
	}
	if (t->type == TOK_END && !IN(t->tag, TAG_BODY, TAG_HTML, TAG_BR))
		return;
	insert_tag(p, TAG_BODY);
	p->mode = M_IN_BODY;
	process(p, t);
}

/* Elements allowed open at the end of body without a parse error are
 * not checked: errors are not reported. */

static void merge_attrs(soie_node *el, struct token *t)
{
	int i;

	for (i = 0; i < t->nattrs; i++)
		if (!element_find_attr(el, t->attrs[i].name.s))
			element_add_attr(el, SOIE_NS_NONE, t->attrs[i].name.s,
				t->attrs[i].value.s, t->attrs[i].value.len);
}

static void in_body_start(struct parser *p, struct token *t)
{
	soie_node *n;
	int i;
	const char *type;

	switch (t->tag) {
	case TAG_HTML:
		if (!has_template(p))
			merge_attrs(p->stack[0], t);
		return;
	case TAG_BASE:
	case TAG_BASEFONT:
	case TAG_BGSOUND:
	case TAG_LINK:
	case TAG_META:
	case TAG_NOFRAMES:
	case TAG_SCRIPT:
	case TAG_STYLE:
	case TAG_TEMPLATE:
	case TAG_TITLE:
		m_in_head(p, t);
		return;
	case TAG_BODY:
		if (p->nstack < 2 || !is_html(p->stack[1], TAG_BODY) ||
		    has_template(p))
			return;
		p->frameset_ok = 0;
		merge_attrs(p->stack[1], t);
		return;
	case TAG_FRAMESET:
		if (p->nstack < 2 || !is_html(p->stack[1], TAG_BODY) ||
		    !p->frameset_ok)
			return;
		node_detach(p->stack[1]);
		orphan(p, p->stack[1]);
		pop_to(p, 1);
		insert_html(p, t);
		p->mode = M_IN_FRAMESET;
		return;
	case TAG_ADDRESS: case TAG_ARTICLE: case TAG_ASIDE:
	case TAG_BLOCKQUOTE: case TAG_CENTER: case TAG_DETAILS:
	case TAG_DIALOG: case TAG_DIR: case TAG_DIV: case TAG_DL:
	case TAG_FIELDSET: case TAG_FIGCAPTION: case TAG_FIGURE:
	case TAG_FOOTER: case TAG_HEADER: case TAG_HGROUP: case TAG_MAIN:
	case TAG_MENU: case TAG_NAV: case TAG_OL: case TAG_P:
	case TAG_SEARCH: case TAG_SECTION: case TAG_SUMMARY: case TAG_UL:
		if (in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		insert_html(p, t);
		return;
	case TAG_H1: case TAG_H2: case TAG_H3:
	case TAG_H4: case TAG_H5: case TAG_H6:
		if (in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		n = current(p);
		if (n->ns == SOIE_NS_HTML && IN(n->tag, TAG_H1, TAG_H2,
			TAG_H3, TAG_H4, TAG_H5, TAG_H6))
			pop(p);
		insert_html(p, t);
		return;
	case TAG_PRE:
	case TAG_LISTING:
		if (in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		insert_html(p, t);
		p->skip_lf = 1;
		p->frameset_ok = 0;
		return;
	case TAG_FORM:
		if (p->form && !has_template(p))
			return;
		if (in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		n = insert_html(p, t);
		if (!has_template(p))
			p->form = n;
		return;
	case TAG_LI:
	case TAG_DD:
	case TAG_DT:
		p->frameset_ok = 0;
		for (i = p->nstack - 1; i >= 0; i--) {
			n = p->stack[i];
			if (t->tag == TAG_LI ? is_html(n, TAG_LI) :
			    (is_html(n, TAG_DD) || is_html(n, TAG_DT))) {
				generate_implied(p, n->tag);
				pop_until(p, n->tag);
				break;
			}
			if (is_special(n) && !is_html(n, TAG_ADDRESS) &&
			    !is_html(n, TAG_DIV) && !is_html(n, TAG_P))
				break;
		}
		if (in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		insert_html(p, t);
		return;
	case TAG_PLAINTEXT:
		if (in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		insert_html(p, t);
		p->state = S_PLAINTEXT;
		return;
	case TAG_BUTTON:
		if (in_scope(p, TAG_BUTTON, NULL, SCOPE_DEFAULT)) {
			generate_implied(p, 0);
			pop_until(p, TAG_BUTTON);
		}
		reconstruct_afe(p);
		insert_html(p, t);
		p->frameset_ok = 0;
		return;
	case TAG_A:
		n = afe_find(p, TAG_A);
		if (n) {
			adoption_agency(p, t);
			if ((i = afe_index(p, n)) >= 0)
				afe_remove_at(p, i);
			if ((i = stack_index(p, n)) >= 0)
				stack_remove_at(p, i);
		}
		reconstruct_afe(p);
		afe_push(p, insert_html(p, t));
		return;
	case TAG_B: case TAG_BIG: case TAG_CODE: case TAG_EM:
	case TAG_FONT: case TAG_I: case TAG_S: case TAG_SMALL:
	case TAG_STRIKE: case TAG_STRONG: case TAG_TT: case TAG_U:
		reconstruct_afe(p);
		afe_push(p, insert_html(p, t));
		return;
	case TAG_NOBR:
		reconstruct_afe(p);
		if (in_scope(p, TAG_NOBR, NULL, SCOPE_DEFAULT)) {
			adoption_agency(p, t);
			reconstruct_afe(p);
		}
		afe_push(p, insert_html(p, t));
		return;
	case TAG_APPLET:
	case TAG_MARQUEE:
	case TAG_OBJECT:
		reconstruct_afe(p);
		insert_html(p, t);
		afe_marker(p);
		p->frameset_ok = 0;
		return;
	case TAG_TABLE:
		if (p->doc->u.doc.quirks != SOIE_QUIRKS &&
		    in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		insert_html(p, t);
		p->frameset_ok = 0;
		p->mode = M_IN_TABLE;
		return;
	case TAG_AREA: case TAG_BR: case TAG_EMBED: case TAG_IMG:
	case TAG_KEYGEN: case TAG_WBR:
		reconstruct_afe(p);
		insert_html(p, t);
		pop(p);
		ack(t);
		p->frameset_ok = 0;
		return;
	case TAG_INPUT:
		if (p->context && is_html(p->context, TAG_SELECT))
			return;
		if (in_scope(p, TAG_SELECT, NULL, SCOPE_DEFAULT))
			pop_until(p, TAG_SELECT);
		reconstruct_afe(p);
		insert_html(p, t);
		pop(p);
		ack(t);
		type = tok_attr(t, "type");
		if (!type || !strcaseeq(type, "hidden"))
			p->frameset_ok = 0;
		return;
	case TAG_PARAM:
	case TAG_SOURCE:
	case TAG_TRACK:
		insert_html(p, t);
		pop(p);
		ack(t);
		return;
	case TAG_HR:
		if (in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		if (in_scope(p, TAG_SELECT, NULL, SCOPE_DEFAULT))
			generate_implied(p, 0);
		insert_html(p, t);
		pop(p);
		ack(t);
		p->frameset_ok = 0;
		return;
	case TAG_IMAGE:
		buf_reset(&t->name);
		buf_add(&t->name, "img", 3);
		t->tag = TAG_IMG;
		process(p, t);
		return;
	case TAG_TEXTAREA:
		insert_html(p, t);
		p->skip_lf = 1;
		p->state = S_RCDATA;
		p->orig_mode = p->mode;
		p->frameset_ok = 0;
		p->mode = M_TEXT;
		return;
	case TAG_XMP:
		if (in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			close_p(p);
		reconstruct_afe(p);
		p->frameset_ok = 0;
		raw_text(p, t, S_RAWTEXT);
		return;
	case TAG_IFRAME:
		p->frameset_ok = 0;
		raw_text(p, t, S_RAWTEXT);
		return;
	case TAG_NOEMBED:
		raw_text(p, t, S_RAWTEXT);
		return;
	case TAG_NOSCRIPT:
		if (p->scripting) {
			raw_text(p, t, S_RAWTEXT);
			return;
		}
		break;
	case TAG_SELECT:
		if (p->context && is_html(p->context, TAG_SELECT))
			return;
		if (in_scope(p, TAG_SELECT, NULL, SCOPE_DEFAULT)) {
			pop_until(p, TAG_SELECT);
			return;
		}
		reconstruct_afe(p);
		insert_html(p, t);
		p->frameset_ok = 0;
		return;
	case TAG_OPTION:
		if (in_scope(p, TAG_SELECT, NULL, SCOPE_DEFAULT)) {
			generate_implied(p, TAG_OPTGROUP);
		} else if (is_html(current(p), TAG_OPTION)) {
			pop(p);
		}
		reconstruct_afe(p);
		insert_html(p, t);
		return;
	case TAG_OPTGROUP:
		if (in_scope(p, TAG_SELECT, NULL, SCOPE_DEFAULT)) {
			generate_implied(p, 0);
		} else if (is_html(current(p), TAG_OPTION)) {
			pop(p);
		}
		reconstruct_afe(p);
		insert_html(p, t);
		return;
	case TAG_RB:
	case TAG_RTC:
		if (in_scope(p, TAG_RUBY, NULL, SCOPE_DEFAULT))
			generate_implied(p, 0);
		insert_html(p, t);
		return;
	case TAG_RP:
	case TAG_RT:
		if (in_scope(p, TAG_RUBY, NULL, SCOPE_DEFAULT))
			generate_implied(p, TAG_RTC);
		insert_html(p, t);
		return;
	case TAG_MATH:
	case TAG_SVG:
		reconstruct_afe(p);
		insert_foreign(p, t, t->tag == TAG_MATH ? SOIE_NS_MATHML :
			SOIE_NS_SVG);
		ack(t);
		return;
	case TAG_CAPTION: case TAG_COL: case TAG_COLGROUP: case TAG_FRAME:
	case TAG_HEAD: case TAG_TBODY: case TAG_TD: case TAG_TFOOT:
	case TAG_TH: case TAG_THEAD: case TAG_TR:
		return;
	}
	reconstruct_afe(p);
	insert_html(p, t);
}

static void in_body_end(struct parser *p, struct token *t)
{
	soie_node *n;

	switch (t->tag) {
	case TAG_TEMPLATE:
		m_in_head(p, t);
		return;
	case TAG_BODY:
	case TAG_HTML:
		if (!in_scope(p, TAG_BODY, NULL, SCOPE_DEFAULT))
			return;
		p->mode = M_AFTER_BODY;
		if (t->tag == TAG_HTML)
			process(p, t);
		return;
	case TAG_ADDRESS: case TAG_ARTICLE: case TAG_ASIDE:
	case TAG_BLOCKQUOTE: case TAG_BUTTON: case TAG_CENTER:
	case TAG_DETAILS: case TAG_DIALOG: case TAG_DIR: case TAG_DIV:
	case TAG_DL: case TAG_FIELDSET: case TAG_FIGCAPTION:
	case TAG_FIGURE: case TAG_FOOTER: case TAG_HEADER: case TAG_HGROUP:
	case TAG_LISTING: case TAG_MAIN: case TAG_MENU: case TAG_NAV:
	case TAG_OL: case TAG_PRE: case TAG_SEARCH: case TAG_SECTION:
	case TAG_SELECT: case TAG_SUMMARY: case TAG_UL:
		if (!in_scope(p, t->tag, NULL, SCOPE_DEFAULT))
			return;
		generate_implied(p, 0);
		pop_until(p, t->tag);
		return;
	case TAG_FORM:
		if (!has_template(p)) {
			n = p->form;
			p->form = NULL;
			if (!n || !in_scope(p, 0, n, SCOPE_DEFAULT))
				return;
			generate_implied(p, 0);
			stack_remove_at(p, stack_index(p, n));
		} else {
			if (!in_scope(p, TAG_FORM, NULL, SCOPE_DEFAULT))
				return;
			generate_implied(p, 0);
			pop_until(p, TAG_FORM);
		}
		return;
	case TAG_P:
		if (!in_scope(p, TAG_P, NULL, SCOPE_BUTTON))
			insert_tag(p, TAG_P);
		close_p(p);
		return;
	case TAG_LI:
		if (!in_scope(p, TAG_LI, NULL, SCOPE_LIST_ITEM))
			return;
		generate_implied(p, TAG_LI);
		pop_until(p, TAG_LI);
		return;
	case TAG_DD:
	case TAG_DT:
		if (!in_scope(p, t->tag, NULL, SCOPE_DEFAULT))
			return;
		generate_implied(p, t->tag);
		pop_until(p, t->tag);
		return;
	case TAG_H1: case TAG_H2: case TAG_H3:
	case TAG_H4: case TAG_H5: case TAG_H6:
		if (!in_scope(p, TAG_HEADING, NULL, SCOPE_DEFAULT))
			return;
		generate_implied(p, 0);
		pop_until(p, TAG_HEADING);
		return;
	case TAG_A: case TAG_B: case TAG_BIG: case TAG_CODE: case TAG_EM:
	case TAG_FONT: case TAG_I: case TAG_NOBR: case TAG_S:
	case TAG_SMALL: case TAG_STRIKE: case TAG_STRONG: case TAG_TT:
	case TAG_U:
		adoption_agency(p, t);
		return;
	case TAG_APPLET:
	case TAG_MARQUEE:
	case TAG_OBJECT:
		if (!in_scope(p, t->tag, NULL, SCOPE_DEFAULT))
			return;
		generate_implied(p, 0);
		pop_until(p, t->tag);
		afe_clear_to_marker(p);
		return;
	case TAG_BR:
		t->type = TOK_START;
		t->nattrs = 0;
		t->self_closing = 0;
		in_body_start(p, t);
		return;
	}
	any_other_end_tag(p, t);
}

static void m_in_body(struct parser *p, struct token *t)
{
	switch (t->type) {
	case TOK_CHARS:
		if (nul_chars(t))
			return;
		reconstruct_afe(p);
		insert_chars(p, t->chars, t->nchars);
		if (!ws_chars(t))
			p->frameset_ok = 0;
		return;
	case TOK_COMMENT:
		insert_comment(p, t, NULL);
		return;
	case TOK_DOCTYPE:
		return;
	case TOK_START:
		in_body_start(p, t);
		return;
	case TOK_END:
		in_body_end(p, t);
		return;
	case TOK_EOF:
		if (p->ntmodes)
			m_in_template(p, t);
		return;
	}
}

static void m_text(struct parser *p, struct token *t)
{
	if (t->type == TOK_CHARS) {
		insert_chars(p, t->chars, t->nchars);
		return;
	}
	pop(p);
	p->mode = p->orig_mode;
	if (t->type == TOK_EOF)
		process(p, t);
}

static void m_in_table(struct parser *p, struct token *t)
{
	soie_node *n = current(p);
	const char *type;

	switch (t->type) {
	case TOK_CHARS:
		if (n->ns == SOIE_NS_HTML && IN(n->tag, TAG_TABLE, TAG_TBODY,
		    TAG_TEMPLATE, TAG_TFOOT, TAG_THEAD, TAG_TR)) {
			buf_reset(&p->pending);
			p->pending_dirty = 0;
			p->orig_mode = p->mode;
			p->mode = M_IN_TABLE_TEXT;
			process(p, t);
			return;
		}
		break;
	case TOK_COMMENT:
		insert_comment(p, t, NULL);
		return;
	case TOK_DOCTYPE:
		return;
	case TOK_START:
		switch (t->tag) {
		case TAG_CAPTION:
			CLEAR_TO(p, TAG_TABLE, TAG_TEMPLATE, TAG_HTML);
			afe_marker(p);
			insert_html(p, t);
			p->mode = M_IN_CAPTION;
			return;
		case TAG_COLGROUP:
			CLEAR_TO(p, TAG_TABLE, TAG_TEMPLATE, TAG_HTML);
			insert_html(p, t);
			p->mode = M_IN_COLUMN_GROUP;
			return;
		case TAG_COL:
			CLEAR_TO(p, TAG_TABLE, TAG_TEMPLATE, TAG_HTML);
			insert_tag(p, TAG_COLGROUP);
			p->mode = M_IN_COLUMN_GROUP;
			process(p, t);
			return;
		case TAG_TBODY:
		case TAG_TFOOT:
		case TAG_THEAD:
			CLEAR_TO(p, TAG_TABLE, TAG_TEMPLATE, TAG_HTML);
			insert_html(p, t);
			p->mode = M_IN_TABLE_BODY;
			return;
		case TAG_TD:
		case TAG_TH:
		case TAG_TR:
			CLEAR_TO(p, TAG_TABLE, TAG_TEMPLATE, TAG_HTML);
			insert_tag(p, TAG_TBODY);
			p->mode = M_IN_TABLE_BODY;
			process(p, t);
			return;
		case TAG_TABLE:
			if (!in_scope(p, TAG_TABLE, NULL, SCOPE_TABLE))
				return;
			pop_until(p, TAG_TABLE);
			reset_insertion_mode(p);
			process(p, t);
			return;
		case TAG_STYLE:
		case TAG_SCRIPT:
		case TAG_TEMPLATE:
			m_in_head(p, t);
			return;
		case TAG_INPUT:
			type = tok_attr(t, "type");
			if (!type || !strcaseeq(type, "hidden"))
				break;
			insert_html(p, t);
			pop(p);
			ack(t);
			return;
		case TAG_FORM:
			if (p->form && !has_template(p))
				return;
			n = insert_html(p, t);
			if (!has_template(p))
				p->form = n;
			pop(p);
			return;
		}
		break;
	case TOK_END:
		switch (t->tag) {
		case TAG_TABLE:
			if (!in_scope(p, TAG_TABLE, NULL, SCOPE_TABLE))
				return;
			pop_until(p, TAG_TABLE);
			reset_insertion_mode(p);
			return;
		case TAG_BODY: case TAG_CAPTION: case TAG_COL:
		case TAG_COLGROUP: case TAG_HTML: case TAG_TBODY:
		case TAG_TD: case TAG_TFOOT: case TAG_TH: case TAG_THEAD:
		case TAG_TR:
			return;
		case TAG_TEMPLATE:
			m_in_head(p, t);
			return;
		}
		break;
	case TOK_EOF:
		m_in_body(p, t);
		return;
	}
	p->foster = 1;
	m_in_body(p, t);
	p->foster = 0;
}

static void table_text_dirty(struct parser *p, struct token *t)
{
	p->foster = 1;
	m_in_body(p, t);
	p->foster = 0;
}

static void m_in_table_text(struct parser *p, struct token *t)
{
	if (t->type == TOK_CHARS) {
		if (nul_chars(t))
			return;
		buf_add(&p->pending, t->chars, t->nchars);
		if (!ws_chars(t))
			p->pending_dirty = 1;
		return;
	}
	if (p->pending.len) {
		if (p->pending_dirty)
			feed_chars(p, p->pending.s, p->pending.len,
				table_text_dirty);
		else
			insert_chars(p, p->pending.s, p->pending.len);
		buf_reset(&p->pending);
	}
	p->mode = p->orig_mode;
	process(p, t);
}

static int close_caption(struct parser *p)
{
	if (!in_scope(p, TAG_CAPTION, NULL, SCOPE_TABLE))
		return 0;
	generate_implied(p, 0);
	pop_until(p, TAG_CAPTION);
	afe_clear_to_marker(p);
	p->mode = M_IN_TABLE;
	return 1;
}

static void m_in_caption(struct parser *p, struct token *t)
{
	if (is_end(t, TAG_CAPTION)) {
		close_caption(p);
		return;
	}
	if ((t->type == TOK_START && IN(t->tag, TAG_CAPTION, TAG_COL,
	     TAG_COLGROUP, TAG_TBODY, TAG_TD, TAG_TFOOT, TAG_TH, TAG_THEAD,
	     TAG_TR)) || is_end(t, TAG_TABLE)) {
		if (close_caption(p))
			process(p, t);
		return;
	}
	if (t->type == TOK_END && IN(t->tag, TAG_BODY, TAG_COL, TAG_COLGROUP,
	    TAG_HTML, TAG_TBODY, TAG_TD, TAG_TFOOT, TAG_TH, TAG_THEAD, TAG_TR))
		return;
	m_in_body(p, t);
}

static void m_in_column_group(struct parser *p, struct token *t)
{
	if (ws_chars(t)) {
		insert_chars(p, t->chars, t->nchars);
		return;
	}
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, NULL);
		return;
	}
	if (t->type == TOK_DOCTYPE)
		return;
	if (is_start(t, TAG_HTML)) {
		m_in_body(p, t);
		return;
	}
	if (is_start(t, TAG_COL)) {
		insert_html(p, t);
		pop(p);
		ack(t);
		return;
	}
	if (is_end(t, TAG_COLGROUP)) {
		if (is_html(current(p), TAG_COLGROUP)) {
			pop(p);
			p->mode = M_IN_TABLE;
		}
		return;
	}
	if (is_end(t, TAG_COL))
		return;
	if (is_start(t, TAG_TEMPLATE) || is_end(t, TAG_TEMPLATE)) {
		m_in_head(p, t);
		return;
	}
	if (t->type == TOK_EOF) {
		m_in_body(p, t);
		return;
	}
	if (!is_html(current(p), TAG_COLGROUP))
		return;
	pop(p);
	p->mode = M_IN_TABLE;
	process(p, t);
}

static void m_in_table_body(struct parser *p, struct token *t)
{
	if (is_start(t, TAG_TR)) {
		CLEAR_TO(p, TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TEMPLATE,
			TAG_HTML);
		insert_html(p, t);
		p->mode = M_IN_ROW;
		return;
	}
	if (is_start(t, TAG_TH) || is_start(t, TAG_TD)) {
		CLEAR_TO(p, TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TEMPLATE,
			TAG_HTML);
		insert_tag(p, TAG_TR);
		p->mode = M_IN_ROW;
		process(p, t);
		return;
	}
	if (t->type == TOK_END && IN(t->tag, TAG_TBODY, TAG_TFOOT,
				     TAG_THEAD)) {
		if (!in_scope(p, t->tag, NULL, SCOPE_TABLE))
			return;
		CLEAR_TO(p, TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TEMPLATE,
			TAG_HTML);
		pop(p);
		p->mode = M_IN_TABLE;
		return;
	}
	if ((t->type == TOK_START && IN(t->tag, TAG_CAPTION, TAG_COL,
	     TAG_COLGROUP, TAG_TBODY, TAG_TFOOT, TAG_THEAD)) ||
	    is_end(t, TAG_TABLE)) {
		if (!in_scope(p, TAG_TBODY, NULL, SCOPE_TABLE) &&
		    !in_scope(p, TAG_THEAD, NULL, SCOPE_TABLE) &&
		    !in_scope(p, TAG_TFOOT, NULL, SCOPE_TABLE))
			return;
		CLEAR_TO(p, TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TEMPLATE,
			TAG_HTML);
		pop(p);
		p->mode = M_IN_TABLE;
		process(p, t);
		return;
	}
	if (t->type == TOK_END && IN(t->tag, TAG_BODY, TAG_CAPTION, TAG_COL,
	    TAG_COLGROUP, TAG_HTML, TAG_TD, TAG_TH, TAG_TR))
		return;
	m_in_table(p, t);
}

static int close_row(struct parser *p)
{
	if (!in_scope(p, TAG_TR, NULL, SCOPE_TABLE))
		return 0;
	CLEAR_TO(p, TAG_TR, TAG_TEMPLATE, TAG_HTML);
	pop(p);
	p->mode = M_IN_TABLE_BODY;
	return 1;
}

static void m_in_row(struct parser *p, struct token *t)
{
	if (is_start(t, TAG_TH) || is_start(t, TAG_TD)) {
		CLEAR_TO(p, TAG_TR, TAG_TEMPLATE, TAG_HTML);
		insert_html(p, t);
		p->mode = M_IN_CELL;
		afe_marker(p);
		return;
	}
	if (is_end(t, TAG_TR)) {
		close_row(p);
		return;
	}
	if ((t->type == TOK_START && IN(t->tag, TAG_CAPTION, TAG_COL,
	     TAG_COLGROUP, TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TR)) ||
	    is_end(t, TAG_TABLE)) {
		if (close_row(p))
			process(p, t);
		return;
	}
	if (t->type == TOK_END && IN(t->tag, TAG_TBODY, TAG_TFOOT,
				     TAG_THEAD)) {
		if (!in_scope(p, t->tag, NULL, SCOPE_TABLE))
			return;
		if (close_row(p))
			process(p, t);
		return;
	}
	if (t->type == TOK_END && IN(t->tag, TAG_BODY, TAG_CAPTION, TAG_COL,
	    TAG_COLGROUP, TAG_HTML, TAG_TD, TAG_TH))
		return;
	m_in_table(p, t);
}

static void close_cell(struct parser *p)
{
	generate_implied(p, 0);
	while (current(p) && !is_html(current(p), TAG_TD) &&
	       !is_html(current(p), TAG_TH))
		pop(p);
	pop(p);
	afe_clear_to_marker(p);
	p->mode = M_IN_ROW;
}

static void m_in_cell(struct parser *p, struct token *t)
{
	if (is_end(t, TAG_TD) || is_end(t, TAG_TH)) {
		if (!in_scope(p, t->tag, NULL, SCOPE_TABLE))
			return;
		generate_implied(p, 0);
		pop_until(p, t->tag);
		afe_clear_to_marker(p);
		p->mode = M_IN_ROW;
		return;
	}
	if (t->type == TOK_START && IN(t->tag, TAG_CAPTION, TAG_COL,
	    TAG_COLGROUP, TAG_TBODY, TAG_TD, TAG_TFOOT, TAG_TH, TAG_THEAD,
	    TAG_TR)) {
		if (!in_scope(p, TAG_TD, NULL, SCOPE_TABLE) &&
		    !in_scope(p, TAG_TH, NULL, SCOPE_TABLE))
			return;
		close_cell(p);
		process(p, t);
		return;
	}
	if (t->type == TOK_END && IN(t->tag, TAG_BODY, TAG_CAPTION, TAG_COL,
				     TAG_COLGROUP, TAG_HTML))
		return;
	if (t->type == TOK_END && IN(t->tag, TAG_TABLE, TAG_TBODY, TAG_TFOOT,
				     TAG_THEAD, TAG_TR)) {
		if (!in_scope(p, t->tag, NULL, SCOPE_TABLE))
			return;
		close_cell(p);
		process(p, t);
		return;
	}
	m_in_body(p, t);
}

static void switch_template_mode(struct parser *p, struct token *t, int mode)
{
	p->ntmodes--;
	push_tmode(p, mode);
	p->mode = mode;
	process(p, t);
}

static void m_in_template(struct parser *p, struct token *t)
{
	switch (t->type) {
	case TOK_CHARS:
	case TOK_COMMENT:
	case TOK_DOCTYPE:
		m_in_body(p, t);
		return;
	case TOK_START:
		switch (t->tag) {
		case TAG_BASE: case TAG_BASEFONT: case TAG_BGSOUND:
		case TAG_LINK: case TAG_META: case TAG_NOFRAMES:
		case TAG_SCRIPT: case TAG_STYLE: case TAG_TEMPLATE:
		case TAG_TITLE:
			m_in_head(p, t);
			return;
		case TAG_CAPTION: case TAG_COLGROUP: case TAG_TBODY:
		case TAG_TFOOT: case TAG_THEAD:
			switch_template_mode(p, t, M_IN_TABLE);
			return;
		case TAG_COL:
			switch_template_mode(p, t, M_IN_COLUMN_GROUP);
			return;
		case TAG_TR:
			switch_template_mode(p, t, M_IN_TABLE_BODY);
			return;
		case TAG_TD:
		case TAG_TH:
			switch_template_mode(p, t, M_IN_ROW);
			return;
		}
		switch_template_mode(p, t, M_IN_BODY);
		return;
	case TOK_END:
		if (t->tag == TAG_TEMPLATE)
			m_in_head(p, t);
		return;
	case TOK_EOF:
		if (!has_template(p))
			return;
		pop_until(p, TAG_TEMPLATE);
		afe_clear_to_marker(p);
		p->ntmodes--;
		reset_insertion_mode(p);
		process(p, t);
		return;
	}
}

static void m_after_body(struct parser *p, struct token *t)
{
	if (ws_chars(t) || is_start(t, TAG_HTML)) {
		m_in_body(p, t);
		return;
	}
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, p->stack[0]);
		return;
	}
	if (t->type == TOK_DOCTYPE || t->type == TOK_EOF)
		return;
	if (is_end(t, TAG_HTML)) {
		if (!p->context)
			p->mode = M_AFTER_AFTER_BODY;
		return;
	}
	p->mode = M_IN_BODY;
	process(p, t);
}

static void m_in_frameset(struct parser *p, struct token *t)
{
	if (ws_chars(t)) {
		insert_chars(p, t->chars, t->nchars);
		return;
	}
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, NULL);
		return;
	}
	if (t->type == TOK_START) {
		switch (t->tag) {
		case TAG_HTML:
			m_in_body(p, t);
			return;
		case TAG_FRAMESET:
			insert_html(p, t);
			return;
		case TAG_FRAME:
			insert_html(p, t);
			pop(p);
			ack(t);
			return;
		case TAG_NOFRAMES:
			m_in_head(p, t);
			return;
		}
	}
	if (is_end(t, TAG_FRAMESET)) {
		if (p->nstack == 1)
			return;
		pop(p);
		if (!p->context && !is_html(current(p), TAG_FRAMESET))
			p->mode = M_AFTER_FRAMESET;
	}
}

static void m_after_frameset(struct parser *p, struct token *t)
{
	if (ws_chars(t)) {
		insert_chars(p, t->chars, t->nchars);
		return;
	}
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, NULL);
		return;
	}
	if (is_start(t, TAG_HTML)) {
		m_in_body(p, t);
		return;
	}
	if (is_end(t, TAG_HTML)) {
		p->mode = M_AFTER_AFTER_FRAMESET;
		return;
	}
	if (is_start(t, TAG_NOFRAMES))
		m_in_head(p, t);
}

static void m_after_after_body(struct parser *p, struct token *t)
{
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, p->doc);
		return;
	}
	if (t->type == TOK_DOCTYPE || ws_chars(t) || is_start(t, TAG_HTML)) {
		m_in_body(p, t);
		return;
	}
	if (t->type == TOK_EOF)
		return;
	p->mode = M_IN_BODY;
	process(p, t);
}

static void m_after_after_frameset(struct parser *p, struct token *t)
{
	if (t->type == TOK_COMMENT) {
		insert_comment(p, t, p->doc);
		return;
	}
	if (t->type == TOK_DOCTYPE || ws_chars(t) || is_start(t, TAG_HTML)) {
		m_in_body(p, t);
		return;
	}
	if (is_start(t, TAG_NOFRAMES))
		m_in_head(p, t);
}

static void (*const modes[])(struct parser *, struct token *) = {
	m_initial, m_before_html, m_before_head, m_in_head,
	m_in_head_noscript, m_after_head, m_in_body, m_text, m_in_table,
	m_in_table_text, m_in_caption, m_in_column_group, m_in_table_body,
	m_in_row, m_in_cell, m_in_template, m_after_body, m_in_frameset,
	m_after_frameset, m_after_after_body, m_after_after_frameset
};

static void process(struct parser *p, struct token *t)
{
	modes[p->mode](p, t);
}

/* Foreign content (13.2.6.5) */

static void foreign(struct parser *p, struct token *t)
{
	soie_node *n;
	int i;

	switch (t->type) {
	case TOK_CHARS:
		if (nul_chars(t)) {
			for (i = 0; i < (int)t->nchars; i++)
				insert_chars(p, "\xef\xbf\xbd", 3);
			return;
		}
		insert_chars(p, t->chars, t->nchars);
		if (!ws_chars(t))
			p->frameset_ok = 0;
		return;
	case TOK_COMMENT:
		insert_comment(p, t, NULL);
		return;
	case TOK_DOCTYPE:
		return;
	case TOK_START:
		if (IN(t->tag, TAG_B, TAG_BIG, TAG_BLOCKQUOTE, TAG_BODY,
		       TAG_BR, TAG_CENTER, TAG_CODE, TAG_DD, TAG_DIV, TAG_DL,
		       TAG_DT, TAG_EM, TAG_EMBED, TAG_H1, TAG_H2, TAG_H3,
		       TAG_H4, TAG_H5, TAG_H6, TAG_HEAD, TAG_HR, TAG_I,
		       TAG_IMG, TAG_LI, TAG_LISTING, TAG_MENU, TAG_META,
		       TAG_NOBR, TAG_OL, TAG_P, TAG_PRE, TAG_RUBY, TAG_S,
		       TAG_SMALL, TAG_SPAN, TAG_STRONG, TAG_STRIKE, TAG_SUB,
		       TAG_SUP, TAG_TABLE, TAG_TT, TAG_U, TAG_UL, TAG_VAR) ||
		    (t->tag == TAG_FONT && (tok_attr(t, "color") ||
		     tok_attr(t, "face") || tok_attr(t, "size"))))
			break;
		n = adjusted_current(p);
		insert_foreign(p, t, n->ns);
		ack(t);
		return;
	case TOK_END:
		if (t->tag == TAG_BR || t->tag == TAG_P)
			break;
		n = current(p);
		if (t->tag == TAG_SCRIPT && n->ns == SOIE_NS_SVG &&
		    n->tag == TAG_SCRIPT) {
			pop(p);
			return;
		}
		for (i = p->nstack - 1; i > 0; i--) {
			n = p->stack[i];
			if (strcaseeq(n->name, t->name.s)) {
				pop_to(p, i);
				return;
			}
			if (p->stack[i - 1]->ns == SOIE_NS_HTML) {
				process(p, t);
				return;
			}
		}
		return;
	}
	/* Break out of foreign content. */
	while ((n = current(p)) && !is_mathml_text_ip(n) && !is_html_ip(n) &&
	       n->ns != SOIE_NS_HTML)
		pop(p);
	process(p, t);
}

/* Tree construction dispatcher (13.2.6) */

static void dispatch(struct parser *p, struct token *t)
{
	soie_node *n = adjusted_current(p);

	if (!n || n->ns == SOIE_NS_HTML || t->type == TOK_EOF ||
	    (is_mathml_text_ip(n) && (t->type == TOK_CHARS ||
	     (t->type == TOK_START && t->tag != TAG_MGLYPH &&
	      t->tag != TAG_MALIGNMARK))) ||
	    (n->ns == SOIE_NS_MATHML && n->tag == TAG_ANNOTATION_XML &&
	     is_start(t, TAG_SVG)) ||
	    (is_html_ip(n) && (t->type == TOK_START || t->type == TOK_CHARS)))
		process(p, t);
	else
		foreign(p, t);
}

void tree_token(struct parser *p, struct token *t)
{
	const char *s;
	size_t n;

	if (t->type != TOK_CHARS) {
		p->skip_lf = 0;
		dispatch(p, t);
		return;
	}
	s = t->chars;
	n = t->nchars;
	if (p->skip_lf) {
		p->skip_lf = 0;
		if (n && s[0] == '\n') {
			s++;
			n--;
		}
	}
	feed_chars(p, s, n, dispatch);
}

int tree_cdata_allowed(struct parser *p)
{
	soie_node *n = adjusted_current(p);

	return n && n->ns != SOIE_NS_HTML;
}

/* Entry points */

static void parser_run(struct parser *p, const char *buf, size_t len,
	const soie_html_options *opts)
{
	struct buf in = { 0 };
	const char *cs;
	size_t bom;
	int i;

	cs = charset_sniff((const unsigned char *)buf, len,
		opts ? opts->charset : NULL, &bom);
	charset_decode(cs, (const unsigned char *)buf + bom, len - bom, &in);
	if (!p->context)
		p->doc->u.doc.charset = cs;
	p->in = in.s;
	p->len = in.len;
	p->scripting = opts ? opts->scripting : 0;
	p->frameset_ok = 1;
	tokenize(p);
	for (i = 0; i < p->norphans; i++)
		if (!p->orphans[i]->parent)
			soie_node_free(p->orphans[i]);
	free(p->orphans);
	tokenizer_free(p);
	buf_free(&in);
	buf_free(&p->pending);
	free(p->stack);
	free(p->afe);
	free(p->tmodes);
}

soie_node *soie_html_parse(const char *buf, size_t len,
	const soie_html_options *opts)
{
	struct parser p;

	memset(&p, 0, sizeof(p));
	p.doc = soie_document_new();
	p.mode = M_INITIAL;
	parser_run(&p, buf, len, opts);
	return p.doc;
}

soie_node *soie_html_parse_fragment(soie_node *doc, soie_ns context_ns,
	const char *context_name, const char *buf, size_t len,
	const soie_html_options *opts)
{
	struct parser p;
	soie_node *frag;
	int tag;

	memset(&p, 0, sizeof(p));
	p.doc = doc;
	p.context = soie_element_new(doc, context_ns, context_name);
	tag = context_ns == SOIE_NS_HTML ? p.context->tag : TAG_UNKNOWN;
	switch (tag) {
	case TAG_TITLE:
	case TAG_TEXTAREA:
		p.state = S_RCDATA;
		break;
	case TAG_STYLE:
	case TAG_XMP:
	case TAG_IFRAME:
	case TAG_NOEMBED:
	case TAG_NOFRAMES:
		p.state = S_RAWTEXT;
		break;
	case TAG_SCRIPT:
		p.state = S_SCRIPT;
		break;
	case TAG_NOSCRIPT:
		if (opts && opts->scripting)
			p.state = S_RAWTEXT;
		break;
	case TAG_PLAINTEXT:
		p.state = S_PLAINTEXT;
		break;
	}
	p.root = create_element(&p, NULL, SOIE_NS_HTML, TAG_HTML);
	push(&p, p.root);
	if (tag == TAG_TEMPLATE)
		push_tmode(&p, M_IN_TEMPLATE);
	reset_insertion_mode(&p);
	parser_run(&p, buf, len, opts);
	frag = soie_document_fragment_new(doc);
	while (p.root->first)
		node_insert(frag, p.root->first, NULL);
	soie_node_free(p.root);
	soie_node_free(p.context);
	return frag;
}
