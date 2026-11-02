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
 * SoieDownload: a page load that was a file to save (an archive, an
 * ISO image, Content-Disposition: attachment), as WebKitDownload. The
 * view gives it in "download-started"; the body goes to a file in the
 * cache as it comes, then to its destination. Signals:
 *   "progress" (): more came (get_received_data_length()), at most
 *                  four times a second
 *   "finished" (): the file is at its destination
 *   "failed" (const char *error): cancelled, cut, no room...
 * Downloads belong to the process, as to WebKit's context: closing the
 * view one came from does not stop it. Its signals come from the main
 * loop.
 */
#define SOIE_TYPE_DOWNLOAD (soie_download_get_type())
G_DECLARE_FINAL_TYPE(SoieDownload, soie_download, SOIE, DOWNLOAD, GObject)

const char *soie_download_get_uri(SoieDownload *dl);
const char *soie_download_get_mime_type(SoieDownload *dl);
/* ~/Downloads (or $XDG_DOWNLOAD_DIR) and the file's name, a free one,
 * until the browser sets another */
const char *soie_download_get_destination(SoieDownload *dl);
/* In "download-started" only: the file it goes to (NULL: none, it is
 * cancelled). */
void soie_download_set_destination(SoieDownload *dl, const char *path);
guint64 soie_download_get_received_data_length(SoieDownload *dl);
/* Content-Length, -1 when unknown */
gint64 soie_download_get_content_length(SoieDownload *dl);
void soie_download_cancel(SoieDownload *dl);

/*
 * Properties, all read only but "scripts" (construct only, TRUE):
 *   "uri"       the page's address, the one being loaded first
 *   "title"     its title, or its address without one
 *   "loading"   TRUE while the page or its resources are on their way
 *   "can-go-back", "can-go-forward"
 *   "zoom-level" (read and write: soie_view_set_zoom_level())
 * Signals:
 *   "link-hovered" (const char *uri): the link under the pointer, ""
 *                  when it left it
 *   "alert" (const char *text): a script's alert()
 *   "create" (const char *uri) -> GtkWidget *: a new window on uri (a
 *            link of target _blank, window.open() on a click, the
 *            middle button or Ctrl+click on a link), as WebKit's: the
 *            handler returns a new SoieView, which then loads uri
 *            (NULL: none opens); without a handler uri loads here
 *   "context-menu" (GtkMenu *menu, const char *link_uri,
 *            const char *image_uri) -> gboolean: the right button; menu
 *            has the default items (Back, Forward, Reload, Copy the
 *            selection, Copy Link Address, Copy Image Address, Paste
 *            in a field), the URIs
 *            are absolute or "": a handler adds its own items (Open in
 *            New Tab...) or returns TRUE for no menu
 *   "download-started" (SoieDownload *dl): a load was a download; the
 *            page on screen stays. Runs while the view works: set the
 *            destination, connect to dl's signals, keep a ref if you
 *            need it after; no dialog (nested main loop) and do not
 *            destroy the view there. Without a handler it goes to its
 *            default destination.
 * "loading" goes TRUE then FALSE for every load, also one that comes
 * at once (a page of the browser's own scheme, load_html).
 */

/* A view; g_object_new(SOIE_TYPE_VIEW, "scripts", FALSE, NULL) for
 * one without JavaScript, "private", TRUE for a private window's (its
 * cookies in memory only, apart from the other views', shared by the
 * private ones; as WebKit's ephemeral context). */
GtkWidget *soie_view_new(void);

void soie_view_load_uri(SoieView *view, const char *uri);
/* Show html as the page at base_uri (its links' base, the address
 * shown; NULL: about:blank), like webkit_web_view_load_html(). */
void soie_view_load_html(SoieView *view, const char *html,
	const char *base_uri);
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
/* The page's zoom, 1.0 is 100%, 0.25 to 5 ("zoom-level"); the page is
 * laid out again, the same place at the top. */
void soie_view_set_zoom_level(SoieView *view, double zoom);
double soie_view_get_zoom_level(SoieView *view);

/* Text selected in the page: with the mouse (a drag, a double click
 * for a word, a triple one for its block, Shift+click to go on), or
 * Ctrl+A; Ctrl+C or the context menu's Copy copies it. A selection
 * made becomes the PRIMARY one. */
gboolean soie_view_has_selection(SoieView *view);
/* g_free() it; NULL without one */
char *soie_view_get_selected_text(SoieView *view);
void soie_view_copy_clipboard(SoieView *view);
void soie_view_select_all(SoieView *view);

/*
 * A URL scheme of the browser's own ("toile"), for all views, like
 * webkit_web_context_register_uri_scheme() but answered at once: fn
 * gets the whole URI (its query too) in the main thread and returns
 * the page, its type in *content_type (g_malloc()ed, NULL is
 * "text/html"), or NULL for an error page. Pages of this scheme, the
 * address bar and history reach it; web pages cannot (no link, form,
 * resource or script request to it), as WebKit's display isolated
 * schemes.
 */
typedef GBytes *(*SoieURISchemeFunc)(const char *uri, char **content_type,
	gpointer data);
void soie_view_register_uri_scheme(const char *scheme, SoieURISchemeFunc fn,
	gpointer data);

/* Cookies, for all views (as WebKit's cookie manager): third-party
 * ones are refused by default. */
typedef enum {
	SOIE_COOKIE_POLICY_NO_THIRD_PARTY,
	SOIE_COOKIE_POLICY_ALL,
	SOIE_COOKIE_POLICY_NONE
} SoieCookiePolicy;
void soie_view_set_cookie_policy(SoieCookiePolicy policy);
/* Forget all cookies and HTTP logins: the private views' ones with
 * private_ones, else the others (and their cookie file). */
void soie_view_clear_cookies(gboolean private_ones);

/* Find text in the page: a new text shows its first match, the same
 * one again the next (dir 1) or the previous one (-1); highlighted
 * and scrolled to. Returns how many there are, the current one in
 * *current (from 1). NULL or "" ends it. */
int soie_view_find(SoieView *view, const char *text, int dir,
	int *current);

G_END_DECLS

#endif
