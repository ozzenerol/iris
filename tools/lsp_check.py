#!/usr/bin/env python3
"""Talks to `iris lsp` like an editor and checks its answers.

Usage: python3 tools/lsp_check.py [path/to/iris]
Prints what failed; exit status 1 if anything did."""
import json, os, subprocess, sys, tempfile

IRIS = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "iris"))

SOURCE = '''import json

// A point on the screen.
struct Point {
    i64 x
    i64 y
}

// How far the point is from the corner.
i64 Point.sum() => self.x + self.y

void main() {
    p := Point{x: 1, y: 2}
    print("{p.sum()}")
    n := missing + 1
}
'''


class Client:
    def __init__(self):
        self.p = subprocess.Popen([IRIS, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 0

    def send(self, obj):
        body = json.dumps(obj).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def read(self):
        length = 0
        while True:
            line = self.p.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        return json.loads(self.p.stdout.read(length))

    def request(self, method, params):
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params})
        while True:
            m = self.read()
            if m is None or m.get("id") == self.next_id:
                return m

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})


def main():
    failures = []

    def check(what, ok):
        if not ok:
            failures.append(what)

    d = tempfile.mkdtemp()
    path = os.path.join(d, "main.iris")
    with open(path, "w") as f:
        f.write("void main() { }\n")       # what is on disk differs: the editor's text wins
    uri = "file://" + path
    c = Client()
    init = c.request("initialize", {"capabilities": {}})
    check("initialize", init and init["result"]["capabilities"]["hoverProvider"])
    c.notify("initialized", {})
    c.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "iris", "version": 1, "text": SOURCE}})
    diag = c.read()
    msgs = [x["message"] for x in diag["params"]["diagnostics"]] if diag else []
    check("diagnostics: %r" % msgs, diag and diag["params"]["uri"] == uri and any("missing" in m for m in msgs))
    if diag and diag["params"]["diagnostics"]:
        r = diag["params"]["diagnostics"][0]["range"]
        check("diagnostic range %r" % r, r["start"] == {"line": 14, "character": 9})

    td = {"uri": uri}
    h = c.request("textDocument/hover", {"textDocument": td, "position": {"line": 13, "character": 15}})
    hv = h and h["result"] and h["result"]["contents"]["value"]
    check("hover on sum: %r" % hv, hv and "i64 Point.sum()" in hv and "How far the point" in hv)
    h = c.request("textDocument/hover", {"textDocument": td, "position": {"line": 12, "character": 4}})
    hv = h and h["result"] and h["result"]["contents"]["value"]
    check("hover on p: %r" % hv, hv and "Point p" in hv)

    g = c.request("textDocument/definition", {"textDocument": td, "position": {"line": 12, "character": 10}})
    loc = g and g["result"]
    check("definition of Point: %r" % loc, loc and loc["uri"].endswith("main.iris") and loc["range"]["start"]["line"] == 3)

    s = c.request("textDocument/documentSymbol", {"textDocument": td})
    tree = lambda xs: [(x["name"], x["kind"], tree(x.get("children", []))) for x in xs]
    got = tree((s and s["result"]) or [])
    check("symbols: %r" % got, got == [("Point", 23, [("x", 8, []), ("y", 8, []), ("sum", 6, [])]), ("main", 12, [])])
    point = ((s and s["result"]) or [{}])[0]
    check("the range of a struct, to the end of its method: %r" % point.get("range"),
          (point["range"]["start"]["line"], point["range"]["end"]["line"]) == (3, 9))

    def inside(x, parent=None):
        """the name inside the range, and the range inside the parent's"""
        pos = lambda q: (q["line"], q["character"])
        a, b = x["range"], x["selectionRange"]
        ok = pos(a["start"]) <= pos(b["start"]) and pos(b["end"]) <= pos(a["end"])
        if parent:
            ok = ok and pos(parent["start"]) <= pos(a["start"]) and pos(a["end"]) <= pos(parent["end"])
        return ok and all(inside(k, a) for k in x.get("children", []))
    check("symbol names inside their ranges", all(inside(x) for x in (s and s["result"]) or []))

    # completion after "p." and after "json."
    text2 = SOURCE.replace('    print("{p.sum()}")', '    p.\n    print("{p.sum()}")')
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": text2}]})
    c.read()
    comp = c.request("textDocument/completion", {"textDocument": td, "position": {"line": 13, "character": 6}})
    labels = [x["label"] for x in (comp and comp["result"]) or []]
    check("completion after p.: %r" % labels, {"x", "y", "sum"} <= set(labels))
    text3 = SOURCE.replace('    print("{p.sum()}")', '    json.\n    print("{p.sum()}")')
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": text3}]})
    c.read()
    comp = c.request("textDocument/completion", {"textDocument": td, "position": {"line": 13, "character": 9}})
    labels = [x["label"] for x in (comp and comp["result"]) or []]
    check("completion after json.: %r" % labels, {"encode", "decode", "JsonError"} <= set(labels))
    text4 = SOURCE.replace('    print("{p.sum()}")', '    path := "/api/users"\n    path.\n    print("{p.sum()}")')
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 4}, "contentChanges": [{"text": text4}]})
    c.read()
    comp = c.request("textDocument/completion", {"textDocument": td, "position": {"line": 14, "character": 9}})
    labels = [x["label"] for x in (comp and comp["result"]) or []]
    check("completion after a str's name: %r" % labels, {"len", "copy", "startsWith", "endsWith", "trim"} <= set(labels))

    # completion of any name while a line is half typed: locals in scope, the
    # module's names, imports, built-ins; and module names in an import line
    version = [4]

    def complete(text):
        """the labels offered at the | in text"""
        at = text.index("|")
        body = text.replace("|", "")
        version[0] += 1
        c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]},
                                            "contentChanges": [{"text": body}]})
        c.read()
        before = body[:at]
        pos = {"line": before.count("\n"), "character": len(before.split("\n")[-1])}
        comp = c.request("textDocument/completion", {"textDocument": td, "position": pos})
        return set(x["label"] for x in (comp and comp["result"]) or [])

    got = complete("import json\n\nenum Kind { cat, dog }\n\ni64 add(i64 a, i64 b) => a + b\n\n!void main() {\n"
                   "    count := 3\n    for i in 0..count {\n        inner := i\n    }\n    if co|\n")
    check("names while typing: %r" % sorted(got), {"count", "add", "main", "Kind", "json", "print", "List"} <= got)
    check("names out of scope: %r" % sorted(got), not ({"inner", "i", "a"} & got))
    got = complete("struct P {\n    i64 x\n}\n\ni64 P.twice(i64 by) {\n    return se|\n}\n")
    check("self and parameters: %r" % sorted(got), {"self", "by", "P"} <= got)
    got = complete("enum Kind { cat, dog }\n\nvoid main() {\n    k := Kind.|\n}\n")
    check("enum values: %r" % sorted(got), got == {"cat", "dog"})
    got = complete("import fs\n\n!void main() {\n    x := 3.\n    t := fs.rea|\n")
    check("after a dot, with an error above: %r" % sorted(got), {"readAll", "readDir"} <= got)
    got = complete("import json, ti|\n")
    check("import line: %r" % sorted(got), {"time", "json", "http", "sqlite", "core"} <= got)
    got = complete("import co|\n\nvoid main() {\n    print(\"{1}\")\n}\n")
    check("import co: %r" % sorted(got), {"core", "compress"} <= got)
    got = complete("import core\n\nvoid main() {\n    core.|\n}\n")
    check("after core.: %r" % sorted(got), {"List", "Map"} <= got)

    # attributes after @: the ones the declaration below takes
    got = complete("@|\nstruct User {\n    i64 id\n}\n")
    check("@ before a struct: %r" % sorted(got), got == {"json", "entity", "wire", "proto", "env", "flags"})
    got = complete("@json\n@e|\npub struct User {\n    i64 id\n}\n")
    check("@e under @json: %r" % sorted(got), got == {"json", "entity", "wire", "proto", "env", "flags"})
    got = complete("@entity(\"users\")\nstruct User {\n    @|\n    i64 id\n    str name = \"{x}\"\n}\n")
    check("@ on a field: %r" % sorted(got), got == {"json", "id", "column", "wire", "proto", "env", "flag"})
    got = complete("@|\n!void main() {\n}\n")
    check("@ before a function: %r" % sorted(got), got == {"inline", "unchecked", "noreturn", "export"})
    got = complete("@|\nenum Kind { cat, dog }\n")
    check("@ before an enum: %r" % sorted(got), got == {"wire"})
    got = complete("@|\n")
    check("@ before nothing: %r" % sorted(got),
          got == {"json", "entity", "wire", "proto", "env", "flags", "inline", "unchecked", "noreturn", "export"})
    got = complete("void main() {\n    // { not a brace\n    @|\n}\n")
    check("@ in a function body: %r" % sorted(got), got == set())

    # auto imports: a module that isn't imported yet completes, and taking a
    # name adds its import line
    def items(text):
        """the completion items at the | in text"""
        at = text.index("|")
        body = text.replace("|", "")
        version[0] += 1
        c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]},
                                            "contentChanges": [{"text": body}]})
        c.read()
        before = body[:at]
        pos = {"line": before.count("\n"), "character": len(before.split("\n")[-1])}
        comp = c.request("textDocument/completion", {"textDocument": td, "position": pos})
        return {x["label"]: x for x in (comp and comp["result"]) or []}

    def added(item):
        edits = item.get("additionalTextEdits") or []
        return [(e["range"]["start"]["line"], e["newText"]) for e in edits]

    got = items("void main() {\n    t := time.|\n}\n")
    check("time. without the import: %r" % sorted(got), "now" in got and added(got["now"]) == [(0, "import time\n\n")])
    got = items("import fs\nimport json\n\nvoid main() {\n    t := time.|\n}\n")
    check("time. under other imports: %r" % sorted(got), "now" in got and added(got["now"]) == [(2, "import time\n")])
    got = items("import time\n\nvoid main() {\n    t := time.|\n}\n")
    check("time. imported: %r" % sorted(got), "now" in got and added(got["now"]) == [])
    got = items("import fs\n\nvoid main() {\n    x := js|\n}\n")
    check("a module's name adds its import: %r" % sorted(got),
          "json" in got and added(got["json"]) == [(1, "import json\n")] and added(got["fs"]) == [])

    # a quick fix adds the import a name needs
    version[0] += 1
    body = "import fs\n\nvoid main() {\n    t := time.now()\n}\n"
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]}, "contentChanges": [{"text": body}]})
    diag = c.read()
    diags = (diag and diag["params"]["diagnostics"]) or []
    acts = c.request("textDocument/codeAction", {"textDocument": td, "range": diags[0]["range"] if diags else {},
                                                 "context": {"diagnostics": diags}})
    acts = (acts and acts["result"]) or []
    edits = [e for a in acts for e in a.get("edit", {}).get("changes", {}).get(uri, [])]
    check("quick fix for time: %r" % acts, [a["title"] for a in acts] == ["Import time"]
          and [(e["range"]["start"]["line"], e["newText"]) for e in edits] == [(1, "import time\n")])

    # snippets for functions and methods, signature help, and the fields that
    # a struct literal does not set yet
    def ask(text, method):
        """the result of the request at the | in text"""
        at = text.index("|")
        body = text.replace("|", "")
        version[0] += 1
        c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]},
                                            "contentChanges": [{"text": body}]})
        before = body[:at]
        pos = {"line": before.count("\n"), "character": len(before.split("\n")[-1])}
        r = c.request(method, {"textDocument": td, "position": pos})
        return r and r.get("result")

    head = ("struct P {\n    i64 x\n    i64 y\n    str name\n}\n\ni64 P.sum(i64 by) => self.x + by\n\n"
            "i64 add(i64 a, i64 b) => a + b\n\nvoid main() {\n    p := P{x: 1, y: 2, name: \"a\"}\n")
    got = {x["label"]: x for x in ask(head + "    ad|\n}\n", "textDocument/completion") or []}
    snip = lambda name: (got.get(name, {}).get("insertText"), got.get(name, {}).get("insertTextFormat"))
    check("snippet of add: %r" % (snip("add"),), snip("add") == ("add(${1:a}, ${2:b})", 2))
    check("snippet of main: %r" % (snip("main"),), snip("main") == ("main()", 2))
    check("snippet of print: %r" % (snip("print"),), snip("print") == ("print(${1:text})", 2))
    check("no snippet for a type: %r" % (snip("P"),), snip("P") == (None, None))
    got = {x["label"]: x for x in ask(head + "    p.|\n}\n", "textDocument/completion") or []}
    check("snippet of a method: %r" % (snip("sum"),), snip("sum") == ("sum(${1:by})", 2))
    check("no snippet for a field: %r" % (snip("x"),), snip("x") == (None, None))
    check("signature help trigger characters", init["result"]["capabilities"].get("signatureHelpProvider") ==
          {"triggerCharacters": ["(", ","]})

    def sig(text):
        r = ask(text, "textDocument/signatureHelp")
        if not r:
            return None
        s = r["signatures"][r["activeSignature"]]
        return s["label"], [s["label"][a:b] for a, b in (p["label"] for p in s["parameters"])], r["activeParameter"]

    want = ("i64 add(i64 a, i64 b)", ["i64 a", "i64 b"])
    check("signature at add(: %r" % (sig(head + "    n := add(|\n}\n"),), sig(head + "    n := add(|\n}\n") == want + (0,))
    check("signature at add(1, : %r" % (sig(head + "    n := add(1, |\n}\n"),),
          sig(head + "    n := add(1, |\n}\n") == want + (1,))
    check("signature with the ) typed: %r" % (sig(head + "    n := add(1, |)\n}\n"),),
          sig(head + "    n := add(1, |)\n}\n") == want + (1,))
    check("signature in a literal in a call: %r" % (sig(head + "    n := add(P{x: 1, y: 2|}.x)\n}\n"),),
          sig(head + "    n := add(P{x: 1, y: 2|}.x)\n}\n") == want + (0,))
    check("signature of a method: %r" % (sig(head + "    n := p.sum(|\n}\n"),),
          sig(head + "    n := p.sum(|\n}\n") == ("i64 P.sum(i64 by)", ["i64 by"], 0))
    check("signature of print: %r" % (sig(head + "    print(|\n}\n"),),
          sig(head + "    print(|\n}\n") == ("void print(str text)", ["str text"], 0))
    check("no signature outside a call: %r" % (sig(head + "    n := 1|\n}\n"),), sig(head + "    n := 1|\n}\n") is None)

    # inlay hints: the parameter names at calls with literal arguments
    check("inlay hint provider", init["result"]["capabilities"].get("inlayHintProvider") is True)
    body = ("struct P {\n    i64 x\n}\n\ni64 P.move(i64 dx, i64 dy) => self.x + dx + dy\n"
            "i64 add(i64 a, i64 b) => a + b\ni64 neg(i64 a) => -a\n"
            "void say(str text, bool loud, f64 level = 1.0) { }\n\nvoid main() {\n    a := 1\n    p := P{x: 1}\n"
            "    n := add(1, -2)\n    m := add(a, 2)\n    k := add(a, a)\n    o := add(b: 5, a: 3)\n    q := neg(4)\n"
            "    r := p.move(1, 2)\n    say(\"hi {a}\", true)\n    t := add(\n        7,\n        8)\n"
            "    print(\"{n}{m}{k}{o}{q}{r}{t}\")\n}\n")
    version[0] += 1
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]}, "contentChanges": [{"text": body}]})
    lines = body.split("\n")

    def hints(first, last):
        r = c.request("textDocument/inlayHint", {"textDocument": td, "range": {
            "start": {"line": first, "character": 0}, "end": {"line": last, "character": 0}}})
        return [(h["position"]["line"], h["label"], lines[h["position"]["line"]][h["position"]["character"]:][:4])
                for h in (r and r.get("result")) or []]

    got = hints(0, len(lines))
    check("inlay hints: %r" % got, got == [(12, "a:", "1, -"), (12, "b:", "-2)"), (13, "b:", "2)"),
                                           (17, "dx:", "1, 2"), (17, "dy:", "2)"), (18, "text:", '"hi '),
                                           (18, "loud:", "true"), (20, "a:", "7,"), (21, "b:", "8)")])
    got = hints(17, 18)
    check("inlay hints in a range: %r" % got, [h[0] for h in got] == [17, 17, 18, 18])

    # document highlights: the uses of a name in the file, 3 for a write and 2 for a read
    check("document highlight provider", init["result"]["capabilities"].get("documentHighlightProvider") is True)
    body = ("struct P {\n    i64 x\n}\n\ni64 P.get() => self.x\n\nvoid bump(i64 n) {\n    n += 1\n    print(\"{n}\")\n}\n\n"
            "void main() {\n    count := 0\n    count = count + 1\n    count++\n    p := P{x: count}\n    p.x = 5\n"
            "    print(\"{p.x} {p.get()}\")\n    for i in 0..count {\n        bump(i)\n    }\n    bump(count)\n}\n")
    version[0] += 1
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]}, "contentChanges": [{"text": body}]})

    def marks(line, character):
        r = c.request("textDocument/documentHighlight", {"textDocument": td, "position": {"line": line, "character": character}})
        return [(h["range"]["start"]["line"], h["range"]["start"]["character"],
                 h["range"]["end"]["character"] - h["range"]["start"]["character"], h["kind"])
                for h in (r and r.get("result")) or []]

    got = marks(13, 13)
    check("highlights of a local: %r" % got, got == [(12, 4, 5, 3), (13, 4, 5, 3), (13, 12, 5, 2), (14, 4, 5, 3),
                                                     (15, 14, 5, 2), (18, 16, 5, 2), (21, 9, 5, 2)])
    got = marks(1, 8)
    check("highlights of a field: %r" % got, got == [(1, 8, 1, 3), (4, 20, 1, 2), (15, 11, 1, 3), (16, 6, 1, 3),
                                                     (17, 14, 1, 2)])
    got = marks(8, 12)
    check("highlights of a parameter: %r" % got, got == [(6, 14, 1, 3), (7, 4, 1, 3), (8, 12, 1, 2)])
    got = marks(21, 5)
    check("highlights of a function: %r" % got, got == [(6, 5, 4, 3), (19, 8, 4, 2), (21, 4, 4, 2)])
    got = marks(10, 0)
    check("no highlights on a blank line: %r" % got, got == [])

    # semantic tokens: each name by what it names
    legend = init["result"]["capabilities"].get("semanticTokensProvider", {}).get("legend", {})
    check("semantic token legend: %r" % legend, legend == {
        "tokenTypes": ["namespace", "type", "struct", "enum", "enumMember", "parameter", "variable", "property",
                       "function", "method"],
        "tokenModifiers": ["declaration", "readonly", "defaultLibrary"]})
    body = ("import json, sqlite\n\nconst i64 MAX = 3\n\nenum Kind { cat, dog }\n\n@json\nstruct Point {\n    i64 x\n"
            "    Kind k\n}\n\ni64 Point.sum(i64 by) => self.x + by + MAX\n\nvoid main() {\n    p := Point{x: 1, k: .cat}\n"
            "    s := json.encode(p)\n    sqlite.Db db\n    for i in 0..p.sum(2) {\n        print(\"{i}\")\n    }\n"
            "    v := \"\u00e9\" + s\n}\n")
    version[0] += 1
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]}, "contentChanges": [{"text": body}]})
    r = c.request("textDocument/semanticTokens/full", {"textDocument": td})
    data = (r and r.get("result") or {}).get("data", [])
    lines, toks, line, col = body.split("\n"), [], 0, 0
    for i in range(0, len(data), 5):
        dl, dc, n, ty, md = data[i:i + 5]
        line += dl
        col = dc if dl else col + dc
        mods = "".join("DRS"[b] for b in range(3) if md >> b & 1)
        toks.append((line, lines[line][col:col + n], legend.get("tokenTypes", [""] * 10)[ty], mods))
    check("semantic tokens: %r" % toks, toks == [
        (0, "json", "namespace", "D"), (0, "sqlite", "namespace", "D"), (2, "MAX", "variable", "DR"),
        (4, "Kind", "enum", "D"), (4, "cat", "enumMember", "D"), (4, "dog", "enumMember", "D"),
        (7, "Point", "struct", "D"), (8, "x", "property", "D"), (9, "Kind", "enum", ""), (9, "k", "property", "D"),
        (12, "Point", "struct", ""), (12, "sum", "method", "D"), (12, "by", "parameter", "D"),
        (12, "self", "parameter", ""), (12, "x", "property", ""), (12, "by", "parameter", ""),
        (12, "MAX", "variable", "R"), (14, "main", "function", "D"), (15, "p", "variable", "D"),
        (15, "Point", "struct", ""), (15, "x", "property", ""), (15, "k", "property", ""),
        (15, "cat", "enumMember", ""), (16, "s", "variable", "D"), (16, "json", "namespace", ""),
        (16, "encode", "function", "S"), (16, "p", "variable", ""), (17, "sqlite", "namespace", ""),
        (17, "Db", "struct", "S"), (17, "db", "variable", "D"), (18, "i", "variable", "D"), (18, "p", "variable", ""),
        (18, "sum", "method", ""), (19, "print", "function", "S"), (19, "i", "variable", ""),
        (21, "v", "variable", "D"), (21, "s", "variable", "")])

    fields = lambda text: [x["label"] for x in ask(head + text, "textDocument/completion") or []]
    check("fields of P{: %r" % fields("    q := P{|}\n}\n"), fields("    q := P{|}\n}\n") == ["x", "y", "name"])
    check("fields of P{x: 1, : %r" % fields("    q := P{x: 1, |}\n}\n"), fields("    q := P{x: 1, |}\n}\n") == ["y", "name"])
    check("fields on lines: %r" % fields("    q := P{\n        name: \"a, x: b\",\n        |\n    }\n}\n"),
          fields("    q := P{\n        name: \"a, x: b\",\n        |\n    }\n}\n") == ["x", "y"])
    check("a block is no literal", "p" in fields("    if p.x > 0 {\n        |\n    }\n}\n"))
    # the last text has an error: the next test waits for the diagnostics that clear it
    check("a value in a literal: %r" % fields("    q := P{x: a|}\n}\n")[:3], "add" in fields("    q := P{x: a|}\n}\n"))

    # quick fixes: the diagnostics of a text, then the code actions for each one,
    # applied to the text
    def apply(text, edits):
        lines = text.split("\n")
        offs = [0]
        for l in lines:
            offs.append(offs[-1] + len(l) + 1)
        at = lambda p: offs[p["line"]] + p["character"] if p["line"] < len(lines) else len(text)
        for e in sorted(edits, key=lambda e: at(e["range"]["start"]), reverse=True):
            text = text[:at(e["range"]["start"])] + e["newText"] + text[at(e["range"]["end"]):]
        return text

    def fixes(text):
        """{message: [(title, preferred, text after the fix)]} for the diagnostics of text"""
        version[0] += 1
        c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]},
                                            "contentChanges": [{"text": text}]})
        diags = []
        while True:
            m = c.read()
            if m is None or (m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri):
                diags = (m and m["params"]["diagnostics"]) or []
                break
        out = {}
        for d in diags:
            acts = c.request("textDocument/codeAction", {"textDocument": td, "range": d["range"],
                                                         "context": {"diagnostics": [d]}})
            out[d["message"]] = [(a["title"], a.get("isPreferred", False), apply(text, a["edit"]["changes"][uri]))
                                 for a in (acts and acts.get("result")) or []]
        return out, diags

    text = ("import fs\n\nvoid main() {\n    t := readAll(\"x\") catch e {\n        return\n    }\n"
            "    print(\"{t}\")\n    h := sha256(\"x\")\n    print(\"{h.len}\")\n}\n")
    got, diags = fixes(text)
    check("fixes for a std name: %r" % got.get("readAll is not declared"), got.get("readAll is not declared") ==
          [("Use fs.readAll", True, text.replace("t := readAll", "t := fs.readAll")),
           ("Import io and use io.readAll", False, text.replace("t := readAll", "t := io.readAll").replace(
               "import fs\n", "import fs\nimport io\n"))])
    check("fix for a name of one module: %r" % got.get("sha256 is not declared"), got.get("sha256 is not declared") ==
          [("Import crypto and use crypto.sha256", True, text.replace("h := sha256", "h := crypto.sha256").replace(
              "import fs\n", "import fs\nimport crypto\n"))])
    text = "void main() {\n    u := Url{}\n    print(\"{u.host}\")\n}\n"
    got, diags = fixes(text)
    check("fix for a std struct: %r" % got, got.get("Url is not a struct") ==
          [("Import url and use url.Url", True, "import url\n\n" + text.replace("Url{}", "url.Url{}"))])

    body = ("i64 f() => 1\n\nvoid main() {\n    a := 1\n    i64 b = f()\n    c := [1,\n        2]\n    d := f()\n"
            "    x, y := (f(), 2)\n    print(\"{x}\")\n    Point p\n    scoped String s = format(\"a\")\n    _ = f()\n}\n"
            "\nstruct Point {\n    i64 x\n}\n")
    got, diags = fixes(body)
    warn = sorted((d["range"]["start"]["line"], d["message"], d["severity"], d.get("tags")) for d in diags)
    check("variables that are not used: %r" % warn, warn == [
        (3, "the variable a is not used", 2, [1]), (4, "the variable b is not used", 2, [1]),
        (5, "the variable c is not used", 2, [1]), (7, "the variable d is not used", 2, [1]),
        (8, "the variable y is not used", 2, [1]), (10, "the variable p is not used", 2, [1])])
    fix = lambda name: got.get("the variable %s is not used" % name, [])
    lines = body.split("\n")
    without = lambda *drop: "\n".join(l for i, l in enumerate(lines) if i not in drop)
    check("remove a: %r" % fix("a"), fix("a") == [("Remove the variable", True, without(3))])
    check("rename b: %r" % fix("b"), fix("b") == [("Rename to _", True, body.replace("i64 b = f()", "_ = f()"))])
    check("remove c on two lines: %r" % fix("c"), fix("c") == [("Remove the variable", True, without(5, 6))])
    check("rename d: %r" % fix("d"), fix("d") == [("Rename to _", True, body.replace("d := f()", "_ = f()"))])
    check("rename y: %r" % fix("y"), fix("y") == [("Rename to _", True, body.replace("x, y :=", "x, _ :="))])
    check("remove p: %r" % fix("p"), fix("p") == [("Remove the variable", True, without(10))])
    # an error in the program: no warnings, as the checker did not link every name
    got, diags = fixes("void main() {\n    a := 1\n    b := missing\n}\n")
    check("no warnings with an error: %r" % diags, [d["severity"] for d in diags] == [1])

    # fixing the error clears the diagnostics; formatting
    fixed = SOURCE.replace("    n := missing + 1\n", "    n:=1+2\n")
    version[0] += 1
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]}, "contentChanges": [{"text": fixed}]})
    diag = c.read()
    check("diagnostics cleared: %r" % diag, diag and [d for d in diag["params"]["diagnostics"] if d["severity"] == 1] == [])
    fm = c.request("textDocument/formatting", {"textDocument": td, "options": {"tabSize": 4, "insertSpaces": True}})
    edits = fm and fm["result"]
    check("formatting: %r" % edits, edits and "n := 1 + 2" in edits[0]["newText"])

    # definition in the standard library opens a copy of it
    g = c.request("textDocument/definition", {"textDocument": td, "position": {"line": 0, "character": 8}})
    loc = g and g["result"]
    check("definition of the module json: %r" % loc, loc and loc["uri"].endswith("/json/json.iris"))

    # in a project: names in type position, from another file of the module;
    # the receiver of a method; a field in a struct literal; a module's name
    proj = tempfile.mkdtemp()
    os.makedirs(os.path.join(proj, "cli"))
    files = {
        "iris.mod": "module example.com/shelf\n",
        "main.iris": "import cli\n\nvoid main() {\n    cli.run()\n}\n",
        "cli/model.iris": "enum Genre { fiction, science }\n",
        "cli/service.iris": ("import sqlite\n\npub struct Service {\n    sqlite.Db db\n}\n\n"
                             "Service make(sqlite.Db db) => Service{db: db}\n\n"
                             "void Service.add(str title, Genre genre) {\n}\n\npub void run() {\n}\n"),
    }
    for name, text in files.items():
        with open(os.path.join(proj, name), "w") as f:
            f.write(text)
    spath = os.path.join(proj, "cli", "service.iris")
    suri = "file://" + spath
    c.notify("textDocument/didOpen", {"textDocument": {"uri": suri, "languageId": "iris", "version": 1,
                                                       "text": files["cli/service.iris"]}})

    def where(line, character):
        g = c.request("textDocument/definition", {"textDocument": {"uri": suri},
                                                  "position": {"line": line, "character": character}})
        loc = g and g["result"]
        return loc and (loc["uri"].rsplit("/", 2)[-2:], loc["range"]["start"]["line"])

    check("definition of Genre in another file: %r" % (where(8, 32),), where(8, 32) == (["cli", "model.iris"], 0))
    check("definition of a return type: %r" % (where(6, 2),), where(6, 2) == (["cli", "service.iris"], 2))
    with open(os.path.join(os.path.dirname(__file__), "..", "std", "sqlite", "sqlite.iris")) as f:
        db_line = next(i for i, line in enumerate(f) if line.startswith("pub struct Db "))
    check("definition of sqlite.Db: %r" % (where(3, 12),), where(3, 12) == (["sqlite", "sqlite.iris"], db_line))
    check("definition of a module name: %r" % (where(3, 6),), where(3, 6) == (["sqlite", "sqlite.iris"], 0))
    check("definition of a method's receiver: %r" % (where(8, 7),), where(8, 7) == (["cli", "service.iris"], 2))
    check("definition of a field in a literal: %r" % (where(6, 38),), where(6, 38) == (["cli", "service.iris"], 3))

    # every kind of name: builtins of std, a call with errors, methods, generics,
    # match arms, loop and catch variables, a file not saved yet, and a std file
    # opened by go to definition
    gproj = tempfile.mkdtemp()
    os.makedirs(os.path.join(gproj, "models"))
    gfiles = {
        "iris.mod": "module example.com/probe\n",
        "models/models.iris": ("pub enum Role { admin, member }\n\n@json\npub struct User {\n    pub String name\n"
                               "    pub Role role\n}\n\npub i64 User.score() => self.name.len\n"),
        "main.iris": """import fs, json, flags
import "models"

interface Shape {
    f64 area()
}

struct Square {
    f64 side
}

f64 Square.area() => self.side * self.side

@json
struct Bad {
    str name
}

@flags("probe")
struct Options {
    u16 port = 8080
}

T biggest[T](T a, T b) => a if a > b else b

f64 total(dyn Shape s) => s.area()

!void main() {
    text := r"{}"
    b := json.decode(Bad, text) catch e {
        print("{e.message}")
        return
    }
    u := try json.decode(models.User, text)
    o := flags.parse(Options)
    List[i64] xs = List[i64]{}
    xs.push(biggest(u.score(), 2))
    for x in xs {
        print("{x} {o.port}")
    }
    match u.role {
        .admin => print("admin")
        .member => print(json.encode(u))
    }
}
""",
    }
    for name, text in gfiles.items():
        with open(os.path.join(gproj, name), "w") as f:
            f.write(text)
    gpath = os.path.join(gproj, "main.iris")
    glines = gfiles["main.iris"].split("\n")
    c.notify("textDocument/didOpen", {"textDocument": {"uri": "file://" + gpath, "languageId": "iris", "version": 1,
                                                       "text": gfiles["main.iris"]}})

    def goto(path, lines, line, word, nth=1):
        col = -1
        for _ in range(nth):
            col = lines[line].index(word, col + 1)
        g = c.request("textDocument/definition", {"textDocument": {"uri": "file://" + path},
                                                  "position": {"line": line, "character": col}})
        loc = g and g["result"]
        if not loc:
            return None
        start = loc["range"]["start"]
        with open(loc["uri"][7:]) as f:
            target = f.read().split("\n")[start["line"]]
        return loc["uri"].rsplit("/", 1)[-1], start["line"], target[start["character"]:].split("(")[0].split(" ")[0]

    cases = [
        ("json.decode in a call with errors", 29, "decode", 1, ("json.iris", None, "decode")),
        ("an argument after the error", 29, "text", 1, ("main.iris", 28, "text")),
        ("the catch variable", 29, "e {", 1, ("main.iris", 29, "e")),
        ("a use of the catch variable", 30, "e.", 1, ("main.iris", 29, "e")),
        ("a module's type as a value", 33, "User", 1, ("models.iris", 3, "User")),
        ("flags.parse", 34, "parse", 1, ("flags.iris", None, "parse")),
        ("json.encode", 42, "encode", 1, ("json.iris", None, "encode")),
        ("List in a type", 35, "List", 1, ("list.iris", None, "List[T]")),
        ("List in a value", 35, "List", 2, ("list.iris", None, "List[T]")),
        ("a generic method", 36, "push", 1, ("list.iris", None, "push")),
        ("a generic function", 36, "biggest", 1, ("main.iris", 23, "biggest[T]")),
        ("a method of another module", 36, "score", 1, ("models.iris", 8, "score")),
        ("a loop variable", 37, "x", 1, ("main.iris", 37, "x")),
        ("a match arm", 41, "admin", 1, ("models.iris", 0, "admin,")),
        ("a method's name where it is declared", 11, "area", 1, ("main.iris", 11, "area")),
        ("an interface after dyn", 25, "Shape", 1, ("main.iris", 3, "Shape")),
        ("a type parameter", 23, "T", 1, ("main.iris", 23, "T]")),
        ("a type parameter in a parameter", 23, "T a", 1, ("main.iris", 23, "T]")),
        ("print, a built-in", 30, "print", 1, ("builtin.iris", None, "print")),
        ("an Error's message", 30, "message", 1, ("builtin.iris", None, "message")),
    ]
    for what, line, word, nth, want in cases:
        got = goto(gpath, glines, line, word, nth)
        ok = got and got[0] == want[0] and (want[1] is None or got[1] == want[1]) and got[2] == want[2]
        check("definition of %s: %r" % (what, got), ok)

    # a file not saved yet, in the project
    npath = os.path.join(gproj, "models", "extra.iris")
    ntext = "pub i64 twice(User u) => u.score() * 2\n"
    c.notify("textDocument/didOpen", {"textDocument": {"uri": "file://" + npath, "languageId": "iris", "version": 1,
                                                       "text": ntext}})
    got = goto(npath, ntext.split("\n"), 0, "User")
    check("definition in a file not saved yet: %r" % (got,), got and got[0] == "models.iris" and got[1] == 3)

    # find usages and rename: locals, parameters, fields, methods, types, type
    # parameters, generics, enum members, and names in another module of the project
    rproj = tempfile.mkdtemp()
    os.makedirs(os.path.join(rproj, "models"))
    rfiles = {
        "iris.mod": "module example.com/refs\n",
        "models/models.iris": ("pub enum Role { admin, member }\n\npub struct User {\n    pub String name\n"
                               "    pub Role role\n}\n\npub i64 User.score() => self.name.len\n\n"
                               "pub i64 twice(i64 n) => n * 2\n"),
        "main.iris": """import "models"

struct Point {
    i64 x
    i64 y
}

i64 Point.sum() => self.x + self.y

T biggest[T](T a, T b) => a if a > b else b

i64 other() {
    p := 5
    return p
}

void main() {
    p := Point{x: 1, y: 2}
    total := p.sum() + p.x
    print("{p.x} {total} {other()}")
    u := models.User{name: "a".copy(), role: .admin}
    print("{u.score()} {models.twice(total)} {biggest(total, 3)}")
    if u.role == .admin {
        print("admin")
    }
    u.name.free()
    List[i64] xs = List[i64]{}
    xs.push(1)
    xs.free()
}
""",
    }
    for name, text in rfiles.items():
        with open(os.path.join(rproj, name), "w") as f:
            f.write(text)
    rpath = os.path.join(rproj, "main.iris")
    rlines = rfiles["main.iris"].split("\n")
    mpath = os.path.join(rproj, "models", "models.iris")
    mlines = rfiles["models/models.iris"].split("\n")
    c.notify("textDocument/didOpen", {"textDocument": {"uri": "file://" + rpath, "languageId": "iris", "version": 1,
                                                       "text": rfiles["main.iris"]}})

    def at(path, lines, line, word, nth=1):
        col = -1
        for _ in range(nth):
            col = lines[line].index(word, col + 1)
        return {"textDocument": {"uri": "file://" + path}, "position": {"line": line, "character": col}}

    def usages(path, lines, line, word, nth=1, decl=True):
        params = at(path, lines, line, word, nth)
        params["context"] = {"includeDeclaration": decl}
        r = c.request("textDocument/references", params)
        res = r and r.get("result")
        if res is None:
            return None
        return sorted((x["uri"].rsplit("/", 1)[-1], x["range"]["start"]["line"], x["range"]["start"]["character"],
                       x["range"]["end"]["character"]) for x in res)

    m, mo = "main.iris", "models.iris"
    cases = [
        ("a local", (rpath, rlines, 17, "p"), [(m, 17, 4, 5), (m, 18, 13, 14), (m, 18, 23, 24), (m, 19, 12, 13)]),
        ("a local with the name of a local in another function", (rpath, rlines, 12, "p"),
         [(m, 12, 4, 5), (m, 13, 11, 12)]),
        ("a field", (rpath, rlines, 3, "x"),
         [(m, 3, 8, 9), (m, 7, 24, 25), (m, 17, 15, 16), (m, 18, 25, 26), (m, 19, 14, 15)]),
        ("a method", (rpath, rlines, 18, "sum"), [(m, 7, 10, 13), (m, 18, 15, 18)]),
        ("a struct", (rpath, rlines, 2, "Point"), [(m, 2, 7, 12), (m, 7, 4, 9), (m, 17, 9, 14)]),
        ("a parameter", (rpath, rlines, 9, "a", 2), [(m, 9, 15, 16), (m, 9, 26, 27), (m, 9, 31, 32)]),
        ("a type parameter", (rpath, rlines, 9, "T"), [(m, 9, 0, 1), (m, 9, 10, 11), (m, 9, 13, 14), (m, 9, 18, 19)]),
        ("a generic function", (rpath, rlines, 21, "biggest"), [(m, 9, 2, 9), (m, 21, 46, 53)]),
        ("a type of another module", (rpath, rlines, 20, "User"), [(m, 20, 16, 20), (mo, 2, 11, 15), (mo, 7, 8, 12)]),
        ("a method of another module", (rpath, rlines, 21, "score"), [(m, 21, 14, 19), (mo, 7, 13, 18)]),
        ("a function of another module", (rpath, rlines, 21, "twice"), [(m, 21, 31, 36), (mo, 9, 8, 13)]),
        ("an enum member", (rpath, rlines, 20, "admin"), [(m, 20, 46, 51), (m, 22, 18, 23), (mo, 0, 16, 21)]),
        ("from the module's own file", (mpath, mlines, 2, "User"), [(m, 20, 16, 20), (mo, 2, 11, 15), (mo, 7, 8, 12)]),
    ]
    for what, where_, want in cases:
        got = usages(*where_)
        check("usages of %s: %r" % (what, got), got == want)
    got = usages(rpath, rlines, 18, "sum", decl=False)
    check("usages without the declaration: %r" % (got,), got == [(m, 18, 15, 18)])
    got = usages(rpath, rlines, 19, "print")
    check("usages of a built-in: %r" % (got,), got == [])

    def prepare(path, lines, line, word, nth=1):
        r = c.request("textDocument/prepareRename", at(path, lines, line, word, nth))
        return r and (r["result"]["placeholder"] if r.get("result") else r.get("error", {}).get("message"))

    check("prepare a rename of a local", prepare(rpath, rlines, 17, "p") == "p")
    check("prepare a rename of an enum member", prepare(rpath, rlines, 22, "admin") == "admin")
    got = prepare(rpath, rlines, 19, "print")
    check("no rename of a built-in: %r" % (got,), got == "a built-in name can't be renamed")
    got = prepare(rpath, rlines, 27, "push")
    check("no rename of a std name: %r" % (got,), got == "a name of the standard library can't be renamed")

    def rename(path, lines, line, word, new, nth=1):
        params = at(path, lines, line, word, nth)
        params["newName"] = new
        return c.request("textDocument/rename", params)

    for bad in ["for", "1x", "a-b", "", "self"]:
        r = rename(rpath, rlines, 3, "x", bad)
        check("refuse the name %r: %r" % (bad, r), r and "error" in r and "is not a valid name" in r["error"]["message"])

    def apply(changes):
        texts = {}
        for uri, edits in changes.items():
            with open(uri[7:]) as f:
                lines = f.read().split("\n")
            for e in sorted(edits, key=lambda e: (e["range"]["start"]["line"], e["range"]["start"]["character"]),
                            reverse=True):
                s, n = e["range"]["start"], e["range"]["end"]
                assert s["line"] == n["line"]
                ln = lines[s["line"]]
                lines[s["line"]] = ln[:s["character"]] + e["newText"] + ln[n["character"]:]
            texts[uri[7:]] = "\n".join(lines)
        return texts

    # a rename gives a program that still compiles, with the new name everywhere
    for what, where_, new in [("a field", (rpath, rlines, 3, "x"), "px"),
                              ("an enum member", (rpath, rlines, 20, "admin"), "boss"),
                              ("a type of another module", (mpath, mlines, 2, "User"), "Person"),
                              ("a method", (rpath, rlines, 7, "sum"), "total")]:
        r = rename(*where_, new)
        changes = r and r.get("result") and r["result"]["changes"]
        if not changes:
            check("rename %s: %r" % (what, r), False)
            continue
        texts = apply(changes)
        copy = tempfile.mkdtemp()
        for name, text in rfiles.items():
            os.makedirs(os.path.dirname(os.path.join(copy, name)), exist_ok=True)
            full = os.path.join(rproj, name)
            with open(os.path.join(copy, name), "w") as f:
                f.write(texts.get(full, text))
        p = subprocess.run([IRIS, "check", os.path.join(copy, "main.iris")], capture_output=True, text=True)
        check("rename %s compiles: %s" % (what, p.stdout + p.stderr), p.returncode == 0)
        check("rename %s changes %d places" % (what, sum(len(e) for e in changes.values())),
              all(new in t for t in texts.values()))

    # a std file opened by go to definition: its names resolve and it has no errors
    loc = c.request("textDocument/definition", {"textDocument": {"uri": "file://" + gpath},
                                                "position": {"line": 29, "character": glines[29].index("decode")}})
    jpath = loc["result"]["uri"][7:]
    with open(jpath) as f:
        jtext = f.read()
    c.notify("textDocument/didOpen", {"textDocument": {"uri": "file://" + jpath, "languageId": "iris", "version": 1,
                                                       "text": jtext}})
    jlines = jtext.split("\n")
    jline = next(i for i, l in enumerate(jlines) if "rawDecode(table" in l)
    got = goto(jpath, jlines, jline, "rawDecode")
    check("definition inside a std file: %r" % (got,), got and got[0] == "json.iris" and got[2] == "rawDecode")
    c.notify("textDocument/didSave", {"textDocument": {"uri": "file://" + jpath}})
    sym = c.request("textDocument/documentSymbol", {"textDocument": {"uri": "file://" + jpath}})
    check("symbols of a std file: %r" % (sym and len(sym["result"]),), sym and len(sym["result"]) > 3)
    check("std symbol names inside their ranges", sym and all(inside(x) for x in sym["result"]))

    # workspace symbols: a second server with the shelf project as its folder
    w = Client()
    winit = w.request("initialize", {"capabilities": {}, "rootUri": "file://" + proj})
    check("workspace symbol provider", winit["result"]["capabilities"].get("workspaceSymbolProvider") is True)

    def wsym(query):
        r = w.request("workspace/symbol", {"query": query})
        return [(x["name"], x["kind"], x.get("containerName"), x["location"]["uri"].rsplit("/", 2)[-2:],
                 x["location"]["range"]["start"]["line"]) for x in (r and r.get("result")) or []]

    got = wsym("genre")
    check("workspace symbol by a part: %r" % got, got[:1] == [("Genre", 10, None, ["cli", "model.iris"], 0)])
    got = wsym("SA")
    check("workspace symbol by the starts of words: %r" % got, ("Service.add", 6, None, ["cli", "service.iris"], 8) in got)
    got = [x for x in wsym("readall") if x[2] == "fs"]
    check("workspace symbol in std: %r" % got, ("readAll", 12, "fs", ["fs", "fs.iris"], 35) in got)
    check("workspace symbol with no match", wsym("zzzzq") == [])
    w.request("shutdown", None)
    w.notify("exit", None)
    w.p.wait(timeout=10)

    shut = c.request("shutdown", None)
    check("shutdown", shut is not None and "result" in shut)
    c.notify("exit", None)
    check("exit status", c.p.wait(timeout=10) == 0)

    for f in failures:
        print("FAIL lsp: " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
