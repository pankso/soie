#!/bin/sh
#
# run.sh - render WPT reftests and their references with
# tools/soie-render (800x600 viewport, scripts on) and compare pixels.
#
# A test passes when it matches every one of its references: no pixel
# differs (ImageMagick compare -metric AE), or the differences stay in
# the test's <meta name="fuzzy" content="..."> ranges (max difference
# per channel, number of pixels), read as wptrunner does. A fuzzy meta
# in a reference file is ignored, a keyed one (ref.html:...) only
# applies to that reference.
#
# Root-absolute URLs (/fonts/ahem.css, /css/support/...) are read in
# WPT_DIR through soie-render's test-only -r option: each page gets
# the base file:///<its path in WPT_DIR>. No network, no server, and
# nothing in the WPT tree is run but the pages' own scripts in Soie.
#
# Usage: tests/wpt/run.sh [-k DIR] WPT_DIR [LIST]
#   LIST defaults to tests/wpt/reftests.list; -k keeps the PNGs and
#   the diff images in DIR. SOIE_RENDER picks another soie-render.
# Prints PASS/FAIL/ERROR per test, then a summary per directory.
#

here=$(cd "$(dirname "$0")" && pwd)
render=${SOIE_RENDER:-$here/../../tools/soie-render}
keep=
if [ "$1" = -k ]; then
	keep=$2
	shift 2
fi
wpt=${1:?usage: run.sh [-k DIR] WPT_DIR [LIST]}
list=${2:-$here/reftests.list}
wpt=$(cd "$wpt" && pwd) || exit 1
out=${keep:-$(mktemp -d)} || exit 1
mkdir -p "$out"
[ -x "$render" ] || { echo "run.sh: no $render (make)" >&2; exit 1; }
# scripts run in the script extension: soie-render's -j alone runs
# none, and tests that change the page from a script were judged on
# their first state
ext=$(dirname "$render")/../ext/script.so
script=
[ -f "$ext" ] && script="-x $ext"

# Render page $1 (relative to WPT_DIR) to $2.
shot() {
	timeout -k 5 30 "$render" $script -j -w 800 -H 600 -r "$wpt" \
		-b "file:///$1" -o "$2" "$wpt/$1" >/dev/null 2>&1 &&
	[ -s "$2" ]
}

# The fuzzy content for reference $2 of test $1, if any: a meta keyed
# with the reference's name (or URL ending) first, else an unkeyed one.
fuzzy() {
	tr '\n\r\t' '   ' < "$wpt/$1" | grep -oi '<meta[^>]*>' |
	grep -iE "name *= *[\"']?fuzzy" |
	sed -n "s/.*[cC][oO][nN][tT][eE][nN][tT] *= *[\"']\([^\"']*\).*/\1/p" |
	awk -v ref="${2##*/}" '
		{ v = $0; gsub(/ /, "", v); k = "" }
		match(v, /^[^:=;]*:/) {
			k = substr(v, 1, RLENGTH - 1); v = substr(v, RLENGTH + 1)
		}
		k == "" && any == "" { any = v }
		k != "" && (k == ref || substr(k, length(k) - length(ref)) == "/" ref) {
			keyed = v
		}
		END { print (keyed != "" ? keyed : any) }'
}

# Is a difference of max $1 per channel over $2 pixels within the
# fuzzy content $3? (wptrunner's check_pass)
within() {
	echo "$3" | awk -v m=$1 -v p=$2 -F';' '
		function lo(r) { sub(/.*=/, "", r); sub(/-.*/, "", r); return r + 0 }
		function hi(r) { sub(/.*=/, "", r); sub(/.*-/, "", r); return r + 0 }
		{
			a = $1; b = $2
			if ($1 ~ /^totalPixels/ || $2 ~ /^maxDifference/) {
				a = $2; b = $1
			}
			ok = (p == 0 && lo(b) == 0) || (m == 0 && lo(a) == 0) ||
				(lo(a) <= m && m <= hi(a) && lo(b) <= p && p <= hi(b))
			exit !ok
		}'
}

results=$out/results
: > "$results"
grep -v '^#' "$list" | while read -r test refs; do
	[ -n "$test" ] || continue
	n=$(echo "$test" | tr / _)
	if ! shot "$test" "$out/$n.png"; then
		echo "ERROR $test (render)"
		echo "ERROR ${test%/*}" >> "$results"
		continue
	fi
	status=PASS why=
	i=0
	for ref in $refs; do
		i=$((i + 1))
		if ! shot "$ref" "$out/$n.ref$i.png"; then
			status=ERROR why=" (render $ref)"
			break
		fi
		diff=null:
		[ -n "$keep" ] && diff=$out/$n.diff$i.png
		px=$(compare -metric AE "$out/$n.png" "$out/$n.ref$i.png" \
			"$diff" 2>&1 >/dev/null)
		px=$(echo "${px%% *}" | awk '{ printf "%d", $1 }')
		[ "$px" = 0 ] && continue
		max=$(convert "$out/$n.png" "$out/$n.ref$i.png" \
			-compose difference -composite -separate \
			-format '%[max] ' info: |
			awk '{ for (i = 1; i <= NF; i++) if ($i > m) m = $i }
				END { printf "%d", m / 257 + 0.5 }')
		f=$(fuzzy "$test" "$ref")
		if [ -n "$f" ] && within "$max" "$px" "$f"; then
			continue
		fi
		status=FAIL
		why=" ($px px differ, max $max per channel${f:+, fuzzy $f}"
		[ "$i" -gt 1 ] && why="$why, ref $i"
		why="$why)"
		break
	done
	echo "$status $test$why"
	echo "$status ${test%/*}" >> "$results"
done

echo
awk '{
	split($2, p, "/"); d = p[1] "/" p[2]
	n[d]++; if ($1 == "PASS") ok[d]++; if ($1 == "ERROR") e[d]++
	if (!(d in seen)) { seen[d] = 1; o[++k] = d }
}
END {
	for (i = 1; i <= k; i++) {
		d = o[i]
		printf "%-16s %3d/%3d pass  %5.1f %%  (%d render errors)\n", d,
			ok[d], n[d], 100 * ok[d] / n[d], e[d]
	}
}' "$results"
[ -n "$keep" ] || rm -rf "$out"
