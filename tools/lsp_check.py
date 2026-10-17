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
    names = [x["name"] for x in (s and s["result"]) or []]
    check("symbols: %r" % names, names == ["Point", "Point.sum", "main"])

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

    # completion of any name while a line is half typed: locals in scope, the
    # module's names, imports, built-ins; and module names in an import line
    version = [3]

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

    # fixing the error clears the diagnostics; formatting
    fixed = SOURCE.replace("    n := missing + 1\n", "    n:=1+2\n")
    version[0] += 1
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": version[0]}, "contentChanges": [{"text": fixed}]})
    diag = c.read()
    check("diagnostics cleared: %r" % diag, diag and diag["params"]["diagnostics"] == [])
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
    check("definition of sqlite.Db: %r" % (where(3, 12),), where(3, 12) == (["sqlite", "sqlite.iris"], 82))
    check("definition of a module name: %r" % (where(3, 6),), where(3, 6) == (["sqlite", "sqlite.iris"], 0))
    check("definition of a method's receiver: %r" % (where(8, 7),), where(8, 7) == (["cli", "service.iris"], 2))
    check("definition of a field in a literal: %r" % (where(6, 38),), where(6, 38) == (["cli", "service.iris"], 3))

    shut = c.request("shutdown", None)
    check("shutdown", shut is not None and "result" in shut)
    c.notify("exit", None)
    check("exit status", c.p.wait(timeout=10) == 0)

    for f in failures:
        print("FAIL lsp: " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
