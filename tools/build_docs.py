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
    "menu": '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 7h16M4 12h16M4 17h16"/></svg>',
    "theme": '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="8"/><path d="M12 4a8 8 0 0 1 0 16z" class="fill"/></svg>',
}

# the sections, as the sidebar lists them: (folder, label)
TABS = [("tour", "Language Tour"), ("library-tour", "Library Tour"), ("std", "Library reference"),
        ("design", "Design")]

# The sections whose chapters are read in order, and so show their numbers
NUMBERED = {"tour", "design"}


def toolbar(root):
    """The search bar over the text, with the theme button beside it"""
    return f"""<div class="toolbar" data-root="{root}">
  <div class="search-form">
    <input id="site-search" type="search" placeholder="Search (press /)" aria-label="Search the documentation" autocomplete="off">
    <ul id="site-results" class="results" hidden></ul>
  </div>
  <button id="theme" class="tool-btn" type="button" aria-label="Switch between light and dark" title="Light or dark">{ICONS["theme"]}</button>
</div>"""


def shell(title, body, root, desc="", side="", kind="docs"):
    """A whole page. kind docs: the sidebar (side), then the search bar over
    body. kind std: body is the library's panes, which hold both itself.
    root: the way back to docs/ ("" or "../")."""
    full = "Iris" if title == "Iris" else f"{title} - Iris"
    meta = f'<meta name="description" content="{html.escape(desc)}">' if desc else ""
    if kind == "docs":
        inner = f"""{side}
<div class="scrim" id="scrim" hidden></div>
<main class="main">
{toolbar(root)}
{body}
</main>"""
    else:
        inner = body
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(full)}</title>
{meta}
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Fira+Sans:ital,wght@0,400;0,500;0,600;1,400&family=Source+Code+Pro:wght@400;600&display=swap">
<link rel="stylesheet" href="{root}assets/style.css">
<link rel="icon" href="{root}assets/logo.svg" type="image/svg+xml">
<script src="{root}assets/version.js"></script>
<script>/* the theme: the one picked last in this tab (its name), else the one in
   the address (?theme=dark, which links carry, also into new tabs), else
   the one remembered. Pages opened as files can't share storage in every
   browser, so the first two carry it there. */
var h = document.documentElement, t = /^iris-theme:(dark|light)$/.exec(window.name) || /[?&]theme=(dark|light)/.exec(location.search);
if (t) h.dataset.theme = t[1];
try {{
  if (t) localStorage.setItem("iris-theme", t[1]);
  else if ((t = localStorage.getItem("iris-theme"))) h.dataset.theme = t;
  ["std-left", "std-right"].forEach(function (s) {{
    if (localStorage.getItem("iris-side-" + s) === "closed") h.classList.add("closed-" + s);
  }});
}} catch (e) {{}}</script>
</head>
<body class="is-{kind}">
<header class="mobile-top">
  <button id="nav-toggle" class="tool-btn" type="button" aria-label="Show the contents" aria-controls="sidebar" aria-expanded="false">{ICONS["menu"]}</button>
  <a class="mobile-logo" href="{root}index.html"><img src="{root}assets/logo.svg" alt="">Iris</a>
</header>
{inner}
<script src="{root}assets/search.js"></script>
<script src="{root}assets/site.js"></script>
</body>
</html>
"""


def side_top(root, section):
    """The top of every sidebar: the logo, the version and the sections"""
    links = "".join(
        f'<li><a href="{root}{folder}/index.html"{" class=current" if folder == section else ""}>{label}</a></li>'
        for folder, label in TABS)
    return (f'<a class="logo" href="{root}index.html"><img src="{root}assets/logo.svg" alt="">'
            f'<span>Iris</span><span class="version ver"></span></a>'
            f'<ul class="sections">{links}</ul>')


def sidebar(section, label, chs, here, toc):
    """A section's sidebar: its chapters by part, and under the one being read, its headings"""
    blocks = []
    numbered = section in NUMBERED
    for part, items in by_part(chs):
        lis = []
        for n, ch in items:
            on = ch[0] == here
            num = f'<span class="n">{n}.</span> ' if numbered else ""
            li = f'<li><a href="{ch[0]}.html"{" class=current aria-current=page" if on else ""}>{num}{html.escape(ch[1])}</a>'
            if on and toc:
                li += "<ul class=\"headings\">" + "".join(
                    f'<li><a href="#{sid}">{html.escape(text)}</a></li>' for sid, text in toc) + "</ul>"
            lis.append(li + "</li>")
        head = f'<h3>{html.escape(part)}</h3>' if len(PARTS[section]) > 1 else ""
        blocks.append(f'<section>{head}<ul class="block">{"".join(lis)}</ul></section>')
    title = f'<h2 class="location"><a href="index.html">{html.escape(label)}</a></h2>'
    return (f'<nav id="sidebar" class="sidebar" aria-label="Contents">{side_top("../", section)}'
            f'<div class="sidebar-elems">{title}{"".join(blocks)}</div></nav>')


def item_table(rows):
    """Links with a line about each, the way a module lists its items: [(href, name, desc)]"""
    return '<dl class="item-table">' + "".join(
        f'<dt><a href="{h}">{n}</a></dt><dd>{html.escape(d)}</dd>' for h, n, d in rows) + "</dl>"


def docs_page(section, label, chs, stem, title, body, toc, desc, crumbs, out_of_band, prev, nxt):
    def link(ch, cls, word):
        if not ch:
            return ""
        return f'<a class="{cls}" rel="{cls}" href="{ch[0]}.html"><span>{word}</span>{html.escape(ch[1])}</a>'
    pager = ""
    if prev or nxt:
        pager = f'<nav class="pager" aria-label="Previous and next">{link(prev, "prev", "Previous")}{link(nxt, "next", "Next")}</nav>'
    page = f"""<section class="content">
  <div class="main-heading">
    <div class="rustdoc-breadcrumbs">{crumbs}</div>
    <span class="out-of-band">{out_of_band}</span>
  </div>
  <div class="docblock prose">
{body}
  </div>
  {pager}
</section>"""
    write(f"{section}/{stem}.html", shell(title, page, "../", desc, sidebar(section, label, chs, stem, toc)))


def chapter_pages(chs, index, section, label):
    for i, (stem, title, lede, body, toc, part) in enumerate(chs):
        prev = chs[i - 1] if i > 0 else ("index", label)
        nxt = chs[i + 1] if i + 1 < len(chs) else None
        crumbs = f'<a href="index.html">{html.escape(label)}</a>'
        if len(PARTS[section]) > 1:
            crumbs += f'<span class="sep">::</span><span>{html.escape(part)}</span>'
        oob = f"Chapter {i + 1} of {len(chs)}" if section in NUMBERED else ""
        docs_page(section, label, chs, stem, title, body, toc, lede, crumbs, oob, prev, nxt)
        for sid, text in toc:
            index.append({"t": text, "p": title, "u": f"{section}/{stem}.html#{sid}", "k": "section"})
        index.append({"t": title, "p": label, "u": f"{section}/{stem}.html", "k": "chapter"})


def section_index(section, label, lede, chs):
    groups = []
    numbered = section in NUMBERED
    toc = []
    for part, items in by_part(chs):
        rows = [(f"{ch[0]}.html", (f'<span class="n">{n}.</span> ' if numbered else "") + html.escape(ch[1]), ch[2])
                for n, ch in items]
        if len(PARTS[section]) > 1:
            sid = part.lower().replace(" ", "-")
            groups.append(f'<h2 id="{sid}"><a href="#{sid}">{html.escape(part)}</a></h2>')
            toc.append((sid, part))
        groups.append(item_table(rows))
    body = f"""<h1>{html.escape(label)}</h1>
<p class="lede">{html.escape(lede)}</p>
{"".join(groups)}"""
    docs_page(section, label, chs, "index", label, body, toc, lede, "", f"{len(chs)} chapters", None, chs[0])


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
  <aside id="sidebar" class="std-left sidebar" aria-label="Modules">
    """ + side_top("../", "std") + """
    <div class="std-top">
      <div class="side-head"><a href="index.html">Modules</a>""" + hide_button("std-left", "the module list", "&#8249;") + """</div>
      <input id="std-filter" type="search" placeholder="Filter modules and names" aria-label="Find in the library" autocomplete="off">
    </div>
    <nav id="std-modules"></nav>
    <ul id="std-found" class="std-found" hidden></ul>
  </aside>
  <div class="std-center">
    """ + toolbar("../") + """
    <main id="std-main" class="std-main"></main>
  </div>
  <aside class="std-right" aria-label="In this module">
    <div class="side-head"><span>In this module</span>""" + hide_button("std-right", "this module's names", "&#8250;") + """</div>
    <div id="std-right"></div>
  </aside>
  """ + show_button("std-left", "the module list", "&#8250;", "l") + show_button("std-right", "this module's names", "&#8249;", "r") + """
</div>
<div class="scrim" id="scrim" hidden></div>
<script src="data.js"></script>
<script src="../assets/std.js"></script>"""
    write("std/index.html", shell("Library reference", body, "../",
                                  "The Iris standard library: every module, type and function.", kind="std"))
    return sum(len(m["decls"]) for m in mods)


def home_page(index):
    """The front page: index.md, with the "Where to start" list as a table of the sections"""
    title, lede, body, toc, _ = md.convert(open(os.path.join(SRC, "index.md")).read())
    rows = [
        ("tour/index.html", "Language Tour", "The whole language, one topic per chapter, with programs you can run. Start here."),
        ("library-tour/index.html", "Library Tour", "The standard library, one task per chapter: files, HTTP, databases, formats, cryptography and more."),
        ("std/index.html", "Library reference", "Every module, type and function of the standard library."),
        ("design/index.html", "Design", "How the compiler and runtime work, for people working on Iris."),
    ]
    body = re.sub(r'(<h2 id="where-to-start">.*?</h2>)\s*<ul>.*?</ul>', lambda m: m.group(1) + item_table(rows),
                  body, count=1, flags=re.S)
    links = "".join(f'<li><a href="{h}">{n}</a></li>' for h, n, _ in rows)
    blocks = ""
    if toc:
        blocks = ('<section><ul class="block">' + "".join(
            f'<li><a href="#{sid}">{html.escape(text)}</a></li>' for sid, text in toc) + "</ul></section>")
    side = (f'<nav id="sidebar" class="sidebar" aria-label="Contents">{side_top("", "")}'
            f'<div class="sidebar-elems"><h2 class="location"><a href="index.html">Iris</a></h2>{blocks}</div></nav>')
    page = f"""<section class="content">
  <div class="main-heading"><div class="rustdoc-breadcrumbs"></div></div>
  <div class="docblock prose">
{body}
  </div>
</section>"""
    write("index.html", shell("Iris", page, "", "Iris, a compiled language for servers, network tools and system programs.", side))
    index.append({"t": title or "Iris", "p": "Iris", "u": "index.html", "k": "page"})
    for sid, text in toc:
        index.append({"t": text, "p": "Iris", "u": f"index.html#{sid}", "k": "section"})


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
    home_page(index)
    for section, label, lede in SECTIONS:
        chs = chapters(section)
        chapter_pages(chs, index, section, label)
        section_index(section, label, lede, chs)
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
