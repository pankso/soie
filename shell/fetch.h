/*
 * Background fetches: a few threads run net_fetch() for queued jobs,
 * the main thread collects the finished ones.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SOIE_FETCH_H
#define SOIE_FETCH_H

#include "net.h"

struct job {
	char *url, *post, *first_party;
	int kind;		/* the host's: what the bytes are for */
	unsigned gen;		/* the host's: which page asked */
	int ok;			/* net_fetch() succeeded */
	struct response r;
	struct job *next;
};

/* Start the threads; returns a descriptor that becomes readable when
 * jobs finish, -1 on failure. */
int fetch_start(int threads);
/* Queue a request; urgent ones go before the others. */
void fetch_add(const char *url, const char *post, const char *first_party,
	int kind, unsigned gen, int urgent);
/* A finished job, or NULL when there is none yet. */
struct job *fetch_done(void);
void job_free(struct job *j);
/* Jobs queued or running. */
int fetch_busy(void);

#endif
