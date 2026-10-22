# Soie

A lightweight HTML/CSS rendering engine in C99, with a small
reference browser. No toolkit, no JavaScript in the core, built to
render modern pages on old machines. See `doc/DECISIONS.md` for what
Soie is and is not, and how extensions plug in.

Status: phase 7 done: `browser/soie` browses the web (HTTPS, forms,
history), without JavaScript or cookies yet. The HTML parser follows the WHATWG algorithm
and passes the html5lib tree-construction tests; the CSS parser,
selectors, cascade and computed values pass css-parsing-tests,
including all of CSS Color 4; block and inline layout work, with a
FreeType/HarfBuzz text (phase 4) and tables.

## Build

    make                # libsoie, tools/soie-render, browser/soie
    make check          # unit tests and the local corpus, no network
    make check-html5lib # fetches the html5lib corpus, runs it
    make check-css      # fetches css-parsing-tests, runs it (Python 3)
    make install PREFIX=/usr DESTDIR=...

Needs a C99 compiler, GNU make, pkg-config, FreeType, libpng, libjpeg
and (optional, `make HARFBUZZ=0`) HarfBuzz.

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
zlib.

## Layout

    lib/         libsoie (public headers in lib/soie/)
    browser/     the browser: Xlib UI, HTTP and TLS
    tools/       soie-render, generators
    tests/       unit tests, html5lib test runner, local corpus
    doc/         DECISIONS.md
