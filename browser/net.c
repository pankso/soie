/*
 * Networking for the browser: HTTP/1.1 over TCP or TLS (GnuTLS, system
 * trust store, SNI, certificate and host name checks), redirects,
 * chunked bodies, gzip and deflate, file:// URLs. Cookies, request
 * filters and other URL schemes come from the extensions.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <gnutls/gnutls.h>
#include <zlib.h>
#include "net.h"

#define TIMEOUT_MS 20000

static gnutls_certificate_credentials_t creds;

static int tls_ok;

static void tls_setup(void)
{
	if (gnutls_global_init() < 0 ||
	    gnutls_certificate_allocate_credentials(&creds) < 0)
		return;
	if (gnutls_certificate_set_x509_system_trust(creds) <= 0)
		gnutls_certificate_set_x509_trust_file(creds,
			"/etc/ssl/certs/ca-certificates.crt",
			GNUTLS_X509_FMT_PEM);
	tls_ok = 1;
}

/* Once, whichever thread comes first (media players fetch too). */
static int tls_init(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, tls_setup);
	return tls_ok;
}

struct conn {
	int fd;
	gnutls_session_t tls;
};

static void set_error(struct response *r, const char *msg)
{
	snprintf(r->error, sizeof(r->error), "%s", msg);
}

/* Connect with a timeout. */
static int dial(const char *host, const char *port, struct response *r)
{
	struct addrinfo hints, *res, *a;
	int fd = -1, err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((err = getaddrinfo(host, port, &hints, &res))) {
		set_error(r, gai_strerror(err));
		return -1;
	}
	for (a = res; a; a = a->ai_next) {
		struct pollfd p;
		int flags, soerr = 0;
		socklen_t len = sizeof(soerr);

		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0)
			continue;
		flags = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		if (connect(fd, a->ai_addr, a->ai_addrlen) && errno !=
		    EINPROGRESS) {
			close(fd);
			fd = -1;
			continue;
		}
		p.fd = fd;
		p.events = POLLOUT;
		if (poll(&p, 1, TIMEOUT_MS) == 1 &&
		    !getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) &&
		    !soerr) {
			fcntl(fd, F_SETFL, flags);
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		set_error(r, "could not connect");
	return fd;
}

static int conn_write(struct conn *c, const char *s, size_t n)
{
	while (n) {
		ssize_t k = c->tls ? gnutls_record_send(c->tls, s, n) :
			write(c->fd, s, n);

		if (k == GNUTLS_E_AGAIN || k == GNUTLS_E_INTERRUPTED ||
		    (k < 0 && !c->tls && errno == EINTR))
			continue;
		if (k <= 0)
			return -1;
		s += k;
		n -= (size_t)k;
	}
	return 0;
}

static ssize_t conn_read(struct conn *c, char *s, size_t n)
{
	struct pollfd p;
	ssize_t k;

	for (;;) {
		if (!c->tls || !gnutls_record_check_pending(c->tls)) {
			p.fd = c->fd;
			p.events = POLLIN;
			if (poll(&p, 1, TIMEOUT_MS) != 1)
				return -1;
		}
		k = c->tls ? gnutls_record_recv(c->tls, s, n) :
			read(c->fd, s, n);
		if (k == GNUTLS_E_AGAIN || k == GNUTLS_E_INTERRUPTED)
			continue;
		if (k == GNUTLS_E_PREMATURE_TERMINATION)
			return 0;	/* servers that just close */
		return k;
	}
}

static int tls_start(struct conn *c, const char *host, struct response *r)
{
	int ret;

	if (!tls_init()) {
		set_error(r, "TLS unavailable");
		return -1;
	}
	gnutls_init(&c->tls, GNUTLS_CLIENT);
	gnutls_set_default_priority(c->tls);
	gnutls_credentials_set(c->tls, GNUTLS_CRD_CERTIFICATE, creds);
	gnutls_server_name_set(c->tls, GNUTLS_NAME_DNS, host, strlen(host));
	gnutls_session_set_verify_cert(c->tls, host, 0);
	gnutls_transport_set_int(c->tls, c->fd);
	gnutls_handshake_set_timeout(c->tls, TIMEOUT_MS);
	do
		ret = gnutls_handshake(c->tls);
	while (ret < 0 && !gnutls_error_is_fatal(ret));
	if (ret < 0) {
		set_error(r, gnutls_strerror(ret));
		return -1;
	}
	return 0;
}

static void conn_close(struct conn *c)
{
	if (c->tls) {
		gnutls_bye(c->tls, GNUTLS_SHUT_WR);
		gnutls_deinit(c->tls);
	}
	if (c->fd >= 0)
		close(c->fd);
}

/* URL pieces */

struct url {
	char scheme[16], host[256], port[8];
	char *path;		/* path and query, malloc'ed */
};

static int parse_url(const char *u, struct url *p)
{
	const char *s = strstr(u, "://"), *h, *e, *colon;
	size_t n;

	memset(p, 0, sizeof(*p));
	if (!s || (size_t)(s - u) >= sizeof(p->scheme))
		return -1;
	memcpy(p->scheme, u, (size_t)(s - u));
	h = s + 3;
	for (e = h; *e && *e != '/' && *e != '?' && *e != '#'; e++)
		;
	colon = memchr(h, ':', (size_t)(e - h));
	n = (size_t)((colon ? colon : e) - h);
	if (memchr(h, '@', (size_t)(e - h)))
		return -1;	/* no credentials in URLs */
	if (n >= sizeof(p->host))
		return -1;
	memcpy(p->host, h, n);
	if (colon && (size_t)(e - colon - 1) < sizeof(p->port))
		memcpy(p->port, colon + 1, (size_t)(e - colon - 1));
	else
		strcpy(p->port, !strcmp(p->scheme, "https") ? "443" : "80");
	n = strcspn(e, "#");
	p->path = malloc(n + 2);
	if (!p->path)
		return -1;
	if (!n || *e != '/') {
		p->path[0] = '/';
		memcpy(p->path + 1, e, n);
		p->path[n + 1] = '\0';
	} else {
		memcpy(p->path, e, n);
		p->path[n] = '\0';
	}
	return 0;
}

/* Byte buffer */

static void grow(struct response *r, size_t more)
{
	if (r->len + more + 1 > r->cap) {
		while (r->len + more + 1 > r->cap)
			r->cap = r->cap ? r->cap * 2 : 16384;
		r->body = realloc(r->body, r->cap);
		if (!r->body)
			abort();
	}
}

static void add(struct response *r, const char *s, size_t n)
{
	grow(r, n);
	memcpy(r->body + r->len, s, n);
	r->len += n;
	r->body[r->len] = '\0';
}

static void adds(struct response *r, const char *s)
{
	add(r, s, strlen(s));
}

/* Read everything until the peer closes (headers and body raw). */
static int read_all(struct conn *c, struct response *raw, size_t max)
{
	char buf[16384];
	ssize_t k;

	while ((k = conn_read(c, buf, sizeof(buf))) > 0) {
		add(raw, buf, (size_t)k);
		if (raw->len > max)
			return -1;
	}
	return k < 0 && !raw->len ? -1 : 0;
}

static const char *header(const char *h, const char *name)
{
	size_t n = strlen(name);

	for (; h && *h; h = strchr(h, '\n') ? strchr(h, '\n') + 1 : NULL)
		if (!strncasecmp(h, name, n) && h[n] == ':') {
			h += n + 1;
			while (*h == ' ' || *h == '\t')
				h++;
			return h;
		}
	return NULL;
}

static size_t header_len(const char *v)
{
	size_t n = strcspn(v, "\r\n");

	while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
		n--;
	return n;
}

static int dechunk(struct response *r, const char *s, size_t n)
{
	const char *end = s + n;

	while (s < end) {
		char *e;
		unsigned long size = strtoul(s, &e, 16);

		if (e == s)
			return -1;
		s = memchr(e, '\n', (size_t)(end - e));
		if (!s)
			return -1;
		s++;
		if (!size)
			return 0;
		if (size > (size_t)(end - s))
			size = (size_t)(end - s);
		add(r, s, size);
		s += size;
		if (s < end && *s == '\r')
			s++;
		if (s < end && *s == '\n')
			s++;
	}
	return 0;
}

static int inflate_body(struct response *r)
{
	z_stream z;
	struct response out;
	char buf[16384];
	int ret;

	memset(&z, 0, sizeof(z));
	memset(&out, 0, sizeof(out));
	if (inflateInit2(&z, 15 + 32) != Z_OK)	/* gzip or zlib */
		return -1;
	z.next_in = (unsigned char *)r->body;
	z.avail_in = (unsigned)r->len;
	do {
		z.next_out = (unsigned char *)buf;
		z.avail_out = sizeof(buf);
		ret = inflate(&z, Z_NO_FLUSH);
		if (ret == Z_DATA_ERROR && !out.len) {
			/* raw deflate, as some servers send */
			inflateEnd(&z);
			memset(&z, 0, sizeof(z));
			inflateInit2(&z, -15);
			z.next_in = (unsigned char *)r->body;
			z.avail_in = (unsigned)r->len;
			continue;
		}
		if (ret != Z_OK && ret != Z_STREAM_END)
			break;
		add(&out, buf, sizeof(buf) - z.avail_out);
	} while (ret != Z_STREAM_END && z.avail_in);
	inflateEnd(&z);
	free(r->body);
	r->body = out.body;
	r->len = out.len;
	r->cap = out.cap;
	return 0;
}

static int fetch_file(const char *url, struct response *r)
{
	FILE *f = fopen(url + 7, "rb");
	char buf[16384];
	size_t k;

	if (!f) {
		set_error(r, strerror(errno));
		return -1;
	}
	while ((k = fread(buf, 1, sizeof(buf), f)) > 0)
		add(r, buf, k);
	fclose(f);
	r->status = 200;
	{
		size_t n = strlen(url);

		if (n > 5 && !strcasecmp(url + n - 5, ".html"))
			strcpy(r->type, "text/html");
		else if (n > 4 && !strcasecmp(url + n - 4, ".htm"))
			strcpy(r->type, "text/html");
		else if (n > 4 && !strcasecmp(url + n - 4, ".css"))
			strcpy(r->type, "text/css");
		else if (n > 4 && !strcasecmp(url + n - 4, ".txt"))
			strcpy(r->type, "text/plain");
	}
	return 0;
}

/* Whether a "Name: value\r\n" list has the header. */
static int has_header(const char *h, const char *name)
{
	size_t n = strlen(name);

	for (; h && *h; h = strchr(h, '\n') ? strchr(h, '\n') + 1 : NULL)
		if (!strncasecmp(h, name, n) && h[n] == ':')
			return 1;
	return 0;
}

/* Keep the response headers, names in lower case, and hand each
 * Set-Cookie to the jar. */
static void keep_headers(const char *raw, const char *end, const char *url,
	const char *first_party, struct response *r)
{
	struct response h;
	const char *p = strchr(raw, '\n'), *e, *colon;
	char *line;
	int cookie;

	memset(&h, 0, sizeof(h));
	for (p = p ? p + 1 : end; p < end; p = e + 1) {
		e = memchr(p, '\n', (size_t)(end - p));
		if (!e)
			e = end;
		colon = memchr(p, ':', (size_t)(e - p));
		if (!colon || *p == '\r')
			continue;
		cookie = colon - p == 10 && !strncasecmp(p, "set-cookie", 10);
		grow(&h, (size_t)(e - p) + 2);
		for (; p < colon; p++)
			h.body[h.len++] = (char)tolower((unsigned char)*p);
		add(&h, colon, (size_t)(e - colon));
		if (h.body[h.len - 1] == '\r')
			h.len--;
		add(&h, "\r\n", 2);
		if (cookie) {
			line = strndup(colon + 1, header_len(colon + 1));
			if (line) {
				char *v = line + strspn(line, " \t");

				soie_cookies_set(url, first_party ? first_party :
					url, v, 0);
				free(line);
			}
		}
	}
	free(r->headers);
	r->headers = h.body;
}

/* One request; redirects are followed by net_fetch(). */
static int request(const char *url, const soie_request *rq,
	const char *method, const char *body_in, size_t body_len,
	struct response *r, char **location)
{
	struct url u;
	struct conn c = { -1, NULL };
	struct response raw, req;
	const char *body, *v, *fp = rq->first_party ? rq->first_party : url;
	char *cookie, *hend;
	int ret = -1;

	*location = NULL;
	if (parse_url(url, &u)) {
		set_error(r, "bad URL");
		return -1;
	}
	memset(&raw, 0, sizeof(raw));
	memset(&req, 0, sizeof(req));
	c.fd = dial(u.host, u.port, r);
	if (c.fd < 0)
		goto out;
	if (!strcmp(u.scheme, "https") && tls_start(&c, u.host, r))
		goto out;
	{
		char line[1024];

		snprintf(line, sizeof(line), "%s ", method);
		adds(&req, line);
		adds(&req, u.path);
		snprintf(line, sizeof(line), " HTTP/1.1\r\nHost: %s\r\n"
			"User-Agent: Mozilla/5.0 (X11; Linux) Soie/0.1\r\n"
			"Accept-Encoding: gzip, deflate\r\n"
			"Connection: close\r\n", u.host);
		adds(&req, line);
	}
	if (!has_header(rq->headers, "Accept"))
		adds(&req, "Accept: text/html,application/xhtml+xml,*/*;"
			"q=0.8\r\n");
	if ((cookie = soie_cookies_get(url, fp, 0))) {
		adds(&req, "Cookie: ");
		adds(&req, cookie);
		adds(&req, "\r\n");
		free(cookie);
	}
	if (rq->headers)
		adds(&req, rq->headers);
	if (body_in) {
		char line[128];

		if (!has_header(rq->headers, "Content-Type"))
			adds(&req, "Content-Type: application/"
				"x-www-form-urlencoded\r\n");
		snprintf(line, sizeof(line), "Content-Length: %zu\r\n",
			body_len);
		adds(&req, line);
	}
	adds(&req, "\r\n");
	if (body_in)
		add(&req, body_in, body_len);
	if (conn_write(&c, req.body, req.len)) {
		set_error(r, "write failed");
		goto out;
	}
	if (read_all(&c, &raw, 256 << 20) || !raw.len) {
		set_error(r, "no response");
		goto out;
	}
	if (sscanf(raw.body, "HTTP/%*s %d", &r->status) != 1) {
		set_error(r, "not HTTP");
		goto out;
	}
	hend = strstr(raw.body, "\r\n\r\n");
	body = hend ? hend + 4 : raw.body + raw.len;
	keep_headers(raw.body, hend ? hend + 2 : raw.body + raw.len, url,
		rq->first_party, r);
	if ((v = header(raw.body, "Location")) && r->status >= 300 &&
	    r->status < 400) {
		*location = strndup(v, header_len(v));
		ret = 0;
		goto out;
	}
	if ((v = header(raw.body, "Content-Type"))) {
		size_t n = header_len(v), k;
		const char *cs;

		k = strcspn(v, ";\r\n");
		if (k < sizeof(r->type)) {
			memcpy(r->type, v, k);
			r->type[k] = '\0';
		}
		cs = strstr(v, "charset=");
		if (cs && cs < v + n) {
			cs += 8;
			k = strcspn(cs, "; \"\r\n");
			if (*cs == '"')
				cs++, k = strcspn(cs, "\"\r\n");
			if (k < sizeof(r->charset)) {
				memcpy(r->charset, cs, k);
				r->charset[k] = '\0';
			}
		}
	}
	v = header(raw.body, "Transfer-Encoding");
	if (v && !strncasecmp(v, "chunked", 7))
		dechunk(r, body, (size_t)(raw.body + raw.len - body));
	else
		add(r, body, (size_t)(raw.body + raw.len - body));
	v = header(raw.body, "Content-Encoding");
	if (v && (!strncasecmp(v, "gzip", 4) || !strncasecmp(v, "deflate", 7)))
		inflate_body(r);
	ret = 0;
out:
	conn_close(&c);
	free(raw.body);
	free(req.body);
	free(u.path);
	return ret;
}

int net_fetch(const soie_request *rq, struct response *r)
{
	const char *url = rq->url, *method, *body = rq->body;
	char *cur, *loc;
	soie_response er;
	int i;

	memset(r, 0, sizeof(*r));
	if (soie_ext_blocked(rq)) {
		set_error(r, "blocked");
		return -1;
	}
	if (!soie_ext_fetch(rq, &er)) {
		char *t = soie_response_header(&er, "content-type");

		r->status = er.status;
		r->body = er.body;
		r->len = r->cap = er.len;
		r->headers = er.headers;
		snprintf(r->url, sizeof(r->url), "%s", er.url ? er.url : url);
		if (t) {
			snprintf(r->type, sizeof(r->type), "%.*s",
				(int)strcspn(t, ";"), t);
			free(t);
		}
		free(er.url);
		return 0;
	}
	if (!strncmp(url, "file://", 7)) {
		snprintf(r->url, sizeof(r->url), "%s", url);
		return fetch_file(url, r);
	}
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
		set_error(r, "unsupported URL scheme");
		return -1;
	}
	method = rq->method ? rq->method : body ? "POST" : "GET";
	cur = strdup(url);
	if (!cur)
		abort();
	for (i = 0; i < 10; i++) {
		if (request(cur, rq, method, body, rq->body_len, r, &loc)) {
			free(cur);
			return -1;
		}
		if (!loc)
			break;
		{
			char *next = soie_url_resolve(cur, loc);

			free(loc);
			free(cur);
			cur = next;
		}
		/* 303 and the old 301/302 habits: the next one is a GET */
		if (r->status != 307 && r->status != 308) {
			body = NULL;
			method = "GET";
		}
		free(r->body);
		r->body = NULL;
		r->len = r->cap = 0;
	}
	snprintf(r->url, sizeof(r->url), "%s", cur);
	free(cur);
	return 0;
}

int net_request(void *data, const soie_request *req, soie_response *res)
{
	struct response r;

	(void)data;
	memset(res, 0, sizeof(*res));
	if (net_fetch(req, &r)) {
		response_free(&r);
		return -1;
	}
	res->status = r.status;
	res->url = strdup(r.url);
	res->headers = r.headers ? r.headers : strdup("");
	res->body = r.body ? r.body : malloc(1);
	res->len = r.len;
	if (!res->url || !res->headers || !res->body)
		abort();
	if (!r.headers && *r.type) {
		/* file:// and extension responses: give the type */
		free(res->headers);
		res->headers = malloc(strlen(r.type) + 20);
		if (!res->headers)
			abort();
		sprintf(res->headers, "content-type: %s\r\n", r.type);
	}
	return 0;
}

void response_free(struct response *r)
{
	free(r->body);
	free(r->headers);
	r->body = NULL;
	r->headers = NULL;
}
