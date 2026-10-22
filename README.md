# Soie

A lightweight HTML/CSS rendering engine in C99, with a small
reference browser. No toolkit, no JavaScript in the core, built to
render modern pages on old machines. See `doc/DECISIONS.md` for what
Soie is and is not, and how extensions plug in.

Status: phase 1, HTML parser and DOM. The parser follows the WHATWG
algorithm and passes the html5lib tree-construction tests.

## Build

    make                # libsoie.a, libsoie.so.0, tools/soie-render
    make check          # unit tests and the local corpus, no network
    make check-html5lib # fetches the html5lib corpus, runs it
    make install PREFIX=/usr DESTDIR=...

Only a C99 compiler and POSIX make are needed for now.

## Try it

    tools/soie-render page.html          # dump the DOM tree
    tools/soie-render -f tr cells.html   # parse as a fragment of <tr>
    tools/soie-render -c latin1 old.html # charset from the transport

## Layout

    lib/         libsoie (public headers in lib/soie/)
    tools/       soie-render, generators
    tests/       unit tests, html5lib test runner, local corpus
    doc/         DECISIONS.md
