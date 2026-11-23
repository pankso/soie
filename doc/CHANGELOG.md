# Changelog

All notable changes to Soie. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions
follow [Semantic Versioning](https://semver.org/). The reasons behind
each change are in [DECISIONS.md](DECISIONS.md).

## [Unreleased]

## [0.1.10] - 2026-11-24

### Added
- Translations (gettext, domain `soie`, `po/`): SoieView's context
  menu and the shell's pages (error, login, settings, downloads) follow
  the locale of GTK browsers; French comes first. xsoie and wsoie stay
  in English.
- Extension ABI 0.3: `download` appended to `soie_script_host`; a 0.3
  script extension wants a 0.3 host.
- `<a download>` links to blob: and data: URLs are saved as downloads
  (GitHub's download button, Filature bug 1062).

### Fixed
- fetch() and XHR of the page's origin redirected to another origin
  that answers `Access-Control-Allow-Origin: *` were refused (GitHub
  raw files).

## [0.1.9] - 2026-11-18

### Added
- Extension ABI 0.2: `frame_for` and `next_frame` appended to the media
  hooks (older extensions and engines keep working).
- file:// URLs answer Range requests: local films are not read whole.
- `SOIE_MEDIA_SLOW=N` burns N ms per decoded frame (tests of the
  frame dropping).
- Library: `soie_layout_images()` asks again for the images that were
  on their way and tells whether a layout is needed (2), painting the
  damage is enough (1) or nothing came (0).
- `SOIE_DEBUG_PAINT=1` checks gsoie's partial redraws too.

### Changed
- Scrolling: blurred box shadows are kept as nine-part tiles (2 MB at
  most) instead of blurred at every paint, and a paint leaves the
  boxes whose subtree is out of the canvas instead of walking the
  whole tree; sticky and fixed boxes are listed once per layout. A
  scroll step of the SliTaz home page costs 3.5 times less, same
  pixels.
- Faster painting in the distribution's packages: the pixel loops are
  built at -O2 whatever the CFLAGS (the -Os of the SliTaz packages
  cost a quarter of the paint time), blends divide by 255 with shifts
  (div255()). A viewport of www.slitaz.org: 49 ms to 38.
- Gradients are painted from a table of their colors, filled as
  pixels need it, with a fixed point index: no float per pixel, no square root
  for radial gradients, vertical ones fill their rows. Hard stops and
  steep gradients keep the exact path; the result is within one level
  per channel of the former one. www.slitaz.org's viewport: 38 ms to
  26; a 1000x600 linear gradient at 45 degrees: 21 ms to 1.4, a
  radial one 26 to 2.2.
- QuickJS built from QUICKJS_SRC is compiled at -O2 -fwrapv whatever
  the CFLAGS, as upstream requires.
- Video: frames converted once at the size and in the byte order the
  page shows them and copied by rows; the page under a playing video
  is not painted; ticks at the frames' times (60 fps shows 60). A
  1080p60 clip in a 960 px box: 8.4 s of CPU for 10 s of playback to
  2.4 s, 217 frames shown to 597.
- Video on slow machines: late frames are not converted, the decoder
  skips the loop filter and unreferenced frames, then B-frames, then
  all but key frames, and comes back when it has time; the page's
  own slowness makes it convert fewer frames; the sound no longer
  stops while the page is busy.
- xsoie and Toile (SoieView) paint a scroll once the queued wheel and
  key events are read: a slow machine jumps to where the wheel is
  instead of falling seconds behind.
- An image that comes after the first paint and does not change its
  box (width and height given, same natural size) is painted alone, or
  not at all below the view: no layout, no paint of the whole page.
  One that changes its box lays out and paints what moved.
- Late images wait for twice what the last pass cost (200 ms to 2 s),
  counted from the end of its paint: a slow machine takes more images
  per pass.
- A paint no longer asks for an image on its way (`SOIE_FETCH_LATER`):
  a layout or `soie_layout_images()` does. A host that only paints
  calls `soie_layout_images()` when its resource came.
- Scrolling paints sticky and fixed boxes at their width, not the
  view's, and not the rows the band that came in already painted.

### Fixed
- The tiles of a repeated background are at the same place whatever
  part of the page is painted (a band could differ from the whole
  page by a pixel at tile edges).
- A page grew at each late image when an inline or text box kept from
  the last layout was below a moved box (blank space after its end).
- A fixed box holding text had the whole view painted at each scroll
  step.
- The rects of a change reached up to the page's origin when the
  element held text: much more was painted than what changed.

## [0.1.8] - 2026-11-16

### Added
- Text fields edit as in other browsers (Toile, xsoie, wsoie): a click
  places the caret where it is; Shift+click, a drag, a double or
  triple click select; Shift with the arrows, Home and End selects;
  Ctrl+arrows move by words (Ctrl+Shift selects them); typing, paste
  and Backspace replace the selection; Ctrl+A, Ctrl+C, Ctrl+X work in
  a field. The selection is painted, a long input scrolls to keep the
  caret in sight. Textareas: Enter makes a new line, Up and Down move
  between lines, the caret is drawn.
- Scripts: `selectionStart`, `selectionEnd`, `selectionDirection`,
  `setSelectionRange()` and `select()` of inputs and textareas. A value
  set by a script puts the caret at its end.
- Textareas wrap their lines at spaces, a word wider than the field
  is cut; Up, Down, Home and End move by the lines shown. `wrap="off"`
  keeps lines whole. The text stays inside the field.
- Library: `soie_set_focus_range()`, `soie_get_focus_anchor()`,
  `soie_field_offset()`, `soie_field_move()`.
- `tools/gui-smoke.sh`: clicks and types through a browser on its own
  Xvfb and fails if it crashes.

### Fixed
- GTK view (Toile, gsoie): Tab, Shift, Control, Alt, Caps Lock and
  F1-F12 pressed in a text field typed their names ("TabShift").
  Named keys now type nothing; pages still see them in `keydown`.
- GTK view: the caret of a text field did not move on screen with the
  arrow keys, Home and End (the text went in the right place).
- `<textarea>`: what was typed was not its `value` for scripts nor in
  `FormData` (DuckDuckGo's search box, a textarea, sent an empty
  query: Enter did nothing). A textarea already filled lost its text
  at the first key, and a value over 16 KB was cut when edited.
- Scripts: `mouseup` and `click` on a text field saw the caret where
  it was before the click (`selectionStart`). The press places it
  first, as in other browsers; a cancelled `mousedown` leaves the focus
  where it was.

## [0.1.7] - 2026-11-14

### Added
- `position: sticky`: a sticky box stays in the view while its
  container is there (headers and bars that follow the page, table
  headers), with `top`, `bottom`, `left` and `right`. It was laid out
  as a static box and scrolled away. The view is the only scrollport:
  inside a box that clips its overflow, a sticky box stays static.
  Library: `soie_layout_scroll()` tells the engine the host's scroll
  offset.
- `position: fixed` follows the view: bars, menus and buttons fixed
  to the window stay there while the page scrolls. They scrolled away
  with the page.

### Fixed
- A local file opened at an anchor (`file:///page.html#part`) or with
  a query was not found: the fragment was taken for the file's name.
  Percent escapes in `file://` URLs are decoded, and a path typed with
  a `#`, `?` or `%` in it opens that file.

## [0.1.6] - 2026-11-11

### Added
- `Content-Encoding: br` over HTTPS, when brotli's decoder
  (libbrotlidec) is there at build time: pages and scripts come 15
  to 20 % smaller than with gzip.
- A drop-down `<select>` opens a pop-up list of its options on a
  click, or with Space, Enter, F4 or Alt+Down: the current option
  highlighted, `<optgroup>` labels bold with their options indented,
  disabled options greyed, in the select's font and the system colours
  of its color scheme. The pointer, the arrows, Home, End, Page Up and
  Down move in it, a click, Enter or Space picks, Escape or a click
  elsewhere closes it, letters jump to the option they start. A long
  list fits in the view and scrolls; a select at the bottom opens it
  upwards. A click cycled through the options before, with no way to
  see them. A focused select also changes with the arrows and letters,
  list closed. The shell paints the list over the page: xsoie, wsoie
  and SoieView all have it.
- A page dark by `color-scheme` alone (`:root { color-scheme: dark }`,
  `<meta name="color-scheme" content="dark">`, or `light dark` when
  the user prefers dark) gets the dark canvas, light text, links and
  dark form controls, as in Firefox: it stayed white with black text.
  System colors (Canvas, CanvasText, ButtonFace...) follow the
  element's color scheme.
- `font-variant: small-caps` (and `font-variant-caps`, `all-small-caps`,
  the `font` shorthand's small-caps): small caps were drawn as plain
  lowercase. Synthesized as in Firefox, capitals at 0.8 of the size.
- `tab-size` (a number of spaces or a length): tabs in `<pre>` were
  always 8 columns, counted from the start of each text node, so a tab
  after a `<b>` or in a proportional font missed its stop. Tab stops
  are now measured from the content edge, as in browsers.
- WOFF2 web fonts, when FreeType is built with brotli: most sites'
  fonts were skipped and their text came in DejaVu, with other
  widths. A FreeType without brotli falls back as before.

### Fixed
- `-webkit-line-clamp` drew no ellipsis at the end of its last line:
  clamped texts stopped short without a sign of it. `line-clamp` is
  accepted as its alias.
- `border-radius: a / b` and the two values of `border-*-radius`:
  the radii down were dropped, elliptical corners came out circular.
- Grid placement by line names: `grid-column: col 3` (the third line
  named col), `span name`, names repeated by `repeat(4, [col] 1fr)`
  and names the grid does not have (past it, as in browsers) all put
  items in the first columns.

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

[0.1.10]: https://hg.slitaz.org/soie/rev/0.1.10
[0.1.9]: https://hg.slitaz.org/soie/rev/0.1.9
[0.1.8]: https://hg.slitaz.org/soie/rev/0.1.8
[0.1.7]: https://hg.slitaz.org/soie/rev/0.1.7
[0.1.6]: https://hg.slitaz.org/soie/rev/0.1.6
[0.1.5]: https://hg.slitaz.org/soie/rev/0.1.5
[0.1.4]: https://hg.slitaz.org/soie/rev/0.1.4
[0.1.3]: https://hg.slitaz.org/soie/rev/0.1.3
[0.1.2]: https://hg.slitaz.org/soie/rev/0.1.2
[0.1.1]: https://hg.slitaz.org/soie/rev/0.1.1
[0.1.0]: https://hg.slitaz.org/soie/rev/0.1.0
