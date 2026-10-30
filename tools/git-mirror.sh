#!/bin/sh
#
# git-mirror.sh - copy Soie's Mercurial history into a git repository
# and push it to the read-only mirrors. The code lives at
# https://hg.slitaz.org/soie; the mirrors only make it easy to clone
# with git.
#
# The whole history is converted on each run with git fast-import:
# the same history gives the same git commits, so the import and the
# push only add the new ones. Branch "default" becomes "main",
# Mercurial tags become git tags.
#
# Usage: tools/git-mirror.sh [REMOTE_URL...]
#   Without a URL it only builds the git repository ($GIT_MIRROR, by
#   default ~/.cache/soie/git-mirror). Run it after each push of a tag:
#   tools/git-mirror.sh https://github.com/pankso/soie.git
#   An https URL is pushed with gh's login when gh is there.
#

HG=$(cd "$(dirname "$0")/.." && pwd)
GIT=${GIT_MIRROR:-$HOME/.cache/soie/git-mirror}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

hg() { command hg -R "$HG" "$@"; }

# git wants "+0100" where Mercurial says -3600 (seconds west of UTC)
tz() {
	awk -v o="$1" 'BEGIN { s = o > 0 ? "-" : "+"; o = o < 0 ? -o : o
		printf "%s%02d%02d", s, o / 3600, o % 3600 / 60 }'
}

# A file's content as a fast-import data block
blob() {
	printf 'data %s\n' "$(wc -c < "$1" | tr -d ' ')"
	cat "$1"
	echo
}

# The fast-import stream: one commit per revision, marked rev + 1
stream() {
	for rev in $(hg log -r 'sort(all(), rev)' -T '{rev}\n'); do
		hg log -r "$rev" -T '{p1rev} {p2rev} {date|hgdate}\n{author}\n{desc}\n' \
			> "$TMP/log"
		set -- $(sed -n 1p "$TMP/log")
		p1=$1 p2=$2 when=$3 off=$4
		author=$(sed -n 2p "$TMP/log")
		case $author in
		*\<*\>*) ;;
		*) author="$author <>" ;;
		esac
		sed 1,2d "$TMP/log" > "$TMP/msg"
		echo "commit refs/heads/main"
		echo "mark :$((rev + 1))"
		echo "committer $author $when $(tz "$off")"
		blob "$TMP/msg"
		[ "$p1" -ge 0 ] && echo "from :$((p1 + 1))"
		[ "$p2" -ge 0 ] && echo "merge :$((p2 + 1))"
		# changes against the first parent: removed files, then the
		# others with their flags (x executable, l link), all fetched
		# by one hg cat
		hg status --change "$rev" -r -n | sed 's/^/D /'
		rm -rf "$TMP/f"
		hg log -r "$rev" -T '{file_adds % "{file}\n"}{file_mods % "{file}\n"}' \
			> "$TMP/changed"
		[ -s "$TMP/changed" ] || continue
		hg files -r "$rev" -T '{flags} {path}\n' > "$TMP/flags"
		hg cat -r "$rev" -o "$TMP/f/%p" $(sed 's/^/path:/' "$TMP/changed")
		while read -r f; do
			case $(grep -F -x -e "x $f" -e "l $f" "$TMP/flags" |
				cut -c1) in
			x) mode=100755 ;;
			l) mode=120000 ;;
			*) mode=100644 ;;
			esac
			echo "M $mode inline $f"
			blob "$TMP/f/$f"
		done < "$TMP/changed"
	done
	hg tags -T '{tag} {rev}\n' | while read -r tag rev; do
		[ "$tag" = tip ] && continue
		echo "reset refs/tags/$tag"
		echo "from :$((rev + 1))"
		echo
	done
}

[ -d "$GIT" ] || { mkdir -p "$GIT" && git init -q --bare "$GIT"; } || exit 1
stream | git --git-dir="$GIT" fast-import --quiet || exit 1
git --git-dir="$GIT" symbolic-ref HEAD refs/heads/main
echo "$(git --git-dir="$GIT" rev-list --count main) commits," \
	"$(git --git-dir="$GIT" tag | wc -l) tags in $GIT"

for remote in "$@"; do
	if [ "${remote#https://github.com/}" != "$remote" ] &&
	    command -v gh >/dev/null; then
		git --git-dir="$GIT" -c credential.helper= \
			-c credential.helper='!gh auth git-credential' \
			push "$remote" main --tags
	else
		git --git-dir="$GIT" push "$remote" main --tags
	fi || exit 1
done
