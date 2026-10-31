# Changelog

All notable changes to Soie. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions
follow [Semantic Versioning](https://semver.org/). The reasons behind
each change are in [DECISIONS.md](DECISIONS.md).

## [Unreleased]

### Changed
- Backgrounds and borders are painted on whole pixels, as browsers
  paint them: sharp table grids, no line of a box showing through
  another at the same place. Border widths are whole pixels.
- Connections to a page's server are opened while the page loads, for
  its resources: Hacker News loads in 1.2 s instead of 1.9.

### Added
- Grid: `space-around` and `space-evenly` content distribution,
  `auto-fit` collapsing its empty tracks, `auto-fill`/`auto-fit` rows
  in a grid of definite height.

### Fixed
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

[0.1.1]: https://hg.slitaz.org/soie/rev/0.1.1
[0.1.0]: https://hg.slitaz.org/soie/rev/0.1.0
