/*
 * SoieView, a GTK 3 widget showing web pages with Soie: what Toile
 * needs from a web view, the way WebKitWebView gives it. Each view has
 * its page and history; all of them share the network, the cookies and
 * the extensions. It scrolls (GtkScrollable): put it in a
 * GtkScrolledWindow for a scroll bar.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_VIEW_H
#define SOIE_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SOIE_TYPE_VIEW (soie_view_get_type())
G_DECLARE_FINAL_TYPE(SoieView, soie_view, SOIE, VIEW, GtkWidget)

/*
 * Properties, all read only but "scripts" (construct only, TRUE):
 *   "uri"       the page's address, the one being loaded first
 *   "title"     its title, or its address without one
 *   "loading"   TRUE while the page or its resources are on their way
 *   "can-go-back", "can-go-forward"
 * Signals:
 *   "link-hovered" (const char *uri): the link under the pointer, ""
 *                  when it left it
 *   "alert" (const char *text): a script's alert()
 */

/* A view; g_object_new(SOIE_TYPE_VIEW, "scripts", FALSE, NULL) for
 * one without JavaScript. */
GtkWidget *soie_view_new(void);

void soie_view_load_uri(SoieView *view, const char *uri);
/* What was typed in an address bar: a URL, a path, a host, or words
 * to search for ($SOIE_SEARCH, or DuckDuckGo Lite). */
void soie_view_go(SoieView *view, const char *typed);
void soie_view_reload(SoieView *view);
void soie_view_go_back(SoieView *view);
void soie_view_go_forward(SoieView *view);

const char *soie_view_get_uri(SoieView *view);
const char *soie_view_get_title(SoieView *view);
gboolean soie_view_is_loading(SoieView *view);
gboolean soie_view_can_go_back(SoieView *view);
gboolean soie_view_can_go_forward(SoieView *view);

/* Find text in the page: a new text shows its first match, the same
 * one again the next (dir 1) or the previous one (-1); highlighted
 * and scrolled to. Returns how many there are, the current one in
 * *current (from 1). NULL or "" ends it. */
int soie_view_find(SoieView *view, const char *text, int dir,
	int *current);

G_END_DECLS

#endif
