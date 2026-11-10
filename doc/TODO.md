# To do

What comes next, most useful first. Done items leave this list for
the CHANGELOG; the reasons of a choice go to DECISIONS.md. Measures
against Firefox: tests/wpt, and screenshots of real sites compared
by RMSE (the 38 sites survey of 2026-11-03).

## Rendering, in the plan

- SVG styled by the page's CSS cascade: icons that take `fill`,
  `stroke` or `currentcolor` from a class are drawn black or not at
  all.
- Form controls: the width of a `<select>` from its options, the
  default sizes of controls (rust-lang.org's language menu: its caret
  covers the text).
- Shadow DOM: decide whether to implement it (attachShadow, slots,
  declarative `<template shadowrootmode>`); web components show
  nothing without it.

## Rendering, small

- `<details>` without a `<summary>` shows nothing; it should show a
  "Details" summary of its own.
- Only the nearest text decoration is drawn, not those of every
  decorating ancestor (an underlined link in a struck paragraph).
- inset, outset, groove and ridge borders: their shades differ a
  little from Firefox's.
- Dashed borders on square corners: Firefox puts a whole dash on each
  corner and stretches the gaps; Soie keeps a fixed pitch.
- `border-radius: a / b`: the vertical radii are dropped (needs four
  more longhands in the cascade).
- Mojeek: "Mojeek User Survey" drawn inside the search box, icons
  right of "News" drawn as thin lines. To study on a saved copy.

## JavaScript

- `unhandledrejection` is never fired.
- Error and DOMException objects cannot be cloned (postMessage,
  structuredClone throw).
- No JavaScript in iframes (no realm of their own): no reCAPTCHA.
- `navigator.hardwareConcurrency` is 1: ALTCHA runs a single worker.

## Speed

- Blurred shadows are not cached (browsers keep nine-patches): 1.3 ms
  a 200x140 card with a 30px blur, painted again at each repaint.
- A rounded overflow: hidden box paints its content through a mask,
  without the solid fast paths: 18 % slower for a whole page inside
  one.

## Release and site

- Retake the site's comparisons with the new version (they show
  0.1.2).
- The wok receipt follows each release tag (x86_64/wok/soie).
