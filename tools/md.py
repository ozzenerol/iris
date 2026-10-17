"""The Markdown the Iris docs are written in, to HTML.

A small subset, on purpose: # headings (## and ### get anchors), paragraphs,
- and 1. lists, | tables |, ``` fenced blocks with an info string, `code`,
**bold**, *italic*, [links](url), and lines of raw HTML (starting with <).

convert(text) returns (title, lede, html, toc, blocks): the # heading, the
paragraph after it (plain text), the body, the ## headings as (id, text),
and every fenced block as (info, code, line) for check.py.
"""
import html
import re

KEYWORDS = set("""
if else for in match break continue return struct union enum interface type dyn
pub const comptime when import module try catch defer errdefer scoped spawn select
chan atomic threadlocal asm test not and or is true false null packed align volatile
""".split())
TYPES = set("""
i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool void str String Error
u16be u32be u64be u16le u32le u64le Duration
""".split())

TOKEN = re.compile(r"""
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:\\.|[^"\\\n])*")
  | (?P<char>'(?:\\.|[^'\\\n])+')
  | (?P<attr>@[A-Za-z_]\w*)
  | (?P<number>\b(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|\d[\d_]*(?:\.\d+)?(?:[eE][+-]?\d+)?)\b)
  | (?P<word>[A-Za-z_]\w*)
""", re.S | re.X | re.M)


def highlight(code):
    """Iris source as HTML with <span class="k|t|s|c|n|a|p"> around tokens"""
    out, last = [], 0
    for m in TOKEN.finditer(code):
        out.append(html.escape(code[last:m.start()]))
        text, kind = m.group(), m.lastgroup
        cls = None
        if kind == "comment":
            cls = "c"
        elif kind in ("string", "char"):
            cls = "s"
        elif kind == "number":
            cls = "n"
        elif kind == "attr":
            cls = "a"
        elif kind == "word":
            cls = "k" if text in KEYWORDS else "t" if text in TYPES else None
        out.append(f'<span class="{cls}">{html.escape(text)}</span>' if cls else html.escape(text))
        last = m.end()
    out.append(html.escape(code[last:]))
    return "".join(out)


def slug(text):
    s = re.sub(r"<[^>]+>", "", text)
    s = re.sub(r"[`*]", "", s).lower()
    s = re.sub(r"[^a-z0-9]+", "-", s).strip("-")
    return s or "section"


def inline(text):
    """The inline Markdown of one paragraph, list item or cell"""
    parts = re.split(r"(`[^`]+`)", text)
    out = []
    for p in parts:
        if p.startswith("`") and p.endswith("`") and len(p) > 1:
            out.append("<code>" + html.escape(p[1:-1]) + "</code>")
            continue
        p = html.escape(p, quote=False)
        p = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", lambda m: f'<a href="{m.group(2)}">{m.group(1)}</a>', p)
        p = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", p)
        p = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<em>\1</em>", p)
        out.append(p)
    return "".join(out)


def table(rows):
    def cells(line):
        line = line.strip()
        if line.startswith("|"):
            line = line[1:]
        if line.endswith("|"):
            line = line[:-1]
        # a | inside `code` stays in its cell
        cells, cur, code = [], "", False
        for ch in line:
            if ch == "`":
                code = not code
            if ch == "|" and not code and not cur.endswith("\\"):
                cells.append(cur.strip())
                cur = ""
            else:
                cur += ch
        cells.append(cur.strip())
        return [c.replace("\\|", "|") for c in cells]
    head = cells(rows[0])
    body = [cells(r) for r in rows[2:]]
    h = "".join(f"<th>{inline(c)}</th>" for c in head)
    b = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>" for r in body)
    return f'<div class="table"><table><thead><tr>{h}</tr></thead><tbody>{b}</tbody></table></div>'


def code_block(info, code):
    words = info.split()
    lang = words[0] if words else ""
    mode = words[1] if len(words) > 1 else ""
    if lang == "iris" and mode == "step":
        # a guide's whole file after a step, folded
        return ('<details class="step-file"><summary>main.iris after this step</summary>'
                f'<pre class="code"><code>{highlight(code)}</code></pre></details>')
    if lang == "iris":
        body = highlight(code)
        cls = "code" + (" run" if mode == "run" else " bad" if mode == "error" else "")
        label = '<span class="tag">does not compile</span>' if mode == "error" else ""
        return f'<pre class="{cls}">{label}<code>{body}</code></pre>'
    if lang == "output":
        return f'<pre class="out"><code>{html.escape(code)}</code></pre>'
    return f'<pre class="code"><code>{html.escape(code)}</code></pre>'


def convert(text):
    lines = text.split("\n")
    out, toc, blocks = [], [], []
    title, lede = "", ""
    para, items, list_kind, rows = [], [], None, []
    seen = set()

    def flush():
        nonlocal para, items, list_kind, rows, lede
        if para:
            joined = " ".join(l.strip() for l in para)
            if title and not lede and len(out) == 1 and out[0].startswith("<h1>"):
                lede = re.sub(r"<[^>]+>", "", inline(joined))
                out.append(f'<p class="lede">{inline(joined)}</p>')
            else:
                out.append(f"<p>{inline(joined)}</p>")
            para = []
        if items:
            tag = "ol" if list_kind == "ol" else "ul"
            out.append(f"<{tag}>" + "".join(f"<li>{inline(i)}</li>" for i in items) + f"</{tag}>")
            items, list_kind = [], None
        if rows:
            out.append(table(rows))
            rows = []

    i = 0
    while i < len(lines):
        line = lines[i]
        fence = re.match(r"^```\s*(.*)$", line)
        if fence:
            flush()
            info = fence.group(1).strip()
            start = i + 1
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            code = "\n".join(lines[i + 1:j])
            blocks.append((info, code, start))
            out.append(code_block(info, code))
            i = j + 1
            continue
        h = re.match(r"^(#{1,3})\s+(.*)$", line)
        if h:
            flush()
            level, text = len(h.group(1)), h.group(2).strip()
            if level == 1:
                title = re.sub(r"`", "", text)
                out.append(f"<h1>{inline(text)}</h1>")
            else:
                sid = slug(text)
                n = 2
                while sid in seen:
                    sid = f"{slug(text)}-{n}"
                    n += 1
                seen.add(sid)
                if level == 2:
                    toc.append((sid, re.sub(r"<[^>]+>", "", inline(text))))
                out.append(f'<h{level} id="{sid}"><a href="#{sid}">{inline(text)}</a></h{level}>')
            i += 1
            continue
        if line.strip().startswith("|") and line.strip().endswith("|"):
            if para or items:
                flush()
            rows.append(line)
            i += 1
            continue
        li = re.match(r"^\s*(?:[-*]|(\d+)\.)\s+(.*)$", line)
        if li and not para:
            if rows:
                flush()
            kind = "ol" if li.group(1) else "ul"
            if items and kind != list_kind:
                flush()
            list_kind = kind
            items.append(li.group(2))
            i += 1
            continue
        if items and line.startswith("  ") and line.strip():
            items[-1] += " " + line.strip()      # a list item's next line
            i += 1
            continue
        if line.strip().startswith("<") and not para:
            flush()
            out.append(line)
            i += 1
            continue
        if not line.strip():
            flush()
            i += 1
            continue
        if items or rows:
            flush()
        para.append(line)
        i += 1
    flush()
    return title, lede, "\n".join(out), toc, blocks
