#!/bin/sh
#
# releases-atom.sh - make the site's Atom feed of releases from
# doc/CHANGELOG.md, one entry per version. Run at each release:
#   tools/releases-atom.sh > website/releases.atom
#

cd "$(dirname "$0")/.." || exit 1

SITE=https://soie.slitaz.org

awk -v site="$SITE" '
function esc(s) {
	gsub(/&/, "\\&amp;", s); gsub(/</, "\\&lt;", s); gsub(/>/, "\\&gt;", s)
	return s
}
# `code` spans of the Markdown
function code(s,   r) {
	while (match(s, /`[^`]*`/)) {
		r = r substr(s, 1, RSTART - 1) "<code>" \
			substr(s, RSTART + 1, RLENGTH - 2) "</code>"
		s = substr(s, RSTART + RLENGTH)
	}
	return r s
}
# The entry body is HTML, escaped once more inside the XML.
function out(s) { body = body esc(s) }
function close_item() {
	if (item != "") { out("<li>" code(esc(item)) "</li>"); item = "" }
}
function close_list() {
	close_item()
	if (inlist) { out("</ul>"); inlist = 0 }
}
function flush() {
	if (version == "") return
	close_list()
	n++
	v[n] = version; d[n] = date; b[n] = body
	version = ""; body = ""
}
/^## \[[0-9]/ {
	flush()
	version = $2; gsub(/[][]/, "", version)
	date = $4
	next
}
version == "" { next }
/^\[/ { next }
/^### / {
	close_list()
	out("<h3>" esc(substr($0, 5)) "</h3>")
	next
}
/^- / {
	close_item()
	if (!inlist) { out("<ul>"); inlist = 1 }
	item = substr($0, 3)
	next
}
/^  [^ ]/ && item != "" {
	sub(/^ +/, "")
	item = item " " $0
	next
}
END {
	flush()
	print "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
	print "<feed xmlns=\"http://www.w3.org/2005/Atom\">"
	print "\t<title>Soie releases</title>"
	print "\t<link href=\"" site "/\"/>"
	print "\t<link rel=\"self\" href=\"" site "/releases.atom\"/>"
	print "\t<id>" site "/releases.atom</id>"
	print "\t<updated>" d[1] "T00:00:00Z</updated>"
	print "\t<author><name>Christophe Lincoln</name></author>"
	for (i = 1; i <= n; i++) {
		print "\t<entry>"
		print "\t\t<title>Soie " v[i] "</title>"
		print "\t\t<link href=\"https://hg.slitaz.org/soie/rev/" v[i] "\"/>"
		print "\t\t<id>" site "/releases/" v[i] "</id>"
		print "\t\t<updated>" d[i] "T00:00:00Z</updated>"
		print "\t\t<content type=\"html\">" b[i] "</content>"
		print "\t</entry>"
	}
	print "</feed>"
}' doc/CHANGELOG.md
