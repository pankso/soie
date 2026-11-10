/*
 * Longhand properties: PROP(id, name, inherited, kind, flags, keywords,
 * initial). Order matters: font-size, then color and line-height are
 * computed first, as others depend on them. Keywords are '|'-separated
 * and stored by index; initial values are CSS text.
 *
 * Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
 * Written with the assistance of Claude (Anthropic) and Opencode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
PROP(FONT_SIZE, "font-size", 1, K_FSIZE, 0, "", "medium")
PROP(COLOR, "color", 1, K_COLOR, 0, "", "canvastext")
PROP(LINE_HEIGHT, "line-height", 1, K_LH, 0, "normal", "normal")
PROP(FONT_FAMILY, "font-family", 1, K_FAMILY, 0, "", "serif")
PROP(FONT_WEIGHT, "font-weight", 1, K_FWEIGHT, 0, "", "normal")
PROP(FONT_STYLE, "font-style", 1, K_KW, 0, "normal|italic|oblique",
	"normal")
PROP(FONT_VARIANT_CAPS, "font-variant-caps", 1, K_KW, 0,
	"normal|small-caps|all-small-caps|petite-caps|all-petite-caps|"
	"unicase|titling-caps", "normal")
PROP(FONT_STRETCH, "font-stretch", 1, K_RAW, 0, "", "normal")
PROP(LETTER_SPACING, "letter-spacing", 1, K_LEN, L_NOPCT, "normal",
	"normal")
PROP(WORD_SPACING, "word-spacing", 1, K_LEN, 0, "normal", "normal")
PROP(TEXT_ALIGN, "text-align", 1, K_KW, 0,
	"start|end|left|right|center|justify|match-parent|-webkit-center|"
	"-webkit-left|-webkit-right", "start")
PROP(TEXT_INDENT, "text-indent", 1, K_LEN, 0, "", "0")
PROP(TEXT_TRANSFORM, "text-transform", 1, K_KW, 0,
	"none|capitalize|uppercase|lowercase|full-width", "none")
PROP(WHITE_SPACE_COLLAPSE, "white-space-collapse", 1, K_KW, 0,
	"collapse|preserve|preserve-breaks|break-spaces|preserve-spaces|"
	"discard", "collapse")
PROP(TEXT_WRAP_MODE, "text-wrap-mode", 1, K_KW, 0, "wrap|nowrap", "wrap")
PROP(TEXT_WRAP_STYLE, "text-wrap-style", 1, K_KW, 0,
	"auto|balance|pretty|stable", "auto")
PROP(WORD_BREAK, "word-break", 1, K_KW, 0,
	"normal|break-all|keep-all|break-word|auto-phrase", "normal")
PROP(OVERFLOW_WRAP, "overflow-wrap", 1, K_KW, 0,
	"normal|break-word|anywhere", "normal")
PROP(VISIBILITY, "visibility", 1, K_KW, 0, "visible|hidden|collapse",
	"visible")
PROP(DIRECTION, "direction", 1, K_KW, 0, "ltr|rtl", "ltr")
PROP(LIST_STYLE_TYPE, "list-style-type", 1, K_LIST, 0, "", "disc")
PROP(LIST_STYLE_POSITION, "list-style-position", 1, K_KW, 0,
	"outside|inside", "outside")
PROP(LIST_STYLE_IMAGE, "list-style-image", 1, K_RAW, 0, "", "none")
PROP(CURSOR, "cursor", 1, K_RAW, 0, "", "auto")
PROP(QUOTES, "quotes", 1, K_RAW, 0, "", "auto")
PROP(TEXT_SHADOW, "text-shadow", 1, K_RAW, 0, "", "none")
PROP(BORDER_COLLAPSE, "border-collapse", 1, K_KW, 0, "separate|collapse",
	"separate")
PROP(BORDER_SPACING, "border-spacing", 1, K_LEN, L_NONNEG | L_NOPCT, "",
	"0")
PROP(CAPTION_SIDE, "caption-side", 1, K_KW, 0, "top|bottom", "top")
PROP(EMPTY_CELLS, "empty-cells", 1, K_KW, 0, "show|hide", "show")
PROP(POINTER_EVENTS, "pointer-events", 1, K_KW, 0,
	"auto|none|visiblepainted|visiblefill|visiblestroke|visible|painted|"
	"fill|stroke|all|bounding-box", "auto")
PROP(TEXT_UNDERLINE_OFFSET, "text-underline-offset", 1, K_LEN, 0, "auto",
	"auto")
/* SVG painting (svg.c): presentation attributes are hints of these */
PROP(FILL, "fill", 1, K_RAW, 0, "", "black")
PROP(STROKE, "stroke", 1, K_RAW, 0, "", "none")
PROP(STROKE_WIDTH, "stroke-width", 1, K_RAW, 0, "", "1")
PROP(STROKE_LINEJOIN, "stroke-linejoin", 1, K_KW, 0,
	"miter|round|bevel|arcs|miter-clip", "miter")
PROP(STROKE_LINECAP, "stroke-linecap", 1, K_KW, 0, "butt|round|square",
	"butt")
PROP(STROKE_MITERLIMIT, "stroke-miterlimit", 1, K_NUM, L_NONNEG, "", "4")
PROP(FILL_RULE, "fill-rule", 1, K_KW, 0, "nonzero|evenodd", "nonzero")
PROP(FILL_OPACITY, "fill-opacity", 1, K_ALPHA, 0, "", "1")
PROP(STROKE_OPACITY, "stroke-opacity", 1, K_ALPHA, 0, "", "1")
PROP(TEXT_ANCHOR, "text-anchor", 1, K_KW, 0, "start|middle|end", "start")

PROP(DISPLAY, "display", 0, K_DISPLAY, 0,
	"inline|block|inline-block|flex|inline-flex|grid|inline-grid|"
	"flow-root|list-item|table|inline-table|table-row-group|"
	"table-header-group|table-footer-group|table-row|table-cell|"
	"table-column-group|table-column|table-caption|contents|none|"
	"ruby|ruby-text|-webkit-box|-webkit-inline-box", "inline")
PROP(POSITION, "position", 0, K_KW, 0,
	"static|relative|absolute|fixed|sticky", "static")
PROP(FLOAT, "float", 0, K_KW, 0, "none|left|right|inline-start|inline-end",
	"none")
PROP(CLEAR, "clear", 0, K_KW, 0,
	"none|left|right|both|inline-start|inline-end", "none")
PROP(BOX_SIZING, "box-sizing", 0, K_KW, 0, "content-box|border-box",
	"content-box")
PROP(TOP, "top", 0, K_LEN, 0, "auto", "auto")
PROP(RIGHT, "right", 0, K_LEN, 0, "auto", "auto")
PROP(BOTTOM, "bottom", 0, K_LEN, 0, "auto", "auto")
PROP(LEFT, "left", 0, K_LEN, 0, "auto", "auto")
PROP(WIDTH, "width", 0, K_LEN, L_NONNEG,
	"auto|min-content|max-content|fit-content", "auto")
PROP(HEIGHT, "height", 0, K_LEN, L_NONNEG,
	"auto|min-content|max-content|fit-content", "auto")
PROP(MIN_WIDTH, "min-width", 0, K_LEN, L_NONNEG,
	"auto|min-content|max-content|fit-content", "auto")
PROP(MIN_HEIGHT, "min-height", 0, K_LEN, L_NONNEG,
	"auto|min-content|max-content|fit-content", "auto")
PROP(MAX_WIDTH, "max-width", 0, K_LEN, L_NONNEG,
	"none|min-content|max-content|fit-content", "none")
PROP(MAX_HEIGHT, "max-height", 0, K_LEN, L_NONNEG,
	"none|min-content|max-content|fit-content", "none")
PROP(MARGIN_TOP, "margin-top", 0, K_LEN, 0, "auto", "0")
PROP(MARGIN_RIGHT, "margin-right", 0, K_LEN, 0, "auto", "0")
PROP(MARGIN_BOTTOM, "margin-bottom", 0, K_LEN, 0, "auto", "0")
PROP(MARGIN_LEFT, "margin-left", 0, K_LEN, 0, "auto", "0")
PROP(PADDING_TOP, "padding-top", 0, K_LEN, L_NONNEG, "", "0")
PROP(PADDING_RIGHT, "padding-right", 0, K_LEN, L_NONNEG, "", "0")
PROP(PADDING_BOTTOM, "padding-bottom", 0, K_LEN, L_NONNEG, "", "0")
PROP(PADDING_LEFT, "padding-left", 0, K_LEN, L_NONNEG, "", "0")
PROP(BORDER_TOP_STYLE, "border-top-style", 0, K_KW, 0, BORDER_STYLES,
	"none")
PROP(BORDER_RIGHT_STYLE, "border-right-style", 0, K_KW, 0, BORDER_STYLES,
	"none")
PROP(BORDER_BOTTOM_STYLE, "border-bottom-style", 0, K_KW, 0,
	BORDER_STYLES, "none")
PROP(BORDER_LEFT_STYLE, "border-left-style", 0, K_KW, 0, BORDER_STYLES,
	"none")
PROP(BORDER_TOP_WIDTH, "border-top-width", 0, K_BWIDTH, 0, "", "medium")
PROP(BORDER_RIGHT_WIDTH, "border-right-width", 0, K_BWIDTH, 0, "",
	"medium")
PROP(BORDER_BOTTOM_WIDTH, "border-bottom-width", 0, K_BWIDTH, 0, "",
	"medium")
PROP(BORDER_LEFT_WIDTH, "border-left-width", 0, K_BWIDTH, 0, "", "medium")
PROP(BORDER_TOP_COLOR, "border-top-color", 0, K_COLOR, 0, "",
	"currentcolor")
PROP(BORDER_RIGHT_COLOR, "border-right-color", 0, K_COLOR, 0, "",
	"currentcolor")
PROP(BORDER_BOTTOM_COLOR, "border-bottom-color", 0, K_COLOR, 0, "",
	"currentcolor")
PROP(BORDER_LEFT_COLOR, "border-left-color", 0, K_COLOR, 0, "",
	"currentcolor")
PROP(BORDER_TOP_LEFT_RADIUS, "border-top-left-radius", 0, K_LEN,
	L_NONNEG, "", "0")
PROP(BORDER_TOP_RIGHT_RADIUS, "border-top-right-radius", 0, K_LEN,
	L_NONNEG, "", "0")
PROP(BORDER_BOTTOM_RIGHT_RADIUS, "border-bottom-right-radius", 0, K_LEN,
	L_NONNEG, "", "0")
PROP(BORDER_BOTTOM_LEFT_RADIUS, "border-bottom-left-radius", 0, K_LEN,
	L_NONNEG, "", "0")
PROP(OUTLINE_STYLE, "outline-style", 0, K_KW, 0,
	"none|auto|dotted|dashed|solid|double|groove|ridge|inset|outset",
	"none")
PROP(OUTLINE_WIDTH, "outline-width", 0, K_BWIDTH, 0, "", "medium")
PROP(OUTLINE_COLOR, "outline-color", 0, K_COLOR, 0, "", "currentcolor")
PROP(OUTLINE_OFFSET, "outline-offset", 0, K_LEN, L_NOPCT, "", "0")
PROP(OVERFLOW_X, "overflow-x", 0, K_KW, 0,
	"visible|hidden|clip|scroll|auto", "visible")
PROP(OVERFLOW_Y, "overflow-y", 0, K_KW, 0,
	"visible|hidden|clip|scroll|auto", "visible")
PROP(TEXT_OVERFLOW, "text-overflow", 0, K_KW, 0, "clip|ellipsis", "clip")
/* The old WebKit box, for its line clamp: display: -webkit-box;
 * -webkit-box-orient: vertical; -webkit-line-clamp: 2 (cascade.c) */
PROP(WEBKIT_BOX_ORIENT, "-webkit-box-orient", 0, K_KW, 0,
	"horizontal|vertical|inline-axis|block-axis", "horizontal")
PROP(WEBKIT_LINE_CLAMP, "-webkit-line-clamp", 0, K_INT, 0, "none", "none")
PROP(Z_INDEX, "z-index", 0, K_INT, 0, "auto", "auto")
PROP(OPACITY, "opacity", 0, K_ALPHA, 0, "", "1")
PROP(BACKGROUND_COLOR, "background-color", 0, K_COLOR, 0, "",
	"transparent")
PROP(BACKGROUND_IMAGE, "background-image", 0, K_RAW, 0, "", "none")
PROP(BACKGROUND_REPEAT, "background-repeat", 0, K_RAW, 0, "", "repeat")
PROP(BACKGROUND_POSITION, "background-position", 0, K_RAW, 0, "",
	"0% 0%")
PROP(BACKGROUND_SIZE, "background-size", 0, K_RAW, 0, "", "auto")
PROP(BACKGROUND_ATTACHMENT, "background-attachment", 0, K_KW, 0,
	"scroll|fixed|local", "scroll")
PROP(BACKGROUND_CLIP, "background-clip", 0, K_RAW, 0, "", "border-box")
PROP(BACKGROUND_ORIGIN, "background-origin", 0, K_RAW, 0, "",
	"padding-box")
/* masks: image layers as backgrounds', their alpha (icons painted in
 * currentColor through an SVG) */
PROP(MASK_IMAGE, "mask-image", 0, K_RAW, 0, "", "none")
PROP(MASK_REPEAT, "mask-repeat", 0, K_RAW, 0, "", "repeat")
PROP(MASK_POSITION, "mask-position", 0, K_RAW, 0, "", "0% 0%")
PROP(MASK_SIZE, "mask-size", 0, K_RAW, 0, "", "auto")
/* clipping: rect() of an absolute box, a shape (clip-path) */
PROP(CLIP, "clip", 0, K_RAW, 0, "", "auto")
PROP(CLIP_PATH, "clip-path", 0, K_RAW, 0, "", "none")
PROP(VERTICAL_ALIGN, "vertical-align", 0, K_LEN, 0,
	"baseline|sub|super|text-top|text-bottom|middle|top|bottom",
	"baseline")
PROP(CONTENT, "content", 0, K_RAW, 0, "", "normal")
PROP(TEXT_DECORATION_LINE, "text-decoration-line", 0, K_DECO, 0, "",
	"none")
PROP(TEXT_DECORATION_STYLE, "text-decoration-style", 0, K_KW, 0,
	"solid|double|dotted|dashed|wavy", "solid")
PROP(TEXT_DECORATION_COLOR, "text-decoration-color", 0, K_COLOR, 0, "",
	"currentcolor")
PROP(TEXT_DECORATION_THICKNESS, "text-decoration-thickness", 0, K_LEN, 0,
	"auto|from-font", "auto")
PROP(FLEX_DIRECTION, "flex-direction", 0, K_KW, 0,
	"row|row-reverse|column|column-reverse", "row")
PROP(FLEX_WRAP, "flex-wrap", 0, K_KW, 0, "nowrap|wrap|wrap-reverse",
	"nowrap")
PROP(FLEX_GROW, "flex-grow", 0, K_NUM, L_NONNEG, "", "0")
PROP(FLEX_SHRINK, "flex-shrink", 0, K_NUM, L_NONNEG, "", "1")
PROP(FLEX_BASIS, "flex-basis", 0, K_LEN, L_NONNEG,
	"auto|content|min-content|max-content|fit-content", "auto")
PROP(ORDER, "order", 0, K_INT, 0, "", "0")
PROP(JUSTIFY_CONTENT, "justify-content", 0, K_RAW, 0, "", "normal")
PROP(ALIGN_ITEMS, "align-items", 0, K_RAW, 0, "", "normal")
PROP(ALIGN_SELF, "align-self", 0, K_RAW, 0, "", "auto")
PROP(ALIGN_CONTENT, "align-content", 0, K_RAW, 0, "", "normal")
PROP(JUSTIFY_ITEMS, "justify-items", 0, K_RAW, 0, "", "legacy")
PROP(JUSTIFY_SELF, "justify-self", 0, K_RAW, 0, "", "auto")
PROP(ROW_GAP, "row-gap", 0, K_LEN, L_NONNEG, "normal", "normal")
PROP(COLUMN_GAP, "column-gap", 0, K_LEN, L_NONNEG, "normal", "normal")
PROP(GRID_TEMPLATE_COLUMNS, "grid-template-columns", 0, K_RAW, 0, "",
	"none")
PROP(GRID_TEMPLATE_ROWS, "grid-template-rows", 0, K_RAW, 0, "", "none")
PROP(GRID_TEMPLATE_AREAS, "grid-template-areas", 0, K_RAW, 0, "", "none")
PROP(GRID_AUTO_FLOW, "grid-auto-flow", 0, K_RAW, 0, "", "row")
PROP(GRID_AUTO_COLUMNS, "grid-auto-columns", 0, K_RAW, 0, "", "auto")
PROP(GRID_AUTO_ROWS, "grid-auto-rows", 0, K_RAW, 0, "", "auto")
PROP(GRID_COLUMN_START, "grid-column-start", 0, K_RAW, 0, "", "auto")
PROP(GRID_COLUMN_END, "grid-column-end", 0, K_RAW, 0, "", "auto")
PROP(GRID_ROW_START, "grid-row-start", 0, K_RAW, 0, "", "auto")
PROP(GRID_ROW_END, "grid-row-end", 0, K_RAW, 0, "", "auto")
PROP(TRANSFORM, "transform", 0, K_RAW, 0, "", "none")
PROP(TRANSFORM_ORIGIN, "transform-origin", 0, K_RAW, 0, "", "50% 50%")
PROP(ASPECT_RATIO, "aspect-ratio", 0, K_RAW, 0, "", "auto")
PROP(OBJECT_FIT, "object-fit", 0, K_KW, 0,
	"fill|contain|cover|none|scale-down", "fill")
PROP(OBJECT_POSITION, "object-position", 0, K_RAW, 0, "", "50% 50%")
PROP(BOX_SHADOW, "box-shadow", 0, K_RAW, 0, "", "none")
PROP(TABLE_LAYOUT, "table-layout", 0, K_KW, 0, "auto|fixed", "auto")
/* counters (boxes.c): names and integers, kept as text */
PROP(COUNTER_RESET, "counter-reset", 0, K_RAW, 0, "", "none")
PROP(COUNTER_INCREMENT, "counter-increment", 0, K_RAW, 0, "", "none")
PROP(COUNTER_SET, "counter-set", 0, K_RAW, 0, "", "none")
/* animations: only where they end (cascade.c), each a list by commas */
PROP(ANIMATION_NAME, "animation-name", 0, K_RAW, 0, "", "none")
PROP(ANIMATION_ITERATION_COUNT, "animation-iteration-count", 0, K_RAW, 0,
	"", "1")
PROP(ANIMATION_DIRECTION, "animation-direction", 0, K_RAW, 0, "", "normal")
PROP(ANIMATION_FILL_MODE, "animation-fill-mode", 0, K_RAW, 0, "", "none")
