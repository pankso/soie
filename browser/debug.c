/*
 * The browser's debugging (debug.h). The crash handler is best effort:
 * once memory is broken, only the backtrace is sure to come out, so it
 * writes that first and asks gdb for the full one (with line numbers,
 * every thread) after.
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <soie/soie.h>
#include "debug.h"

int debug_level;

#define TRAIL 64
static char trail[TRAIL][200];
static int ntrail;
static double t0;
static char page_url[1024];
static char crash_path[600];
static char gdb[] = "/usr/bin/gdb";
static int have_gdb;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void debug(int level, const char *fmt, ...)
{
	char *line = trail[ntrail++ % TRAIL];
	int n;
	va_list ap;

	n = snprintf(line, sizeof(trail[0]), "%8.3f ", now() - t0);
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(trail[0]) - (size_t)n, fmt, ap);
	va_end(ap);
	if (level <= debug_level)
		fprintf(stderr, "soie: %s\n", line);
}

void debug_page(const char *url)
{
	snprintf(page_url, sizeof(page_url), "%s", url);
}

void debug_trail(int fd)
{
	int i = ntrail > TRAIL ? ntrail - TRAIL : 0;

	for (; i < ntrail; i++) {
		const char *l = trail[i % TRAIL];

		if (write(fd, l, strlen(l)) < 0 || write(fd, "\n", 1) < 0)
			return;
	}
}

const char *debug_dir(void)
{
	static char dir[512];
	const char *c = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");

	if (*dir)
		return dir;
	if (c && *c) {
		snprintf(dir, sizeof(dir), "%s", c);
	} else if (h && *h) {
		snprintf(dir, sizeof(dir), "%s/.cache", h);
	} else {
		return NULL;
	}
	mkdir(dir, 0700);
	strncat(dir, "/soie", sizeof(dir) - strlen(dir) - 1);
	if (mkdir(dir, 0700) && access(dir, W_OK)) {
		*dir = '\0';
		return NULL;
	}
	return dir;
}

static void put(int fd, const char *s)
{
	if (fd >= 0 && write(fd, s, strlen(s)) < 0)
		return;
}

static void crashed(int sig)
{
	void *frames[64];
	char buf[1400];
	int n, fd, i;
	pid_t pid;

	fd = *crash_path ? open(crash_path, O_WRONLY | O_CREAT | O_TRUNC,
		0600) : -1;
	snprintf(buf, sizeof(buf), "\nsoie %s crashed: signal %d (%s)\n"
		"page: %s\n", soie_version(), sig, sig == SIGSEGV ? "SIGSEGV" :
		sig == SIGBUS ? "SIGBUS" : sig == SIGFPE ? "SIGFPE" :
		sig == SIGILL ? "SIGILL" : sig == SIGABRT ? "SIGABRT" : "?",
		page_url);
	put(2, buf);
	put(fd, buf);
	put(fd, "\nlast events:\n");
	debug_trail(fd);
	n = backtrace(frames, 64);
	put(fd, "\nbacktrace:\n");
	if (fd >= 0)
		backtrace_symbols_fd(frames, n, fd);
	if (have_gdb && fd >= 0) {
		/* the whole story, from outside: gdb attaches to us */
		put(fd, "\ngdb:\n");
		snprintf(buf, sizeof(buf), "%d", (int)getpid());
		if ((pid = fork()) == 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			execl(gdb, "gdb", "-nx", "-batch", "-p", buf,
				"-ex", "info threads", "-ex", "thread apply all bt",
				(char *)NULL);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, &i, 0);
	}
	if (fd >= 0) {
		close(fd);
		snprintf(buf, sizeof(buf), "crash report: %s\n", crash_path);
		put(2, buf);
	} else {
		backtrace_symbols_fd(frames, n, 2);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

void debug_init(void)
{
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL,
		SIGABRT };
	static char altstack[65536];
	const char *e = getenv("SOIE_DEBUG"), *dir;
	struct sigaction sa;
	stack_t ss;
	void *f[2];
	size_t i;

	t0 = now();
	debug_level = e ? atoi(e) : 0;
	if (e && !debug_level)
		debug_level = 1;
	if ((dir = debug_dir()))
		snprintf(crash_path, sizeof(crash_path), "%s/crash-%d.txt", dir,
			(int)getpid());
	have_gdb = !getenv("SOIE_NO_GDB") && !access(gdb, X_OK);
#if defined(__linux__) && defined(PR_SET_PTRACER)
	/* under Yama, gdb may attach to us */
	prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
	backtrace(f, 2);	/* loads libgcc now, not in the handler */
	/* a stack of its own: a stack overflow is a crash too */
	ss.ss_sp = altstack;
	ss.ss_size = sizeof(altstack);
	ss.ss_flags = 0;
	sigaltstack(&ss, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = crashed;
	sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	for (i = 0; i < sizeof(sigs) / sizeof(*sigs); i++)
		sigaction(sigs[i], &sa, NULL);
}
