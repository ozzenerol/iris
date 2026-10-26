#!/usr/bin/env python3
# Builds every benchmark in Iris, Rust and Go, runs them and writes results.json.
#   python3 bench/run.py [--iris PATH] [--runs N] [--langs iris,rust] [name...]
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
LIB = ["fmt-text", "utf8", "bigint", "rwlock", "heap", "sortedmap", "deque", "sort-by", "fs-walk",
       "xml-parse", "toml-parse", "zip", "tar", "template", "sha-hmac", "aes-ctr", "aes-gcm", "chacha20", "crc32c", "base32", "base64", "hex", "ecdsa", "rsa",
       "x509-parse", "yaml-parse", "wire", "ssh"]
# CPU programs that run threads get the server's cores rather than one
THREADED = {"rwlock"}
NET = ["http-hello", "https-hello", "http-json", "tcp-echo", "http2", "tcp-proxy", "tcp-proxy-bulk"]
# a benchmark that runs another one's programs under a different load
SAME_AS = {"tcp-proxy-bulk": "tcp-proxy"}
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
    names = sorted({SAME_AS.get(n, n) for n in names})
    for n in names:
        sh([iris, "build", "--mode=release-fast", "-o", f"{OUT}/iris-{n}", f"{HERE}/{n}/main.iris"])
        sh([iris, "build", "--mode=release-safe", "-o", f"{OUT}/iris_safe-{n}", f"{HERE}/{n}/main.iris"])
        sh(["go", "build", "-o", f"{OUT}/go-{n}", f"./{n}"], cwd=HERE)
    sh(["cargo", "build", "--release", "-q"], cwd=f"{HERE}/rust")
    for n in names:
        shutil.copy(f"{HERE}/rust/target/release/{n}", f"{OUT}/rust-{n}")
    sh(["go", "build", "-o", f"{OUT}/echo-load", "./tools/echo-load"], cwd=HERE)
    sh(["go", "build", "-o", f"{OUT}/echo-server", "./tools/echo-server"], cwd=HERE)
    sh(["cc", "-O2", "-o", f"{OUT}/memrun", f"{HERE}/tools/memrun.c"])
    # the TLS servers' certificate: ECDSA P-256, made for this run
    sh(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
        "-days", "2", "-subj", "/CN=127.0.0.1", "-keyout", f"{OUT}/key.pem", "-out", f"{OUT}/cert.pem"])


def timed(binary, runs, cores=None):
    """Median wall time, median peak memory (MiB) and the outputs."""
    times, mems, outs = [], [], set()
    for _ in range(runs):
        mem = f"{OUT}/mem.txt"
        t = time.perf_counter()
        out = sh(["taskset", "-c", cores or CPU_CORE, f"{OUT}/memrun", mem, binary])
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
    if name in ("http-hello", "https-hello"):
        scheme = "https" if name == "https-hello" else "http"
        out = sh(pin + ["wrk", "-t4", "-c256", f"-d{LOAD_TIME}s", f"{scheme}://127.0.0.1:9000/"])
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
    if name in ("tcp-echo", "tcp-proxy"):
        return float(sh(pin + [f"{OUT}/echo-load", "-c", "100", "-d", f"{LOAD_TIME}s"]))
    if name == "tcp-proxy-bulk":
        # 256 KiB messages: MiB/s through the proxy, each way
        rate = float(sh(pin + [f"{OUT}/echo-load", "-c", "16", "-s", str(256 << 10), "-d", f"{LOAD_TIME}s"]))
        return rate / 4


def cpu_seconds(pid):
    """User and system time of a process and all its threads."""
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def serve(binary, name):
    backend = None
    if name in ("tcp-proxy", "tcp-proxy-bulk"):
        # the same backend for every proxy, on the load's cores
        backend = subprocess.Popen(["taskset", "-c", LOAD_CORES, f"{OUT}/echo-server"])
        wait_port(9001)
    # TLS servers read cert.pem and key.pem from the folder they run in
    p = subprocess.Popen(["taskset", "-c", SERVER_CORES, binary], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, cwd=OUT)
    try:
        wait_port(9000)
        if name == "http-json":
            check = sh(["curl", "-s", "-XPOST", "-H", "Content-Type: application/json", "-d", ORDER,
                        "http://127.0.0.1:9000/order"])
            if '"total"' not in check:
                raise RuntimeError(f"{binary}: bad reply {check}")
        cpu0 = cpu_seconds(p.pid)
        rate = load(name)
        cpu = cpu_seconds(p.pid) - cpu0
        # the server's CPU time for one request or message: it shows a gain
        # even when the load generator is what limits the rate
        ops = rate * (LOAD_TIME * 1024 * 1024 * 4 / (256 << 10) if name == "tcp-proxy-bulk" else LOAD_TIME)
        return rate, peak_mib(p.pid), cpu / ops * 1e9 if ops else 0
    finally:
        p.send_signal(signal.SIGKILL)
        p.wait()
        if backend:
            backend.send_signal(signal.SIGKILL)
            backend.wait()
        time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iris", default=shutil.which("iris") or "iris")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--out", default=os.path.join(HERE, "results.json"))
    ap.add_argument("--langs", default="iris,iris_safe,rust,go", help="the languages to run, for an A/B")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    names = [n for n in CPU + NET + LIB if not a.names or n in a.names]
    build(names, a.iris)
    data = json.load(open(a.out)) if os.path.exists(a.out) else {"results": {}}
    for n in names:
        r = {}
        for lang in a.langs.split(","):
            b = f"{OUT}/{lang}-{SAME_AS.get(n, n)}"
            if n in CPU or n in LIB:
                t, mem, outs = timed(b, a.runs, SERVER_CORES if n in THREADED else None)
                r[lang], r[lang + "_out"] = round(t, 3), sorted(outs)[0].strip()
            else:
                runs = [serve(b, n) for _ in range(max(1, a.runs // 2))]
                r[lang] = round(statistics.median(x[0] for x in runs))
                mem = statistics.median(x[1] for x in runs)
                r[lang + "_cpu_ns"] = round(statistics.median(x[2] for x in runs))
            r[lang + "_mem"] = round(mem, 1)
            cpu = f"  {r[lang + '_cpu_ns']} ns CPU each" if lang + "_cpu_ns" in r else ""
            print(f"{n:15} {lang:5} {r[lang]}  {r[lang + '_mem']} MiB{cpu}", flush=True)
        if (n in CPU or n in LIB) and "rust_out" in r and len({r["iris_out"], r["rust_out"]}) != 1:
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
