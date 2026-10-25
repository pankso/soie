/*
 * xsoie: the Soie browser on Xlib, the toolkit part of a front
 * (front.h): the window, its pixels in shared memory (MIT-SHM) when the
 * server is on this machine, the keys through the input method, the
 * selections, the cursors.
 *
 * Usage: xsoie [-s] [-w WIDTH] [-H HEIGHT] [-o out.png] [URL]
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <soie/soie.h>
#include "shell.h"
#include "front.h"

static Display *dpy;
static Window win;
static GC gc;
static XImage *ximg;
static XShmSegmentInfo shm;
static int use_shm;		/* the pixels are shared with the server */
static XIM xim;
static XIC xic;
static Cursor cursors[3];
static Atom a_utf8, a_clipboard, a_targets, a_xsel;
static Time last;		/* of the last event, for the selections */
static int win_w = 1024, win_h = 768;

/* Selections we own: PRIMARY and CLIPBOARD, and where a paste goes */
static char *sel_text[2];
static int paste_to;

/* Front calls of the shell */

void ui_title(const char *title)
{
	char full[600];

	if (!dpy)
		return;
	snprintf(full, sizeof(full), "%s - Soie", title);
	XStoreName(dpy, win, full);
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", 0),
		a_utf8, 8, PropModeReplace, (unsigned char *)full,
		(int)strlen(full));
}

/* Toolkit calls of the front */

void tk_show(const int (*r)[4], int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (use_shm)
			XShmPutImage(dpy, win, gc, ximg, r[i][0], r[i][1],
				r[i][0], r[i][1], (unsigned)r[i][2],
				(unsigned)r[i][3], False);
		else
			XPutImage(dpy, win, gc, ximg, r[i][0], r[i][1],
				r[i][0], r[i][1], (unsigned)r[i][2],
				(unsigned)r[i][3]);
	/* the server has read shared pixels before they are painted
	 * again */
	if (use_shm)
		XSync(dpy, False);
	else
		XFlush(dpy);
}

void tk_cursor(int shape)
{
	XDefineCursor(dpy, win, cursors[shape]);
}

void tk_copy(int clipboard, const char *text)
{
	free(sel_text[clipboard]);
	sel_text[clipboard] = strdup(text);
	XSetSelectionOwner(dpy, clipboard ? a_clipboard : XA_PRIMARY, win,
		last);
}

/* The text comes as a SelectionNotify. */
void tk_paste(int clipboard, int to)
{
	paste_to = to;
	XConvertSelection(dpy, clipboard ? a_clipboard : XA_PRIMARY, a_utf8,
		a_xsel, win, last);
}

/* Selections */

/* Give our selection to another client. */
static void give(XSelectionRequestEvent *r)
{
	XSelectionEvent ev;
	const char *text = r->selection == XA_PRIMARY ? sel_text[0] :
		r->selection == a_clipboard ? sel_text[1] : NULL;
	Atom prop = r->property ? r->property : r->target;

	memset(&ev, 0, sizeof(ev));
	ev.type = SelectionNotify;
	ev.requestor = r->requestor;
	ev.selection = r->selection;
	ev.target = r->target;
	ev.time = r->time;
	ev.property = None;
	if (text && r->target == a_targets) {
		Atom t[3];

		t[0] = a_targets;
		t[1] = a_utf8;
		t[2] = XA_STRING;
		XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32,
			PropModeReplace, (unsigned char *)t, 3);
		ev.property = prop;
	} else if (text && r->target == a_utf8) {
		XChangeProperty(dpy, r->requestor, prop, a_utf8, 8,
			PropModeReplace, (unsigned char *)text,
			(int)strlen(text));
		ev.property = prop;
	} else if (text && r->target == XA_STRING) {
		/* Latin-1: what does not fit becomes '?' */
		char *l = malloc(strlen(text) + 1);
		const unsigned char *s = (const unsigned char *)text;
		int k = 0;

		if (!l)
			abort();
		while (*s) {
			if (*s < 0x80) {
				l[k++] = (char)*s++;
			} else if ((*s == 0xc2 || *s == 0xc3) &&
				   (s[1] & 0xc0) == 0x80) {
				l[k++] = (char)((*s & 3) << 6 | (s[1] & 0x3f));
				s += 2;
			} else {
				l[k++] = '?';
				for (s++; (*s & 0xc0) == 0x80; s++)
					;
			}
		}
		XChangeProperty(dpy, r->requestor, prop, XA_STRING, 8,
			PropModeReplace, (unsigned char *)l, k);
		free(l);
		ev.property = prop;
	}
	XSendEvent(dpy, r->requestor, False, 0, (XEvent *)&ev);
}

/* A selection's text arrived. */
static void pasted(XSelectionEvent *e)
{
	Atom type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	char *text;
	size_t i, k = 0;

	if (e->property == None || XGetWindowProperty(dpy, win, e->property,
	    0, 65536, True, AnyPropertyType, &type, &fmt, &n, &after,
	    &data) != Success || !data)
		return;
	if (fmt != 8 || (type != a_utf8 && type != XA_STRING)) {
		XFree(data);
		return;
	}
	text = malloc(n * 2 + 1);
	if (!text)
		abort();
	for (i = 0; i < n; i++)
		if (type == XA_STRING && data[i] >= 0x80) {
			text[k++] = (char)(0xc0 | data[i] >> 6);
			text[k++] = (char)(0x80 | (data[i] & 0x3f));
		} else {
			text[k++] = (char)data[i];
		}
	text[k] = '\0';
	XFree(data);
	front_pasted(paste_to, text);
	free(text);
}

/* Keys */

/* A code point in UTF-8; returns its length. */
static int utf8(unsigned long c, char *out)
{
	if (c < 0x80) {
		out[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (char)(0xc0 | c >> 6);
		out[1] = (char)(0x80 | (c & 0x3f));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (char)(0xe0 | c >> 12);
		out[1] = (char)(0x80 | (c >> 6 & 0x3f));
		out[2] = (char)(0x80 | (c & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | c >> 18);
	out[1] = (char)(0x80 | (c >> 12 & 0x3f));
	out[2] = (char)(0x80 | (c >> 6 & 0x3f));
	out[3] = (char)(0x80 | (c & 0x3f));
	return 4;
}

/* Latin-1 from XLookupString to UTF-8. */
static int to_utf8(const char *in, int n, char *out)
{
	int i, k = 0;

	for (i = 0; i < n; i++)
		k += utf8((unsigned char)in[i], out + k);
	return k;
}

/* The DOM key value of a key press: a character key is the character
 * of its keysym, so that Ctrl+L is "l" though it types nothing. */
static const char *key_name(KeySym ks, const char *txt, int n)
{
	static char buf[8];

	switch (ks) {
	case XK_Return: case XK_KP_Enter: return "Enter";
	case XK_BackSpace: return "Backspace";
	case XK_Tab: return "Tab";
	case XK_Escape: return "Escape";
	case XK_Delete: return "Delete";
	case XK_Insert: return "Insert";
	case XK_Left: return "ArrowLeft";
	case XK_Right: return "ArrowRight";
	case XK_Up: return "ArrowUp";
	case XK_Down: return "ArrowDown";
	case XK_Home: return "Home";
	case XK_End: return "End";
	case XK_Prior: return "PageUp";
	case XK_Next: return "PageDown";
	case XK_Shift_L: case XK_Shift_R: return "Shift";
	case XK_Control_L: case XK_Control_R: return "Control";
	case XK_Alt_L: case XK_Alt_R: return "Alt";
	case XK_ISO_Level3_Shift: case XK_Mode_switch: return "AltGraph";
	case XK_Meta_L: case XK_Meta_R: case XK_Super_L: case XK_Super_R:
		return "Meta";
	case XK_Caps_Lock: return "CapsLock";
	case XK_Num_Lock: return "NumLock";
	}
	if (ks >= XK_F1 && ks <= XK_F12) {
		snprintf(buf, sizeof(buf), "F%d", (int)(ks - XK_F1 + 1));
		return buf;
	}
	if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xa0 && ks <= 0xff)) {
		buf[utf8(ks, buf)] = '\0';
		return buf;
	}
	if ((ks & 0xff000000) == 0x01000000) {
		buf[utf8(ks & 0xffffff, buf)] = '\0';
		return buf;
	}
	if (n > 0 && n < (int)sizeof(buf) && (unsigned char)txt[0] >= 0x20) {
		memcpy(buf, txt, (size_t)n);
		buf[n] = 0;
		return buf;
	}
	return "Unidentified";
}

static int mods(unsigned state)
{
	return (state & ShiftMask ? SOIE_MOD_SHIFT : 0) |
		(state & ControlMask ? SOIE_MOD_CTRL : 0) |
		(state & Mod1Mask ? SOIE_MOD_ALT : 0);
}

static void key(XKeyEvent *e)
{
	char raw[64], txt[256];
	KeySym ks = NoSymbol;
	Status st = XLookupNone;
	int n;

	if (xic) {
		n = Xutf8LookupString(xic, e, txt, sizeof(txt) - 1, &ks, &st);
		if (st == XBufferOverflow || st == XLookupNone ||
		    st == XLookupKeySym)
			n = 0;
	} else {
		n = XLookupString(e, raw, sizeof(raw), &ks, NULL);
		n = to_utf8(raw, n, txt);
	}
	txt[n] = '\0';
	front_key(key_name(ks, txt, n), txt, mods(e->state));
}

/* Window */

static int trapped;

static int trap(Display *d, XErrorEvent *e)
{
	(void)d;
	(void)e;
	trapped = 1;
	return 0;
}

static void drop_image(void)
{
	if (!ximg)
		return;
	if (use_shm) {
		XShmDetach(dpy, &shm);
		XDestroyImage(ximg);
		shmdt(shm.shmaddr);
	} else {
		free(ximg->data);
		ximg->data = NULL;
		XDestroyImage(ximg);
	}
	ximg = NULL;
}

/* The pixels in shared memory: 0 when the server cannot (another
 * machine's, no MIT-SHM). */
static int shm_image(Visual *vis, int depth)
{
	XErrorHandler old;

	ximg = XShmCreateImage(dpy, vis, (unsigned)depth, ZPixmap, NULL, &shm,
		(unsigned)win_w, (unsigned)win_h);
	if (!ximg)
		return 0;
	shm.shmid = shmget(IPC_PRIVATE, (size_t)ximg->bytes_per_line *
		(size_t)ximg->height, IPC_CREAT | 0600);
	if (shm.shmid < 0) {
		XDestroyImage(ximg);
		ximg = NULL;
		return 0;
	}
	shm.shmaddr = ximg->data = shmat(shm.shmid, NULL, 0);
	shm.readOnly = False;
	trapped = 0;
	old = XSetErrorHandler(trap);
	if (shm.shmaddr != (char *)-1)
		XShmAttach(dpy, &shm);
	else
		trapped = 1;
	XSync(dpy, False);
	XSetErrorHandler(old);
	/* gone when both have detached */
	shmctl(shm.shmid, IPC_RMID, NULL);
	if (trapped) {
		if (shm.shmaddr != (char *)-1)
			shmdt(shm.shmaddr);
		ximg->data = NULL;
		XDestroyImage(ximg);
		ximg = NULL;
		return 0;
	}
	return 1;
}

static void resize(int w, int h)
{
	Visual *vis = DefaultVisual(dpy, DefaultScreen(dpy));
	int depth = DefaultDepth(dpy, DefaultScreen(dpy));
	static int tried;

	if (w == win_w && h == win_h && ximg)
		return;
	win_w = w;
	win_h = h;
	drop_image();
	if (!tried++ || use_shm)
		use_shm = XShmQueryExtension(dpy) && shm_image(vis, depth);
	if (!use_shm) {
		char *px = calloc((size_t)win_w * (size_t)win_h, 4);

		if (!px)
			abort();
		ximg = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, px,
			(unsigned)win_w, (unsigned)win_h, 32, win_w * 4);
	}
	/* 32-bit pixels, red the high byte of the low three, sent as they
	 * are in memory: B G R A bytes, Soie paints them so */
	front_buffer((unsigned char *)ximg->data, win_w, win_h,
		ximg->bytes_per_line, vis->red_mask == 0xff0000 &&
		ximg->byte_order == LSBFirst);
}

/* The window icon: an "S", painted by Soie, at 32 and 16 pixels. */
static void set_icon(void)
{
	static const int sizes[] = { 32, 16 };
	unsigned long *icon = malloc((2 + 32 * 32 + 2 + 16 * 16) *
		sizeof(*icon));
	int i, k = 0, j;

	if (!icon)
		return;
	for (i = 0; i < 2; i++) {
		int s = sizes[i];
		unsigned char px[32 * 32 * 4];

		front_icon(px, s);
		icon[k++] = (unsigned long)s;
		icon[k++] = (unsigned long)s;
		for (j = 0; j < s * s; j++)
			icon[k++] = 0xff000000ul |
				(unsigned long)px[j * 4] << 16 |
				(unsigned long)px[j * 4 + 1] << 8 | px[j * 4 + 2];
	}
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", 0),
		XA_CARDINAL, 32, PropModeReplace, (unsigned char *)icon, k);
	free(icon);
}

/* UTF-8 keyboard input through the input method, when there is one. */
static void open_im(long *mask)
{
	long filter = 0;

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		setlocale(LC_CTYPE, "C");
	XSetLocaleModifiers("");
	if (!(xim = XOpenIM(dpy, NULL, NULL, NULL))) {
		XSetLocaleModifiers("@im=none");
		xim = XOpenIM(dpy, NULL, NULL, NULL);
	}
	if (!xim)
		return;
	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing |
		XIMStatusNothing, XNClientWindow, win, XNFocusWindow, win,
		(char *)NULL);
	if (xic && !XGetICValues(xic, XNFilterEvents, &filter, (char *)NULL))
		*mask |= filter;
}

int main(int argc, char **argv)
{
	XEvent ev;
	Atom wm_delete;
	XClassHint class = { "xsoie", "Xsoie" };
	long mask = ExposureMask | KeyPressMask | ButtonPressMask |
		ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
		FocusChangeMask;
	int i;

	if ((i = front_start("xsoie", argc, argv, &win_w, &win_h)) >= 0)
		return i;
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fputs("xsoie: cannot open the display\n", stderr);
		return 1;
	}
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_xsel = XInternAtom(dpy, "XSOIE_SELECTION", False);
	win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0,
		(unsigned)win_w, (unsigned)win_h, 0, 0, 0xffffff);
	open_im(&mask);
	XSelectInput(dpy, win, mask);
	XSetClassHint(dpy, win, &class);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);
	set_icon();
	gc = XCreateGC(dpy, win, 0, NULL);
	cursors[SHELL_ARROW] = XCreateFontCursor(dpy, XC_left_ptr);
	cursors[SHELL_HAND] = XCreateFontCursor(dpy, XC_hand2);
	cursors[SHELL_TEXT] = XCreateFontCursor(dpy, XC_xterm);
	XMapWindow(dpy, win);
	resize(win_w, win_h);
	front_load();
	front_redraw();
	for (;;) {
		/* timers run while no X event waits */
		while (!XPending(dpy)) {
			struct pollfd p[2];
			int t = front_tick();

			if (XPending(dpy))
				break;
			p[0].fd = ConnectionNumber(dpy);
			p[0].events = POLLIN;
			p[1].fd = front_fd();
			p[1].events = POLLIN;
			poll(p, 2, t);
		}
		XNextEvent(dpy, &ev);
		if (XFilterEvent(&ev, None))
			continue;
		switch (ev.type) {
		case Expose:
			if (!ev.xexpose.count)
				front_expose();
			break;
		case ConfigureNotify:
			/* a window dragged bigger sends many: the last one */
			while (XCheckTypedWindowEvent(dpy, win, ConfigureNotify,
			    &ev))
				;
			if (ev.xconfigure.width != win_w ||
			    ev.xconfigure.height != win_h) {
				resize(ev.xconfigure.width < 100 ? 100 :
					ev.xconfigure.width,
					ev.xconfigure.height < 84 ? 84 :
					ev.xconfigure.height);
				front_redraw();
			}
			break;
		case FocusIn:
			if (xic)
				XSetICFocus(xic);
			break;
		case FocusOut:
			if (xic)
				XUnsetICFocus(xic);
			break;
		case ButtonPress:
			last = ev.xbutton.time;
			front_button((int)ev.xbutton.button, ev.xbutton.x,
				ev.xbutton.y, mods(ev.xbutton.state) &
				~SOIE_MOD_ALT);
			front_redraw();
			break;
		case ButtonRelease:
			front_release();
			front_redraw();
			break;
		case MotionNotify:
			/* only the last of consecutive moves */
			while (XPending(dpy)) {
				XEvent next;

				XPeekEvent(dpy, &next);
				if (next.type != MotionNotify)
					break;
				XNextEvent(dpy, &ev);
			}
			front_motion(ev.xmotion.x, ev.xmotion.y);
			front_redraw();
			break;
		case KeyPress:
			last = ev.xkey.time;
			key(&ev.xkey);
			front_redraw();
			break;
		case SelectionNotify:
			pasted(&ev.xselection);
			front_redraw();
			break;
		case SelectionRequest:
			give(&ev.xselectionrequest);
			break;
		case SelectionClear:
			i = ev.xselectionclear.selection == XA_PRIMARY ? 0 : 1;
			free(sel_text[i]);
			sel_text[i] = NULL;
			break;
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == wm_delete)
				return 0;
			break;
		}
	}
}
