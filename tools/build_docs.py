#!/usr/bin/env python3
"""Builds docs/ from docs-src/.

docs-src/index.md and docs-src/roadmap.md are single pages. docs-src/design/,
docs-src/language/ and docs-src/guides/ are sections: their pages are listed in SECTIONS, in
order, and each section's index.md is its first page.

Run from the repo root: python3 tools/build_docs.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import md  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "docs-src")
OUT = os.path.join(ROOT, "docs")

SECTIONS = {
    "design": ["index", "decisions", "compiler", "values", "workflow"],
    "language": ["index", "first-program", "variables-and-types", "functions", "control-flow",
                 "text", "structs", "enums", "pointers-and-memory", "errors", "generics",
                 "collections", "modules", "compile-time", "concurrency", "calling-c", "tools"],
    "guides": ["index", "rest-api"],
}
TOP = [("Design", "design/index.html"), ("Language", "language/index.html"),
       ("Guides", "guides/index.html"), ("Roadmap", "roadmap.html")]


def read(path):
    with open(path) as f:
        return f.read()


def page(title, depth, current_top, body, side=""):
    up = "../" * depth
    nav = "".join('<a href="%s%s"%s>%s</a>' % (up, href, ' class="here"' if name == current_top else "",
                                               name) for name, href in TOP)
    layout = "with-side" if side else "single"
    return ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "<title>%s</title>\n<link rel=\"stylesheet\" href=\"%sstyle.css\">\n</head>\n<body>\n"
            "<header class=\"top\"><a class=\"home\" href=\"%sindex.html\">Iris</a>"
            "<nav>%s</nav></header>\n"
            "<div class=\"%s\">%s<main>\n%s\n</main></div>\n</body>\n</html>\n"
            % (title, up, up, nav, layout, side, body))


def write(rel, text):
    path = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def main():
    os.makedirs(OUT, exist_ok=True)
    write("style.css", read(os.path.join(SRC, "style.css")))

    for name in ("index", "roadmap"):
        title, _, body, _, _ = md.convert(read(os.path.join(SRC, name + ".md")))
        top = "Roadmap" if name == "roadmap" else None
        write(name + ".html", page(title if name == "index" else title + " - Iris", 0, top, body))

    for section, names in SECTIONS.items():
        pages = []
        for n in names:
            title, _, body, toc, _ = md.convert(read(os.path.join(SRC, section, n + ".md")))
            pages.append((n, title, body, toc))
        for i, (n, title, body, toc) in enumerate(pages):
            links = "".join('<li><a href="%s.html"%s>%s</a></li>' % (
                m, ' class="here"' if m == n else "", t) for m, t, _, _ in pages)
            side = '<aside class="side"><ol>%s</ol></aside>' % links
            prev = '<a href="%s.html">&larr; %s</a>' % (pages[i - 1][0], pages[i - 1][1]) if i else "<span></span>"
            nxt = '<a href="%s.html">%s &rarr;</a>' % (pages[i + 1][0], pages[i + 1][1]) \
                if i + 1 < len(pages) else "<span></span>"
            body += '\n<div class="pager">%s%s</div>' % (prev, nxt)
            write(os.path.join(section, n + ".html"),
                  page(title + " - Iris", 1, section.capitalize(), body, side))

    # every relative link must point at a page that exists
    bad = []
    for d, _, files in os.walk(OUT):
        for f in files:
            if not f.endswith(".html"):
                continue
            text = read(os.path.join(d, f))
            for href in re.findall(r'href="([^"#:]+)(?:#[^"]*)?"', text):
                if not os.path.exists(os.path.normpath(os.path.join(d, href))):
                    bad.append("%s: %s" % (os.path.relpath(os.path.join(d, f), OUT), href))
    for b in bad:
        print("broken link:", b)
    print("docs built%s" % (", %d broken links" % len(bad) if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
