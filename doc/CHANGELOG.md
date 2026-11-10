# Changelog

All notable changes to Soie. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions
follow [Semantic Versioning](https://semver.org/). The reasons behind
each change are in [DECISIONS.md](DECISIONS.md).

## [Unreleased]

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
  content faded in from `opacity: 0` stayed invisible.
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
- A response that is not HTML (JSON, a script) is never parsed as
  HTML.
- A navigation started by the page alone is held by the private
  network guard, like its other requests.
- An endless loop on a body wrongly said compressed.

### Fixed
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

[0.1.3]: https://hg.slitaz.org/soie/rev/0.1.3
[0.1.2]: https://hg.slitaz.org/soie/rev/0.1.2
[0.1.1]: https://hg.slitaz.org/soie/rev/0.1.1
[0.1.0]: https://hg.slitaz.org/soie/rev/0.1.0
