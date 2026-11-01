#!/bin/sh
#
# Soie under AddressSanitizer: the tree's files copied into asan-build/
# (their dates kept, so the next run builds only what changed), built
# with ASan, the tests run there; with -s or URLs, sites rendered whole
# by xsoie too. A use after free, a read out of bounds stops the
# program with a report and makes this fail.
#
# Usage: tools/asan.sh [-m | -s] [URL...]   (make asan, make asan-sites)
# -m: make the local copies of other people's sites, once (network);
# -s: our sites live, the others from those copies, served on 127.0.0.1.
# $ASAN_CC: a gcc with libasan (gcc-83 on SliTaz, gcc 6.3 has none);
# $QUICKJS_SRC: as for make, to have the script extension.
#
# Copyright (C) 2026 Christophe Lincoln <pankso@slitaz.org>
# Written with the assistance of Claude (Anthropic) and Opencode.
#
# SPDX-License-Identifier: BSD-2-Clause
#

# Our own servers: rendered live.
SITES="https://www.slitaz.org/en/ https://soie.slitaz.org/"
# Other people's sites: rendered from local copies, not fetched at each
# run (distrowatch.com blocked "Soie/0.1" after repeated runs).
COPIES="https://en.wikipedia.org/wiki/SliTaz https://news.ycombinator.com/
https://lwn.net/ https://www.bbc.com/news https://old.reddit.com/
https://distrowatch.com/ https://www.youtube.com/ https://github.com/
https://www.debian.org/ https://archlinux.org/"
# What the copies' scripts would still ask of those sites is blocked.
BLOCK="wikipedia.org wikimedia.org ycombinator.com lwn.net bbc.com bbc.co.uk
bbci.co.uk reddit.com redd.it redditstatic.com redditmedia.com
distrowatch.com youtube.com ytimg.com googlevideo.com ggpht.com
github.com githubassets.com githubusercontent.com debian.org archlinux.org"
PORT=8078

cd "$(dirname "$0")/.." || exit 1
cc=${ASAN_CC:-gcc-83}
dir=${ASAN_DIR:-asan-build}
mirror=$PWD/$dir/mirror
if [ "$1" = "-m" ]; then
	rm -rf "$mirror"
	exec python3 tools/bench-mirror.py "$mirror" $COPIES
fi
lib=$($cc -print-file-name=libasan.so 2>/dev/null)
if [ ! -f "$lib" ]; then
	echo "asan: $cc has no libasan (on SliTaz: ASAN_CC=gcc-83, package gcc83)"
	exit 1
fi
lib=$(dirname "$lib")
sites=
copies=
if [ "$1" = "-s" ]; then
	shift
	sites=$SITES
	copies=yes
fi
[ $# -gt 0 ] && sites="$*" && copies=
case $QUICKJS_SRC in
	""|/*) ;;
	*) QUICKJS_SRC=$PWD/$QUICKJS_SRC ;;
esac

mkdir -p "$dir" || exit 1
hg files | tar -cf - -T - | tar -xf - -C "$dir" || exit 1
# libasan lives in gcc's own directory: found by rpath, not the loader
make -C "$dir" -j"$(nproc 2>/dev/null || echo 2)" QUICKJS_SRC="$QUICKJS_SRC" \
	CC="$cc -fsanitize=address -fno-omit-frame-pointer -Wl,-rpath,$lib" \
	all tests/unit tests/html5lib tests/painttest tests/exttest \
	>"$dir/build.log" 2>&1 || { tail -20 "$dir/build.log"; exit 1; }

# cookies in memory only: the sites do not write into the user's jar
export ASAN_OPTIONS=detect_leaks=0 SOIE_AUDIO=none SOIE_COOKIES=''
fail=0
make -C "$dir" check || fail=1
if [ -f "$dir/ext/script.so" ]; then
	make -C "$dir" check-js || fail=1
else
	echo "asan: no script extension (QUICKJS_SRC), scripts not checked"
fi

render() {
	if timeout 200 "$dir/browser/xsoie" -w 1024 -H 6000 \
	    -o "$dir/site.png" "$1" >/dev/null 2>"$dir/site.err" &&
	    ! grep -q AddressSanitizer "$dir/site.err"; then
		echo "ok   $1"
	else
		echo "FAIL $1"
		grep -A12 "ERROR: AddressSanitizer" "$dir/site.err"
		fail=1
	fi
}
for url in $sites; do
	render "$url"
done
if [ -n "$copies" ] && [ ! -d "$mirror" ]; then
	echo "asan: no local copies of other sites, make them once: tools/asan.sh -m"
elif [ -n "$copies" ]; then
	for host in $BLOCK; do
		echo "$host"
	done >"$dir/block.txt"
	export SOIE_BLOCK="$PWD/$dir/block.txt"
	python3 -m http.server -b 127.0.0.1 -d "$mirror" $PORT \
		>/dev/null 2>&1 &
	server=$!
	sleep 1
	for page in "$mirror"/*/; do
		[ -f "$page/index.html" ] || continue
		render "http://127.0.0.1:$PORT/$(basename "$page")/"
	done
	kill $server
fi
[ $fail = 0 ] && echo "asan: all clean"
exit $fail
