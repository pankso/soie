/*
 * Driver for the value tests of css-parsing-tests (An+B, colors).
 * Same input protocol as csstest.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "css.h"

static void run(const char *fn, const char *s, size_t len)
{
	struct cvlist l;
	struct cur c;
	int a, b;

	css_component_values(s, len, &l);
	cur_init(&c, &l);
	if (!strcmp(fn, "An+B")) {
		if (parse_anb(&c, &a, &b) && cur_end(&c))
			printf("[%d, %d]\n", a, b);
		else
			printf("null\n");
	} else {
		struct color col;
		struct buf o = { 0 };

		if (parse_color(&c, &col) && cur_end(&c)) {
			serialize_color(&o, &col);
			printf("\"%s\"\n", o.s);
		} else {
			printf("null\n");
		}
		buf_free(&o);
	}
	cvlist_free(&l);
}

int main(int argc, char **argv)
{
	size_t len;
	char *buf;

	if (argc != 2)
		return 2;
	while (scanf("%zu", &len) == 1) {
		getchar();
		buf = malloc(len + 1);
		if (fread(buf, 1, len, stdin) != len)
			return 1;
		run(argv[1], buf, len);
		free(buf);
		fflush(stdout);
	}
	return 0;
}
