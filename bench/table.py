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
    ("http-json", "HTTP/1.1 JSON in and out, wrk"),
    ("tcp-echo", "TCP echo, 100 connections"),
    ("http2", "HTTP/2 hello, h2load"),
]
NET = {"http-hello", "http-json", "tcp-echo", "http2"}


def fmt(v, net):
    return f"{v / 1000:,.0f}k" if net else f"{v:.2f}"


def main():
    data = json.load(open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results.json")))
    res = data["results"]
    print("| Benchmark | What it does | Iris | Iris safe | Rust | Go | Memory, MiB: Iris / Rust / Go |")
    print("|---|---|--:|--:|--:|--:|--:|")
    for name, what in ROWS:
        r = res[name]
        net = name in NET
        langs = ["iris", "iris_safe", "rust", "go"]
        vals = [r.get(l) for l in langs]
        best = max(v for v in vals if v is not None) if net else min(v for v in vals if v is not None)
        unit = "msg/s" if name == "tcp-echo" else "req/s" if net else "s"
        cells = " | ".join("" if v is None else f"**{fmt(v, net)}**" if v == best else fmt(v, net) for v in vals)
        mem = " / ".join(f"{r.get(l + '_mem', 0):.0f}" for l in ("iris", "rust", "go"))
        print(f"| {name} | {what} ({unit}) | {cells} | {mem} |")


main()
