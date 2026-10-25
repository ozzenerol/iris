# Iris

<img src="docs/assets/logo.svg" alt="" width="96">

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

The standard library covers HTTP and TCP servers and clients, SQLite, PostgreSQL and Redis, JSON, regular expressions, compression and command-line tools. The docs in `docs/` have a Language Tour, a Library Tour and the library reference.

## Speed

`bench/` has 18 programs written the same way in Iris, Rust and Go: number crunching, memory, collections, text, channels and network servers.

- Machine: AMD Ryzen 7 5700X, Linux. Iris 0.71.0 with clang 19, Rust 1.99, Go 1.27.
- Times are the median of 5 runs, in seconds.
- Servers run for 10 seconds, twice, on 4 cores, with the load generator on the other 4.
- Memory is the peak, in MiB. The fastest result is in bold.
- Iris builds with `--mode=release-fast`, and with `--mode=release-safe` ("Iris safe"), which checks every index and slice as Rust does.
- Rust builds with `--release`, LTO and one codegen unit. Go builds with `go build`.

| Benchmark | What it does | Iris | Iris safe | Rust | Go | Memory, MiB: Iris / Rust / Go |
|---|---|--:|--:|--:|--:|--:|
| fib | recursive fibonacci(42) (s) | 0.53 | 0.53 | **0.47** | 1.06 | 2 / 2 / 2 |
| sieve | sieve of Eratosthenes to 100M (s) | 0.54 | 0.54 | **0.54** | 0.57 | 97 / 97 / 97 |
| nbody | n-body, 50M steps (s) | 1.75 | 1.75 | **1.72** | 2.56 | 2 / 2 / 2 |
| spectral-norm | spectral norm, n = 5500 (s) | 1.21 | 1.21 | **0.94** | 1.20 | 2 / 2 / 2 |
| mandelbrot | mandelbrot, 4000 x 4000 (s) | **0.79** | 0.79 | 0.80 | 0.81 | 2 / 2 / 2 |
| fannkuch-redux | fannkuch-redux, n = 11 (s) | **1.29** | 1.30 | 1.41 | 1.64 | 2 / 2 / 2 |
| binary-trees | binary trees, depth 21 (s) | 6.40 | **6.37** | 8.28 | 17.34 | 130 / 258 / 276 |
| hashmap | 10M inserts and lookups, int keys (s) | **0.91** | 0.95 | 1.15 | 2.12 | 418 / 410 / 366 |
| sort | quicksort of 10M ints (s) | **0.58** | 0.58 | 0.59 | 0.60 | 78 / 78 / 78 |
| strings | build, count, split, replace (s) | **0.18** | 0.19 | 0.27 | 0.49 | 187 / 187 / 237 |
| channels | ping-pong, then 1000 tasks on one channel (s) | **0.21** | **0.21** | 11.09 | 0.57 | 6 / 3 / 4 |
| http-hello | HTTP/1.1 hello, wrk (req/s) | **392k** | 383k | 372k | 226k | 6 / 10 / 19 |
| https-hello | HTTP/1.1 hello over TLS, wrk (req/s) | 287k | **299k** | 278k | 202k | 26 / 15 / 25 |
| http-json | HTTP/1.1 JSON in and out, wrk (req/s) | **413k** | 410k | 332k | 143k | 6 / 10 / 20 |
| tcp-echo | TCP echo, 100 connections (msg/s) | 375k | **380k** | 365k | 288k | 4 / 3 / 5 |
| http2 | HTTP/2 hello, h2load (req/s) | **2,348k** | 2,307k | 733k | 148k | 8 / 13 / 43 |
| tcp-proxy | TCP proxy, 64-byte messages, 100 connections (msg/s) | 189k | **190k** | 185k | 157k | 4 / 4 / 6 |
| tcp-proxy-bulk | TCP proxy, 256 KiB messages, 16 connections (MiB/s each way) | **8,688** | 8,684 | 2,062 | 8,309 | 3 / 3 / 5 |

In fib and spectral-norm, Iris produces the same machine code as C compiled by clang 19. Rust 1.99 uses LLVM 23, which is faster on those two loops on this CPU. On an Intel Core i7-12650H they are level. `bench/README.md` explains how to run the benchmarks.

The servers do less work than the rates show, since the load generator limits them too. This is the CPU time a server uses for one request or message, in nanoseconds, user and system:

| Benchmark | Iris | Iris safe | Rust | Go |
|---|--:|--:|--:|--:|
| http-hello | **7,460** | 7,584 | 9,426 | 17,132 |
| https-hello | 10,311 | **10,109** | 11,786 | 19,246 |
| http-json | 7,253 | **7,252** | 11,561 | 27,056 |
| tcp-echo | **7,894** | 7,948 | 8,257 | 9,554 |
| http2 | **1,295** | 1,299 | 5,437 | 26,212 |
| tcp-proxy | 16,160 | **16,032** | 16,897 | 19,527 |
| tcp-proxy-bulk | 19,340 | **18,521** | 120,265 | 22,640 |

`bench/` also has 14 library benchmarks. Each does the same work with Iris's standard library, the best-known Rust crate, and Go's standard library or its usual package. Times are in seconds.

| Benchmark | What it does | Iris | Rust | Go | Rust uses | Go uses |
|---|---|--:|--:|--:|---|---|
| fmt-text | 2M lines with widths, hex and precision | **0.236** | 0.433 | 0.721 | `write!` | `fmt.Fprintf` |
| utf8 | count, walk and check 16 MB of UTF-8 | **0.050** | 0.052 | 0.114 | `chars`, `from_utf8` | `unicode/utf8` |
| bigint | 20000!, its digits, 300 modular powers | 0.043 | 0.071 | **0.037** | num-bigint | `math/big` |
| rwlock | 4 threads reading and writing | 0.039 | 0.108 | **0.019** | `RwLock` | `sync.RWMutex` |
| heap | 2M pushes and pops | **0.179** | 0.223 | 0.486 | `BinaryHeap` | `container/heap` |
| sortedmap | 1M inserts, lookups and range scans | **0.253** | 0.276 | 0.376 | `BTreeMap` | a sorted slice |
| deque | 27M pushes and pops at both ends | **0.089** | 0.096 | 0.178 | `VecDeque` | a ring buffer |
| sort-by | 5M structs by a key | **0.209** | 0.225 | 0.579 | `sort_unstable_by_key` | `slices.SortFunc` |
| fs-walk | make, walk and remove 50k files | 0.769 | **0.765** | 1.008 | `std::fs` | `filepath.WalkDir` |
| xml-parse | 30 MB of XML as events | **0.094** | 0.121 | 0.730 | quick-xml | `encoding/xml` |
| toml-parse | a 2 MB TOML file, 20 times | **0.813** | 1.219 | 3.937 | toml | BurntSushi/toml |
| zip | 2000 files deflated and read back | **0.166** | 0.267 | 0.203 | zip | `archive/zip` |
| tar | 2000 files into a tar and back | **0.031** | 0.037 | 0.144 | tar | `archive/tar` |
| template | a 1000-row HTML page, 2000 times | **0.433** | 0.693 | 4.494 | minijinja | `html/template` |

In zip each writes at its default level: Iris's archive is 2.8 MB, Go's 3.3 MB.

## Building and installing

```
./build-and-install.sh
```

This builds `iris` on every core and installs it in `/usr/local/bin`, using sudo only for the copy.

- `--prefix DIR` installs elsewhere.
- `--test` runs the tests first.
- `--no-install` only builds `./iris`.

It needs a C11 compiler, `clang` 15 or newer to compile programs, and Python 3 for the tests and docs. While working on the compiler, `make -j` builds `./iris` and `make test` runs the tests.

## Editors

### Neovim and Vim

`./build-and-install.sh` copies the Vim files (file type, syntax, indent) to `/usr/local/share/iris/editors/vim`. Add that folder to the runtime path:

```lua
vim.opt.rtp:append("/usr/local/share/iris/editors/vim")
```

With lazy.nvim, add it to `performance.rtp.paths` instead, because lazy resets the runtime path at startup.

Then start the language server for `.iris` files (Neovim 0.11 or newer):

```lua
vim.lsp.config("iris", {
  cmd = { "iris", "lsp" },
  filetypes = { "iris" },
  root_markers = { "iris.mod", ".git" },
})

vim.lsp.enable("iris")
```

### VS Code

The extension in `editors/vscode` highlights `.iris` files and `iris` blocks in Markdown, runs `iris lsp`, and debugs programs with gdb. Build and install it:

```
make vscode
code --install-extension editors/vscode/iris.vsix
```

To debug, open a `.iris` file, set a breakpoint and press F5. The extension builds the program with `iris build` and runs it under gdb 14 or newer. A `launch.json` entry of type `iris` takes `program` (a file or a project folder), `args`, `cwd`, `env` and `mode`. See `editors/vscode/README.md`.

## Docs

The docs are Markdown files in `docs-src/`. Build and serve them:

```
python3 tools/build_docs.py --serve
```

Then open http://localhost:8000. Without `--serve` it only builds `docs/`. The pages also open as files, but some browsers then forget the theme between pages.

The docs have four parts:

- Language Tour: the whole language, with runnable examples.
- Library Tour: the standard library, one task per chapter.
- Library: every module, type and function.
- Design: the compiler's architecture, for people working on it.
