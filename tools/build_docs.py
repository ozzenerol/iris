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
    "gear": '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 8.5a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7z"/><path d="M19.4 13.5a7.6 7.6 0 0 0 0-3l2-1.6-2-3.4-2.4 1a7.4 7.4 0 0 0-2.6-1.5L14 2.5h-4l-.4 2.5A7.4 7.4 0 0 0 7 6.5l-2.4-1-2 3.4 2 1.6a7.6 7.6 0 0 0 0 3l-2 1.6 2 3.4 2.4-1a7.4 7.4 0 0 0 2.6 1.5l.4 2.5h4l.4-2.5a7.4 7.4 0 0 0 2.6-1.5l2.4 1 2-3.4z"/></svg>',
}

# the sections, as every sidebar lists them under "Documentation": (folder, label)
TABS = [("tour", "Language Tour"), ("library-tour", "Library Tour"), ("std", "Library reference"),
        ("design", "Design")]

# The sections whose chapters are read in order, and so show their numbers
NUMBERED = {"tour", "design"}

GITHUB = "https://github.com/ozzenerol/iris/blob/main/"


def search_bar(root):
    """rustdoc's bar over the text: the search, the help (?) and the settings"""
    return f"""<nav class="sub" data-root="{root}">
  <form class="search-form" onsubmit="return false">
    <span></span>
    <div id="sidebar-button" tabindex="-1"><a href="{root}index.html" title="Show the sidebar">{ICONS["menu"]}</a></div>
    <input id="site-search" class="search-input" name="search" aria-label="Run search in the documentation" autocomplete="off" spellcheck="false" placeholder="Type &lsquo;S&rsquo; or &lsquo;/&rsquo; to search, &lsquo;?&rsquo; for more options&hellip;" type="search">
    <div id="help-button" tabindex="-1"><a href="#" title="Help" role="button">?</a>
      <div class="popover" id="help" hidden>
        <h3>Keyboard shortcuts</h3>
        <dl>
          <dt><kbd>?</kbd></dt><dd>Show this help</dd>
          <dt><kbd>S</kbd> / <kbd>/</kbd></dt><dd>Focus the search field</dd>
          <dt><kbd>&uarr;</kbd> / <kbd>&darr;</kbd></dt><dd>Move through the search results</dd>
          <dt><kbd>&crarr;</kbd></dt><dd>Go to the active search result</dd>
          <dt><kbd>&larr;</kbd> / <kbd>&rarr;</kbd></dt><dd>Go to the chapter before or after</dd>
          <dt><kbd>Esc</kbd></dt><dd>Close the search or this window</dd>
        </dl>
      </div>
    </div>
    <div id="settings-menu" tabindex="-1"><a href="#" title="Settings" role="button">{ICONS["gear"]}<span class="label">Settings</span></a>
      <div class="popover" id="settings" hidden>
        <div class="setting-line"><div class="setting-name">Theme</div>
          <div class="choices">
            <label><input type="radio" name="theme" value="system"> System</label>
            <label><input type="radio" name="theme" value="light"> Light</label>
            <label><input type="radio" name="theme" value="dark"> Dark</label>
            <label><input type="radio" name="theme" value="ayu"> Ayu</label>
          </div>
        </div>
      </div>
    </div>
  </form>
</nav>"""


def shell(title, body, root, desc="", side="", kind="docs", mobile=""):
    """A whole page as rustdoc lays one out: the sidebar (side), and beside it
    the search bar over the main content (body). root: the way back to
    docs/ ("" or "../"). mobile: the title the top bar shows on phones."""
    full = "Iris" if title == "Iris" else f"{title} - Iris"
    meta = f'<meta name="description" content="{html.escape(desc)}">' if desc else ""
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(full)}</title>
{meta}
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Fira+Sans:ital,wght@0,400;0,500;1,400&family=Source+Code+Pro:ital,wght@0,400;0,600;1,400&family=Source+Serif+4:ital,opsz,wght@0,8..60,400;0,8..60,700;1,8..60,400&display=swap">
<link rel="stylesheet" href="{root}assets/style.css">
<link rel="icon" href="{root}assets/logo.svg" type="image/svg+xml">
<script src="{root}assets/version.js"></script>
<script>/* the theme: the one picked last in this tab (its name), else the one in
   the address (?theme=dark, which links carry, also into new tabs), else
   the one remembered; none follows the system. Pages opened as files can't
   share storage in every browser, so the first two carry it there. */
var h = document.documentElement, t = /^iris-theme:(dark|light|ayu)$/.exec(window.name) || /[?&]theme=(dark|light|ayu)/.exec(location.search);
if (t) h.dataset.theme = t[1];
try {{
  if (t) localStorage.setItem("iris-theme", t[1]);
  else if ((t = localStorage.getItem("iris-theme"))) h.dataset.theme = t;
}} catch (e) {{}}</script>
</head>
<body class="rustdoc {kind}">
<nav class="mobile-topbar">
  <button class="sidebar-menu-toggle" id="nav-toggle" type="button" title="Show the sidebar" aria-controls="sidebar" aria-expanded="false">{ICONS["menu"]}</button>
  <a class="logo-container" href="{root}index.html"><img src="{root}assets/logo.svg" alt="Iris"></a>
  <h2 class="location">{html.escape(mobile or title)}</h2>
</nav>
{side}
<div class="scrim" id="scrim" hidden></div>
<main>
<div class="width-limiter">
{search_bar(root)}
<section id="main-content" class="content">
{body}
</section>
<section id="search" class="content" hidden></section>
</div>
</main>
<script src="{root}assets/search.js"></script>
<script src="{root}assets/site.js"></script>
</body>
</html>
"""


def side_crate(root):
    """The top of every sidebar: the logo, Iris and the version"""
    return (f'<div class="sidebar-crate"><a class="logo-container" href="{root}index.html">'
            f'<img class="rust-logo" src="{root}assets/logo.svg" alt="logo"></a>'
            f'<h2><a href="{root}index.html">Iris</a><span class="version ver"></span></h2></div>')


def side_docs(root, section):
    """The bottom of every sidebar: the parts of the documentation, as rustdoc lists the crates"""
    links = "".join(
        f'<li><a href="{root}{folder}/index.html"{" class=current" if folder == section else ""}>{label}</a></li>'
        for folder, label in TABS)
    return f'<h3>Documentation</h3><ul class="block crate">{links}</ul>'


def side_nav(root, inner):
    return (f'<nav id="sidebar" class="sidebar" aria-label="Contents">{side_crate(root)}'
            f'<div class="sidebar-elems">{inner}</div></nav>')


def headings_block(toc):
    if not toc:
        return ""
    return ('<h3><a href="#">Sections</a></h3><ul class="block top-toc">' + "".join(
        f'<li><a href="#{sid}">{html.escape(text)}</a></li>' for sid, text in toc) + "</ul>")


def sidebar(section, label, chs, here, toc, title):
    """A chapter's sidebar, as rustdoc's for an item: the chapter and its
    headings, then the section's chapters by part, then the documentation"""
    numbered = section in NUMBERED
    top = ""
    if here != "index":
        top = (f'<section id="rustdoc-toc"><h2 class="location"><a href="#">{html.escape(title)}</a></h2>'
               f'<div class="sidebar-elems-inner">{headings_block(toc)}</div></section>')
    parts = []
    for part, items in by_part(chs):
        lis = []
        for n, ch in items:
            on = ch[0] == here
            num = f'<span class="n">{n}.</span> ' if numbered else ""
            lis.append(f'<li><a href="{ch[0]}.html"{" class=current aria-current=page" if on else ""}>'
                       f'{num}{html.escape(ch[1])}</a></li>')
        head = f'<h3><a href="index.html#{anchor(part)}">{html.escape(part)}</a></h3>' if len(PARTS[section]) > 1 else ""
        parts.append(f'{head}<ul class="block">{"".join(lis)}</ul>')
    where = "In " if here != "index" else ""
    cur = ' class="current"' if here == "index" else ""
    inner = (top + f'<div class="sidebar-elems-inner"><h2 class="in-crate"><a href="index.html"{cur}>{where}{html.escape(label)}</a></h2>'
             + "".join(parts) + side_docs("../", section) + "</div>")
    return side_nav("../", inner)


def anchor(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def item_table(rows, cls="mod"):
    """Links with a line about each, the way rustdoc lists a module's items: [(href, name, desc)]"""
    return '<dl class="item-table">' + "".join(
        f'<dt><a class="{cls}" href="{h}">{n}</a></dt><dd>{html.escape(d)}</dd>' for h, n, d in rows) + "</dl>"


def section_header(sid, text):
    return f'<h2 id="{sid}" class="section-header">{html.escape(text)}<a href="#{sid}" class="anchor">&sect;</a></h2>'


def strip_h1(body):
    return re.sub(r"^\s*<h1>.*?</h1>\s*", "", body, count=1, flags=re.S)


def main_heading(crumbs, h1, out_of_band):
    bc = f'<div class="rustdoc-breadcrumbs">{crumbs}</div>' if crumbs else ""
    return (f'<div class="main-heading">{bc}<h1>{h1}</h1>'
            f'<span class="out-of-band">{out_of_band}</span></div>')


def pager(prev, nxt):
    def link(ch, cls, word):
        if not ch:
            return "<span></span>"
        return f'<a class="{cls}" rel="{cls}" href="{ch[0]}.html"><span>{word}</span>{html.escape(ch[1])}</a>'
    if not (prev or nxt):
        return ""
    return f'<nav class="pager" aria-label="Previous and next">{link(prev, "prev", "Previous")}{link(nxt, "next", "Next")}</nav>'


def chapter_pages(chs, index, section, label):
    for i, (stem, title, lede, body, toc, part) in enumerate(chs):
        prev = chs[i - 1] if i > 0 else ("index", label)
        nxt = chs[i + 1] if i + 1 < len(chs) else None
        crumbs = f'<a href="index.html">{html.escape(label)}</a>'
        if len(PARTS[section]) > 1:
            crumbs += f'::<a href="index.html#{anchor(part)}">{html.escape(part)}</a>'
        kind = "Chapter" if section in NUMBERED else "Guide"
        h1 = f'{kind} <span class="mod">{html.escape(title)}</span>'
        oob = (f'<span class="since">{i + 1} of {len(chs)}</span> &middot; '
               f'<a class="src" href="{GITHUB}docs-src/{section}/{stem}.md">Source</a>')
        page = (main_heading(crumbs, h1, oob)
                + f'<div class="docblock">{strip_h1(body)}</div>' + pager(prev, nxt))
        side = sidebar(section, label, chs, stem, toc, title)
        write(f"{section}/{stem}.html", shell(title, page, "../", lede, side, mobile=title))
        for sid, text in toc:
            index.append({"t": text, "p": title, "u": f"{section}/{stem}.html#{sid}", "k": "section"})
        index.append({"t": title, "p": label, "u": f"{section}/{stem}.html", "k": "chapter"})


def section_index(section, label, lede, chs):
    """A section's front page, as rustdoc's page of a crate: what it is, then
    its chapters by part"""
    groups = []
    numbered = section in NUMBERED
    for part, items in by_part(chs):
        rows = [(f"{ch[0]}.html", (f'<span class="n">{n}.</span> ' if numbered else "") + html.escape(ch[1]), ch[2])
                for n, ch in items]
        title = part if len(PARTS[section]) > 1 else "Chapters"
        groups.append(section_header(anchor(title), title) + item_table(rows))
    h1 = f'Book <span class="mod">{html.escape(label)}</span>'
    body = (main_heading("", h1, f'<span class="since">{len(chs)} chapters</span>')
            + f'<div class="docblock"><p>{html.escape(lede)}</p></div>' + "".join(groups) + pager(None, chs[0]))
    write(f"{section}/index.html", shell(label, body, "../", lede, sidebar(section, label, chs, "index", [], label)))


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
    # std.js fills the sidebar's middle and the main content from the address
    side = (f'<nav id="sidebar" class="sidebar" aria-label="Contents">{side_crate("../")}'
            f'<div class="sidebar-elems"><div id="std-side"></div>{side_docs("../", "std")}</div></nav>')
    body = """<div id="std-main"></div>
<script src="data.js"></script>
<script src="../assets/std.js"></script>"""
    write("std/index.html", shell("Library reference", body, "../",
                                  "The Iris standard library: every module, type and function.", side, kind="std"))
    return sum(len(m["decls"]) for m in mods)


def home_page(index):
    """The front page: index.md, as rustdoc's page of a crate, with the
    "Where to start" list as a table of the sections"""
    title, lede, body, toc, _ = md.convert(open(os.path.join(SRC, "index.md")).read())
    rows = [
        ("tour/index.html", "Language Tour", "The whole language, one topic per chapter, with programs you can run. Start here."),
        ("library-tour/index.html", "Library Tour", "The standard library, one task per chapter: files, HTTP, databases, formats, cryptography and more."),
        ("std/index.html", "Library reference", "Every module, type and function of the standard library."),
        ("design/index.html", "Design", "How the compiler and runtime work, for people working on Iris."),
    ]
    body = re.sub(r'(<h2 id="where-to-start">.*?</h2>)\s*<ul>.*?</ul>', lambda m: m.group(1) + item_table(rows),
                  strip_h1(body), count=1, flags=re.S)
    inner = (f'<section id="rustdoc-toc"><h2 class="location"><a href="#">Iris</a></h2>'
             f'<div class="sidebar-elems-inner">{headings_block(toc)}{side_docs("", "")}</div></section>')
    page = (main_heading("", f'Language <span class="mod">{html.escape(title or "Iris")}</span>',
                         f'<a class="src" href="https://github.com/ozzenerol/iris">Source</a>')
            + f'<div class="docblock">{body}</div>')
    write("index.html", shell("Iris", page, "", "Iris, a compiled language for servers, network tools and system programs.",
                              side_nav("", inner)))
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
