#!/bin/sh
#
# pr-import.sh - take a pull request of the GitHub mirror into the
# Mercurial repository, its authors, dates and messages kept.
#
# Usage, from a working copy with nothing uncommitted:
#   tools/pr-import.sh N      apply pull request N, not committed: read
#                             the diff, build, test
#   tools/pr-import.sh -c N   approved: commit it (one revision per
#                             commit of the request)
#   tools/pr-import.sh -x N   once pushed to hg.slitaz.org and mirrored
#                             (tools/git-mirror.sh): close the request
#                             with links to the revisions
# Needs gh, logged in to an account allowed on the mirror.
#

REPO=pankso/soie
HGWEB=https://hg.slitaz.org/soie/rev
HG=$(cd "$(dirname "$0")/.." && pwd)
cd "$HG" || exit 1

mode=try
case $1 in
-c) mode=commit; shift ;;
-x) mode=close; shift ;;
esac
n=$1
case $n in
''|*[!0-9]*) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
patch=.hg/pr-$n.patch
revs=.hg/pr-$n.revs

clean() {
	[ -z "$(hg status -mard)" ] && return
	echo "Uncommitted changes here: commit or revert them first." >&2
	exit 1
}

case $mode in
try)
	clean
	gh pr view "$n" -R "$REPO" || exit 1
	gh pr diff "$n" -R "$REPO" --patch > "$patch" || exit 1
	hg import --no-commit "$patch" || exit 1
	echo
	hg diff --stat
	echo
	echo "Applied, not committed. Approved: $0 -c $n;" \
		"rejected: hg revert --all, then remove the added files."
	;;
commit)
	[ -f "$patch" ] || { echo "No $patch: run $0 $n first." >&2; exit 1; }
	# undo the trial (its added files too), then import for real
	added=$(hg status -an)
	hg revert --all --no-backup -q
	[ -n "$added" ] && echo "$added" | while read -r f; do rm -f "$f"; done
	clean
	first=$(hg log -r tip -T '{rev}')
	hg import "$patch" || exit 1
	hg log -r "$((first + 1)):tip" -T '{rev}:{node|short} {desc|firstline}\n' |
		tee "$revs"
	echo "Committed. Then hg push, tools/git-mirror.sh, $0 -x $n"
	;;
close)
	[ -f "$revs" ] || { echo "No $revs: run $0 -c $n first." >&2; exit 1; }
	msg="Thank you! Merged in the Mercurial repository, where Soie lives:"
	while IFS=: read -r rev rest; do
		node=${rest%% *}
		msg="$msg
- $HGWEB/$node"
	done < "$revs"
	gh pr close "$n" -R "$REPO" --comment "$msg" || exit 1
	rm -f "$patch" "$revs"
	;;
esac
