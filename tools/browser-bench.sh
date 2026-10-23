#!/bin/sh
#
# browser-bench.sh - load pages in Soie, Dillo and Firefox, measured
# the same way: each browser opens the page in a window on its own
# Xvfb, and its whole process tree is sampled in /proc.
#
#   settle  seconds until the page is done: the tree's CPU stayed under
#           5% for 2 s (the time of its last activity is reported)
#
# By default each run starts the browser on the page (start-up
# included). -W measures a browser already open: started on an empty
# page, left to calm down, then sent to the page (Firefox through its
# remote command, the others by typing the address), and only what
# follows is counted (settle from then, cpu above the idle browser).
#   cpu     CPU seconds the tree used
#   peak    largest memory, MB, PSS (shared pages counted once)
#   end     memory once settled, MB, PSS
#
# Usage: tools/browser-bench.sh [-n runs] [-b "soie dillo firefox"]
#        [-s DIR] [-W] URL...
# -s serves DIR on http://127.0.0.1:8077/ while measuring: copies made
# by tools/bench-mirror.py, for Dillo (no HTTPS) and without network
# noise. FIREFOX=/path/to/firefox picks a Firefox not in the PATH.
# Runs alternate browsers, medians are reported.
#

RUNS=3
BROWSERS="soie dillo firefox"
W=1024
H=768
DPY=:58
MAXWAIT=60
QUIET=2			# seconds of calm
CALM=5			# percent of one CPU

while [ $# -gt 0 ]; do
	case "$1" in
	-n) RUNS=$2; shift 2 ;;
	-b) BROWSERS=$2; shift 2 ;;
	-s) SERVE=$2; shift 2 ;;
	-W) WARM=1; shift ;;
	-h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) break ;;
	esac
done
[ $# -gt 0 ] || { echo "usage: $0 [-n runs] [-b browsers] URL..." >&2; exit 2; }

REPO=$(cd "$(dirname "$0")/.." && pwd)
FIREFOX=${FIREFOX:-firefox}
TMP=$(mktemp -d /tmp/browser-bench.XXXXXX) || exit 1
HZ=$(getconf CLK_TCK)
XPID=
SPID=

cleanup() {
	[ -n "$XPID" ] && kill "$XPID" 2>/dev/null
	[ -n "$SPID" ] && kill "$SPID" 2>/dev/null
	rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

Xvfb $DPY -nolisten tcp -screen 0 ${W}x${H}x24 >/dev/null 2>&1 &
XPID=$!
sleep 1
kill -0 $XPID 2>/dev/null || { echo "Xvfb $DPY failed" >&2; exit 1; }
export DISPLAY=$DPY
if [ -n "$SERVE" ]; then
	python3 -m http.server -b 127.0.0.1 -d "$SERVE" 8077 >/dev/null 2>&1 &
	SPID=$!
	sleep 1
fi

# Firefox: no first run, updates, telemetry or Safe Browsing traffic
firefox_profile() {
	mkdir -p "$1"
	cat > "$1/user.js" <<-EOF
	user_pref("browser.shell.checkDefaultBrowser", false);
	user_pref("browser.startup.homepage_override.mstone", "ignore");
	user_pref("browser.aboutwelcome.enabled", false);
	user_pref("datareporting.policy.dataSubmissionEnabled", false);
	user_pref("datareporting.healthreport.uploadEnabled", false);
	user_pref("toolkit.telemetry.enabled", false);
	user_pref("app.update.auto", false);
	user_pref("app.update.enabled", false);
	user_pref("browser.safebrowsing.malware.enabled", false);
	user_pref("browser.safebrowsing.phishing.enabled", false);
	user_pref("browser.safebrowsing.downloads.enabled", false);
	user_pref("extensions.update.enabled", false);
	user_pref("browser.newtabpage.enabled", false);
	user_pref("media.autoplay.default", 5);
	user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
	user_pref("termsofuse.acceptedVersion", 999);
	user_pref("termsofuse.acceptedDate", "1735689600000");
	user_pref("browser.termsofuse.prefMigrationCheck", true);
	user_pref("trailhead.firstrun.didSeeAboutWelcome", true);
	EOF
}

# ms since boot (busybox date has no %N)
now_ms() {
	awk '{ printf "%d", $1 * 1000 }' /proc/uptime
}

# Start a browser on url (none: its empty page) in the background,
# with a fresh profile in $DIR; prints its PID.
start() {
	b=$1 url=$2 dir=$DIR
	mkdir -p "$dir"
	case $b in
	soie)
		SOIE_EXT_DIR=$REPO/ext SOIE_COOKIES= SOIE_AUDIO=none \
			"$REPO/browser/soie" -w $W -H $H $url ;;
	dillo)
		HOME=$dir dillo -g ${W}x$H $url ;;
	firefox)
		firefox_profile "$dir/profile"
		# -W sends it pages later: remotely reachable
		if [ -n "$WARM" ]; then
			set -- --new-instance
		else
			set -- --no-remote
		fi
		"$FIREFOX" "$@" --profile "$dir/profile" --width $W \
			--height $H ${url:-about:blank} ;;
	esac >/dev/null 2>&1 &
	echo $!
}

# -W: type url in the running browser's address bar (Firefox: nothing,
# its remote command carries the URL)...
type_url() {
	[ "$1" = firefox ] && return
	xdotool mousemove $((W / 2)) $((H / 2))
	xdotool key ctrl+l
	xdotool type --delay 2 "$2"
	usleep 500000		# let the typing's redraws pass
}

# ... and go: from here on it is measured
go() {
	case $1 in
	firefox) "$FIREFOX" --profile "$DIR/profile" "$2" >/dev/null 2>&1 ;;
	*) xdotool key Return ;;
	esac
}

# Dillo's dpid and its plugins outlive it: their PIDs, to end them
dpis() {
	ps | awk '$4 ~ /(^|\/)dpid$|\.dpi$/ { print $1 }'
}

# One sample of the tree under pid: "ticks pss_kb pids..."
sample() {
	awk -v root="$1" '
	BEGIN {
		while (("ls /proc 2>/dev/null" | getline d) > 0)
			if (d ~ /^[0-9]+$/) {
				f = "/proc/" d "/stat"
				if ((getline line < f) > 0) {
					i = match(line, /\) /)
					while ((j = index(substr(line, i + 1), ") ")))
						i += j
					n = split(substr(line, i + 2), s, " ")
					ppid[d] = s[2]
					t[d] = s[12] + s[13]
					if (d == root)
						t[d] += s[14] + s[15]
				}
				close(f)
			}
		inside[root] = 1
		do {
			more = 0
			for (p in ppid)
				if (!(p in inside) && (ppid[p] in inside)) {
					inside[p] = 1
					more = 1
				}
		} while (more)
		for (p in inside) {
			ticks += t[p]
			f = "/proc/" p "/smaps_rollup"
			while ((getline line < f) > 0)
				if (line ~ /^Pss:/) {
					split(line, a, " ")
					pss += a[2]
				}
			close(f)
			list = list " " p
		}
		print ticks + 0, pss + 0, list
	}'
}

# Measure one load: prints "settle cpu peak end"
# Sample the tree under pid until it calms down; prints
# "last_busy_ms ticks peak_kb pss_kb pids..." (times from t0, CPU
# above base ticks).
watch() {
	pid=$1 t0=$2 base=$3
	last_busy=0 prev=$base peak=0 pss=0 pids=$pid
	calm_ticks=$((HZ * CALM / 100 / 4))	# per 0.25 s sample
	while :; do
		usleep 250000
		now=$(( $(now_ms) - t0 ))
		set -- $(sample "$pid")
		[ $# -ge 3 ] || break		# the browser exited
		ticks=$1 pss=$2
		shift 2
		pids="$*"
		[ "$pss" -gt "$peak" ] && peak=$pss
		[ $((ticks - prev)) -gt "$calm_ticks" ] && last_busy=$now
		prev=$ticks
		[ "$now" -gt $((last_busy + QUIET * 1000)) ] &&
			[ "$now" -gt 1500 ] && break
		[ "$now" -gt $((MAXWAIT * 1000)) ] && break
	done
	echo "$last_busy $((prev - base)) $peak $pss $pids"
}

# Measure one load: prints "settle cpu peak end"
measure() {
	b=$1 url=$2
	DIR=$TMP/$b.$(now_ms)
	before=$(dpis)
	if [ -n "$WARM" ]; then
		pid=$(start "$b" "")
		set -- $(watch "$pid" "$(now_ms)" 0)
		type_url "$b" "$url"
		base=$(sample "$pid" | cut -d' ' -f1)
		t0=$(now_ms)
		go "$b" "$url"
		set -- $(watch "$pid" "$t0" "${base:-0}")
	else
		pid=$(start "$b" "$url")
		set -- $(watch "$pid" "$(now_ms)" 0)
	fi
	r="$1 $2 $3 $4"
	shift 4
	kill "$@" 2>/dev/null
	usleep 500000
	kill -9 "$@" 2>/dev/null
	for p in $(dpis); do
		case " $before " in *" $p "*) ;; *) kill "$p" 2>/dev/null ;; esac
	done
	echo "$r" | awk -v hz="$HZ" \
		'{ printf "%.2f %.2f %.1f %.1f\n", $1 / 1000, $2 / hz,
		   $3 / 1024, $4 / 1024 }'
}

median() {
	sort -n | awk '{ v[NR] = $1 } END {
		if (!NR) { print "-"; exit }
		print NR % 2 ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }'
}

printf '%-36s %-8s %8s %8s %8s %8s\n' page browser settle cpu peak end
for url in "$@"; do
	for b in $BROWSERS; do
		: > "$TMP/r.$b"
	done
	i=0
	while [ $i -lt "$RUNS" ]; do
		for b in $BROWSERS; do
			measure "$b" "$url" >> "$TMP/r.$b"
		done
		i=$((i + 1))
	done
	for b in $BROWSERS; do
		printf '%-36s %-8s' "$(echo "$url" | cut -c1-36)" "$b"
		for col in 1 2 3 4; do
			printf ' %8s' "$(cut -d' ' -f$col "$TMP/r.$b" | median)"
		done
		echo
	done
done
