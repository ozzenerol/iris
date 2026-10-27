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


def shell(title, body, root, section="", wide=False, desc=""):
    """A whole page: the top bar, body, footer. root: the way back to docs/ ("" or "../")."""
    def nav(href, label, key):
        on = ' class="on"' if key == section else ""
        return f'<a href="{root}{href}"{on}>{label}</a>'
    full = "Iris" if title == "Iris" else f"{title} - Iris"
    meta = f'<meta name="description" content="{html.escape(desc)}">' if desc else ""
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
   browser, so the first two carry it there. */
var t = /^iris-theme:(dark|light)$/.exec(window.name) || /[?&]theme=(dark|light)/.exec(location.search);
if (t) document.documentElement.dataset.theme = t[1];
try {{
  if (t) localStorage.setItem("iris-theme", t[1]);
  else if ((t = localStorage.getItem("iris-theme"))) document.documentElement.dataset.theme = t;
  ["chapters", "onpage", "std-left", "std-right"].forEach(function (s) {{
    if (localStorage.getItem("iris-side-" + s) === "closed") document.documentElement.classList.add("closed-" + s);
  }});
}} catch (e) {{}}</script>
</head>
<body{' class="wide"' if wide else ''}>
<header class="top">
  <div class="bar">
    <a class="brand" href="{root}index.html"><img src="{root}assets/logo.svg" alt="">Iris</a>
    <nav class="main">
      {nav("tour/index.html", "Language Tour", "tour")}
      {nav("library-tour/index.html", "Library Tour", "library-tour")}
      {nav("std/index.html", "Library", "std")}
      {nav("design/index.html", "Design", "design")}
    </nav>
    <div class="search" data-root="{root}">
      <input id="site-search" type="search" placeholder="Search" aria-label="Search the documentation" autocomplete="off">
      <kbd>/</kbd>
      <ul id="site-results" class="results" hidden></ul>
    </div>
    <span class="ver"></span>
    <button id="theme" class="theme" aria-label="Switch between light and dark">&#9680;</button>
  </div>
</header>
{body}
<footer>
  <div class="bar">
    <span>Iris <span class="ver"></span></span>
    <a href="{root}tour/index.html">Language Tour</a>
    <a href="{root}library-tour/index.html">Library Tour</a>
    <a href="{root}std/index.html">Library</a>
  </div>
</footer>
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


def chapter_list(parts, section, stem_on=None):
    """The chapters by part, as the sidebar and the phone's menu show them"""
    out = []
    for part, items in parts:
        lis = "".join(
            f'<li><a href="{ch[0]}.html"{' class="on" aria-current="page"' if ch[0] == stem_on else ''}>'
            + (f'<span class="num">{n}</span>' if section in NUMBERED else "")
            + f'{html.escape(ch[1])}</a></li>'
            for n, ch in items)
        out.append(f'<h5>{html.escape(part)}</h5><ol>{lis}</ol>')
    return "".join(out)


def hide_button(side, label, arrow):
    return (f'<button class="side-btn side-hide" type="button" data-side="{side}" aria-expanded="true" '
            f'aria-label="Hide {label}" title="Hide {label}">{arrow}</button>')


def show_button(side, label, arrow, where):
    return (f'<button class="side-btn side-show side-show-{where}" type="button" data-side="{side}" '
            f'aria-expanded="true" aria-label="Show {label}" title="Show {label}">{arrow}</button>')


def chapter_pages(chs, index, section, label):
    parts = by_part(chs)
    total = len(chs)
    for n, (stem, title, lede, body, toc, part) in enumerate(chs):
        side = chapter_list(parts, section, stem)
        on_page = "".join(f'<li><a href="#{sid}">{html.escape(text)}</a></li>' for sid, text in toc)
        prev_link = next_link = ""
        if n > 0:
            p = chs[n - 1]
            prev_link = f'<a class="prev" href="{p[0]}.html"><span>&#8249; Previous</span>{html.escape(p[1])}</a>'
        if n + 1 < len(chs):
            q = chs[n + 1]
            next_link = f'<a class="next" href="{q[0]}.html"><span>Next &#8250;</span>{html.escape(q[1])}</a>'
        crumbs = (f'<div class="crumbs"><a href="index.html">{html.escape(label)}</a><span aria-hidden="true">/</span>'
                  f'<span>{html.escape(part)}</span><span class="pos">{n + 1} of {total}</span></div>')
        menu = (f'<details class="chap-menu"><summary>All chapters</summary>'
                f'<nav aria-label="Chapters">{side}</nav></details>')
        onpage = ""
        if toc:
            onpage = f"""<aside class="onpage" aria-label="On this page">
    <div class="side-head"><span>On this page</span>{hide_button("onpage", "on this page", "&#8250;")}</div>
    <ul>{on_page}</ul>
  </aside>
  {show_button("onpage", "on this page", "&#8249;", "r")}"""
        page = f"""<div class="doc">
  <aside class="chapters" aria-label="Chapters">
    <div class="side-head"><a href="index.html">{html.escape(label)}</a>{hide_button("chapters", "the chapter list", "&#8249;")}</div>
    <nav>{side}</nav>
  </aside>
  {show_button("chapters", "the chapter list", "&#8250;", "l")}
  <article class="prose">
{crumbs}
{menu}
{body}
    <nav class="pager" aria-label="Previous and next chapter">{prev_link}{next_link}</nav>
  </article>
  {onpage}
</div>"""
        write(f"{section}/{stem}.html", shell(title, page, "../", section, wide=True, desc=lede))
        for sid, text in toc:
            index.append({"t": text, "p": title, "u": f"{section}/{stem}.html#{sid}", "k": "section"})
        index.append({"t": title, "p": label, "u": f"{section}/{stem}.html", "k": "chapter"})


def section_index(section, label, lede, chs):
    groups = []
    for part, items in by_part(chs):
        lis = "".join(
            f'<li><a href="{ch[0]}.html">'
            + (f'<span class="num">{n}</span>' if section in NUMBERED else "")
            + f'<b>{html.escape(ch[1])}</b><span class="desc">{html.escape(ch[2])}</span></a></li>'
            for n, ch in items)
        head = f'<h2>{html.escape(part)}</h2>' if len(PARTS[section]) > 1 else ""
        groups.append(f'{head}<ol class="toc{" plain" if section not in NUMBERED else ""}">{lis}</ol>')
    start = f'<p class="start"><a class="button" href="{chs[0][0]}.html">Start with {html.escape(chs[0][1])} &#8250;</a></p>' \
        if section in NUMBERED else ""
    body = f"""<main class="narrow">
  <h1>{html.escape(label)}</h1>
  <p class="lede">{html.escape(lede)}</p>
  {start}
  {"".join(groups)}
</main>"""
    write(f"{section}/index.html", shell(label, body, "../", section, desc=lede))


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
    write("std/index.html", shell("Library", body, "../", "std", wide=True,
                                  desc="The Iris standard library: every module, type and function."))
    return sum(len(m["decls"]) for m in mods)


START = """<nav class="paths-cards" aria-label="Where to start">
  <a href="tour/index.html"><b>Language Tour</b><span>The whole language, one topic per chapter. Start here.</span></a>
  <a href="library-tour/index.html"><b>Library Tour</b><span>One task per chapter: files, HTTP, databases, formats and more.</span></a>
  <a href="std/index.html"><b>Library</b><span>Every module, type and function of the standard library.</span></a>
</nav>"""


def prose_page(name, title, section, desc, index):
    src = open(os.path.join(SRC, name + ".md")).read()
    page_title, lede, body, toc, _ = md.convert(src)
    if name == "index":
        # where to go next, right under the first lines
        cut = body.index("</p>", body.index('class="lede"')) + 4
        body = body[:cut] + START + body[cut:]
    write(name + ".html", shell(title, f'<main class="narrow prose">\n{body}\n</main>', "", section,
                                desc=desc or lede))
    index.append({"t": page_title or title, "p": "Iris", "u": name + ".html", "k": "page"})


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
    for section, label, lede in SECTIONS:
        chs = chapters(section)
        chapter_pages(chs, index, section, label)
        section_index(section, label, lede, chs)
        counts.append(f"{len(chs)} {section}")
    n = std_pages(index, iris)
    prose_page("index", "Iris", "", "Iris, a compiled language for servers, network tools and system programs.", index)
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
