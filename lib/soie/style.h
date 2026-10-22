/*
 * Soie styles: CSS cascade and computed values.
 */
#ifndef SOIE_STYLE_H
#define SOIE_STYLE_H

#include <stddef.h>
#include "dom.h"

/* Resolve a URL reference against a base URL; free() the result. */
char *soie_url_resolve(const char *base, const char *ref);

#endif
