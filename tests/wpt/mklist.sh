#!/bin/sh
#
# mklist.sh - pick the fixed subset of WPT reftests listed in
# tests/wpt/reftests.list, from a WPT checkout. Only needed again
# when the pinned WPT commit moves; the list is checked in.
#
# A test is eligible when, under css/css-flexbox or css/css-grid:
#   - it is a .html file (the HTML parser only: no .xht, .xhtml, .svg),
#     outside support/ and reference/ dirs, not named *-ref*.html;
#   - not under css/css-grid/grid-lanes/: grid-lanes (ex-masonry) is
#     a CSS Grid Level 3 draft, 848 of the 1460 eligible grid tests at
#     this commit; it would make the grid number a masonry number;
#   - it has at least one <link rel="match">, every reference an
#     existing .html file, and no <link rel="mismatch"> (skipped);
#   - it is not class="reftest-wait" (needs the harness's screenshot
#     signal), not a print or paged-media test (*print* in the name,
#     or a "paged" flag).
# Then, per directory, of the n sorted eligible paths (LC_ALL=C), the
# ones at index floor(i * n / CAP), i = 0 .. CAP-1 (all when n <= CAP):
# an even spread over the whole directory. Nothing is left out
# because Soie fails it.
#
# Usage: tests/wpt/mklist.sh WPT_DIR [CAP] > tests/wpt/reftests.list
# Each line: TEST REF [REF...], paths relative to WPT_DIR.
# The WPT tree is data only: nothing in it is run.
#

wpt=${1:?usage: mklist.sh WPT_DIR [CAP]}
cap=${2:-100}
wpt=$(cd "$wpt" && pwd) || exit 1
export LC_ALL=C

# The hrefs of the <link> tags whose rel is $2, in file $1.
links() {
	tr '\n\r\t' '   ' < "$1" | grep -oi '<link[^>]*>' |
	grep -iE "rel *= *[\"']?$2[\"' >]" |
	sed -n "s/.*[hH][rR][eE][fF] *= *[\"']\{0,1\}\([^\"' >]*\).*/\1/p"
}

# A reference href made relative to the WPT root, empty if missing.
resolve() {
	case "$2" in
	/*) p="$wpt$2" ;;
	*) p="$(dirname "$1")/$2" ;;
	esac
	p=${p%%[?#]*}
	[ -f "$p" ] || return
	p=$(realpath "$p")
	echo "${p#$wpt/}"
}

eligible() {
	cd "$wpt" &&
	find "$1" -type f -name '*.html' | sort |
	grep -v -e '/support/' -e '/reference/' -e '/references/' \
		-e '-ref[-.]' -e '/ref-' -e 'print' -e '^css/css-grid/grid-lanes/' |
	while read -r t; do
		grep -qiE "rel *= *[\"']?match" "$t" || continue
		grep -qiE "rel *= *[\"']?mismatch" "$t" && continue
		grep -qi 'reftest-wait' "$t" && continue
		grep -qiE 'name="flags"[^>]*paged|paged[^>]*name="flags"' \
			"$t" && continue
		refs= ok=1
		for h in $(links "$wpt/$t" match); do
			r=$(resolve "$wpt/$t" "$h")
			case "$r" in
			*.html) refs="$refs $r" ;;
			*) ok=0 ;;
			esac
		done
		[ $ok = 1 ] && [ -n "$refs" ] && echo "$t$refs"
	done
}

commit=$(git -C "$wpt" rev-parse HEAD 2>/dev/null)
cat <<EOF
# WPT reftests for Soie, made by tests/wpt/mklist.sh (rules there).
# WPT commit: ${commit:-unknown}
# Per directory: of the n eligible tests sorted, those at index
# floor(i * n / $cap), i = 0 .. $cap-1 (all when n <= $cap).
# Line: TEST REF [REF...] (all REFs must match), relative to WPT_DIR.
EOF
for d in css/css-flexbox css/css-grid; do
	eligible "$d" > "${TMPDIR:-/tmp}/mklist.$$"
	n=$(wc -l < "${TMPDIR:-/tmp}/mklist.$$")
	echo "# $d: $n eligible"
	awk -v n=$n -v cap=$cap 'BEGIN { if (n < cap) cap = n }
		i < cap && NR - 1 == int(i * n / cap) { print; i++ }' \
		"${TMPDIR:-/tmp}/mklist.$$"
	rm -f "${TMPDIR:-/tmp}/mklist.$$"
done
