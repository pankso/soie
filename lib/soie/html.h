/*
 * Soie HTML parser: WHATWG tokenizer and tree construction.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_HTML_H
#define SOIE_HTML_H

#include <stddef.h>
#include "dom.h"

typedef struct {
	/* Charset label from the transport (HTTP Content-Type), or NULL
	 * to sniff it from the bytes. A BOM always wins. */
	const char *charset;
	/* Scripting flag of the spec: 1 parses <noscript> as raw text.
	 * Leave 0 unless a script engine runs the page. */
	int scripting;
} soie_html_options;

/* Parse a complete document. opts may be NULL. */
soie_node *soie_html_parse(const char *buf, size_t len,
	const soie_html_options *opts);

/*
 * Parse a fragment as if it were the content of an element named
 * context_name in namespace context_ns (what innerHTML does). The
 * returned document fragment belongs to doc.
 */
soie_node *soie_html_parse_fragment(soie_node *doc, soie_ns context_ns,
	const char *context_name, const char *buf, size_t len,
	const soie_html_options *opts);

/* Name of the charset that was used for the last parse of doc. */
const char *soie_document_charset(const soie_node *doc);

#endif
