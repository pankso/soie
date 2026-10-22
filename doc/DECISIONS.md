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

- Nodes are malloc'ed one by one; a document owns its tree and
  `soie_node_free(document)` frees everything. A node removed from
  the tree is detached and becomes the caller's to free (or reinsert).
  An arena would be faster, but mutation from scripts needs
  individual frees.
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
- The browser's TLS is mbedTLS 3.6 LTS rather than BearSSL. BearSSL
  has had no release since 2018 and does not speak TLS 1.3; mbedTLS
  is maintained and has an LTS branch. The SliTaz wok only has
  mbedtls 2.6.0, so the receipt needs a bump when the browser lands.
