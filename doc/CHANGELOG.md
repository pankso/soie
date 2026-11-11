# Changelog

All notable changes to Soie. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions
follow [Semantic Versioning](https://semver.org/). The reasons behind
each change are in [DECISIONS.md](DECISIONS.md).

## [Unreleased]

### Added
- WOFF2 web fonts, when FreeType is built with brotli: most sites'
  fonts were skipped and their text came in DejaVu, with other
  widths. A FreeType without brotli falls back as before.

## [0.1.5] - 2026-11-11

### Added
- Shadow DOM: shadow trees render in place of their host's children,
  `<slot>` elements show the host's children (named, default,
  fallback content). Their `<style>` and adopted constructed sheets
  apply to them only, with `:host`, `:host()`, `:host-context()` and
  `::slotted()`; the document's rules stay out. Custom elements
  upgrade inside shadow trees. Scripts get `assignedNodes()`,
  `assignedElements()`, `assignedSlot`, `getRootNode({ composed })`,
  `isConnected` through hosts. Web components (Lit, Stencil...)
  showed nothing. Library: `soie_attach_shadow()`,
  `soie_shadow_root()`, `soie_shadow_host()`, `soie_shadow_open()`,
  `soie_shadow_css()`, `soie_assigned_slot()`.
- `color-scheme`, and `light-dark()` follows it element by element:
  a dark section of a light page (MDN's home page header) was light,
  its text unreadable.
- `:defined` is real: a custom element matches it once its class
  came. Pages hide their components until then
  (`x-menu:not(:defined) { display: none }`), and lit.dev showed its
  mobile drawer over the page. Library: `soie_element_set_defined()`.
- `import.meta.url` in module scripts and imported modules: reddit's
  loader resolved its lazy chunks against `undefined`.
- `attachInternals()` and a minimal `ElementInternals` (states,
  validity, the shadow root): Material's components threw without it.
  Its `states` is a `CustomStateSet`: `:state(x)` and the older
  `:--x` match them. Library: `soie_element_set_states()`.
- `::part()` (CSS Shadow Parts), the styling interface of Shoelace,
  Web Awesome, Ionic and Material components: `x-tab::part(base)`,
  several names, `::part(x)::before` / `::after`, `:host::part(x)`
  from the component itself, and `exportparts` to forward the parts
  of nested components. An outer tree's `::part()` declarations win
  over the component's own, its `style` attributes included, and lose
  for `!important`. Scripts get `element.part`.
- `window.cookieStore` (get, getAll, set, delete) over
  `document.cookie`, without change events.
- Shadow DOM events: the path goes through the slots of slotted nodes
  and from a shadow root to its host for composed events (the user's
  clicks, keys, focus, input...; `composed: true` for scripts'),
  listeners outside a tree see its host as `target` and
  `relatedTarget`, `composedPath()` hides closed trees from outside,
  a host gets its tree's events at target. `document.activeElement` is
  the host of a focused element of a shadow tree,
  `shadowRoot.activeElement` the element; `elementFromPoint()` gives
  the host. `slotchange` events.
- Declarative shadow DOM: `<template shadowrootmode=open|closed>` in a
  parsed document gives its parent a shadow root (not in `innerHTML`).
- The browser follows a link or a button around a shadow tree or
  around the slot of a slotted node, edits text fields, checkboxes and
  details inside shadow trees; a form of a shadow tree submits its own
  controls, an outer form leaves the tree's.
- `:host` inside `:is()` and `:where()`; animation events in shadow
  trees; `document.adoptedStyleSheets.push()` applies.
- `attachShadow()` on a host the parser gave a declarative root of
  that mode empties it and returns it, as browsers do: server-side
  rendered components (Lit SSR, Stencil, Enhance) threw at upgrade.
  Library: `soie_shadow_claim()`.
- `delegatesFocus`: a host's `focus()` goes to the first focusable
  element of its tree; `clonable` and `serializable` reflect the
  options; `getHTML()` and `setHTMLUnsafe()` on shadow roots (without
  shadow roots inside). `focus()` and `blur()` fire `focusout`, focus
  events carry `relatedTarget`.

### Changed
- `attachShadow()` throws `NotSupportedError` on elements that cannot
  host a shadow root (`img`, `i`...), as browsers do.
- Events at their target run their capturing listeners first, as
  browsers do now (they ran in the order they were added);
  `composedPath()` is empty once the event was dispatched.
- Tree context in the cascade comes before the style attribute, as
  CSS Cascade 5 says: a component's `:host { x !important }` wins over
  the host's `style="x !important"` (it lost).
- Shadow roots that adopt the same style sheets (every instance of a
  Lit component) share one parse of them: 300 instances added one by
  one took over 5 s (the script was stopped), now 0.1 s. Shadow trees
  whose `<style>` elements have the same text (server-side rendered
  components) do too: 4000 such components took 0.69 s to lay out,
  0.14 s now, as many plain elements 0.12 s.
- `soie_attach_shadow()` takes the elements DOM's `attachShadow()`
  takes only, and `soie_node_free()` of a shadow root that still has
  its host does nothing: the host's boxes kept its nodes.

### Fixed
- Shadow DOM, after a review: `:host::before` and `::after` styled the
  host itself (no pseudo-element, the host red or 30 px high); a
  component's `.dark :host`, `body :host span` or `.a + :host` matched
  the host's real ancestors and siblings; cascade layers were global
  to the page, a component's `@layer` order changed the document's;
  shadow trees nested 100,000 deep overflowed the stack and 20,000
  took 3 s; a `light-dark()` of the wrong syntax was kept, and
  `@supports` said yes to it.
- Closed shadow roots were readable by any page: through the event's
  `_path`, `new ElementInternals(host)`, `host._internals` and the
  `_roots` of an adopted sheet. `ElementInternals` and
  `CustomStateSet` cannot be constructed by pages any more.
- A parsed shadow root fired `slotchange` on every slot at its first
  change; focus moving inside one shadow tree fired `blur`, `focus`
  and `focusin` on its host; a `relatedTarget` that is the host did
  not stop the path there; `adoptedStyleSheets` gave another object
  after each assignment (a list kept and pushed to undid the change);
  `attachInternals()` worked on any element; 2000 roots sharing a
  sheet took 0.45 s to adopt it (37 ms now).
- A custom element moved in the page gets `disconnectedCallback`
  before `connectedCallback`; `stopImmediatePropagation()` in an `on*`
  handler stops the listeners after it.

## [0.1.4] - 2026-11-10

### Added
- `aspect-ratio` on every box: cards, video embeds and thumbnails
  sized by a ratio were 0 px high.
- SVG in a page styled by CSS: `fill`, `stroke` and the other paint
  properties from rules, classes, `currentColor` and `var()`, a
  `<style>` inside the SVG, sizes with units. Icon systems that paint
  through a class were black or missing.
- Gradients as real background images: `background-size`,
  `-position` and `-repeat` apply to them; repeating and conic
  gradients, radial shapes, sizes and centres, color hints, stops with
  two positions.
- `background-origin` and `background-clip`, `background-repeat:
  space` and `round`, positions of three and four values, sizes and
  positions with units and `calc()`.
- `@keyframes` and `animation`: nothing moves, but an element that an
  animation leaves in its last keyframe (`forwards`) is shown there;
  content faded in from `opacity: 0` stayed invisible. Scripts get
  `animationstart` and `animationend`.
- Transforms that rotate, scale or skew (`rotate()`, `scale()`,
  `skew()`, `matrix()`, the `rotate`, `scale` and `translate`
  properties, `transform-origin`): carets, chevrons and flipped icons
  were drawn as they lie.
- `background-clip: text` and `-webkit-text-fill-color`: gradient
  text.
- `text-overflow: ellipsis`.
- More `-webkit-` names of properties Soie has (flex, transform,
  box-shadow, background-clip...).
- Form controls sized as in browsers (text fields, selects as wide
  as their options, list boxes) and drawn: `range`, `color`, `file`,
  `<progress>`, `<meter>`; unstyled controls get a thin rounded ring.
- JavaScript: a minimal `Intl` (English conventions) and the
  `toLocale...` methods with their options.
- JavaScript: style sheets as objects (`sheet.cssRules`,
  `insertRule()`, `document.styleSheets`, `new CSSStyleSheet()`,
  `adoptedStyleSheets`), the factory methods of `<svg>`: sites that
  test features with Modernizr no longer hide their icons.
- `revert`.
- CSS counters: `counter-reset`, `counter-increment`, `counter-set`,
  `counter()` and `counters()`.
- A positioned inline box is the containing block of its absolute
  descendants.
- Lines break after hyphens and slashes, and between Chinese,
  Japanese and Korean characters.

### Changed
- Lines are as high as in Firefox and Chrome: font ascent and descent
  in whole pixels (19 px for DejaVu Sans at 16 px, it was 18.625).
- The background images of the root or of the body cover the whole
  page, as their color did.
- Background images are placed from the padding box, as specified,
  not from the border box.

### Security
- The same-origin policy and CORS of `fetch()`, `XMLHttpRequest`,
  modules and `importScripts()` are enforced by the engine, from the
  origin it knows: a page that rewrote the objects of its own scripts
  (`URL.prototype.origin`, `Headers.prototype.get`, `eval` in a
  worker) could read other origins' answers, send forged `Origin`
  headers and read HttpOnly cookies from `Set-Cookie`.
- A response that is not HTML (JSON, a script) is never parsed as
  HTML.
- A navigation started by the page alone is held by the private
  network guard, like its other requests.
- An endless loop on a body wrongly said compressed.

### Fixed
- Floats keep their source order when one waits for the next line; a
  transformed box holds its absolute descendants.
- Deeply nested flex containers took exponential time to lay out (4.5
  s for 22 levels, now 4 ms).
- JavaScript: `form.submit()` of a POST form (logins and payments
  that post by script).
- JavaScript: `:scope` in queries, `location = url`, forms' controls
  and the page's elements by name (`form.q`, `out.textContent`),
  `javascript:` links, `Object.prototype.toString` on platform
  objects, checkboxes changed before their click event.
- Forms: long textareas sent whole, options in an `<optgroup>`.
- The Host header carries the port; a body cut short is not cached.
- `white-space: nowrap`: a nowrap run moves to the next line whole,
  inline-blocks no longer wrap inside it.
- A word made of several inline boxes (`<b>W</b>ord`, a comma after a
  link) is no longer cut between them.
- `flex-direction: row-reverse` and `column-reverse` start from the
  other edge.
- Tabs in preformatted text.
- A float was hidden by the background of the block after its
  parent.
- `<details>` without a `<summary>` shows "Details".
- Inline-blocks, inline images and inline boxes moved by `position:
  relative`.
- Tables: a cell's `height` is a minimum (text ran over the next
  row), the table's `height`, `<col>` and `<colgroup>` widths.
- Grid: heights in percent or from outside, `min-height`, items
  spanning several tracks, images as items, `calc()` and viewport
  units in track sizes.
- Floats: a shrink-to-fit box holds its floats side by side; a box
  that does not fit beside a float goes below it.
- Absolute boxes centred with `margin: auto` between their offsets
  (modals, spinners) stayed in the corner.
- `:is()` and `:where()` with arguments of different kinds (a class
  and a tag) never matched.
- A stack overflow on a gradient of more than 32 stops, a crash on
  grid lines beyond 46340, on 200000 nested `display: contents`, on a
  rule of more than 32767 selectors; endless loops and memory blow-ups
  on huge background positions, shadow blurs, `var()` chains, `<use>`
  trees, roman list markers and long descendant selectors.
- Next.js sites (free.fr) showed their error screen:
  `removeAttributeNode()` was missing and `attachShadow()` threw. A
  shadow root now exists for scripts; its content is not rendered yet.
- Sizes no page means (boxes of 1e10 px, `flex: -1`, grid lines in
  the millions, giant SVG text) froze the painter or the layout, and
  one read a byte before an array: found by fuzzing, all bounded.

## [0.1.3] - 2026-11-04

### Added
- HTTP Basic logins: a page that answers 401 opens about:auth, the
  login is kept in memory for its site and forgotten with the cookies
  (CUPS administration, routers).
- An HTTP cache in memory (Cache-Control, ETag and Last-Modified
  revalidation), partitioned by the site of the top page: back to a
  Wikipedia article in 0.4 s instead of 1.35.
- A private network guard: a public page reaches no address of the
  local network nor of the machine, a local network page none of the
  machine.
- `box-shadow`, outer and inset, with spread and blur.
- Elliptical corners: `border-radius: 50%` of a box that is not
  square is an ellipse.
- `overflow`, images and background images clipped to rounded
  corners.
- `<details>` and `<summary>`: closed, only the summary shows; a
  click on it opens or closes it.
- `display: -webkit-box` with `-webkit-line-clamp`.
- Fonts chosen by width (condensed, expanded), `font-stretch` in the
  `font` shorthand.
- Grid lines named after an area's edges (`name-start`, `name-end`).
- JavaScript: `crypto.subtle` HMAC and PBKDF2, raw keys (ALTCHA
  captchas, Mojeek), `performance.navigation`,
  `PromiseRejectionEvent`.

### Changed
- Border corners are mitred: CSS triangles (the carets of Bootstrap
  and many menus) are triangles; rounded borders keep each side's
  color and style; dots are round.
- JavaScript: no touch event handlers, as desktop browsers: sites no
  longer take Soie for a phone.
- `vertical-align: super` and `sub` raise and lower the glyphs, text
  decorations stay at their box's baseline.
- FormData bodies are sent as multipart/form-data, fetch() bodies
  bring their content type.
- `element.style` gives colors back as `rgb()`, as browsers do.

### Fixed
- A flex container lost a height set from outside: OpenStreetMap's
  map was blank. Stretched items are laid out again at their line's
  height, a single line keeps its min and max heights.
- Links inside an inline box (Hacker News) could not be clicked.
- about:blank gave an error page.
- fetch() and XMLHttpRequest in workers never worked.
- A timer set by a promise waited for another event (ALTCHA froze).
- Cookie expiry dates took the day of the month as the hour.
- The page height took in fixed boxes, and missed what overflows the
  root.
- Quoted `url()` and `url()` in custom properties were resolved
  against the page, not their style sheet: missing icons and
  backgrounds.
- `document.createElement()` refused valid names.
- Images with min or max sizes and both sizes auto kept their ratio
  as CSS 2.1 says.
- The caret of a password field is placed among its bullets.

## [0.1.2] - 2026-10-31

### Added
- Baseline alignment in flex lines and grid rows; inline-flex and
  inline-grid sit on their first baseline.
- `flex-wrap: wrap-reverse`.
- The static position of absolute children of flex and grid
  containers follows their alignment; a positioned grid container is
  the containing block of its absolute children's grid areas.
- The `grid` and `grid-template` shorthands.
- Grid: `space-around` and `space-evenly` content distribution,
  `auto-fit` collapsing its empty tracks, `auto-fill`/`auto-fit` rows
  in a grid of definite height.

### Changed
- Backgrounds and borders are painted on whole pixels, as browsers
  paint them: sharp table grids, no line of a box showing through
  another at the same place. Border widths are whole pixels.
- Connections to a page's server are opened while the page loads, for
  its resources: Hacker News loads in 1.2 s instead of 1.9.

### Fixed
- Flex items' automatic minimum size: images (through their aspect
  ratio), `overflow: clip`, a column item's content height without
  its own height.
- A column's item that does not stretch is as wide as its content,
  not the line.
- The max-content width of a grid with `fr` columns.
- xsoie crashed at the first paint on a 16-bit X server (old cards,
  fbdev, cirrus); it now converts to 16-bit, 15-bit, 24-bit and 8-bit
  colormap visuals.
- Grid line names given before the first track were lost (and
  leaked).
- `soie.h` gave the version as 0.1.0 in 0.1.1; `make check` now
  compares it with the Makefile's.

## [0.1.1] - 2026-10-29

### Added
- about:settings, the settings page of xsoie and wsoie, kept in
  `~/.config/soie/settings`.
- The fronts follow the desktop's dark or light theme.
- The logo, an S of three silk threads: icons, window icon, favicon.

### Changed
- Checkboxes and radio buttons are drawn as browsers draw them,
  placeholders are faded.
- SVG curves are cut finer when they are large on screen.

### Fixed
- history.pushState() and replaceState() keep the page's origin: in
  0.1.0 a page could take another site's address (security).
- Flex column items are measured at their laid out width, max-width
  included.
- The auto width of a replaced box with a height follows its ratio.

## [0.1.0] - 2026-10-28

### Added
- First public release.
- libsoie: HTML parsed as the WHATWG says (html5lib tree tests), the
  CSS cascade (css-parsing-tests), blocks, inline text, tables,
  flexbox, grid and SVG, painted with FreeType and HarfBuzz;
  incremental restyle and layout.
- Extensions: cookies, content blocking, WebP, JavaScript with
  QuickJS (DOM, fetch, workers), audio and video with FFmpeg and
  Media Source Extensions.
- Browsers: xsoie (X11), wsoie (Wayland), SoieView (GTK 3 widget).
- soie-render, a headless renderer for tests and screenshots.

[0.1.5]: https://hg.slitaz.org/soie/rev/0.1.5
[0.1.4]: https://hg.slitaz.org/soie/rev/0.1.4
[0.1.3]: https://hg.slitaz.org/soie/rev/0.1.3
[0.1.2]: https://hg.slitaz.org/soie/rev/0.1.2
[0.1.1]: https://hg.slitaz.org/soie/rev/0.1.1
[0.1.0]: https://hg.slitaz.org/soie/rev/0.1.0
