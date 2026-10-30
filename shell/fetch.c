/*
 * Background fetches. Workers take urgent jobs (the page, its style
 * sheets and scripts) before the others (images); each finished job
 * goes to the done list and a byte down the pipe wakes the main loop.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fetch.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static struct job *urgent, *normal, *done;	/* FIFO, FIFO, any order */

#define K_AHEAD (-1)	/* a connection opened ahead, no fetch; flags: how many */
static int pipefd[2] = { -1, -1 };

static void append(struct job **list, struct job *j)
{
	while (*list)
		list = &(*list)->next;
	j->next = NULL;
	*list = j;
}

static void *worker(void *arg)
{
	soie_request rq;
	struct job *j;
	char c = 1;

	(void)arg;
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!urgent && !normal)
			pthread_cond_wait(&cond, &lock);
		if (urgent) {
			j = urgent;
			urgent = j->next;
		} else {
			j = normal;
			normal = j->next;
		}
		pthread_mutex_unlock(&lock);
		if (j->kind == K_AHEAD) {
			net_preconnect(j->url, j->flags);
			job_free(j);
			continue;
		}
		memset(&rq, 0, sizeof(rq));
		rq.url = j->url;
		rq.body = j->post;
		rq.body_len = j->post ? strlen(j->post) : 0;
		rq.first_party = j->first_party;
		rq.flags = j->flags;
		j->ok = net_fetch(&rq, &j->r) == 0;
		pthread_mutex_lock(&lock);
		append(&done, j);
		pthread_mutex_unlock(&lock);
		/* a full pipe already has wake-ups waiting */
		(void)!write(pipefd[1], &c, 1);
	}
	return NULL;
}

int fetch_start(int threads)
{
	pthread_t t;
	int i;

	if (pipe(pipefd))
		return -1;
	fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
	fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
	for (i = 0; i < threads; i++)
		if (pthread_create(&t, NULL, worker, NULL) == 0)
			pthread_detach(t);
	return pipefd[0];
}

static char *dup_or_null(const char *s)
{
	char *d;

	if (!s)
		return NULL;
	d = strdup(s);
	if (!d)
		abort();
	return d;
}

struct job *fetch_add(const char *url, const char *post,
	const char *first_party, int kind, unsigned gen, int is_urgent,
	int flags, struct download *dl)
{
	struct job *j = calloc(1, sizeof(*j));

	if (!j)
		abort();
	j->url = dup_or_null(url);
	j->post = dup_or_null(post);
	j->first_party = dup_or_null(first_party);
	j->kind = kind;
	j->flags = flags;
	j->gen = gen;
	j->r.dl = dl;
	pthread_mutex_lock(&lock);
	append(is_urgent ? &urgent : &normal, j);
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);
	return j;
}

struct job *fetch_done(void)
{
	struct job *j;
	char buf[64];

	while (read(pipefd[0], buf, sizeof(buf)) > 0)
		;
	pthread_mutex_lock(&lock);
	j = done;
	if (j)
		done = j->next;
	pthread_mutex_unlock(&lock);
	return j;
}

void fetch_preconnect(const char *url, int n, unsigned gen)
{
	int i;

	for (i = 0; i < n; i++)
		fetch_add(url, NULL, NULL, K_AHEAD, gen, 1, n, NULL);
}

/* A page left: its jobs not started yet go (downloads stay), the next
 * page's come first */
void fetch_drop(unsigned gen)
{
	struct job **lists[2] = { &urgent, &normal }, **p, *j, *gone = NULL;
	int i;

	pthread_mutex_lock(&lock);
	for (i = 0; i < 2; i++)
		for (p = lists[i]; (j = *p); ) {
			if (j->gen == gen && !j->r.dl) {
				*p = j->next;
				j->next = gone;
				gone = j;
			} else {
				p = &j->next;
			}
		}
	pthread_mutex_unlock(&lock);
	while ((j = gone)) {
		gone = j->next;
		job_free(j);
	}
}

void job_free(struct job *j)
{
	if (!j)
		return;
	if (j->r.dl) {
		/* a file nobody took (path emptied when taken) */
		if (j->r.dl->state == DL_DONE && *j->r.dl->path)
			unlink(j->r.dl->path);
		free(j->r.dl->headers);
		free(j->r.dl);
	}
	response_free(&j->r);
	free(j->url);
	free(j->post);
	free(j->first_party);
	free(j);
}
