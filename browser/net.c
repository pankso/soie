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
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <gnutls/gnutls.h>
#include <zlib.h>
#include "net.h"

#define TIMEOUT_MS 20000
#define MAX_BODY (256u << 20)	/* response bodies, files */
#define MAX_INFLATED (64u << 20)	/* what gzip may grow into */

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
	if (!n || n >= sizeof(p->host) ||
	    strspn(h, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"0123456789.-_") < n)
		return -1;	/* only a host name goes on the wire */
	memcpy(p->host, h, n);
	if (colon && (size_t)(e - colon - 1) < sizeof(p->port) &&
	    e - colon > 1 &&
	    strspn(colon + 1, "0123456789") == (size_t)(e - colon - 1))
		memcpy(p->port, colon + 1, (size_t)(e - colon - 1));
	else if (colon)
		return -1;
	else
		strcpy(p->port, !strcmp(p->scheme, "https") ? "443" : "80");
	/* the request target: controls, spaces and 8-bit bytes encoded,
	 * so that nothing typed or pasted can end the request line */
	n = strcspn(e, "#");
	p->path = malloc(n * 3 + 2);
	if (!p->path)
		return -1;
	{
		char *o = p->path;
		size_t i;

		if (*e != '/')
			*o++ = '/';
		for (i = 0; i < n; i++) {
			unsigned char c = (unsigned char)e[i];

			if (c <= 0x20 || c >= 0x7f) {
				sprintf(o, "%%%02X", c);
				o += 3;
			} else {
				*o++ = (char)c;
			}
		}
		*o = '\0';
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

/*
 * Where a chunked body ends (just past its last CRLF), or 0 while it is
 * incomplete; *pos keeps the parsing between calls.
 */
static size_t chunked_end(const char *s, size_t n, size_t *pos)
{
	size_t p = *pos, size;
	const char *e;
	char *num_end;

	for (;;) {
		e = memchr(s + p, '\n', n - p);
		if (!e)
			return 0;
		size = strtoul(s + p, &num_end, 16);
		if (num_end == s + p)
			return (size_t)-1;	/* not a chunk size */
		if (!size) {
			/* trailers, then an empty line */
			p = (size_t)(e - s) + 1;
			for (;;) {
				e = memchr(s + p, '\n', n - p);
				if (!e)
					return 0;
				if (e == s + p || (e == s + p + 1 &&
				    s[p] == '\r'))
					return (size_t)(e - s) + 1;
				p = (size_t)(e - s) + 1;
			}
		}
		p = (size_t)(e - s) + 1;
		if (size > MAX_BODY)
			return (size_t)-1;	/* no chunk is that big */
		if (n - p < size + 2)
			return 0;
		if (s[p + size] != '\r' || s[p + size + 1] != '\n')
			return (size_t)-1;	/* data not followed by CRLF */
		p += size + 2;
		*pos = p;
	}
}

/* A token in a header's comma separated list ("close", "chunked") */
static int has_token(const char *v, size_t len, const char *tok)
{
	size_t n = strlen(tok), i = 0, k;

	while (i < len) {
		while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ','))
			i++;
		for (k = i; k < len && v[k] != ','; k++)
			;
		while (k > i && (v[k - 1] == ' ' || v[k - 1] == '\t'))
			k--;
		if (k - i == n && !strncasecmp(v + i, tok, n))
			return 1;
		while (i < len && v[i] != ',')
			i++;
	}
	return 0;
}

/* The last token of a list: Transfer-Encoding ends with chunked */
static int last_token(const char *v, size_t len, const char *tok)
{
	size_t n = strlen(tok), k = len;

	while (k && (v[k - 1] == ' ' || v[k - 1] == '\t'))
		k--;
	return k >= n && !strncasecmp(v + k - n, tok, n) &&
		(k == n || v[k - n - 1] == ',' || v[k - n - 1] == ' ' ||
		 v[k - n - 1] == '\t');
}

/*
 * Read one response, headers and raw body, framed as HTTP/1.1 says:
 * chunked, Content-Length, nothing (HEAD, 204, 304), or until the peer
 * closes. *reuse: the connection can carry another request, only when
 * the frame is sure and nothing past it was read. Headers are looked
 * up in the header block only, never in the body.
 */
static int read_response(struct conn *c, struct response *raw,
	const char *method, int *reuse)
{
	char buf[16384], *hb = NULL;
	const char *v;
	size_t head = 0, want = 0, pos = 0, end, n;
	ssize_t k;
	int status = 0, mode = 0, have = 0;	/* 0 close, 1 length, 2 chunked */

	*reuse = 0;
	for (;;) {
		if (!have && raw->len && (v = strstr(raw->body, "\r\n\r\n"))) {
			head = (size_t)(v - raw->body) + 4;
			if (sscanf(raw->body, "HTTP/%*s %d", &status) == 1 &&
			    status >= 100 && status < 200) {
				/* 100 Continue and friends: skip */
				memmove(raw->body, raw->body + head,
					raw->len - head + 1);
				raw->len -= head;
				continue;
			}
			have = 1;
			hb = strndup(raw->body, head);
			if (!hb)
				abort();
			*reuse = !strncmp(hb, "HTTP/1.1", 8);
			if ((v = header(hb, "Connection"))) {
				n = header_len(v);
				if (has_token(v, n, "close"))
					*reuse = 0;
				else if (has_token(v, n, "keep-alive"))
					*reuse = 1;
			}
			if (!strcmp(method, "HEAD") || status == 204 ||
			    status == 304) {
				mode = 1;
				want = 0;
			} else if ((v = header(hb, "Transfer-Encoding"))) {
				/* chunked last, or the body ends with the
				 * connection; with a Content-Length too, the
				 * connection is not kept (RFC 9112 6.1) */
				mode = last_token(v, header_len(v), "chunked") ?
					2 : 0;
				if (!mode || header(hb, "Content-Length"))
					*reuse = 0;
			} else if ((v = header(hb, "Content-Length"))) {
				const char *l, *w;

				n = header_len(v);
				mode = n && n < 10 &&
					strspn(v, "0123456789") == n;
				want = mode ? strtoul(v, NULL, 10) : 0;
				if (want > MAX_BODY)
					mode = 0;
				/* every Content-Length must say the same */
				for (l = strchr(hb, '\n'); l && mode;
				     l = strchr(l + 1, '\n')) {
					if (strncasecmp(l + 1, "Content-Length:",
					    15))
						continue;
					for (w = l + 16; *w == ' ' || *w == '\t';
					     w++)
						;
					if (header_len(w) != n ||
					    strncmp(w, v, n))
						mode = 0;
				}
				if (!mode)
					*reuse = 0;
			} else {
				mode = 0;
				*reuse = 0;
			}
		}
		if (have && mode == 1 && raw->len - head >= want) {
			if (raw->len - head > want)
				*reuse = 0;	/* more than ours: drop it */
			raw->len = head + want;
			raw->body[raw->len] = '\0';
			free(hb);
			return 0;
		}
		if (have && mode == 2) {
			end = chunked_end(raw->body + head, raw->len - head,
				&pos);
			if (end == (size_t)-1) {
				*reuse = 0;
				free(hb);
				return 0;
			}
			if (end) {
				if (head + end < raw->len)
					*reuse = 0;
				raw->len = head + end;
				raw->body[raw->len] = '\0';
				free(hb);
				return 0;
			}
		}
		k = conn_read(c, buf, sizeof(buf));
		if (k <= 0) {
			/* the peer closed: fine when that frames the body */
			*reuse = 0;
			free(hb);
			return raw->len && have ? 0 : -1;
		}
		add(raw, buf, (size_t)k);
		if (raw->len > MAX_BODY) {
			*reuse = 0;
			free(hb);
			return -1;
		}
	}
}

/* Idle connections, kept for the next request to the same server */
#define POOL 16
#define POOL_IDLE 30		/* seconds */

static struct {
	char key[300];
	struct conn c;
	time_t since;
} pool[POOL];
static int npool;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

static void conn_close(struct conn *c);

/* A pooled connection to key, still open, or -1. */
static int pool_take(const char *key, struct conn *c)
{
	struct pollfd p;
	time_t now = time(NULL);
	int i, found = -1;

	pthread_mutex_lock(&pool_lock);
	for (i = npool - 1; i >= 0; i--) {
		if (now - pool[i].since > POOL_IDLE) {
			conn_close(&pool[i].c);
			pool[i] = pool[--npool];
			continue;
		}
		if (found < 0 && !strcmp(pool[i].key, key))
			found = i;
	}
	if (found >= 0) {
		*c = pool[found].c;
		pool[found] = pool[--npool];
	}
	pthread_mutex_unlock(&pool_lock);
	if (found < 0)
		return -1;
	/* readable while idle: closed by the server, or garbage */
	p.fd = c->fd;
	p.events = POLLIN;
	if (poll(&p, 1, 0) != 0) {
		conn_close(c);
		return -1;
	}
	return 0;
}

static void pool_put(const char *key, struct conn *c)
{
	int i, old = 0;

	pthread_mutex_lock(&pool_lock);
	if (npool == POOL) {
		for (i = 1; i < npool; i++)
			if (pool[i].since < pool[old].since)
				old = i;
		conn_close(&pool[old].c);
		pool[old] = pool[--npool];
	}
	snprintf(pool[npool].key, sizeof(pool[npool].key), "%s", key);
	pool[npool].c = *c;
	pool[npool].since = time(NULL);
	npool++;
	pthread_mutex_unlock(&pool_lock);
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
		if (out.len > MAX_INFLATED) {
			/* a compression bomb */
			inflateEnd(&z);
			free(out.body);
			return -1;
		}
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
	struct stat st;
	char buf[16384];
	size_t k;

	if (!f) {
		set_error(r, strerror(errno));
		return -1;
	}
	/* regular files only: not /dev/zero, not a FIFO */
	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) ||
	    (unsigned long long)st.st_size > MAX_BODY) {
		fclose(f);
		set_error(r, "not a readable file");
		return -1;
	}
	while ((k = fread(buf, 1, sizeof(buf), f)) > 0 && r->len < MAX_BODY)
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

/* RFC 9110 token characters: methods and header names */
static int is_token(const char *s, size_t n)
{
	static const char extra[] = "!#$%&'*+-.^_`|~";
	size_t i;

	if (!n)
		return 0;
	for (i = 0; i < n; i++)
		if (!(isalnum((unsigned char)s[i]) || (s[i] && strchr(extra,
		    s[i]))))
			return 0;
	return 1;
}

/*
 * The caller's headers (a page's, through scripts), kept only when
 * they cannot change how the request is framed or routed: names are
 * tokens, values have no control bytes, and the engine's own headers
 * (Host, lengths, encodings, cookies, connection) are not theirs.
 */
static void add_headers(struct response *req, const char *h)
{
	static const char *const forbidden[] = { "host", "content-length",
		"transfer-encoding", "te", "trailer", "upgrade", "connection",
		"keep-alive", "cookie", "cookie2", "accept-encoding", "expect",
		"proxy-authorization", "proxy-connection", NULL };
	const char *e, *colon, *v;
	size_t n;
	int i, ok;

	for (; h && *h; h = *e ? e + 1 : e) {
		e = strchr(h, '\n');
		if (!e)
			e = h + strlen(h);
		colon = memchr(h, ':', (size_t)(e - h));
		if (!colon || !is_token(h, (size_t)(colon - h)))
			continue;
		n = (size_t)(colon - h);
		for (ok = 1, i = 0; forbidden[i]; i++)
			if (strlen(forbidden[i]) == n &&
			    !strncasecmp(h, forbidden[i], n))
				ok = 0;
		if (n > 6 && !strncasecmp(h, "proxy-", 6))
			ok = 0;
		for (v = colon + 1; ok && v < e; v++)
			if (((unsigned char)*v < 0x20 && *v != '\t' &&
			     !(*v == '\r' && v + 1 == e)) || *v == 0x7f)
				ok = 0;
		if (!ok)
			continue;
		add(req, h, (size_t)(e - h));
		if (e[-1] != '\r')
			adds(req, "\r");
		adds(req, "\n");
	}
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
	const char *first_party, int jar, struct response *r)
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
		if (cookie && jar) {
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
	const char *method, const char *headers, const char *body_in,
	size_t body_len, struct response *r, char **location)
{
	struct url u;
	struct conn c = { -1, NULL };
	struct response raw, req;
	const char *body, *v, *fp = rq->first_party ? rq->first_party : url;
	char *cookie, *hend, *hb = NULL, key[300];
	int ret = -1, reused, reuse = 0, tries;

	*location = NULL;
	if (!is_token(method, strlen(method))) {
		set_error(r, "bad method");
		return -1;
	}
	if (parse_url(url, &u)) {
		set_error(r, "bad URL");
		return -1;
	}
	memset(&raw, 0, sizeof(raw));
	memset(&req, 0, sizeof(req));
	snprintf(key, sizeof(key), "%s://%s:%s", u.scheme, u.host, u.port);
	{
		char line[1024];

		snprintf(line, sizeof(line), "%s ", method);
		adds(&req, line);
		adds(&req, u.path);
		snprintf(line, sizeof(line), " HTTP/1.1\r\nHost: %s\r\n"
			"User-Agent: Mozilla/5.0 (X11; Linux) Soie/0.1\r\n"
			"Accept-Encoding: gzip, deflate\r\n", u.host);
		adds(&req, line);
	}
	if (!has_header(headers, "Accept"))
		adds(&req, "Accept: text/html,application/xhtml+xml,*/*;"
			"q=0.8\r\n");
	if (!(rq->flags & SOIE_REQ_NO_COOKIES) &&
	    (cookie = soie_cookies_get(url, fp, 0))) {
		adds(&req, "Cookie: ");
		adds(&req, cookie);
		adds(&req, "\r\n");
		free(cookie);
	}
	add_headers(&req, headers);
	if (body_in) {
		char line[128];

		if (!has_header(headers, "Content-Type"))
			adds(&req, "Content-Type: application/"
				"x-www-form-urlencoded\r\n");
		snprintf(line, sizeof(line), "Content-Length: %zu\r\n",
			body_len);
		adds(&req, line);
	}
	adds(&req, "\r\n");
	if (body_in)
		add(&req, body_in, body_len);
	/* a pooled connection the server dropped gets one more try, on
	 * a new connection */
	for (tries = 0; tries < 2; tries++) {
		reused = pool_take(key, &c) == 0;
		if (!reused) {
			c.fd = dial(u.host, u.port, r);
			if (c.fd < 0)
				goto out;
			if (!strcmp(u.scheme, "https") &&
			    tls_start(&c, u.host, r))
				goto out;
		}
		raw.len = 0;
		if (!conn_write(&c, req.body, req.len) &&
		    !read_response(&c, &raw, method, &reuse) && raw.len)
			break;
		conn_close(&c);
		c.fd = -1;
		c.tls = NULL;
		if (!reused) {
			set_error(r, "no response");
			goto out;
		}
	}
	if (tries == 2) {
		set_error(r, "no response");
		goto out;
	}
	if (sscanf(raw.body, "HTTP/%*s %d", &r->status) != 1) {
		set_error(r, "not HTTP");
		goto out;
	}
	hend = strstr(raw.body, "\r\n\r\n");
	body = hend ? hend + 4 : raw.body + raw.len;
	/* the header block alone: no header is read from the body */
	hb = strndup(raw.body, (size_t)(body - raw.body));
	if (!hb)
		abort();
	keep_headers(raw.body, hend ? hend + 2 : raw.body + raw.len, url,
		rq->first_party, !(rq->flags & SOIE_REQ_NO_COOKIES), r);
	if ((v = header(hb, "Location")) && r->status >= 300 &&
	    r->status < 400) {
		*location = strndup(v, header_len(v));
		ret = 0;
		goto out;
	}
	if ((v = header(hb, "Content-Type"))) {
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
	v = header(hb, "Transfer-Encoding");
	if (v && last_token(v, header_len(v), "chunked"))
		dechunk(r, body, (size_t)(raw.body + raw.len - body));
	else
		add(r, body, (size_t)(raw.body + raw.len - body));
	v = header(hb, "Content-Encoding");
	if (v && (!strncasecmp(v, "gzip", 4) || !strncasecmp(v, "deflate", 7)) &&
	    inflate_body(r)) {
		set_error(r, "body too large");
		goto out;
	}
	ret = 0;
out:
	if (ret == 0 && reuse && c.fd >= 0) {
		pool_put(key, &c);
		c.fd = -1;
		c.tls = NULL;
	}
	conn_close(&c);
	free(raw.body);
	free(req.body);
	free(u.path);
	free(hb);
	return ret;
}

/* scheme://host:port of two URLs */
static int same_origin(const char *a, const char *b)
{
	struct url x, y;
	int same;

	if (parse_url(a, &x))
		return 0;
	if (parse_url(b, &y)) {
		free(x.path);
		return 0;
	}
	same = !strcasecmp(x.scheme, y.scheme) && !strcasecmp(x.host, y.host) &&
		!strcmp(x.port, y.port);
	free(x.path);
	free(y.path);
	return same;
}

int net_fetch(const soie_request *rq, struct response *r)
{
	const char *url = rq->url, *method, *body = rq->body;
	const char *headers = rq->headers;
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
		/* local files: for what the user opens, and what local
		 * pages load; never for a web page */
		if (rq->first_party && strncmp(rq->first_party, "file://", 7)) {
			set_error(r, "file:// from a web page");
			return -1;
		}
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
		if (request(cur, rq, method, headers, body, rq->body_len, r,
			    &loc)) {
			free(cur);
			return -1;
		}
		if (!loc)
			break;
		{
			char *next = soie_url_resolve(cur, loc);

			free(loc);
			if (!next || (strncmp(next, "http://", 7) &&
			    strncmp(next, "https://", 8))) {
				/* only to the web: no file://, no others */
				free(next);
				free(cur);
				set_error(r, "bad redirection");
				return -1;
			}
			/* another origin gets none of the caller's headers
			 * (Authorization...) */
			if (!same_origin(cur, next))
				headers = NULL;
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
