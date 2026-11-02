/*
 * Settings: ~/.config/soie/settings (or $XDG_CONFIG_HOME/soie), one
 * key=value a line, '#' a comment; about:settings shows them in a form
 * that posts to itself, without a script. Unknown keys and bad values
 * are left out: the defaults stand.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <soie/soie.h>
#include "net.h"
#include "settings.h"
#include "debug.h"

static struct shell_settings set = { "", "", "", 1,
	SHELL_COOKIES_NO_THIRD_PARTY, SHELL_COLORS_SYSTEM, 100, 1 };
static int loaded;

static const char *const cookie_names[] = { "all", "no-third-party",
	"none" };
static const char *const color_names[] = { "system", "light", "dark" };

/* ~/.config/soie/NAME (or $XDG_CONFIG_HOME/soie/NAME); 0 without $HOME */
static int config_path(char *out, size_t size, const char *name)
{
	const char *c = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");

	if (c && *c == '/')
		snprintf(out, size, "%s/soie/%s", c, name);
	else if (h && *h)
		snprintf(out, size, "%s/.config/soie/%s", h, name);
	else
		return 0;
	return 1;
}

static int name_of(const char *const *names, int n, const char *v)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(names[i], v))
			return i;
	return -1;
}

/* What a URL setting may be: a page the user could type */
static int page_url(const char *v)
{
	return !strncmp(v, "https://", 8) || !strncmp(v, "http://", 7) ||
		!strncmp(v, "file:///", 8) || !strncmp(v, "about:", 6);
}

static int search_url(const char *v)
{
	const char *p = strstr(v, "%s");

	return (!strncmp(v, "https://", 8) || !strncmp(v, "http://", 7)) &&
		p && !strstr(p + 2, "%s") && !strchr(v, '\n');
}

/* One setting from the file or the form; 0 when the value is bad */
static int put(struct shell_settings *t, const char *k, const char *v)
{
	int n;

	if (!strcmp(k, "home") && (!*v || page_url(v)) &&
	    strlen(v) < sizeof(t->home))
		strcpy(t->home, v);
	else if (!strcmp(k, "search") && (!*v || search_url(v)) &&
	    strlen(v) < sizeof(t->search))
		strcpy(t->search, v);
	else if (!strcmp(k, "downloads") && (!*v || *v == '/') &&
	    strlen(v) < sizeof(t->downloads))
		strcpy(t->downloads, v);
	else if (!strcmp(k, "scripts") && (!strcmp(v, "0") ||
	    !strcmp(v, "1")))
		t->scripts = *v == '1';
	else if (!strcmp(k, "block") && (!strcmp(v, "0") || !strcmp(v, "1")))
		t->block = *v == '1';
	else if (!strcmp(k, "cookies") && (n = name_of(cookie_names, 3, v)) >=
	    0)
		t->cookies = n;
	else if (!strcmp(k, "colors") && (n = name_of(color_names, 3, v)) >= 0)
		t->colors = n;
	else if (!strcmp(k, "zoom") && (n = atoi(v)) >= 30 && n <= 300)
		t->zoom = n;
	else
		return 0;
	return 1;
}

/* What the shell does itself with them */
static void apply(void)
{
	soie_cookies_policy(set.cookies == SHELL_COOKIES_ALL ?
		SOIE_COOKIES_ALL : set.cookies == SHELL_COOKIES_NONE ?
		SOIE_COOKIES_NONE : SOIE_COOKIES_NO_THIRD_PARTY);
	net_blocking(set.block);
}

static void load_file(void)
{
	char path[4096], line[2048];
	FILE *f;

	if (!config_path(path, sizeof(path), "settings") ||
	    !(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '=');

		line[strcspn(line, "\r\n")] = '\0';
		if (*line == '#' || !v)
			continue;
		*v++ = '\0';
		if (!put(&set, line, v))
			debug(1, "settings: %s=%s left out", line, v);
	}
	fclose(f);
}

/* The folders above path, made as mkdir -p does */
static void make_dirs(const char *path)
{
	char d[4096], *p;

	snprintf(d, sizeof(d), "%s", path);
	for (p = d + 1; (p = strchr(p, '/')); p++) {
		*p = '\0';
		mkdir(d, 0755);
		*p = '/';
	}
}

static int save_file(void)
{
	char path[4096], tmp[4200];
	FILE *f;

	if (!config_path(path, sizeof(path), "settings"))
		return -1;
	make_dirs(path);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (!(f = fopen(tmp, "w")))
		return -1;
	fprintf(f, "# Soie's settings, written by about:settings\n"
		"home=%s\nsearch=%s\nscripts=%d\ncookies=%s\ncolors=%s\n"
		"zoom=%d\nblock=%d\ndownloads=%s\n", set.home, set.search,
		set.scripts, cookie_names[set.cookies],
		color_names[set.colors], set.zoom, set.block, set.downloads);
	if (fclose(f) || rename(tmp, path)) {
		remove(tmp);
		return -1;
	}
	return 0;
}

const struct shell_settings *shell_settings(void)
{
	if (!loaded) {
		loaded = 1;
		load_file();
		apply();
	}
	return &set;
}

const struct shell_settings *settings_get(void)
{
	return loaded ? &set : NULL;
}

int shell_settings_dark(void)
{
	if (loaded && set.colors != SHELL_COLORS_SYSTEM)
		return set.colors == SHELL_COLORS_DARK;
	return shell_system_dark();
}

int settings_url(const char *url)
{
	size_t n = strlen(SETTINGS_URL);

	return !strncasecmp(url, SETTINGS_URL, n) && (!url[n] ||
		url[n] == '?' || url[n] == '#');
}

/* The page */

struct buf {
	char *s;
	size_t len, cap;
};

static void add(struct buf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2;
		if (!(b->s = realloc(b->s, b->cap)))
			abort();
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static void adds(struct buf *b, const char *s)
{
	add(b, s, strlen(s));
}

static void addesc(struct buf *b, const char *s)
{
	for (; *s; s++) {
		const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" :
			*s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;

		if (e)
			adds(b, e);
		else
			add(b, s, 1);
	}
}

/* A text field */
static void field(struct buf *b, const char *name, const char *label,
	const char *value, const char *hint)
{
	adds(b, "<p><label>");
	adds(b, label);
	adds(b, "<input type=text name=");
	adds(b, name);
	adds(b, " value=\"");
	addesc(b, value);
	adds(b, "\" placeholder=\"");
	addesc(b, hint);
	adds(b, "\"></label>");
}

/* Radio buttons, one of names */
static void choice(struct buf *b, const char *name, const char *const *names,
	const char *const *labels, int n, int cur)
{
	int i;

	for (i = 0; i < n; i++) {
		adds(b, "<label class=opt><input type=radio name=");
		adds(b, name);
		adds(b, " value=");
		adds(b, names[i]);
		adds(b, i == cur ? " checked> " : "> ");
		adds(b, labels[i]);
		adds(b, "</label>");
	}
}

static void check(struct buf *b, const char *name, const char *label,
	int on)
{
	adds(b, "<label class=opt><input type=checkbox name=");
	adds(b, name);
	adds(b, " value=1");
	adds(b, on ? " checked> " : "> ");
	adds(b, label);
	adds(b, "</label>");
}

/* %XX and + of a urlencoded value, in place */
void settings_decode(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s == '+')
			*o++ = ' ';
		else if (*s == '%' && isxdigit((unsigned char)s[1]) &&
		    isxdigit((unsigned char)s[2])) {
			char h[3] = { s[1], s[2], 0 };

			*o++ = (char)strtol(h, NULL, 16);
			s += 2;
		} else
			*o++ = *s;
	}
	*o = '\0';
}

/*
 * The form's body: the saving form sends every setting (an unchecked
 * box sends nothing: "save" says the form was there), the cookies
 * form "clear" alone. Returns what to say on the page, *ok 1 when all
 * went well.
 */
static const char *post_apply(const char *post, int *changed, int *ok)
{
	struct shell_settings t = set;
	char *body = strdup(post), *p, *next;
	int save = 0, clear = 0, bad = 0;

	if (!body)
		return NULL;
	t.scripts = t.block = 0;
	for (p = body; p && *p; p = next) {
		char *v;

		if ((next = strchr(p, '&')))
			*next++ = '\0';
		if ((v = strchr(p, '=')))
			*v++ = '\0';
		else
			v = p + strlen(p);
		settings_decode(p);
		settings_decode(v);
		if (!strcmp(p, "save"))
			save = 1;
		else if (!strcmp(p, "clear"))
			clear = 1;
		else if (!put(&t, p, v))
			bad = 1;
	}
	free(body);
	if (clear) {
		soie_cookies_clear(0);
		net_auth_clear(0);
		debug(1, "settings: cookies and logins cleared");
		*ok = 1;
		return "The cookies and logins are cleared.";
	}
	if (!save)
		return NULL;
	*changed = strcmp(t.home, set.home) || strcmp(t.search, set.search) ||
		strcmp(t.downloads, set.downloads) || t.scripts != set.scripts ||
		t.cookies != set.cookies || t.colors != set.colors ||
		t.zoom != set.zoom || t.block != set.block;
	set = t;
	apply();
	if (save_file())
		return "Could not write ~/.config/soie/settings.";
	debug(1, "settings saved");
	*ok = !bad;
	return bad ? "Saved; what was not valid was left as it was." :
		"Saved.";
}

const char settings_style[] =
	"<style>"
	":root{color-scheme:light;--bg:#f4f6f8;--card:#fff;--text:#2c3e50;"
	"--muted:#7f8c8d;--line:#d5dde3;--accent:#e74c3c}"
	"@media(prefers-color-scheme:dark){:root{--bg:#141414;--card:#1e1e1e;"
	"--text:#e0e0e0;--muted:#8a8a8a;--line:#333}}"
	"body{margin:0;background:var(--bg);color:var(--text);"
	"font:15px/1.5 sans-serif}"
	"main{max-width:40em;margin:0 auto;padding:1.5em 1em}"
	"h1{font-size:1.6em;margin:0 0 .8em}"
	"h2{font-size:1.05em;margin:0 0 .6em}"
	"section{background:var(--card);border:1px solid var(--line);"
	"border-radius:8px;padding:1em 1.2em;margin:0 0 1em}"
	"label{display:block;margin:.3em 0}"
	"label.opt{display:inline-block;margin:.2em 1.2em .2em 0}"
	"input[type=text],input[type=password]{display:block;width:100%;"
	"box-sizing:border-box;"
	"margin-top:.2em;padding:5px 8px;border:1px solid var(--line);"
	"border-radius:4px;background:var(--bg);color:var(--text);"
	"font:14px sans-serif}"
	"p{margin:.5em 0}.hint{color:var(--muted);font-size:.9em}"
	"button{font:14px sans-serif;padding:6px 16px;border-radius:4px;"
	"border:1px solid var(--line);background:var(--card);"
	"color:var(--text)}"
	"button.go{background:var(--accent);border-color:var(--accent);"
	"color:#fff}"
	".said{background:var(--accent);color:#fff;border-radius:6px;"
	"padding:.5em 1em;margin:0 0 1em}.said.ok{background:#27ae60}"
	"code{font-size:.9em;overflow-wrap:anywhere}"
	"</style>";

char *settings_page(const char *post, int *changed)
{
	static const char *const cookie_labels[] = { "All",
		"No third-party", "None" };
	static const char *const color_labels[] = { "As the system's",
		"Light", "Dark" };
	struct buf b = { NULL, 0, 0 };
	const char *said = NULL;
	char zoom[16], block[4096];
	int ok = 0;

	*changed = 0;
	if (post)
		said = post_apply(post, changed, &ok);
	adds(&b, "<!DOCTYPE html><html><head><meta charset=utf-8>"
		"<title>Settings</title>");
	adds(&b, settings_style);
	adds(&b, "</head><body><main><h1>Settings</h1>");
	if (said) {
		adds(&b, ok ? "<p class=\"said ok\">" : "<p class=said>");
		adds(&b, said);
		adds(&b, "</p>");
	}
	adds(&b, "<form method=post action=\"" SETTINGS_URL "\">"
		"<input type=hidden name=save value=1>");

	adds(&b, "<section><h2>Browsing</h2>");
	field(&b, "home", "Start page", set.home, "about:soie");
	field(&b, "search", "Search with (a URL, %s for the words)",
		set.search, "https://lite.duckduckgo.com/lite/?q=%s");
	adds(&b, "</section>");

	adds(&b, "<section><h2>Pages</h2><p>");
	check(&b, "scripts", "JavaScript", set.scripts);
	adds(&b, "</p><p>Colours: ");
	choice(&b, "colors", color_names, color_labels, 3, set.colors);
	snprintf(zoom, sizeof(zoom), "%d", set.zoom);
	adds(&b, "</p>");
	field(&b, "zoom", "Zoom, in percent (30 to 300)", zoom, "100");
	adds(&b, "</section>");

	adds(&b, "<section><h2>Privacy</h2><p>Cookies: ");
	choice(&b, "cookies", cookie_names, cookie_labels, 3, set.cookies);
	adds(&b, "</p><p>");
	check(&b, "block", "Block the hosts of my list", set.block);
	adds(&b, "</p><p class=hint>The list: <code>");
	if (getenv("SOIE_BLOCK") && *getenv("SOIE_BLOCK"))
		snprintf(block, sizeof(block), "%s", getenv("SOIE_BLOCK"));
	else if (!config_path(block, sizeof(block), "block.txt"))
		strcpy(block, "~/.config/soie/block.txt");
	addesc(&b, block);
	adds(&b, "</code>, a host a line.</p></section>");

	adds(&b, "<section><h2>Downloads</h2>");
	field(&b, "downloads", "Save files in", set.downloads, "~/Downloads");
	adds(&b, "</section>");

	adds(&b, "<p><button class=go type=submit>Save</button></p></form>");
	adds(&b, "<form method=post action=\"" SETTINGS_URL "\"><p>"
		"<button type=submit name=clear value=1>Clear the cookies and logins"
		"</button></p></form>");
	adds(&b, "<p class=hint>Scripts and cookies apply to the next pages; "
		"other windows take the settings when they open.</p>");
	adds(&b, "</main></body></html>");
	return b.s;
}
