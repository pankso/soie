/*
 * Soie DOM: a tree of opaque nodes, read and changed through functions.
 *
 * A document owns every node created for it, in the tree or not:
 * soie_node_free() on the document frees them all. A node taken out
 * of the tree with soie_node_remove() stays valid until then, so it
 * can be reinserted; soie_node_free() on it releases it earlier.
 */
#ifndef SOIE_DOM_H
#define SOIE_DOM_H

#include <stddef.h>

typedef struct soie_node soie_node;

typedef enum {
	SOIE_DOCUMENT = 1,
	SOIE_DOCUMENT_FRAGMENT,
	SOIE_DOCTYPE,
	SOIE_ELEMENT,
	SOIE_TEXT,
	SOIE_COMMENT
} soie_node_type;

typedef enum {
	SOIE_NS_NONE = 0,
	SOIE_NS_HTML,
	SOIE_NS_SVG,
	SOIE_NS_MATHML,
	SOIE_NS_XLINK,
	SOIE_NS_XML,
	SOIE_NS_XMLNS
} soie_ns;

typedef enum {
	SOIE_NO_QUIRKS = 0,
	SOIE_QUIRKS,
	SOIE_LIMITED_QUIRKS
} soie_quirks;

/* Tree navigation. All return NULL when there is no such node. */
soie_node_type soie_node_get_type(const soie_node *n);
soie_node *soie_node_document(const soie_node *n);
soie_node *soie_node_parent(const soie_node *n);
soie_node *soie_node_first_child(const soie_node *n);
soie_node *soie_node_last_child(const soie_node *n);
soie_node *soie_node_next_sibling(const soie_node *n);
soie_node *soie_node_prev_sibling(const soie_node *n);

/* Documents */
soie_node *soie_document_new(void);
soie_quirks soie_document_quirks(const soie_node *doc);
soie_node *soie_document_element(const soie_node *doc);
soie_node *soie_document_fragment_new(soie_node *doc);

/* Doctype: strings are never NULL, missing parts are "". */
const char *soie_doctype_name(const soie_node *n);
const char *soie_doctype_public_id(const soie_node *n);
const char *soie_doctype_system_id(const soie_node *n);

/* Elements. Names are local names ("foreignObject", "div"). */
soie_node *soie_element_new(soie_node *doc, soie_ns ns, const char *name);
const char *soie_element_name(const soie_node *n);
soie_ns soie_element_ns(const soie_node *n);
size_t soie_element_attr_count(const soie_node *n);
const char *soie_element_attr_name(const soie_node *n, size_t i);
soie_ns soie_element_attr_ns(const soie_node *n, size_t i);
const char *soie_element_attr_value(const soie_node *n, size_t i);
/* Attribute lookup and change, for attributes without namespace. */
const char *soie_element_get_attr(const soie_node *n, const char *name);
int soie_element_set_attr(soie_node *n, const char *name, const char *value);
int soie_element_remove_attr(soie_node *n, const char *name);
/* The content fragment of an HTML <template>, NULL for other nodes. */
soie_node *soie_template_content(const soie_node *n);

/* Text and comments. Data may contain NUL bytes, hence the length. */
soie_node *soie_text_new(soie_node *doc, const char *s, size_t len);
soie_node *soie_comment_new(soie_node *doc, const char *s, size_t len);
const char *soie_node_data(const soie_node *n, size_t *len);
int soie_node_set_data(soie_node *n, const char *s, size_t len);

/*
 * Mutation. Return 0 on success, -1 when the insertion would break
 * the tree (inserting an ancestor into its descendant, a node of
 * another document, a child into a text node...). A node that is
 * already in the tree is moved.
 */
int soie_node_append(soie_node *parent, soie_node *child);
int soie_node_insert_before(soie_node *parent, soie_node *child,
	soie_node *ref);
void soie_node_remove(soie_node *n);
/* Free a node and its subtree. The node is removed first if needed. */
void soie_node_free(soie_node *n);

/* A copy of n (and its subtree when deep), not in the tree. NULL for
 * a document. */
soie_node *soie_node_clone(const soie_node *n, int deep);

/*
 * Move n, out of its tree, with its subtree to document doc (DOM
 * adoptNode): from then on doc owns it and frees it. Its computed
 * styles are dropped, a player of a media element in it stops.
 * Returns -1 for a document. Since 1.1.
 */
int soie_node_adopt(soie_node *doc, soie_node *n);

/* One pointer per node for the host (a script engine's wrapper). */
void soie_node_set_user(soie_node *n, void *p);
void *soie_node_user(const soie_node *n);

/* A counter bumped by every change to the tree, attributes or text:
 * the host restyles when it moved. */
unsigned long soie_document_generation(const soie_node *doc);

/*
 * HTML serialization: the children of n (innerHTML), or n itself
 * with outer (outerHTML). A string to free(); len may be NULL.
 */
char *soie_node_serialize(const soie_node *n, int outer, size_t *len);

/*
 * Selectors: 1 when el matches, 0 when not, -1 for a selector list
 * that does not parse (a SyntaxError for scripts).
 */
int soie_element_matches(const soie_node *el, const char *selectors);
/*
 * The descendant elements of root that match, in tree order, at most
 * max of them (0: all): a NULL-terminated array to free(), count gets
 * their number. NULL for a selector list that does not parse.
 */
soie_node **soie_query(soie_node *root, const char *selectors, size_t max,
	size_t *count);

/*
 * Dump a node (or the children of a document or fragment) in the
 * html5lib tree-construction test format, one "| " line per node.
 * Returns a string to release with free(); len (may be NULL) gets its
 * length.
 */
char *soie_node_dump(const soie_node *n, size_t *len);

#endif
