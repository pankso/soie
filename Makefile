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
	lib/text.o lib/boxes.o lib/block.o lib/inline.o lib/table.o lib/font.o lib/paint.o \
	lib/image.o lib/svg.o lib/forms.o lib/flex.o lib/grid.o lib/ext.o \
	lib/query.o lib/script.o lib/player.o lib/damage.o
LIBS = $(shell pkg-config --libs $(FONT_PKGS) libpng) -ljpeg -lm -ldl
PNG_LIBS =
CPPFLAGS += $(shell pkg-config --cflags libpng)
HEADERS = lib/soie/soie.h lib/soie/dom.h lib/soie/html.h lib/soie/style.h \
	lib/soie/layout.h lib/soie/ext.h
EXTDIR = $(PREFIX)/lib/soie/ext
INTERNAL = lib/internal.h lib/parser.h lib/tags.h lib/css.h lib/props.h \
	lib/layout.h lib/font.h lib/paint.h lib/image.h

# The Wayland browser when its libraries and protocols are there
ifeq ($(shell pkg-config --exists wayland-client wayland-cursor xkbcommon \
	wayland-protocols && command -v wayland-scanner >/dev/null && echo y),y)
WSOIE = browser/wsoie
endif

# Extensions: those without dependencies always, the others when
# their libraries are there (or list them: make EXTS="ext/cookies.so").
EXTS = ext/cookies.so ext/block.so
ifeq ($(shell pkg-config --exists libwebp && echo y),y)
EXTS += ext/webp.so
endif
# Audio and video: FFmpeg and ALSA.
MEDIA_PKGS = libavformat libavcodec libswscale libswresample libavutil alsa
ifeq ($(shell pkg-config --exists $(MEDIA_PKGS) && echo y),y)
EXTS += ext/media.so
endif
# JavaScript: QuickJS 2026-06-04, its sources (an unpacked release)
# built in, else the installed library (SliTaz's quickjs-dev, built
# with -fPIC).
QUICKJS_SRC ?=
QUICKJS ?= /usr
ifneq ($(QUICKJS_SRC),)
EXTS += ext/script.so
QJS_INC = $(QUICKJS_SRC)
QJS_OBJS = ext/script/qjs-quickjs.o ext/script/qjs-libregexp.o \
	ext/script/qjs-libunicode.o ext/script/qjs-cutils.o ext/script/qjs-dtoa.o
else ifneq ($(wildcard $(QUICKJS)/lib/quickjs/libquickjs.a),)
EXTS += ext/script.so
QJS_INC = $(QUICKJS)/include/quickjs
QJS_OBJS = $(QUICKJS)/lib/quickjs/libquickjs.a
endif

all: lib/libsoie.a lib/libsoie.so.$(SOVERSION) tools/soie-render browser/xsoie \
	$(WSOIE) $(EXTS)

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

# Programs that load extensions link the shared library, which the
# extensions link too; found next door in the tree or once installed.
RPATH = -Wl,-rpath,'$$ORIGIN/../lib'
tools/soie-render: tools/soie-render.o lib/libsoie.so.$(SOVERSION)
	$(CC) $(LDFLAGS) $(RPATH) -o $@ tools/soie-render.o \
		lib/libsoie.so.$(SOVERSION) $(LIBS)

ext/%.so: ext/%.c lib/libsoie.so.$(SOVERSION) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $(LDFLAGS) -o $@ $< \
		lib/libsoie.so.$(SOVERSION) $(EXT_LIBS)

ext/cookies.so: EXT_LIBS = -lpthread
ext/media.so: CPPFLAGS += $(shell pkg-config --cflags $(MEDIA_PKGS))
ext/media.so: EXT_LIBS = $(shell pkg-config --libs $(MEDIA_PKGS)) -lpthread \
	-lm
ext/webp.so: CPPFLAGS += $(shell pkg-config --cflags libwebp)
ext/webp.so: EXT_LIBS = $(shell pkg-config --libs libwebp)

# 64-bit atomics (Atomics, SharedArrayBuffer) need libatomic on i486
QJS_ATOMIC = $(shell echo 'int main(void){return 0;}' | \
	$(CC) -x c - -latomic -o /dev/null 2>/dev/null && echo -latomic)
ext/script/qjs-%.o: $(QUICKJS_SRC)/%.c
	$(CC) $(CFLAGS:-pedantic=) -std=gnu99 -w -fPIC -DNDEBUG -D_GNU_SOURCE \
		-DCONFIG_VERSION=\"$$(cat $(QUICKJS_SRC)/VERSION)\" \
		-c -o $@ $<

ext/script/dom-js.c: ext/script/dom.js
	{ echo 'const char dom_js[] ='; \
	  sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' $<; \
	  echo ';'; } > $@

ext/script/script.o: CPPFLAGS += -isystem $(QJS_INC)
ext/script/script.o: $(wildcard $(QJS_INC)/quickjs.h)
ext/script/dom-js.o: CFLAGS += -Wno-overlength-strings
ext/script/script.o ext/script/dom-js.o: $(HEADERS)
ext/script.so: ext/script/script.o ext/script/dom-js.o $(QJS_OBJS) \
	lib/libsoie.so.$(SOVERSION)
	$(CC) -shared $(LDFLAGS) -o $@ ext/script/script.o \
		ext/script/dom-js.o $(QJS_OBJS) lib/libsoie.so.$(SOVERSION) \
		-lm -lpthread $(QJS_ATOMIC)

tests/html5lib: tests/html5lib.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/html5lib.o lib/libsoie.a $(LIBS)

tests/unit: tests/unit.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/unit.o lib/libsoie.a $(LIBS)

# The shell (shell/: pages, network: GnuTLS, zlib), no toolkit; the
# browser's fronts (browser/): their common part (front.c) and their
# toolkits: xsoie on Xlib (MIT-SHM).
SHELL_PKGS = gnutls zlib libpng
SHELL_OBJS = shell/shell.o shell/net.o shell/fetch.o shell/debug.o
SHELL_HEADERS = shell/shell.h shell/net.h shell/fetch.h shell/debug.h
FRONT_OBJS = browser/front.o $(SHELL_OBJS)
browser/xsoie: browser/xsoie.o $(FRONT_OBJS) lib/libsoie.so.$(SOVERSION)
	$(CC) $(LDFLAGS) $(RPATH) -o $@ browser/xsoie.o $(FRONT_OBJS) \
		lib/libsoie.so.$(SOVERSION) $(LIBS) \
		$(shell pkg-config --libs $(SHELL_PKGS) x11 xext) -lpthread

browser/xsoie.o $(FRONT_OBJS): $(SHELL_HEADERS) browser/front.h $(HEADERS)
browser/xsoie.o browser/wsoie.o browser/front.o: CPPFLAGS += -Ishell
$(SHELL_OBJS): CPPFLAGS += $(shell pkg-config --cflags $(SHELL_PKGS)) \
	-DSOIE_EXT_DIR='"$(EXTDIR)"'
browser/xsoie.o: CPPFLAGS += $(shell pkg-config --cflags x11 xext)

# wsoie on Wayland, when its libraries are there; xdg-shell's and
# xdg-decoration's code is made from the protocols by wayland-scanner.
WL_PKGS = wayland-client wayland-cursor xkbcommon
WL_PROTO = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WL_OBJS = browser/xdg-shell.o browser/xdg-decoration.o
browser/wsoie: browser/wsoie.o $(WL_OBJS) $(FRONT_OBJS) \
	lib/libsoie.so.$(SOVERSION)
	$(CC) $(LDFLAGS) $(RPATH) -o $@ browser/wsoie.o $(WL_OBJS) \
		$(FRONT_OBJS) lib/libsoie.so.$(SOVERSION) $(LIBS) \
		$(shell pkg-config --libs $(SHELL_PKGS) $(WL_PKGS)) -lpthread
browser/xdg-shell.h: $(WL_PROTO)/stable/xdg-shell/xdg-shell.xml
	wayland-scanner client-header $< $@
browser/xdg-shell.c: $(WL_PROTO)/stable/xdg-shell/xdg-shell.xml
	wayland-scanner private-code $< $@
browser/xdg-decoration.h: \
	$(WL_PROTO)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	wayland-scanner client-header $< $@
browser/xdg-decoration.c: \
	$(WL_PROTO)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	wayland-scanner private-code $< $@
browser/wsoie.o: browser/xdg-shell.h browser/xdg-decoration.h \
	shell/shell.h browser/front.h $(HEADERS)
browser/wsoie.o $(WL_OBJS): CPPFLAGS += \
	$(shell pkg-config --cflags $(WL_PKGS))
tools/soie-render.o: CPPFLAGS += -DSOIE_EXT_DIR='"$(EXTDIR)"'

tests/exttest: tests/exttest.o lib/libsoie.so.$(SOVERSION)
	$(CC) $(LDFLAGS) $(RPATH) -o $@ tests/exttest.o \
		lib/libsoie.so.$(SOVERSION)

tests/painttest: tests/painttest.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/painttest.o lib/libsoie.a $(LIBS) \
		$(PNG_LIBS)

tests/csstest: tests/csstest.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/csstest.o lib/libsoie.a $(LIBS)

tests/cssvalue: tests/cssvalue.o lib/libsoie.a
	$(CC) $(LDFLAGS) -o $@ tests/cssvalue.o lib/libsoie.a $(LIBS)

tests/csstest.o tests/cssvalue.o: $(INTERNAL)

tools/soie-render.o tests/html5lib.o tests/unit.o tests/painttest.o \
	tests/exttest.o: $(HEADERS)
tests/csstest.o tests/cssvalue.o: CPPFLAGS += -Ilib

# Tests run with glibc filling freed memory: a use after free shows.
PERTURB = MALLOC_PERTURB_=165

# Our own corpus, no network needed.
check: tests/unit tests/html5lib tests/painttest tests/exttest $(EXTS)
	$(PERTURB) ./tests/unit
	$(PERTURB) ./tests/exttest
	$(PERTURB) ./tests/painttest tests/paint/*.html
	$(PERTURB) ./tests/html5lib tests/tree/*.dat tests/style/*.dat \
		tests/layout/*.dat

# Scripts: each tests/js page's console output against its .out file,
# without the stack traces (they name lines of dom.js).
# media-* pages need the media extension, played silently.
check-js: tools/soie-render ext/script.so
	@fail=0; for f in tests/js/*.html; do \
		x="-x ext/script.so"; \
		case $$f in */media-*) [ -f ext/media.so ] || continue; \
			x="$$x -x ext/media.so -T 8000";; esac; \
		$(PERTURB) SOIE_AUDIO=none tools/soie-render -S $$x -j $$f \
			2>&1 >/dev/null | \
			grep -v '^    at ' | cmp -s - $${f%.html}.out || \
			{ echo "FAIL $$f"; fail=1; }; \
	done; [ $$fail = 0 ] && echo "js: all passed"

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
	cp tools/soie-render browser/xsoie $(WSOIE) $(DESTDIR)$(PREFIX)/bin/
	mkdir -p $(DESTDIR)$(EXTDIR)
	cp $(EXTS) $(DESTDIR)$(EXTDIR)/

clean:
	rm -f lib/*.o lib/libsoie.a lib/libsoie.so.* tools/*.o \
		tools/soie-render shell/*.o browser/*.o browser/xsoie browser/wsoie \
		browser/xdg-shell.[ch] browser/xdg-decoration.[ch] \
		tests/*.o tests/html5lib tests/unit \
		tests/csstest tests/cssvalue tests/painttest tests/exttest lib/ua-css.c \
		tests/paint/*.out.png ext/*.so \
		ext/script/*.o ext/script/dom-js.c

.PHONY: all check check-js check-html5lib check-css install clean
