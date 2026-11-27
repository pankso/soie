# To do

What comes next, most useful first. Done items leave this list for
the CHANGELOG; the reasons of a choice go to DECISIONS.md. Measures
against Firefox: tests/wpt, and screenshots of real sites compared
by RMSE (the 38 sites survey of 2026-11-03).

## Rendering, in the plan

- Iframes, the next steps (the first, pages without scripts, is in):
  scripts in an iframe's page, in a runtime of its own (embedded
  videos, maps); a realm shared with same-origin parents
  (contentWindow, contentDocument); postMessage across origins
  through an opaque WindowProxy (payment forms, Turnstile, hCaptcha);
  third-party cookies partitioned by top-level site. Text selection
  inside an iframe.
- Form controls: list boxes (`<select multiple>`, `size` above 1)
  that scroll, show and pick their selected options, a `range` that
  slides, a color picker, date fields.
- Shadow DOM, what is left: `slot.assign()` (manual assignment),
  `setHTMLUnsafe()` / `parseHTMLUnsafe()` with declarative roots,
  `getHTML()` with shadow roots, `clonable` roots cloned with their
  host, the `shadowrootdelegatesfocus` / `-clonable` /
  `-serializable` attributes of declarative roots, `delegatesFocus`
  for a click on the host, `<script>` inside declarative shadow trees
  (never run), `slotchange` for changes made by the parser,
  `link.sheet` (null, in shadow trees and out), the `::before`
  mapping of `exportparts`, `cookieStore` change events. A shadow
  root's `<style>` elements and its adopted sheets order their
  `@layer`s apart. An `on*` handler runs before the listeners added
  before it.

## Rendering, small

- `<details>` without a `<summary>`: its "Details" line does not
  open it on a click yet.
- Only the nearest text decoration is drawn, not those of every
  decorating ancestor (an underlined link in a struck paragraph).
- inset, outset, groove and ridge borders: their shades differ a
  little from Firefox's.
- Dashed borders on square corners: Firefox puts a whole dash on each
  corner and stretches the gaps; Soie keeps a fixed pitch.
- MDN's home page: its content column was reported too far left.
  Not reproduced from the two sheets saved (global and homepage: the
  named lines through var() are right); to check on a whole saved
  copy. Two line name lists side by side (`[a] [b]`, after var())
  are invalid for browsers, merged by Soie.
- Mojeek: "Mojeek User Survey" drawn inside the search box, icons
  right of "News" drawn as thin lines. To study on a saved copy.

## JavaScript

- `Intl` in other languages than English (CLDR data), real time zone
  names.
- The JS heap limit from the memory free at start, without the
  256 MB floor: on a 512 MB live system the kernel kills the browser
  on github.com before the limit is reached.

- `unhandledrejection` is never fired.
- Error and DOMException objects cannot be cloned (postMessage,
  structuredClone throw).
- `navigator.hardwareConcurrency` is 1: ALTCHA runs a single worker.

## Speed

- Nothing of the November audit was measured on a real Atom or as a
  32-bit build: time a wheel notch and the pelote video there.
- Video: the ALSA path of the frame dropping was only read (tests run
  without sound); a poster frame for `preload`; the loop filter is off
  while late (blocks between key frames).
- A full-width sticky header going down still has its rows painted at
  each step (keeping its pixels needs a proof that nothing passes
  over it).
- dom.js is compiled at the first page of each process (20 ms here,
  a quarter of a second on an Atom): bytecode made at build time. The
  UA style sheet is parsed again for each document; var() values are
  tokenized again per element.
- Conic and repeating radial gradients, and those whose color and
  alpha change together, are still computed per pixel.
- `linear-gradient(red 50%, blue 50%)` on an odd height splits its
  middle row (sinf of 180 degrees is not 0): exact directions for the
  four straight angles.
- A rounded overflow: hidden box paints its content through a mask,
  without the solid fast paths: 18 % slower for a whole page inside
  one.

## Release and site

- Retake the site's comparisons with the new version (they show
  0.1.2).
- The wok receipt follows each release tag (x86_64/wok/soie).
