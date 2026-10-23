#!/usr/bin/env python3
# Builds every benchmark in Iris, Rust and Go, runs them and writes results.json.
#   python3 bench/run.py [--iris PATH] [--runs N] [name...]
# CPU benchmarks: median wall time and peak memory of N runs. Servers run on
# half the cores and the load generator (wrk, h2load, echo-load) on the rest; their
# memory is the server's peak. Iris is built twice: release-fast, and
# release-safe, which keeps the bounds checks as Rust does.
import argparse, json, os, platform, re, shutil, signal, socket, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
CPU = ["fib", "sieve", "nbody", "spectral-norm", "mandelbrot", "binary-trees", "fannkuch-redux",
       "hashmap", "sort", "strings", "channels"]
# the standard library against Go's and the best-known Rust crates
LIB = ["fmt-text", "utf8", "bigint", "rwlock"]
NET = ["http-hello", "http-json", "tcp-echo", "http2"]
# the cores this process may use (a container may get any of the host's):
# the first half runs servers, the rest the load, and one CPU program at a time
_CORES = sorted(os.sched_getaffinity(0))
SERVER_CORES = ",".join(map(str, _CORES[:len(_CORES) // 2]))
LOAD_CORES = ",".join(map(str, _CORES[len(_CORES) // 2:]))
CPU_CORE = str(_CORES[min(2, len(_CORES) - 1)])
LOAD_TIME = 10
ORDER = json.dumps({"id": 7, "customer": "Ana Pop", "items": [
    {"name": "cable", "qty": 2, "price": 4.5}, {"name": "router", "qty": 1, "price": 89.99},
    {"name": "switch", "qty": 3, "price": 24.0}], "tags": ["fast", "gift"]})


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def build(names, iris):
    os.makedirs(OUT, exist_ok=True)
    for n in names:
        sh([iris, "build", "--mode=release-fast", "-o", f"{OUT}/iris-{n}", f"{HERE}/{n}/main.iris"])
        sh([iris, "build", "--mode=release-safe", "-o", f"{OUT}/iris_safe-{n}", f"{HERE}/{n}/main.iris"])
        sh(["go", "build", "-o", f"{OUT}/go-{n}", f"{HERE}/{n}/main.go"])
    sh(["cargo", "build", "--release", "-q"], cwd=f"{HERE}/rust")
    for n in names:
        shutil.copy(f"{HERE}/rust/target/release/{n}", f"{OUT}/rust-{n}")
    sh(["go", "build", "-o", f"{OUT}/echo-load", f"{HERE}/tools/echo-load/main.go"])
    sh(["cc", "-O2", "-o", f"{OUT}/memrun", f"{HERE}/tools/memrun.c"])


def timed(binary, runs):
    """Median wall time, median peak memory (MiB) and the outputs."""
    times, mems, outs = [], [], set()
    for _ in range(runs):
        mem = f"{OUT}/mem.txt"
        t = time.perf_counter()
        out = sh(["taskset", "-c", CPU_CORE, f"{OUT}/memrun", mem, binary])
        times.append(time.perf_counter() - t)
        mems.append(int(open(mem).read()) / 1024)
        outs.add(out)
    return statistics.median(times), statistics.median(mems), outs


def peak_mib(pid):
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("VmHWM:"):
            return int(line.split()[1]) / 1024
    return 0


def wait_port(port):
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 0.1).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def load(name):
    pin = ["taskset", "-c", LOAD_CORES]
    if name == "http-hello":
        out = sh(pin + ["wrk", "-t4", "-c256", f"-d{LOAD_TIME}s", "http://127.0.0.1:9000/"])
        return float(re.search(r"Requests/sec:\s+([\d.]+)", out).group(1))
    if name == "http-json":
        lua = f"{OUT}/post.lua"
        with open(lua, "w") as f:
            f.write('wrk.method = "POST"\nwrk.headers["Content-Type"] = "application/json"\n'
                    f"wrk.body = {json.dumps(ORDER)}\n")
        out = sh(pin + ["wrk", "-t4", "-c256", f"-d{LOAD_TIME}s", "-s", lua, "http://127.0.0.1:9000/order"])
        return float(re.search(r"Requests/sec:\s+([\d.]+)", out).group(1))
    if name == "http2":
        out = sh(pin + ["h2load", "-t4", "-c128", "-m16", "-D", str(LOAD_TIME), "http://127.0.0.1:9000/"])
        if not re.search(r"(\d+) succeeded", out) or re.search(r" 0 succeeded", out):
            raise RuntimeError("h2load: no request succeeded\n" + out)
        return float(re.search(r"finished in [\d.]+s, ([\d.]+) req/s", out).group(1))
    if name == "tcp-echo":
        return float(sh(pin + [f"{OUT}/echo-load", "-c", "100", "-d", f"{LOAD_TIME}s"]))


def serve(binary, name):
    p = subprocess.Popen(["taskset", "-c", SERVER_CORES, binary], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    try:
        wait_port(9000)
        if name == "http-json":
            check = sh(["curl", "-s", "-XPOST", "-H", "Content-Type: application/json", "-d", ORDER,
                        "http://127.0.0.1:9000/order"])
            if '"total"' not in check:
                raise RuntimeError(f"{binary}: bad reply {check}")
        rate = load(name)
        return rate, peak_mib(p.pid)
    finally:
        p.send_signal(signal.SIGKILL)
        p.wait()
        time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iris", default=shutil.which("iris") or "iris")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--out", default=os.path.join(HERE, "results.json"))
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    names = [n for n in CPU + NET + LIB if not a.names or n in a.names]
    build(names, a.iris)
    data = json.load(open(a.out)) if os.path.exists(a.out) else {"results": {}}
    for n in names:
        r = {}
        for lang in ("iris", "iris_safe", "rust", "go"):
            b = f"{OUT}/{lang}-{n}"
            if n in CPU or n in LIB:
                t, mem, outs = timed(b, a.runs)
                r[lang], r[lang + "_out"] = round(t, 3), sorted(outs)[0].strip()
            else:
                runs = [serve(b, n) for _ in range(max(1, a.runs // 2))]
                r[lang] = round(statistics.median(x[0] for x in runs))
                mem = statistics.median(x[1] for x in runs)
            r[lang + "_mem"] = round(mem, 1)
            print(f"{n:15} {lang:5} {r[lang]}  {r[lang + '_mem']} MiB", flush=True)
        if (n in CPU or n in LIB) and len({r["iris_out"], r["rust_out"]}) != 1:
            print(f"warning: {n}: iris and rust print different results", flush=True)
        data["results"][n] = r
    cpu = re.search(r"model name\s*:\s*(.*)", open("/proc/cpuinfo").read())
    data["setup"] = {
        "machine": f"{platform.node()}, {cpu.group(1) if cpu else platform.machine()}, {os.cpu_count()} threads",
        "iris": sh([a.iris, "--version"]).strip(),
        "rust": sh(["rustc", "--version"]).strip(),
        "go": sh(["go", "version"]).strip(),
        "date": time.strftime("%Y-%m-%d %H:%M"),
    }
    with open(a.out, "w") as f:
        json.dump(data, f, indent=1)


main()
