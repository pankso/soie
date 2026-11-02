/*
 * about:auth, the login page shown for a page that asks for one (HTTP
 * 401 with a Basic challenge), in place of a dialog: the same in every
 * front.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_AUTH_H
#define SOIE_AUTH_H

#define AUTH_URL "about:auth"

/* Is url about:auth, its query and fragment aside? */
int auth_url(const char *url);
/* The page, malloc()ed: the login asked for url (realm: what the server
 * says, or ""), said: a line above the form, or NULL. url NULL: there
 * is nothing to log in to. */
char *auth_page(const char *url, const char *realm, const char *said);
/* The form's body: 1 log in (*user, *password malloc()ed), 0 cancel,
 * -1 neither. */
int auth_form(const char *post, char **user, char **password);

#endif
