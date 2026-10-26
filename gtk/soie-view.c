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
#include <string.h>
#include <glib-unix.h>
#include <soie/soie.h>
#include "shell.h"
#include "soie-view.h"

#define WHEEL 60		/* pixels a wheel step scrolls */
#define NRECT 8			/* rects of a partial repaint */

struct _SoieView {
	GtkWidget parent;
	shell *sh;
	gboolean scripts;
	cairo_surface_t *surf;	/* the page view's pixels */
	int w, h;
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
	int cursor;
	gboolean loading;
	/* what changed during the shell's calls, notified after them */
	gboolean title_changed, uri_changed;
	gboolean back, forward;
};

enum {
	PROP_0, PROP_URI, PROP_TITLE, PROP_LOADING, PROP_CAN_GO_BACK,
	PROP_CAN_GO_FORWARD, PROP_SCRIPTS, N_PROPS,
	PROP_HADJ = N_PROPS, PROP_VADJ, PROP_HPOLICY, PROP_VPOLICY
};
static GParamSpec *props[N_PROPS];

enum { SIG_LINK_HOVERED, SIG_ALERT, N_SIGS };
static guint sigs[N_SIGS];

static void scrollable_init(GtkScrollableInterface *iface)
{
	(void)iface;
}

G_DEFINE_TYPE_WITH_CODE(SoieView, soie_view, GTK_TYPE_WIDGET,
	G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, scrollable_init))

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
}

static void ui_alert(void *data, const char *text)
{
	SoieView *v = data;

	g_free(v->alert);
	v->alert = g_strdup(text);
}

static const struct shell_ui ui = { ui_title, ui_address, ui_alert };

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
	char *alert = v->alert;

	g_object_ref(v);
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
	v->alert = NULL;
	if (alert)
		g_signal_emit(v, sigs[SIG_ALERT], 0, alert);
	g_free(alert);
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
	v->syncing = TRUE;
	gtk_adjustment_configure(v->vadj, scroll, 0,
		height > view ? height : view, 40, view > 80 ? view - 40 : view,
		view);
	v->syncing = FALSE;
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
			gtk_widget_queue_draw_area(w, r[i][0], r[i][1],
				r[i][2], r[i][3]);
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

	GTK_WIDGET_CLASS(soie_view_parent_class)->size_allocate(w, a);
	if (a->width == v->w && a->height == v->h && v->surf)
		return;
	v->w = a->width > 1 ? a->width : 1;
	v->h = a->height > 1 ? a->height : 1;
	if (v->surf)
		cairo_surface_destroy(v->surf);
	v->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, v->w, v->h);
	shell_resize(v->sh, v->w, v->h);
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
		gtk_widget_queue_draw_area(GTK_WIDGET(v), x, y, w, h);
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

	set_cursor(v, shell_hover(v->sh, (int)e->x, (int)e->y, link,
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

static gboolean button_press(GtkWidget *w, GdkEventButton *e)
{
	SoieView *v = SOIE_VIEW(w);

	if (e->type != GDK_BUTTON_PRESS)
		return FALSE;	/* double clicks: the first did it */
	gtk_widget_grab_focus(w);
	switch (e->button) {
	case 1:
		shell_click(v->sh, (int)e->x, (int)e->y, mods_of(e->state));
		break;
	case 2:
		/* the PRIMARY selection into a text field */
		shell_click(v->sh, (int)e->x, (int)e->y, mods_of(e->state));
		if (shell_field(v->sh))
			gtk_clipboard_request_text(gtk_widget_get_clipboard(w,
				GDK_SELECTION_PRIMARY), pasted,
				g_object_ref(v));
		break;
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
		shell_scroll(v->sh, d);
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
	shell_scroll_to(v->sh, (int)gtk_adjustment_get_value(a));
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
	/* no horizontal scrolling yet: an empty range */
	if (a == v->hadj)
		return;
	if (v->hadj)
		g_object_unref(v->hadj);
	if (!a)
		a = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
	v->hadj = g_object_ref_sink(a);
	gtk_adjustment_configure(a, 0, 0, 0, 0, 0, 0);
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
	if (fd >= 0 && !fd_watch)
		fd_watch = g_unix_fd_add(fd, G_IO_IN, on_fd, NULL);
	v->sh = shell_new(&ui, v, v->scripts);
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
}

static void soie_view_init(SoieView *v)
{
	GtkWidget *w = GTK_WIDGET(v);

	v->scripts = TRUE;
	v->cursor = -1;
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
