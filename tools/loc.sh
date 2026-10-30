#!/bin/sh
#
# loc.sh - count Soie's lines of code: C, headers and JavaScript of the
# engine, extensions, shell, browsers and tools, blank lines and
# comments left out. Generated files (lib/ua-css.c, ext/script/dom-js.c,
# browser/xdg-*, lib/entities.inc) and the libraries Soie links
# (QuickJS, FFmpeg, FreeType, HarfBuzz...) are not counted.
# Run from the source tree: tools/loc.sh
#

cd "$(dirname "$0")/.." || exit 1

find lib ext shell browser gtk tools \
	-name '*.c' -o -name '*.h' -o -name '*.js' |
	grep -v -e '^lib/ua-css\.c$' -e '^ext/script/dom-js\.c$' \
		-e '^browser/xdg-' | sort |
while read f; do cat "$f"; done | awk '
{
	line = $0; out = ""; i = 1
	while (i <= length(line)) {
		if (incomment) {
			j = index(substr(line, i), "*/")
			if (j) { incomment = 0; i += j + 1 }
			else i = length(line) + 1
			continue
		}
		c2 = substr(line, i, 2)
		if (c2 == "/*") { incomment = 1; i += 2; continue }
		if (c2 == "//") break
		out = out substr(line, i, 1); i++
	}
	if (out ~ /[^ \t]/) n++
}
END { print n + 0 }'
