/*
 * The browser's networking: HTTP(S) and file:// fetches.
 */
#ifndef SOIE_NET_H
#define SOIE_NET_H

#include <stddef.h>
#include <soie/soie.h>

struct response {
	int status;
	char *body;
	size_t len, cap;
	char type[128];		/* Content-Type without parameters */
	char charset[64];
	char url[4096];		/* after redirects */
	char error[256];
};

/* GET, or POST when post is not NULL (form-urlencoded body). */
int net_fetch(const char *url, const char *post, struct response *r);
void response_free(struct response *r);

#endif
