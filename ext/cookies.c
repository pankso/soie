/*
 * The cookie jar (RFC 6265), kept in a Netscape cookies.txt file:
 * $SOIE_COOKIES, else $XDG_DATA_HOME/soie/cookies.txt, else
 * ~/.local/share/soie/cookies.txt. SOIE_COOKIES set to "" keeps the
 * jar in memory only.
 *
 * Third-party cookies are neither sent nor stored: a request whose
 * site differs from the page's site gets no cookie jar at all.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#include <soie/ext.h>

#define MAX_COOKIES 3000
#define MAX_PER_DOMAIN 60
#define MAX_SIZE 4096

struct cookie {
	char *name, *value, *domain, *path;
	long long expires;	/* 0: session cookie */
	long long created;
	unsigned char secure, httponly, hostonly;
};

static struct cookie *jar;
static int njar, capjar;
static char *file;		/* NULL: memory only */
static long long serial;	/* creation order within one second */
/* media players fetch from their own threads */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct url {
	char scheme[16];
	char host[256];
	char path[1024];
};

static char *sdup(const char *s, size_t len)
{
	char *d = malloc(len + 1);

	if (!d)
		abort();
	memcpy(d, s, len);
	d[len] = 0;
	return d;
}

static void lower(char *s)
{
	for (; *s; s++)
		*s = (char)tolower((unsigned char)*s);
}

/* Control bytes (tab included), DEL: never in a stored cookie */
static int has_ctl(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 0x20 || *s == 0x7f)
			return 1;
	return 0;
}

static int parse_url(const char *u, struct url *out)
{
	const char *p = strstr(u, "://"), *h, *e;
	size_t len;

	memset(out, 0, sizeof(*out));
	if (!p || (size_t)(p - u) >= sizeof(out->scheme))
		return -1;
	memcpy(out->scheme, u, (size_t)(p - u));
	lower(out->scheme);
	h = p + 3;
	e = h + strcspn(h, "/?#");
	p = memchr(h, '@', (size_t)(e - h));
	if (p)
		h = p + 1;
	len = (size_t)(e - h);
	if (*h == '[') {
		p = memchr(h, ']', len);
		len = p ? (size_t)(p - h) + 1 : len;
	} else {
		p = memchr(h, ':', len);
		if (p)
			len = (size_t)(p - h);
	}
	if (!len || len >= sizeof(out->host))
		return -1;
	memcpy(out->host, h, len);
	lower(out->host);
	len = strcspn(e, "?#");
	if (*e != '/' || len >= sizeof(out->path))
		strcpy(out->path, "/");
	else
		memcpy(out->path, e, len);
	return 0;
}

static int is_ip(const char *host)
{
	return *host == '[' || strspn(host, "0123456789.") == strlen(host);
}

/*
 * Labels in the public suffix of host, without the public suffix
 * list: one, or two under a country code whose second level is a
 * usual registry name ("co.uk", "com.au", "ne.jp"). Wrong now and
 * then (all of blogspot.com is one site), right for the common cases.
 */
static int labels(const char *host)
{
	int n = 1;

	for (; *host; host++)
		n += *host == '.';
	return n;
}

static int suffix_labels(const char *host)
{
	static const char *const sld[] = { "co", "com", "net", "org", "gov",
		"edu", "ac", "ne", "or", "go", "gob", "mil", "ltd", "plc",
		"sch", "nhs", "gv", "ed", "lg", "nom", "gouv", NULL };
	/* hosting where each customer has a subdomain: each one is a
	 * site, as the public suffix list says */
	static const char *const shared[] = { "github.io", "gitlab.io",
		"blogspot.com", "herokuapp.com", "appspot.com", "netlify.app",
		"vercel.app", "pages.dev", "workers.dev", "web.app",
		"firebaseapp.com", "azurewebsites.net", "cloudfront.net",
		"s3.amazonaws.com", "wordpress.com", "tumblr.com",
		"neocities.org", "glitch.me", "fly.dev", "onrender.com",
		"ngrok.io", "ngrok-free.app", "duckdns.org", "no-ip.org",
		"dyndns.org", "myshopify.com", "wixsite.com", "weebly.com",
		"codeberg.page", "sourceforge.io", "readthedocs.io", NULL };
	const char *tld = strrchr(host, '.'), *p;
	size_t len, hl = strlen(host), sl;
	int i;

	for (i = 0; shared[i]; i++) {
		sl = strlen(shared[i]);
		if (hl >= sl && !strcmp(host + hl - sl, shared[i]) &&
		    (hl == sl || host[hl - sl - 1] == '.'))
			return labels(shared[i]);
	}
	if (!tld || strlen(tld + 1) != 2)
		return 1;
	for (p = tld; p > host && p[-1] != '.'; p--)
		;
	len = (size_t)(tld - p);
	for (i = 0; sld[i]; i++)
		if (strlen(sld[i]) == len && !strncmp(p, sld[i], len))
			return 2;
	return 1;
}


/* The registrable domain: one label more than the suffix. */
static const char *site_of(const char *host)
{
	int skip;

	if (is_ip(host))
		return host;
	for (skip = labels(host) - suffix_labels(host) - 1; skip > 0; skip--)
		host = strchr(host, '.') + 1;
	return host;
}

static int public_suffix(const char *domain)
{
	return labels(domain) <= suffix_labels(domain);
}

static int domain_match(const char *host, const char *domain)
{
	size_t h = strlen(host), d = strlen(domain);

	if (!strcmp(host, domain))
		return 1;
	return h > d && host[h - d - 1] == '.' &&
		!strcmp(host + h - d, domain) && !is_ip(host);
}

static int path_match(const char *path, const char *cpath)
{
	size_t n = strlen(cpath);

	if (!n)
		return 0;
	return !strncmp(path, cpath, n) &&
		(cpath[n - 1] == '/' || path[n] == '/' || !path[n]);
}

static int third_party(const struct url *u, const char *first_party)
{
	struct url f;

	if (!first_party)
		return 0;
	if (parse_url(first_party, &f))
		return 1;
	return strcmp(site_of(u->host), site_of(f.host)) != 0;
}

static int secure_scheme(const struct url *u)
{
	return !strcmp(u->scheme, "https") || !strcmp(u->scheme, "wss") ||
		!strcmp(u->host, "localhost") || !strcmp(u->host, "127.0.0.1");
}

/* Dates (RFC 6265 5.1.1) */

static long long days_from_civil(int y, int m, int d)
{
	long long era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long long)doe - 719468;
}

static int parse_date(const char *s, long long *out)
{
	static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
	int hh = -1, mm = 0, ss = 0, day = -1, mon = -1, year = -1, n, i;
	const char *p;
	char tok[32];

	while (*s) {
		while (*s && !isalnum((unsigned char)*s) && *s != ':')
			s++;
		for (n = 0; *s && (isalnum((unsigned char)*s) ||
		     *s == ':') && n < 31; s++)
			tok[n++] = *s;
		tok[n] = 0;
		while (*s && (isalnum((unsigned char)*s) || *s == ':'))
			s++;
		if (!n)
			continue;
		if (hh < 0 && sscanf(tok, "%d:%d:%d", &hh, &mm, &ss) == 3)
			continue;
		hh = hh < 0 || mm < 0 ? -1 : hh;
		if (day < 0 && n <= 2 && isdigit((unsigned char)tok[0])) {
			day = atoi(tok);
			continue;
		}
		if (mon < 0 && n >= 3 && isalpha((unsigned char)tok[0])) {
			for (i = 0; i < 3; i++)
				tok[i] = (char)tolower((unsigned char)tok[i]);
			tok[3] = 0;
			p = strstr(months, tok);
			if (p && (p - months) % 3 == 0) {
				mon = (int)(p - months) / 3 + 1;
				continue;
			}
		}
		if (year < 0 && n >= 2 && n <= 4 &&
		    strspn(tok, "0123456789") == (size_t)n) {
			year = atoi(tok);
			if (year < 70 && n == 2)
				year += 2000;
			else if (year < 100 && n == 2)
				year += 1900;
		}
	}
	if (hh < 0 || hh > 23 || mm > 59 || ss > 59 || day < 1 ||
	    day > 31 || mon < 0 || year < 1601)
		return -1;
	*out = days_from_civil(year, mon, day) * 86400 + hh * 3600 +
		mm * 60 + ss;
	return 0;
}

/* The jar */

static void cookie_free(struct cookie *c)
{
	free(c->name);
	free(c->value);
	free(c->domain);
	free(c->path);
}

static void drop(int i)
{
	cookie_free(&jar[i]);
	jar[i] = jar[--njar];
}

static void purge(long long now)
{
	int i;

	for (i = njar - 1; i >= 0; i--)
		if (jar[i].expires && jar[i].expires <= now)
			drop(i);
}

static void save(void)
{
	char *tmp;
	FILE *f;
	int i, fd;

	if (!file)
		return;
	tmp = malloc(strlen(file) + 5);
	if (!tmp)
		return;
	sprintf(tmp, "%s.new", file);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	f = fd < 0 ? NULL : fdopen(fd, "w");
	if (!f) {
		if (fd >= 0)
			close(fd);
		free(tmp);
		return;
	}
	fprintf(f, "# Netscape HTTP Cookie File\n");
	for (i = 0; i < njar; i++) {
		if (!jar[i].expires)
			continue;	/* session cookies die with us */
		if (has_ctl(jar[i].name) || has_ctl(jar[i].value) ||
		    has_ctl(jar[i].domain) || has_ctl(jar[i].path))
			continue;	/* would break the file's lines */
		fprintf(f, "%s%s%s\t%s\t%s\t%s\t%lld\t%s\t%s\n",
			jar[i].httponly ? "#HttpOnly_" : "",
			jar[i].hostonly ? "" : ".", jar[i].domain,
			jar[i].hostonly ? "FALSE" : "TRUE", jar[i].path,
			jar[i].secure ? "TRUE" : "FALSE", jar[i].expires,
			jar[i].name, jar[i].value);
	}
	if (fclose(f) || rename(tmp, file))
		remove(tmp);
	free(tmp);
}

static void add(struct cookie *c)
{
	int i, same = 0, oldest = -1;

	for (i = 0; i < njar; i++) {
		if (!strcmp(jar[i].name, c->name) &&
		    !strcmp(jar[i].domain, c->domain) &&
		    !strcmp(jar[i].path, c->path)) {
			c->created = jar[i].created;
			drop(i);
			break;
		}
	}
	for (i = 0; i < njar; i++)
		if (!strcmp(jar[i].domain, c->domain)) {
			same++;
			if (oldest < 0 || jar[i].created < jar[oldest].created)
				oldest = i;
		}
	if (same >= MAX_PER_DOMAIN)
		drop(oldest);
	if (njar >= MAX_COOKIES) {
		oldest = 0;
		for (i = 1; i < njar; i++)
			if (jar[i].created < jar[oldest].created)
				oldest = i;
		drop(oldest);
	}
	if (njar == capjar) {
		capjar = capjar ? capjar * 2 : 64;
		jar = realloc(jar, (size_t)capjar * sizeof(*jar));
		if (!jar)
			abort();
	}
	jar[njar++] = *c;
}

static char *trim(const char *s, size_t len)
{
	while (len && (*s == ' ' || *s == '\t')) {
		s++;
		len--;
	}
	while (len && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		len--;
	return sdup(s, len);
}

static void set_locked(const char *url, const char *first_party,
	const char *line, int script);

static void set(void *data, const char *url, const char *first_party,
	const char *line, int script)
{
	(void)data;
	pthread_mutex_lock(&lock);
	set_locked(url, first_party, line, script);
	pthread_mutex_unlock(&lock);
}

static void set_locked(const char *url, const char *first_party,
	const char *line, int script)
{
	struct cookie c;
	struct url u;
	const char *p = line, *e, *eq;
	char *k, *v, *slash;
	long long now = (long long)time(NULL), maxage = 0, exp;
	int has_maxage = 0, i;

	if (!line || strlen(line) > MAX_SIZE || parse_url(url, &u) ||
	    third_party(&u, first_party) ||
	    (strcmp(u.scheme, "http") && strcmp(u.scheme, "https")))
		return;
	memset(&c, 0, sizeof(c));
	e = p + strcspn(p, ";");
	eq = memchr(p, '=', (size_t)(e - p));
	if (!eq)
		return;	/* nameless cookies are rare and odd: refuse */
	c.name = trim(p, (size_t)(eq - p));
	c.value = trim(eq + 1, (size_t)(e - eq - 1));
	if (!*c.name || has_ctl(c.name) || has_ctl(c.value))
		goto reject;
	for (p = e; *p == ';'; p = e) {
		p++;
		e = p + strcspn(p, ";");
		eq = memchr(p, '=', (size_t)(e - p));
		k = trim(p, (size_t)((eq ? eq : e) - p));
		v = eq ? trim(eq + 1, (size_t)(e - eq - 1)) : sdup("", 0);
		if (!strcasecmp(k, "expires")) {
			if (!parse_date(v, &exp))
				c.expires = exp > 0 ? exp : 1;
		} else if (!strcasecmp(k, "max-age")) {
			if ((*v == '-' || isdigit((unsigned char)*v)) &&
			    strspn(v + 1, "0123456789") == strlen(v + 1)) {
				maxage = atoll(v);
				has_maxage = 1;
			}
		} else if (!strcasecmp(k, "domain") && *v) {
			free(c.domain);
			c.domain = sdup(v + (*v == '.'), strlen(v + (*v == '.')));
			lower(c.domain);
		} else if (!strcasecmp(k, "path") && *v == '/') {
			free(c.path);
			c.path = sdup(v, strlen(v));
		} else if (!strcasecmp(k, "secure")) {
			c.secure = 1;
		} else if (!strcasecmp(k, "httponly")) {
			c.httponly = 1;
		}
		free(k);
		free(v);
	}
	if (has_maxage)
		c.expires = maxage <= 0 ? 1 : now + maxage;
	if (c.domain) {
		if (!domain_match(u.host, c.domain) ||
		    (public_suffix(c.domain) && strcmp(c.domain, u.host)))
			goto reject;
		if (!strcmp(c.domain, u.host) && is_ip(u.host))
			c.hostonly = 1;
	} else {
		c.domain = sdup(u.host, strlen(u.host));
		c.hostonly = 1;
	}
	if (!c.path) {
		slash = strrchr(u.path, '/');
		c.path = slash == u.path ? sdup("/", 1) :
			sdup(u.path, (size_t)(slash - u.path));
	}
	if (has_ctl(c.domain) || has_ctl(c.path))
		goto reject;	/* a Path or Domain smuggling a line */
	if ((c.secure && !secure_scheme(&u)) || (script && c.httponly))
		goto reject;
	if ((!strncmp(c.name, "__Secure-", 9) || !strncmp(c.name, "__Host-",
	     7)) && !c.secure)
		goto reject;
	if (!strncmp(c.name, "__Host-", 7) && (!c.hostonly ||
	    strcmp(c.path, "/")))
		goto reject;
	for (i = 0; i < njar; i++)
		if (!strcmp(jar[i].name, c.name) &&
		    !strcmp(jar[i].domain, c.domain) &&
		    !strcmp(jar[i].path, c.path) &&
		    ((script && jar[i].httponly) ||
		     (jar[i].secure && !secure_scheme(&u))))
			goto reject;
	c.created = now * 1000 + serial++ % 1000;
	if (c.expires && c.expires <= now) {
		/* a deletion */
		for (i = 0; i < njar; i++)
			if (!strcmp(jar[i].name, c.name) &&
			    !strcmp(jar[i].domain, c.domain) &&
			    !strcmp(jar[i].path, c.path)) {
				drop(i);
				save();
				break;
			}
		goto reject;
	}
	add(&c);
	if (c.expires)
		save();
	return;
reject:
	cookie_free(&c);
}

static int by_path(const void *a, const void *b)
{
	const struct cookie *x = *(struct cookie *const *)a;
	const struct cookie *y = *(struct cookie *const *)b;
	size_t lx = strlen(x->path), ly = strlen(y->path);

	if (lx != ly)
		return lx > ly ? -1 : 1;
	return x->created < y->created ? -1 : x->created > y->created;
}

static char *get_locked(const char *url, const char *first_party,
	int script);

static char *get(void *data, const char *url, const char *first_party,
	int script)
{
	char *r;

	(void)data;
	pthread_mutex_lock(&lock);
	r = get_locked(url, first_party, script);
	pthread_mutex_unlock(&lock);
	return r;
}

static char *get_locked(const char *url, const char *first_party,
	int script)
{
	struct cookie **v;
	struct url u;
	char *out, *o;
	size_t len = 0;
	int i, n = 0;

	if (parse_url(url, &u) || third_party(&u, first_party))
		return NULL;
	purge((long long)time(NULL));
	v = malloc(((size_t)njar + 1) * sizeof(*v));
	if (!v)
		return NULL;
	for (i = 0; i < njar; i++) {
		struct cookie *c = &jar[i];

		if (c->hostonly ? strcmp(c->domain, u.host) :
		    !domain_match(u.host, c->domain))
			continue;
		if (!path_match(u.path, c->path) ||
		    (c->secure && !secure_scheme(&u)) ||
		    (script && c->httponly))
			continue;
		v[n++] = c;
		len += strlen(c->name) + strlen(c->value) + 3;
	}
	if (!n) {
		free(v);
		return NULL;
	}
	qsort(v, (size_t)n, sizeof(*v), by_path);
	out = o = malloc(len + 1);
	if (!out)
		abort();
	for (i = 0; i < n; i++)
		o += sprintf(o, "%s%s=%s", i ? "; " : "", v[i]->name,
			v[i]->value);
	free(v);
	return out;
}

static void load(void)
{
	char line[MAX_SIZE + 512], *f[7], *p;
	struct cookie c;
	FILE *in = fopen(file, "r");
	int n, httponly;

	if (!in)
		return;
	while (fgets(line, sizeof(line), in)) {
		line[strcspn(line, "\r\n")] = 0;
		httponly = !strncmp(line, "#HttpOnly_", 10);
		p = httponly ? line + 10 : line;
		if (*p == '#' || !*p)
			continue;
		for (n = 0; n < 7 && p; n++) {
			f[n] = p;
			p = strchr(p, '\t');
			if (p)
				*p++ = 0;
		}
		/* what set() would refuse, the file does not get either */
		if (n < 7 || !*f[0] || f[2][0] != '/' || !*f[5] ||
		    atoll(f[4]) <= 0 || has_ctl(f[6]) ||
		    (f[0][0] == '.' && public_suffix(f[0] + 1)))
			continue;
		memset(&c, 0, sizeof(c));
		c.hostonly = *f[0] != '.';
		c.domain = sdup(f[0] + !c.hostonly, strlen(f[0] + !c.hostonly));
		c.path = sdup(f[2], strlen(f[2]));
		c.secure = !strcmp(f[3], "TRUE");
		c.expires = atoll(f[4]);
		c.name = sdup(f[5], strlen(f[5]));
		c.value = sdup(f[6], strlen(f[6]));
		c.httponly = (unsigned char)httponly;
		c.created = serial++;
		add(&c);
	}
	fclose(in);
	purge((long long)time(NULL));
}

static char *jar_path(void)
{
	const char *env = getenv("SOIE_COOKIES"), *base;
	char *path, *p;

	if (env)
		return *env ? sdup(env, strlen(env)) : NULL;
	base = getenv("XDG_DATA_HOME");
	if (base && *base) {
		path = malloc(strlen(base) + 32);
		if (path)
			sprintf(path, "%s/soie/cookies.txt", base);
	} else {
		base = getenv("HOME");
		if (!base)
			return NULL;
		path = malloc(strlen(base) + 48);
		if (path)
			sprintf(path, "%s/.local/share/soie/cookies.txt", base);
	}
	if (!path)
		return NULL;
	/* mkdir -p, owner only */
	for (p = path + 1; (p = strchr(p, '/')); p++) {
		*p = 0;
		mkdir(path, 0700);
		*p = '/';
	}
	return path;
}

static struct soie_ext_cookies hooks = {
	{ SOIE_EXT_ABI, sizeof(hooks), SOIE_EXT_COOKIES, "cookies", NULL },
	get, set
};

int soie_ext_init(void)
{
	file = jar_path();
	if (file)
		load();
	return soie_ext_register(&hooks.base);
}
