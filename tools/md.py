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
pub const comptime when import try catch defer errdefer scoped spawn select chan
thread threadlocal asm test not and or is true false null self
""".split())
TYPES = set("""
i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool void str String Error
u16be u32be u64be u16le u32le u64le Duration List Map Atomic Mutex Thread
""".split())

TOKEN = re.compile(r'''
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<raw>r""".*?"""|r"[^"\n]*")
  | (?P<string>")
  | (?P<char>'(?:\\.|[^'\\\n])+')
  | (?P<attr>@[A-Za-z_]\w*)
  | (?P<number>\b(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?)
               (?:ns|us|ms|s|m|h|KiB|MiB|GiB)?\b)
  | (?P<word>[A-Za-z_]\w*)
''', re.S | re.X | re.M)

ESCAPE = re.compile(r"\\(?:x[0-9a-fA-F]{2}|u\{[0-9a-fA-F]+\}|.)")


def span(cls, text):
    return f'<span class="{cls}">{html.escape(text)}</span>'


def string_end(code, i):
    """Where the string that starts at code[i] ends: just past its closing
    quote. Braces hold code, which may have strings of its own."""
    triple = code.startswith('"""', i)
    i += 3 if triple else 1
    depth = 0
    while i < len(code):
        ch = code[i]
        if ch == "\\":
            i += 2
            continue
        if depth == 0:
            if triple and code.startswith('"""', i):
                return i + 3
            if not triple and (ch == '"' or ch == "\n"):
                return i + 1 if ch == '"' else i
            if code.startswith("{{", i) or code.startswith("}}", i):
                i += 2
                continue
        if ch == "{":
            depth += 1
        elif ch == "}" and depth > 0:
            depth -= 1
        elif ch == '"' and depth > 0:
            i = string_end(code, i)
            continue
        i += 1
    return i


def string_html(text):
    """A string literal: escapes and the {} around values stand out, and
    the code inside the braces is highlighted as code."""
    q = 3 if text.startswith('"""') else 1
    body_end = len(text) - q if text.endswith('"' * q) and len(text) >= 2 * q else len(text)
    out, plain = [], []

    def flush():
        if plain:
            out.append(span("s", "".join(plain)))
            plain.clear()

    plain.append(text[:q])
    i = q
    while i < body_end:
        if text.startswith("{{", i) or text.startswith("}}", i):
            flush()
            out.append(span("e", text[i:i + 2]))
            i += 2
            continue
        m = ESCAPE.match(text, i)
        if m:
            flush()
            out.append(span("e", m.group()))
            i = m.end()
            continue
        if text[i] == "{":
            j, depth = i + 1, 1
            while j < body_end and depth:
                if text[j] == '"':
                    j = string_end(text, j)
                    continue
                depth += {"{": 1, "}": -1}.get(text[j], 0)
                j += 1
            flush()
            inner_end = j - 1 if depth == 0 else j
            out.append(span("e", "{"))
            out.append(highlight(text[i + 1:inner_end]))
            if depth == 0:
                out.append(span("e", "}"))
            i = j
            continue
        plain.append(text[i])
        i += 1
    plain.append(text[body_end:])
    flush()
    return "".join(out)


def word_class(code, m):
    text = m.group()
    if text in KEYWORDS:
        return "k"
    if text in TYPES:
        return "t"
    rest = code[m.end():m.end() + 1]
    if rest == "(":
        return "f"
    if text[0].isupper() and any(c.islower() for c in text):
        return "t"
    return None


def highlight(code):
    """Iris source as HTML, with <span class="k|t|f|s|e|c|n|a"> around
    keywords, types, called functions, strings, the escapes and {} in them,
    comments, numbers and attributes"""
    out, last, pos = [], 0, 0
    while True:
        m = TOKEN.search(code, pos)
        if not m:
            break
        out.append(html.escape(code[last:m.start()]))
        text, kind = m.group(), m.lastgroup
        end = m.end()
        if kind == "string":
            end = string_end(code, m.start())
            out.append(string_html(code[m.start():end]))
        elif kind == "word":
            cls = word_class(code, m)
            out.append(span(cls, text) if cls else html.escape(text))
        else:
            cls = {"comment": "c", "raw": "s", "char": "s", "number": "n", "attr": "a"}[kind]
            out.append(span(cls, text))
        last = pos = end
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
