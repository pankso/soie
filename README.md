# Soie

A lightweight HTML/CSS rendering engine in C99, with a small
reference browser. No toolkit, no JavaScript in the core, built to
render modern pages on old machines. See `doc/DECISIONS.md` for what
Soie is and is not, and how extensions plug in.

Status: the eleven phases are in, version 1.0.0. Extensions
(`lib/soie/ext.h`) bring cookies, content blocking, WebP, JavaScript
(QuickJS) and audio/video (FFmpeg, ALSA). `browser/soie` browses the
web (HTTPS, forms, history, cookies, scripts, video). The HTML parser follows the WHATWG algorithm and passes the
html5lib tree-construction tests; the CSS parser, selectors, cascade and computed values pass css-parsing-tests,
including all of CSS Color 4; block and inline layout work, with a
FreeType/HarfBuzz text (phase 4) and tables; flexbox and grid.

## Build

    make                # libsoie, tools/soie-render, browser/soie
    make check          # unit tests and the local corpus, no network
    make check-html5lib # fetches the html5lib corpus, runs it
    make check-css      # fetches css-parsing-tests, runs it (Python 3)
    make install PREFIX=/usr DESTDIR=...

Needs a C99 compiler, GNU make, pkg-config, FreeType, libpng, libjpeg
and (optional, `make HARFBUZZ=0`) HarfBuzz.

Extensions are modules in `ext/`, installed in `$(PREFIX)/lib/soie/ext`:
`cookies.so` and `block.so` need nothing, `webp.so` is built when
pkg-config finds libwebp, `media.so` when it finds FFmpeg and ALSA,
`script.so` (JavaScript) when quickjs-dev is installed (built with
-fPIC), or from the QuickJS sources:
`make QUICKJS_SRC=/path/to/quickjs-2024-01-13`.
`make EXTS="ext/cookies.so"` picks them. `make check-js` tests the
script extension.

## Try it

    tools/soie-render page.html          # dump the DOM tree
    tools/soie-render -f tr cells.html   # parse as a fragment of <tr>
    tools/soie-render -c latin1 old.html # charset from the transport
    tools/soie-render -p display,color,margin-top page.html
                                         # computed styles, after the cascade
    tools/soie-render -l -w 800 page.html # box tree and geometry
    tools/soie-render -o page.png -a page.html # render the whole page

The browser: `browser/soie [URL]`, or `browser/soie -o out.png URL`
to render a page without a display. It also needs Xlib, GnuTLS and
zlib. It loads the installed extensions, or those of
`$SOIE_EXT_DIR`: `SOIE_EXT_DIR=ext browser/soie` in the tree.
Cookies live in `~/.local/share/soie/cookies.txt`; a hosts-style
block list in `~/.config/soie/block.txt` turns content blocking on.
`soie-render -x ext/webp.so page.html` loads an extension;
`soie-render -x ext/script.so -j page.html` runs the page's scripts
(console output on stderr). `browser/soie -s` turns scripts off,
`SOIE_DEBUG=1` prints their errors. A click plays a video;
`SOIE_AUDIO=none` keeps players silent.

## Layout

    lib/         libsoie (public headers in lib/soie/)
    browser/     the browser: Xlib UI, HTTP and TLS
    ext/         extensions (cookies, block, webp, script, media)
    tools/       soie-render, generators
    tests/       unit tests, html5lib test runner, local corpus
    doc/         DECISIONS.md
