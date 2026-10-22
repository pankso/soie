# Soie - lightweight HTML rendering engine

VERSION = 0.1.0
SOVERSION = 0
PREFIX ?= /usr
DESTDIR ?=

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -pedantic -fPIC
CPPFLAGS += -Ilib

LIB_OBJS = lib/dom.o lib/dump.o lib/encoding.o
all: lib/libsoie.a lib/libsoie.so.$(SOVERSION)

lib/libsoie.a: $(LIB_OBJS)
	rm -f $@
	ar rcs $@ $(LIB_OBJS)

lib/libsoie.so.$(SOVERSION): $(LIB_OBJS)
	$(CC) -shared -Wl,-soname,libsoie.so.$(SOVERSION) $(LDFLAGS) \
		-o $@ $(LIB_OBJS)

$(LIB_OBJS): $(HEADERS) $(INTERNAL)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/soie
	cp lib/libsoie.a $(DESTDIR)$(PREFIX)/lib/
	cp lib/libsoie.so.$(SOVERSION) $(DESTDIR)$(PREFIX)/lib/
	ln -sf libsoie.so.$(SOVERSION) $(DESTDIR)$(PREFIX)/lib/libsoie.so
	cp $(HEADERS) $(DESTDIR)$(PREFIX)/include/soie/

clean:
	rm -f lib/*.o lib/libsoie.a lib/libsoie.so.*

.PHONY: all install clean
