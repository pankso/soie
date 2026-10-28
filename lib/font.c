/*
 * Fonts: system font discovery with FreeType (no fontconfig), CSS font
 * matching (CSS Fonts 4, simplified), per-character fallback, web
 * fonts from @font-face, and text shaping for measurement and drawing.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "layout.h"
#include "font.h"

static FT_Library ft;
static int ft_ready;

struct face *faces;		/* system and web faces */
const void *font_doc;		/* document whose web fonts are visible */

float doc_zoom(void)
{
	const soie_node *d = font_doc;

	return d && d->type == SOIE_DOCUMENT && d->u.doc->zoom > 0 ?
		d->u.doc->zoom : 1;
}
int nfaces;
static int scanned;
static int stub_mode;

void soie_fonts_stub(int on)
{
	stub_mode = on;
}

int fonts_stub(void)
{
	if (stub_mode)
		return 1;
	if (!scanned)
		fonts_scan(NULL);
	return nfaces == 0;
}

static int lib_init(void)
{
	if (!ft_ready && FT_Init_FreeType(&ft) == 0)
		ft_ready = 1;
	return ft_ready;
}

static char *lowdup(const char *s)
{
	size_t n = strlen(s), i;
	char *d = xstrndup(s, n);

	for (i = 0; i < n; i++)
		if (d[i] >= 'A' && d[i] <= 'Z')
			d[i] = (char)(d[i] + 32);
	return d;
}

static int os2_weight(FT_Face f)
{
	TT_OS2 *os2 = FT_Get_Sfnt_Table(f, FT_SFNT_OS2);

	if (os2 && os2->usWeightClass >= 1 && os2->usWeightClass <= 1000)
		return os2->usWeightClass;
	return f->style_flags & FT_STYLE_FLAG_BOLD ? 700 : 400;
}

static struct face *add_face(void)
{
	faces = xrealloc(faces, (size_t)(nfaces + 1) * sizeof(*faces));
	memset(&faces[nfaces], 0, sizeof(*faces));
	return &faces[nfaces++];
}

/* Record every face of a font file. */
static void add_file(const char *path)
{
	FT_Face f;
	long i, n = 1;

	for (i = 0; i < n; i++) {
		struct face *x;

		if (FT_New_Face(ft, path, i, &f))
			return;
		n = f->num_faces;
		if (f->family_name && FT_IS_SCALABLE(f)) {
			x = add_face();
			x->family = lowdup(f->family_name);
			x->path = xstrndup(path, strlen(path));
			x->index = i;
			x->weight = os2_weight(f);
			x->italic = (f->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
		}
		FT_Done_Face(f);
	}
}

static void scan_dir(const char *dir, int depth)
{
	DIR *d;
	struct dirent *e;

	if (depth > 6 || !(d = opendir(dir)))
		return;
	while ((e = readdir(d))) {
		char path[1024];
		struct stat st;
		size_t n = strlen(e->d_name);

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			scan_dir(path, depth + 1);
		else if (n > 4 && (!strcmp(e->d_name + n - 4, ".ttf") ||
				   !strcmp(e->d_name + n - 4, ".otf") ||
				   !strcmp(e->d_name + n - 4, ".ttc") ||
				   !strcmp(e->d_name + n - 4, ".TTF") ||
				   !strcmp(e->d_name + n - 4, ".OTF")))
			add_file(path);
	}
	closedir(d);
}

void fonts_scan(const char *extra)
{
	const char *home = getenv("HOME");
	char path[1024];

	scanned = 1;
	if (!lib_init())
		return;
	if (extra)
		scan_dir(extra, 0);
	scan_dir("/usr/share/fonts", 0);
	scan_dir("/usr/local/share/fonts", 0);
	if (home) {
		snprintf(path, sizeof(path), "%s/.fonts", home);
		scan_dir(path, 0);
		snprintf(path, sizeof(path), "%s/.local/share/fonts", home);
		scan_dir(path, 0);
	}
}

int soie_fonts_add_dir(const char *dir)
{
	int before = nfaces;

	if (!lib_init())
		return -1;
	scanned = 1;
	scan_dir(dir, 0);
	return nfaces - before;
}

/* Web fonts: bytes kept with the face, which is per document. */
struct face *font_add_memory(const void *doc, const char *family,
	unsigned char *data, size_t len, int weight, int italic)
{
	struct face *x;
	FT_Face f;

	if (!lib_init() || FT_New_Memory_Face(ft, data, (FT_Long)len, 0, &f)) {
		free(data);
		return NULL;
	}
	if (!scanned)
		fonts_scan(NULL);
	x = add_face();
	x->family = lowdup(family);
	x->data = data;
	x->len = len;
	x->weight = weight ? weight : os2_weight(f);
	x->italic = italic;
	x->doc = doc;
	x->ft = f;
	x->upem = f->units_per_EM ? f->units_per_EM : 1000;
	return x;
}

void fonts_drop_document(const void *doc)
{
	int i, j = 0;

	for (i = 0; i < nfaces; i++) {
		if (faces[i].doc == doc) {
			face_close(&faces[i]);
			free(faces[i].family);
			free(faces[i].data);
			continue;
		}
		faces[j++] = faces[i];
	}
	nfaces = j;
	font_cache_clear();
}

int face_open(struct face *x)
{
	if (x->ft)
		return 1;
	if (x->failed || !x->path)
		return 0;
	if (FT_New_Face(ft, x->path, x->index, &x->ft)) {
		x->failed = 1;
		return 0;
	}
	x->upem = x->ft->units_per_EM ? x->ft->units_per_EM : 1000;
	return 1;
}

void face_close(struct face *x)
{
#ifdef SOIE_HARFBUZZ
	if (x->hb)
		hb_font_destroy(x->hb);
	x->hb = NULL;
#endif
	if (x->ft)
		FT_Done_Face(x->ft);
	x->ft = NULL;
	x->mft = NULL;
}

/* Matching */

static const char *const generics[][5] = {
	{ "serif", "dejavu serif", "liberation serif", "times new roman",
		"noto serif" },
	{ "sans-serif", "dejavu sans", "liberation sans", "arial",
		"noto sans" },
	{ "monospace", "dejavu sans mono", "liberation mono", "courier new",
		"noto sans mono" },
	{ "system-ui", "dejavu sans", "liberation sans", "noto sans", "arial" },
	{ "ui-sans-serif", "dejavu sans", "liberation sans", "noto sans",
		"arial" },
	{ "ui-serif", "dejavu serif", "liberation serif", "noto serif",
		"times new roman" },
	{ "ui-monospace", "dejavu sans mono", "liberation mono",
		"noto sans mono", "courier new" },
	{ "cursive", "dejavu serif", "liberation serif", "noto serif",
		"times new roman" },
	{ "fantasy", "dejavu sans", "liberation sans", "noto sans", "arial" },
	{ "math", "dejavu serif", "liberation serif", "noto serif", "serif" },
	{ "emoji", "noto color emoji", "dejavu sans", "symbola", "arial" },
	{ NULL, NULL, NULL, NULL, NULL }
};

/* CSS Fonts 4 5.2: the weight closest the right way. */
static int weight_cost(int want, int have)
{
	if (want == have)
		return 0;
	if (want >= 400 && want <= 500) {
		if (have >= want && have <= 500)
			return have - want;
		if (have < want)
			return 1000 + want - have;
		return 2000 + have - want;
	}
	if (want < 400)
		return have < want ? want - have : 1000 + have - want;
	return have > want ? have - want : 1000 + want - have;
}

/* Best face of a family for weight and italic, or -1. */
static int find_family(const char *fam, int weight, int italic,
	const void *doc)
{
	int i, best = -1, bc = 1 << 30;

	for (i = 0; i < nfaces; i++) {
		int c;

		if (strcmp(faces[i].family, fam) ||
		    (faces[i].doc && faces[i].doc != doc))
			continue;
		c = weight_cost(weight, faces[i].weight) +
			(faces[i].italic != italic ? 100000 : 0) +
			(faces[i].doc ? 0 : 1);	/* web fonts first */
		if (c < bc) {
			bc = c;
			best = i;
		}
	}
	return best;
}

static int find_name(const char *name, int weight, int italic,
	const void *doc)
{
	int i, k, r;

	r = find_family(name, weight, italic, doc);
	if (r >= 0)
		return r;
	for (i = 0; generics[i][0]; i++)
		if (!strcmp(name, generics[i][0]))
			for (k = 1; k < 5; k++)
				if ((r = find_family(generics[i][k], weight,
						     italic, doc)) >= 0)
					return r;
	return -1;
}

/* A font chain: the faces of font-family, in order, plus a default. */
struct chain {
	const char *family;	/* interned family list */
	int weight, italic;
	const void *doc;
	int face[8];
	int n;
};

#define NCHAINS 256
static struct chain chains[NCHAINS];
static int nchains;

void font_cache_clear(void)
{
	nchains = 0;
	width_cache_clear();
}

static const struct chain *chain_for(const struct style *st)
{
	const char *fam = st->v[P_FONT_FAMILY].u.str;
	int weight = (int)st->v[P_FONT_WEIGHT].u.num;
	int italic = !kw(st, P_FONT_STYLE, "normal");
	const void *doc = font_doc;
	struct chain *c;
	const char *p;
	int i;

	for (i = 0; i < nchains; i++)
		if (chains[i].family == fam && chains[i].weight == weight &&
		    chains[i].italic == italic && chains[i].doc == doc)
			return &chains[i];
	if (nchains == NCHAINS)
		nchains = 0;	/* crude: start again */
	c = &chains[nchains++];
	memset(c, 0, sizeof(*c));
	c->family = fam;
	c->weight = weight;
	c->italic = italic;
	c->doc = doc;
	for (p = fam ? fam : ""; *p && c->n < 7; ) {
		char name[128];
		size_t n = 0;
		int f;

		while (*p == ' ' || *p == ',')
			p++;
		if (*p == '"') {
			p++;
			while (*p && *p != '"' && n < sizeof(name) - 1)
				name[n++] = (char)(*p >= 'A' && *p <= 'Z' ?
					*p + 32 : *p), p++;
			if (*p == '"')
				p++;
		} else {
			while (*p && *p != ',' && n < sizeof(name) - 1)
				name[n++] = (char)(*p >= 'A' && *p <= 'Z' ?
					*p + 32 : *p), p++;
			while (n && name[n - 1] == ' ')
				n--;
		}
		name[n] = '\0';
		while (*p && *p != ',')
			p++;
		if (n && (f = find_name(name, weight, italic, doc)) >= 0)
			c->face[c->n++] = f;
	}
	/* the default: serif, as browsers */
	if ((i = find_name("serif", weight, italic, doc)) >= 0 ||
	    (i = find_name("sans-serif", weight, italic, doc)) >= 0)
		c->face[c->n++] = i;
	else if (nfaces)
		c->face[c->n++] = 0;
	return c;
}


static int has_glyph(int fi, unsigned long cp)
{
	struct face *x = &faces[fi];

	if (!face_open(x))
		return 0;
	return FT_Get_Char_Index(x->ft, cp) != 0;
}

/* Face to draw cp with: the chain, then any face that has it. */
int font_pick(const struct style *st, unsigned long cp)
{
	const struct chain *c = chain_for(st);
	static unsigned long last_cp = (unsigned long)-1;
	static int last_face = -1;
	int i;

	for (i = 0; i < c->n; i++)
		if (has_glyph(c->face[i], cp))
			return c->face[i];
	if (cp == last_cp && last_face >= 0)
		return last_face;
	for (i = 0; i < nfaces; i++)
		if ((!faces[i].doc || faces[i].doc == font_doc) &&
		    has_glyph(i, cp)) {
			last_cp = cp;
			last_face = i;
			return i;
		}
	return c->n ? c->face[0] : -1;
}

int font_primary(const struct style *st)
{
	const struct chain *c = chain_for(st);

	return c->n ? c->face[0] : -1;
}

/* Font metrics in px for a face at size px. */
void face_metrics(int fi, float size, struct fontm *m)
{
	struct face *x = &faces[fi];
	FT_Face f;
	TT_OS2 *os2;
	float s, asc, desc, gap;

	if (!face_open(x)) {
		m->ascent = size * 0.75f;
		m->descent = size * 0.25f;
		m->normal_lh = size * 1.25f;
		return;
	}
	f = x->ft;
	s = size / (float)x->upem;
	if (x->mft != f) {
		os2 = FT_Get_Sfnt_Table(f, FT_SFNT_OS2);
		if (os2 && (os2->fsSelection & 128)) {	/* USE_TYPO_METRICS */
			x->masc = os2->sTypoAscender;
			x->mdesc = -os2->sTypoDescender;
			x->mgap = os2->sTypoLineGap;
		} else {
			x->masc = f->ascender;
			x->mdesc = -f->descender;
			x->mgap = f->height - f->ascender + f->descender;
		}
		if (x->mgap < 0)
			x->mgap = 0;
		x->mft = f;
	}
	asc = x->masc;
	desc = x->mdesc;
	gap = x->mgap;
	m->ascent = asc * s;
	m->descent = desc * s;
	m->normal_lh = (asc + desc + gap) * s;
}
