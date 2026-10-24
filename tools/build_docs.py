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
  docs/assets/               docs-src/assets/, and search.js (what the search box finds)

A section's pages are its NN-name.md files in order; its index page lists them.

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
    <span class="ver">{VERSION}</span>
    <button id="theme" class="theme" aria-label="Switch between light and dark">&#9680;</button>
  </div>
</header>
{body}
<footer>
  <div class="bar">
    <span>Iris {VERSION}</span>
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
    """[(file stem, title, lede, html, toc)] in order"""
    out = []
    for path in sorted(glob.glob(os.path.join(SRC, section, "[0-9][0-9]-*.md"))):
        title, lede, body, toc, _ = md.convert(open(path).read())
        out.append((os.path.basename(path)[:-3], title, lede, body, toc))
    return out


def chapter_pages(chs, index, section, label):
    side = "".join(
        f'<li><a href="{stem}.html"{{on{stem}}}><span class="num">{stem[:2].lstrip("0")}</span>{html.escape(title)}</a></li>'
        for stem, title, _, _, _ in chs)
    for n, (stem, title, lede, body, toc) in enumerate(chs):
        nav = side
        for s, *_ in chs:
            nav = nav.replace("{on" + s + "}", ' class="on"' if s == stem else "")
        on_page = "".join(f'<li><a href="#{sid}">{html.escape(text)}</a></li>' for sid, text in toc)
        prev_link = next_link = ""
        if n > 0:
            p = chs[n - 1]
            prev_link = f'<a class="prev" href="{p[0]}.html"><span>Previous</span>{html.escape(p[1])}</a>'
        if n + 1 < len(chs):
            q = chs[n + 1]
            next_link = f'<a class="next" href="{q[0]}.html"><span>Next</span>{html.escape(q[1])}</a>'
        page = f"""<div class="doc">
  <button class="side-toggle" type="button" data-side="chapters" data-text="{html.escape(label)}" data-label="the chapter list" aria-expanded="true">&#8249; {html.escape(label)}</button>
  <aside class="chapters" aria-label="Chapters"><h4>{label}</h4><ol>{nav}</ol></aside>
  <article class="prose">
{body}
    <nav class="pager">{prev_link}{next_link}</nav>
  </article>
  <button class="side-toggle side-toggle-r" type="button" data-side="onpage" data-text="On this page" data-label="on this page" aria-expanded="true">On this page &#8250;</button>
  <aside class="onpage" aria-label="On this page"><h4>On this page</h4><ul>{on_page}</ul></aside>
</div>"""
        write(f"{section}/{stem}.html", shell(title, page, "../", section, wide=True, desc=lede))
        for sid, text in toc:
            index.append({"t": text, "p": title, "u": f"{section}/{stem}.html#{sid}", "k": "section"})
        index.append({"t": title, "p": label, "u": f"{section}/{stem}.html", "k": "chapter"})


def section_index(section, label, lede, chs):
    items = "".join(
        f'<li><a href="{stem}.html"><span class="num">{stem[:2].lstrip("0")}</span>'
        f'<b>{html.escape(title)}</b><span class="desc">{html.escape(desc)}</span></a></li>'
        for stem, title, desc, _, _ in chs)
    body = f"""<main class="narrow">
  <h1>{html.escape(label)}</h1>
  <p class="lede">{html.escape(lede)}</p>
  <ol class="toc">{items}</ol>
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
    data = {"version": VERSION, "modules": mods}
    write("std/data.js", "window.IRIS_STD = " + json.dumps(data, separators=(",", ":")) + ";\n")
    body = """<div class="std" id="std">
  <button class="side-toggle" type="button" data-side="std-left" data-text="Modules" data-label="modules" aria-expanded="true">&#8249; Modules</button>
  <aside class="std-left" aria-label="Modules">
    <input id="std-filter" type="search" placeholder="Find a function, type or module" aria-label="Find in the library" autocomplete="off">
    <nav id="std-modules"></nav>
    <ul id="std-found" class="std-found" hidden></ul>
  </aside>
  <main id="std-main" class="std-main"></main>
  <button class="side-toggle side-toggle-r" type="button" data-side="std-right" data-text="Names" data-label="this module's names" aria-expanded="true">Names &#8250;</button>
  <aside id="std-right" class="std-right" aria-label="In this module"></aside>
</div>
<script src="data.js"></script>
<script src="../assets/std.js"></script>"""
    write("std/index.html", shell("Library", body, "../", "std", wide=True,
                                  desc="The Iris standard library: every module, type and function."))
    return sum(len(m["decls"]) for m in mods)


def prose_page(name, title, section, desc, index):
    src = open(os.path.join(SRC, name + ".md")).read()
    page_title, lede, body, toc, _ = md.convert(src)
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
