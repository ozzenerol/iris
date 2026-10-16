#!/usr/bin/env python3
"""Runs the Iris tests.

  tests/parse/NAME.iris    `iris parse` must succeed and print NAME.tree
  tests/errors/NAME.iris   `iris check` must fail with the errors listed at
                           the top of the file, one per line:
                               // error: LINE: MESSAGE
  tests/run/NAME.iris      `iris run` must print what the comments say:
                               // expect: LINE     one per line of output
                               // exit: N          the exit status (default 0)
                               // panic: MESSAGE   stderr has "panic: MESSAGE", exit 2
                               // stderr: TEXT     stderr has TEXT
                               // mode: MODE       build mode (default debug)
  docs                     every ```iris block in docs-src/ must parse
                           (```iris error blocks are skipped); an ```iris run
                           block must also run and print the ```output block
                           that follows it

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
        code, out, err = run(iris, ["check", path])
        if code == 0:
            failures.append((name, "compiled, but should have failed"))
            continue
        for line, msg in expected:
            pat = re.compile(r":%s:\d+: error: %s$" % (line, re.escape(msg.strip())), re.M)
            if not pat.search(err):
                failures.append((name, "missing error at line %s: %s\ngot:\n%s" % (line, msg, err)))
                break
    return count


def run_tests(iris, only, failures):
    count = 0
    for path in sorted(glob.glob(os.path.join(ROOT, "tests/run/*.iris"))):
        name = "run/" + os.path.basename(path)[:-5]
        if only and not any(o in name for o in only):
            continue
        count += 1
        text = open(path).read()
        expect = re.findall(r"^// expect:(?: (.*))?$", text, re.M)
        exits = re.findall(r"^// exit: (\d+)$", text, re.M)
        panics = re.findall(r"^// panic: (.*)$", text, re.M)
        modes = re.findall(r"^// mode: (\S+)$", text, re.M)
        stderrs = re.findall(r"^// stderr: (.*)$", text, re.M)
        want_exit = int(exits[0]) if exits else (2 if panics or stderrs else 0)
        args = ["run"] + (["--mode=" + modes[0]] if modes else []) + [path]
        code, out, err = run(iris, args)
        want_out = "".join(line + "\n" for line in expect)
        problems = []
        if out != want_out:
            problems.append(diff(want_out, out).replace("tree", "output"))
        if code != want_exit:
            problems.append("exit status %d, want %d" % (code, want_exit))
        for p in panics:
            if "panic: " + p not in err:
                problems.append("stderr lacks 'panic: %s'" % p)
        for p in stderrs:
            if p not in err:
                problems.append("stderr lacks '%s'" % p)
        if problems:
            failures.append((name, "\n".join(problems) + ("\nstderr:\n" + err if err else "")))
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
                    where = "docs %s:%d" % (rel, start)
                    code, out, err = run(iris, ["parse", "--snippet", src])
                    if code != 0:
                        err = err.replace(src, "%s (block at line %d)" % (rel, start))
                        failures.append((where, err))
                    elif mode == "run":
                        # a whole program: it runs, and prints the ```output block after it
                        k = j + 1
                        while k < len(lines) and not lines[k].strip():
                            k += 1
                        want = None
                        if k < len(lines) and lines[k].startswith("```output"):
                            m2 = k + 1
                            while m2 < len(lines) and not lines[m2].startswith("```"):
                                m2 += 1
                            want = "".join(l + "\n" for l in lines[k + 1:m2])
                        code, out, err = run(iris, ["run", src])
                        if code != 0:
                            failures.append((where, "exit status %d\n%s" % (code, err.replace(src, rel))))
                        elif want is not None and out != want:
                            failures.append((where, diff(want, out).replace("tree", "output")))
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
    total += run_tests(args.iris, args.only, failures)
    total += doc_tests(args.iris, args.only, failures)

    for name, why in failures:
        print("FAIL %s\n  %s" % (name, why.rstrip().replace("\n", "\n  ")))
    print("%d tests, %d passed, %d failed" % (total, total - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
