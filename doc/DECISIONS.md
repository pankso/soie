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
- Version 0.1.0, soname `libsoie.so.0`, until the first public
  release is ready (see "Version and repository"). From 1.0 on: new
  functions and appended hooks are minor versions, any change to an
  existing signature or struct a major one.
- A few structs are public because callers fill them: the option
  structs (`soie_html_options`, `soie_style_options`), requests and
  responses, events, the script host, media states. Their layouts are
  part of the ABI once 1.0 is out; new options will then come as new
  functions, not as new fields.

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
- Shadow DOM (2026-11-10; before, attachShadow() returned a fragment
  that was never rendered). A shadow root is a document fragment of
  the host's document, `u.el.shadow` on the host, `u.frag.host` on
  the root (not `u.el.content`: clone, serialize and dump take that
  for a template). Public: soie_attach_shadow(), soie_shadow_root(),
  soie_shadow_host(), soie_shadow_open() (the mode lives in C, so a
  closed root stays closed whatever happens to its wrapper) and
  soie_shadow_css() (adopted sheets as one text), soie_assigned_slot()
  (the shell's and the events' way up). The root goes with
  its host: freed (a root a script holds survives, without host),
  adopted, never cloned nor serialized; adopting it alone fails.
  node_above() (parent, else host) is "the node above" for connected
  (lib/cascade.c, lib/script.c, the extension), the restyle marks'
  depth and cover, the insertion cycle check and the wrappers' tree.
  soie_attach_shadow() checks the host itself (shadow_host_ok(), as
  attachShadow()): an embedder's root on a <details> or an <input>
  was hidden by their own boxes. soie_node_free() of a root that
  still has its host does nothing (the host's boxes, kept, pointed to
  freed nodes).
- Shadow nesting has no bound: every walk that enters shadow trees
  iterates (walk_start()/walk_next() in lib/dom.c, a stack of scopes
  on the heap; next_deep() in the extension), none recurses. A cap
  would have to be checked at each insertion too (a host moved into
  another tree nests both), a walk of the subtree each time. Before,
  100,000 nested declarative roots overflowed the stack (74,750
  frames of tree_signature()), and held() walked the whole chain at
  each level: 20,000 levels took 3 s, 0.08 s now, 100,000 0.43 s.
- Flat tree: gen_children() of a host walks its root's children; of a
  slot, the host's children of its name (first slot of that name in
  tree order only), else its own (fallback). flat_parent() is the
  parent for inheritance and for layout_touch(): a slotted node's
  is its slot, so a change below it invalidates the kept boxes of
  the slot's ancestors. Assignment is computed, not stored, but each
  root keeps the list of its slots (slots_of(): a light child finds
  its slot among a few, not in a walk of the whole shadow tree: the
  style of a host of 1000 children and 6000 shadow nodes took 98 ms,
  24 now); a change of slots (insert, remove, name) drops that list,
  touches every slot and restyles the host's children. node_detach()
  looks at slots once the node is out: the list must not keep it.
  docinfo.shadows (roots made) keeps all of this off pages without.
- color-scheme is an inherited property that only light-dark()
  reads: "dark" alone is dark, "light dark" is what the user prefers
  (prefers-color-scheme), anything else light. compute_style() computes
  it first and sets color_dark for the element's other values; a
  declaration holding light-dark() is kept pending like one with
  var(), since its color is the element's to choose, but it is parsed
  once when the sheet is: an invalid one is dropped there (the
  declaration before it stays, @supports says no). color_dark is
  thread-local (workers parse colors), set and cleared only for a
  dark element: light pages pay no access per element.
- System colors (Canvas, CanvasText, LinkText, Field, ButtonFace...)
  follow color-scheme too, with Firefox's dark values: a sheet parses
  them once, light, and keeps their index in struct color (sys); the
  cascade gives a dark element the dark one. They resolve at computed
  time, as in Firefox: a dark section of a light page keeps the black
  text it inherits, its links, buttons and fields turn dark. The
  initial color is CanvasText and the UA sheet's links LinkText, so a
  page dark by color-scheme alone gets light text; the canvas is
  Canvas in the root's scheme (style_dark()). `<meta
  name=color-scheme>` is a presentational hint of the root, which a
  :root rule overrides; a meta added later by a script does not
  restyle the root. Colors inside shadows and gradients stay light
  (parsed at paint time), and so do the checkboxes, radios and ranges
  forms.c draws itself.
- `:defined`: a node flag set by the script extension when a custom
  element is upgraded or constructed (soie_element_set_defined()); an
  HTML element with a hyphen in its name does not match before, any
  other element always does. Without the script extension no custom
  element is ever defined, as in a browser without scripts: a page
  that hides undefined components hides them there too.
- ElementInternals keeps what the element gives it (states, validity,
  form value); a form does not take the value. Its states are a
  CustomStateSet (a Set) that gives the engine the names as one string
  at each change (soie_element_set_states()): few elements have
  states, so a list in the document holds them and a byte of the node
  says whether to look (`:state()` costs nothing to the others, and
  no node grew a pointer). `:--x` is the state "--x", as Chrome had
  it first.
- import.meta.url is set on a module after it is compiled and before
  it runs (module_meta()): the script's address, the page's for an
  inline module.
- Style order: host, its shadow tree, its light children (they need
  their slot's style). A rule keeps the shadow root of its sheet as
  scope (a pointer only compared: the root may be gone before the
  rules). In collect() a rule of el's own tree goes through the old
  path; others (collect_other()) only for UA and user rules, a
  shadow tree's :host rules on its host, ::slotted() rules on the
  host's children, matched from their slot. Those crossing rules
  lose to the outer tree's normal declarations and win with
  !important (CSS Cascade 5, context: see tree context below).
  The selector matcher knows the rules' tree (selector_shadow()):
  :host is its host, combinators step from the tree's top to the host
  only for a compound with :host. The thread-local is read only at
  the top of a fragment, never per compound: callgrind on a page of
  3000 elements and 1500 rules without shadow roots counts 0.1% more
  instructions than before (a first version, with a check per
  compound, 3%).
  Shadow roots are styled per tree scope, layers too: a shadow
  tree's sheets have their own @layer tree (struct scoped, by rule
  scope), so a component's `@layer two, one` does not reorder the
  page's (they were looked up by name only). A root's <style>
  elements and its adopted sheets order their layers apart (two
  scopes): rare, not done. The :host compound has no ancestors nor
  siblings for its tree's rules (CSS Scoping 3.2): `.dark :host`,
  `body :host span`, `.a + :host` never match (match_rec() hands
  over to the host for the first compound only; selector_has_host()
  wants :host alone). `:host::before` is the host's ::before (it was
  applied to the host itself).
  Adopted sheets are shared by text: soie_shadow_css() keeps one
  struct shadow_css per distinct text in the document (docinfo.css,
  counted), the root points to it, its rules are loaded once after
  every tree's sheets with that struct as scope, and an element of a
  root takes the rules of its root's text too (of_root()). Every Lit
  instance of a component adopts the same sheet: 300 instances parsed
  their CSS 300 times and kept 300 copies of the rules (95 MB, 290 ms;
  each added between two styles: over 5 s, quadratic); now once (11
  MB, 23 ms; 96 ms one by one). sheet_signature() hashes the distinct
  texts, not the roots: a new instance keeps the parsed rules, a root
  whose text changes restyles its host's subtree. A shadow tree whose
  sheets are plain `<style>` elements (no media, no <link>) shares
  them the same way: tree_signature() keeps their texts, NUL ended,
  in a struct shadow_css of the document's `sheets` list
  (share_sheets()), loaded once (shadow_sheets()). Server-side
  rendered pages repeat the same <style> in every instance: 4000
  components kept 4000 copies of the rules, each element looked at
  every copy (collect_other(): 5.1 G of 9.7 G instructions), and a
  new instance rebuilt them all; 0.69 s to lay out, 0.14 s now (0.12
  s for the same page without shadow trees). collect() skips another
  tree's author rule at once unless el is a host or a host's child
  (a page of many different components).
  `:host` counts inside `:is()` and `:where()` (has_host()).
- Tree context is a field of the cascade key above the style
  attribute (CSS Cascade 5, 6.1: context comes before element-attached
  styles), two bits taken from the layer rank (4095 layers, was
  16383): an inner tree's rule matched outside it (:host, ::slotted())
  is under the tree's own rules, an outer tree's ::part() rule over
  them, one from further out (exportparts) over that; !important
  turns it upside down. Before, the crossing rules took layer rank 0,
  under the style attribute: the host's style="x !important" beat
  the component's :host { x !important }, wrong.
- `::part()`: its rules go to a bucket of their own (by their first
  name), not the host's buckets, and are matched from the part: in
  collect(), only for an element of a shadow tree with a part
  attribute when the rules have any (pages without shadow roots pay
  one NULL test). collect_parts() walks out: in each tree it matches
  the compound before ::part() from the host (that tree's rules), and
  `:host::part()` of the tree within (the component styling its own
  parts: the part's own context, as its other rules); then
  exportparts maps the names to the next host's. What follows
  ::part() (`:hover`, `:state()`) is matched on the part,
  `::part(x)::before` is the part's ::before. A ::part() rule's
  subject compound is the host, so its dependencies count as deep
  for incremental restyling (sellist_flags()).
- Script side: the wrappers of a shadow tree belong to the tree of
  its host (rehome() climbs through hosts, strengthen() enters
  roots), so expandos on a root and its nodes live as long as the
  host's tree. Adopted constructed sheets are kept in step through
  WeakRefs of the roots that adopted them (a sheet shared by every
  instance must not hold them): one WeakRef per root in a Set per
  sheet, dead ones dropped when the sheet changes (a filter of the
  whole list at each adoption: 2000 roots of one sheet, 0.45 s).
  adoptedStyleSheets is one object per root and for the document,
  changed in place. customElements: define(), upgrades and
  connected/disconnected callbacks see shadow trees (api.shadows()).
  attachShadow() takes the spec's elements only (custom elements,
  div, span, p, section...): NotSupportedError for the others. On a
  host with a declarative root of that mode it empties the root and
  returns it, once (soie_shadow_claim(): a flag of the root the
  parser sets), as browsers: server-side rendered components call
  attachShadow() in their constructor. delegatesFocus, clonable and
  serializable are kept per root; a host's focus() goes to the first
  focusable element of its tree ([autofocus] first).
- Closed roots: no property a page can read or set may lead into a
  closed root or change what a component's own objects do. dom.js
  keeps what it knows of roots, sheets and events in module WeakMaps
  (evPaths, rootInit, rootSheets, sheetRoots, hasInternals) and
  private fields (#el of ElementInternals and CustomStateSet), never
  in `_x` properties: `ev._path`, `host._internals`, `sheet._roots`
  gave any page the closed root (Lit exposes its sheets), and a page
  that set `x._internals = {}` first broke the component.
  ElementInternals and CustomStateSet throw "Illegal constructor"
  for pages (a module token). Not a security boundary in browsers
  either, but embedded widgets count on it.
- Events (dom.js dispatch(), DOM 2.5 and 4.8): the path goes up
  through api.up() (the slot of a slotted node, else the parent,
  the host of a shadow root), from a root to its host only for a
  composed event or one that started below that tree; the target
  changes to the host on the way out (that item is "at target", a
  host's listeners run for a focus event), listeners see it
  retargeted, relatedTarget too (an event of a move inside one host
  stops at it); after dispatch target is null if still inside a
  tree. composedPath() is the spec's algorithm on per item flags
  (closed root, slot of a closed tree). A page without shadow roots
  builds the same arrays as before: the per item work starts at the
  first boundary. The host's UI events (click, keys, focus, input,
  pointer, wheel, touch, composition) and focus()/click() are
  composed. At target, capturing listeners now run first, as in
  browsers since 2021. slotchange: the mutation natives (insert,
  remove, attributes) note the hosts whose slots may change, once a
  ShadowRoot was ever wrapped (before, nobody can listen); a microtask
  compares each slot's assigned nodes with the last look (and a slot
  whose fallback changed) and fires. document.activeElement and
  elementFromPoint() retarget to the document, ShadowRoot.activeElement
  to its root.
- Shell: ancestor() (links, buttons, summary, media around a click or
  hover) follows the event's way (soie_assigned_slot(), parent, host);
  the form of a control is looked for in its own tree only (form_of()),
  as is a radio group without form. Focus and text editing need
  nothing: the focused node is any element.
- Element.removeAttributeNode(), and attributes.removeNamedItem():
  the map is a copy here, so the node taken out leaves the map it
  came from too. React empties html, head and body with "while
  (attrs.length) el.removeAttributeNode(attrs[0])", which never ends
  on a copy that stays full.
- Declarative shadow DOM (HTML 13.2.6.4.4): a `<template
  shadowrootmode=open|closed>` start tag of a document's parse, when
  the current node may host a shadow root (shadow_host_ok()) and has
  none, attaches one; the template goes on the stack only, its
  content goes to the root (the parser's template/root pairs,
  template_content()) and it is freed as an orphan. Fragment parsing
  (innerHTML) keeps a plain template, as the spec says. Scripts in
  such a tree do not run (soie_script_start() does not enter shadow
  trees). The html5lib corpus has no such test; tests/tree/shadow01.dat
  prints the root as `#shadow-root` before the host's children.

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
- `revert-layer` behaves like `revert` (see below). `@container`,
  `@scope`, `@page` and `@counter-style` are ignored.
  `@font-face` blocks are stored for phase 4.
- Animations do not run, but an element is shown as its animations
  leave it: `@keyframes` rules keep the declarations of their first
  and last keyframes, and an animation that ends and fills forwards
  (or both) puts its last keyframe in the cascade (the first when it
  ran backwards), over the author's declarations and under the
  important ones (animated() in cascade.c; animation-name,
  -iteration-count, -direction and -fill-mode are properties, the
  shorthand drops the rest). `opacity: 0; animation: fade-in .3s
  forwards`, the usual way to bring a page's content in, left it
  invisible. Infinite animations and those that do not fill leave the
  style as it is. Scripts get the events all the same: an element
  that gets an animation-name it did not have is marked when it is
  styled (soie_animations(), a public function added for the script
  extension), and dom.js sends animationstart, then animationend
  unless the animation loops for ever, a moment after the document
  changed. Pages wait for animationend to remove a loader or the
  class that hides their content.
- `revert` goes back to the user agent's (or the user's) declaration:
  the cascade keeps the winner among those beside the overall one. It
  was read as unset: `div { display: revert }` was inline, `all:
  revert` took the defaults of headings and lists away.
- Selectors: level 3, plus `:is()`, `:where()`, `:not()` with lists,
  `:has()` and `:nth-child(An+B of S)`. `:host`, `:host()`,
  `:host-context()` and `::slotted()` match in shadow trees (see
  HTML parsing, shadow DOM); `::part()` parses but never matches:
  Tailwind 4 puts its theme on `:root, :host`, and an unknown
  pseudo-class would drop the whole rule. Dynamic states (`:hover`, `:focus`...)
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
- Incremental layout, steps 2 and 3: boxes live from one layout to
  the next. Each node has a stamp (lchg) moved by layout_touch() on
  what changes the boxes it gives: an attribute, text, an insertion
  or a removal (the parent), a new computed style (styles are
  interned: another pointer is another style), a video's size; the
  stamp goes up to the root and stops at an ancestor already stamped
  since the last layout. Building the new tree, an element whose
  subtree has no stamp since keeps its box with everything below,
  moved out of the old tree, when that box makes no part of what is
  around it: block-level or atomic (its content starts lines of its
  own), not a table part, not a list item (numbers count the ones
  before), with images and videos of the same sizes. New rules (a
  sheet came) keep nothing: boxes point to their styles.
- A box laid out alone (floats, inline-blocks, flex and grid items,
  absolute boxes, table cells: layout_alone()) again with the same
  width and containing block height only moves (translate()). The
  absolute boxes it held are laid out again, against their containing
  block as it is now (a flex item may be stretched after). Content
  moved after its box was laid out (a cell's vertical-align) loses
  the cache.
- Old bugs this found, fixed: an absolute box was laid out once for
  each layout of its ancestors, each time from the last place (it
  drifted: -1 px a time on Bootstrap, Wikipedia's rotated navbox
  labels), now once per layout from its static position, kept apart
  (sx, sy, moved with its ancestors, not when it places itself); a
  table's trial layouts (its intrinsic widths) wrote over its real
  geometry and its absolute boxes' static positions: they are
  measured once now, the positions put back, what holds the table
  laid out again.
- Checked by a differential test (outside the tree): 60 random
  changes (classes, style, hidden, insertions, removals, text) on six
  pages (Bootstrap, Wikipedia, Le Monde, GitHub, two YouTube DOMs),
  four seeds each, every incremental layout compared with a fresh
  layout of a copy: no difference over 0.25 px. Sub-pixel ones
  remain: text placement depends on the absolute position by a few
  hundredths, and a table's fresh layout puts a cell's content 0.17 px
  from its padding. The updates take 0.1 to 3 s where fresh layouts
  take 7 to 68 s. On YouTube, no script task over 2.5 s left (5 to
  25 s before), no script interrupted: the video starts as soon as
  the ad is skipped.
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
  instead of moving down with it; sticky is static; vertical margins of
  floats do not collapse (as specified), but clearance is computed
  the simple way (move below the floats).
- Box tree depth is capped at 512 levels (layout is recursive).
  Deeper content is kept as plain text flowing in the box at the
  limit, collected without recursion. Blink caps its DOM at 512 too.
- An absolute box with top and bottom and an auto height is as high
  as they leave: that height is definite for its children's
  percentages (CSS Sizing 3), as browsers do. It was set after its
  contents were laid out, so a height: 100% inside it was auto.
  YouTube's player sits in such a box (#player-container, top 0 and
  bottom 0 in a padding-top: 56.25% ratio box): everything under it
  that is 100% high was 0 px, its ad layer drew the skip button above
  the video, out of sight, and its controls, spinner and ad card fell
  out of place as well (and the player came as a 153 px strip from
  the search page). The same for a stretched flex item (CSS Flexbox
  9.8): laid out again at its stretched height when the container's
  height is definite, single line, and the height changed, so that
  not every row of cards is laid out twice. Checked against Firefox.
- A padding below 0 (a calc() result: lwn.net's
  calc(50% * 0 - 65em / 2)) is 0; the column was drawn 352 px to the
  left, under the sidebar.
- A replaced box with a percentage width or max-width is compressible
  (CSS Sizing 3): its min-content contribution is 0. debian.org's
  icons, width="512" and max-width: 96%, made each column of its
  flex-wrap rows 512 px at least: the two columns one under the other.
- text-transform maps ASCII and the letters that are two bytes in both
  cases (Latin-1, Latin Extended-A, Greek, Cyrillic), in place: the É
  of "LA COMMUNAUTÉ" stayed é. ß stays, its capital being two letters.
- aspect-ratio (CSS Sizing 4, 5.1) for every box, not only the
  natural ratio of replaced ones: ratio_of() in block.c gives the
  preferred ratio (the property, else the natural one; `auto 16 / 9`
  is the natural one when there is one), ratio_w() and ratio_h() one
  content size from the other, on the border box when box-sizing says
  so and the ratio is the property's. An automatic height comes from
  the used width, and is definite for the children; an automatic width
  comes from a definite height wherever the width would be shrink to
  fit, and for a block in the flow too (not stretched, as browsers).
  The box still grows with its content (the automatic minimum size)
  unless it sets min-height or clips. A grid item with a ratio is not
  stretched in its row. Not done: min and max sizes transferred
  through the ratio. Tailwind's aspect-video cards, video embeds and
  thumbnails were 0 px high.
- Absolute boxes (CSS 2 10.3.7, 10.6.4): with left, right and a
  width (or a max-width that stops the fill), the automatic margins
  take what is left, both the same; the same for top, bottom and a
  height. `inset: 0; margin: auto`, the way modals and spinners are
  centred, left them in the corner. fit-content, min-content and
  max-content widths are not the fill between left and right. A
  positioned inline box is a containing block: the padding box around
  its pieces on the lines (inline_rect()), so a badge or a tooltip in
  a `position: relative` span sits on the span, not on the page.
- Line breaking (inline.c) knows more than spaces: a line may break
  after a hyphen in a word (as Firefox does in running text: three
  characters before it at least, two after, never before a digit, so
  that dates, version numbers and CVE-2026-1234 stay whole), after a
  slash that is not a fraction's,
  after ? ! | and dashes, at a zero width space, and between
  ideographs, kana and hangul except before closing punctuation and
  after opening brackets (break_between(), measured against Firefox;
  not the whole of UAX 14). Chinese and Japanese pages were one long
  line, a long URL never wrapped. Soft hyphens are dropped: without
  hyphenation they never show, and fonts drew them.
- A break needs an opportunity: the text of two inline boxes that
  touch is one word (`<b>W</b>ord`, a comma after a link), and
  `white-space: nowrap` only stops the breaks inside its own text. The
  line remembers the last place where it may break (snapshot()); when
  what follows cannot break and does not fit, it goes back there
  (rewind_line()) and the layout resumes from that item. Before, a
  wrapping text could break before any of its words, even glued to
  the previous box, a nowrap run could not move to the next line and
  an inline-block wrapped inside a nowrap parent (the tabs of a
  horizontal scroller one under the other). The min-content width
  counts such runs whole as well. Not across floats: they are placed
  once.
- A preserved tab is spaces up to the next stop of 8 columns (it was
  drawn as a missing glyph). Floats waiting for the next line are no
  longer 32 at most (the 33rd was never laid out).
- Then a tab stays a tab: a fragment of its own, as wide as the way to
  the next tab stop, measured from the container's content edge (CSS
  Text 3, 4.2): tab-size spaces (each with letter- and word-spacing)
  or a length, the following stop when the next one is closer than
  0.5ch, no room at tab-size 0 (tab_width()). Spaces counted columns
  from the start of each text node: a tab after a `<b>` or in a
  proportional font missed the stop. Not drawn, never shaped; a line
  breaks after it, not before. tab-size is a K_LH value (a number or
  a length, no percentage).
- min-content, max-content and fit-content are widths of floats and
  inline-blocks too (keyword_width()), not only of blocks.
- Flex items of row-reverse and column-reverse are placed from the
  other edge of the line: only their order was reversed, the row
  stayed at the left.
- Floats in a row add up in their container's max-content width,
  until one clears them: two floated columns and a clearfix in a
  shrink-to-fit box were one under the other. Not with the blocks
  beside them, as Firefox. A box that starts a formatting context
  (overflow, a table, a replaced box) and does not fit beside a float
  goes below it (CSS 2 9.5): it overflowed. Floats get their
  containing block's height: percentage heights in them resolve.
- Grid, from the same review: the container's definite height is the
  one it is given (it was resolved a second time against itself:
  height: 50% sized the rows for a quarter; a height from top and
  bottom was ignored), and an automatic height within min-height and
  max-height sizes the rows for the one that holds (min-height:
  100vh with rows auto 1fr auto: the footer at the bottom). Items
  over several tracks contribute to them (span_contribution(): what
  they exceed is shared by the tracks sized by content; a tall item
  spanning two rows overflowed a 40 px grid). A replaced item takes
  its CSS size. calc(100% - 200px) as a track size keeps its length
  (only the percentage was read), and vw, vh and rem in tracks are
  the real ones (doc_units(): they were 1024, 768 and 16 px).
- Tables: a cell's height is a minimum for its row, not the cell's
  height (six lines in a cell of height: 20px ran over the next row),
  and vertical-align works in it; the table's height is a minimum
  shared by the rows; `<col>` and `<colgroup>` widths size the
  columns, in the automatic layout as a cell's width would, in the
  fixed layout before the first row (their boxes were thrown away).
- CSS counters (CSS Lists 3): counter-reset, counter-increment and
  counter-set, counter() and counters() in content with the decimal,
  alphabetic and roman styles. The counters live in a stack while the
  box tree is built (boxes.c): one made by an element is seen by it,
  its descendants and its later siblings, until its parent ends; a
  pseudo-element counts as a child. Numbered headings, nested 1.2.3
  lists and footnotes had no number. A layout that used counters
  keeps no box for the next one (layout.counters): a number may
  depend on any element before. List markers still count by
  themselves (ordinal(): start, value, reversed), not through the
  implicit list-item counter: an author's counter-reset on an ol does
  not stop them as it does in browsers.
- Inline boxes with position: relative are offset: once a line is
  placed, each of its pieces moves by the offsets of the relative
  inline boxes around it (finish_line()); the line itself does not
  change. python.org's Donate link (inline, top: 38px) sat 38 px too
  high, over the logo row.
- Nested flex containers were laid out in exponential time: a
  column's item is measured without a height, then laid out at that
  very height as a definite one, a different layout for the cache of
  layout_alone(), so each level did its content twice (22 levels: 4.5
  s). A layout made without a height from outside now also answers
  for one made with a height that is the height it has, and the
  reverse for a plain block container, when nothing in it depended on
  whether a height is definite (lsame): no percentage height, flex
  basis or grid row, no fr row, no growing item of a column
  (box_pct_seen(), counted while the box is laid out). A flex or grid
  container given a height reports that height as its content's, so
  its layout with one never answers for one without. 22 levels: 4 ms.
  Checked by the differential test of incremental layout (28 runs of
  60 random changes on 7 site copies, no difference; it caught two
  wrong versions of this rule) and by the layouts of 11 site copies
  at two widths, the same with the rule on and off.
- A float that waits for the next line holds the floats after it
  there too: none is higher than one before it in the source (CSS 2
  9.5.1, rule 5). docs.python.org's theme menu, a float after its
  search box's, went up to the first line alone. What a replaced box
  gives its container (box_sizes()) is within its min-width and
  max-width; its content size is not (a flex base size ignores them).
- A transformed box is the containing block of its absolute and fixed
  descendants (CSS Transforms 1, 3), and a stacking context when it is
  painted through its matrix: its negative layers and its background
  belong to its own canvas (they were painted a second time upright).
- A second review of this whole change set (another model, read
  only) found eight defects, all fixed: the two above; the scan to
  the end of the word at each hyphen (quadratic on 40000 `abc-`); a
  rule's declarations cut at every semicolon in the CSSOM (a data:
  URL lost its end), and changes to a rule inside @media not written
  back; the page's box-shadow dropped on controls drawn with the thin
  ring; -webkit-text-fill-color ignored for colors starting with a c;
  a counter increment that overflows is now not done, as specified;
  the `&` memo is only used while match_from() runs.
- Bounds found by a review: grid lines are clamped to 10000 as in
  Firefox (the occupancy grid is rows x columns; line 50000 on both
  axes overflowed its index); display: contents counts as a level of
  the box tree's depth limit (200000 nested ones overflowed the
  stack); roman list markers stop at 3999, decimal beyond.

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
  during the cascade and opened from memory. WOFF2 (most web fonts
  today) is FreeType's work: it reads it when it was built with
  brotli (SliTaz's freetype package is, since 2026-11: libbrotli, the
  decoder alone, 75 KB). No decoder of our own: the glyf and hmtx
  transforms are 2,000 lines in FreeType, already fuzzed. FreeType
  does not say whether it has brotli, so WOFF2 sources are tried; the
  first one it refuses (no_woff2, lib/cascade.c) stops the fetching
  of the others, and each font goes on to its next source.
- Small caps (font-variant-caps small-caps and all-small-caps, petite
  caps falling back to them, `font-variant` an alias of it, the `font`
  shorthand's small-caps) are synthesized as Firefox does without the
  font's smcp feature: text_shape() shapes a copy in capitals, the
  letters that were lowercase (all of them for all-small-caps) in runs
  of 0.8 times the size, each glyph keeping its size for painting.
  Measuring and painting both go through text_shape(), so they agree;
  the width cache is per style. The smcp feature itself is not asked
  of HarfBuzz: one path for every font, with or without HarfBuzz. Case
  pairs are those text-transform knows (ASCII and the two-byte
  letters): ß stays small, not SS.
- `soie_fonts_stub(1)` keeps the 0.5em stub font: the test runner and
  `soie-render -S` use it, so layout tests do not depend on fonts.
- Faces are matched by width first, then style, then weight (CSS
  Fonts 4 5.2), the width being the OS/2 usWidthClass and
  `font-stretch` (keywords, percentages). DejaVu Sans Condensed calls
  itself "DejaVu Sans" with the same weights, and the directory scan
  found it first: every page was drawn about 8 % narrower than in
  Firefox, lines broke elsewhere (seen on 11 of 38 surveyed sites,
  2026-11-03). Web fonts are normal width (the descriptor is not
  read). The `font` shorthand takes a stretch keyword as one token.

- A face's ascent and descent are whole pixels, each rounded up
  (face_metrics()), and `line-height: normal` their sum: what
  FreeType's size metrics give, which Firefox and Chrome use on Linux.
  DejaVu Sans at 16 px is 15 + 4 = 19 px a line, where the scaled
  design metrics said 18.625: forty lines down a page everything was
  15 px higher than in a browser. Measured against Firefox at 13
  sizes of three families, all equal. Hacker News against Firefox:
  0.163 to 0.078 RMSE.

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
- Backgrounds and borders are painted on whole pixels: the box's
  edges rounded to the nearest pixel, each on its own (the width is
  the difference of the rounded edges, not a rounded width), as
  browsers do. Painted with their exact coverage, a box over another
  at the same fractional place let a line of the one below through,
  and two boxes meeting at a fractional edge both half-painted the
  pixel between them: the red lines of WPT's green-square tests, and
  blurred table grids. Text keeps its own snapping (glyph origins),
  rounded rectangles, SVG and images their anti-aliasing. Border
  widths are snapped as CSS Values 4 says, before layout: whole
  pixels, rounded down, 1 at least when not 0 (a 0.5px border is 1px,
  1.5px is 1px). With both, 11 WPT reftests passed.
- Borders: solid, dashed, dotted, double, inset/outset/groove/ridge
  (shaded). Sides alike on square corners are four rectangles (the
  common case, fast); otherwise each point of the ring goes to the
  side it is nearest by depth over that side's width, which mitres
  the corners: CSS triangles (an empty box's borders, the carets of
  Bootstrap and many menus) come out as triangles, a rounded box keeps
  each side's color and pattern (doc.slitaz.org's cards were all red).
  4x4 samples a pixel near the corners only. Dots are round, spaced to
  fall on the corners; a rounded double border is two rings. Since
  2026-11-03; before, sides overlapped at the corners and a rounded
  border took the top color, solid.
- box-shadow (since 2026-11-03; parsed but never painted before):
  outer shadows under the background, painted out of the border box
  only (a translucent box does not show its own shadow through),
  inset ones over the background, in the padding box; the first
  listed on top, currentcolor unless a color is given. Spread grows
  the radii as the spec says (a small radius less than the spread,
  square stays square). The blur is the spec's Gaussian (deviation
  blur / 2) as three box blurs (feGaussianBlur's way), whatever its
  radius the same cost per pixel, over a buffer of what the clip
  shows plus the margin the blur reads: a band of the page paints
  the same pixels as the whole page. 12 cases match Firefox. Damage
  takes the shadows' reach. Cost:
  about 1.3 ms a 200x140 card with a 30px blur; a viewport of 16
  such cards, two shadows each, 25 ms more. No cache of blurred
  shapes (browsers keep nine-patches): not needed yet.
- Corners are elliptical: a radius in % is of the width across and of
  the height down (border-radius: 50% of a box that is not square is
  an ellipse), and radii that would meet past a side are scaled down
  together (css-backgrounds-3 5.5), no longer each clamped to half
  the smaller side. The curves inside (padding box, content box, a
  double border's lines) take each side's width on their own axis: a
  thick top border under thin sides curves as in Firefox. Since
  2026-11-03.
- Radii down that differ from those across (`border-radius: 40px /
  15px`, `border-top-left-radius: 60px 20px`): a kind of their own,
  K_RADIUS, the across radius a length as before and the down one in
  the computed value's spare bytes (v, flag 1 px, 2 %), so styles stay
  24 bytes a value and no longhands are added; the specified value
  keeps a second slen (len2). A calc() mixing px and % down keeps its
  px. Getters give "40px 15px", or one value when both are the same,
  as Firefox. Before, the part after the slash was dropped.
- overflow other than visible clips to the padding box, in the curve
  of its corners: a mask of what the clip shows (as mask-image's), its
  rounded rectangle times the mask there was. An image, video or SVG
  is clipped the same way to its content box's curve (form controls
  draw their own), a background image to the border box's (gradient
  buttons had square corners). A positioned box is painted out of
  its ancestors' walk, so it takes their clipping back when its layer
  is painted: that of its containing block chain (an absolute box
  skips the static ancestors up to its containing block, a fixed one
  all of them), the rounded ones of the 8 nearest too. python.org hid
  its skip link, a relative <a>, in a 1px overflow: hidden box: drawn
  on the page without it. A test page of 12 cases: 0.082 RMSE from
  Firefox before, 0.0095 after. Cost: a page in a rounded overflow:
  hidden box paints 18% slower (masked pixels take the blend path),
  5 ms more on a viewport.
- clip: rect() of an absolute or fixed box (the skip links of go.dev
  and many others: clip: rect(0 0 0 0)), from its border box's
  corner, auto its edge; an empty rect is empty, not a pixel wide.
- clip-path: inset() (with round), circle(), ellipse(), polygon(),
  path() and url(#<clipPath>) of an SVG in the page, in their
  reference box (border box, margin-box, padding-box, content-box).
  The shape goes through the mask code: filled into the buffer of the
  border box, its coverage multiplies the mask's. Content beyond the
  border box is cut even where the shape would reach. A clipped box is
  a stacking context when positioned, as a masked one. Unknown shapes
  or references clip nothing. Test: `tests/paint/clip.html`.
- mask-image (and -webkit-mask-*, the mask shorthands): the way icons
  are drawn now, a box of background-color currentColor through an
  SVG (Wikipedia, the Arch wiki, MDN: black squares without it). Like
  opacity, no layer: the mask's layers (images, SVG, gradients, with
  mask-size, -position and -repeat, painted by the background code)
  go into a buffer of the box's border box, whose alpha then weighs
  every pixel the box and its descendants paint (blend(), images,
  SVG); a mask inside a mask multiplies both. A masked box is a
  stacking context; mask-mode (alpha only), mask-composite, -clip and
  -origin are not used. Test: `tests/paint/mask.html`.
- Paint tests (`tests/paint/*.html`, 320x240, stub font drawn as one
  box per character) compare pixels with reference PNGs. References
  are written with `tests/painttest -u` and checked by eye before
  they are committed. Pixels may differ by 2 levels per channel: the
  i486 x87 FPU and x86_64 SSE round floats differently (measured: 1
  level at most on some anti-aliased edges).

- Background layers were rewritten (image.c, paint_layers()): a
  gradient is an image without a size of its own, so it takes
  background-size, -position and -repeat like any other (the 2 px
  underline, the grid of 1em squares, the striped progress bar were
  one gradient over the whole box). Each layer has its positioning
  area (background-origin, the padding box by default: it was the
  border box) and its clip (background-clip; the color is clipped by
  the last layer's); positions take one to four values with their
  keywords in any order (parse_position(): `top right` and
  `right 10px bottom 5px` were read as x then y); lengths go through
  the real parser with their units and calc() (raw_lenp(): 2em was
  2 px); repeat has space and round; tiles start from a division, not
  a loop (a position of 1e10 px never ended). The root's background
  images, or the body's when the root has none, cover the canvas (CSS
  2 14.2): only the color did. background-clip: text is still the
  border box.
- Gradients (CSS Images 3 and 4): linear ones to a corner at the
  angle that puts the 50 % line through the two other corners (atan(h
  / w), it was w / h); radial ones are ellipses by default, with
  circle, the four extents, one or two radii and `at <position>`;
  conic gradients with `from` and `at`; the three repeating forms;
  stops with two positions, color hints, lengths with units; colors
  mixed premultiplied (a fade to transparent keeps its color); any
  number of stops (the 33rd wrote past a stack array). `in oklch` and
  other color spaces are read and mixed in sRGB. One sample per pixel:
  hard stops on a slant are not anti-aliased.
- box-shadow blurs are capped at 300 px, as browsers: the blur buffer
  grows with it and 99999px aborted on its allocation.

- Transforms: box_transform() (block.c) gives a box's 2D matrix from
  translate, rotate, scale and the functions of transform (the 3D
  ones keep their part in the plane). A transform that only moves is
  the layout's, as before: the box and what it holds are translated.
  Anything else (rotate, scale, skew, matrix) is painted: the box is
  drawn where it lies, with everything it holds and as a stacking
  context, into a canvas of its own, which is then drawn through the
  matrix around transform-origin, each pixel taking its color from
  where it comes from, bilinear (paint_transformed()). Rotated carets
  and chevrons (a bordered square turned 45 degrees), flipped icons,
  scaled badges were drawn untransformed. Text in it is scaled as
  pixels, not laid out again at its size; the hit test and
  getBoundingClientRect() still know the box where it lies; what
  overflows a box that clips is not in the canvas. Damage takes the
  box's diagonal times its scale around it. Inline-blocks are now
  moved by position: relative and by moving transforms (they were
  not).

- background-clip: text: the text of the box and of what it holds
  is painted alone and opaque into a buffer (text_mask_pass: no
  backgrounds, borders nor images), whose coverage masks the box's
  background (text_mask_push()). With -webkit-text-fill-color (an
  inherited property kept as text, so that its currentcolor stays
  each element's) or a transparent color, that is gradient text: it
  was a gradient bar with the text over it, or without it.
- text-overflow: ellipsis at paint time: a line that runs past the
  edge of a box that clips it is drawn up to what fits and ends with
  the ellipsis; the layout does not change.
- The last line kept by -webkit-line-clamp ends with an ellipsis too,
  whatever text-overflow says (Firefox and Chrome draw it with clip):
  after the line's content when it fits in the box, the line not
  aligned again (a centered "bbb..." stays where "bbb" was), else in
  place of the content's end through the text-overflow path.
  line_clamp() is the one test of a clamping box, for layout and
  paint. `line-clamp` is an alias of `-webkit-line-clamp`: it clamps
  only the old WebKit box, as the prefixed one; Firefox 140 ignores it
  and css-overflow-4 makes it work on any block (not done).
- Floats are painted over the backgrounds of all the blocks of their
  formatting context (Appendix E): they wait in a list for the box
  that paints as a unit around them (float_owner(): the root, a
  stacking unit, a float, an inline-block, what clips or fades). A
  float in a block was painted with that block, then covered by the
  background of the next one (an image floated in a paragraph and the
  code block after it).

- Form controls sized and drawn as browsers do (measured in Firefox,
  equal to the pixel at two font sizes): a text field is its size in
  characters as wide as a 0, and one em (191 px for 20, it was 141);
  a select is as wide as its widest option and its arrow (it was 5
  em: long options were cut, the arrow over the text), a list box
  (multiple, size) shows its rows; buttons have 4 px of padding;
  range, color and file inputs, progress and meter are drawn (they
  were text fields, or nothing). A control whose border is still the
  style sheet's 2 px inset or outset is drawn with the thin rounded
  ring browsers give it (native_control()): its computed style is
  unchanged, so pages that set a border, a radius or a background
  image get exactly that. No pop-up list, no slider nor color
  picker behind them yet.

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
  fill-rule, currentColor, viewBox and preserveAspectRatio, stroke
  joins (miter with stroke-miterlimit, round, bevel) and caps (butt,
  round, square), nested <svg> (each its viewport, viewBox and
  clipping, percentages of the one around: Wikipedia's sprite is 23 of
  them) and <use> of a <symbol>, an <svg> or any element of the
  document (href or xlink:href, x, y, width, height; 8 deep at most,
  so a loop draws nothing). Not yet: patterns (drawn gray), clipPath
  on SVG shapes (clip-path on HTML boxes is done), masks, <use> of
  another file, CSS in <style>, dashes.
- SVG gradients: linearGradient and radialGradient as fill or stroke,
  stops with offsets, stop-color and stop-opacity (attributes or
  style), interpolated premultiplied as CSS gradients are; both units
  (objectBoundingBox from the shape's box in its user space, or
  userSpaceOnUse with percentages of the viewport), gradientTransform,
  spreadMethod, href (or xlink:href) to another gradient for its
  attributes and stops, 8 deep at most; a focal point and fr. A focal
  point out of the circle is brought back onto it (SVG 1.1; SVG 2's
  cones are not drawn). Each pixel of the filler maps back into the
  gradient's space and looks its color up in a 256 entry table.
  url(#id) with a fallback color uses it when the id names nothing;
  without one, nothing is painted, as the spec says (it used to be
  gray). debian.org's banner is five radial gradients.
- SVG text: <text>, <tspan> and <a> in it, x, y, dx, dy (the first
  value of a list), font-size (user units, em and %), font-family,
  font-weight, fill, text-anchor by chunk (a run with its own x starts
  one), white space collapsed. Drawn by the text code of HTML with the
  box's style changed (font, color), so the fonts and glyph caches are
  shared: these styles live in a ring of 32, the same values giving
  the same one (widths are cached by style). Text is not rotated nor
  skewed (the transform's scale only), has no stroke, and a gradient
  fill is drawn black. An SVG drawn as an image
  starts from the initial font (serif, 16px), an inline one from its
  box's.
- An SVG image (img, CSS background) with width and height but no
  viewBox scales to its box as if it had viewBox="0 0 width height",
  as browsers do: rust-lang.org's logo, 144 px drawn in 40, was cut to
  its top left corner.
- Strokes have their own list of segments: they were drawn from the
  fill's edges, which leave out horizontal ones (a rect's top and
  bottom were missing, Toile's tab icon was two brackets), and without
  joints (curves had teeth). Each segment, joint and cap is a convex
  piece, all turned the same way and filled at once: nonzero merges
  them, a translucent stroke is not blended twice where they meet.
- The polygon filler is analytic in x with 4 sub-scanlines per pixel:
  good anti-aliasing for the cost.
- Curves become lines: 16 a cubic and 16 a half turn of an arc at
  least, more as the drawing grows on screen, for 0.2 device px at
  most between line and curve (Wang's bound for cubics, the sagitta
  for arcs; 1024 at most). Below about 100 px nothing changes; the
  logo at 380 px had flats.
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

- An SVG in a page is painted from the cascade: fill, stroke,
  stroke-width, -linejoin, -linecap, -miterlimit, fill-rule,
  fill-opacity, stroke-opacity and text-anchor are inherited
  properties (props.h), the presentation attributes are hints
  (svg_hints() in cascade.c, a bare font-size being px), and svg.c
  reads the computed style of each element (styled_paint()) instead
  of its attributes and its style="" text. Icons that take their
  paint from a class, from currentColor through a rule or from a
  var() were black or missing; display: none and visibility by a
  rule work the same way. A `<style>` inside an SVG is a sheet of the
  page, as in browsers. An element drawn through `<use>` inherits
  from the `<use>`, not from where it is written: the paint state
  carries the inherited values down and an element only takes from
  its computed style what differs from its parent's there (own()).
  A value set to what the parent already has is then seen as
  inherited: the same result except through a `<use>` that says
  otherwise, rare enough. SVG images (their own document, no
  cascade) keep the attribute path. Sizes take their units
  (width="1.5em", "5in": ulen()).
- One svg_render() draws 200000 elements at most, those reached
  through `<use>` counted each time: the depth limit of `<use>` did
  not bound eight levels of thirty each.

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
- Brotli bodies (`Content-Encoding: br`) when libbrotlidec is there
  at build time (pkg-config --exists; the system has it since
  FreeType reads WOFF2 with it). Asked for over HTTPS only, as
  browsers do: a proxy on the way of plain HTTP may not know it.
  Same limit and same leniency as gzip (a body that is not brotli is
  kept as it came, a cut one keeps what was decoded). Linked as
  -lbrotlidec, not through its .pc: brotli 1.0.9's has a `-R` flag
  that cc refuses.
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
- Connections opened ahead (October 2026): with a page's request go
  three jobs that open connections to its server and leave them in
  the pool (net_preconnect(), none when three wait there already), so
  that its sheets, scripts and images do not each pay a TCP and TLS
  handshake once the page is in. A TLS 1.3 server sends its session
  tickets after the handshake: an idle connection with only those to
  read is kept, not taken for one the server dropped. Measured on live
  sites, 200 ms from the servers, until all resources are in: Hacker
  News 1.9 to 1.2 s, Wikipedia 2.3 to 1.5, www.slitaz.org 0.27 to
  0.13. Only the page's own server: what comes from elsewhere (a CDN)
  gains nothing. The cost is a handshake per connection never used,
  felt on the slowest processors; three is what a page of one server
  needs (its connection, plus three for the first resources).
- Forms: text-like inputs, password, textarea, checkbox, radio,
  select (a pop-up list since, see "The pop-up list of a select"),
  submit buttons and Enter;
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

- The browser is split in two (three since: the fronts' common part
  is browser/front.c, see "Repainting what changed"): browser/shell.c
  keeps the page, its
  resources, history, forms, scripts, meta refresh, find and the
  offscreen PNG render, with no toolkit; a front shows it and feeds
  the input in (shell.h). The first front is `xsoie` (browser/xsoie.c,
  Xlib), so that a GTK 3 one (`gsoie`, GtkEntry address bar, real
  scroll bar) can come without copying a line of the shell. The
  engine and its files keep the Soie name (`~/.config/soie`, the
  extensions): both fronts share them.
- The logo: an S of three silk threads side by side, coral, SliTaz
  red and dark red, the outer one of a bowl the inner one of the
  other (browser/icons/soie.svg, three stroked elliptical arcs, under
  1 KB, written by hand). The threads overlap by a pixel: touching
  edges leave a dark seam in any anti-aliased renderer. hicolor PNGs
  from 16 to 256 px. The window icon is the same SVG, in front.c,
  drawn by Soie at 48, 32 and 16 px; Soie paints on an opaque canvas,
  so it is drawn on black then on white, the difference giving its
  alpha. doc/share-image.html is the site's Open Graph picture, drawn
  by soie-render too.
- The shell talks to the front through three calls the front defines
  (ui_title, ui_address, ui_alert); the rest goes the other way:
  shell_tick() returns the poll timeout and says when to repaint,
  keys come in as DOM key values ("Enter", "ArrowLeft") and UTF-8
  text, not toolkit key codes.
- Keyboard input goes through the X input method (XIM,
  Xutf8LookupString): dead keys, compose and every character, not
  just Latin-1 (`€`, `œ` could not be typed before). No input method:
  XLookupString as before.
- Clipboard: Ctrl+C copies the address, the focused field's value or
  the text selected in the page, Ctrl+V pastes the
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
- Visuals: Soie paints 32-bit pixels, sent as they are (shared memory,
  no copy) when the screen's are 32-bit B G R A or R G B A. Any other
  visual gets them in a buffer of its own and xsoie converts the
  rectangles shown: TrueColor through a table per colour built from
  the visual's masks (565, 555, packed 24), an 8-bit colormap through
  a cube of 6x6x6 colours allocated in the default colormap (black or
  white when it is full). The image was made at the screen's depth and
  painted as 32-bit: at 16 bits, twice its size, and xsoie crashed at
  the first paint on old cards (cirrus, fbdev at vga=791), the very
  machines of the xsoie flavor.
- history.pushState() and replaceState() change the address the host
  shows, with no load: the script host gained a url hook (libsoie 1.3,
  NULL in older hosts), pushState a history entry of the shell's own
  (Back then loads the URL before it), replaceState the current one's.
  The page's URL is the new one, for links resolved from it, as in
  browsers. A fragment link updates the address too. YouTube's address
  followed none of its videos before.

- Requests carry Accept-Language from the locale (LC_ALL, LC_MESSAGES,
  LANG): fr_CH.UTF-8 asks "fr-CH,fr;q=0.9,en;q=0.8", C asks "en".
  Without it Google answered in the language of the address's country
  (German, from Switzerland).

## about:settings

- The settings of xsoie and wsoie, a page of the shell's
  (shell/settings.c), not of each front: start page, search URL,
  JavaScript, colours (the system's, light, dark), zoom, cookies (all,
  no third party, none), content blocking, downloads folder; and a
  button that clears the cookies. ~/.config/soie/settings holds them,
  key=value lines one can edit by hand, written whole through a
  rename; bad values are left out and the defaults stand.
- A front opts in with shell_settings(): Toile has its settings and
  does not call it, about:settings is not found there.
- Only the user reaches it, typed or from itself: scheme_allowed()
  holds it as it does file://, for links, forms, scripts' navigations,
  refreshes and windows; resources and scripts' requests never get it
  (the network does not know about:). It changes nothing on a GET (a
  link that would reach it could not save), only a POST of its own
  form; no script runs on it.
- A plain form without a script, drawn by Soie with the page's colours
  (prefers-color-scheme); the ⚙ button of the bar opens it. The
  shell applies search, scripts, cookies, blocking and downloads; the
  front zoom and colours, through a new ui call (settings). Other
  windows, other processes, take them when they open.
- Form controls looked their age next to it: checkboxes and radios
  are painted as Firefox's and Chrome's (a gray ring on white, the
  accent blue and a white tick or a dot when checked, never the
  page's text colour, invisible on dark pages), radios round; the UA
  sheet gives them browsers' margins and no field border; placeholders
  are the text colour at 55 %.

## about:auth, HTTP logins

- The CUPS administration pages (localhost:631) were unusable: they
  answer 401 with a Basic challenge, Soie had no login and showed the
  401 page (Filature bug 1025, HGT). Basic only: CUPS, routers, most
  private pages; Digest and Negotiate are not done.
- A page of the shell's (shell/auth.c), not a dialog in each front:
  the same in xsoie, wsoie and SoieView/Toile at once, as
  about:settings. A page load (never a resource) answered 401 with
  Basic shows about:auth: the host in bold, the server's realm quoted
  as its words, a warning on plain http except localhost/127.x, user
  name, password, Log in, Cancel. The shell keeps which URL asked, its
  form body (a POST is sent again after the login) and the server's
  own 401 page, which Cancel shows at its address.
- The login lives in net.c, by origin (one realm each, 16 at most), in
  memory only, never written; a private window's apart, forgotten with
  the private cookies (soie_view_clear_cookies), the others with the
  cookies on about:settings (shell_forget_logins()). Sent with that
  origin's page loads and what its own pages load; never when the
  request comes from another site's page (no ambient login for a page
  that is not the server's, which browsers allow) nor with credentials
  omitted. A login the server answers 401 to is forgotten, the form
  comes back saying so.
- Only about:auth may post to about:auth (scheme_allowed(), as
  about:settings): otherwise a page could log the user in to the
  waiting server as someone else. Tested through form, link, location,
  meta refresh, script form.submit(); with the check removed the
  hostile form does reach the server, the test bites. No script runs
  on it.
- A password field's caret was placed in the bullets by the value's
  byte offset (3 bytes a bullet): now one bullet a character.

## The HTTP cache

- Soie had none: every page asked again for its style sheets, scripts
  and images (Wikipedia: the same 30 resources at each article). The
  idea came from looking at Lightpanda (an RFC 9111 cache in SQLite);
  this one is shell/cache.c, in memory, a private cache by RFC 9111.
- In net_fetch(), so pages, their resources and scripts' fetch/XHR all
  go through it: GET without a body or the caller's own headers (Range,
  Origin: those go to the network), status 200, not a body streamed to
  a file, not a private window's (neither read nor written), not one
  sent with a login (about:auth). Never kept: no-store, a Set-Cookie,
  a Vary on anything but Accept-Encoding, User-Agent, Accept-Language
  (ours never change).
- Fresh for max-age, else Expires - Date, else a tenth of the age since
  Last-Modified, a day at most; less the Age and the apparent age.
  no-cache is kept but always asked again. Stale with an ETag or a
  Last-Modified: asked with If-None-Match / If-Modified-Since, a 304
  makes it fresh again with the 304's headers over the stored ones.
  Neither fresh nor with a validator: not kept.
- Partitioned by the top page's origin (key "origin url"), as browsers
  do since 2020: a site cannot learn from timing what another loaded.
- Back and Forward take the page from the cache when it is fresh; the
  user's reload (NET_RELOAD, shell_reload()) asks the page again,
  its resources still come from the cache (Chrome's way). A POST, PUT,
  DELETE that succeeds drops its URL's entries in all partitions.
- RAM/64, 8 to 64 MB; an entry at most a quarter of it; the least
  recently used go first. SOIE_DEBUG=1 prints its counters after each
  page, 2 each hit, revalidation and entry kept.
- Measured on Wikipedia: an article 1.35 s, back to it after another
  0.40 s (20 of its 30 resources from the cache); the second article
  took 11 from the first's.

## The private network guard

- A web page could make Soie ask anything of the user's own network:
  an image, a fetch() or a form to the router (its DNS, its ports),
  the printer, CUPS on :631, a model server on localhost (Ollama,
  llama.cpp: no password), a dev server; or time the errors to map
  the machines there. Browsers block it now (Chrome's Local Network
  Access, with a prompt); Lightpanda has it as an option for agents.
- Three levels of address, from the most public: public, the local
  network (10/8, 172.16/12, 192.168/16, 169.254/16, 100.64/10 for the
  operators' NAT and VPNs like Tailscale, fc00::/7, fe80::/10), this
  machine (127/8, 0/8: 0.0.0.0 reaches it on Linux, ::1, ::). IPv4 in
  IPv6 (::ffff:a.b.c.d) counts as IPv4. A page asks for nothing more
  private than itself: a public page nothing of the local network or
  of the machine, a local network page nothing of the machine.
- A page's level is where it came from: the address its response came
  from (struct response.level, kept in the cache entry for Back),
  file:// the machine, the others (about:, a front's scheme, data:,
  an error) public. The shell gives it to every request of the page as
  SOIE_REQ_NO_LAN or SOIE_REQ_NO_LOOPBACK (ext.h: the script engine
  passes its host's flags through, SOIE_REQ_PAGE), so its resources,
  frames, scripts' fetch/XHR, modules, workers and players all carry
  it; flags are taken when the request is made, a request still out
  after the next page keeps its own.
- Checked in dial(), on each address the name gives, after every
  redirection: a name that said public may say 127.0.0.1 a minute
  later (DNS rebinding), it is the address that counts. A pooled
  connection is only reused at its level or under; a cache entry from
  further than the request may go is a miss.
- What the user does is not guarded: an address typed, a bookmark, a
  link followed, a GET form (the user sees where they land). A POST
  form of a public page to the local network is (the routers' attack:
  a form that changes their settings); a script's form.submit() of a
  POST form goes the same guarded way (see JavaScript). Nothing to set: no page found yet that
  needs an exception; a settings switch if one shows up.
- Tested in network namespaces (unshare -n, addresses 203.0.113.5 and
  192.168.250.1 on lo, a hosts file naming 127.0.0.1): from a public
  page no path reached the local network or the machine (style sheet,
  image, redirect, 0.0.0.0, a name, frame, video, fetch, sync XHR,
  module, worker and its fetch, POST form), from a local
  network page only the machine was refused, and from a page on the
  machine all went through (the control).

## The shell on its own, many page views

- The shell left browser/ for shell/ (shell.c, net.c, fetch.c,
  debug.c): it has no toolkit and the fronts are not its only users,
  a GTK 3 widget for Toile (toile-soie, the build for old hardware)
  comes on it next. browser/ keeps the fronts: front.c, xsoie,
  wsoie. Moved with `hg mv`, the history follows.
- A shell is an object now (`shell *shell_new(ui, data, scripts)`,
  shell_free(), every shell_*() call takes it): one page view with
  its history, focus, find and painting state, as many as a toolkit
  has tabs, in one process and one thread. xsoie and wsoie make one
  and behave as before (renders byte for byte the same, offscreen and
  on screen). The three front calls became a struct shell_ui of
  callbacks with the front's data, NULL for the unwanted ones; the
  title, which the toolkit part sets, is tk_title() in front.h.
- What the shells share: the extensions, the fetch threads and their
  descriptor (shell_init(), once), the HTTP pool, the cookies, the
  debug trail. A job is tagged with its load's gen as before, but gens
  now count across shells: the finished jobs go to the inbox of the
  shell whose page has that gen, any other is dropped (a page gone,
  a shell freed). So when the descriptor is readable a front ticks
  every shell; a shell's tick reads its own inbox only.
- The script host is per shell (its data is the shell); requests of
  the script threads only reach net_request(), never the shell, so
  one still out after shell_free() is harmless (the script engine
  copied the function and data when it queued them).
- The engine needed nothing: its other static state (the damage's old
  table, the layout's old and new trees, the font document) lives
  only during one soie_layout_document() call, and all shells lay out
  in the same thread; the glyph and font caches are meant to be
  shared.
- Checked with a scratch program: four shells loading at once (two
  pages with scripts, so two QuickJS runtimes, one network page, one
  freed while loading, under MALLOC_CHECK_=3) paint exactly what each
  paints alone.
- The extensions are those of the build in use: $SOIE_EXT_DIR, else
  the source tree's when the program (xsoie, gsoie) or libsoie (a
  program built on the tree's, found with dladdr()) is in one (a
  directory up to four levels above with lib/soie/soie.h and ext/),
  else the installed ones. Before, a program of the tree took the
  installed extensions whenever there were some (another ABI), and
  only a program one level down found the tree's.
- With Ctrl, Alt or Meta a key the page does not take is the front's:
  the shell no longer scrolls the page on Ctrl+Page Up or Alt+Down
  (Ctrl+Home and Ctrl+End still do), and a text field gives back the
  shortcuts it does not know. A GTK notebook's Ctrl+Page Up never
  reached it.

## SoieView, the GTK 3 widget

- gtk/soie-view.[ch]: SoieView, a GtkWidget on a shell, for Toile's
  Soie build (toile-soie) the way WebKitWebView is for its WebKit one;
  the API follows WebKit's names where it has the same thing
  (load_uri, reload, go_back, can_go_back, "uri", "title"). Its own
  directory, not browser/: it is a library, not a front, and does not
  use front.c (Toile has its bars). Built when GTK 3 is there, as
  gtk/libsoie-gtk.so.0 (the shell inside, linking libsoie.so) with the
  header installed as soie/soie-view.h.
- Painting: a cairo ARGB32 image surface is B G R A on a little endian
  machine, what shell_bgra() paints, no conversion. A repaint is the
  shell's damage, a scroll moved pixels, or all of it, and only those
  rects are queued to GTK; video frames their rect.
- The loop: one g_unix_fd_add() on the shells' descriptor for all the
  views (shell_route() then a tick of each, so it never spins when no
  view is left), a GLib timeout per view for its next tick, an idle
  tick after input (scripts' new timers, a load started).
- What the shell tells (title, address, alert) is kept and told after
  its call returns, as GObject notifications ("uri", "title",
  "loading", "can-go-back", "can-go-forward") and signals ("alert",
  "link-hovered"): a handler may destroy the view, never inside the
  shell.
- Keys through a GtkIMMulticontext (dead keys, compose, the user's
  input method); shortcuts (Ctrl, Alt, Meta) skip it, and a key the
  page does not take goes on to the window. Ctrl+V and Shift+Insert
  paste into a page's text field, the middle button the PRIMARY
  selection.
- GtkScrollable: in a GtkScrolledWindow the page has a real scroll
  bar; the vertical adjustment follows the page (height, scroll), its
  moves scroll the page. No horizontal scrolling yet.
- gtk/gsoie: a little tabbed browser on it (address bar, back,
  forward, reload, status line, Ctrl+T/W/L, F5), to try the widget
  and show its use; installed too. Checked on Xvfb: tabs loading at
  once, Ctrl+Page Up between them, the address bar, history, tabs
  closed while YouTube's and Wikipedia's scripts load (MALLOC_CHECK_).
- What Toile needed came the night after, worked out with the Claude
  session porting Toile (toile-soie): each item below was asked by
  it, mapped on the WebKit call it replaces, and tested in Toile too.
- Adblock: no hook in the widget; Soie's block.so
  (~/.config/soie/block.txt) blocks for every view. Web Workers came
  later (see JavaScript): hg.slitaz.org's Anubis page passes.

## SoieView for Toile: what a browser needs

- pkg-config: soie.pc and soie-gtk.pc installed, and their
  -uninstalled twins at the tree's top made by the build, so that a
  browser built on the tree needs only PKG_CONFIG_PATH=<the tree>
  (its paths and rpath come from there). Headers: <soie/soie.h> and
  "soie-view.h" (Cflags has $includedir/soie).
- A GtkScrolledWindow with an automatic horizontal policy left Toile's
  window black ("infinite surface size"): the widget's horizontal
  adjustment was all zero. It is the view's width now (nothing to
  scroll), the usual way to say so.
- The browser's own pages: shell_scheme() / soie_view_register_uri_
  scheme("toile", fn, data). As WebKit's, but answered at once in the
  main thread (Toile's pages are built at once anyway): fn gets the
  whole URI with its query (a GET form's fields), returns the page and
  its type, NULL for an error page. The scheme is isolated as WebKit's
  "display isolated" ones: its pages, the address bar and history reach
  it, a web page cannot (no link, form, refresh, resource, script
  request, new window, middle click): a page could otherwise make the
  user click toile://admin?action=clear_cookies&confirm=1. load_html()
  shows a string at a base URI (a history entry).
- "loading" goes TRUE then FALSE for every load, also those that come
  at once (the scheme's, load_html): Toile adds to its history and
  refreshes its buttons on the end of a load, as on WebKit's
  LOAD_FINISHED.
- New windows: the script host's navigate() takes 2 for a new window
  (window.open(), a click on a link of another target), honoured only
  in the user's click or key (in_input): pop-ups from timers are
  dropped, as browsers block them. shell_ui.open asks the front;
  xsoie starts another process (as Ctrl+N), SoieView emits "create",
  as WebKit's: the handler returns a new view, unloaded, which then
  loads the URI; without a handler it loads in place. The middle
  button and Ctrl+click on a link go the same way.
- Context menu: the widget builds the stock menu (Back, Forward,
  Reload, Copy Link Address, Copy Image Address, Paste in a field)
  from shell_hit() (link, image, field under the pointer), emits
  "context-menu" (menu, link, image) for the browser to add its items
  or say no, then pops it up: Toile already worked by adding items to
  WebKit's stock menu.
- Downloads: a page load whose response is not to be shown
  (net_shown(): Content-Disposition: attachment, or a type that is not
  text, image, HTML, XML, JSON, JavaScript) is a download, decided in
  the fetch thread as soon as the headers are read, so that the body
  goes to a file in the cache as it comes (struct download in net.h:
  identity bodies; chunked or compressed ones stay in memory and are
  written at the end): an ISO image does not go through the RAM of an
  old PC. The shell adopts it, the page on screen stays, the front
  picks the destination (~/Downloads or $XDG_DOWNLOAD_DIR, a free
  name), the file is renamed (or copied across file systems) at the
  end, cancelled or failed ones leave no file behind. Downloads belong
  to the process, as to WebKit's context: closing their view does not
  stop them (Toile's target=_blank downloads open a tab the user
  closes). SoieView: "download-started" (destination set there,
  synchronously as WebKit's decide-destination), SoieDownload with
  "progress", "finished", "failed" emitted from an idle.
- Zoom: a page zoom in the cascade, as CSS zoom on the root, not a
  scaled painting: style options' zoom multiplies absolute lengths
  (lin_resolve), the 16 px defaults, font-size and border-width
  keywords; past the cascade, doc_zoom() (the document laid out or
  painted) scales intrinsic sizes, transform translations, background
  sizes and positions and gradient stops. Media queries see the
  viewport divided by the zoom, so responsive pages switch layouts as
  in browsers; vw and vh stay the screen's. At 1 the renders are byte
  for byte the same (checked on the corpus and real sites). Scripts
  still see device pixels (innerWidth, rects). shell_zoom(), xsoie
  Ctrl+plus, minus, 0; SoieView "zoom-level" (WebKit's name).
- HiDPI: the widget paints at gtk_widget_get_scale_factor() (a cairo
  surface with its device scale), the shell's zoom is the user's
  times the scale; events and adjustments converted. Checked with
  GDK_SCALE=2.
- Dark pages: prefers-color-scheme follows the desktop. SoieView asks
  GtkSettings (a theme named dark, gtk-application-prefer-dark-theme)
  and follows its changes live; xsoie and wsoie have no toolkit to
  ask, so shell_system_dark() reads what GTK would: $GTK_THEME, then
  gtk-3.0 and gtk-4.0 settings.ini, the user's then /etc/xdg (SliTaz
  sets SliTaz-Dark there), again each time the window gets the focus.
  No XSETTINGS or portal: one more protocol for the same answer. Their
  bars have their own dark colours. A page without dark rules stays
  as it is (no forced inversion); one with color-scheme gets the dark
  canvas and system colors.
- Private windows: SOIE_REQ_PRIVATE on every request of a private
  shell (page, resources, scripts' requests through the script host's
  flags, players) and document.cookie: the cookie extension keeps a
  second jar for them, in memory only (swapped in under its lock).
  SoieView "private" (construct only), as a view on WebKit's ephemeral
  context. The cookie policy (third-party refused by default, all, or
  none) and clearing a jar are new hooks of the cookie extension,
  appended to its struct (SOIE_EXT_HOOK sees whether a module has
  them): soie_view_set_cookie_policy(), soie_view_clear_cookies().
- Fixed on the way: <noscript> is hidden when scripts run (HTML
  15.3.1, @media (scripting) in the UA sheet; its raw text showed on
  duckduckgo.com); @font-face src URLs are resolved against their
  sheet (relative and protocol-relative ones were never fetched);
  data: URLs in the network layer (RFC 2397: images, backgrounds,
  fonts, scripts' requests); the offscreen render paints again once
  late background images came. make asan-sites keeps cookies in
  memory (the sites wrote into the user's jar).
- Text selection in the page, asked by pankso after the night: no DOM
  ranges, two page points (where the button went down, where it is)
  and a unit (characters, words, blocks, all). The engine walks the
  text fragments in layout order, as find does; a point stands for the
  nearest character of the nearest fragment (nearest line first, then
  along it; justified spaces counted). soie_select() gives the rects,
  soie_select_text() the text: a wrapped line joins with a space, a
  forced break (newline, <br>: line boxes now remember it) and a new
  block with a newline, blocks side by side (table cells) with a
  space; list markers and generated content are left out. The points
  stay in page coordinates: after a relayout the shell finds the rects
  again, the same place of the page selected. Fronts: a drag, a double
  click for a word, a triple one for the block, Shift+click to go on,
  Ctrl+A, Ctrl+C; a selection made is the PRIMARY one; SoieView adds
  Copy to its context menu and soie_view_has_selection(),
  get_selected_text(), copy_clipboard(), select_all() (WebKit's
  editing commands). A press on a link or a field is theirs: no
  selection starts there. Not done: user-select, text in form
  controls, selecting by keyboard, dragging the selection.

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

- With `SOIE_DEBUG=2`, the script engine says when its heap crosses a
  32 MB step (measured every 10 s): its size against the limit
  (256 MB then), objects, nodes wrapped. YouTube: 141 MB 16 s after a
  watch page, 410 000 objects, 11 000 nodes wrapped; steady while it
  plays.
- Wrappers lived as long as the page: a node held its wrapper from
  the first time a script touched it, and with it all the page hung
  on it. YouTube replaces its components at each video: 62, 84, 148,
  207, 227 MB and 29 000 wrapped nodes in 45 s, out of memory at the
  third video (and QuickJS crashed building that error's backtrace,
  see below). A wrapper now lives as long as its tree can be reached,
  as browsers do: each wrapper holds its tree root's wrapper, the root
  holds its tree's wrappers, and the class's gc_mark tells the cycle
  collector both. The document's tree is held (the page's document);
  a tree out of it lives while a script holds any node of it, root or
  not, and goes as one cycle when none does. A node moved to another
  tree is put in it at once when inserted, at the next second's sweep
  when taken out (held by the old tree until then, never too early).
  A page building and dropping 500-element subtrees, each element with
  data and a listener closing over it, 20 times a second for 10 s:
  226 MB to 65 MB of RSS (tests/js/wrapper-identity.html checks the
  wrappers kept: in, out and back, held while out).
- IntersectionObserver and ResizeObserver held every observer (and so
  its targets, and the component its callback closes over) in a set of
  the page until disconnect(), which YouTube does not always call. As
  in browsers, the page holds an observer only while a target is in
  the document; a node holds its observers; targets out of the
  document, once told so, are kept by a WeakRef, and an observer with
  all its targets out rests (a WeakRef too), woken when one is back
  (tests/js/observer-rest.html).
- MutationObserver kept its targets in a set and the registrations in
  a WeakMap whose values held the keys back: no tree a mutation
  observer watched was ever freed (a page dropping 200-element trees
  observed: 195 MB in 20 s, 11 MB now). As in browsers, a node holds
  its registrations (a property of its wrapper), the observer its
  nodes by WeakRef.
- `SOIE_DEBUG_HEAP=1`: the heap line every 10 s, with the biggest trees
  out of the page still held (their root's tag, id and class, how many
  wrappers): what found the observers above. After them YouTube still
  keeps about 13 MB a video, its description renderers above all; who
  holds them is not known (QuickJS cannot say who refers to an object).
- What YouTube's heap is made of (`SOIE_DEBUG_HEAP=1` now prints
  QuickJS's own table too), October 25: after the first video 397 000
  objects, 122 MB of 4 KB arenas 90 % full; each navigation adds
  130 000 to 150 000 objects, 27 to 42 MB (149, then 191 MB after two).
  Objects (72-byte blocks) and their property arrays (56) are most of
  it, bytecode 26 MB. Not fragmentation (the arenas stay 88 % full),
  and not something of ours: the timers, object URLs, observers and
  requests the script engine holds stay the same from one video to
  the next. The trees out of the page are YouTube's components of the
  videos before (suggestion lockups, description renderers), held by
  YouTube's own objects. A search page and a few videos reached 256 MB:
  YouTube then showed its error.
- So the heap limit follows the machine: a quarter of the RAM, never
  under the 256 MB it was (a 1 GB machine keeps it) nor over 1 GB.
  16 GB here gives 1 GB. A fixed 512 MB was turned down for the old
  1 GB machines. Who holds YouTube's old components is still to find
  (a path from the collector's roots, in a scratch QuickJS).
- Past half the limit the script engine runs the cycle collector at
  most every 10 s: QuickJS's own threshold grows with the heap and
  came too late for the trees dropped.
- YouTube, a video every 40 s (a suggestion clicked by a start script):
  before, 62 to 227 MB and the crash at the third; with the wrappers
  alone, the crash at the fifth; with all three, 141 MB on the first
  video, 160 to 192 MB for the next ones, 215 MB at the sixth, no
  crash in 4 minutes. About 10 MB a video is still kept (YouTube's own
  history, or something of ours: not found yet).
- The crash at out of memory is QuickJS's: at a bytecode exception it
  builds the backtrace on rt->current_exception without holding it; an
  allocation failing in there throws again, JS_Throw() frees the old
  exception, and the "stack" property is written into freed memory.
  Read in its code (2026-06-04), not reproduced on purpose. Holding
  the exception around build_backtrace() fixes it (the patch is in
  notes/quickjs-2026-06-04-backtrace.patch, for the wok's receipt and
  upstream): with a 150 MB limit YouTube then gives 17 "out of memory"
  errors and goes on, where it crashed.

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
- Another size restyles only the elements whose style has viewport
  units: the cascade notes them (vw, vh, vmin, vmax met while
  resolving a length, calc() included; a node byte), and a resize
  marks them SELF: each restyled alone, its subtree only when its
  style changed, as for a script's change (past 512 marks, all of
  it). A page without viewport units is not restyled at all. Wikipedia
  (some vw in its skin): 45 ms of cascade a resize to 9-13 ms. Checked
  by a differential test (notes/soie-difftests/vpdiff.c: resized
  against fresh, computed values of 31 properties and the boxes) on a
  trap page (vw font sizes inherited by em, ::before, grid, calc, a
  media query crossed) and Wikipedia offline: the same styles.
- xsoie keeps only the last of the ConfigureNotify events waiting: a
  window dragged bigger cost one relayout per event (40 for a short
  drag on YouTube, 10 s behind the mouse); now one per relayout time.

## Repainting what changed

- A redraw used to paint the whole window: page, toolbar and bars, then
  swap red and blue in every pixel and send them all down the X socket.
  A wheel step on a long page with rounded boxes cost 127 ms of CPU, a
  hover over a link that changed the status line 25 ms, a key typed in
  the address bar 80 ms (the page painted again for nothing).
- Pixel order: soie_paint_bgra() (libsoie 1.3.0, soname unchanged)
  writes B G R A, what X and Wayland servers take on little-endian
  machines. The three places that write pixels (colors, images, SVG)
  swap their source, not the canvas: no pass over the pixels after.
  The front picks the order from the visual (red mask 0xff0000, LSB
  first) and tells the shell (shell_bgra()).
- The fronts' common part, browser/front.c (front.h), knows what it
  changed: the toolbar alone for a key in the address bar; the page
  when the shell or the input changed it (a click, a key the page
  took, a navigation, a find); the thumb and the bottom bar (find bar
  or status line) by comparing them with what was painted, the page
  painted again under where they were. The rects painted are what is
  sent (tk_show()), merged to one when more than 8.
- Scrolling alone moves pixels: shell_paint_scroll() moves the view's
  rows and paints the band that came in. It holds because painting is
  a translation of page coordinates (sticky is static, fixed boxes
  stay where laid out: when they follow the view, they will need their
  rects painted again). shell_key() returns 2 for a key that only
  scrolled, so arrows and Page Down move pixels too.
- What tells that the page changed: shell_tick()'s repaint, and the
  document generation at the last full paint. Scripts lay out the page
  themselves (getBoundingClientRect()), leaving no update for the
  shell to see after them; YouTube's top bar stayed three grey circles
  after its sign-in button came. A generation that moved means a full
  paint, and no move of the old pixels.
- xsoie sends the pixels through MIT-SHM (XShmPutImage, an XSync
  before they are painted again) when the server is on this machine,
  XPutImage otherwise. An expose sends the kept pixels again, without
  painting.
- Painting a band of a long page cost as much as half the page: every
  text was shaped and every rounded box sampled 16 times a pixel, then
  thrown away by the clip. Text fragments out of the clip (glyphs 2em
  up, 1em down from the baseline) are skipped; a pixel inside a
  rounded box away from its corners is covered, one outside is not,
  without sampling. Pixels are byte for byte the same (checked on the
  paint tests and a long page at 3 offsets against 1.2.0); a full
  paint of the long test page went from 120 ms to 27 ms, a band from
  13 ms to 2 ms.
- Measured on a 60-card page (rounded borders, gradient, images, SVG),
  CPU of xsoie in 1/100 s: 80 wheel steps 1017 to 58, 40 hovers 101 to
  1, 30 keys in the address bar 235 to 15. YouTube while it plays does
  not move (1055): decoding and its scripts, which change the page
  all the time, take the CPU.
- A page changed by its scripts paints only what changed: the engine
  tracks damage (soie_layout_damage(), libsoie 1.3, lib/damage.c).
  Around each layout it sums the old and the new box trees up per
  element: a signature of what its boxes paint themselves (rects,
  borders, style, the text of their fragments, the DOM stamp of
  images, SVG and form controls; players always) and where, alone and
  with their subtree. A signature that changed, an element that came
  or went, gives its rects, old and new; a style that changed gives
  its subtree's (opacity, clipping, stacking reach below); the root's
  or body's style, a new viewport, the first layout give all. The
  focus or caret moved damage both controls. Ancestors of a change
  are rebuilt by the incremental layout, so "new boxes" would be all
  the page: comparing is what finds the little that moved. The shell
  paints those rects (shell_paint_changed()) unless something the
  engine does not see happened (a page, a stage, a resource: an image
  or a font that arrived, another size), then all. On a long page
  with a clock and a progress bar updated every 100 ms: 2.57 s of CPU
  in 10 s to 0.83 s; YouTube while it plays, about 70% of its
  repaints are 1 to 6 rects.
- Timers of a page's scripts waited for the first input event: the
  tick where the page arrived asked the scripts' next timer before
  they started. A tick where the page, its stage or its resources
  changed is now followed by another at once.
- The scripts' own layouts (getBoundingClientRect()) left the page
  height of the last layout the shell made: Wikipedia with scripts
  was one view high, no scroll bar until a resize. A generation that
  moved without an update now takes the new height too.
- `SOIE_DEBUG_PAINT=1` paints the window whole after each redraw into
  another buffer and reports where they differ (2 levels of rounding
  allowed), but in the players' rect: frames come from decoding
  threads. It found the generation case above. The paint test checks
  the same at the engine's level: BGRA against RGBA, bands, a rect and
  a scrolled page against the whole.

## The pop-up list of a select

- A click on a drop-down select cycled through its options: no way to
  see them, a list of fifty meant fifty clicks. It now opens a list of
  them under it, as every browser does (November 2026).
- The list is a little page the shell writes (one line per option,
  the optgroups' labels bold, their options indented, disabled ones
  in GrayText, hidden ones left out), lays out and paints itself over
  the page view, last in paint_view(), clipped to the rect painted.
  The fronts already paint their bars that way: no toolkit, the
  engine draws the text in the select's font, and the three fronts
  (xsoie, wsoie, SoieView) have it with no code of their own. The
  colours are the system colours (Field, FieldText, Highlight,
  HighlightText, GrayText) of the select's color scheme, as the
  cascade decides it: dark in a dark section of a light page.
- Its limit: it lives in the view and never reaches out of it, where
  an OS window could. It opens under the select when the options fit
  there, above when they fit there, else on the larger side, as tall
  as that side and scrolling (a thumb drawn by the shell, the wheel,
  the keys, a click on the bar pages). A pop-up window could reach
  past the view, at the price of a window, a grab and a placement in
  each front's toolkit.
- Input goes to it first, through what the fronts already call:
  shell_click() picks a row or, anywhere else, closes it and goes no
  further (the click that closes a pop-up does nothing else in
  browsers); shell_hover() highlights the row under the pointer;
  shell_scroll() scrolls it under the pointer, closes it elsewhere;
  shell_key() gives it every key before the page's keydown, as a
  pop-up window would get them. A press it took starts no text
  selection, a drag over it highlights. Up, Down, Home, End, Page Up
  and Down move, Enter, Space, Tab, F4 and Alt+Up or Down pick,
  Escape closes, letters typed within a second of each other jump to
  the first option they start (the same letter again: the next one).
  A focused select with its list closed changes at once with the
  arrows, Home, End and letters, and opens with Space, Enter, F4 or
  Alt+Down; Enter no longer sends its form.
- A click focuses the select (focus and blur events, as in browsers;
  it took no focus before) unless a script cancelled the mousedown,
  the way pages draw their own list. A pick that changes the value
  sets `selected` on that option only and fires input then change,
  once; the same option again fires nothing.
- It closes when the page scrolls, navigates or goes, the view is
  resized or zoomed, the colours change, the select loses the focus
  (a script's blur() too), and when the view loses the keyboard focus
  (shell_focus_out(): the window or the tab), which the fronts say
  (xsoie's FocusOut, wsoie's keyboard leave, SoieView's focus-out).
  Scripts run while it is open: it checks, at each tick and input,
  that the page's generation did not move, else that its select is
  still in the document (a walk that compares pointers, the select
  may be freed) and that its options make the same list; otherwise
  it closes. Nothing of the page is touched while it is not sure.
- Repainting: its rect, old and new, is a damage rect of the shell's
  own, painted by shell_paint_changed() with the engine's; a scroll
  does not move its pixels (shell_paint_scroll() says no). SoieView
  ticks after a motion so that the highlight shows.
- Not done: list boxes (multiple, size above 1) are painted as their
  first rows, without scrolling nor the selected options, and a click
  still cycles them; a long press and a release over a row (the
  drag-to-pick of Windows) does nothing; a double click on the select
  leaves the list open (front.c makes the second click a word
  selection, which the list refuses).

## wsoie: the Wayland front

- `browser/wsoie` is the second toolkit part on front.h, built when
  wayland-client, wayland-cursor, xkbcommon, wayland-protocols and
  wayland-scanner are there (xdg-shell's code is generated). Shared
  memory and the pixel order are what Soie paints: XRGB8888 is B G R A
  in memory, soie_paint_bgra() fills it with no conversion.
- The front keeps its own pixels; what it shows is copied, rect by
  rect, into one of two wl_buffers the compositor has released, each
  buffer keeping the rects it missed. A commit waits for the last
  frame callback: no frame painted that the compositor would drop,
  never a wait on the compositor's reading.
- A configure is applied (acked, buffers made, the page laid out and
  painted) only when no frame is on its way, the last one only: tisse
  sends one per pointer move while a window is dragged bigger. On
  Wikipedia, 30 moves 5 ms apart gave 31 relayouts and the last size
  1.6 s late (1.65 s of CPU); now 4 relayouts, the last size in
  0.17 s (0.22 s of CPU).
- The shared file is a mkstemp() in $XDG_RUNTIME_DIR, unlinked at
  once: SliTaz's C library has no memfd_create().
- Keys: the compositor's keymap through xkbcommon, dead keys through
  its compose table (from the locale), repeat by a timerfd at the
  compositor's rate and delay. A character key's DOM name is its
  keysym's character, so shortcuts are the same as in xsoie.
- The wheel: 4 pixels per axis unit (a notch is 15: 60 pixels, as in
  xsoie); touchpads scroll smoothly through the same path.
- Clipboard through wl_data_device version 1 (no drag and drop): Ctrl+C
  offers UTF-8 text, Ctrl+V reads the offer through a pipe in the main
  loop. No primary selection, no window icon, no fractional scale:
  tisse, the compositor it is made on, has none of these protocols.
  Server-side decorations are tisse's policy: wsoie draws none.
- Tested in a headless tisse of its own (own XDG_RUNTIME_DIR: tisse
  writes the workspace file a panel reads), grim for screenshots;
  input by hand until tisse has virtual pointer and keyboard.

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
  reverse directions and columns, wrap-reverse (lines from the cross
  end, start and end swapped in them).
- The automatic minimum size (4.5): the content size suggestion (the
  min-content size; for a replaced box with an aspect ratio, the size
  its definite cross size gives through it, clamped by its cross min
  and max), clamped by the max size, the specified size if smaller;
  0 for a scroll container, which overflow: clip is not. A column's
  content height is measured with the item's height property left
  out (box_content_height()): a height: 100px item over 80 px of
  content shrank to 100, not 80. Images in a column had 0, an image
  of width 100px in a 0 high column vanished.
- A column's item that does not stretch (align-self other than
  stretch, or auto cross margins) is fit-content wide, measured and
  laid out at that width: it was as wide as the line, nothing left to
  center or align at the end.
- In a column, the automatic minimum size is the content height (its
  height if smaller; 0 for a scroll container or a replaced box), and
  a column of auto height resolves its flexible lengths against its
  content's height clamped by its min-height and max-height. go.dev
  and rust-lang.org: body { display: flex; flex-direction: column;
  min-height: 100vh }, main { flex: 1 }, a basis of 0 (0% and 0px are
  the same once computed): <main> was 0 high, the footer drawn over
  the page; with a short page the footer now ends the screen.
- Grid: track lists with px, %, em, fr, auto, min/max-content,
  minmax(), fit-content(), repeat() including auto-fill and auto-fit
  (auto-fit collapses its empty tracks, below), line names and
  grid-template-areas; placement by line numbers (negative too),
  spans, names and areas; row (or column) flow auto placement without
  dense packing; implicit tracks from grid-auto-rows/columns; track
  sizing: fixed tracks, single-span content contributions, auto tracks
  growing toward max-content, fr sharing with inflexible tracks taken
  out; item alignment (justify/align-self and -items) and content
  alignment. Not yet: multi-span contributions, subgrid, masonry,
  baseline alignment, vw/vh in track sizes (taken as 1024x768).
- Grid tracks are maximized (12.6) before the fr sharing: the free
  space goes in equal shares to the inflexible tracks, each up to its
  growth limit (a fixed maximum, else its content). MDN's content
  column, minmax(0, 48rem) between two 1fr ones, stayed 0 wide: the
  page one letter a line.
- Content distribution of the grid (justify-content, align-content):
  start, center, end, space-between, space-around and space-evenly;
  the last two were taken as start (WPT grid-content-distribution).
- auto-fit tracks that no item is in collapse: size 0, and the gutters
  on either side of them with them (a collapsed track counts for
  nothing in positions, spans, gaps and content distribution). Their
  items then get the whole width: repeat(auto-fit, minmax(200px, 1fr))
  with two items fills the row with two, as auto-fill does not.
  Absolute items keep no track open. A row repeat gets the grid's
  definite height for its count (one row before).
- Line names before the first track ([a] repeat(...)) were dropped
  when the first track was added, and their memory with them (found
  by ASan on WPT): kept now.
- Grid lines by name (CSS Grid 8.3): `name N` is the Nth line of
  that name (from the end when N < 0), `span name` reaches the next
  line of that name from the item's other, definite line, and a name
  the grid lacks is on all its implicit lines: the item goes past the
  explicit grid, as in browsers (it was auto placed). The names inside
  repeat() are repeated with its tracks: the body is parsed apart and
  copied ([a] x [b] twice is [a] x [b a] x [b]); only the first
  repetition had them. An end line before the start swaps with it.
  An absolute item's line past the grid is auto. Not done: two name
  lists side by side (`[a] [b]`, possible after var()) are invalid
  for browsers, Soie merges them.
  The WPT subset loses grid-placement-using-named-grid-lines-007 by
  this: tests/wpt/run.sh runs no script (soie-render needs -x
  ext/script.so), the test stays in its first state, without a line
  named main, and its item now goes past the grid, red as in Firefox
  without the script; it passed by the old auto placement. With the
  script extension it passes. subgrid/line-names-004 moves the same
  way (no subgrid: its names are all missing).
- Baseline alignment (align-self/-items baseline, first and last), in
  flex lines and grid rows (items of one row): the items of a group
  share the line's largest ascent (first) or descent (last), the line
  or row grows to hold them. A box's baseline is its first (last)
  line's, else its first (last) in-flow child's at any depth
  (box_baseline()); without one it is synthesized from the border
  box's bottom. In a flex column the text runs across the cross axis:
  the baseline is synthesized from the left edge (right for the
  last), and the group goes to the cross start (end with
  wrap-reverse). An inline-flex or inline-grid sits on the line by
  its first baseline (an inline-block by its last, at any depth now,
  not only one level down). Not done: justify-self baseline (needs
  writing modes), subgrid items in their parent's groups.
- The static position of an absolute child of a flex or grid
  container (CSS Align 3): as the only item of the container, in its
  content box, placed by its self alignment (grid: justify-self and
  align-self, auto from -items; flex: justify-content on the main
  axis, space-between as start, space-around and -evenly as center,
  align-self on the cross axis); normal and stretch as start, last
  baseline as end. The container gives the rectangle and the
  alignment (abs_rect()), layout_abs() applies them once the box's
  size is known. When the grid container is the containing block, an
  absolute child's grid lines make it (abs_area()): its grid area,
  an auto line (or one past the grid) the padding edge, a span
  against auto auto too (CSS Grid 1, 9.1); its static position is in
  that area. RTL and vertical writing modes are not done
  (grid-abspos-staticpos-align-self-vertWM-last-baseline-004 passed
  by chance before, start and end meeting there).
- grid-template and grid, the shorthands: <rows> / <columns>, the
  areas form ('a a' 40px [x] 'b c' / 1fr 2fr: rows from the sizes
  after the strings, auto when missing), none, and grid's auto-flow
  forms (auto-flow dense 30px / repeat(3, 1fr)). They were not known
  at all: grids written with them were one column of auto rows (the
  references of the WPT positioned-grid-items tests).
- The max-content width of a grid gives each flexible track the
  largest max-content per fr of them (12.7.1), not its own: four 1fr
  columns of cards are four times the widest. A fit-content section
  (margin: 0 auto in a column body, www.slitaz.org's cards) was
  narrower than in Firefox.
- Text directly in a flex or grid container becomes an anonymous item,
  even when there are no block children.
- A replaced item (image, <svg>) given its width but no height takes
  the height of its natural ratio, or its natural height: it kept 0,
  and every icon and image of a flex row without a CSS height vanished
  (GitHub's octicons, whose width and height are attributes).
- The other way round, a height but width auto: its intrinsic width
  goes through the ratio too (content_sizes()), not the natural width:
  the site's 256 px SVG logo at height: 28px took 256 px of the
  header, slitaz.org's 40 px logo 40 for 28, BBC's and Ars Technica's
  icons too wide or too narrow.
- Real sites built on complex grids (Wikipedia's Vector skin, the
  Tailwind site) render largely right but not yet like browsers: the
  remaining gaps are in grid placement details and in var()-computed
  track lists.
- A column item is measured at the width it is laid out at, its
  max-width included: measured at the line's width, a centred section
  (max-width 1000px, margin auto) in a body that is a column wrapped
  its text less and got a short height, and the next section covered
  its bottom (www.slitaz.org's cards, soie.slitaz.org's).
- A flex container's height is the definite one it is given: its
  height, or one set from outside, a stretched item or an absolute
  box between its top and bottom (layout_contents() passes it).
  layout_flex() used to read the height property again: OSM's map
  (an absolute flex row between top: 55px and bottom: 0) was 0 px
  tall, and a height in % applied twice. A stretched item is laid
  out again at its line's height whenever that changes it, the
  container's height definite or not (9.4 step 11): go.dev's and
  php.net's menus centre again. A single line of auto height is
  clamped to the container's min and max heights, a column of auto
  height too, for justify-content (GitHub's header, framasoft's
  cards). +1 WPT (percentage-heights-021), layout time unchanged.

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
  or shared workers, localStorage lasts as long as the page, crypto
  has getRandomValues and randomUUID (from /dev/urandom), and of
  subtle only digest() (SHA-1, SHA-256, SHA-384, SHA-512, ext/script/
  digest.c); its other methods reject with NotSupportedError.
  ES modules with import maps (imports, not scopes).
- Limits: 5 s of running per entry (then an exception), a quarter of
  the RAM of JavaScript heap per page, 256 MB to 1 GB (see QuickJS
  2026-06-04). Errors go to the host (`SOIE_DEBUG=1`
  prints them in the browser).
- Tests: `make check-js` runs the pages of `tests/js` and compares
  their console output; `soie-render -j` runs a page's scripts.
- Cost: Wikipedia takes 0.3 s more CPU and 11 MB more with scripts.
- `<link>` fires load and error, once per element. A style sheet's
  when the engine has it: the style pass that fetches it marks the
  element, `soie_link_sheet()` tells (1 in, -1 failed, 0 on its way),
  the script engine looks every 50 ms while some wait (the first look
  runs a style pass, which asks for the sheet) and says loaded after
  30 s whatever happens. Preloads are said loaded from a task (the
  engine does not fetch them). Links of the parsed page are looked at
  from the window's load. A first version said every link loaded at
  once, as images are; but a loader that waits for a sheet means to
  measure with it. YouTube's navigation hung on it: its player inserts the
  link of its CSS and waits for load before it takes a video, so a
  click on another video changed the page and the address while the
  player went on with the first one's ads and content, with no error
  anywhere. Found with a log of every thrown exception in a scratch
  QuickJS (none: the wait was a promise that never resolved), then by
  wrapping YouTube's player objects from `SOIE_DEBUG_JS_START`. The
  loadCSS pattern (`rel=preload onload="this.rel='stylesheet'"`), on
  many sites, needed it too. Test: `tests/js/link-load.html`.
- Web Workers (dedicated ones), for hg.slitaz.org's Anubis page: its
  proof of work runs in workers (`sha256-webcrypto.mjs`, a classic
  script despite the name, crypto.subtle.digest in a loop). Each
  worker is a thread with a QuickJS runtime of its own, running
  dom.js with `api.worker` set: no document, the rest that needs none
  (events, timers, fetch, XHR, TextEncoder, URL, Blob, crypto), plus
  self, location, postMessage, close and importScripts. The page
  fetches the worker's script (blob: and data: URLs too) and hands
  it over. Messages are QuickJS's serialization (JS_WriteObject:
  objects, arrays, typed arrays and buffers, dates, cycles; a function
  throws), copied and never transferred; structuredClone() uses it
  too. A worker's console lines and uncaught errors go to its page
  (the host's message() is the main thread's), the errors as error
  events on its Worker. One lock, the request jobs': a worker sleeps
  on its condition until a message, an answer, its next timer or its
  end. Its requests, importScripts and modules too, go through the
  job threads, never its own, and interrupt() looks at its stop flag:
  terminate() or a closed page stops it at once, even in an endless
  loop, and the page waits for it, so the host's data outlives it. A
  page with workers looks for their messages every 10 ms. A worker's
  heap is a quarter of a page's, at least 64 MB; it runs without the
  5 s limit. Checked with ThreadSanitizer (gcc-83 -fsanitize=thread):
  no race. Not done: nested workers, SharedWorker, transfer lists,
  MessagePorts in messages. Anubis passes in 0.2 s (difficulty 4).
  Fixed on the way: addEventListener() called bare (Anubis's worker,
  pages' `addEventListener('load', ...)`) threw: the global's
  EventTarget methods are bound to it. Test: `tests/js/worker.html`.
- dom.js keeps the engine's Promise.prototype.then and a resolved
  promise of its own for queueMicrotask: core-js, on the BBC, finds
  the native Promise wanting, replaces it and queues its own jobs with
  queueMicrotask, which then went through its Promise again: endless,
  1 GB of heap in three seconds, a blank page. React's input
  hydration also set defaultChecked, which had no setter. Test:
  `tests/js/polyfill.html`.
- A desktop has no touch: window and elements have no ontouchstart,
  ontouchend, ontouchmove or onorientationchange, the properties
  Modernizr and others test (python.org showed its mobile layout).
  PromiseRejectionEvent exists but is not fired yet (QuickJS's
  rejection tracker is not hooked): core-js, missing it, replaces the
  native Promise, and its jobs flooded radio.net until the heap ran
  out. performance.navigation (type 0) for Le Monde's bundles;
  createElement takes DOM's valid element names ('_', ':', non-ASCII
  first, as the classList probe of mozilla.org); navigator's methods
  are writable, so pages can wrap sendBeacon. Test:
  `tests/js/compat.html`.

- Found by the same review, in the bindings: `:scope` in
  querySelector, querySelectorAll, matches and closest is the element
  asked (selector_scope(); it was the root: `el.querySelectorAll(
  ':scope > p')` found nothing). `location = url` navigates.
  Object.prototype.toString names the interface (Symbol.toStringTag on
  every global class, elements by their own): axios tells a
  URLSearchParams from an object by it and sent it as JSON. A
  javascript: link runs on the user's click, not only on click(). A
  form has its controls by name (form.q, form.email; a radio group is
  a list with a value), document.forms its forms by name: getters set
  when the controls are listed and once the page is parsed, not a
  proxy on every form. The window has the elements by their id and
  the forms and images by their name (HTML 7.3.3), set before each
  script and each event from a native list of the ids (api.ids()): a
  proxy in the window's prototype chain was tried first, and made
  every unknown name undefined where scripts expect a ReferenceError.
- On a real click a checkbox or a radio changes before its click
  event and goes back if a handler cancels it, as the synthetic
  click() did: handlers read the new state. A submitted textarea is
  sent whole (it was cut at 16 KB), a select's options are found in
  its optgroups.

- Intl exists, the smallest that keeps scripts going (dom.js, 300
  lines): NumberFormat, DateTimeFormat, Collator, PluralRules,
  RelativeTimeFormat, ListFormat, Segmenter, DisplayNames and Locale
  with English conventions whatever the locale asked, the machine's
  time zone as an Etc/GMT name. QuickJS has no Intl, and a script
  that calls it (most date and number code does) stopped its whole
  bundle on a ReferenceError (OpenStreetMap's). toLocaleString(),
  toLocaleDateString(), toLocaleTimeString() and localeCompare() go
  through it and take their options. Other languages' month names,
  separators and collations would need CLDR data: megabytes, not for
  now.

- The CSS object model, as much as scripts read of it (dom.js): a
  style element's `sheet` with `cssRules` (each rule its text, type,
  selector, declarations as a `style`, the rules of a group),
  insertRule() and deleteRule(), document.styleSheets,
  `new CSSStyleSheet()` with replaceSync() and
  document.adoptedStyleSheets (each adopted sheet is a style element
  at the end of the head). The engine has no such objects: the rules
  are cut out of the element's text when asked and a change writes
  the text back, appended when the rule goes at the end. `sheet` was
  an object with no rules: Modernizr reads a rule back to know
  whether @font-face exists, and python.org then hid its icons. The
  sheets of `<link>` elements are still not read back.
- An `<svg>` element has createSVGRect() and the other factory
  methods (plain objects), viewBox, getCTM(): Modernizr tests SVG by
  the first, and the same site hid its icons for lack of it.

- form.submit() and requestSubmit() of a POST form are carried out
  by the host, as a click on the form's button is: dom.js marks the
  form (data-soie-submit, the submitter data-soie-submitter) and
  calls navigate with the mode 3, the shell finds the marked form
  once the script returns, removes the marks and sends it with the
  guard of the page's own requests, without a submit event for
  submit(). No new field in soie_script_host: the extension ABI is
  unchanged, a host that does not know the mode loads the action as a
  link. Logins that post by script (SAML, OAuth form_post, payment
  redirects) did nothing but log "not supported". A form's controls
  by name are refreshed before each script, so that an inline script
  right after the form finds them.

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
- YouTube may stop sending media to a session: its SABR answers stay
  HTTP 200 but carry 107 bytes and no segment, the buffer runs out and
  the player says "an error occurred, refresh the page" about a minute
  into the video. Read in its own statistics (api/stats/qoe: bwm 107
  bytes per answer, then vps ER). Each answer has a
  STREAM_PROTECTION_STATUS part (UMP part 58: 1 fine, 2 attestation
  pending, 3 required): the attestation BotGuard makes in the page is
  not always trusted. Same cookies and User-Agent on the test bench
  played whole videos with status 1; pankso's sessions were cut. Not
  solved: what BotGuard finds odd in Soie is not known.
- Google Search (2026-10) asks every browser for its BotGuard
  challenge first, then reloads: Soie runs it and gets the 429 "sorry"
  page with a reCAPTCHA, with a Firefox User-Agent too, while Firefox
  from the same address and with the same consent cookie gets the
  results. The home page works (the plain one Google gives unknown
  browsers), the consent page and its forms too. The shell's search
  stays DuckDuckGo Lite.
- Bodies of fetch and XHR given as typed arrays (every segment) were
  first tried as an ArrayBuffer: QuickJS threw, with a backtrace, and
  the typed array path came next. About 30 000 exceptions a minute on
  YouTube, 260 now: the class is looked at first.
- A POST, PUT or PATCH without a body says Content-Length: 0. Some of
  YouTube's video servers answered 411 without it.
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

A second audit (October 2026, ASan and a mutation fuzzer, every
finding with its reproducer) found the first point above was only
true for a page's resources:

- A navigation of the page itself (a link, location, a meta refresh,
  a form, window.open) went to file:// too: net.c's check looks at the
  first party, and a page's load has none. scheme_allowed() now holds
  file:// like a front's scheme: only from a local page, or from the
  user (address bar, history).
- Nesting that ran the stack out: component values nest 64 deep at
  most (deeper blocks and functions are dropped, real sheets stay under
  ten), which bounds every recursive walk of rules, selectors and
  calc(); SVG groups and svgs 256 deep (by_id() walks without
  recursion); the hit test 1024 deep, its kids no longer in a 32 KB
  array on the stack of each level (2000 nested divs were enough).
- Freeing a deep document walked up to the root for each node (its
  restyle check): 80,000 nested elements took minutes. A document
  that goes marks itself restyle_all, and nothing walks.
- soie-render -a: a page taller than 65536 rows is cut there, and a
  failed allocation is said.
- Checked sound: image decoders (fuzzed, no crash), HTTP framing,
  cookies, CORS, TLS verification, download names.

A third pass (November 2026, 4,900 generated pages of hostile CSS
values, each finding kept as a minimal page) found one memory error
and loops that never ended, all from sizes no page means:

- A page coordinate becomes a pixel index through ifloor() and
  iceil() (paint.h), which bound it first: a box ten billion pixels
  away does not fit an int, the conversion is undefined, and gave
  clips wider than the canvas (a 6 px gradient tiled 100,000 times
  each way). Tiles are counted in doubles and never past the clip.
- Dashes and dots are walked in doubles and from the first one the
  clip shows: at 1e10 px a float no longer moves by one dash, so the
  loop stood still; before that, the whole side was painted dash by
  dash.
- paint_borders() read sd[-1] when no side had a width (the memory
  error).
- `flex: -1` was taken as a grow factor and the lines never
  resolved: a negative number makes the declaration invalid.
- Grid lines are clamped to 10,000 (as the specification allows) and
  the track table stops growing there; text over 2,000 px is not
  rasterized; a blur over 300 px is drawn at 300.

Found while about:settings was attacked (October 2026, hostile pages
over HTTP, real clicks, each test run again with the guard off to
check it fails then):

- history.pushState() and replaceState() took any URL: a page of
  evil.example could show about:settings or a bank's address in the
  address bar, and the shell took it for that page, so its forms went
  where only that page may (about:settings, file://). Now HTML's rule
  (soie_url_rewritable(): http and https keep their origin, file its
  path, other schemes all but the fragment): dom.js throws
  SecurityError, and the shell refuses the address on its own too
  (js_url()), either one enough.

- From a third review (shell, network, scripts), fixed:
  a body said gzip that is neither gzip, zlib nor raw deflate made
  inflate_body() try raw deflate for ever (a fetch thread at 100 %):
  tried once, then the body as it came. A response that is not HTML
  (JSON, a script, text of any case) is shown as text, never parsed
  as HTML: a JSON answer that echoes `<script>` ran it in the site's
  origin. A body cut before its Content-Length or its last chunk is
  shown but not cached (it was kept as fresh). The Host header has
  the port when it is not the scheme's. A navigation the page starts
  by itself (location, a meta refresh, a scripted click, without a
  click or a key of the user) is guarded like its other requests: a
  public page could send a GET to the router or to the machine that
  way. With the user's click it goes anywhere, as before. Known
  cost: a public page that redirects by script to a loopback address
  on its own (some command line logins) is refused.
- The same-origin and CORS checks of fetch and XMLHttpRequest lived
  in dom.js, which a page can rewrite (URL.prototype.origin,
  Headers.prototype.get: another origin's answer was read), and
  importScripts() had none (a rewritten eval got another origin's
  text). script.c decides now (cors_fetch()), from the origin C knows
  (the page's address; a worker's is its page's, "null" for data:),
  and dom.js only passes the arguments and the credentials and mode
  asked: the forbidden headers dropped and Origin set there, the
  preflight made there too (with Access-Control-Allow-Headers checked,
  a preflight not redirected), the answer from another origin given
  only when it allows it, with the headers it exposes; Set-Cookie
  never (HttpOnly cookies were readable), no-cors opaque, its custom
  headers dropped. importScripts() runs the script from C, another
  origin's errors muted; modules and import() are CORS requests; a
  worker comes from its page's origin, blob: or data:. net.c on its
  own drops Referer and Sec-*, an Origin that is not the first
  party's, and a page's PUT or DELETE redirected (307, 308) to another
  origin, which had no preflight. Checked with hostile pages on two
  local origins, each leaking before. Known: a request redirected to
  another origin goes there without Origin (browsers say null) and
  its answer is checked against the page's origin.

## Speed

An audit of where the time goes (October 2026, 12 sites copied
locally, per phase timings, every change measured before and after,
renders compared byte for byte and the incremental differential tests
run again):

- Images: PNG and JPEG sizes come from their headers, the pixels when
  painted (small ones then stay decoded, large ones are in the budget
  as before). Le Monde's first layout went from 223 to 17 ms, debian's
  from 87 to 3: it decoded every photo of the page to know its size,
  then again to paint it.
- Rules indexed further: by attribute name when a compound has no id,
  class or tag (GitHub's [data-*]), :root apart, and an :is() or
  :where() compound in the buckets of its arguments (an entry found
  twice is kept once). Rules with a state the engine does not have
  (:hover...) are not indexed: they never match. GitHub's style went
  from 515 to 430 ms.
- Found by a review of the selector and cascade code: the :is() and
  :where() index took its bucket and its key in one call's arguments,
  whose order of evaluation is the compiler's: `:is(.a, p)` (a class
  and a tag) was filed under the wrong key and never matched, only
  lists of one kind worked (`.markdown :is(h1, .h1)`,
  `:where(a, .link)`). Matching a chain of descendant combinators
  tried every ancestor at every level; match_rec() now says when no
  ancestor further up can match either (-1), as WebKit does, and the
  search stops: 1.2 s to 8 ms on six combinators over fifty nested
  divs. `&` keeps the last answers for a parent list while one
  selector is matched (`&:is(&)` nested 28 deep asked 2^28 times). A
  value after var() substitution is invalid past 256 KB (a custom
  property that doubles at each of 32 levels ran out of memory). A
  rule's selectors past the 32767th are not indexed (their index is a
  short, the 32769th read before the list).
- intern_style() hashes the 3 KB struct style 8 bytes at a time:
  -16 ms of cascade on Wikipedia.
- Setting an attribute to the value it has changes nothing (no
  restyle, no relayout): frameworks rewrite class at every render.
- Paint: opaque rectangles and rounded backgrounds fill their inner
  rows with stores, the edges stay anti-aliased: paint halved
  (Guardian 34 to 17 ms), pixels identical.
- soie_element_rect() takes the element's box from the layout that
  made it instead of searching the tree; kw() finds its cache slot by
  a hash; list items with a marker that is not a number are kept
  between layouts; face metrics are read once per face; the text width
  cache has two ways.
- A soie_node is 168 bytes, not 264: the document's fields moved out
  of the union into struct docinfo.
- dom.js is compiled once per process and its bytecode read by each
  page and worker: 8 workers start in 10 ms instead of 20 to 40.
  getElementById() is a native walk (no selector to parse), head and
  body look at the root's children, the page's baseURI is kept while
  neither its tree nor its address change: 18 times faster together.
  Custom element reactions look for the defined names only, not a
  wrapper for every element below.
- Shell: an arriving resource repaints the loading line only (its
  relayout repaints the page, at most 200 ms later); the title goes to
  the front when it changed; a page left drops its jobs no thread
  started; connecting waits 3 s for an address before the next one
  (an IPv6 route to nowhere cost 20 s), the last one still 20 s.
- media.so (6 MB of FFmpeg mapped) is loaded when a page first asks
  for audio or video, not at start: soie_ext_load_dir() keeps its path
  and soie_ext_get(SOIE_EXT_MEDIA) loads it, one thread doing it (the
  registry is read from the fetch threads too: the count is published
  after the slot). dom.js asks soie_ext_present() whether to have
  MediaSource, which loads nothing. A page without media: 18.9 MB at
  most, 14.7 now.
- Not done, for later: keeping the parsed rules of unchanged sheets
  when a script inserts one (GitHub reparses 7 MB, 370 ms): keeping
  the parsed trees would only save the parse (150 ms of it) for twice
  the rules' memory, refused; what would pay is adding an appended
  sheet (or insertRule(), which appends to its <style>) to the rules
  in place. TLS session resumption (tickets link visits: they would
  need partitioning by site and private window). An image drawn at
  its own size copied (no gain measured). Normal flow blocks are still
  laid out again at each layout.

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
- Leaks are checked with glibc `mtrace()` through an `LD_PRELOAD`
  shim, crashes with a random tag-soup fuzzer under `MALLOC_CHECK_=3`.
  Both live outside the repo for now.
- AddressSanitizer: SliTaz's gcc 6.3 is built without it, gcc83 (gcc
  8.3, `gcc-83`, installed beside it) has it. `make asan`
  (tools/asan.sh) copies the tracked files into asan-build/ with their
  dates (the next run builds only what changed, ~20 s), builds there
  with `-fsanitize=address` and runs the tests, check-js when
  QUICKJS_SRC is given; `make asan-sites` renders a dozen real sites
  whole with xsoie too. libasan is in gcc83's own directory, not the
  loader's path (a gap of the gcc83 package, libstdc++ is copied to
  /usr/lib, the sanitizers are not): the build gives its rpath. The
  first sweep found a use after free in the incremental layout (boxes
  thrown away by the fix-ups, box_drop()), a read before shrunk
  images' pixels (the distrowatch.com crash) and a test passing an int
  as a size_t.
- Other people's sites are no longer fetched at each `make asan-sites`:
  distrowatch.com answered 403 to the exact token `Soie/0.1` after our
  runs (`Soie/0.2`, any browser's UA: 200), likely blocked by hand.
  Only our servers (slitaz.org, soie.slitaz.org) are rendered live; the
  others come from local copies made once by `tools/asan.sh -m`
  (tools/bench-mirror.py, into asan-build/mirror/), served on
  127.0.0.1:8078, their own domains blocked through SOIE_BLOCK so the
  copies' scripts do not reach them either. The UA stays as it is:
  changing it to get past a block would be dodging it.

## Version and repository

- Back to 0.1.0 (soname `libsoie.so.0`, extension ABI 0.1) on
  2026-10-25: nothing was released yet, and 1.x promised a stable ABI
  to no one. The entries above that cite libsoie 1.0 to 1.3 or ABI
  1.1 are those internal numbers, kept as they were written. The
  headers no longer say "since" a version: the history starts at the
  first release.
- The version stays 0.x until a first public release, with its own
  site (soie.slitaz.org). That release is tagged in the repository.
- Mercurial, like the other SliTaz tools, hosted on the SliTaz
  servers, not on a forge. The git history (106 commits, from
  2026-10-22) was converted with `hg convert`, dates and messages as
  they were.
- A read-only git mirror on GitHub (October 2026), for those who clone
  with git: tools/git-mirror.sh rebuilds it from the Mercurial history
  with git fast-import (no hg-git, no converter to install), the same
  commits each time, so a push only adds. hg.slitaz.org stays the
  repository, maille.slitaz.org the place for bugs; the mirror has no
  issues. Its pull requests are welcome all the same:
  tools/pr-import.sh applies one (gh pr diff --patch, hg import) for
  review, commits it with its authors once approved, and closes it
  with links to the Mercurial revisions. Not Codeberg: its terms
  refuse code written by language models, which Soie is.
- 0.1.1 (2026-10-29): history.pushState() and replaceState() keep
  the page's origin (a 0.1.0 page could take another's address, see
  the security audits); about:settings for xsoie and wsoie; the
  desktop's dark or light theme; the logo and the window icon;
  flex columns, replaced boxes sized by their ratio, SVG curves
  smooth at any size; checkboxes and radios as browsers draw them.
  Same soname and extension ABI: one function added
  (soie_url_rewritable()).
- 0.1.2 (2026-10-31): xsoie on 16-bit, 15-bit, 24-bit and 8-bit
  colormap X servers (it crashed on the old cards of the xsoie
  flavor); boxes painted on whole pixels; grid shorthands, content
  distribution, auto-fit collapsing, absolute children placed in
  grid areas and by their alignment; baseline alignment, wrap-reverse
  and the automatic minimum size of flex items. WPT layout subset:
  flexbox 44 to 61 %, grid 23 to 46 %. Same soname and extension ABI,
  no public function added.
- 0.1.3 (2026-11-04): HTTP Basic logins (about:auth), an HTTP cache in
  memory, the private network guard; box-shadow, mitred and rounded
  borders, elliptical corners and rounded clipping; flex containers
  of a height set from outside (OpenStreetMap's map), details and
  summary, fonts by width; JavaScript fixes for desktop sites, ALTCHA
  captchas and workers. Against Firefox on a survey of 38 sites:
  OpenStreetMap 0.20 to 0.065 RMSE, debian.org 0.26 to 0.10,
  github.com 0.10 to 0.046. WPT layout subset: flexbox 62 %, grid
  46 %. Same soname and extension ABI; ext.h gains two request flags
  (SOIE_REQ_NO_LAN, SOIE_REQ_NO_LOOPBACK) and SOIE_REQ_PAGE, no
  public function added.
- 0.1.4 (2026-11-10): a review of the whole engine and a pass on
  standards: aspect-ratio, SVG painted from the cascade, backgrounds
  and gradients rewritten, line breaking, floats, counters, keyframes'
  end state and animation events, transforms, form controls, CSSOM,
  named access on window and forms; the same-origin policy and CORS
  of scripts' requests enforced in C; bounds found by fuzzing; Next.js
  sites (removeAttributeNode, a minimal shadow root). Against Firefox
  on 24 live sites: slitaz.org 0.061 to 0.032 RMSE, Hacker News 0.156
  to 0.062. WPT layout subset: flexbox 66 %, grid 49 %. Same soname
  and extension ABI; one public function added, soie_animations()
  (soie/style.h).
- 0.1.5 (2026-11-11): shadow DOM. Shadow trees render through the
  flat tree, with slots, scoped styles (:host, ::slotted, ::part,
  adopted sheets), events retargeted at each root and declarative
  roots; :defined, :state(), color-scheme and light-dark() element
  by element. An independent review of it found 16 defects, fixed
  before the release. MDN's chrome, reddit's feed and the Lit,
  Shoelace and Ionic sites render. Same soname and extension ABI;
  nine public functions added (soie/dom.h): soie_attach_shadow(),
  soie_shadow_root(), soie_shadow_host(), soie_shadow_open(),
  soie_shadow_css(), soie_shadow_claim(), soie_assigned_slot(),
  soie_element_set_defined(), soie_element_set_states().
- 0.1.6 (2026-11-11): what real pages missed most after 0.1.5.
  WOFF2 fonts (FreeType with brotli), Content-Encoding: br, a pop-up
  list for select, the dark canvas from color-scheme, small caps,
  tab-size, elliptical radii, grid lines by name, the line-clamp
  ellipsis. The WPT run now really runs the tests' scripts: flexbox
  66 %, grid 51 %. Same soname, extension ABI and public API. The
  packages need libbrotli; without it at build time the engine is
  the same, less br.
- The site's figures are rounded so that they stay true across
  releases: "under 50,000 lines of code" (tools/loc.sh: 45,503 in
  0.1.2, 47,548 in 0.1.3, 51,661 in 0.1.4, 53,444 in 0.1.5, 54,517
  in 0.1.6: "under 50,000" became "under 55,000"; "about 45,000" was
  out of date one release later). At each release, check them all the
  same: tools/loc.sh, the WPT percentages (tests/wpt/run.sh; the site
  said 44 and 23 % until 0.1.3, when they were 62 and 46), the
  download size (soie and xsoie packages: 725 KB in 0.1.3, 773 KB in
  0.1.5: "700 KB" became "under 800 KB"), then tools/releases-atom.sh.

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
