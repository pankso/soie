/*
 * The browser's debugging: a trail of what happened, $SOIE_DEBUG, and
 * a crash report (~/.cache/soie/crash-PID.txt) on a fatal signal.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_DEBUG_H
#define SOIE_DEBUG_H

/* $SOIE_DEBUG: 1 loads, timings and script errors on stderr, 2 the
 * input and script logs as well, 3 scrolling. */
extern int debug_level;

/* Read $SOIE_DEBUG, catch the fatal signals. */
void debug_init(void);
/* A line into the trail, and on stderr when level <= debug_level; any
 * thread may call it. */
void debug(int level, const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;
/* The page a crash report names */
void debug_page(const char *url);
/* ~/.cache/soie (or $XDG_CACHE_HOME/soie), made if needed; NULL when
 * it cannot be. */
const char *debug_dir(void);
/* The trail, oldest first, into fd */
void debug_trail(int fd);

#endif
