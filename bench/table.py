#!/usr/bin/env python3
# Prints results.json as the Markdown table in the README.
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROWS = [
    ("fib", "recursive fibonacci(42)"),
    ("sieve", "sieve of Eratosthenes to 100M"),
    ("nbody", "n-body, 50M steps"),
    ("spectral-norm", "spectral norm, n = 5500"),
    ("mandelbrot", "mandelbrot, 4000 x 4000"),
    ("fannkuch-redux", "fannkuch-redux, n = 11"),
    ("binary-trees", "binary trees, depth 21"),
    ("hashmap", "10M inserts and lookups, int keys"),
    ("sort", "quicksort of 10M ints"),
    ("strings", "build, count, split, replace"),
    ("channels", "ping-pong, then 1000 tasks on one channel"),
    ("http-hello", "HTTP/1.1 hello, wrk"),
    ("https-hello", "HTTP/1.1 hello over TLS, wrk"),
    ("http-json", "HTTP/1.1 JSON in and out, wrk"),
    ("tcp-echo", "TCP echo, 100 connections"),
    ("http2", "HTTP/2 hello, h2load"),
    ("grpc", "gRPC unary hello, h2load"),
    ("tcp-proxy", "TCP proxy, 64-byte messages, 100 connections"),
    ("tcp-proxy-bulk", "TCP proxy, 256 KiB messages, 16 connections"),
]
NET = {"http-hello", "https-hello", "http-json", "tcp-echo", "http2", "tcp-proxy", "tcp-proxy-bulk", "grpc"}
UNITS = {"tcp-echo": "msg/s", "tcp-proxy": "msg/s", "tcp-proxy-bulk": "MiB/s each way"}
# the standard library against Go's and the best-known Rust crates
LIB_ROWS = [
    ("fmt-text", "2M lines with widths, hex and precision", "`write!`", "`fmt.Fprintf`"),
    ("utf8", "count, walk and check 16 MB of UTF-8", "`chars`, `from_utf8`", "`unicode/utf8`"),
    ("bigint", "20000!, its digits, 300 modular powers", "num-bigint", "`math/big`"),
    ("rwlock", "4 threads reading and writing", "`RwLock`", "`sync.RWMutex`"),
    ("heap", "2M pushes and pops", "`BinaryHeap`", "`container/heap`"),
    ("sortedmap", "1M inserts, lookups and range scans", "`BTreeMap`", "a sorted slice"),
    ("deque", "27M pushes and pops at both ends", "`VecDeque`", "a ring buffer"),
    ("sort-by", "5M structs by a key", "`sort_unstable_by_key`", "`slices.SortFunc`"),
    ("fs-walk", "make, walk and remove 50k files", "`std::fs`", "`filepath.WalkDir`"),
    ("xml-parse", "30 MB of XML as events", "quick-xml", "`encoding/xml`"),
    ("toml-parse", "a 2 MB TOML file, 20 times", "toml", "BurntSushi/toml"),
    ("zip", "2000 files deflated and read back", "zip", "`archive/zip`"),
    ("tar", "2000 files into a tar and back", "tar", "`archive/tar`"),
    ("template", "a 1000-row HTML page, 2000 times", "minijinja", "`html/template`"),
    ("sha-hmac", "SHA-384 of 256 MiB, 1M HMAC-SHA384", "sha2, hmac", "`crypto/sha512`, `crypto/hmac`"),
    ("aes-ctr", "256 MiB with AES-256 in counter mode", "aes, ctr", "`crypto/cipher`"),
    ("aes-gcm", "64 KiB sealed and opened with AES-256-GCM, 2000 times", "ring", "`crypto/cipher`"),
    ("chacha20", "256 MiB with ChaCha20", "chacha20", "`x/crypto/chacha20`"),
    ("crc32c", "CRC32C of 1 GiB", "crc32c", "`hash/crc32`"),
    ("base32", "64 KiB to base32 and back, 2000 times", "data-encoding", "`encoding/base32`"),
    ("base64", "64 KiB to base64 and back, 2000 times", "data-encoding", "`encoding/base64`"),
    ("hex", "64 KiB to hex and back, 2000 times", "data-encoding", "`encoding/hex`"),
    ("ecdsa", "4000 P-256 signatures, each checked", "ring", "`crypto/ecdsa`"),
    ("rsa", "500 RSA-2048 signatures, each checked 20 times", "ring", "`crypto/rsa`"),
    ("x509-parse", "a certificate parsed 200000 times", "x509-parser", "`crypto/x509`"),
    ("yaml-parse", "a 6.7 MB YAML list, 3 times", "serde_yaml", "`gopkg.in/yaml.v3`"),
    ("wire", "100,000 binary messages read and written, 30 times", "by hand, `from_be_bytes`", "`encoding/binary`"),
    ("ssh", "100 SSH logins with a command each, then 512 MiB through a channel", "russh", "`x/crypto/ssh`"),
    ("protobuf", "100,000 orders read and written as protobuf, 20 times", "prost", "by hand, `protowire`"),
    ("msgpack", "100,000 orders read and written as MessagePack, 20 times", "rmp-serde", "vmihailenco/msgpack"),
    ("cbor", "100,000 orders read and written as CBOR, 20 times", "ciborium", "fxamacker/cbor"),
    ("metrics", "10M requests counted with labels and a histogram, 2,000 series written 200 times", "prometheus", "client_golang"),
    ("tracing", "1M spans with traceparent, written as OTLP JSON", "opentelemetry-proto + serde_json", "otlp + protojson"),
]


def fmt(v, net, name=""):
    if name == "tcp-proxy-bulk":
        return f"{v:,.0f}"
    return f"{v / 1000:,.0f}k" if net else f"{v:.2f}"


def lib_table(res):
    print("| Benchmark | What it does | Iris | Rust | Go | Rust uses | Go uses |")
    print("|---|---|--:|--:|--:|---|---|")
    for name, what, rust, go in LIB_ROWS:
        r = res.get(name)
        if not r:
            continue
        vals = [r["iris"], r["rust"], r["go"]]
        best = min(vals)
        cells = " | ".join(f"**{v:.3f}**" if v == best else f"{v:.3f}" for v in vals)
        print(f"| {name} | {what} | {cells} | {rust} | {go} |")


def cpu_table(res):
    """The servers' CPU time per request or message: it shows how much work
    a server does even when the load generator is what limits the rate."""
    print("| Benchmark | Iris | Iris safe | Rust | Go |")
    print("|---|--:|--:|--:|--:|")
    for name, what in ROWS:
        r = res.get(name)
        if name not in NET or not r or "iris_cpu_ns" not in r:
            continue
        vals = [r.get(l + "_cpu_ns") for l in ("iris", "iris_safe", "rust", "go")]
        best = min(v for v in vals if v)
        cells = " | ".join(f"**{v:,}**" if v == best else f"{v:,}" for v in vals)
        print(f"| {name} | {cells} |")


def main():
    args = [a for a in sys.argv[1:] if a not in ("--lib", "--cpu")]
    data = json.load(open(args[0] if args else os.path.join(HERE, "results.json")))
    res = data["results"]
    if "--lib" in sys.argv:
        lib_table(res)
        return
    if "--cpu" in sys.argv:
        cpu_table(res)
        return
    print("| Benchmark | What it does | Iris | Iris safe | Rust | Go | Memory, MiB: Iris / Rust / Go |")
    print("|---|---|--:|--:|--:|--:|--:|")
    for name, what in ROWS:
        r = res.get(name)
        if not r:
            continue
        net = name in NET
        langs = ["iris", "iris_safe", "rust", "go"]
        vals = [r.get(l) for l in langs]
        best = max(v for v in vals if v is not None) if net else min(v for v in vals if v is not None)
        unit = UNITS.get(name, "req/s" if net else "s")
        cells = " | ".join("" if v is None else f"**{fmt(v, net, name)}**" if v == best else fmt(v, net, name)
                           for v in vals)
        mem = " / ".join(f"{r.get(l + '_mem', 0):.0f}" for l in ("iris", "rust", "go"))
        print(f"| {name} | {what} ({unit}) | {cells} | {mem} |")


main()
