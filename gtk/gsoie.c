/*
 * gsoie: a little GTK 3 browser on SoieView, to try the widget and show
 * how a browser uses it: tabs, an address bar, back, forward, reload,
 * the link under the pointer. Ctrl+T a tab, Ctrl+W closes it, Ctrl+L
 * the address, Ctrl+R or F5 reloads, Alt+arrows the history, Ctrl+Q.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <gtk/gtk.h>
#include "soie-view.h"

#define HOME "about:soie"

static GtkWidget *win, *tabs, *entry, *back, *fwd, *status;

static SoieView *current(void)
{
	int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs));
	GtkWidget *sw = i < 0 ? NULL :
		gtk_notebook_get_nth_page(GTK_NOTEBOOK(tabs), i);

	return sw ? SOIE_VIEW(gtk_bin_get_child(GTK_BIN(sw))) : NULL;
}

/* The bars follow the current tab. */
static void update(void)
{
	SoieView *v = current();
	char title[600];

	if (!v)
		return;
	if (!gtk_widget_has_focus(entry))
		gtk_entry_set_text(GTK_ENTRY(entry), soie_view_get_uri(v));
	gtk_widget_set_sensitive(back, soie_view_can_go_back(v));
	gtk_widget_set_sensitive(fwd, soie_view_can_go_forward(v));
	g_snprintf(title, sizeof(title), "%s - gsoie", soie_view_get_title(v));
	gtk_window_set_title(GTK_WINDOW(win), title);
}

static gboolean update_idle(gpointer data)
{
	(void)data;
	update();
	return G_SOURCE_REMOVE;
}

static void changed(GObject *o, GParamSpec *ps, gpointer label)
{
	SoieView *v = SOIE_VIEW(o);

	(void)ps;
	gtk_label_set_text(GTK_LABEL(label), soie_view_get_title(v));
	gtk_widget_set_tooltip_text(GTK_WIDGET(label), soie_view_get_title(v));
	if (v == current())
		update();
}

static void link_hovered(SoieView *v, const char *uri, gpointer data)
{
	(void)data;
	if (v == current())
		gtk_label_set_text(GTK_LABEL(status), uri);
}

static void alert(SoieView *v, const char *text, gpointer data)
{
	GtkWidget *d;

	(void)v;
	(void)data;
	d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
		GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", text);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

static void new_tab(const char *uri)
{
	GtkWidget *view = soie_view_new(), *sw, *label;
	int i;

	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(sw), view);
	label = gtk_label_new("New tab");
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_width_chars(GTK_LABEL(label), 18);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 18);
	g_signal_connect(view, "notify::title", G_CALLBACK(changed), label);
	g_signal_connect(view, "notify::uri", G_CALLBACK(changed), label);
	g_signal_connect(view, "notify::can-go-back", G_CALLBACK(changed),
		label);
	g_signal_connect(view, "notify::can-go-forward", G_CALLBACK(changed),
		label);
	g_signal_connect(view, "link-hovered", G_CALLBACK(link_hovered),
		NULL);
	g_signal_connect(view, "alert", G_CALLBACK(alert), NULL);
	gtk_widget_show_all(sw);
	i = gtk_notebook_append_page(GTK_NOTEBOOK(tabs), sw, label);
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(tabs), sw, TRUE);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), i);
	soie_view_load_uri(SOIE_VIEW(view), uri);
	gtk_widget_grab_focus(view);
}

static void close_tab(void)
{
	int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs));

	if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(tabs)) <= 1) {
		gtk_widget_destroy(win);
		return;
	}
	gtk_notebook_remove_page(GTK_NOTEBOOK(tabs), i);
}

static void switched(GtkNotebook *nb, GtkWidget *page, guint n, gpointer d)
{
	(void)nb;
	(void)page;
	(void)n;
	(void)d;
	gtk_label_set_text(GTK_LABEL(status), "");
	/* after the switch: current() is the new tab then */
	g_idle_add(update_idle, NULL);
}

static void go(GtkEntry *e, gpointer data)
{
	SoieView *v = current();

	(void)data;
	if (!v)
		return;
	soie_view_go(v, gtk_entry_get_text(e));
	gtk_widget_grab_focus(GTK_WIDGET(v));
}

static void clicked(GtkButton *b, gpointer data)
{
	SoieView *v = current();
	int what = GPOINTER_TO_INT(data);

	(void)b;
	if (!v)
		return;
	if (what < 0)
		soie_view_go_back(v);
	else if (what > 0)
		soie_view_go_forward(v);
	else
		soie_view_reload(v);
}

/* The window's shortcuts, before the page has the keys */
static gboolean shortcut(GtkWidget *w, GdkEventKey *e, gpointer data)
{
	guint m = e->state & gtk_accelerator_get_default_mod_mask();
	guint k = gdk_keyval_to_lower(e->keyval);
	SoieView *v = current();

	(void)w;
	(void)data;
	if (m == GDK_CONTROL_MASK && k == GDK_KEY_t) {
		new_tab(HOME);
		gtk_widget_grab_focus(entry);
	} else if (m == GDK_CONTROL_MASK && k == GDK_KEY_w) {
		close_tab();
	} else if (m == GDK_CONTROL_MASK && k == GDK_KEY_q) {
		gtk_widget_destroy(win);
	} else if (m == GDK_CONTROL_MASK && k == GDK_KEY_l) {
		gtk_widget_grab_focus(entry);
	} else if (v && ((m == GDK_CONTROL_MASK && k == GDK_KEY_r) ||
		   (!m && k == GDK_KEY_F5))) {
		soie_view_reload(v);
	} else if (v && m == GDK_MOD1_MASK && k == GDK_KEY_Left) {
		soie_view_go_back(v);
	} else if (v && m == GDK_MOD1_MASK && k == GDK_KEY_Right) {
		soie_view_go_forward(v);
	} else {
		return FALSE;
	}
	return TRUE;
}

static GtkWidget *button(const char *icon, const char *tip, int what)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon,
		GTK_ICON_SIZE_SMALL_TOOLBAR);

	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_focus_on_click(b, FALSE);
	g_signal_connect(b, "clicked", G_CALLBACK(clicked),
		GINT_TO_POINTER(what));
	return b;
}

int main(int argc, char **argv)
{
	GtkWidget *box, *bar, *reload;
	int i;

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 1024, 768);
	gtk_window_set_title(GTK_WINDOW(win), "gsoie");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(shortcut), NULL);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_container_set_border_width(GTK_CONTAINER(bar), 3);
	back = button("go-previous", "Back (Alt+Left)", -1);
	fwd = button("go-next", "Forward (Alt+Right)", 1);
	reload = button("view-refresh", "Reload (F5)", 0);
	entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
		"Address, or words to search for");
	g_signal_connect(entry, "activate", G_CALLBACK(go), NULL);
	gtk_box_pack_start(GTK_BOX(bar), back, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), fwd, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), reload, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), entry, TRUE, TRUE, 0);
	tabs = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(tabs), TRUE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(tabs), FALSE);
	g_signal_connect(tabs, "switch-page", G_CALLBACK(switched), NULL);
	status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_margin_start(status, 4);
	gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), tabs, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(win), box);
	gtk_widget_show_all(win);
	if (argc < 2)
		new_tab(HOME);
	for (i = 1; i < argc; i++)
		new_tab(argv[i]);
	gtk_main();
	return 0;
}
