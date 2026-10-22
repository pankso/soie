/*
 * Run html5lib tree-construction tests (.dat files) against libsoie.
 * Usage: html5lib [-v] [-x KNOWN] FILE.dat...
 * KNOWN lists expected failures, one "file.dat number" per line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <soie/soie.h>

static int verbose;
static char *known;		/* contents of the -x file */

static int is_known(const char *file, int num)
{
	const char *base = strrchr(file, '/'), *p;
	char key[256];
	size_t n;

	base = base ? base + 1 : file;
	n = (size_t)snprintf(key, sizeof(key), "%s %d\n", base, num);
	for (p = known; p && (p = strstr(p, key)); p += n)
		if (p == known || p[-1] == '\n')
			return 1;
	return 0;
}

static char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (!f) {
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		perror(path);
		exit(1);
	}
	buf[n] = '\0';
	fclose(f);
	*len = (size_t)n;
	return buf;
}

/* Test files contain NUL bytes: no strstr(). */
static char *find(char *s, char *end, const char *pat)
{
	size_t n = strlen(pat);

	for (; s + n <= end; s++)
		if (*s == *pat && !memcmp(s, pat, n))
			return s;
	return NULL;
}

/* Find the line "#name" in [s, end); returns the start of its content. */
static char *section(char *s, char *end, const char *name)
{
	size_t n = strlen(name);
	char *p = s;

	if (s + n < end && !memcmp(s, name, n) && s[n] == '\n')
		return s + n + 1;
	while ((p = find(p, end, name))) {
		if (p > s && p[-1] == '\n' && p + n < end && p[n] == '\n')
			return p + n + 1;
		p += n;
	}
	return NULL;
}

/* Section content runs to the next line starting with '#'. */
static size_t section_len(const char *s, const char *end)
{
	const char *p = s;

	while (p < end) {
		if (*p == '#' && (p == s || p[-1] == '\n'))
			break;
		p = memchr(p, '\n', (size_t)(end - p));
		if (!p)
			return (size_t)(end - s);
		p++;
	}
	return (size_t)(p - s);
}

static soie_ns context_ns(const char **name)
{
	if (!strncmp(*name, "svg ", 4)) {
		*name += 4;
		return SOIE_NS_SVG;
	}
	if (!strncmp(*name, "math ", 5)) {
		*name += 5;
		return SOIE_NS_MATHML;
	}
	return SOIE_NS_HTML;
}

/* One test in [s, end) with a scripting flag; 1 on success, -1 skipped. */
static int run_one(const char *file, int num, char *s, char *end,
	int scripting)
{
	soie_html_options opts = { NULL, 0 };
	char *data, *doc_s, *frag, *got;
	size_t data_len, doc_len, got_len;
	soie_node *doc, *root;
	int ok;

	data = section(s, end, "#data");
	doc_s = section(s, end, "#document");
	if (!data || !doc_s)
		return -1;
	{
		char *e = find(data - 1, end, "\n#errors\n");

		if (!e)
			return -1;
		data_len = e > data ? (size_t)(e - data) : 0;
	}
	doc_len = (size_t)(end - doc_s);
	while (doc_len && doc_s[doc_len - 1] == '\n')
		doc_len--;
	opts.scripting = scripting;
	opts.charset = "UTF-8";
	frag = section(s, end, "#document-fragment");
	if (frag) {
		size_t n = section_len(frag, end);
		char *name = malloc(n + 1);
		const char *ctx = name;
		soie_ns ns;

		memcpy(name, frag, n);
		while (n && (name[n - 1] == '\n' || name[n - 1] == ' '))
			n--;
		name[n] = '\0';
		ns = context_ns(&ctx);
		doc = soie_document_new();
		root = soie_html_parse_fragment(doc, ns, ctx, data, data_len,
			&opts);
		free(name);
	} else {
		doc = soie_html_parse(data, data_len, &opts);
		root = doc;
	}
	{
		char *props = section(s, end, "#properties");

		if (props) {
			size_t n = section_len(props, end);
			char *list = malloc(n + 1);

			memcpy(list, props, n);
			while (n && (list[n - 1] == '\n' || list[n - 1] == ' '))
				n--;
			list[n] = '\0';
			soie_style_document(doc, NULL);
			got = soie_node_dump_style(root, list, &got_len);
			free(list);
		} else {
			got = soie_node_dump(root, &got_len);
		}
	}
	{
		size_t n = got_len;

		while (n && got[n - 1] == '\n')
			got[--n] = '\0';
		ok = n == doc_len && !memcmp(got, doc_s, n);
	}
	if (!ok && verbose)
		printf("FAIL %s #%d%s\n--- input\n%.*s\n--- expected\n%.*s\n"
		       "--- got\n%.*s\n\n", file, num,
		       scripting ? " (scripting)" : "", (int)data_len, data,
		       (int)doc_len, doc_s, (int)got_len, got);
	free(got);
	if (root != doc)
		soie_node_free(root);
	soie_node_free(doc);
	return ok;
}

/* Tests without #script-on or #script-off run in both modes. */
static int run_test(const char *file, int num, char *s, char *end)
{
	int r;

	if (section(s, end, "#script-on"))
		return run_one(file, num, s, end, 1);
	if (section(s, end, "#script-off"))
		return run_one(file, num, s, end, 0);
	r = run_one(file, num, s, end, 0);
	if (r != 1)
		return r;
	return run_one(file, num, s, end, 1);
}

int main(int argc, char **argv)
{
	int i, pass = 0, fail = 0, skip = 0, xfail = 0;

	for (i = 1; i < argc; i++) {
		char *buf, *s, *next, *end;
		size_t len;
		int num = 0, fp = 0, ff = 0, r;

		if (!strcmp(argv[i], "-v")) {
			verbose = 1;
			continue;
		}
		if (!strcmp(argv[i], "-x") && i + 1 < argc) {
			known = read_file(argv[++i], &len);
			continue;
		}
		buf = read_file(argv[i], &len);
		end = buf + len;
		s = strncmp(buf, "#data\n", 6) ? find(buf, end, "\n#data\n") :
			buf - 1;
		if (s)
			s++;
		while (s && s < end) {
			next = find(s + 1, end, "\n#data\n");
			r = run_test(argv[i], ++num, s, next ? next + 1 : end);
			if (r < 0) {
				skip++;
			} else if (is_known(argv[i], num)) {
				if (r)
					printf("known failure passes: %s %d\n",
						argv[i], num);
				xfail++;
			} else if (r) {
				fp++;
			} else {
				ff++;
			}
			s = next ? next + 1 : NULL;
		}
		printf("%-40s %4d passed %4d failed\n", argv[i], fp, ff);
		pass += fp;
		fail += ff;
		free(buf);
	}
	printf("total: %d passed, %d failed, %d known failures, %d skipped\n",
		pass, fail, xfail, skip);
	free(known);
	return fail != 0;
}
