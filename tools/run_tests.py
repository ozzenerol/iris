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
                               // command: test    use iris test (or bench) instead of iris run
                               // args: A B        arguments after the file
                               // env: NAME=VALUE  set for the run
                               // stdin: LINE      one per line of input
                               // tty: KEYS        run in a terminal and type KEYS
                                                   (Python escapes: \r, \x1b[D), one
                                                   line at a time once the output
                                                   is quiet; each // expect: line
                                                   must then be in the output
  tests/pg/NAME.iris       as tests/run, with a PostgreSQL server that
                           tools/pg-cluster.sh starts (IRIS_PG_URL); skipped
                           when there is no PostgreSQL
  tests/redis/NAME.iris    the same with tools/redis-server.sh (IRIS_REDIS_URL)
  tests/projects/NAME/     a folder with iris.mod: `iris run` on the folder must
                           print what main.iris's comments say; with
                           // error: FILE:LINE: MESSAGE lines, `iris check` must fail
  tests/fmt/NAME.iris      `iris fmt -` must print NAME.out, and NAME.out must
                           stay as it is when formatted again
  fmt-std                  the standard library is in the standard layout
  get                      iris get with a local git repository (needs git)
  lsp                      tools/lsp_check.py: an editor session with iris lsp
  std/FILE                 a standard library file with test blocks: `iris test`
                           on it alone must pass, so such a file needs nothing
                           else from its module
  docs                     every ```iris block in docs-src/ must parse
                           (```iris error blocks are skipped); an ```iris run
                           block must also run and print the ```output block
                           that follows it

Usage: python3 tools/run_tests.py [--iris PATH] [--update] [NAME...]
  --update   rewrite the .tree and fmt .out files from the current output
  NAME       run only tests whose name contains NAME ("docs" for the docs,
             "docs-src/tour/03" for one page's examples)
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# tests run several at a time; each is a separate process
POOL = ThreadPoolExecutor(max_workers=int(os.environ.get("IRIS_TEST_JOBS", os.cpu_count() or 4)))


def gather(jobs, failures):
    """Waits for the jobs, in order, adding the failures each one gives."""
    for j in jobs:
        failures.extend(j.result())
    return len(jobs)


def run(iris, args, env=None, stdin=None):
    full = dict(os.environ, **(env or {}))
    p = subprocess.run([iris] + args, capture_output=True, text=True, timeout=60, env=full,
                       **({"input": stdin} if stdin is not None else {"stdin": subprocess.DEVNULL}))
    return p.returncode, p.stdout, p.stderr


def run_tty(iris, args, env, keys):
    """Runs iris in a pseudo-terminal and types each of keys once the output
    has been quiet for a while. Gives the exit status and everything shown,
    without escape codes."""
    import pty
    import select
    import time
    master, slave = pty.openpty()
    full = dict(os.environ, TERM="xterm", **(env or {}))
    full.pop("NO_COLOR", None)
    p = subprocess.Popen([iris] + args, stdin=slave, stdout=slave, stderr=slave, env=full, close_fds=True)
    os.close(slave)
    shown = b""

    def drain(quiet, limit):
        nonlocal shown
        end = time.time() + limit
        last = time.time()
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                try:
                    data = os.read(master, 4096)
                except OSError:
                    return False
                if not data:
                    return False
                shown += data
                last = time.time()
            elif shown and time.time() - last > quiet:
                return True
        return True

    for k in keys:
        if not drain(0.3, 30):
            break
        os.write(master, k.encode("utf-8").decode("unicode_escape").encode("latin-1"))
    drain(0.5, 10)
    try:
        code = p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        code = -1
    os.close(master)
    text = shown.decode("utf-8", "replace")
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z~]", "", text).replace("\r", "")
    return code, text


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


def fmt_tests(iris, only, update, failures):
    count = 0
    for path in sorted(glob.glob(os.path.join(ROOT, "tests/fmt/*.iris"))):
        name = "fmt/" + os.path.basename(path)[:-5]
        if only and not any(o in name for o in only):
            continue
        count += 1
        want_path = path[:-5] + ".out"
        with open(path) as f:
            src = f.read()
        p = subprocess.run([iris, "fmt", "-"], input=src, capture_output=True, text=True, timeout=60)
        if p.returncode != 0:
            failures.append((name, "fmt failed:\n" + p.stderr))
            continue
        if update or not os.path.exists(want_path):
            with open(want_path, "w") as f:
                f.write(p.stdout)
            continue
        with open(want_path) as f:
            want = f.read()
        if p.stdout != want:
            failures.append((name, diff(want, p.stdout)))
            continue
        again = subprocess.run([iris, "fmt", "-"], input=want, capture_output=True, text=True, timeout=60)
        if again.stdout != want:
            failures.append((name, "formatting the result again changes it:\n" + diff(want, again.stdout)))
    if not only or any(o in "fmt-std" for o in only):
        count += 1
        p = subprocess.run([iris, "fmt", "--check", os.path.join(ROOT, "std")], capture_output=True, text=True,
                           timeout=60)
        if p.returncode != 0:
            failures.append(("fmt-std", "not in the standard layout:\n" + p.stdout + p.stderr))
    if not only or any(o in "fmt-docs" for o in only):
        count += 1
        bad = []
        for md in sorted(glob.glob(os.path.join(ROOT, "docs-src/**/*.md"), recursive=True)):
            with open(md) as f:
                text = f.read()
            for m in re.finditer(r"```iris( run| step| whole)?\n(.*?)```", text, re.S):
                q = subprocess.run([iris, "fmt", "-"], input=m.group(2), capture_output=True, text=True, timeout=60)
                if q.returncode != 0 or q.stdout != m.group(2):
                    line = text[:m.start()].count("\n") + 1
                    bad.append("%s:%d" % (os.path.relpath(md, ROOT), line))
        if bad:
            failures.append(("fmt-docs", "examples not in the standard layout:\n" + "\n".join(bad)))
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
    jobs = []
    for path in sorted(glob.glob(os.path.join(ROOT, "tests/errors/*.iris"))):
        name = "errors/" + os.path.basename(path)[:-5]
        if only and not any(o in name for o in only):
            continue
        jobs.append(POOL.submit(error_test, iris, path, name))
    return gather(jobs, failures)


def error_test(iris, path, name):
    with open(path) as f:
        expected = re.findall(r"^// error: (\d+): (.*)$", f.read(), re.M)
    if not expected:
        return [(name, "no // error: lines at the top")]
    code, out, err = run(iris, ["check", path])
    if code == 0:
        return [(name, "compiled, but should have failed")]
    for line, msg in expected:
        pat = re.compile(r":%s:\d+: error: %s$" % (line, re.escape(msg.strip())), re.M)
        if not pat.search(err):
            return [(name, "missing error at line %s: %s\ngot:\n%s" % (line, msg, err))]
    return []


def run_tests(iris, only, failures, folder="run", env=None):
    jobs = []
    for path in sorted(glob.glob(os.path.join(ROOT, "tests", folder, "*.iris"))):
        name = folder + "/" + os.path.basename(path)[:-5]
        if only and not any(o in name for o in only):
            continue
        jobs.append(POOL.submit(run_test, iris, path, name, env))
    return gather(jobs, failures)


def run_test(iris, path, name, env):
    failures = []
    text = open(path).read()
    expect = re.findall(r"^// expect:(?: (.*))?$", text, re.M)
    exits = re.findall(r"^// exit: (\d+)$", text, re.M)
    panics = re.findall(r"^// panic: (.*)$", text, re.M)
    modes = re.findall(r"^// mode: (\S+)$", text, re.M)
    stderrs = re.findall(r"^// stderr: (.*)$", text, re.M)
    want_exit = int(exits[0]) if exits else (2 if panics or stderrs else 0)
    commands = re.findall(r"^// command: (\S+)$", text, re.M)
    extra = re.findall(r"^// args: (.*)$", text, re.M)
    args = [commands[0] if commands else "run"] + (["--mode=" + modes[0]] if modes else []) + [path]
    args += extra[0].split() if extra else []
    envs = re.findall(r"^// env: (\S+?)=(.*)$", text, re.M) + list((env or {}).items())
    stdin = re.findall(r"^// stdin:(?: (.*))?$", text, re.M)
    keys = re.findall(r"^// tty: (.*)$", text, re.M)
    problems = []
    if keys:
        code, out = run_tty(iris, args, dict(envs), keys)
        err = ""
        for line in expect:
            if line not in out:
                problems.append("the terminal didn't show '%s'" % line)
        if problems:
            problems.append("it showed:\n" + out)
        if code != want_exit:
            problems.append("exit status %d, want %d" % (code, want_exit))
        if problems:
            failures.append((name, "\n".join(problems)))
        return failures
    code, out, err = run(iris, args, dict(envs), "".join(l + "\n" for l in stdin) if stdin else None)
    want_out = "".join(line + "\n" for line in expect)
    if "…" in want_out:
        bad = output_mismatch(want_out, out)
        if bad:
            problems.append(bad)
    elif out != want_out:
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
    return failures


def project_tests(iris, only, failures):
    count = 0
    for d in sorted(glob.glob(os.path.join(ROOT, "tests/projects/*/"))):
        name = "projects/" + os.path.basename(d.rstrip("/"))
        if only and not any(o in name for o in only):
            continue
        count += 1
        text = open(os.path.join(d, "main.iris")).read()
        errors = re.findall(r"^// error: (\S+?):(\d+): (.*)$", text, re.M)
        if errors:
            code, out, err = run(iris, ["check", d])
            if code == 0:
                failures.append((name, "compiled, but should have failed"))
                continue
            for f, line, msg in errors:
                pat = re.compile(r"%s:%s:\d+: error: %s$" % (re.escape(f), line, re.escape(msg.strip())), re.M)
                if not pat.search(err):
                    failures.append((name, "missing error %s:%s: %s\ngot:\n%s" % (f, line, msg, err)))
                    break
            continue
        expect = re.findall(r"^// expect:(?: (.*))?$", text, re.M)
        code, out, err = run(iris, ["run", d])
        want = "".join(l + "\n" for l in expect)
        if code != 0 or out != want:
            failures.append((name, "exit %d\n%s%s" % (code, diff(want, out).replace("tree", "output"), err)))
    return count


def mod_init_test(iris, only, failures):
    """iris mod init makes iris.mod, from the folder's name or a path, once"""
    if only and not any(o in "mod-init" for o in only):
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        app = os.path.join(tmp, "shelf")
        os.makedirs(app)
        steps = []
        for args in (["mod", "init"], ["mod", "init"]):
            p = subprocess.run([iris] + args, cwd=app, capture_output=True, text=True)
            steps.append((p.returncode, open(os.path.join(app, "iris.mod")).read()))
        os.remove(os.path.join(app, "iris.mod"))
        p = subprocess.run([iris, "mod", "init", "github.com/me/shelf"], cwd=app, capture_output=True, text=True)
        steps.append((p.returncode, open(os.path.join(app, "iris.mod")).read()))
        os.remove(os.path.join(app, "iris.mod"))
        p = subprocess.run([iris, "mod", "init", "bad path"], cwd=app, capture_output=True, text=True)
        steps.append((p.returncode, os.path.exists(os.path.join(app, "iris.mod"))))
        want = [(0, "module shelf\n"), (1, "module shelf\n"), (0, "module github.com/me/shelf\n"), (1, False)]
        if steps != want:
            failures.append(("mod-init", "want %r\ngot  %r" % (want, steps)))
    return 1


def get_test(iris, only, failures):
    if only and not any(o in "get" for o in only):
        return 0
    if not shutil.which("git"):
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        repo = os.path.join(tmp, "base", "example.com", "x", "greet")
        os.makedirs(repo)
        with open(os.path.join(repo, "greet.iris"), "w") as f:
            f.write('pub str hi() => "hi from a package"\n')
        g = ["git", "-C", repo, "-c", "user.email=t@t", "-c", "user.name=t"]
        subprocess.run(["git", "init", "-q", repo], check=True)
        subprocess.run(g + ["add", "."], check=True)
        subprocess.run(g + ["commit", "-qm", "init"], check=True)
        subprocess.run(g + ["tag", "v1.0.0"], check=True)
        app = os.path.join(tmp, "app")
        os.makedirs(app)
        with open(os.path.join(app, "iris.mod"), "w") as f:
            f.write("module example.com/me/app\n")
        with open(os.path.join(app, "main.iris"), "w") as f:
            f.write('import "example.com/x/greet"\n\nvoid main() {\n    print(greet.hi())\n}\n')
        env = dict(os.environ, IRIS_PATH=os.path.join(tmp, "cache"), IRIS_GET_BASE=os.path.join(tmp, "base"))
        p = subprocess.run([iris, "get", "example.com/x/greet@v1.0.0"], cwd=app, env=env, capture_output=True, text=True)
        mod = open(os.path.join(app, "iris.mod")).read()
        r = subprocess.run([iris, "run", "."], cwd=app, env=env, capture_output=True, text=True)
        if p.returncode != 0 or "require example.com/x/greet v1.0.0" not in mod or r.stdout != "hi from a package\n":
            failures.append(("get", "get: %s %s\nmod: %s\nrun: %s %s" % (p.stdout, p.stderr, mod, r.stdout, r.stderr)))
    return 1


def doc_tests(iris, only, failures):
    files = sorted(glob.glob(os.path.join(ROOT, "docs-src/**/*.md"), recursive=True))
    # "docs" runs every page's examples; "docs-src/tour/03" only that page's
    if only and not any(o in "docs" or any(o in os.path.relpath(f, ROOT) for f in files) for o in only):
        return 0
    jobs = []
    count = 0
    with tempfile.TemporaryDirectory() as tmp:
        for md in files:
            rel = os.path.relpath(md, ROOT)
            if only and not any(o in "docs" or o in rel for o in only):
                continue
            lines = open(md).read().split("\n")
            blocks = []
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
                    want = None
                    if mode == "run":
                        # a whole program: it runs, and prints the ```output block after it
                        k = j + 1
                        while k < len(lines) and not lines[k].strip():
                            k += 1
                        if k < len(lines) and lines[k].startswith("```output"):
                            m2 = k + 1
                            while m2 < len(lines) and not lines[m2].startswith("```"):
                                m2 += 1
                            want = "".join(l + "\n" for l in lines[k + 1:m2])
                    blocks.append(("\n".join(lines[start:j]) + "\n", start, mode, want))
                i = j + 1
            # a page's examples run in order, since one may use what another made
            folder = os.path.join(tmp, str(len(jobs)))
            os.mkdir(folder)
            jobs.append(POOL.submit(doc_page, iris, os.path.join(folder, "example.iris"), rel, blocks))
            count += len(blocks)
        gather(jobs, failures)
    return count


def doc_page(iris, src, rel, blocks):
    failures = []
    for code_text, start, mode, want in blocks:
        failures += doc_test(iris, src, code_text, rel, start, mode, want)
    return failures


def doc_test(iris, src, code_text, rel, start, mode, want):
    with open(src, "w") as f:
        f.write(code_text)
    where = "docs %s:%d" % (rel, start)
    code, out, err = run(iris, ["parse", "--snippet", src])
    if code != 0:
        return [(where, err.replace(src, "%s (block at line %d)" % (rel, start)))]
    if mode == "run":
        code, out, err = run(iris, ["run", src])
        if code != 0:
            return [(where, "exit status %d\n%s" % (code, err.replace(src, rel)))]
        if want is not None and out != want:
            return [(where, diff(want, out).replace("tree", "output"))]
    return []


def output_mismatch(want, got):
    """None when got has the lines of want, where … in want matches any
    text; else what differs"""
    w, g = want.splitlines(), got.splitlines()
    for i in range(max(len(w), len(g))):
        a = w[i] if i < len(w) else "<missing>"
        b = g[i] if i < len(g) else "<missing>"
        pattern = ".*".join(re.escape(part) for part in a.split("…"))
        if i >= len(w) or i >= len(g) or not re.fullmatch(pattern, b):
            return "output differs at line %d:\n  want: %s\n  got:  %s" % (i + 1, a, b)
    return None


def service_tests(iris, only, failures, skipped, folder, script, prefix):
    """The tests in tests/FOLDER, with a server that SCRIPT starts; its
    PREFIX_ variables go to the tests. Skipped when it can't start one."""
    paths = glob.glob(os.path.join(ROOT, "tests", folder, "*.iris"))
    names = [folder + "/" + os.path.basename(p)[:-5] for p in paths]
    if not paths or (only and not any(o in n for n in names for o in only)):
        return 0
    script = os.path.join(ROOT, "tools", script)
    p = subprocess.run([script, "start"], capture_output=True, text=True)
    found = dict(re.findall(r"(%s_\w+)='([^']*)'" % prefix, p.stdout))
    dirs = [v for k, v in found.items() if k.endswith("_DIR")]
    if p.returncode != 0 or not dirs:
        skipped.append("%s (no server here)" % folder)
        return 0
    try:
        return run_tests(iris, only, failures, folder, {k: v for k, v in found.items() if not k.endswith("_DIR")})
    finally:
        subprocess.run([script, "stop", dirs[0]], capture_output=True)


def std_tests(iris, only, failures):
    jobs = []
    for path in sorted(glob.glob(os.path.join(ROOT, "std/**/*.iris"), recursive=True)):
        name = os.path.relpath(path, ROOT)
        if only and not any(o in name for o in only):
            continue
        if not re.search(r'^test "', open(path).read(), re.M):
            continue
        jobs.append(POOL.submit(std_test, iris, path, name))
    return gather(jobs, failures)


def std_test(iris, path, name):
    code, out, err = run(iris, ["test", path])
    return [(name, out + err)] if code != 0 else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iris", default=os.path.join(ROOT, "iris"))
    ap.add_argument("--update", action="store_true")
    ap.add_argument("only", nargs="*")
    args = ap.parse_args()
    args.iris = os.path.abspath(args.iris)

    failures = []
    skipped = []
    total = 0
    total += parse_tests(args.iris, args.only, args.update, failures)
    total += fmt_tests(args.iris, args.only, args.update, failures)
    total += error_tests(args.iris, args.only, failures)
    total += run_tests(args.iris, args.only, failures)
    total += project_tests(args.iris, args.only, failures)
    total += mod_init_test(args.iris, args.only, failures)
    total += get_test(args.iris, args.only, failures)
    total += doc_tests(args.iris, args.only, failures)
    total += std_tests(args.iris, args.only, failures)
    total += service_tests(args.iris, args.only, failures, skipped, "pg", "pg-cluster.sh", "IRIS_PG")
    total += service_tests(args.iris, args.only, failures, skipped, "redis", "redis-server.sh", "IRIS_REDIS")
    if not args.only or any(o in "lsp" for o in args.only):
        total += 1
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools/lsp_check.py"), args.iris],
                           capture_output=True, text=True, timeout=120)
        if p.returncode != 0:
            failures.append(("lsp", p.stdout + p.stderr))

    for name, why in failures:
        print("FAIL %s\n  %s" % (name, why.rstrip().replace("\n", "\n  ")))
    for name in skipped:
        print("SKIP %s" % name)
    print("%d tests, %d passed, %d failed" % (total, total - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
