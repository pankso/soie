# Soie

A lightweight HTML/CSS rendering engine in C99 with a small
reference browser. No toolkit, no JavaScript in the core, built to
render modern pages on old machines. See `doc/DECISIONS.md` for what
Soie is and is not, and how extensions plug in.

Status: version 0.1.1 (October 2026). Extensions
(`lib/soie/ext.h`) bring cookies, content blocking, WebP, JavaScript
(QuickJS) and audio/video (FFmpeg, ALSA, Media Source Extensions: YouTube plays). `browser/xsoie` browses the
web (HTTPS, forms, history, cookies, scripts, video). The HTML parser follows the WHATWG algorithm and passes the
html5lib tree-construction tests; the CSS parser, selectors, cascade and computed values pass css-parsing-tests,
including all of CSS Color 4; block and inline layout work with a
FreeType/HarfBuzz text (phase 4) and tables; flexbox and grid.

## Source

The repository is https://hg.slitaz.org/soie (Mercurial). A read-only
git mirror is at https://github.com/pankso/soie: pull requests there
are welcome, they are read and imported by hand into Mercurial
(`tools/pr-import.sh`). Bugs and ideas: https://maille.slitaz.org/

## Build

    make                # libsoie, tools/soie-render, browser/xsoie
    make check          # unit tests and the local corpus, no network
    make check-html5lib # fetches the html5lib corpus, runs it
    make check-css      # fetches css-parsing-tests, runs it (Python 3)
    make asan           # the tests under AddressSanitizer (gcc-83) in asan-build/
    make asan-sites     # the same, and real sites rendered whole (network)
    make install PREFIX=/usr DESTDIR=...

Needs a C99 compiler, GNU make, pkg-config, FreeType, libpng, libjpeg
and (optional, `make HARFBUZZ=0`) HarfBuzz.

Extensions are modules in `ext/`, installed in `$(PREFIX)/lib/soie/ext`:
`cookies.so` and `block.so` need nothing, `webp.so` is built when
pkg-config finds libwebp, `media.so` when it finds FFmpeg and ALSA,
`script.so` (JavaScript) when quickjs-dev is installed (built with
-fPIC) or from the QuickJS sources:
`make QUICKJS_SRC=/path/to/quickjs-2026-06-04`.
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

The browser: `browser/xsoie [URL]` or `browser/xsoie -o out.png URL`
to render a page without a display. It also needs Xlib, GnuTLS and
zlib. Its shell (`shell/`) has no toolkit; what its fronts
share (bars, keys, repainting only what changed) is `browser/front.c`,
xsoie is the Xlib part (MIT-SHM), `browser/wsoie` the Wayland one
(built when wayland-client, wayland-cursor and xkbcommon are there). Ctrl+L address (or words to search for,
`$SOIE_SEARCH`), Ctrl+F find, a drag (a double, triple click) selects
text, Ctrl+A all of it, Ctrl+C/Ctrl+V clipboard, Ctrl+N or the
middle button on a link a new window, Alt+arrows history, F5 reload,
Ctrl+plus, Ctrl+minus, Ctrl+0 zoom. Files that are not pages (archives,
ISO images) are downloaded to `~/Downloads` (or `$XDG_DOWNLOAD_DIR`).
It loads the extensions of `$SOIE_EXT_DIR`, else those of the source
tree when run from it, else the installed ones.
Cookies live in `~/.local/share/soie/cookies.txt`; a hosts-style
block list in `~/.config/soie/block.txt` turns content blocking on.
The ⚙ button (or `about:settings`) sets the start page, the search
engine, JavaScript, colours, zoom, cookies, blocking and the downloads
folder, kept in `~/.config/soie/settings`.
`soie-render -x ext/webp.so page.html` loads an extension;
`soie-render -x ext/script.so -j page.html` runs the page's scripts
(console output on stderr). `browser/xsoie -s` turns scripts off,
`SOIE_DEBUG=1` prints loads, timings and script errors (2: the input too), F12 saves the page for a bug report, a crash writes `~/.cache/soie/crash-PID.txt`. A click plays a video;
`SOIE_AUDIO=none` keeps players silent.

With GTK 3, `gtk/libsoie-gtk.so` is SoieView, a widget for a browser
to show its pages with Soie (`gtk/soie-view.h`), and `gtk/gsoie [URL...]`
a little tabbed browser on it.

## Layout

    lib/         libsoie (public headers in lib/soie/)
    shell/       the shell, no toolkit: pages, history, forms, find, HTTP
                 and TLS, fetch threads, debugging
    browser/     the browser's fronts: their common part, xsoie (Xlib),
                 wsoie (Wayland)
    gtk/         SoieView, a GTK 3 widget (libsoie-gtk) for browsers like
                 Toile, and gsoie, a little tabbed browser on it
    ext/         extensions (cookies, block, webp, script, media)
    tools/       soie-render, generators
    tests/       unit tests, html5lib test runner, local corpus
    doc/         DECISIONS.md

## Known limits

- Google search and YouTube may stop answering after a while: their
  bot checks (BotGuard) do not trust a JavaScript engine that is not
  SpiderMonkey or V8.
- No JavaScript in iframes (no realm of their own yet).
- No HSTS, and an https page may be redirected to http.
- No `:hover`, `:focus` and other user states in style sheets.
- No tabs in xsoie and wsoie, windows (Ctrl+N): Toile has tabs, on
  SoieView.

## Licence

BSD (2 clauses), as most SliTaz tools: see LICENSE. Authors and
assistants: see AUTHORS.
