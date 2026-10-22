#!/usr/bin/env python3
"""
bench-mirror.py - copy web pages and what they load into a directory,
for tools/browser-bench.sh to serve over plain local HTTP: browsers
without HTTPS (Dillo) can then load them, and network noise is gone.

    tools/bench-mirror.py DIR URL...

Each page becomes DIR/<name>/index.html; its style sheets, scripts,
images and the url()s of its style sheets go next to it under r/, the
references rewritten. srcset is dropped (one image per <img>).
Requests made later by scripts still go to the network.
"""
import hashlib, os, re, sys, urllib.parse, urllib.request

UA = "Mozilla/5.0 (X11; Linux x86_64; rv:153.0) Gecko/20100101 Firefox/153.0"
ATTR = re.compile(r'''(\s(?:src|href|poster|data-src)\s*=\s*)(["']?)([^"'\s>]+)\2''', re.I)
SRCSET = re.compile(r'''\s(?:srcset|data-srcset|integrity)\s*=\s*(["']).*?\1''', re.I | re.S)
CSSURL = re.compile(r'''url\(\s*(["']?)([^"')]+)\1\s*\)|@import\s+(["'])([^"']+)\3''', re.I)
BASE = re.compile(r'''<base\s[^>]*>''', re.I)


def get(url):
    req = urllib.request.Request(url, headers={
        "User-Agent": UA, "Accept-Encoding": "identity",
        "Accept": "text/html,application/xhtml+xml,*/*;q=0.8",
        "Accept-Language": "en-US,en;q=0.5"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read(), r.headers.get_content_type(), r.geturl()


class Mirror:
    def __init__(self, root):
        self.root = root
        self.done = {}
        os.makedirs(os.path.join(root, "r"), exist_ok=True)

    def local(self, url, kind):
        """The local path of url (fetched once), or None."""
        url = url.split("#")[0]
        if url in self.done:
            return self.done[url]
        if not url.startswith(("http://", "https://")):
            return None
        ext = os.path.splitext(urllib.parse.urlparse(url).path)[1][:6]
        name = "r/" + hashlib.sha1(url.encode()).hexdigest()[:16] + ext
        self.done[url] = name
        try:
            data, ctype, final = get(url)
        except Exception as e:
            sys.stderr.write("  skip %s: %s\n" % (url[:80], e))
            self.done[url] = None
            return None
        if kind == "css" or ctype == "text/css":
            data = self.css(data.decode("utf-8", "replace"), final).encode()
        with open(os.path.join(self.root, name), "wb") as f:
            f.write(data)
        return name

    def css(self, text, base):
        def sub(m):
            ref = m.group(2) or m.group(4)
            if ref.startswith("data:"):
                return m.group(0)
            to = self.local(urllib.parse.urljoin(base, ref),
                            "css" if m.group(4) else "any")
            # sheets live in r/, so a path relative to them
            to = to[2:] if to else "about:blank"
            return 'url("%s")' % to if m.group(2) else '@import "%s"' % to
        return CSSURL.sub(sub, text)

    def page(self, url):
        data, ctype, final = get(url)
        html = data.decode("utf-8", "replace")
        html = SRCSET.sub("", BASE.sub("", html))

        def sub(m):
            ref = m.group(3)
            if ref.startswith(("data:", "#", "javascript:", "mailto:")):
                return m.group(0)
            tag = html.rfind("<", 0, m.start())
            name = html[tag:m.start()].lower()
            t = re.match(r"<([a-z0-9-]+)", name)
            t = t.group(1) if t else ""
            # what the page loads, not its links (nor its videos)
            if t not in ("img", "script", "link", "video", "input"):
                return m.group(0)
            if t == "video" and "poster" not in m.group(1).lower():
                return m.group(0)
            if t == "link" and not re.search(
                    r'rel\s*=\s*["\']?[^"\'>]*(stylesheet|icon|preload|modulepreload)',
                    html[tag:html.find(">", m.end()) + 1].lower()):
                return m.group(0)
            kind = "css" if "stylesheet" in name else "any"
            to = self.local(urllib.parse.urljoin(final, ref), kind)
            return m.group(1) + '"' + (to or ref) + '"'
        html = ATTR.sub(sub, html)
        with open(os.path.join(self.root, "index.html"), "w") as f:
            f.write(html)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    for url in sys.argv[2:]:
        name = re.sub(r"[^a-z0-9]+", "-", urllib.parse.urlparse(url).netloc
                      + urllib.parse.urlparse(url).path, flags=re.I).strip("-")
        try:
            Mirror(os.path.join(sys.argv[1], name)).page(url)
            print(name)
        except Exception as e:
            print("%s: failed: %s" % (name, e))


main()
