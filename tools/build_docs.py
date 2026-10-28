#!/usr/bin/env python3
"""Builds the documentation site: docs-src/ -> docs/.

  docs/index.html            docs-src/index.md, the home page
  docs/tour/                 docs-src/tour/NN-name.md: the Language Tour
  docs/library-tour/         docs-src/library-tour/NN-name.md: the Library Tour
  docs/design/               docs-src/design/NN-name.md: how Iris works underneath
  docs/std/index.html        the library browser (docs-src/assets/std.js)
  docs/std/data.js           the library's API: `iris doc --json` on every module
                             of docs-src/std/modules.json, with docs-src/std/NAME.md
                             as a module's introduction when there is one
  docs/assets/               docs-src/assets/, search.js (what the search box finds) and
                             version.js (the version every page shows)

A section's pages are its NN-name.md files, in the parts PARTS gives them;
its index page lists them by part.

Run from anywhere: python3 tools/build_docs.py [--iris PATH] [--serve [PORT]]

--serve then serves docs/ at http://localhost:8000 (or PORT) until Ctrl-C.
Read that way, every page shares the theme and the sidebars' state; pages
opened as files can't share them in every browser.
"""
import glob
import html
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import md  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "docs-src")
OUT = os.path.join(ROOT, "docs")


def version():
    text = open(os.path.join(ROOT, "src", "iris.h")).read()
    return "v" + re.search(r'IRIS_VERSION "([^"]+)"', text).group(1)


VERSION = version()

SECTIONS = [
    # (folder, name in the top bar, what its index page says)
    ("tour", "Language Tour",
     "The whole language, one topic per chapter, with runnable programs and their output. Read it in order "
     "once, then use it as the reference."),
    ("library-tour", "Library Tour",
     "The standard library, one task per chapter: files, the terminal, formats, HTTP and networking, "
     "databases, cryptography and more. Read the Language Tour first."),
    ("design", "Design",
     "The compiler's architecture: the passes, their data structures, the runtime, and where to start "
     "reading the source."),
]


# Each section's chapters in parts: (part title, [file stems without NN-]).
# The chapters are read in this order, and a new chapter goes into a part.
PARTS = {
    "tour": [
        ("Basics", ["hello", "variables", "integers", "floating-point", "booleans-and-operators", "text",
                    "values-in-text", "if-and-match", "loops"]),
        ("Functions and data", ["functions", "function-values", "arrays-and-slices", "structs", "methods",
                                "enums", "tuples-and-aliases"]),
        ("Memory and errors", ["pointers", "errors", "memory"]),
        ("Building larger programs", ["generics", "interfaces", "collections", "modules"]),
        ("Concurrency", ["tasks-and-channels", "threads"]),
        ("Going further", ["compile-time", "attributes", "calling-c", "testing", "tools"]),
    ],
    "library-tour": [
        ("Programs and the system", ["files", "time", "logging", "flags", "processes", "terminal",
                                     "configuration", "machine-numbers", "metrics-and-tracing"]),
        ("Text and data", ["json", "regular-expressions", "csv-and-xml", "encodings-and-ids", "templates",
                           "compression-and-archives", "binary-messages", "msgpack-and-cbor"]),
        ("Networking", ["http", "networking", "quic-and-http3", "protobuf-and-grpc", "mqtt", "email",
                        "ssh", "sftp", "s3", "acme"]),
        ("Databases", ["sqlite", "postgres-and-redis", "mysql", "mongodb", "rows-as-structs", "migrations"]),
        ("Numbers and security", ["random-and-crypto", "math-and-big-numbers"]),
    ],
    "design": [
        ("The compiler", ["overview", "front-end", "checker", "ir", "values", "runtime", "decisions",
                          "workflow"]),
    ],
}


ICONS = {
    "menu": '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6h18M3 12h18M3 18h18"/></svg>',
    "search": '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="10.5" cy="10.5" r="6.5"/><path d="M15.5 15.5 21 21"/></svg>',
    "theme": '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="8.5"/><path d="M12 3.5a8.5 8.5 0 0 1 0 17z" class="fill"/></svg>',
}


def shell(title, body, root, desc="", sidebar="", wide=False):
    """A whole page: the sidebar (on book pages), the menu bar and the body.
    root: the way back to docs/ ("" or "../")."""
    full = "Iris" if title == "Iris" else f"{title} - Iris"
    meta = f'<meta name="description" content="{html.escape(desc)}">' if desc else ""
    toggle = (f'<button id="sidebar-toggle" class="icon-button" type="button" aria-label="Show or hide the chapters" '
              f'title="Chapters" aria-controls="sidebar">{ICONS["menu"]}</button>') if sidebar else ""
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(full)}</title>
{meta}
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Public+Sans:ital,wght@0,400;0,600;0,700;1,400&family=JetBrains+Mono:wght@400;600&display=swap">
<link rel="stylesheet" href="{root}assets/style.css">
<link rel="icon" href="{root}assets/logo.svg" type="image/svg+xml">
<script src="{root}assets/version.js"></script>
<script>/* the theme: the one picked last in this tab (its name), else the one in
   the address (?theme=dark, which links carry, also into new tabs), else
   the one remembered. Pages opened as files can't share storage in every
   browser, so the first two carry it there. The sidebar: open on a wide
   screen unless it was closed, closed on a narrow one. */
var h = document.documentElement, t = /^iris-theme:(dark|light)$/.exec(window.name) || /[?&]theme=(dark|light)/.exec(location.search);
if (t) h.dataset.theme = t[1];
var side = window.innerWidth >= 1080 ? "visible" : "hidden";
try {{
  if (t) localStorage.setItem("iris-theme", t[1]);
  else if ((t = localStorage.getItem("iris-theme"))) h.dataset.theme = t;
  if (side === "visible" && localStorage.getItem("iris-sidebar") === "hidden") side = "hidden";
  ["std-left", "std-right"].forEach(function (s) {{
    if (localStorage.getItem("iris-side-" + s) === "closed") h.classList.add("closed-" + s);
  }});
}} catch (e) {{}}
h.classList.add("sidebar-" + side);</script>
</head>
<body class="{"book" if sidebar else "wide" if wide else "plain"}">
{sidebar}
<div class="page-wrapper">
<header class="menu-bar">
  <div class="left-buttons">
    {toggle}
    <button id="theme" class="icon-button" type="button" aria-label="Switch between light and dark" title="Light or dark">{ICONS["theme"]}</button>
    <button id="search-toggle" class="icon-button" type="button" aria-label="Search" title="Search (/)" aria-controls="search-wrapper">{ICONS["search"]}</button>
  </div>
  <a class="menu-title" href="{root}index.html"><img src="{root}assets/logo.svg" alt="">Iris</a>
  <div class="right-buttons">
    <a class="wide-only" href="{root}tour/index.html">Language Tour</a>
    <a class="wide-only" href="{root}library-tour/index.html">Library Tour</a>
    <a href="{root}std/index.html">Library</a>
    <span class="ver"></span>
  </div>
</header>
<div id="search-wrapper" class="search-wrapper" data-root="{root}" hidden>
  <input id="site-search" type="search" placeholder="Search the documentation" aria-label="Search the documentation" autocomplete="off">
  <ul id="site-results" class="results" hidden></ul>
</div>
{body}
</div>
<script src="{root}assets/search.js"></script>
<script src="{root}assets/site.js"></script>
</body>
</html>
"""


def write(rel, text):
    path = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def chapters(section):
    """[(file stem, title, lede, html, toc, part)] in the order of PARTS"""
    found = {}
    for path in glob.glob(os.path.join(SRC, section, "[0-9][0-9]-*.md")):
        stem = os.path.basename(path)[:-3]
        found[stem[3:]] = (stem, path)
    out = []
    for part, names in PARTS[section]:
        for name in names:
            if name not in found:
                sys.exit(f"PARTS in tools/build_docs.py names {section}/{name}, which has no NN-{name}.md")
            stem, path = found.pop(name)
            title, lede, body, toc, _ = md.convert(open(path).read())
            out.append((stem, title, lede, body, toc, part))
    for name, (stem, _) in sorted(found.items()):
        sys.exit(f"docs-src/{section}/{stem}.md isn't in a part: add it to PARTS in tools/build_docs.py")
    return out


def by_part(chs):
    """[(part, [(number from 1, chapter)])]"""
    out = []
    for n, ch in enumerate(chs, 1):
        if not out or out[-1][0] != ch[5]:
            out.append((ch[5], []))
        out[-1][1].append((n, ch))
    return out


# The sections whose chapters are read in order, and so show their numbers
NUMBERED = {"tour", "design"}


class Book:
    """Every page of the book in reading order, and the sidebar that lists them:
    the introduction, then each section's index page and chapters, by part."""

    def __init__(self, sections):
        self.entries = [("page", "Introduction", "index.html", "", 0)]
        self.order = [("index.html", "Introduction")]
        for section, label, chs in sections:
            href = f"{section}/index.html"
            self.entries.append(("part", label, href, "", 0))
            self.order.append((href, label))
            parts = by_part(chs)
            for g, (part, items) in enumerate(parts, 1):
                if len(parts) > 1:
                    self.entries.append(("group", part, None, f"{g}.", 0))
                for i, (n, ch) in enumerate(items, 1):
                    num = f"{g}.{i}." if len(parts) > 1 else f"{n}."
                    href = f"{section}/{ch[0]}.html"
                    self.entries.append(("page", ch[1], href, num, 1 if len(parts) > 1 else 0))
                    self.order.append((href, ch[1]))
        self.entries.append(("part", "Reference", None, "", 0))
        self.entries.append(("page", "Standard library", "std/index.html", "", 0))

    def sidebar(self, root, here):
        items = []
        for kind, title, href, num, depth in self.entries:
            t = html.escape(title)
            n = f'<strong aria-hidden="true">{num}</strong> ' if num else ""
            on = ' class="active" aria-current="page"' if href == here else ""
            if kind == "part":
                inner = f'<a href="{root}{href}"{on}>{t}</a>' if href else t
                items.append(f'<li class="part-title">{inner}</li>')
            elif kind == "group":
                items.append(f'<li class="chapter-item group">{n}{t}</li>')
            else:
                items.append(f'<li class="chapter-item depth-{depth}"><a href="{root}{href}"{on}>{n}{t}</a></li>')
        return (f'<nav id="sidebar" class="sidebar" aria-label="Chapters"><div class="sidebar-scroll">'
                f'<ol class="chapters">{"".join(items)}</ol></div></nav>')

    def page(self, rel, title, body, desc=""):
        """A book page at docs/rel: the sidebar, the text and the arrows to the pages before and after"""
        root = "../" * rel.count("/")
        hrefs = [h for h, _ in self.order]
        i = hrefs.index(rel)
        arrows = []
        wide = []
        if i > 0:
            h, t = self.order[i - 1]
            arrows.append(f'<a rel="prev" class="prev" href="{root}{h}" title="{html.escape(t)}">'
                          f'<span>&#8249; Previous</span>{html.escape(t)}</a>')
            wide.append(f'<a rel="prev" class="nav-chapters previous" href="{root}{h}" '
                        f'title="Previous: {html.escape(t)}" aria-label="Previous: {html.escape(t)}">&#8249;</a>')
        if i + 1 < len(self.order):
            h, t = self.order[i + 1]
            arrows.append(f'<a rel="next" class="next" href="{root}{h}" title="{html.escape(t)}">'
                          f'<span>Next &#8250;</span>{html.escape(t)}</a>')
            wide.append(f'<a rel="next" class="nav-chapters next" href="{root}{h}" '
                        f'title="Next: {html.escape(t)}" aria-label="Next: {html.escape(t)}">&#8250;</a>')
        page = f"""<div class="content">
<main class="prose">
{body}
</main>
<nav class="pager" aria-label="Previous and next page">{"".join(arrows)}</nav>
</div>
<nav class="nav-wide" aria-label="Previous and next page">{"".join(wide)}</nav>"""
        write(rel, shell(title, page, root, desc, sidebar=self.sidebar(root, rel)))


def chapter_pages(book, chs, index, section, label):
    for stem, title, lede, body, toc, part in chs:
        book.page(f"{section}/{stem}.html", title, body, desc=lede)
        for sid, text in toc:
            index.append({"t": text, "p": title, "u": f"{section}/{stem}.html#{sid}", "k": "section"})
        index.append({"t": title, "p": label, "u": f"{section}/{stem}.html", "k": "chapter"})


def section_index(book, section, label, lede, chs):
    groups = []
    for g, (part, items) in enumerate(by_part(chs), 1):
        many = len(PARTS[section]) > 1
        lis = "".join(
            f'<li><a href="{ch[0]}.html">'
            + (f'<span class="num">{f"{g}.{i}" if many else n}</span>' if section in NUMBERED or many else "")
            + f'<b>{html.escape(ch[1])}</b><span class="desc">{html.escape(ch[2])}</span></a></li>'
            for i, (n, ch) in enumerate(items, 1))
        head = f'<h2>{g}. {html.escape(part)}</h2>' if many else ""
        groups.append(f'{head}<ol class="toc">{lis}</ol>')
    body = f"""<h1>{html.escape(label)}</h1>
<p class="lede">{html.escape(lede)}</p>
{"".join(groups)}"""
    book.page(f"{section}/index.html", label, body, desc=lede)


def hide_button(side, label, arrow):
    return (f'<button class="side-btn side-hide" type="button" data-side="{side}" aria-expanded="true" '
            f'aria-label="Hide {label}" title="Hide {label}">{arrow}</button>')


def show_button(side, label, arrow, where):
    return (f'<button class="side-btn side-show side-show-{where}" type="button" data-side="{side}" '
            f'aria-expanded="true" aria-label="Show {label}" title="Show {label}">{arrow}</button>')


def std_pages(index, iris):
    spec = json.load(open(os.path.join(SRC, "std", "modules.json")))
    mods = []
    for group in spec["groups"]:
        for entry in group["modules"]:
            p = subprocess.run([iris, "doc", "--json", entry["name"]], capture_output=True, text=True)
            if p.returncode != 0:
                sys.exit(f"iris doc --json {entry['name']}: {p.stderr.strip()}")
            m = json.loads(p.stdout)
            intro = os.path.join(SRC, "std", entry["name"] + ".md")
            mods.append({
                "name": entry["name"], "title": entry["title"], "group": group["title"],
                "summary": entry["summary"],
                "intro": md.convert(open(intro).read())[2] if os.path.exists(intro) else "",
                "doc": m["doc"], "decls": m["decls"],
            })
    known = {m["name"] for m in mods}
    for path in glob.glob(os.path.join(ROOT, "std", "*")):
        name = os.path.basename(path)
        if os.path.isdir(path) and name not in known:
            sys.exit(f"std/{name} isn't in docs-src/std/modules.json")
    for m in mods:
        for d in m["decls"]:
            index.append({"t": f"{m['name']}.{d['name']}", "p": m["title"],
                          "u": f"std/index.html#{m['name']}/{d['name']}", "k": d["kind"]})
        index.append({"t": m["name"], "p": m["title"], "u": f"std/index.html#{m['name']}", "k": "module"})
    data = {"modules": mods}
    write("std/data.js", "window.IRIS_STD = " + json.dumps(data, separators=(",", ":")) + ";\n")
    body = """<div class="std" id="std">
  <aside class="std-left" aria-label="Modules">
    <div class="std-top">
      <div class="side-head"><a href="index.html">Modules</a>""" + hide_button("std-left", "the module list", "&#8249;") + """</div>
      <input id="std-filter" type="search" placeholder="Find a function, type or module" aria-label="Find in the library" autocomplete="off">
    </div>
    <nav id="std-modules"></nav>
    <ul id="std-found" class="std-found" hidden></ul>
  </aside>
  <main id="std-main" class="std-main"></main>
  <aside class="std-right" aria-label="In this module">
    <div class="side-head"><span>In this module</span>""" + hide_button("std-right", "this module's names", "&#8250;") + """</div>
    <div id="std-right"></div>
  </aside>
  """ + show_button("std-left", "the module list", "&#8250;", "l") + show_button("std-right", "this module's names", "&#8249;", "r") + """
</div>
<script src="data.js"></script>
<script src="../assets/std.js"></script>"""
    write("std/index.html", shell("Library", body, "../", wide=True,
                                  desc="The Iris standard library: every module, type and function."))
    return sum(len(m["decls"]) for m in mods)


def intro_page(book, index):
    title, lede, body, toc, _ = md.convert(open(os.path.join(SRC, "index.md")).read())
    book.page("index.html", "Iris", body,
              desc="Iris, a compiled language for servers, network tools and system programs.")
    index.append({"t": title or "Iris", "p": "Iris", "u": "index.html", "k": "page"})
    for sid, text in toc:
        index.append({"t": text, "p": "Introduction", "u": f"index.html#{sid}", "k": "section"})


def broken_links():
    """Links between the site's pages to a file that isn't there"""
    out = []
    for root, _, files in os.walk(OUT):
        for f in files:
            if not f.endswith(".html"):
                continue
            path = os.path.join(root, f)
            for href in re.findall(r'href="([^"#:]+)(?:#[^"]*)?"', open(path).read()):
                if not os.path.exists(os.path.normpath(os.path.join(root, href))):
                    out.append((os.path.relpath(path, OUT), href))
    return sorted(set(out))


def main():
    iris = os.path.join(ROOT, "iris")
    if "--iris" in sys.argv:
        iris = sys.argv[sys.argv.index("--iris") + 1]
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    shutil.copytree(os.path.join(SRC, "assets"), os.path.join(OUT, "assets"))
    open(os.path.join(OUT, ".nojekyll"), "w").close()
    index = []
    counts = []
    all_chs = {section: chapters(section) for section, _, _ in SECTIONS}
    book = Book([(section, label, all_chs[section]) for section, label, _ in SECTIONS])
    intro_page(book, index)
    for section, label, lede in SECTIONS:
        chs = all_chs[section]
        chapter_pages(book, chs, index, section, label)
        section_index(book, section, label, lede, chs)
        counts.append(f"{len(chs)} {section}")
    n = std_pages(index, iris)
    # the only file a new version changes; site.js puts it into each page
    write("assets/version.js", "window.IRIS_VERSION = " + json.dumps(VERSION) + ";\n")
    write("assets/search.js", "window.IRIS_SEARCH = " + json.dumps(index, separators=(",", ":")) + ";\n")
    print(f"docs built: {', '.join(counts)}, {n} library declarations, {len(index)} search entries")
    broken = broken_links()
    for page, href in broken:
        print(f"broken link: {page} -> {href}")
    if broken:
        sys.exit(1)
    if "--serve" in sys.argv:
        serve()


def serve():
    import functools
    import http.server
    rest = sys.argv[sys.argv.index("--serve") + 1:]
    port = int(rest[0]) if rest and rest[0].isdigit() else 8000
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=OUT)
    handler.log_message = lambda *a: None
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as srv:
        print(f"the docs are at http://localhost:{port}/ (Ctrl-C stops)", flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
