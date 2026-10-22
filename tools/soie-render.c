/*
 * soie-render: render a page with libsoie, from the command line.
 * For now it parses and dumps the DOM tree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <soie/soie.h>

static void usage(void)
{
	fputs("Usage: soie-render [options] FILE|-\n"
	      "  -d          dump the DOM tree (default)\n"
	      "  -c CHARSET  charset from the transport, skips sniffing\n"
	      "  -f CONTEXT  parse as a fragment inside element CONTEXT\n"
	      "  -s          scripting flag on (<noscript> as raw text)\n"
	      "  -V          print the library version\n", stderr);
	exit(2);
}

static char *read_all(const char *path, size_t *len)
{
	FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
	char *buf = NULL;
	size_t cap = 0, n;

	if (!f) {
		perror(path);
		exit(1);
	}
	*len = 0;
	do {
		if (*len + 65536 > cap) {
			cap = cap ? cap * 2 : 65536;
			buf = realloc(buf, cap);
			if (!buf) {
				perror("realloc");
				exit(1);
			}
		}
		n = fread(buf + *len, 1, cap - *len, f);
		*len += n;
	} while (n);
	if (ferror(f)) {
		perror(path);
		exit(1);
	}
	if (f != stdin)
		fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	soie_html_options opts = { NULL, 0 };
	const char *context = NULL;
	soie_node *doc, *root;
	char *buf, *dump;
	size_t len;
	int i;

	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-d"))
			;
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			opts.charset = argv[++i];
		else if (!strcmp(argv[i], "-f") && i + 1 < argc)
			context = argv[++i];
		else if (!strcmp(argv[i], "-s"))
			opts.scripting = 1;
		else if (!strcmp(argv[i], "-V")) {
			printf("soie %s\n", soie_version());
			return 0;
		} else
			usage();
	}
	if (i != argc - 1)
		usage();
	buf = read_all(argv[i], &len);
	if (context) {
		doc = soie_document_new();
		root = soie_html_parse_fragment(doc, SOIE_NS_HTML, context,
			buf, len, &opts);
	} else {
		doc = soie_html_parse(buf, len, &opts);
		root = doc;
	}
	dump = soie_node_dump(root, &len);
	fwrite(dump, 1, len, stdout);
	free(dump);
	if (root != doc)
		soie_node_free(root);
	soie_node_free(doc);
	free(buf);
	return 0;
}
