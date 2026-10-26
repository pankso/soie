/*
 * SoieView (soie-view.h): a shell (shell.h) in a GTK 3 widget. The page
 * is painted into a cairo image surface (B G R A, cairo's ARGB32 on a
 * little endian machine), again only where it changed; the shell's
 * descriptor and timers come through the GLib main loop; GTK's events
 * become the shell's input, keys through an input method (dead keys,
 * compose).
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdlib.h>
#include <string.h>
#include <glib-unix.h>
#include <soie/soie.h>
#include "shell.h"
#include "soie-view.h"

#define WHEEL 60		/* pixels a wheel step scrolls */
/* The page is painted in device pixels: the screen's scale (HiDPI)
 * times the user's zoom. A widget coordinate to the page view's. */
#define DEV(v, c) ((int)((c) * (v)->scale))
#define NRECT 8			/* rects of a partial repaint */

struct _SoieView {
	GtkWidget parent;
	shell *sh;
	gboolean scripts;
	gboolean private;	/* construct only */
	cairo_surface_t *surf;	/* the page view's pixels */
	int w, h;		/* widget size; the surface's is times scale */
	int scale;		/* gtk_widget_get_scale_factor() */
	double zoom;		/* the user's: "zoom-level" */
	gboolean full, changed;	/* repaint all of it, what changed */
	guint timer;		/* the shell's next tick */
	guint idle;		/* a tick after input */
	GtkIMContext *im;
	GtkAdjustment *hadj, *vadj;
	guint hpolicy, vpolicy;
	gboolean syncing;	/* setting vadj from the page */
	double smooth;		/* touchpad scrolling below a pixel */
	char *title, *link;
	char *alert;		/* told after the shell's call */
	char *open;		/* a new window asked for, the same */
	gboolean started;	/* a load started or a page came */
	int cursor;
	gboolean selecting;	/* button 1 down, selecting text */
	gboolean loading;
	/* what changed during the shell's calls, notified after them */
	gboolean title_changed, uri_changed;
	gboolean back, forward;
};

enum {
	PROP_0, PROP_URI, PROP_TITLE, PROP_LOADING, PROP_CAN_GO_BACK,
	PROP_CAN_GO_FORWARD, PROP_SCRIPTS, PROP_ZOOM, PROP_PRIVATE, N_PROPS,
	PROP_HADJ = N_PROPS, PROP_VADJ, PROP_HPOLICY, PROP_VPOLICY
};
static GParamSpec *props[N_PROPS];

enum { SIG_LINK_HOVERED, SIG_ALERT, SIG_CREATE, SIG_CONTEXT_MENU,
	SIG_DOWNLOAD_STARTED, N_SIGS };
static guint sigs[N_SIGS];

static void scrollable_init(GtkScrollableInterface *iface)
{
	(void)iface;
}

G_DEFINE_TYPE_WITH_CODE(SoieView, soie_view, GTK_TYPE_WIDGET,
	G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, scrollable_init))

/* Downloads */

struct _SoieDownload {
	GObject parent;
	unsigned id;
	char *uri, *mime, *dest;
	char *dest_buf;		/* the shell's, in "download-started" */
	size_t dest_size;
	gint64 total;
	guint64 got;
};

G_DEFINE_TYPE(SoieDownload, soie_download, G_TYPE_OBJECT)

enum { DL_PROGRESS, DL_FINISHED, DL_FAILED, N_DL_SIGS };
static guint dl_sigs[N_DL_SIGS];

static void soie_download_finalize(GObject *o)
{
	SoieDownload *d = SOIE_DOWNLOAD(o);

	g_free(d->uri);
	g_free(d->mime);
	g_free(d->dest);
	G_OBJECT_CLASS(soie_download_parent_class)->finalize(o);
}

static void soie_download_class_init(SoieDownloadClass *c)
{
	G_OBJECT_CLASS(c)->finalize = soie_download_finalize;
	dl_sigs[DL_PROGRESS] = g_signal_new("progress", G_TYPE_FROM_CLASS(c),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	dl_sigs[DL_FINISHED] = g_signal_new("finished", G_TYPE_FROM_CLASS(c),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	dl_sigs[DL_FAILED] = g_signal_new("failed", G_TYPE_FROM_CLASS(c),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
		G_TYPE_STRING);
}

static void soie_download_init(SoieDownload *d)
{
	d->total = -1;
}

const char *soie_download_get_uri(SoieDownload *d)
{
	g_return_val_if_fail(SOIE_IS_DOWNLOAD(d), NULL);
	return d->uri;
}

const char *soie_download_get_mime_type(SoieDownload *d)
{
	g_return_val_if_fail(SOIE_IS_DOWNLOAD(d), NULL);
	return d->mime;
}

const char *soie_download_get_destination(SoieDownload *d)
{
	g_return_val_if_fail(SOIE_IS_DOWNLOAD(d), NULL);
	return d->dest_buf ? d->dest_buf : d->dest;
}

void soie_download_set_destination(SoieDownload *d, const char *path)
{
	g_return_if_fail(SOIE_IS_DOWNLOAD(d));
	if (!d->dest_buf) {
		g_warning("soie_download_set_destination() outside "
			"\"download-started\"");
		return;
	}
	g_snprintf(d->dest_buf, d->dest_size, "%s", path ? path : "");
}

guint64 soie_download_get_received_data_length(SoieDownload *d)
{
	g_return_val_if_fail(SOIE_IS_DOWNLOAD(d), 0);
	return d->got;
}

gint64 soie_download_get_content_length(SoieDownload *d)
{
	g_return_val_if_fail(SOIE_IS_DOWNLOAD(d), -1);
	return d->total;
}

void soie_download_cancel(SoieDownload *d)
{
	g_return_if_fail(SOIE_IS_DOWNLOAD(d));
	shell_download_cancel(d->id);
}

/* The downloads on their way (id -> SoieDownload), the process's as
 * the shell's: a view closed does not stop them */
static GHashTable *downloads;

/* News of a download, told from an idle: outside the shell's calls */
struct dl_event {
	SoieDownload *d;
	int sig;
	char *error;
};
static GList *dl_events;
static guint dl_idle;

static gboolean tell_downloads(gpointer data);

static void dl_event(SoieDownload *d, int sig, const char *error)
{
	struct dl_event *e;
	GList *l;

	/* one progress waiting is enough */
	for (l = dl_events; sig == DL_PROGRESS && l; l = l->next)
		if (((struct dl_event *)l->data)->d == d &&
		    ((struct dl_event *)l->data)->sig == DL_PROGRESS)
			return;
	e = g_new0(struct dl_event, 1);
	e->d = g_object_ref(d);
	e->sig = sig;
	e->error = g_strdup(error);
	dl_events = g_list_append(dl_events, e);
	if (!dl_idle)
		dl_idle = g_idle_add(tell_downloads, NULL);
}

static void ui_download(void *data, unsigned id, const char *url,
	const char *type, long long total, char *dest, size_t size)
{
	SoieView *v = data;
	SoieDownload *d = g_object_new(SOIE_TYPE_DOWNLOAD, NULL);

	d->id = id;
	d->uri = g_strdup(url);
	d->mime = g_strdup(type);
	d->total = total;
	d->dest_buf = dest;
	d->dest_size = size;
	g_signal_emit(v, sigs[SIG_DOWNLOAD_STARTED], 0, d);
	d->dest_buf = NULL;
	d->dest = g_strdup(dest);
	g_hash_table_insert(downloads, GUINT_TO_POINTER(id), d);
}

static void download_progress(void *data, unsigned id, long long got,
	long long total)
{
	SoieDownload *d = g_hash_table_lookup(downloads,
		GUINT_TO_POINTER(id));

	(void)data;
	if (!d)
		return;
	d->got = (guint64)got;
	d->total = total;
	dl_event(d, DL_PROGRESS, NULL);
}

static void download_done(void *data, unsigned id, const char *error)
{
	SoieDownload *d = g_hash_table_lookup(downloads,
		GUINT_TO_POINTER(id));

	(void)data;
	if (!d)
		return;
	if (!error)
		d->got = d->total >= 0 ? (guint64)d->total : d->got;
	dl_event(d, error ? DL_FAILED : DL_FINISHED, error);
	g_hash_table_remove(downloads, GUINT_TO_POINTER(id));
}

/* Tell the downloads' news */
static gboolean tell_downloads(gpointer data)
{
	GList *l = dl_events;

	(void)data;
	dl_idle = 0;
	dl_events = NULL;
	for (; l; l = g_list_delete_link(l, l)) {
		struct dl_event *e = l->data;

		if (e->sig == DL_FAILED)
			g_signal_emit(e->d, dl_sigs[DL_FAILED], 0, e->error);
		else
			g_signal_emit(e->d, dl_sigs[e->sig], 0);
		g_object_unref(e->d);
		g_free(e->error);
		g_free(e);
	}
	return G_SOURCE_REMOVE;
}

/* All views: the shells' descriptor is theirs */
static GList *views;
static guint fd_watch;

static void tick(SoieView *v);

/* The shell's calls */

static void ui_title(void *data, const char *title)
{
	SoieView *v = data;

	g_free(v->title);
	v->title = g_strdup(title);
	v->title_changed = TRUE;
}

static void ui_address(void *data, const char *url)
{
	SoieView *v = data;

	(void)url;
	v->uri_changed = TRUE;
	v->started = TRUE;
}

static void ui_alert(void *data, const char *text)
{
	SoieView *v = data;

	g_free(v->alert);
	v->alert = g_strdup(text);
}

static void ui_open(void *data, const char *url)
{
	SoieView *v = data;

	g_free(v->open);
	v->open = g_strdup(url);
}

static const struct shell_ui ui = { ui_title, ui_address, ui_alert,
	ui_open, ui_download };

/*
 * A new window on uri: the handlers of "create" give the view, which
 * then loads it; without a handler it loads here, with one that gives
 * none it does not at all (the browser said no).
 */
static void open_new(SoieView *v, const char *uri)
{
	GtkWidget *w = NULL;

	if (!shell_may_open(v->sh, uri))
		return;
	if (!g_signal_has_handler_pending(v, sigs[SIG_CREATE], 0, FALSE)) {
		soie_view_load_uri(v, uri);
		return;
	}
	g_signal_emit(v, sigs[SIG_CREATE], 0, uri, &w);
	if (w && SOIE_IS_VIEW(w))
		soie_view_load_uri(SOIE_VIEW(w), uri);
}

/*
 * What the shell's calls changed, told once they returned: a handler
 * may do anything, even destroy the view, but not inside the shell.
 */
static void tell(SoieView *v)
{
	const char *text;
	gboolean loading = shell_loading(v->sh, &text) > 0;
	gboolean back = shell_can_go(v->sh, -1) != 0;
	gboolean fwd = shell_can_go(v->sh, 1) != 0;
	char *alert = v->alert, *open = v->open;

	g_object_ref(v);
	/* a load that came at once was a load all the same: loading goes
	 * TRUE then FALSE, as WebKit's load-changed */
	if (v->started && !v->loading) {
		v->loading = TRUE;
		g_object_notify_by_pspec(G_OBJECT(v), props[PROP_LOADING]);
	}
	v->started = FALSE;
	g_object_freeze_notify(G_OBJECT(v));
	if (v->uri_changed)
		g_object_notify_by_pspec(G_OBJECT(v), props[PROP_URI]);
	if (v->title_changed)
		g_object_notify_by_pspec(G_OBJECT(v), props[PROP_TITLE]);
	v->uri_changed = v->title_changed = FALSE;
	if (loading != v->loading) {
		v->loading = loading;
		g_object_notify_by_pspec(G_OBJECT(v), props[PROP_LOADING]);
	}
	if (back != v->back) {
		v->back = back;
		g_object_notify_by_pspec(G_OBJECT(v),
			props[PROP_CAN_GO_BACK]);
	}
	if (fwd != v->forward) {
		v->forward = fwd;
		g_object_notify_by_pspec(G_OBJECT(v),
			props[PROP_CAN_GO_FORWARD]);
	}
	g_object_thaw_notify(G_OBJECT(v));
	v->alert = v->open = NULL;
	if (alert)
		g_signal_emit(v, sigs[SIG_ALERT], 0, alert);
	if (open && v->sh)
		open_new(v, open);
	g_free(alert);
	g_free(open);
	g_object_unref(v);
}

/* Painting */

/* The scroll position and page height to the vertical adjustment */
static void sync_adjustment(SoieView *v)
{
	int scroll, height, view;

	if (!v->vadj || !v->sh)
		return;
	shell_view(v->sh, &scroll, &height, &view);
	/* the adjustments are in widget pixels */
	scroll /= v->scale;
	height /= v->scale;
	view /= v->scale;
	v->syncing = TRUE;
	/* no horizontal scrolling: the range is the view's width */
	if (v->hadj)
		gtk_adjustment_configure(v->hadj, 0, 0, v->w, 10, v->w, v->w);
	gtk_adjustment_configure(v->vadj, scroll, 0,
		height > view ? height : view, 40, view > 80 ? view - 40 : view,
		view);
	v->syncing = FALSE;
}

/* A rect of the page view shown again: in widget pixels */
static void show_rect(SoieView *v, int x, int y, int w, int h)
{
	int s = v->scale;

	gtk_widget_queue_draw_area(GTK_WIDGET(v), x / s, y / s,
		(x + w + s - 1) / s - x / s, (y + h + s - 1) / s - y / s);
}

/* Paint what changed and ask GTK to show it. */
static void redraw(SoieView *v)
{
	GtkWidget *w = GTK_WIDGET(v);
	int stride, dy, n, i, r[NRECT][4];
	unsigned char *px;

	if (!v->surf)
		return;
	cairo_surface_flush(v->surf);
	px = cairo_image_surface_get_data(v->surf);
	stride = cairo_image_surface_get_stride(v->surf);
	if (v->changed && !v->full &&
	    (n = shell_paint_changed(v->sh, px, stride, r, NRECT)) >= 0) {
		for (i = 0; i < n; i++)
			show_rect(v, r[i][0], r[i][1], r[i][2], r[i][3]);
	} else if (v->full || v->changed ||
	    !shell_paint_scroll(v->sh, px, stride, &dy)) {
		shell_paint(v->sh, px, stride);
		gtk_widget_queue_draw(w);
	} else if (dy) {
		gtk_widget_queue_draw(w);
	}
	v->full = v->changed = FALSE;
	cairo_surface_mark_dirty(v->surf);
	sync_adjustment(v);
}

static gboolean draw(GtkWidget *w, cairo_t *cr)
{
	SoieView *v = SOIE_VIEW(w);

	if (!v->surf) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_paint(cr);
		return TRUE;
	}
	cairo_set_source_surface(cr, v->surf, 0, 0);
	cairo_paint(cr);
	return TRUE;
}

static void size_allocate(GtkWidget *w, GtkAllocation *a)
{
	SoieView *v = SOIE_VIEW(w);

	int s = gtk_widget_get_scale_factor(w);

	GTK_WIDGET_CLASS(soie_view_parent_class)->size_allocate(w, a);
	if (s < 1)
		s = 1;
	if (a->width == v->w && a->height == v->h && s == v->scale && v->surf)
		return;
	v->w = a->width > 1 ? a->width : 1;
	v->h = a->height > 1 ? a->height : 1;
	if (s != v->scale) {
		/* another screen: the page in its pixels */
		v->scale = s;
		shell_zoom(v->sh, (float)(v->zoom * s));
	}
	if (v->surf)
		cairo_surface_destroy(v->surf);
	v->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, v->w * s,
		v->h * s);
	cairo_surface_set_device_scale(v->surf, s, s);
	shell_resize(v->sh, v->w * s, v->h * s);
	v->full = TRUE;
	redraw(v);
	tell(v);
}

static void get_preferred_width(GtkWidget *w, int *min, int *nat)
{
	(void)w;
	*min = 100;
	*nat = 800;
}

static void get_preferred_height(GtkWidget *w, int *min, int *nat)
{
	(void)w;
	*min = 50;
	*nat = 600;
}

static void realize(GtkWidget *w)
{
	SoieView *v = SOIE_VIEW(w);
	GtkAllocation a;
	GdkWindowAttr at;
	GdkWindow *win;

	gtk_widget_set_realized(w, TRUE);
	gtk_widget_get_allocation(w, &a);
	memset(&at, 0, sizeof(at));
	at.window_type = GDK_WINDOW_CHILD;
	at.x = a.x;
	at.y = a.y;
	at.width = a.width;
	at.height = a.height;
	at.wclass = GDK_INPUT_OUTPUT;
	at.visual = gtk_widget_get_visual(w);
	at.event_mask = gtk_widget_get_events(w) | GDK_EXPOSURE_MASK |
		GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
		GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
		GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK |
		GDK_KEY_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK |
		GDK_FOCUS_CHANGE_MASK;
	win = gdk_window_new(gtk_widget_get_parent_window(w), &at,
		GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
	gtk_widget_set_window(w, win);
	gtk_widget_register_window(w, win);
	gtk_im_context_set_client_window(v->im, win);
}

static void unrealize(GtkWidget *w)
{
	SoieView *v = SOIE_VIEW(w);

	gtk_im_context_set_client_window(v->im, NULL);
	GTK_WIDGET_CLASS(soie_view_parent_class)->unrealize(w);
}

/* The loop */

static gboolean on_timer(gpointer data)
{
	SoieView *v = data;

	v->timer = 0;
	tick(v);
	return G_SOURCE_REMOVE;
}

static gboolean on_idle(gpointer data)
{
	SoieView *v = data;

	v->idle = 0;
	tick(v);
	return G_SOURCE_REMOVE;
}

/* After input: the scripts' new timers, a load started */
static void tick_soon(SoieView *v)
{
	if (!v->idle)
		v->idle = g_idle_add(on_idle, v);
}

/* Run what the shell has due, show what changed, wait for the next. */
static void tick(SoieView *v)
{
	const char *text;
	int repaint, t, x, y, w, h;

	t = shell_tick(v->sh, &repaint);
	if (repaint == 2 && v->surf && shell_dirty(v->sh, &x, &y, &w, &h)) {
		/* video frames */
		cairo_surface_flush(v->surf);
		shell_paint_rect(v->sh, cairo_image_surface_get_data(v->surf),
			cairo_image_surface_get_stride(v->surf), x, y, w, h);
		cairo_surface_mark_dirty(v->surf);
		show_rect(v, x, y, w, h);
	} else if (repaint) {
		v->changed = TRUE;
	}
	redraw(v);
	/* loading is told as it goes */
	if (shell_loading(v->sh, &text) && (t < 0 || t > 500))
		t = 500;
	if (v->timer)
		g_source_remove(v->timer);
	v->timer = t >= 0 ? g_timeout_add(t, on_timer, v) : 0;
	tell(v);
}

/* Resources came: the shells take theirs */
static gboolean on_fd(gint fd, GIOCondition c, gpointer data)
{
	GList *l, *all;

	(void)fd;
	(void)c;
	(void)data;
	shell_route();
	/* a view a handler destroys is not ticked */
	all = g_list_copy(views);
	for (l = all; l; l = l->next)
		g_object_ref(l->data);
	for (l = all; l; l = l->next) {
		if (g_list_find(views, l->data))
			tick(l->data);
		g_object_unref(l->data);
	}
	g_list_free(all);
	return G_SOURCE_CONTINUE;
}

/* Input */

static int mods_of(guint state)
{
	return (state & GDK_SHIFT_MASK ? SOIE_MOD_SHIFT : 0) |
		(state & GDK_CONTROL_MASK ? SOIE_MOD_CTRL : 0) |
		(state & GDK_MOD1_MASK ? SOIE_MOD_ALT : 0) |
		(state & (GDK_SUPER_MASK | GDK_META_MASK) ? SOIE_MOD_META : 0);
}

/* The DOM key value of a keyval: a character key is its character;
 * NULL for a key the page does not know. */
static const char *key_name(guint kv, char *buf, size_t size)
{
	gunichar c;
	int n;

	switch (kv) {
	case GDK_KEY_Return: case GDK_KEY_KP_Enter: return "Enter";
	case GDK_KEY_BackSpace: return "Backspace";
	case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return "Tab";
	case GDK_KEY_Escape: return "Escape";
	case GDK_KEY_Delete: case GDK_KEY_KP_Delete: return "Delete";
	case GDK_KEY_Insert: return "Insert";
	case GDK_KEY_Left: case GDK_KEY_KP_Left: return "ArrowLeft";
	case GDK_KEY_Right: case GDK_KEY_KP_Right: return "ArrowRight";
	case GDK_KEY_Up: case GDK_KEY_KP_Up: return "ArrowUp";
	case GDK_KEY_Down: case GDK_KEY_KP_Down: return "ArrowDown";
	case GDK_KEY_Home: case GDK_KEY_KP_Home: return "Home";
	case GDK_KEY_End: case GDK_KEY_KP_End: return "End";
	case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: return "PageUp";
	case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: return "PageDown";
	case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: return "Shift";
	case GDK_KEY_Control_L: case GDK_KEY_Control_R: return "Control";
	case GDK_KEY_Alt_L: case GDK_KEY_Alt_R: return "Alt";
	case GDK_KEY_ISO_Level3_Shift: case GDK_KEY_Mode_switch:
		return "AltGraph";
	case GDK_KEY_Meta_L: case GDK_KEY_Meta_R: case GDK_KEY_Super_L:
	case GDK_KEY_Super_R: return "Meta";
	case GDK_KEY_Caps_Lock: return "CapsLock";
	case GDK_KEY_Num_Lock: return "NumLock";
	}
	if (kv >= GDK_KEY_F1 && kv <= GDK_KEY_F12) {
		g_snprintf(buf, size, "F%u", kv - GDK_KEY_F1 + 1);
		return buf;
	}
	if ((c = gdk_keyval_to_unicode(kv)) >= 0x20 && c != 0x7f &&
	    (n = g_unichar_to_utf8(c, buf)) > 0 && (size_t)n < size) {
		buf[n] = '\0';
		return buf;
	}
	return NULL;
}

/* A key taken by the shell, its changes shown */
static int key(SoieView *v, const char *k, const char *text, int mods)
{
	int r = shell_key(v->sh, k, text, mods);

	if (r)
		redraw(v);
	tick_soon(v);
	return r;
}

/* The input method's text: typed characters, one key each */
static void on_commit(GtkIMContext *im, const char *text, gpointer data)
{
	SoieView *v = data;
	const char *p;
	char c[8];

	(void)im;
	for (p = text; *p; p = g_utf8_next_char(p)) {
		size_t n = (size_t)(g_utf8_next_char(p) - p);

		memcpy(c, p, n);
		c[n] = '\0';
		key(v, c, c, 0);
	}
}

static void pasted(GtkClipboard *cb, const char *text, gpointer data)
{
	SoieView *v = data;

	(void)cb;
	if (text && shell_paste(v->sh, text))
		redraw(v);
	tick_soon(v);
	g_object_unref(v);
}

static gboolean key_press(GtkWidget *w, GdkEventKey *e)
{
	SoieView *v = SOIE_VIEW(w);
	int mods = mods_of(e->state);
	const char *k;
	char buf[16];

	/* typing (not shortcuts) through the input method */
	if (!(mods & (SOIE_MOD_CTRL | SOIE_MOD_ALT | SOIE_MOD_META)) &&
	    gtk_im_context_filter_keypress(v->im, e))
		return TRUE;
	/* Ctrl+V, Shift+Insert into a text field of the page */
	if (shell_field(v->sh) && (((mods & SOIE_MOD_CTRL) &&
	    (e->keyval == GDK_KEY_v || e->keyval == GDK_KEY_V)) ||
	    ((mods & SOIE_MOD_SHIFT) && e->keyval == GDK_KEY_Insert))) {
		gtk_clipboard_request_text(gtk_widget_get_clipboard(w,
			e->keyval == GDK_KEY_Insert ? GDK_SELECTION_PRIMARY :
			GDK_SELECTION_CLIPBOARD), pasted, g_object_ref(v));
		return TRUE;
	}
	/* Ctrl+C, Ctrl+A on the page's text */
	if (!shell_field(v->sh) && (mods & SOIE_MOD_CTRL) &&
	    (e->keyval == GDK_KEY_c || e->keyval == GDK_KEY_C)) {
		if (!soie_view_has_selection(v))
			return FALSE;
		soie_view_copy_clipboard(v);
		return TRUE;
	}
	if (!shell_field(v->sh) && (mods & SOIE_MOD_CTRL) &&
	    (e->keyval == GDK_KEY_a || e->keyval == GDK_KEY_A)) {
		soie_view_select_all(v);
		return TRUE;
	}
	if (!(k = key_name(e->keyval, buf, sizeof(buf))))
		return FALSE;
	/* shortcuts type nothing; what the page does not take goes on
	 * to the window (its accelerators) */
	return key(v, k, mods & (SOIE_MOD_CTRL | SOIE_MOD_ALT |
		SOIE_MOD_META) ? "" : k, mods) != 0;
}

static gboolean key_release(GtkWidget *w, GdkEventKey *e)
{
	return gtk_im_context_filter_keypress(SOIE_VIEW(w)->im, e);
}

static gboolean focus_in(GtkWidget *w, GdkEventFocus *e)
{
	(void)e;
	gtk_im_context_focus_in(SOIE_VIEW(w)->im);
	return FALSE;
}

static gboolean focus_out(GtkWidget *w, GdkEventFocus *e)
{
	(void)e;
	gtk_im_context_focus_out(SOIE_VIEW(w)->im);
	return FALSE;
}

static void set_cursor(SoieView *v, int shape)
{
	GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(v));
	GdkCursor *c;

	if (shape == v->cursor || !win)
		return;
	v->cursor = shape;
	c = gdk_cursor_new_from_name(gdk_window_get_display(win),
		shape == SHELL_HAND ? "pointer" : shape == SHELL_TEXT ?
		"text" : "default");
	gdk_window_set_cursor(win, c);
	if (c)
		g_object_unref(c);
}

static void hovered(SoieView *v, const char *link)
{
	if (!g_strcmp0(link, v->link ? v->link : ""))
		return;
	g_free(v->link);
	v->link = g_strdup(link);
	g_signal_emit(v, sigs[SIG_LINK_HOVERED], 0, link);
}

static gboolean motion(GtkWidget *w, GdkEventMotion *e)
{
	SoieView *v = SOIE_VIEW(w);
	char link[4096];

	if (v->selecting && (e->state & GDK_BUTTON1_MASK)) {
		/* past the top or the bottom, the page comes */
		if (e->y < 0 || e->y >= v->h)
			shell_scroll(v->sh, (int)(e->y < 0 ? e->y :
				e->y - v->h) * v->scale);
		if (shell_select_to(v->sh, DEV(v, e->x), DEV(v, e->y))) {
			v->full = TRUE;
			redraw(v);
		}
		return TRUE;
	}
	set_cursor(v, shell_hover(v->sh, DEV(v, e->x), DEV(v, e->y), link,
		sizeof(link)));
	hovered(v, link);
	return TRUE;
}

static gboolean leave(GtkWidget *w, GdkEventCrossing *e)
{
	(void)e;
	hovered(SOIE_VIEW(w), "");
	return FALSE;
}

/* The context menu */

static void copy_text(GtkMenuItem *item, gpointer data)
{
	(void)data;
	gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
		g_object_get_data(G_OBJECT(item), "text"), -1);
}

static void menu_back(GtkMenuItem *item, gpointer v)
{
	(void)item;
	soie_view_go_back(v);
}

static void menu_forward(GtkMenuItem *item, gpointer v)
{
	(void)item;
	soie_view_go_forward(v);
}

static void menu_reload(GtkMenuItem *item, gpointer v)
{
	(void)item;
	soie_view_reload(v);
}

static void menu_paste(GtkMenuItem *item, gpointer v)
{
	(void)item;
	gtk_clipboard_request_text(gtk_widget_get_clipboard(v,
		GDK_SELECTION_CLIPBOARD), pasted, g_object_ref(v));
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label,
	GCallback fn, gpointer data, gboolean on)
{
	GtkWidget *i = gtk_menu_item_new_with_mnemonic(label);

	g_signal_connect(i, "activate", fn, data);
	gtk_widget_set_sensitive(i, on);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
	return i;
}

static void copy_item(GtkWidget *menu, const char *label, const char *text)
{
	GtkWidget *i = menu_item(menu, label, G_CALLBACK(copy_text), NULL,
		TRUE);

	g_object_set_data_full(G_OBJECT(i), "text", g_strdup(text), g_free);
}

static gboolean menu_gone(gpointer menu)
{
	gtk_widget_destroy(menu);
	g_object_unref(menu);
	return G_SOURCE_REMOVE;
}

/* After its item ran: an item's activate comes after the hide */
static void menu_hidden(GtkWidget *menu, gpointer data)
{
	(void)data;
	g_idle_add(menu_gone, menu);
}

/*
 * The default items for what is at x,y, then the browser's handlers of
 * "context-menu" (add theirs, or TRUE: no menu), then the menu.
 */
static void context_menu(SoieView *v, GdkEventButton *e)
{
	GtkWidget *menu = gtk_menu_new();
	char link[4096], image[4096], *text;
	gboolean field, stop = FALSE;

	g_object_ref_sink(menu);
	field = shell_hit(v->sh, DEV(v, e->x), DEV(v, e->y), link, image,
		sizeof(link));
	menu_item(menu, "_Back", G_CALLBACK(menu_back), v,
		soie_view_can_go_back(v));
	menu_item(menu, "_Forward", G_CALLBACK(menu_forward), v,
		soie_view_can_go_forward(v));
	menu_item(menu, "_Reload", G_CALLBACK(menu_reload), v, TRUE);
	text = shell_selection(v->sh);
	if (*link || *image || field || text)
		gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			gtk_separator_menu_item_new());
	if (text)
		copy_item(menu, "_Copy", text);
	free(text);
	if (*link)
		copy_item(menu, "Copy _Link Address", link);
	if (*image)
		copy_item(menu, "Copy _Image Address", image);
	if (field)
		menu_item(menu, "_Paste", G_CALLBACK(menu_paste), v, TRUE);
	g_signal_emit(v, sigs[SIG_CONTEXT_MENU], 0, menu, link, image, &stop);
	if (stop) {
		gtk_widget_destroy(menu);
		g_object_unref(menu);
		return;
	}
	gtk_widget_show_all(menu);
	g_signal_connect(menu, "hide", G_CALLBACK(menu_hidden), NULL);
	gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(v), NULL);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
}

static gboolean button_press(GtkWidget *w, GdkEventButton *e)
{
	SoieView *v = SOIE_VIEW(w);
	char link[4096];

	/* a double click selects a word, a triple one its block; the
	 * first click did the rest */
	if (e->button == 1 && (e->type == GDK_2BUTTON_PRESS ||
	    e->type == GDK_3BUTTON_PRESS)) {
		v->selecting = shell_select_start(v->sh, DEV(v, e->x),
			DEV(v, e->y), e->type == GDK_2BUTTON_PRESS ?
			SOIE_SELECT_WORD : SOIE_SELECT_BLOCK);
		v->full = TRUE;
		redraw(v);
		return TRUE;
	}
	if (e->type != GDK_BUTTON_PRESS)
		return FALSE;
	gtk_widget_grab_focus(w);
	switch (e->button) {
	case 1:
		/* Ctrl+click: a link in a new window too */
		if ((e->state & GDK_CONTROL_MASK) && shell_hover(v->sh,
		    DEV(v, e->x), DEV(v, e->y), link, sizeof(link)) ==
		    SHELL_HAND && *link) {
			open_new(v, link);
			return TRUE;
		}
		/* Shift+click: the selection goes on to there */
		if ((e->state & GDK_SHIFT_MASK) && shell_select_to(v->sh,
		    DEV(v, e->x), DEV(v, e->y))) {
			v->selecting = TRUE;
			v->full = TRUE;
			break;
		}
		shell_click(v->sh, DEV(v, e->x), DEV(v, e->y), mods_of(e->state));
		/* a press clears the selection, starts one off links and
		 * fields */
		shell_select_start(v->sh, DEV(v, e->x), DEV(v, e->y),
			SOIE_SELECT_CHAR);
		v->selecting = TRUE;
		v->full = TRUE;
		break;
	case 2:
		/* a link in a new window */
		if (shell_hover(v->sh, DEV(v, e->x), DEV(v, e->y), link,
		    sizeof(link)) == SHELL_HAND && *link) {
			open_new(v, link);
			return TRUE;
		}
		/* the PRIMARY selection into a text field */
		shell_click(v->sh, DEV(v, e->x), DEV(v, e->y), mods_of(e->state));
		if (shell_field(v->sh))
			gtk_clipboard_request_text(gtk_widget_get_clipboard(w,
				GDK_SELECTION_PRIMARY), pasted,
				g_object_ref(v));
		break;
	case 3:
		context_menu(v, e);
		return TRUE;
	case 8:
		shell_history(v->sh, -1);
		break;
	case 9:
		shell_history(v->sh, 1);
		break;
	default:
		return FALSE;
	}
	redraw(v);
	tick_soon(v);
	return TRUE;
}

/* The selection made becomes the PRIMARY one, for a middle click */
static gboolean button_release(GtkWidget *w, GdkEventButton *e)
{
	SoieView *v = SOIE_VIEW(w);
	char *text;

	if (e->button != 1 || !v->selecting)
		return FALSE;
	v->selecting = FALSE;
	if ((text = shell_selection(v->sh))) {
		gtk_clipboard_set_text(gtk_widget_get_clipboard(w,
			GDK_SELECTION_PRIMARY), text, -1);
		free(text);
	}
	return TRUE;
}

static gboolean scroll(GtkWidget *w, GdkEventScroll *e)
{
	SoieView *v = SOIE_VIEW(w);
	double dx, dy;
	int d = 0;

	switch (e->direction) {
	case GDK_SCROLL_UP:
		d = -WHEEL;
		break;
	case GDK_SCROLL_DOWN:
		d = WHEEL;
		break;
	case GDK_SCROLL_SMOOTH:
		gdk_event_get_scroll_deltas((GdkEvent *)e, &dx, &dy);
		v->smooth += dy * WHEEL;
		d = (int)v->smooth;
		v->smooth -= d;
		break;
	default:
		return FALSE;
	}
	if (d) {
		shell_scroll(v->sh, d * v->scale);
		redraw(v);
	}
	return TRUE;
}

/* Scrolling from outside: a GtkScrolledWindow's scroll bar */
static void vadj_changed(GtkAdjustment *a, gpointer data)
{
	SoieView *v = data;

	if (v->syncing)
		return;
	shell_scroll_to(v->sh, (int)(gtk_adjustment_get_value(a) * v->scale));
	redraw(v);
}

static void set_vadj(SoieView *v, GtkAdjustment *a)
{
	if (a == v->vadj)
		return;
	if (v->vadj) {
		g_signal_handlers_disconnect_by_data(v->vadj, v);
		g_object_unref(v->vadj);
	}
	if (!a)
		a = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
	v->vadj = g_object_ref_sink(a);
	g_signal_connect(a, "value-changed", G_CALLBACK(vadj_changed), v);
	sync_adjustment(v);
}

static void set_hadj(SoieView *v, GtkAdjustment *a)
{
	/* no horizontal scrolling yet: the view's width, sync_adjustment() */
	if (a == v->hadj)
		return;
	if (v->hadj)
		g_object_unref(v->hadj);
	if (!a)
		a = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
	v->hadj = g_object_ref_sink(a);
	gtk_adjustment_configure(a, 0, 0, v->w, 10, v->w, v->w);
}

/* The object */

static void get_property(GObject *o, guint id, GValue *val, GParamSpec *ps)
{
	SoieView *v = SOIE_VIEW(o);

	switch (id) {
	case PROP_URI:
		g_value_set_string(val, soie_view_get_uri(v));
		break;
	case PROP_TITLE:
		g_value_set_string(val, soie_view_get_title(v));
		break;
	case PROP_LOADING:
		g_value_set_boolean(val, v->loading);
		break;
	case PROP_CAN_GO_BACK:
		g_value_set_boolean(val, soie_view_can_go_back(v));
		break;
	case PROP_CAN_GO_FORWARD:
		g_value_set_boolean(val, soie_view_can_go_forward(v));
		break;
	case PROP_SCRIPTS:
		g_value_set_boolean(val, v->scripts);
		break;
	case PROP_ZOOM:
		g_value_set_double(val, soie_view_get_zoom_level(v));
		break;
	case PROP_PRIVATE:
		g_value_set_boolean(val, v->private);
		break;
	case PROP_HADJ:
		g_value_set_object(val, v->hadj);
		break;
	case PROP_VADJ:
		g_value_set_object(val, v->vadj);
		break;
	case PROP_HPOLICY:
		g_value_set_enum(val, (gint)v->hpolicy);
		break;
	case PROP_VPOLICY:
		g_value_set_enum(val, (gint)v->vpolicy);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
	}
}

static void set_property(GObject *o, guint id, const GValue *val,
	GParamSpec *ps)
{
	SoieView *v = SOIE_VIEW(o);

	switch (id) {
	case PROP_SCRIPTS:
		v->scripts = g_value_get_boolean(val);
		break;
	case PROP_ZOOM:
		soie_view_set_zoom_level(v, g_value_get_double(val));
		break;
	case PROP_PRIVATE:
		v->private = g_value_get_boolean(val);
		break;
	case PROP_HADJ:
		set_hadj(v, g_value_get_object(val));
		break;
	case PROP_VADJ:
		set_vadj(v, g_value_get_object(val));
		break;
	case PROP_HPOLICY:
		v->hpolicy = (guint)g_value_get_enum(val);
		break;
	case PROP_VPOLICY:
		v->vpolicy = (guint)g_value_get_enum(val);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
	}
}

static void constructed(GObject *o)
{
	SoieView *v = SOIE_VIEW(o);
	int fd;

	G_OBJECT_CLASS(soie_view_parent_class)->constructed(o);
	fd = shell_init();
	if (fd >= 0 && !fd_watch) {
		fd_watch = g_unix_fd_add(fd, G_IO_IN, on_fd, NULL);
		downloads = g_hash_table_new_full(NULL, NULL, NULL,
			g_object_unref);
		shell_downloads(download_progress, download_done, NULL);
	}
	v->sh = shell_new(&ui, v, v->scripts);
	if (v->private)
		shell_private(v->sh);
	shell_bgra(v->sh, G_BYTE_ORDER == G_LITTLE_ENDIAN);
	views = g_list_prepend(views, v);
	sync_adjustment(v);
}

static void dispose(GObject *o)
{
	SoieView *v = SOIE_VIEW(o);

	if (v->timer)
		g_source_remove(v->timer);
	if (v->idle)
		g_source_remove(v->idle);
	v->timer = v->idle = 0;
	views = g_list_remove(views, v);
	if (v->sh) {
		shell_free(v->sh);
		v->sh = NULL;
	}
	g_clear_object(&v->im);
	if (v->vadj)
		g_signal_handlers_disconnect_by_data(v->vadj, v);
	g_clear_object(&v->vadj);
	g_clear_object(&v->hadj);
	G_OBJECT_CLASS(soie_view_parent_class)->dispose(o);
}

static void finalize(GObject *o)
{
	SoieView *v = SOIE_VIEW(o);

	if (v->surf)
		cairo_surface_destroy(v->surf);
	g_free(v->title);
	g_free(v->link);
	g_free(v->alert);
	g_free(v->open);
	G_OBJECT_CLASS(soie_view_parent_class)->finalize(o);
}

static void soie_view_class_init(SoieViewClass *c)
{
	GObjectClass *oc = G_OBJECT_CLASS(c);
	GtkWidgetClass *wc = GTK_WIDGET_CLASS(c);

	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->constructed = constructed;
	oc->dispose = dispose;
	oc->finalize = finalize;
	wc->realize = realize;
	wc->unrealize = unrealize;
	wc->size_allocate = size_allocate;
	wc->get_preferred_width = get_preferred_width;
	wc->get_preferred_height = get_preferred_height;
	wc->draw = draw;
	wc->key_press_event = key_press;
	wc->key_release_event = key_release;
	wc->focus_in_event = focus_in;
	wc->focus_out_event = focus_out;
	wc->motion_notify_event = motion;
	wc->leave_notify_event = leave;
	wc->button_press_event = button_press;
	wc->button_release_event = button_release;
	wc->scroll_event = scroll;
	props[PROP_URI] = g_param_spec_string("uri", "URI",
		"The page's address", NULL, G_PARAM_READABLE |
		G_PARAM_STATIC_STRINGS);
	props[PROP_TITLE] = g_param_spec_string("title", "Title",
		"The page's title", NULL, G_PARAM_READABLE |
		G_PARAM_STATIC_STRINGS);
	props[PROP_LOADING] = g_param_spec_boolean("loading", "Loading",
		"The page or its resources are on their way", FALSE,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	props[PROP_CAN_GO_BACK] = g_param_spec_boolean("can-go-back",
		"Can go back", "A page before in the history", FALSE,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	props[PROP_CAN_GO_FORWARD] = g_param_spec_boolean("can-go-forward",
		"Can go forward", "A page after in the history", FALSE,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	props[PROP_SCRIPTS] = g_param_spec_boolean("scripts", "Scripts",
		"Run the pages' JavaScript", TRUE, G_PARAM_READWRITE |
		G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	props[PROP_PRIVATE] = g_param_spec_boolean("private", "Private",
		"Cookies in memory only, apart from the other views'", FALSE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
		G_PARAM_STATIC_STRINGS);
	props[PROP_ZOOM] = g_param_spec_double("zoom-level", "Zoom level",
		"The page's zoom, 1 is 100%", 0.25, 5, 1, G_PARAM_READWRITE |
		G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
	g_object_class_override_property(oc, PROP_HADJ, "hadjustment");
	g_object_class_override_property(oc, PROP_VADJ, "vadjustment");
	g_object_class_override_property(oc, PROP_HPOLICY,
		"hscroll-policy");
	g_object_class_override_property(oc, PROP_VPOLICY,
		"vscroll-policy");
	sigs[SIG_LINK_HOVERED] = g_signal_new("link-hovered",
		G_TYPE_FROM_CLASS(c), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);
	sigs[SIG_ALERT] = g_signal_new("alert", G_TYPE_FROM_CLASS(c),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
		G_TYPE_STRING);
	sigs[SIG_CONTEXT_MENU] = g_signal_new("context-menu",
		G_TYPE_FROM_CLASS(c), G_SIGNAL_RUN_LAST, 0,
		g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 3,
		GTK_TYPE_MENU, G_TYPE_STRING, G_TYPE_STRING);
	sigs[SIG_DOWNLOAD_STARTED] = g_signal_new("download-started",
		G_TYPE_FROM_CLASS(c), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, SOIE_TYPE_DOWNLOAD);
	sigs[SIG_CREATE] = g_signal_new("create", G_TYPE_FROM_CLASS(c),
		G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_first_wins, NULL,
		NULL, GTK_TYPE_WIDGET, 1, G_TYPE_STRING);
}

static void soie_view_init(SoieView *v)
{
	GtkWidget *w = GTK_WIDGET(v);

	v->scripts = TRUE;
	v->cursor = -1;
	v->scale = 1;
	v->zoom = 1;
	v->full = TRUE;
	gtk_widget_set_has_window(w, TRUE);
	gtk_widget_set_can_focus(w, TRUE);
	v->im = gtk_im_multicontext_new();
	g_signal_connect(v->im, "commit", G_CALLBACK(on_commit), v);
	set_hadj(v, NULL);
	set_vadj(v, NULL);
}

/* The API */

GtkWidget *soie_view_new(void)
{
	return g_object_new(SOIE_TYPE_VIEW, NULL);
}

/* After a call that may load or change the page */
static void after(SoieView *v)
{
	v->full = TRUE;
	redraw(v);
	tick_soon(v);
	tell(v);
}

void soie_view_load_uri(SoieView *v, const char *uri)
{
	g_return_if_fail(SOIE_IS_VIEW(v) && uri);
	shell_load(v->sh, uri);
	after(v);
}

void soie_view_load_html(SoieView *v, const char *html, const char *base)
{
	g_return_if_fail(SOIE_IS_VIEW(v));
	shell_load_html(v->sh, html, base);
	after(v);
}

/* A browser's scheme: its function, behind the shell's */
struct scheme {
	SoieURISchemeFunc fn;
	gpointer data;
};

static int scheme_answer(void *data, const char *url, char **body,
	size_t *len, char *type, size_t size)
{
	struct scheme *sc = data;
	char *ct = NULL;
	GBytes *b = sc->fn(url, &ct, sc->data);
	gsize n;
	const void *p;

	if (!b) {
		g_free(ct);
		return -1;
	}
	p = g_bytes_get_data(b, &n);
	*body = malloc(n + 1);
	if (!*body)
		abort();
	if (n)
		memcpy(*body, p, n);
	(*body)[n] = '\0';
	*len = n;
	g_snprintf(type, size, "%s", ct ? ct : "text/html");
	g_free(ct);
	g_bytes_unref(b);
	return 0;
}

void soie_view_register_uri_scheme(const char *scheme, SoieURISchemeFunc fn,
	gpointer data)
{
	struct scheme *sc = g_new(struct scheme, 1);	/* kept: as WebKit */

	g_return_if_fail(scheme && fn);
	sc->fn = fn;
	sc->data = data;
	shell_scheme(scheme, scheme_answer, sc);
}

void soie_view_go(SoieView *v, const char *typed)
{
	g_return_if_fail(SOIE_IS_VIEW(v) && typed);
	shell_go(v->sh, typed);
	after(v);
}

void soie_view_reload(SoieView *v)
{
	g_return_if_fail(SOIE_IS_VIEW(v));
	shell_reload(v->sh);
	after(v);
}

void soie_view_go_back(SoieView *v)
{
	g_return_if_fail(SOIE_IS_VIEW(v));
	shell_history(v->sh, -1);
	after(v);
}

void soie_view_go_forward(SoieView *v)
{
	g_return_if_fail(SOIE_IS_VIEW(v));
	shell_history(v->sh, 1);
	after(v);
}

const char *soie_view_get_uri(SoieView *v)
{
	g_return_val_if_fail(SOIE_IS_VIEW(v), NULL);
	return shell_url(v->sh);
}

const char *soie_view_get_title(SoieView *v)
{
	g_return_val_if_fail(SOIE_IS_VIEW(v), NULL);
	return v->title ? v->title : shell_url(v->sh);
}

gboolean soie_view_is_loading(SoieView *v)
{
	g_return_val_if_fail(SOIE_IS_VIEW(v), FALSE);
	return v->loading;
}

gboolean soie_view_can_go_back(SoieView *v)
{
	g_return_val_if_fail(SOIE_IS_VIEW(v), FALSE);
	return shell_can_go(v->sh, -1) != 0;
}

gboolean soie_view_can_go_forward(SoieView *v)
{
	g_return_val_if_fail(SOIE_IS_VIEW(v), FALSE);
	return shell_can_go(v->sh, 1) != 0;
}

void soie_view_set_zoom_level(SoieView *v, double zoom)
{
	g_return_if_fail(SOIE_IS_VIEW(v));
	zoom = zoom < 0.25 ? 0.25 : zoom > 5 ? 5 : zoom;
	if (zoom == v->zoom)
		return;
	v->zoom = zoom;
	shell_zoom(v->sh, (float)(zoom * v->scale));
	v->full = TRUE;
	redraw(v);
	tick_soon(v);
	g_object_notify_by_pspec(G_OBJECT(v), props[PROP_ZOOM]);
}

double soie_view_get_zoom_level(SoieView *v)
{
	g_return_val_if_fail(SOIE_IS_VIEW(v), 1);
	return v->zoom;
}

gboolean soie_view_has_selection(SoieView *v)
{
	char *t;

	g_return_val_if_fail(SOIE_IS_VIEW(v), FALSE);
	if (!(t = shell_selection(v->sh)))
		return FALSE;
	free(t);
	return TRUE;
}

char *soie_view_get_selected_text(SoieView *v)
{
	char *t, *r;

	g_return_val_if_fail(SOIE_IS_VIEW(v), NULL);
	if (!(t = shell_selection(v->sh)))
		return NULL;
	r = g_strdup(t);
	free(t);
	return r;
}

void soie_view_copy_clipboard(SoieView *v)
{
	char *t;

	g_return_if_fail(SOIE_IS_VIEW(v));
	if (!(t = shell_selection(v->sh)))
		return;
	gtk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(v),
		GDK_SELECTION_CLIPBOARD), t, -1);
	free(t);
}

void soie_view_select_all(SoieView *v)
{
	char *t;

	g_return_if_fail(SOIE_IS_VIEW(v));
	shell_select_all(v->sh);
	if ((t = shell_selection(v->sh))) {
		gtk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(v),
			GDK_SELECTION_PRIMARY), t, -1);
		free(t);
	}
	v->full = TRUE;
	redraw(v);
}

void soie_view_set_cookie_policy(SoieCookiePolicy policy)
{
	shell_init();	/* the extensions, the cookie jar among them */
	soie_cookies_policy(policy == SOIE_COOKIE_POLICY_ALL ?
		SOIE_COOKIES_ALL : policy == SOIE_COOKIE_POLICY_NONE ?
		SOIE_COOKIES_NONE : SOIE_COOKIES_NO_THIRD_PARTY);
}

void soie_view_clear_cookies(gboolean private_ones)
{
	shell_init();
	soie_cookies_clear(private_ones ? SOIE_COOKIE_PRIVATE : 0);
}

int soie_view_find(SoieView *v, const char *text, int dir, int *current)
{
	int n, cur;

	g_return_val_if_fail(SOIE_IS_VIEW(v), 0);
	n = shell_find(v->sh, text, dir, &cur);
	if (current)
		*current = cur;
	v->full = TRUE;
	redraw(v);
	return n;
}
