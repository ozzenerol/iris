# Benchmarks

Each folder holds one program written three times, the same way: `main.iris`, `main.rs` and `main.go`. The Rust programs build together from `rust/Cargo.toml`.

```
python3 bench/run.py [--runs 5] [name...]   # build, run, write results.json
bench/remote.sh [name...]                   # the same on the LXC, results copied back
python3 bench/table.py                      # results.json as the table in the README
```

`run.py` builds Iris with `--mode=release-fast` and `--mode=release-safe`, Rust with `cargo build --release` (LTO, one codegen unit) and Go with `go build`, in the module `bench/go.mod`, which holds what the Go programs need from outside the standard library. A CPU program runs 5 times on one core, and the median time and peak memory are kept; `tools/memrun.c` measures the memory. A server runs on half of the cores the machine gives and its load generator on the other half, for 10 seconds, twice: `wrk` for HTTP/1.1, `h2load` for HTTP/2 and `tools/echo-load` for TCP echo. The memory of a server is its peak during the load. Its CPU time per request or message, user and system, is kept too (`_cpu_ns` in `results.json`). When the load generator is what limits the rate, that number still shows a change in the server.

It needs `iris`, `cargo`, `go`, `wrk`, `h2load`, `taskset` and a C compiler. `results-before.json` is the first run, before the work in 0.65.
