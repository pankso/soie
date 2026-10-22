# Soie - lightweight HTML rendering engine

VERSION = 0.1.0
SOVERSION = 0
PREFIX ?= /usr
DESTDIR ?=

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -pedantic -fPIC
CPPFLAGS += -Ilib

H5TESTS = tests/html5lib-tests
H5TESTS_URL = https://github.com/html5lib/html5lib-tests.git
H5TESTS_REV = 9329e64694e7835d0dcff9811e22856ef6ad16f9

LIB_OBJS = lib/dom.o lib/dump.o lib/encoding.o lib/tokenizer.o \
	lib/treebuilder.o
HEADERS = lib/soie/soie.h lib/soie/dom.h lib/soie/html.h
INTERNAL = lib/internal.h lib/parser.h lib/tags.h

all: lib/libsoie.a lib/libsoie.so.$(SOVERSION) tools/soie-render

lib/libsoie.a: $(LIB_OBJS)
	rm -f $@
	ar rcs $@ $(LIB_OBJS)

lib/libsoie.so.$(SOVERSION): $(LIB_OBJS)
	$(CC) -shared -Wl,-soname,libsoie.so.$(SOVERSION) $(LDFLAGS) \
		-o $@ $(LIB_OBJS)

$(LIB_OBJS): $(HEADERS) $(INTERNAL)
lib/tokenizer.o: lib/entities.inc

tools/soie-render: tools/soie-render.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tools/soie-render.o lib/libsoie.a

tests/html5lib: tests/html5lib.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/html5lib.o lib/libsoie.a

tests/unit: tests/unit.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/unit.o lib/libsoie.a

tools/soie-render.o tests/html5lib.o tests/unit.o: $(HEADERS)

# Our own corpus, no network needed.
check: tests/unit tests/html5lib
	./tests/unit
	./tests/html5lib tests/tree/*.dat

# The html5lib tree construction corpus, fetched on demand.
$(H5TESTS):
	git clone -q $(H5TESTS_URL) $@
	cd $@ && git checkout -q $(H5TESTS_REV)

check-html5lib: tests/html5lib $(H5TESTS)
	./tests/html5lib -x tests/html5lib-known \
		$(H5TESTS)/tree-construction/*.dat

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
		tools/soie-render tests/*.o tests/html5lib tests/unit

.PHONY: all check check-html5lib install clean
