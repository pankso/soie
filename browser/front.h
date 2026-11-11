/*
 * What the browser's fronts share (xsoie on Xlib): the toolbar, the
 * find bar and the status line, little pages painted by Soie over the
 * shell's page view; the keys, clicks and pastes they take; and the
 * window's pixels, painted again only where they changed. A front's
 * toolkit part owns the window and its pixels, turns its events into
 * front_*() calls and defines the tk_*() ones. Coordinates are the
 * window's, its top left corner at 0,0.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_FRONT_H
#define SOIE_FRONT_H

/* Read the command line, start the shell; name is the program's, for
 * its usage and new windows. Returns -1 to go on with a window of *w x
 * *h, else the exit status (a usage error, a -o render done). */
int front_start(const char *name, int argc, char **argv, int *w, int *h);
/* The descriptor to poll with the toolkit's: shell_tick() is due */
int front_fd(void);
/* The window's pixels: w x h, stride bytes a row, B G R A bytes when
 * bgra, else R G B A. All of it is painted again; px is the front's
 * until the next call. */
void front_buffer(unsigned char *px, int w, int h, int stride, int bgra);
/* The window is up: load the page asked for. */
void front_load(void);
/* The window got the focus: follow the desktop if it went dark or
 * light since (shell_system_dark()). */
void front_focus(void);
/* It lost the focus: a select's open list closes. */
void front_unfocus(void);
/* Run what the shell has due and show what changed; returns the ms the
 * toolkit may wait for events, -1 without a limit. */
int front_tick(void);
/* Paint what the input changed and show it: after every event. */
void front_redraw(void);
/* The window lost its pixels on screen: show them all again. */
void front_expose(void);

/* Input. Keys are DOM key values, a letter as typed ("l", "L") also
 * with Ctrl; text is what the key typed, in UTF-8; mods SOIE_MOD_*. */
void front_key(const char *key, const char *text, int mods);
/* Buttons numbered as X does: 1 left, 2 middle, 3 right, 4 and 5 the
 * wheel, 8 back, 9 forward. */
void front_button(int button, int x, int y, int mods);
void front_release(void);
void front_motion(int x, int y);
/* The wheel, dy pixels down */
void front_wheel(int dy);
/* The text a tk_paste(clipboard, to) asked for */
void front_pasted(int to, const char *text);
/* The window's icon, the logo: size x size, R G B A, not premultiplied */
void front_icon(unsigned char *rgba, int size);

/* Toolkit calls */
/* The page's title, for the window's */
void tk_title(const char *title);
/* Put the rects (x, y, w, h) of the pixels on screen, n of them. */
void tk_show(const int (*rects)[4], int n);
/* The pointer: SHELL_ARROW, SHELL_HAND or SHELL_TEXT */
void tk_cursor(int shape);
/* text becomes the clipboard (1) or the primary selection (0). */
void tk_copy(int clipboard, const char *text);
/* Ask for the clipboard's (1) or the primary selection's (0) text, for
 * front_pasted(to, text) when it comes. */
void tk_paste(int clipboard, int to);

#endif
