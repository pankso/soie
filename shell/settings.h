/*
 * The settings of the fronts without settings of their own (xsoie,
 * wsoie): ~/.config/soie/settings, one key=value a line, and
 * about:settings, the page that shows and changes them. shell.h has
 * what the fronts see (shell_settings()); this is the shell's side.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_SETTINGS_H
#define SOIE_SETTINGS_H

#include "shell.h"

#define SETTINGS_URL "about:settings"

/* The settings, NULL until a front asked for them (shell_settings()):
 * without them about:settings is not found and nothing changes. */
const struct shell_settings *settings_get(void);
/* Is url about:settings, its query and fragment aside? */
int settings_url(const char *url);
/* The page, malloc()ed. post: the body of its form, applied and saved
 * first (NULL: only shown); *changed is 1 when a setting changed. */
char *settings_page(const char *post, int *changed);
/* The shell's own pages' style sheet, a <style> element */
extern const char settings_style[];
/* %XX and + of a urlencoded value, in place */
void settings_decode(char *s);

#endif
