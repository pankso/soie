/*
 * The browser's networking: HTTP(S) and file:// fetches.
 */
#ifndef SOIE_NET_H
#define SOIE_NET_H

#include <stddef.h>
#include <soie/soie.h>
#include <soie/ext.h>

struct response {
	int status;
	char *body;
	size_t len, cap;
	char type[128];		/* Content-Type without parameters */
	char charset[64];
	char url[4096];		/* after redirects */
	char *headers;		/* "name: value\r\n", names lower case */
	char error[256];
};

/* A request through the extensions (blocking, cookies, schemes) and
 * the network. A body without a Content-Type header is sent as a
 * urlencoded form. */
int net_fetch(const soie_request *req, struct response *r);
/* The same as an extension request function (data unused). */
int net_request(void *data, const soie_request *req, soie_response *res);
void response_free(struct response *r);

#endif
