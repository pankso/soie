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
  extension point (image, content, net, cookies, script, media).
- An extension fills the struct and registers it with
  `soie_ext_register()`. Compiled in or loaded with `dlopen()`
  (`soie_ext_load()`, `soie_ext_load_dir()`: a module exports
  `soie_ext_init()`, which registers its hook sets); both paths go
  through the same call, so the core never knows which.
- Each struct starts with `abi` and `size`. The core refuses a major
  mismatch and reads hooks only within `size` (`SOIE_EXT_HOOK`), so a
  newer core can append hooks without breaking older extensions.
- Extensions are separate modules (`ext/*.so`, installed in
  `$(PREFIX)/lib/soie/ext`), not compiled into the browser: the
  browser binary then needs neither QuickJS nor FFmpeg, and a
  distribution can ship each extension as its own package. The cost
  is that hosts link libsoie as a shared library (a static copy plus
  the one the modules link would be two engines); the browser and
  soie-render do, with an `$ORIGIN/../lib` rpath that works in the
  tree and once installed. Hosts load `$SOIE_EXT_DIR` or the
  installed directory; `soie-render -x module.so` loads one.
- `ext.h` is 1.0 once phases 9 to 11 are in: script and media hooks
  arrive with their phases, then it only grows by appending.
- Who calls what: the engine calls image hooks (when its own decoders
  do not know the bytes) and, later, script and media hooks. The host
  calls content, net and cookie hooks, through helpers of the library
  (`soie_ext_document()`, `soie_ext_user_css()`, `soie_ext_blocked()`,
  `soie_ext_fetch()`, `soie_cookies_get()`): the library never fetches
  anything, it asks the host for bytes.
- Requests and responses that cross the boundary are `soie_request` /
  `soie_response` (method, headers, body, first party). The first
  party (the page's URL) is what cookie and blocking policies need.

### The extensions shipped (phase 9)

- `cookies`: RFC 6265 jar (Domain, Path, Expires, Max-Age, Secure,
  HttpOnly, `__Secure-`/`__Host-` prefixes, deletion, per-domain and
  total limits), kept in a Netscape `cookies.txt` (mode 0600) under
  `$XDG_DATA_HOME/soie/`, written when a persistent cookie changes;
  session cookies stay in memory.
  Third-party cookies are refused both ways: a request whose site
  differs from the page's gets no jar at all. Simpler than SameSite
  bookkeeping and what Safari and Brave do; it breaks a few
  cross-site logins, which is the price of not being tracked.
  Without the public suffix list (190 KB, needs updates), the site is
  guessed: the last two labels, three under a country code whose
  second level is a usual registry name (`co.uk`, `com.au`, `ne.jp`).
  Wrong for private suffixes like `blogspot.com`; never lets
  `Domain=co.uk` through.
- `block`: content blocking by host name from a hosts-style list
  (`~/.config/soie/block.txt`, the usual 0.0.0.0 lists work as is,
  a 100 000 entry list costs a few MB). A listed host blocks its
  subdomains. Only sub-resources are blocked, never the page asked
  for. No list, no hooks.
- `webp`: WebP through libwebp, the most common image format the
  engine does not decode itself. Animated WebP is refused for now.

## Public API

- Opaque types and accessor functions (`soie_node_parent()`, ...),
  never public structs. Struct layout can then change without an ABI
  break, which semantic versioning needs. The cost is a function call
  per access; acceptable for a host, and the engine itself uses the
  internal structs directly.
- Prefix `soie_` for functions, `SOIE_` for constants. Strings are
  UTF-8, NUL-terminated, owned by the library unless a function says
  otherwise.
- Version 1.0.0 since the eleven phases are in, soname `libsoie.so.1`
  (0.x before). From now on: new functions and appended hooks are
  minor versions, any change to an existing signature or struct a
  major one.
- A few structs are public because callers fill them: the option
  structs (`soie_html_options`, `soie_style_options`), requests and
  responses, events, the script host, media states. Their layouts are
  part of the 1.0 ABI; new options will come as new functions, not as
  new fields.

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

### Restyling

- `soie_style_document()` hashes what the rules come from (options,
  base URL, the `<style>`, `<link>` and `<base>` elements); when the
  hash is the same as last time, the parsed rules are kept and only
  the cascade runs again. Scripts change the tree all the time and
  rarely the style sheets: GitHub's restyle went from 0.65 to 0.41 s.
- Then only what changed is restyled. Every change marks elements:
  the changed one's subtree; siblings when the sheets have sibling
  selectors (`+`, `~`, `:first-child`, `:nth-*`, `:last-child`...);
  the parent for `:empty`; the ancestors for `:has()`. When such a
  selector part sits on the rule's subject (`li:first-child`,
  `.card:has(img)`), the marked element is restyled alone and its
  subtree only if its style changed: computed styles are interned, so
  that is a pointer comparison. When it sits further left
  (`li:first-child a`, `.wrap:has(.label) .input`), the subtree is
  restyled; for such a `:has()`, only the ancestors that match its
  compound (taken alone, without the `:has()`, which can only say yes
  too often) and their later siblings. Past 512 marks, everything is
  restyled. A class change on GitHub's home page restyles 46 elements
  instead of 1823: 0.05 s for style and layout instead of 0.43.
- Text changes only mark for `:empty` and `:has()`.
- Checked by a differential test (outside the tree): random class,
  style, insertion and removal changes on live pages (GitHub,
  Wikipedia, Bootstrap, Tailwind, Le Monde), each incremental result
  compared with a full restyle of the same document; no difference
  in 375 changes.
- Layout is still redone whole after any change (the box tree is
  rebuilt): 0.02 to 0.03 s on those pages, not worth more yet.
- Incremental layout, step 1 (October 2026, measured on YouTube: 540
  layouts forced by scripts in 90 s, 40 s of layout, 220000 text
  widths asked per layout): an element's intrinsic sizes
  (content_sizes(), min- and max-content) are computed once per
  layout, where nested flex and grid asked again at each level
  (Wikipedia's Cat page: 9.7 s to 0.25 s, its navboxes nest tables);
  the cache of text widths stays from one layout to the next, cleared
  with the rules it is keyed by (styledb_free()) and when fonts change.
  Same layouts on a corpus (Wikipedia, GitHub, Bootstrap, Le Monde,
  two YouTube DOMs, 1280 and 700 px wide) but one known bug: the
  static position of an absolute box in a table cell comes from the
  last of the table's trial layouts (width 0, then 1e7), not from the
  real one; Wikipedia's rotated navbox labels were wrong before and
  are wrong elsewhere after. Next steps: keep the box tree and lay out
  again only what a change reaches.
- Styles on YouTube after that: 315 full restyles in 90 s (17 s), all
  from a deep :has() at the root, `:has(>[role=dialog]:not([aria-
  hidden=true]) .ytwChannelBlocksViewModelHost) [dialog]`, seeing every
  class change. A change is now checked against what the :has()
  arguments can see: an attribute only through the compounds testing
  it (a class or an id through its old and new values, a state
  pseudo-class through the attributes it reads); an inserted or
  removed subtree only when it holds what the subject (rightmost)
  compound requires, since with descendant and child combinators an
  element's ancestors do not move; sibling combinators and positions
  are always seen. 72 full restyles left (2 s). It also showed an old
  bug the root mark hid: for `:has(+ .x)` and `:has(~ .x)` the anchor
  is a previous sibling, now looked for. Checked by a differential
  test (random class, attribute, insertion and removal changes on
  :has() rules, each compared with a full restyle: 24000 changes, no
  difference). A parsed style sheet cache was measured and not done:
  YouTube's 3.8 MB sheet parses in 0.1 s but weighs 56 MB parsed.
- `soie_document_update()` redoes cascade and layout with the last
  options and viewport (the library keeps a copy) when the document's
  change counter moved: the host calls it after scripts ran, and the
  script engine before answering layout questions (offsetWidth,
  getBoundingClientRect, getComputedStyle).

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
- Large images (over 256 KB of pixels) keep their encoded bytes and
  are decoded again when painted, within a 32 MB budget of decoded
  pixels: the least recently painted are dropped first. A page of
  photos then costs a screenful of them, not all of them (GitHub's
  home page went from 197 to 101 MB). The cost is a second decode of
  large images, once for their size and again on screen; reading
  sizes from headers would save the first one.

## The browser (phase 7)

- `browser/soie` (now xsoie, below) uses Xlib directly: no toolkit, a window, an XImage
  the page is painted into, cursors, keyboard and mouse. Its toolbar
  (back, forward, reload, address) is itself a small HTML page laid
  out and painted by Soie, hit-tested like any page.
- Networking (browser/net.c): HTTP/1.1 with persistent connections,
  redirects (10 at most; 303 and old 301/302 become GET), chunked
  bodies, gzip and deflate (zlib), 20 s timeouts, file:// URLs. TLS
  through GnuTLS with the system trust store, SNI, certificate and
  host name verification.
- Loading is in the background (browser/fetch.c): six threads run the
  requests, the page's own, its style sheets and scripts before its
  images; a pipe wakes the X loop when one finishes. The page on
  screen stays, and works, until the new one is laid out. Stages: the
  page's bytes; then everything it names (style sheets, scripts,
  module preloads, images) asked for at once; the first paint once
  the style sheets are in; the scripts once all of them are; images,
  fonts and late sheets as they come (relayout or restyle, at most
  every 200 ms). A "Loading (N)..." line shows what is on its way.
- For that the engine's fetch callback may answer SOIE_FETCH_LATER:
  the engine goes on without the bytes (no image, no sheet) and asks
  again at the next layout or restyle.
- Scripts are given their bytes, images too (the engine keeps them):
  the cache only keeps what a restyle needs again (style sheets).
- Measured against the one-request-at-a-time browser, rendering to
  PNG: Wikipedia 12.2 s to 3.7, Le Monde 38 to 15, GitHub 44 to 6.7
  (most of it was its 84 ES modules, fetched one by one by the module
  loader before; now preloaded). Same memory without scripts.
- Connections stay open: responses are read exactly (Content-Length,
  chunked, nothing for HEAD/204/304; otherwise until the server closes,
  and that connection is not kept), and idle connections wait in a
  pool of 16, by scheme, host and port, for 30 s. A pooled connection
  the server has dropped meanwhile (readable while idle, or no
  answer) is closed and the request goes once more on a new one.
  Saving a TCP and TLS handshake per resource: Le Monde 9.7 to 3.8 s,
  Wikipedia 2.4 to 1.7, GitHub 4.2 to 2.9 (without scripts); renders
  identical.
- Forms: text-like inputs, password, textarea, checkbox, radio,
  select (a click cycles the options), submit buttons and Enter;
  GET and POST in application/x-www-form-urlencoded. No file upload,
  no multipart yet. The engine paints the controls (value, caret,
  placeholder, checks) and keeps the focus (soie_set_focus()).
- History: a list of URLs and scroll positions; same-document
  fragments scroll without reloading.
- `<meta http-equiv=refresh>` is followed after its delay, replacing
  the page in the history (www.slitaz.org's home page is one).
- `xsoie -o page.png URL` loads and renders without a display: the
  network and the engine can be tested headless.
- The engine gained soie_hit_test(), soie_element_rect() and
  soie_set_focus() for this. Cookies and JavaScript are extensions
  (phases 9 and 10).
- Every request goes through the extensions first: net filters may
  cancel it, net fetch hooks may answer it (other URL schemes), the
  cookie jar gives the Cookie header and gets each Set-Cookie,
  redirect hops included.

## Shell and fronts: xsoie

- The browser is split in two: browser/shell.c keeps the page, its
  resources, history, forms, scripts, meta refresh, find and the
  offscreen PNG render, with no toolkit; a front shows it and feeds
  the input in (shell.h). The first front is `xsoie` (browser/xsoie.c,
  Xlib), so that a GTK 3 one (`gsoie`, GtkEntry address bar, real
  scroll bar) can come without copying a line of the shell. The
  engine and its files keep the Soie name (`~/.config/soie`, the
  extensions): both fronts share them.
- The shell talks to the front through three calls the front defines
  (ui_title, ui_address, ui_alert); the rest goes the other way:
  shell_tick() returns the poll timeout and says when to repaint,
  keys come in as DOM key values ("Enter", "ArrowLeft") and UTF-8
  text, not toolkit key codes.
- Keyboard input goes through the X input method (XIM,
  Xutf8LookupString): dead keys, compose and every character, not
  just Latin-1 (`€`, `œ` could not be typed before). No input method:
  XLookupString as before.
- Clipboard: Ctrl+C copies the address or the focused field's value
  (there is no text selection in the page yet), Ctrl+V pastes the
  CLIPBOARD, Shift+Insert and the middle button the PRIMARY selection.
  A focused address is PRIMARY, as a selected one would be. UTF-8 and
  Latin-1 targets, no INCR (pastes up to 256 KB).
- The address bar takes a URL, a path, a host (a dot, a port or
  localhost) or words: those are searched for in DuckDuckGo Lite, a
  page Soie renders well without scripts; `$SOIE_SEARCH`, a URL with
  `%s`, picks another engine.
- Find (Ctrl+F, F3, Enter, Shift+Enter, Escape): a find bar over the
  page's bottom left, the match highlighted and scrolled to. The engine
  gained soie_find() (libsoie 1.1.0, soname unchanged): the n-th match
  in the laid out text and its rect; ASCII case folded, a match does
  not span two elements or two lines.
- A thin scroll bar on the right when the page is taller than the
  window: the thumb drags, the track pages up and down.
- The middle button on a link, and Ctrl+N, open a new window: another
  xsoie process, not a tab. Each page has its own memory and a crash
  takes one window; tabs can come later in the shell.
- The window has a class (`xsoie`, `Xsoie`) for window managers and
  an icon painted by Soie.

## YouTube, the first round

YouTube showed its static skeleton only: its application never
started. In the order they stood in the way:

- `MessageChannel` and `MessagePort` were missing (its scheduler posts
  itself messages): one task per message, delivered once the port is
  started (onmessage, addEventListener or start()).
- YouTube forces the ShadyDOM polyfill, which wants an inert document
  (`createHTMLDocument`) and moves nodes from it to the page. Scripts
  can now make documents of their own (createHTMLDocument,
  createDocument, DOMParser): parsed by the engine, never rendered,
  their scripts never run, freed with the script context. The engine
  gained soie_node_adopt(): a subtree moves to another document (its
  list of nodes, its styles dropped, a media player stopped); an
  insertion from another document adopts first, as the DOM does.
  ownerDocument, importNode and adoptNode follow. A script element
  runs only once in the page's own tree, not in any document.
- `ProcessingInstruction` was missing (ShadyDOM patches it).
- QuickJS kept scripts within 256 KB of C stack (1 MB since 2025):
  40 calls of Polymer's big compiled functions, 1300 simple ones. Now
  4 MB, at most half the thread's stack: 5200 calls.
- Custom elements are constructed when they are made, as the spec's
  reactions do: by innerHTML, a clone or an import (not in a template's
  content nor another document), with attributeChangedCallback for
  their observed attributes, at construction and on every change, and
  disconnectedCallback when they leave the page. Before, an element a
  template stamped after define() had the class's prototype but its
  constructor never ran. The reactions wrap the natives that change
  the tree, so every path of dom.js goes through them. innerHTML and
  outerHTML insertions now call connectedCallback.
- The script time limit (5 s) counted the time spent waiting for the
  network in a request: it no longer does.
- The engine skipped floats and positioned boxes inside inline boxes
  when painting, hit testing and finding text: YouTube's consent dialog
  (position: fixed in an inline element) was never drawn, its backdrop
  greyed the page.
- SVG arc flags are one character each and may be written without
  separators (`a1 1 0 000 2`): YouTube's icons were black blobs.
- F12's page dump lists the computed values that most rendering bugs
  come down to (display, position, z-index, opacity, transform...).

- Requests from scripts no longer block the page: fetch() and
  asynchronous XHR go to a few threads of the script extension (4,
  shared by every page) that call the host's request function, still
  a blocking one; tick() hands the answers to the scripts, and asks to
  be called every 20 ms while requests are out (no new hook, ABI 1.0
  holds; the host's request must be thread-safe since 1.1). A page
  left abandons its requests: their threads free them when the
  network answers. A request's steps (CORS preflight, request) are a
  generator in dom.js, driven synchronously for synchronous XHR, in
  the background otherwise. The browser answers requests from other
  threads straight from the network, its page cache stays the main
  thread's. YouTube shows its application at 11 s instead of 18.
- SOIE_DEBUG=1 names what keeps the window from answering more than
  100 ms. On YouTube: its first scripts, 5.5 s of QuickJS, then timers
  that force restyles (0.1 to 1.3 s each) until 13 s; idle after.

Still to do there: the consent dialog's content is not stamped yet;
the time QuickJS spends in YouTube's scripts (profile it: DOM natives
or the interpreter?).

## YouTube, the second round: consent and search

- MutationObserver was a stub. Polymer and ShadyDOM use one, on a text
  node whose data they change, as their microtask: their templates
  (the consent dialog's content) were never stamped. Records are made
  around the natives that change the tree (insert, remove, attributes,
  data), delivered in a microtask, with subtree, old values and
  attribute filters; nothing is looked at while nobody observes.
- elementFromPoint() was a stub; now the engine's hit test.
- The hit test follows the paint order: positioned boxes are layers by
  z-index then tree order, over the flow, negative ones under it.
  Before, a later backdrop (z-index 2201) took the clicks of the
  dialog over it (2202).
- Stacking contexts, painting and hit testing: a positioned box with a
  z-index, fixed, or translucent orders its positioned descendants
  within itself. The flat model put YouTube's search box (relative, in
  a fixed z-index 2020 header) under the header: clicks missed it.
- visibility: hidden and pointer-events: none are not hit; descendants
  that set them back are. pointer-events is a keyword property now.
- A script's focus() moves the browser's focus: the engine keeps it
  (soie_get_focus(), libsoie 1.1), the shell follows after scripts.
  YouTube focuses its search input that way.
- Flex: a replaced box with a specified width kept its intrinsic one
  in a column; form controls (input, select, textarea) stretch across
  the line like boxes, images keep their size.
- The browser can run a debugging script: $SOIE_DEBUG_JS, Ctrl+Shift+J,
  in a block (its const are new at each run); SOIE_DEBUG=2 logs the
  scripts' requests.

- document.all exists, as HTML has it: an object that reads as
  undefined ([[IsHTMLDDA]], QuickJS's JS_SetIsHTMLDDA): typeof gives
  "undefined", if () false, == undefined true, === undefined false.
  polymer-resin, YouTube's binding sanitizer, leaves false values alone
  with `r || r === document.all ? sanitize(r) : r`: without document.all,
  undefined matched, came back as "zClosurez", a true string, and
  hidden="[[data.hideContents]]" hid every search result.
- $SOIE_DEBUG_JS_START: a script that runs first in the page (put at
  the start of its <head>), to watch the page's own scripts from the
  start (it caught the hidden = "zClosurez").

- calc() parsing freed a tree twice: calc_sum() and calc_product()
  freed out->tree on failure and left the pointer, calc(min(1px, 2%) + )
  then freed it again. YouTube's CSS has such values: a heap corruption
  glibc caught later, in a restyle or in another thread's TLS teardown
  (both crash reports came from it). On failure out owns nothing now;
  a unit test parses the cases.
- IntersectionObserver and ResizeObserver were stubs answering once.
  Now they observe: intersection with the viewport or a root grown by
  rootMargin, thresholds, isVisible; sizes. Looked at from the timers'
  tick when the page scrolled or changed, at most every 100 ms. Their
  entries have their fields on the prototype: the intersection-observer
  polyfill YouTube loads replaced ours without that, and its lazy
  thumbnails never loaded.
- Percentage heights of inline-level atomics (images, inline-blocks)
  resolve against the block container's height when it is definite:
  inline layout passed -1. YouTube's thumbnails were 1 px high.

- The rendering step of HTML's event loop: after the timers and their
  microtasks, at most every 16 ms, animation frame callbacks (asked in
  a frame: the next one), then resize observers (again while sizes
  move, 4 rounds), then intersection observers, a microtask
  checkpoint after each callback (drain()). Its own call from C after
  tick, and it asks to come again only for a frame callback waiting or
  a page that changed.
- overflow-x and overflow-y compute together (CSS Overflow 3): when one
  is neither visible nor clip, visible becomes auto and clip hidden.
  ytd-search has overflow-y: hidden alone: a scroll container, whose
  automatic minimum size in a flex line is 0. Without it its Shorts
  row (widths set by script) kept it 72 px wider than the window.
  Found by measuring the same frozen page in headless Firefox (the
  ESR in /opt/mozilla): same min-content, but overflow-x: auto there.

- transform moves boxes: translate(), translateX(), translateY(),
  translate3d() and the translation of matrix() and matrix3d(),
  percentages of the border box, as position: relative does (layout,
  so painting, hit testing and getBoundingClientRect agree). Scales
  and rotations are left out: the box keeps its size. YouTube's
  thumbnails center with top: 50%; transform: translateY(-50%).
- Negative z-index layers paint under the flow of their stacking
  context, after its root's background (CSS 2 Appendix E), as hit
  testing already had them: YouTube's masthead has a 90 % white
  background at z-index -1, painted over its logo before.
- soie_node_free() on a document freed its nodes first and stopped its
  scripts last: the script engine then walked the wrappers of freed
  nodes (soie-render, any host not calling soie_script_stop() first).
  Scripts stop first now. The tests run with MALLOC_PERTURB_: every JS
  test failed with it before the fix.

Now: the consent dialog shows and its buttons work, search suggests
and shows its results, thumbnails in place; the page matches Firefox
on the same frozen DOM.

## QuickJS 2026-06-04

- QuickJS 2026-06-04, the one release built and tested: no build path
  nor promise for others. YouTube's first scripts ran in 5.5 s with
  2024-01-13, 3.45 s with 2026-06-04 (its small block allocator and
  micro optimizations); it also brings WeakRef, FinalizationRegistry,
  iterator helpers, set methods, RegExp v, Promise.try, Float16Array.
- Its sources are GNU C (inline asm for Atomics.pause, `pause`: a nop
  before the Pentium 4, fine on i486): compiled with -std=gnu99, ours
  stay C99.
- check-js leaves out the stack trace lines: they name lines of dom.js
  and now columns, so every change of dom.js broke a reference.

## Resizing

- The viewport's size is not part of what the style rules are built
  from any more: every media query the rules were chosen by is kept
  with its answer, and another size keeps the rules (parsed sheets,
  fonts) when no answer changes; only the cascade runs again, all of
  it, for the viewport units. YouTube: 0.22 s to 0.01 s a resize
  within a breakpoint (its grid has one every 20 to 30 px, crossing
  one still rebuilds).
- A font or sheet a script's restyle asked for (soie_document_update(),
  outside the browser's relayout) was taken for an image: given to
  the engine, so asked again and fetched again at every restyle, and
  the rules, never complete, were rebuilt each time. The response's
  type now decides: CSS and fonts are copied, never given.
- xsoie keeps only the last of the ConfigureNotify events waiting: a
  window dragged bigger cost one relayout per event (40 for a short
  drag on YouTube, 10 s behind the mouse); now one per relayout time.

## Debugging the browser

- A fatal signal (SEGV, BUS, FPE, ILL, ABRT) writes a crash report,
  `~/.cache/soie/crash-PID.txt`: the page, the last 64 events, the
  backtrace, then gdb's backtrace of every thread with file and line,
  gdb attached to the dying process (`SOIE_NO_GDB=1` skips it). The
  handler runs on its own stack, so a stack overflow is reported too.
  Always on: a crash met while browsing should never be lost.
- `SOIE_DEBUG=1` prints loads, HTTP status, failed resources, the
  times of the first paint, layout, scripts and of the last resource,
  and script errors; 2 adds the input and console.log; 3 scrolling.
- F12 saves the page: its DOM after the scripts as HTML, and a .txt
  with the address, the trail, the DOM and box trees and the
  soie-render line that replays it.
- A browser not installed (no $(PREFIX)/lib/soie/ext) loads the
  extensions of the source tree next to it: `./xsoie` in browser/
  runs with scripts and cookies.

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

## JavaScript (phase 10)

- QuickJS, as the brief says: small, complete ES2023, embeddable,
  MIT. It lives in an extension (`ext/script.so`), linked with the
  installed static library (`/usr/lib/quickjs/libquickjs.a`, from
  quickjs-dev) or built from the release sources
  (`make QUICKJS_SRC=dir`). QuickJS ships no shared library, and its
  static one must be built with -fPIC to go into a module: SliTaz's
  quickjs receipt does so since Soie needs it.
- The library runs the scripts, the extension is the engine
  (`lib/script.c`, the script hooks of `ext.h`). The library walks the
  parsed document's `<script>` elements in order (classic, then defer,
  async and module ones), fetches external ones through the host,
  fires `load`/`error` on them, then DOMContentLoaded and load. So
  another engine would only implement start, stop, run, event and
  tick.
- Scripts run after the parse, not during it: no incremental parser,
  no blocking. `document.write()` during a script inserts after the
  running script (its scripts run), after loading it appends to
  `<body>`. Most pages that still write survive this; the ones that
  open a whole new document do not.
- Script-inserted scripts: inline ones run at insertion, external
  ones from a task, like async scripts. They do not delay load.
  Scripts in markup set through innerHTML never run (as specified).
- The DOM is written in JavaScript (`ext/script/dom.js`, embedded at
  build time) over a few dozen C natives on Soie nodes: tree walking,
  attributes, mutation, queries, serialization, layout, the host's
  services. JS is shorter for the web platform's many small rules
  (classList, dataset, URL, events, forms...), C keeps what must be
  fast (selector matching, serialization, textContent). Each node a
  script touches gets one wrapper with the node's prototype
  (HTMLAnchorElement, SVGElement...), kept until the page goes away:
  expandos and listeners survive, nothing dangles. The cost is that a
  page's wrappers live as long as it.
- Lists (childNodes, children, querySelectorAll, getElementsBy*) are
  arrays made on each access, not live collections: code that keeps
  one while changing the tree sees the old state. Simple, and rare.
- Events: capture, target and bubble phases, once, passive, signal,
  handleEvent objects, on* attributes and properties, preventDefault
  reported to the host so that it skips the default action (link,
  submit, checkbox). The host sends mouse (down, up, click), key
  (keydown, input), focus, blur, change and submit events.
- Timers, requestAnimationFrame (16 ms) and the like live in JS; the
  host asks `soie_script_tick()` for the delay to the next one and
  waits on its event loop meanwhile. Promise jobs run after every
  entry into JS.
- Networking for scripts is the host's blocking request: fetch() and
  asynchronous XHR do it from a task, so callers' code runs first as
  on the web; the UI waits during those requests (the page's own
  scripts come from the background fetches). Response bodies are bytes as a
  Latin-1 string, decoded to text by text()/json().
- Only what pages use most, the rest absent so that feature tests
  fail cleanly: no shadow DOM, iframes are empty, canvas has no
  context, MutationObserver never fires, IntersectionObserver reports
  every target visible once (what lazy loaders wait for), no service
  workers, localStorage lasts as long as the page, crypto has
  getRandomValues and randomUUID (from /dev/urandom) but no subtle.
  ES modules with import maps (imports, not scopes).
- Limits: 5 s of running per entry (then an exception), 256 MB of
  JavaScript heap per page. Errors go to the host (`SOIE_DEBUG=1`
  prints them in the browser).
- Tests: `make check-js` runs the pages of `tests/js` and compares
  their console output; `soie-render -j` runs a page's scripts.
- Cost: Wikipedia takes 0.3 s more CPU and 11 MB more with scripts.

## Audio and video (phase 11)

- The engine owns the `<video>`/`<audio>` side (`lib/player.c`):
  when to open a player, painting its frames (object-fit, default
  contain), drawing controls (play/pause, seek bar) for `controls`,
  clicks on them, the element's intrinsic size once the video's is
  known, and the media events to scripts (play, playing, pause,
  timeupdate, ended, loadedmetadata...). The extension only plays:
  open, play, pause, seek, volume, state, current frame.
- `ext/media.so` uses FFmpeg (libavformat, libavcodec, libswscale,
  libswresample), already packaged and able to read about anything,
  and ALSA directly for sound: no PulseAudio needed, SliTaz's
  default. One decoding thread per player, one sound thread; the
  sound card's clock leads when there is sound, the system's
  otherwise; the host's thread picks the frame due when it paints.
- A player opens when the element is first played (click, script),
  not when the page loads, and `autoplay` is ignored: a news page
  full of muted videos stays a page on an old machine. A poster, or
  a black box with a play button, shows meanwhile.
- Bytes come through the host's request function (a new
  `request` field in `soie_style_options`, called from the player's
  thread) by ranges of 1 MB: a long video is never all in memory.
  Servers without ranges give the whole file. So the browser's
  networking and the cookie jar became thread-safe (TLS set up once
  with pthread_once, the jar behind a mutex).
- Frames are converted to RGBA and scaled down to 1280x720 at most:
  four waiting frames and the one on screen cost under 20 MB.
- Streaming playlists (HLS, DASH, what most video sites serve)
  are refused: they need a segment loader and adaptive bitrate,
  another extension's job. Progressive files (mp4, webm, ogg, mp3,
  wav, flac...) play.
- The host calls `soie_media_tick()` in its event loop: it says when
  to repaint and fires the events. Since 1.2 `soie_media_tick_rect()`
  says when only the players' boxes changed (a new frame, the
  controls), with their rectangle: the browser paints and sends that
  rectangle alone (soie_paint() on a sub-rectangle of the view). A
  full repaint per frame of YouTube's page lost 65% of the frames of
  a 480p video; the rectangle loses 4%.
- `SOIE_AUDIO=none` plays without sound (tests, no sound card),
  `SOIE_AUDIO=hw:0` picks an ALSA device.

## Media Source Extensions

YouTube serves no progressive file any more: its player fetches
segments itself (SABR: a POST, an answer in UMP parts) and appends them
to SourceBuffers. Most video sites do the same through dash.js or
hls.js. So the streaming playlists refused in phase 11 come through
MSE, not through an HLS/DASH loader of our own.

- The media extension plays it (ext ABI 1.1, libsoie 1.2.0): new hooks
  appended to `soie_ext_media` (can_source, open_source, add_buffer,
  append, buffered, source) and `soie_media_source_*()` for the script
  engine, the element as handle. `MediaSource` exists in pages only
  when such an extension is loaded.
- As browsers do, a SourceBuffer keeps coded frames, not bytes: a
  parsing thread per SourceBuffer demuxes what is appended (FFmpeg,
  mp4 and webm, over a FIFO that waits for the next append), each
  frame stored by time with its codec parameters. `buffered`,
  `remove()`, overlapping appends (a quality switch rewrites a time
  range: the old frames and those that need them go), the append
  window and timestampOffset all work on these frames.
- A new initialization segment opens the demuxer again (FFmpeg's mp4
  demuxer skips a second moov); after `abort()` or bad bytes the last
  one is replayed before the next segment. The decoder opens again
  when a frame comes with other parameters: resolution switches work.
- appendBuffer() parses at once (demuxing is cheap) and the events
  follow in a task: `buffered` is right when updateend fires, and a
  full buffer is a QuotaExceededError at the call, as the spec wants.
- Memory: 24 MB of coded video and 4 MB of audio per SourceBuffer;
  past that, what played more than 3 s ago goes (coded frame
  eviction), then QuotaExceededError (players remove() and retry).
- Playing: the decoding thread takes the next frame in decode order
  from the video and the audio tracks. The clock waits (`waiting`)
  where nothing is buffered and goes on with half a second ahead or
  at the end of the stream; gaps under 0.5 s are jumped. `seeked`
  fires when the frames at the new time are there. With sound the
  clock is the card's, and it only goes on while sound is decoded: a
  track that cannot go (the frames wait to be shown, the sound to be
  played) lets the other one decode, else B-frames, late out of the
  decoder, filled the frames, stopped the decoding, the sound ran out
  and the clock with it (YouTube froze after 3 s, silent tests never
  showed it). The card's clock is read at each write (every 80 ms),
  the system's goes on in between: frames were dropped by threes.
- What a small machine plays well: H.264, VP8, AAC, Opus, Vorbis,
  FLAC, MP3, AC-3. VP9, AV1 and HEVC are refused (too heavy without
  hardware, and the sites that matter offer H.264), so are widths or
  heights over 1280, more pixels than 1280x720 and more than 30
  frames a second: `isTypeSupported()` says no and YouTube picks
  another format. `getVideoPlaybackQuality()` counts dropped frames,
  which players use to step down.
- Bodies are bytes: fetch, XHR and Blob gave binary data through a
  UTF-8 string, which broke every segment. Response bodies are now
  ArrayBuffers (no copy), request bodies may be bytes, UTF-8 coding is
  native (QuickJS), `response.body` is a ReadableStream (one chunk:
  the body is complete when it comes), blob: URLs from
  createObjectURL() answer fetch and XHR.
- Found on the way, from YouTube's load: every layout a script forced
  rebuilt all the style rules while a sheet or font was still on its
  way (YouTube's player forces hundreds); the URLs on their way are
  now asked again and the rules rebuilt only when one came. The
  connection pool could give a connection and keep it (a TLS session
  closed twice, with SABR's many connections). Media events waited
  for the next X event when their listeners set timers: media ticks
  now run before the timers.
- Tests: `tests/js/media-source.html` (fMP4 segments made by ffmpeg in
  `tests/media/mse/`): appends, a bad segment, buffered, waiting at
  the end of the data then playing again, endOfStream, ended,
  remove(), detaching. soie-render got `-T ms` for longer pages.
- YouTube, October 2026: the watch page plays through SABR, ads
  first, then the video, H.264 480p and 360p chosen by its own ABR
  (it stepped up from 240p), Opus sound, 0.65 of a core and about
  410 MB in all for the page. What stays slow: layouts a script
  forces are redone whole (layout is not incremental), and YouTube
  forces many when a player starts (10 to 25 s of scripts between an
  ad and the video), and QuickJS compiles base.js in O(n^2) of its
  variables (`find_var()`, linear). The 5 s limit per script entry
  stays: one of YouTube's ad callbacks hits it, the video plays.
- Not done: `changeType()` to another container, sequence mode
  (timestamps are always those of the segments), live seekable ranges
  beyond what is stored, EME (no DRM: such videos do not play),
  `playbackRate`.

## Security

An outside audit (Grok, GROK-AUDIT.md, October 2026) found holes,
checked one by one in the code and fixed; what came out of it:

- A web page reads no local file: file:// is for what the user opens
  and what local pages load (first party file://, or none), and a
  redirection only goes to http(s). Files must be regular files
  (not /dev/zero), 256 MB at most.
- Scripts' requests follow the web's rules: an Origin header off the
  page's origin, a preflight (OPTIONS) for what a form could not send,
  and a response from another origin is readable only when it allows
  it (Access-Control-Allow-Origin, -Credentials). credentials and
  withCredentials are honoured: soie_request gained flags
  (SOIE_REQ_NO_COOKIES). That is a change to the 1.0 ABI, made
  because 1.0 was never published.
- A page cannot shape the request: methods and header names are
  tokens (RFC 9110), forbidden headers (Host, lengths, encodings,
  Connection, Cookie, Origin, Proxy-*, Sec-*...) are dropped twice
  (in the script engine and in net.c), header values lose control
  bytes, and the request target is percent-encoded (no CR LF from a
  pasted address either). A redirection to another origin drops the
  caller's headers (Authorization).
- Responses are framed strictly before a connection is pooled again:
  headers come from the header block only, one Content-Length (all
  digits, all the same), Transfer-Encoding with chunked last (and the
  connection closed when both are present), chunk sizes bounded and
  their CRLF checked, Connection read as a token list; any byte past
  the frame, or any doubt, and the connection is not reused. Gzip
  grows to 64 MB at most.
- The cookie jar refuses control bytes and tabs in every field (a
  Path could write a second line in cookies.txt), checks the file's
  lines again when loading it, and knows the shared hosting suffixes
  (github.io, blogspot.com, netlify.app...) as sites of their own.
- Audio and video requests carry the page as first party (they went
  out with the target's cookies, past the block list): player.c wraps
  the host's request function.
- GIF frames are bounded (16384) and indexed in size_t; LZW tables
  are on the stack. WebP gets the same 16384 limit, checked before
  decoding. A video's size is compared in 64 bits. A media range
  must start where it was asked.
- Hardening: buffers abort rather than overflow, SVG edges with NaN,
  infinities or absurd coordinates are dropped, soie_node_free() only
  detaches a subtree a script still holds (the document frees it),
  the block list ignores a host's trailing dot.
- Checked: a hostile local server (a forged response after a 302 on a
  kept connection, Transfer-Encoding in the body, an overflowing chunk
  size, a gzip bomb, CR LF in a URL) and a hostile page (file://,
  header and method injection, cross-origin reads, credentials: omit).
- Not done, known: no HSTS, and an https page may go to http on a
  redirection; no SameSite (third-party cookies are refused instead);
  requests to the local network from public pages are not blocked
  (their answers are unreadable without CORS).
- The audit's frame race in the media player is not one: the frame on
  screen is never in the decoder's queue, only the interface's thread
  swaps them.

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
