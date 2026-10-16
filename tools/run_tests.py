#!/usr/bin/env python3
"""Runs the Iris tests.

  tests/parse/NAME.iris    `iris parse` must succeed and print NAME.tree
  tests/errors/NAME.iris   `iris parse` must fail with the errors listed at
                           the top of the file, one per line:
                               // error: LINE: MESSAGE
  docs                     every ```iris block in docs-src/ must parse
                           (```iris error blocks are skipped)

Usage: python3 tools/run_tests.py [--iris PATH] [--update] [NAME...]
  --update   rewrite the .tree files from the current output
  NAME       run only tests whose name contains NAME ("docs" for the docs)
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(iris, args):
    p = subprocess.run([iris] + args, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout, p.stderr


def parse_tests(iris, only, update, failures):
    count = 0
    for path in sorted(glob.glob(os.path.join(ROOT, "tests/parse/*.iris"))):
        name = "parse/" + os.path.basename(path)[:-5]
        if only and not any(o in name for o in only):
            continue
        count += 1
        tree = path[:-5] + ".tree"
        code, out, err = run(iris, ["parse", path])
        if code != 0:
            failures.append((name, "did not parse:\n" + err))
            continue
        if update or not os.path.exists(tree):
            with open(tree, "w") as f:
                f.write(out)
            continue
        with open(tree) as f:
            want = f.read()
        if out != want:
            failures.append((name, diff(want, out)))
    return count


def diff(want, got):
    w, g = want.splitlines(), got.splitlines()
    for i in range(max(len(w), len(g))):
        a = w[i] if i < len(w) else "<missing>"
        b = g[i] if i < len(g) else "<missing>"
        if a != b:
            return "tree differs at line %d:\n  want: %s\n  got:  %s" % (i + 1, a, b)
    return "tree differs"


def error_tests(iris, only, failures):
    count = 0
    for path in sorted(glob.glob(os.path.join(ROOT, "tests/errors/*.iris"))):
        name = "errors/" + os.path.basename(path)[:-5]
        if only and not any(o in name for o in only):
            continue
        count += 1
        with open(path) as f:
            expected = re.findall(r"^// error: (\d+): (.*)$", f.read(), re.M)
        if not expected:
            failures.append((name, "no // error: lines at the top"))
            continue
        code, out, err = run(iris, ["parse", path])
        if code == 0:
            failures.append((name, "parsed, but should have failed"))
            continue
        for line, msg in expected:
            pat = re.compile(r":%s:\d+: error: %s$" % (line, re.escape(msg.strip())), re.M)
            if not pat.search(err):
                failures.append((name, "missing error at line %s: %s\ngot:\n%s" % (line, msg, err)))
                break
    return count


def doc_tests(iris, only, failures):
    if only and not any(o in "docs" for o in only):
        return 0
    count = 0
    files = sorted(glob.glob(os.path.join(ROOT, "docs-src/**/*.md"), recursive=True))
    with tempfile.TemporaryDirectory() as tmp:
        for md in files:
            rel = os.path.relpath(md, ROOT)
            lines = open(md).read().split("\n")
            i = 0
            while i < len(lines):
                m = re.match(r"^```\s*(\S*)\s*(\S*)", lines[i])
                if not m or not lines[i].startswith("```"):
                    i += 1
                    continue
                lang, mode = m.group(1), m.group(2)
                start = i + 1
                j = start
                while j < len(lines) and not lines[j].startswith("```"):
                    j += 1
                if lang == "iris" and mode != "error":
                    count += 1
                    src = os.path.join(tmp, "example.iris")
                    with open(src, "w") as f:
                        f.write("\n".join(lines[start:j]) + "\n")
                    code, out, err = run(iris, ["parse", "--snippet", src])
                    if code != 0:
                        err = err.replace(src, "%s (block at line %d)" % (rel, start))
                        failures.append(("docs %s:%d" % (rel, start), err))
                i = j + 1
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iris", default=os.path.join(ROOT, "iris"))
    ap.add_argument("--update", action="store_true")
    ap.add_argument("only", nargs="*")
    args = ap.parse_args()

    failures = []
    total = 0
    total += parse_tests(args.iris, args.only, args.update, failures)
    total += error_tests(args.iris, args.only, failures)
    total += doc_tests(args.iris, args.only, failures)

    for name, why in failures:
        print("FAIL %s\n  %s" % (name, why.rstrip().replace("\n", "\n  ")))
    print("%d tests, %d passed, %d failed" % (total, total - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
