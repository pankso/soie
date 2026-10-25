/*
 * wsoie: the Soie browser on Wayland, the toolkit part of a front
 * (front.h). The front paints into its own pixels; what it shows is
 * copied into one of two shared memory buffers the compositor is not
 * reading, at the pace of its frames. Keys through xkbcommon (compose
 * for dead keys), the clipboard through wl_data_device. No primary
 * selection, no icon: the compositor has neither.
 *
 * Usage: wsoie [-s] [-w WIDTH] [-H HEIGHT] [-o out.png] [URL]
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <soie/soie.h>
#include "xdg-shell.h"
#include "xdg-decoration.h"
#include "shell.h"
#include "front.h"

#define NRECT 8
#define TEXT_MIME "text/plain;charset=utf-8"

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct xdg_wm_base *wm;
static struct wl_surface *surface;
static struct xdg_surface *xsurface;
static struct xdg_toplevel *toplevel;
static struct zxdg_decoration_manager_v1 *deco_manager;
static struct wl_data_device_manager *ddm;
static struct wl_data_device *ddev;
static int win_w = 1024, win_h = 768;
/* The last configure, applied once its frame is shown: a window
 * dragged bigger sends one per pointer move, a relayout each would
 * fall behind the pointer */
static int want_w, want_h, cfg_pending;
static uint32_t cfg_serial;

/* The front's pixels */
static unsigned char *px;

/*
 * The buffers given to the compositor: busy until it releases them.
 * Each keeps the rects shown since it was last given (stale): they
 * are copied into it before it goes again. damage: the rects shown
 * since the last commit.
 */
static struct slot {
	struct wl_buffer *buf;
	unsigned char *data;
	int busy, stale[NRECT][4], nstale;
} slots[2];
static void *pool_data;
static size_t pool_size;
static int damage[NRECT][4], ndamage;
static struct wl_callback *frame_cb;	/* a frame is on its way */

/* Pointer and keyboard */
static uint32_t enter_serial, last_serial;
static int ptr_x, ptr_y;
static double wheel;		/* the part of a wheel move not given yet */
static struct wl_cursor_theme *cursor_theme;
static struct wl_surface *cursor_surface;
static struct wl_cursor *cursors[3];
static int cursor_shape = -1;
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *kstate;
static struct xkb_compose_state *compose;
/* Key repeat: the key held, its name and text, the timer */
static int repeat_fd = -1, repeat_rate = 25, repeat_delay = 600;
static uint32_t repeat_key;
static char repeat_name[16], repeat_text[64];

/* Clipboard: ours, the offer the compositor gives, a paste coming */
static struct wl_data_source *source;
static char *copied;
static struct wl_data_offer *offer;
static int offer_text, paste_fd = -1, paste_to;
static char *pasted;
static size_t npasted;

/* Rects */

static void add(int (*list)[4], int *n, const int *r)
{
	int i;

	if (*n == NRECT) {
		for (i = 1; i < NRECT; i++) {
			int x1 = list[0][0] + list[0][2], y1 = list[0][1] +
				list[0][3];

			if (list[i][0] < list[0][0])
				list[0][0] = list[i][0];
			if (list[i][1] < list[0][1])
				list[0][1] = list[i][1];
			if (list[i][0] + list[i][2] > x1)
				x1 = list[i][0] + list[i][2];
			if (list[i][1] + list[i][3] > y1)
				y1 = list[i][1] + list[i][3];
			list[0][2] = x1 - list[0][0];
			list[0][3] = y1 - list[0][1];
		}
		*n = 1;
	}
	memcpy(list[(*n)++], r, sizeof(*list));
}

/* Buffers */

static void released(void *data, struct wl_buffer *b);

static const struct wl_buffer_listener buffer_listener = { released };

static void drop_buffers(void)
{
	int i;

	for (i = 0; i < 2; i++)
		if (slots[i].buf)
			wl_buffer_destroy(slots[i].buf);
	memset(slots, 0, sizeof(slots));
	if (pool_data)
		munmap(pool_data, pool_size);
	pool_data = NULL;
}

/* A file only the compositor and we see: in the runtime directory,
 * gone from it as soon as it is open (no memfd_create() in older C
 * libraries). */
static int shm_file(void)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char path[4096];
	int fd;

	snprintf(path, sizeof(path), "%s/wsoie-XXXXXX", dir && *dir ? dir :
		"/tmp");
	if ((fd = mkstemp(path)) < 0)
		return -1;
	unlink(path);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

/* Two buffers of win_w x win_h in one shared memory file */
static void make_buffers(void)
{
	size_t one = (size_t)win_w * (size_t)win_h * 4;
	struct wl_shm_pool *pool;
	int fd, i;

	drop_buffers();
	pool_size = one * 2;
	fd = shm_file();
	if (fd < 0 || ftruncate(fd, (off_t)pool_size) < 0 ||
	    (pool_data = mmap(NULL, pool_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0)) == MAP_FAILED) {
		fputs("wsoie: no shared memory for the window\n", stderr);
		exit(1);
	}
	pool = wl_shm_create_pool(shm, fd, (int32_t)pool_size);
	for (i = 0; i < 2; i++) {
		slots[i].data = (unsigned char *)pool_data + one * (size_t)i;
		slots[i].buf = wl_shm_pool_create_buffer(pool,
			(int32_t)(one * (size_t)i), win_w, win_h, win_w * 4,
			WL_SHM_FORMAT_XRGB8888);
		wl_buffer_add_listener(slots[i].buf, &buffer_listener,
			&slots[i]);
		/* nothing in it yet: all of it to copy */
		slots[i].stale[0][2] = win_w;
		slots[i].stale[0][3] = win_h;
		slots[i].nstale = 1;
	}
	wl_shm_pool_destroy(pool);
	close(fd);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t);

static const struct wl_callback_listener frame_listener = { frame_done };

/* Give the compositor what changed: in a free buffer, after its frame */
static void present(void)
{
	struct slot *s = !slots[0].busy ? &slots[0] : !slots[1].busy ?
		&slots[1] : NULL;
	int i, y;

	/* none while the buffers are made again for a new size */
	if (!ndamage || frame_cb || !s || !s->buf)
		return;
	for (i = 0; i < s->nstale; i++) {
		const int *r = s->stale[i];

		for (y = r[1]; y < r[1] + r[3]; y++)
			memcpy(s->data + ((size_t)y * (size_t)win_w +
				(size_t)r[0]) * 4, px + ((size_t)y *
				(size_t)win_w + (size_t)r[0]) * 4,
				(size_t)r[2] * 4);
	}
	s->nstale = 0;
	wl_surface_attach(surface, s->buf, 0, 0);
	for (i = 0; i < ndamage; i++)
		wl_surface_damage_buffer(surface, damage[i][0], damage[i][1],
			damage[i][2], damage[i][3]);
	ndamage = 0;
	frame_cb = wl_surface_frame(surface);
	wl_callback_add_listener(frame_cb, &frame_listener, NULL);
	wl_surface_commit(surface);
	s->busy = 1;
}

static void released(void *data, struct wl_buffer *b)
{
	struct slot *s = data;

	(void)b;
	s->busy = 0;
	present();
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
	(void)data;
	(void)t;
	wl_callback_destroy(cb);
	frame_cb = NULL;
	present();
}

/* Front calls of the shell */

void ui_title(const char *title)
{
	char full[600];

	if (!toplevel)
		return;
	snprintf(full, sizeof(full), "%s - Soie", title);
	xdg_toplevel_set_title(toplevel, full);
}

/* Toolkit calls of the front */

void tk_show(const int (*r)[4], int n)
{
	int i, k;

	for (i = 0; i < n; i++) {
		add(damage, &ndamage, r[i]);
		for (k = 0; k < 2; k++)
			add(slots[k].stale, &slots[k].nstale, r[i]);
	}
	present();
}

static void set_cursor(void)
{
	struct wl_cursor_image *im;

	if (!pointer || !cursor_surface || cursor_shape < 0 ||
	    !cursors[cursor_shape])
		return;
	im = cursors[cursor_shape]->images[0];
	wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(im), 0,
		0);
	wl_surface_damage_buffer(cursor_surface, 0, 0, (int32_t)im->width,
		(int32_t)im->height);
	wl_surface_commit(cursor_surface);
	wl_pointer_set_cursor(pointer, enter_serial, cursor_surface,
		(int32_t)im->hotspot_x, (int32_t)im->hotspot_y);
}

void tk_cursor(int shape)
{
	if (shape == cursor_shape)
		return;
	cursor_shape = shape;
	set_cursor();
}

static void source_send(void *data, struct wl_data_source *s,
	const char *mime, int32_t fd)
{
	size_t n = copied ? strlen(copied) : 0, k = 0;
	ssize_t w;

	(void)data;
	(void)s;
	(void)mime;
	while (k < n && (w = write(fd, copied + k, n - k)) > 0)
		k += (size_t)w;
	close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *s)
{
	(void)data;
	wl_data_source_destroy(s);
	if (s == source)
		source = NULL;
}

static void source_target(void *data, struct wl_data_source *s,
	const char *mime)
{
	(void)data;
	(void)s;
	(void)mime;
}

static const struct wl_data_source_listener source_listener = {
	source_target, source_send, source_cancelled, NULL, NULL, NULL
};

/* The clipboard only: there is no primary selection to own. */
void tk_copy(int clipboard, const char *text)
{
	if (!clipboard || !ddm || !ddev)
		return;
	free(copied);
	copied = strdup(text);
	if (source)
		wl_data_source_destroy(source);
	source = wl_data_device_manager_create_data_source(ddm);
	wl_data_source_add_listener(source, &source_listener, NULL);
	wl_data_source_offer(source, TEXT_MIME);
	wl_data_source_offer(source, "text/plain");
	wl_data_source_offer(source, "UTF8_STRING");
	wl_data_device_set_selection(ddev, source, last_serial);
}

/* The text comes through a pipe, read in the main loop. */
void tk_paste(int clipboard, int to)
{
	int p[2];

	/* ours: no round trip through the compositor, whose offer may not
	 * have come yet */
	if (clipboard && source && copied) {
		front_pasted(to, copied);
		return;
	}
	if (!clipboard || !offer || !offer_text || paste_fd >= 0 ||
	    pipe2(p, O_CLOEXEC) < 0)
		return;
	wl_data_offer_receive(offer, TEXT_MIME, p[1]);
	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	paste_fd = p[0];
	paste_to = to;
	npasted = 0;
	wl_display_flush(dpy);
}

static void paste_read(void)
{
	char buf[4096];
	ssize_t n;

	while ((n = read(paste_fd, buf, sizeof(buf))) > 0) {
		/* 256 KB at most, as xsoie */
		if (npasted + (size_t)n > 256 * 1024)
			continue;
		pasted = realloc(pasted, npasted + (size_t)n + 1);
		if (!pasted)
			abort();
		memcpy(pasted + npasted, buf, (size_t)n);
		npasted += (size_t)n;
	}
	if (n < 0 && errno == EAGAIN)
		return;
	close(paste_fd);
	paste_fd = -1;
	if (pasted) {
		pasted[npasted] = '\0';
		front_pasted(paste_to, pasted);
	}
	free(pasted);
	pasted = NULL;
}

/* Data device: what the clipboard holds */

static void offer_offer(void *data, struct wl_data_offer *o,
	const char *mime)
{
	(void)data;
	if (o == offer && !strcmp(mime, TEXT_MIME))
		offer_text = 1;
}

static const struct wl_data_offer_listener offer_listener = {
	offer_offer, NULL, NULL		/* actions: drag and drop */
};

static void dd_offer(void *data, struct wl_data_device *d,
	struct wl_data_offer *o)
{
	(void)data;
	(void)d;
	/* its types come before it is the selection */
	if (offer)
		wl_data_offer_destroy(offer);
	offer = o;
	offer_text = 0;
	wl_data_offer_add_listener(o, &offer_listener, NULL);
}

static void dd_selection(void *data, struct wl_data_device *d,
	struct wl_data_offer *o)
{
	(void)data;
	(void)d;
	if (!o && offer) {
		wl_data_offer_destroy(offer);
		offer = NULL;
	}
}

static void dd_enter(void *data, struct wl_data_device *d, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
	struct wl_data_offer *o)
{
	(void)data;
	(void)d;
	(void)serial;
	(void)s;
	(void)x;
	(void)y;
	(void)o;
}

static void dd_none(void *data, struct wl_data_device *d)
{
	(void)data;
	(void)d;
}

static void dd_motion(void *data, struct wl_data_device *d, uint32_t t,
	wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)d;
	(void)t;
	(void)x;
	(void)y;
}

static const struct wl_data_device_listener dd_listener = {
	dd_offer, dd_enter, dd_none, dd_motion, dd_none, dd_selection
};

/* Keys */

static int mods(void)
{
	if (!kstate)
		return 0;
	return (xkb_state_mod_name_is_active(kstate, XKB_MOD_NAME_SHIFT,
		XKB_STATE_MODS_EFFECTIVE) > 0 ? SOIE_MOD_SHIFT : 0) |
		(xkb_state_mod_name_is_active(kstate, XKB_MOD_NAME_CTRL,
		XKB_STATE_MODS_EFFECTIVE) > 0 ? SOIE_MOD_CTRL : 0) |
		(xkb_state_mod_name_is_active(kstate, XKB_MOD_NAME_ALT,
		XKB_STATE_MODS_EFFECTIVE) > 0 ? SOIE_MOD_ALT : 0);
}

/* The DOM key value of a keysym: a character key is its character, so
 * that Ctrl+L is "l" though it types nothing. */
static const char *key_name(xkb_keysym_t ks, const char *txt)
{
	static char buf[16];
	uint32_t c;

	switch (ks) {
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: return "Enter";
	case XKB_KEY_BackSpace: return "Backspace";
	case XKB_KEY_Tab: return "Tab";
	case XKB_KEY_Escape: return "Escape";
	case XKB_KEY_Delete: return "Delete";
	case XKB_KEY_Insert: return "Insert";
	case XKB_KEY_Left: return "ArrowLeft";
	case XKB_KEY_Right: return "ArrowRight";
	case XKB_KEY_Up: return "ArrowUp";
	case XKB_KEY_Down: return "ArrowDown";
	case XKB_KEY_Home: return "Home";
	case XKB_KEY_End: return "End";
	case XKB_KEY_Prior: return "PageUp";
	case XKB_KEY_Next: return "PageDown";
	case XKB_KEY_Shift_L: case XKB_KEY_Shift_R: return "Shift";
	case XKB_KEY_Control_L: case XKB_KEY_Control_R: return "Control";
	case XKB_KEY_Alt_L: case XKB_KEY_Alt_R: return "Alt";
	case XKB_KEY_ISO_Level3_Shift: case XKB_KEY_Mode_switch:
		return "AltGraph";
	case XKB_KEY_Meta_L: case XKB_KEY_Meta_R: case XKB_KEY_Super_L:
	case XKB_KEY_Super_R: return "Meta";
	case XKB_KEY_Caps_Lock: return "CapsLock";
	case XKB_KEY_Num_Lock: return "NumLock";
	}
	if (ks >= XKB_KEY_F1 && ks <= XKB_KEY_F12) {
		snprintf(buf, sizeof(buf), "F%d", (int)(ks - XKB_KEY_F1 + 1));
		return buf;
	}
	if ((c = xkb_keysym_to_utf32(ks)) >= 0x20 && c != 0x7f &&
	    xkb_keysym_to_utf8(ks, buf, sizeof(buf)) > 0)
		return buf;
	if ((unsigned char)txt[0] >= 0x20 && strlen(txt) < sizeof(buf)) {
		strcpy(buf, txt);
		return buf;
	}
	return "Unidentified";
}

static void repeat_arm(int ms, int every)
{
	struct itimerspec t;

	memset(&t, 0, sizeof(t));
	t.it_value.tv_sec = ms / 1000;
	t.it_value.tv_nsec = (long)(ms % 1000) * 1000000;
	t.it_interval.tv_sec = every / 1000;
	t.it_interval.tv_nsec = (long)(every % 1000) * 1000000;
	if (repeat_fd >= 0)
		timerfd_settime(repeat_fd, 0, &t, NULL);
}

static void key_press(uint32_t code)
{
	xkb_keycode_t kc = code + 8;
	xkb_keysym_t ks = xkb_state_key_get_one_sym(kstate, kc);
	char txt[64] = "";
	const char *name;

	if (compose && xkb_compose_state_feed(compose, ks) ==
	    XKB_COMPOSE_FEED_ACCEPTED) {
		switch (xkb_compose_state_get_status(compose)) {
		case XKB_COMPOSE_COMPOSING:
			return;
		case XKB_COMPOSE_CANCELLED:
			xkb_compose_state_reset(compose);
			return;
		case XKB_COMPOSE_COMPOSED:
			xkb_compose_state_get_utf8(compose, txt, sizeof(txt));
			ks = xkb_compose_state_get_one_sym(compose);
			xkb_compose_state_reset(compose);
			break;
		default:
			break;
		}
	}
	if (!*txt)
		xkb_state_key_get_utf8(kstate, kc, txt, sizeof(txt));
	name = key_name(ks, txt);
	if (xkb_keymap_key_repeats(keymap, kc) && repeat_rate > 0) {
		repeat_key = code;
		snprintf(repeat_name, sizeof(repeat_name), "%s", name);
		snprintf(repeat_text, sizeof(repeat_text), "%s", txt);
		repeat_arm(repeat_delay, 1000 / repeat_rate);
	}
	front_key(name, txt, mods());
}

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format,
	int32_t fd, uint32_t size)
{
	char *map;

	(void)data;
	(void)k;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	if (kstate)
		xkb_state_unref(kstate);
	if (keymap)
		xkb_keymap_unref(keymap);
	keymap = xkb_keymap_new_from_string(xkb, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	kstate = keymap ? xkb_state_new(keymap) : NULL;
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s, struct wl_array *keys)
{
	(void)data;
	(void)k;
	(void)s;
	(void)keys;
	last_serial = serial;
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s)
{
	(void)data;
	(void)k;
	(void)serial;
	(void)s;
	repeat_arm(0, 0);
}

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t time, uint32_t key, uint32_t state)
{
	(void)data;
	(void)k;
	(void)time;
	last_serial = serial;
	if (!kstate)
		return;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		key_press(key);
	else if (key == repeat_key)
		repeat_arm(0, 0);
}

static void kb_modifiers(void *data, struct wl_keyboard *k,
	uint32_t serial, uint32_t depressed, uint32_t latched,
	uint32_t locked, uint32_t group)
{
	(void)data;
	(void)k;
	(void)serial;
	if (kstate)
		xkb_state_update_mask(kstate, depressed, latched, locked, 0, 0,
			group);
}

static void kb_repeat(void *data, struct wl_keyboard *k, int32_t rate,
	int32_t delay)
{
	(void)data;
	(void)k;
	repeat_rate = rate;
	repeat_delay = delay;
}

static const struct wl_keyboard_listener kb_listener = {
	kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat
};

/* Pointer */

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)p;
	(void)s;
	enter_serial = serial;
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	/* the compositor's cursor until ours is set again */
	cursor_shape = -1;
	front_motion(ptr_x, ptr_y);
	if (cursor_shape < 0)
		tk_cursor(SHELL_ARROW);
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)s;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t t,
	wl_fixed_t x, wl_fixed_t y)
{
	(void)data;
	(void)p;
	(void)t;
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	front_motion(ptr_x, ptr_y);
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t t, uint32_t button, uint32_t state)
{
	int b = button == BTN_LEFT ? 1 : button == BTN_MIDDLE ? 2 :
		button == BTN_RIGHT ? 3 : button == BTN_SIDE ? 8 :
		button == BTN_EXTRA ? 9 : 0;

	(void)data;
	(void)p;
	(void)t;
	last_serial = serial;
	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		front_release();
	else if (b)
		front_button(b, ptr_x, ptr_y, mods() & ~SOIE_MOD_ALT);
}

/* A wheel notch is 15 (libinput): 60 pixels, as in xsoie */
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t t,
	uint32_t axis, wl_fixed_t value)
{
	int dy;

	(void)data;
	(void)p;
	(void)t;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	wheel += wl_fixed_to_double(value) * 4;
	dy = (int)wheel;
	wheel -= dy;
	if (dy)
		front_wheel(dy);
}

static void ptr_frame(void *data, struct wl_pointer *p)
{
	(void)data;
	(void)p;
}

static void ptr_source(void *data, struct wl_pointer *p, uint32_t s)
{
	(void)data;
	(void)p;
	(void)s;
}

static void ptr_stop(void *data, struct wl_pointer *p, uint32_t t,
	uint32_t axis)
{
	(void)data;
	(void)p;
	(void)t;
	(void)axis;
}

static void ptr_discrete(void *data, struct wl_pointer *p, uint32_t axis,
	int32_t n)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)n;
}

static const struct wl_pointer_listener ptr_listener = {
	ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis, ptr_frame,
	ptr_source, ptr_stop, ptr_discrete, NULL, NULL
};

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
	(void)data;
	(void)s;
	(void)name;
}

static const struct wl_seat_listener seat_listener = { seat_caps,
	seat_name };

/* Window */

static void wm_ping(void *data, struct xdg_wm_base *w, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(w, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xs_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	(void)data;
	(void)s;
	cfg_serial = serial;
	cfg_pending = 1;
}

static const struct xdg_surface_listener xs_listener = { xs_configure };

/* The size the compositor wants (0: ours), for the configure that
 * follows */
static void tl_configure(void *data, struct xdg_toplevel *t, int32_t w,
	int32_t h, struct wl_array *states)
{
	(void)data;
	(void)t;
	(void)states;
	if (w <= 0 || h <= 0)
		return;
	want_w = w < 100 ? 100 : w;
	want_h = h < 84 ? 84 : h;
}

/* The last configure, when no frame is on its way: a new size gets
 * new buffers and the page laid out again */
static void configure(void)
{
	if (!cfg_pending || frame_cb)
		return;
	cfg_pending = 0;
	xdg_surface_ack_configure(xsurface, cfg_serial);
	if (slots[0].buf && (!want_w || (want_w == win_w &&
	    want_h == win_h))) {
		wl_surface_commit(surface);
		return;
	}
	if (want_w) {
		win_w = want_w;
		win_h = want_h;
	}
	make_buffers();
	free(px);
	px = calloc((size_t)win_w * (size_t)win_h, 4);
	if (!px)
		abort();
	front_buffer(px, win_w, win_h, win_w * 4, 1);
	front_redraw();
}

static void tl_close(void *data, struct xdg_toplevel *t)
{
	(void)data;
	(void)t;
	exit(0);
}

static const struct xdg_toplevel_listener tl_listener = { tl_configure,
	tl_close, NULL, NULL };

static void global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t version)
{
	(void)data;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface,
			version < 5 ? version : 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm, &wm_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		deco_manager = wl_registry_bind(r, name,
			&zxdg_decoration_manager_v1_interface, 1);
	} else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
		/* version 1: the clipboard, without drag and drop */
		ddm = wl_registry_bind(r, name,
			&wl_data_device_manager_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data;
	(void)r;
	(void)name;
}

static const struct wl_registry_listener registry_listener = { global,
	global_remove };

static void keys_init(void)
{
	const char *locale = getenv("LC_ALL");
	struct xkb_compose_table *table;

	xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!locale || !*locale)
		locale = getenv("LC_CTYPE");
	if (!locale || !*locale)
		locale = getenv("LANG");
	if (!locale || !*locale)
		locale = "C";
	table = xkb_compose_table_new_from_locale(xkb, locale,
		XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (table) {
		compose = xkb_compose_state_new(table,
			XKB_COMPOSE_STATE_NO_FLAGS);
		xkb_compose_table_unref(table);
	}
	repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC |
		TFD_NONBLOCK);
}

static void cursors_init(void)
{
	static const char *const names[3][2] = { { "left_ptr", "default" },
		{ "hand2", "pointer" }, { "xterm", "text" } };
	const char *size = getenv("XCURSOR_SIZE");
	int i;

	cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"),
		size && atoi(size) > 0 ? atoi(size) : 24, shm);
	if (!cursor_theme)
		return;
	for (i = 0; i < 3; i++)
		if (!(cursors[i] = wl_cursor_theme_get_cursor(cursor_theme,
		    names[i][0])))
			cursors[i] = wl_cursor_theme_get_cursor(cursor_theme,
				names[i][1]);
	cursor_surface = wl_compositor_create_surface(compositor);
}

int main(int argc, char **argv)
{
	struct wl_registry *registry;
	int i;

	if ((i = front_start("wsoie", argc, argv, &win_w, &win_h)) >= 0)
		return i;
	setlocale(LC_CTYPE, "");
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fputs("wsoie: cannot connect to the Wayland display\n", stderr);
		return 1;
	}
	registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm || !wm) {
		fputs("wsoie: the compositor lacks xdg-shell or wl_shm\n",
			stderr);
		return 1;
	}
	if (ddm && seat) {
		ddev = wl_data_device_manager_get_data_device(ddm, seat);
		wl_data_device_add_listener(ddev, &dd_listener, NULL);
	}
	keys_init();
	cursors_init();
	surface = wl_compositor_create_surface(compositor);
	xsurface = xdg_wm_base_get_xdg_surface(wm, surface);
	xdg_surface_add_listener(xsurface, &xs_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xsurface);
	xdg_toplevel_add_listener(toplevel, &tl_listener, NULL);
	xdg_toplevel_set_title(toplevel, "Soie");
	xdg_toplevel_set_app_id(toplevel, "wsoie");
	/* The compositor's title bar: wsoie draws none of its own. Its
	 * answer needs no listener, a client side one leaves the window
	 * bare as without the protocol. */
	if (deco_manager)
		zxdg_toplevel_decoration_v1_set_mode(
			zxdg_decoration_manager_v1_get_toplevel_decoration(
			deco_manager, toplevel),
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	wl_surface_commit(surface);
	wl_display_roundtrip(dpy);
	configure();
	front_load();
	for (;;) {
		struct pollfd p[4];
		int t = front_tick(), n = 3;

		while (wl_display_prepare_read(dpy) != 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);
		p[0].fd = wl_display_get_fd(dpy);
		p[1].fd = front_fd();
		p[2].fd = repeat_fd;
		p[3].fd = paste_fd;
		if (paste_fd >= 0)
			n = 4;
		for (i = 0; i < n; i++) {
			p[i].events = POLLIN;
			p[i].revents = 0;
		}
		if (poll(p, (nfds_t)n, t) > 0 && (p[0].revents & POLLIN)) {
			if (wl_display_read_events(dpy) < 0)
				return 1;
		} else {
			wl_display_cancel_read(dpy);
		}
		if (p[0].revents & (POLLERR | POLLHUP))
			return 1;
		if (wl_display_dispatch_pending(dpy) < 0)
			return 1;
		if (p[2].revents & POLLIN) {
			uint64_t times;

			if (read(repeat_fd, &times, sizeof(times)) ==
			    sizeof(times))
				front_key(repeat_name, repeat_text, mods());
		}
		if (n == 4 && (p[3].revents & (POLLIN | POLLHUP)))
			paste_read();
		configure();
		front_redraw();
	}
}
