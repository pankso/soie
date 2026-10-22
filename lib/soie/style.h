/*
 * Soie styles: CSS cascade and computed values.
 */
#ifndef SOIE_STYLE_H
#define SOIE_STYLE_H

#include <stddef.h>
#include "dom.h"

/*
 * Fetch the resource at url (style sheets, for now). On success return
 * 0 with *buf a malloc'ed buffer the library frees, and *len its size.
 * The engine never opens a socket: this is the host's job.
 */
typedef int (*soie_fetch_fn)(void *data, const char *url, char **buf,
	size_t *len);

typedef struct {
	int width, height;	/* viewport in CSS px; 0 means 1024x768 */
	int dark;		/* prefers-color-scheme: dark */
	int print;		/* print media instead of screen */
	int scripting;		/* (scripting) media feature */
	const char *base_url;	/* document URL; <base href> overrides */
	soie_fetch_fn fetch;	/* NULL: no external style sheets */
	void *fetch_data;
	const char *user_css;	/* user style sheet, or NULL */
} soie_style_options;

/*
 * Run the cascade: collect the style sheets of doc (<style>, <link>,
 * @import), match them and compute the style of every element.
 * Running it again recomputes everything. opts may be NULL.
 */
int soie_style_document(soie_node *doc, const soie_style_options *opts);

/*
 * Computed value of a property ("margin-top", "--custom") on an element,
 * or on its pseudo-element ("before", "after", or NULL for none), as
 * getComputedStyle() would serialize it. Returns a string to free(),
 * or NULL for an unknown property or a missing style.
 */
char *soie_computed_value(const soie_node *el, const char *pseudo,
	const char *property);

/*
 * Like soie_node_dump(), with the computed values of the properties
 * named in props (comma separated) after each element's attributes.
 */
char *soie_node_dump_style(const soie_node *n, const char *props,
	size_t *len);

/* Whether a media query list matches the document's last cascade
 * (its viewport and preferences); matchMedia() for scripts. */
int soie_media_matches(const soie_node *doc, const char *query);

/* Resolve a URL reference against a base URL; free() the result. */
char *soie_url_resolve(const char *base, const char *ref);

#endif
