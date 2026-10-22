# Soie

A lightweight HTML/CSS rendering engine in C99, with a small
reference browser. No toolkit, no JavaScript in the core, built to
render modern pages on old machines. See `doc/DECISIONS.md` for what
Soie is and is not, and how extensions plug in.

Status: phase 3 done. The HTML parser follows the WHATWG algorithm
and passes the html5lib tree-construction tests; the CSS parser,
selectors, cascade and computed values pass css-parsing-tests,
including all of CSS Color 4; block and inline layout work, with a
fixed-advance stub font until FreeType arrives (phase 4).

## Build

    make                # libsoie.a, libsoie.so.0, tools/soie-render
    make check          # unit tests and the local corpus, no network
    make check-html5lib # fetches the html5lib corpus, runs it
    make check-css      # fetches css-parsing-tests, runs it (Python 3)
    make install PREFIX=/usr DESTDIR=...

Only a C99 compiler, POSIX make and libm are needed for now.

## Try it

    tools/soie-render page.html          # dump the DOM tree
    tools/soie-render -f tr cells.html   # parse as a fragment of <tr>
    tools/soie-render -c latin1 old.html # charset from the transport
    tools/soie-render -p display,color,margin-top page.html
                                         # computed styles, after the cascade
    tools/soie-render -l -w 800 page.html # box tree and geometry

## Layout

    lib/         libsoie (public headers in lib/soie/)
    tools/       soie-render, generators
    tests/       unit tests, html5lib test runner, local corpus
    doc/         DECISIONS.md
