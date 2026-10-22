# Soie design decisions

Running log of choices a fresh session could not guess from the code.
Newest entries at the end of each section. When a decision is
reversed, edit the entry and say why; do not leave stale rules here.

## What Soie is

- A from-scratch HTML/CSS rendering engine in C99: bytes in, a DOM,
  a styled box tree, and an RGBA pixel buffer out.
- A library first (`libsoie`). The reference browser (`browser/`)
  and the test tool (`tools/soie-render`) are ordinary consumers and
  only use the public headers in `lib/soie/`.
- Aimed at old and small machines: i486 to x86_64, a typical news
  page under 50 MB RSS without extensions.
- Usable by anyone: no dependency on SliTaz, GTK or any toolkit.

## What Soie is not

- Not a web platform. No JavaScript, no cookies, no media decoding,
  no network in the core, ever. Those are extensions.
- Not a browser framework with process isolation, GPU compositing,
  or multi-threaded layout. One thread, one page, one pixel buffer.
- Not pixel-identical to Blink or Gecko. The target is "the page
  looks like itself", measured against our reference PNGs.

## Extension model

- The core knows nothing about any extension. It exposes hooks in
  `lib/soie/ext.h`: one versioned struct of function pointers per
  extension point (script, cookies, media, image, net, content).
- An extension fills the struct and registers it. Compiled in or
  loaded with `dlopen()`; both paths go through the same register
  call, so the core never knows which.
- Each struct starts with `abi_version` and `size`. The core refuses
  a major mismatch and only reads `size` bytes, so a newer core can
  add trailing hooks without breaking older extensions.
- `ext.h` grows hook by hook as phases need them (resource requests
  arrive with images and networking), and is frozen as 1.0 in
  phase 9. Before phase 9 the extension ABI may change freely.
- `net` hooks live in the browser, not in libsoie: the library never
  fetches anything, it asks the host for bytes.

## Public API

- Opaque types and accessor functions (`soie_node_parent()`, ...),
  never public structs. Struct layout can then change without an ABI
  break, which semantic versioning needs. The cost is a function call
  per access; acceptable for a host, and the engine itself uses the
  internal structs directly.
- Prefix `soie_` for functions, `SOIE_` for constants. Strings are
  UTF-8, NUL-terminated, owned by the library unless a function says
  otherwise.
- Version 0.x until the API is frozen; the shared library soname is
  `libsoie.so.0` meanwhile. 1.0 comes with phase 9.

## Memory and ownership

- Nodes are malloc'ed one by one; a document owns every node created
  for it, in the tree or not (a doubly linked list through all of
  them, two pointers per node), and `soie_node_free(document)` frees
  everything. A node removed from the tree stays valid until then, so
  a script can keep it and reinsert it; nobody has to know whether a
  script engine still holds a detached node. The cost: nodes a page
  creates and drops live as long as the page.
  `soie_node_free()` on a node still frees it early.
- Each node has one `user` pointer for the host (a script engine's
  wrapper object), and the document a change counter bumped by every
  tree, attribute or text change: the host restyles when it moved.
- Allocation failure aborts the process with a message, like glib.
  Checking every malloc in a parser doubles its size for a case
  Linux overcommit almost never reports; API functions therefore do
  not return NULL for lack of memory.
- The parser can leave nodes out of the tree (a `<body>` replaced by
  a `<frameset>`, misnested markup in the adoption agency). It keeps
  them in an orphan list and frees those still without a parent at
  the end of the parse, so a parse never leaks.
- Known element names point to a static atom table; only unknown
  names are allocated. The tree builder compares atom ids, not
  strings.

## HTML parsing

- Follows the WHATWG parsing spec as of 2026-10, including the
  relaxed `<select>` parsing (no "in select" insertion modes any
  more, `select` is in the default scope list).
- Processing instructions (`<?target data?>`, added to the spec in
  2026) are NOT implemented yet: `<?` still produces a bogus comment,
  which is what the pinned html5lib tests and current browsers
  expect. Revisit when the tests and engines move.
- No parse error reporting. Errors only matter to conformance
  checkers; the parser just recovers.
- Scripting flag: off by default, because the core has no JS. When
  off, `<noscript>` content is parsed as markup and rendered, which is
  right for a browser without JS. The script extension turns it on.
- One-shot parsing (`soie_html_parse(buf, len)`). Incremental parsing
  and `document.write()` insertion points are deferred until the JS
  extension needs them; adding them will not change this function.
- Sanitizer hooks of the spec are ignored (always null config).
- Declarative shadow DOM (`<template shadowrootmode>`) is not
  implemented: such a template stays a plain template.

## CSS

- CSS Syntax 3 as in the current editor's draft, nesting included:
  declarations after a nested rule go into a "nested declarations"
  rule so that they keep their cascade order. No match tokens
  (`~=`...) and no unicode-range token: they left the spec.
- Conformance is checked against css-parsing-tests (SimonSapin),
  pinned at 203ce36, fetched by `make check-css`. Its runner is Python
  (`tests/css-parsing.py`), the only Python in the tree, because the
  corpus is JSON. The corpus follows the 2021 draft: the runner
  normalizes the differences (whitespace around declaration values,
  match tokens, parse errors) and lists the few inputs where the two
  drafts disagree. CSS Color 5 (`light-dark()`, `device-cmyk()`,
  `color-mix()`) is not done yet.
- Colors keep their color space and `none` components (`oklch(0.6
  0.2 30)` stays oklch), as CSS Color 4 asks for computed values;
  conversion to 8-bit sRGB happens when painting. Wide-gamut spaces
  (display-p3...) are drawn as if they were sRGB: old screens are
  sRGB at best.
- Number serialization rounds to 6 decimals with a +1e-7 bias, the
  same as the corpus generator; lengths, which are floats, to 4.
- Lengths are computed to `px + % of a reference`, so `calc(100% -
  2em)` needs no tree. `min()`, `max()` and `clamp()` with a
  percentage keep a small tree that layout evaluates; without
  percentages they fold to a length.
- Units the engine has no font metrics for yet are approximated:
  `ex` = `ch` = 0.5em, `cap` = 0.7em, `lh` = 1.2em. Phase 4 replaces
  them with real metrics.
- Logical properties (`margin-inline`, `padding-block-start`...) map
  to physical ones at parse time, for horizontal left-to-right text.
  `white-space` is a shorthand of `white-space-collapse` and
  `text-wrap-mode`, as in CSS Text 4.
- Properties layout and painting will need later (grid templates,
  transforms, shadows, background positions) are kept as normalized
  text and parsed by the phase that uses them.
- `revert` and `revert-layer` behave like `unset`. `@container`,
  `@scope`, `@keyframes`, `@page` and `@counter-style` are ignored.
  `@font-face` blocks are stored for phase 4.
- Selectors: level 3, plus `:is()`, `:where()`, `:not()` with lists,
  `:has()` and `:nth-child(An+B of S)`. Shadow DOM selectors
  (`:host`, `::slotted()`, `::part()`) parse but never match: Tailwind
  4 puts its theme on `:root, :host`, and an unknown pseudo-class
  would drop the whole rule. Dynamic states (`:hover`, `:focus`...)
  never match until the browser feeds element states in.
- Media queries level 4 with range syntax and three-valued logic.
  The environment is a 1024x768 light screen unless the host says
  otherwise; `hover: hover`, `pointer: fine`, resolution 1dppx.
- `@supports (prop: value)` asks the property parser, so it answers
  for exactly what the engine supports.
- The cascade follows CSS Cascade 5: origins and importance,
  element-attached styles (`style=""`) above layers, layers (with
  `!important` reversing their order), specificity, order. Each
  declaration gets one 64-bit key so that winning is a comparison.
- HTML presentational hints (`bgcolor`, `<font>`, `align`, table
  `border`/`cellpadding`, `width`/`height`...) are author-level
  declarations below every author layer, as the HTML spec says.
- Rules are indexed by the rightmost compound's id, class or tag;
  the rest goes to a universal list. `::before`/`::after` styles are
  only computed when a matching rule sets `content`.
- Computed styles are interned per document: elements with identical
  values share one struct style. Custom properties are interned sets
  of overrides chained to the parent's set (flattened past 8 levels),
  so a page with 1000 variables on `:root` does not copy them into
  every element.
- Resource loading goes through one synchronous host callback
  (`soie_fetch_fn`); it may become asynchronous with the browser
  (phase 7), while the API is still 0.x. `url()` values are made
  absolute against their sheet's URL at parse time.
- Measured on this machine (x86_64, -O2): tailwindcss.com 2400
  elements styled in 110 ms / 23 MB RSS, lemonde.fr 5000 elements in
  130 ms / 19 MB, en.wikipedia.org 7900 elements in 165 ms / 12 MB.
  github.com (7 MB of CSS, some sheets linked twice) takes 600 ms and
  56 MB: the rule data is about a million small allocations. An arena
  for style sheet data is the planned fix if real pages need it.

## Layout

- Phase 3 covers CSS 2 visual formatting: block and inline formatting
  contexts, anonymous boxes, block-in-inline splitting, margin
  collapsing (siblings, parent and first/last child, empty blocks),
  widths and auto margins (10.3.3), min/max sizes, box-sizing,
  percentages, floats and clear, BFC roots beside floats, relative,
  absolute and fixed positioning, inline-blocks with shrink-to-fit
  widths, replaced elements with intrinsic sizes and ratios,
  ::before/::after with strings, attr() and quotes, list markers.
- Tables (an extra step after phase 3, the brief has none) follow CSS
  2 chapter 17: anonymous table objects (orphan cells get a table),
  thead first and tfoot last, colspan/rowspan, the automatic layout
  (column min/max widths, spanning cells spread over their columns,
  extra width to auto columns only) and the fixed one, percentage
  columns, border-spacing, captions top or bottom, cells stretched to
  their rows and aligned top/middle/bottom (baseline counts as top).
  border-collapse: collapse only drops the spacing: borders are not
  merged yet. Quirks mode tables reset inherited font and text-align
  (HTML 15.3.6), and -webkit-center centers block children, as in
  browsers: old pages rely on both.
- Text measurement goes through font_metrics() and text_width() in
  lib/text.c. Until phase 4 they are a stub: every character is
  0.5em wide, ascent 0.75em, descent 0.25em, normal line-height
  1.25em. Round numbers keep the layout tests computable by hand;
  phase 4 swaps the stub, and the tests will move to a test font.
- Line breaking happens at spaces only (no UAX 14 yet, so no CJK
  breaking), plus overflow-wrap: anywhere/break-word. vertical-align
  top and bottom are approximated as text-top and text-bottom; sub,
  super and middle use the stub font's proportions.
- All geometry is in page coordinates (CSS px, floats), content boxes
  in struct box; fragments of lines carry text ranges for painting.
  `soie_layout_dump()` prints border boxes, line boxes and fragments,
  and the layout tests compare that dump.
- Known simplifications: a float before a block whose top margin
  collapses through its parent stays above the collapsed margin
  instead of moving down with it; inline boxes with position:
  relative are not offset; sticky is static; vertical margins of
  floats do not collapse (as specified), but clearance is computed
  the simple way (move below the floats).
- Box tree depth is capped at 512 levels (layout is recursive).
  Deeper content is kept as plain text flowing in the box at the
  limit, collected without recursion. Blink caps its DOM at 512 too.

## Fonts and text (phase 4)

- System fonts are found by scanning /usr/share/fonts,
  /usr/local/share/fonts, ~/.fonts and ~/.local/share/fonts with
  FreeType (no fontconfig: not in the dependency list, and SliTaz
  ships few fonts). The scan is lazy and done once per process.
- Matching follows CSS Fonts 4 loosely: family names case-insensitive,
  generic families mapped to DejaVu, Liberation, Noto or the usual
  Windows names, weight by the spec's nearest-weight rules, italic
  over normal. No family found: serif, like browsers.
- Per-character fallback: the font-family chain first, then any face
  that has the glyph. Runs are split by face before shaping.
- Measurement uses unhinted design units scaled to the font size, so
  layout does not depend on the screen resolution. HarfBuzz shapes
  when built in; without it, FreeType gives cmap glyphs, advances and
  legacy kerning. Word widths are cached per style.
- Metrics: OS/2 typo metrics when USE_TYPO_METRICS is set, else hhea,
  as browsers do; normal line-height = ascent + descent + line gap.
- Web fonts: @font-face fonts are fetched through the host callback
  during the cascade and opened from memory. FreeType here has zlib
  but no brotli, so WOFF2 (most web fonts today) is skipped and the
  page falls back to system fonts. A small brotli decoder would fix
  that; not done yet.
- `soie_fonts_stub(1)` keeps the 0.5em stub font: the test runner and
  `soie-render -S` use it, so layout tests do not depend on fonts.

## Painting (phase 5)

- `soie_paint()` draws the laid out page into the host's buffer:
  RGBA bytes, any stride, a scroll offset. The engine never owns the
  screen; hosts blit (X11, framebuffer, GTK, Wayland...).
- One software rasterizer, straight alpha source-over on 8-bit RGBA:
  anti-aliased rectangles by exact pixel coverage, rounded rectangles
  by 4x4 supersampling, glyphs from FreeType (light hinting, gray
  anti-aliasing, cached), synthetic bold and oblique when the face
  lacks them.
- Order: canvas background (root, or body when root has none), then
  each box's background and borders, blocks before floats, lines
  (inline boxes, then text and atomics), outlines; positioned boxes
  after the flow sorted by z-index, stable. Negative z-index boxes are
  not under the flow yet, and opacity multiplies colors instead of
  compositing a layer: both are cheap approximations.
- Borders: solid, dashed, dotted, double, inset/outset/groove/ridge
  (shaded); corners are not mitred. Rounded borders draw one color.
- overflow other than visible clips to the padding box (rectangles
  only; rounded clipping later).
- Paint tests (`tests/paint/*.html`, 320x240, stub font drawn as one
  box per character) compare pixels with reference PNGs. References
  are written with `tests/painttest -u` and checked by eye before
  they are committed. Pixels may differ by 2 levels per channel: the
  i486 x87 FPU and x86_64 SSE round floats differently (measured: 1
  level at most on some anti-aliased edges).

## Images (phase 6)

- Decoders: PNG (libpng), JPEG (libjpeg), GIF first frame (own LZW
  decoder, transparency, interlace), SVG. libpng and libjpeg are core
  dependencies, as the brief says; they never print (libpng's error
  and warning hooks are silenced). WebP and AVIF are for the image
  extension (phase 9): a page using them gets no image.
- SVG files go through our HTML parser, which handles foreign content
  well enough for real SVG files (<?xml?> becomes a comment), so
  there is no XML parser. Inline <svg> and SVG files share one
  renderer: rect, circle, ellipse, line, polyline, polygon, path (all
  commands, arcs), g/a with transforms, fill, stroke, opacities,
  fill-rule, currentColor, viewBox and preserveAspectRatio. Not yet:
  text, gradients and patterns (drawn gray), clipping, masks, <use>,
  CSS in <style>, stroke joins and caps (butt only).
- The polygon filler is analytic in x with 4 sub-scanlines per pixel:
  good anti-aliasing for the cost.
- Images load through the host's fetch callback when layout needs
  their intrinsic size, once per URL and document (failures cached
  too). The callback and its data must stay valid as long as the
  document is laid out and painted.
- Backgrounds: layers with url() images, linear-gradient and
  radial-gradient (circle from the center; shape and position are not
  read yet), background-size (cover, contain, lengths), -position
  (keywords, lengths, percentages), -repeat. The background shorthand
  keeps each layer's position, size and repeat. Images are scaled
  bilinearly; object-fit is honored.

## The browser (phase 7)

- `browser/soie` uses Xlib directly: no toolkit, a window, an XImage
  the page is painted into, cursors, keyboard and mouse. Its toolbar
  (back, forward, reload, address) is itself a small HTML page laid
  out and painted by Soie, hit-tested like any page.
- Networking (browser/net.c): HTTP/1.1 with Connection: close,
  redirects (10 at most; 303 and old 301/302 become GET), chunked
  bodies, gzip and deflate (zlib), 20 s timeouts, file:// URLs. TLS
  through GnuTLS with the system trust store, SNI, certificate and
  host name verification. One request at a time and the UI waits:
  simple first; parallel and progressive loading can come later.
- Sub-resources (style sheets, images, fonts) go through the engine's
  fetch callback and are cached per page.
- Forms: text-like inputs, password, textarea, checkbox, radio,
  select (a click cycles the options), submit buttons and Enter;
  GET and POST in application/x-www-form-urlencoded. No file upload,
  no multipart yet. The engine paints the controls (value, caret,
  placeholder, checks) and keeps the focus (soie_set_focus()).
- History: a list of URLs and scroll positions; same-document
  fragments scroll without reloading.
- `soie -o page.png URL` loads and renders without a display: the
  network and the engine can be tested headless.
- The engine gained soie_hit_test(), soie_element_rect() and
  soie_set_focus() for this. No cookies and no JavaScript: phase 9
  and 10 extensions.

## Flexbox and grid (phase 8)

- Flexbox follows the algorithm of CSS Flexbox 1 section 9: order,
  flex base size (flex-basis, then width/height, then max-content),
  the automatic minimum size (min-content), lines with flex-wrap,
  flexible lengths with the freeze loop for min/max violations, auto
  margins first, justify-content (start, end, center, space-*),
  align-items/align-self (stretch by default), align-content, gaps,
  reverse directions and columns. Baseline alignment is start.
- Grid: track lists with px, %, em, fr, auto, min/max-content,
  minmax(), fit-content(), repeat() including auto-fill and auto-fit
  (both behave as auto-fill: empty tracks are kept), line names and
  grid-template-areas; placement by line numbers (negative too),
  spans, names and areas; row (or column) flow auto placement without
  dense packing; implicit tracks from grid-auto-rows/columns; track
  sizing: fixed tracks, single-span content contributions, auto tracks
  growing toward max-content, fr sharing with inflexible tracks taken
  out; item alignment (justify/align-self and -items) and content
  alignment. Not yet: multi-span contributions, subgrid, masonry,
  baseline alignment, auto-fit collapsing, vw/vh in track sizes (taken
  as 1024x768).
- Text directly in a flex or grid container becomes an anonymous item,
  even when there are no block children.
- Real sites built on complex grids (Wikipedia's Vector skin, the
  Tailwind site) render largely right but not yet like browsers: the
  remaining gaps are in grid placement details and in var()-computed
  track lists.

## Character encoding

- The tokenizer only ever sees valid UTF-8 with normalized newlines.
  A decode step in front of it does BOM sniffing, then the
  transport charset given by the host (HTTP `Content-Type`), then a
  prescan of the first 1024 bytes for `<meta charset>` /
  `http-equiv`.
- Without any declaration, a page whose first 64 KiB are valid UTF-8
  is read as UTF-8, otherwise as windows-1252. The spec allows such
  detection; random windows-1252 text with accents is almost never
  valid UTF-8, and undeclared pages today (local files, sloppy
  servers) are mostly UTF-8.
- Built-in decoders: UTF-8, UTF-16LE/BE, windows-1252 (which also
  covers ISO-8859-1 and US-ASCII per the Encoding spec),
  ISO-8859-15, ISO-8859-2. Other labels fall back to windows-1252.
  Pages that are not UTF-8 are mostly old Western European ones;
  more single-byte tables are cheap to add, CJK multibyte decoders
  are not and wait for a real need.
- Invalid UTF-8 sequences become U+FFFD, following the Encoding
  spec's "maximal subpart" rule.

## Tests

- Tree construction is checked against the html5lib-tests corpus,
  pinned at commit 9329e64694e7835d0dcff9811e22856ef6ad16f9 (the
  last one before those tests moved to WPT). The corpus is fetched
  by `make check-html5lib` into `tests/html5lib-tests/` and is not
  committed. `make check` only runs our own small corpus and needs
  no network.
- `soie_node_dump()` (public, for hosts and tests) and
  `soie-render -d` print the DOM in the html5lib test format, so any
  page can be compared with another parser's output.
- `tests/html5lib-known` lists the expected failures of that corpus
  with the reason. Today: the four `<selectedcontent>` tests, which
  need form state (option selectedness), not parsing.
- libpng is used by soie-render (-o) and the paint tests now; the
  library itself needs it from phase 6 (image decoding).
- No valgrind or ASan on the SliTaz toolchain: leaks are checked with
  glibc `mtrace()` through an `LD_PRELOAD` shim, crashes with a random
  tag-soup fuzzer under `MALLOC_CHECK_=3`. Both live outside the repo
  for now.

## Build

- One Makefile, POSIX make, `cc -std=c99`. No configure script:
  dependency flags come from `pkg-config` when a phase needs them.
- The named character reference table `lib/entities.inc` is
  generated from the WHATWG `entities.json` by
  `tools/gen-entities.sh` and committed, so a build never needs the
  network.

## Choices made against the original brief

- CSS target is selectors level 3 plus what modern pages cannot do
  without: custom properties (`var()`), `calc()`, and the level 4
  `:is()`, `:where()`, `:not()` with lists. Without them most sites
  written after 2018 do not look like themselves.
- Inline layout needs text measurement before fonts exist (phase 3
  comes before phase 4). Layout talks to a small text-measurement
  interface; phase 3 ships a fixed-advance stub, phase 4 plugs in
  FreeType + HarfBuzz behind the same interface.
- The browser's TLS is GnuTLS, not BearSSL nor mbedTLS. BearSSL has
  had no release since 2018 and no TLS 1.3; SliTaz only packages
  mbedtls 2.6.0 (no TLS 1.3 either), while gnutls-dev is installed,
  maintained, speaks TLS 1.3 and reads the system CA store. TLS stays
  behind a small interface in the browser, so it can be swapped.
- HarfBuzz is optional (`make HARFBUZZ=0`): FreeType alone shapes
  Latin, Greek and Cyrillic (cmap, advances, legacy kerning); HarfBuzz
  adds GPOS kerning, ligatures and complex scripts. Default on.
