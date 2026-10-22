# Soie - lightweight HTML rendering engine

VERSION = 0.1.0
SOVERSION = 0
PREFIX ?= /usr
DESTDIR ?=

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -pedantic -fPIC
CPPFLAGS += -Ilib

# FreeType is needed; HarfBuzz is optional (make HARFBUZZ=0).
HARFBUZZ ?= 1
FONT_PKGS = freetype2
ifeq ($(HARFBUZZ),1)
FONT_PKGS += harfbuzz
CPPFLAGS += -DSOIE_HARFBUZZ
endif
CPPFLAGS += $(shell pkg-config --cflags $(FONT_PKGS))

H5TESTS = tests/html5lib-tests
H5TESTS_URL = https://github.com/html5lib/html5lib-tests.git
H5TESTS_REV = 9329e64694e7835d0dcff9811e22856ef6ad16f9
CSSTESTS = tests/css-parsing-tests
CSSTESTS_URL = https://github.com/SimonSapin/css-parsing-tests.git
CSSTESTS_REV = 203ce36bffd617db7f118c551e32794561fb273d

LIB_OBJS = lib/dom.o lib/dump.o lib/encoding.o lib/tokenizer.o \
	lib/treebuilder.o lib/csssyntax.o lib/selector.o lib/values.o \
	lib/props.o lib/media.o lib/cascade.o lib/url.o lib/ua-css.o \
	lib/text.o lib/boxes.o lib/block.o lib/inline.o lib/table.o lib/font.o
LIBS = $(shell pkg-config --libs $(FONT_PKGS)) -lm
HEADERS = lib/soie/soie.h lib/soie/dom.h lib/soie/html.h lib/soie/style.h \
	lib/soie/layout.h
INTERNAL = lib/internal.h lib/parser.h lib/tags.h lib/css.h lib/props.h \
	lib/layout.h lib/font.h

all: lib/libsoie.a lib/libsoie.so.$(SOVERSION) tools/soie-render

lib/libsoie.a: $(LIB_OBJS)
	rm -f $@
	ar rcs $@ $(LIB_OBJS)

lib/libsoie.so.$(SOVERSION): $(LIB_OBJS)
	$(CC) -shared -Wl,-soname,libsoie.so.$(SOVERSION) $(LDFLAGS) \
		-o $@ $(LIB_OBJS) $(LIBS)

$(LIB_OBJS): $(HEADERS) $(INTERNAL)
lib/tokenizer.o: lib/entities.inc

# The user agent style sheet, as a C string.
lib/ua-css.c: lib/ua.css
	{ echo 'const char *const ua_css[] = {'; \
	  sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n",/' lib/ua.css; \
	  echo '0 };'; } > $@

tools/soie-render: tools/soie-render.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tools/soie-render.o lib/libsoie.a $(LIBS)

tests/html5lib: tests/html5lib.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/html5lib.o lib/libsoie.a $(LIBS)

tests/unit: tests/unit.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/unit.o lib/libsoie.a $(LIBS)

tests/csstest: tests/csstest.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/csstest.o lib/libsoie.a $(LIBS)

tests/cssvalue: tests/cssvalue.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/cssvalue.o lib/libsoie.a $(LIBS)

tests/csstest.o tests/cssvalue.o: $(INTERNAL)

tools/soie-render.o tests/html5lib.o tests/unit.o: $(HEADERS)
tests/csstest.o tests/cssvalue.o: CPPFLAGS += -Ilib

# FreeType is needed; HarfBuzz is optional (make HARFBUZZ=0).
HARFBUZZ ?= 1
FONT_PKGS = freetype2
ifeq ($(HARFBUZZ),1)
FONT_PKGS += harfbuzz
CPPFLAGS += -DSOIE_HARFBUZZ
endif
CPPFLAGS += $(shell pkg-config --cflags $(FONT_PKGS))

# Our own corpus, no network needed.
check: tests/unit tests/html5lib
	./tests/unit
	./tests/html5lib tests/tree/*.dat tests/style/*.dat tests/layout/*.dat

# The html5lib tree construction corpus, fetched on demand.
$(H5TESTS):
	git clone -q $(H5TESTS_URL) $@
	cd $@ && git checkout -q $(H5TESTS_REV)

check-html5lib: tests/html5lib $(H5TESTS)
	./tests/html5lib -x tests/html5lib-known \
		$(H5TESTS)/tree-construction/*.dat

# The css-parsing-tests corpus (Python 3 runs it), fetched on demand.
$(CSSTESTS):
	git clone -q $(CSSTESTS_URL) $@
	cd $@ && git checkout -q $(CSSTESTS_REV)

check-css: tests/csstest tests/cssvalue $(CSSTESTS)
	python3 tests/css-parsing.py $(CSSTESTS)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/soie \
		$(DESTDIR)$(PREFIX)/bin
	cp lib/libsoie.a $(DESTDIR)$(PREFIX)/lib/
	cp lib/libsoie.so.$(SOVERSION) $(DESTDIR)$(PREFIX)/lib/
	ln -sf libsoie.so.$(SOVERSION) $(DESTDIR)$(PREFIX)/lib/libsoie.so
	cp $(HEADERS) $(DESTDIR)$(PREFIX)/include/soie/
	cp tools/soie-render $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f lib/*.o lib/libsoie.a lib/libsoie.so.* tools/*.o \
		tools/soie-render tests/*.o tests/html5lib tests/unit \
		tests/csstest tests/cssvalue lib/ua-css.c

.PHONY: all check check-html5lib check-css install clean
