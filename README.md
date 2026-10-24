# Iris

<img src="docs/assets/logo.svg" alt="" width="96">

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

The standard library covers HTTP and TCP servers and clients, SQLite, PostgreSQL and Redis, JSON, regular expressions, compression and command-line tools. The docs in `docs/` have a Language Tour, a Library Tour and the library reference.

## Speed

`bench/` has 15 programs written the same way in Iris, Rust and Go: number crunching, memory, collections, text, channels and network servers.

- Machine: AMD Ryzen 7 5700X, Linux. Iris 0.66.0 with clang 19, Rust 1.99, Go 1.27.
- Times are the median of 5 runs, in seconds.
- Servers run for 10 seconds, twice, on 4 cores, with the load generator on the other 4.
- Memory is the peak, in MiB. The fastest result is in bold.
- Iris builds with `--mode=release-fast`, and with `--mode=release-safe` ("Iris safe"), which checks every index and slice as Rust does.
- Rust builds with `--release`, LTO and one codegen unit. Go builds with `go build`.

| Benchmark | What it does | Iris | Iris safe | Rust | Go | Memory, MiB: Iris / Rust / Go |
|---|---|--:|--:|--:|--:|--:|
| fib | recursive fibonacci(42) (s) | 0.53 | 0.53 | **0.47** | 1.08 | 2 / 2 / 2 |
| sieve | sieve of Eratosthenes to 100M (s) | 0.54 | 0.54 | **0.54** | 0.56 | 97 / 97 / 97 |
| nbody | n-body, 50M steps (s) | 1.75 | 1.75 | **1.72** | 2.56 | 2 / 2 / 2 |
| spectral-norm | spectral norm, n = 5500 (s) | 1.20 | 1.20 | **0.94** | 1.20 | 2 / 2 / 2 |
| mandelbrot | mandelbrot, 4000 x 4000 (s) | **0.79** | **0.79** | 0.80 | 0.81 | 2 / 2 / 2 |
| fannkuch-redux | fannkuch-redux, n = 11 (s) | **1.29** | 1.30 | 1.41 | 1.64 | 2 / 2 / 2 |
| binary-trees | binary trees, depth 21 (s) | 6.35 | **6.33** | 8.28 | 17.39 | 130 / 258 / 276 |
| hashmap | 10M inserts and lookups, int keys (s) | **0.90** | 0.95 | 1.14 | 2.12 | 418 / 410 / 366 |
| sort | quicksort of 10M ints (s) | **0.57** | 0.58 | 0.59 | 0.60 | 78 / 78 / 78 |
| strings | build, count, split, replace (s) | **0.18** | 0.18 | 0.26 | 0.46 | 186 / 187 / 230 |
| channels | ping-pong, then 1000 tasks on one channel (s) | 0.22 | **0.22** | 10.67 | 0.57 | 6 / 3 / 4 |
| http-hello | HTTP/1.1 hello, wrk (req/s) | 384k | **389k** | 371k | 228k | 6 / 11 / 19 |
| http-json | HTTP/1.1 JSON in and out, wrk (req/s) | **366k** | 364k | 337k | 143k | 6 / 11 / 21 |
| tcp-echo | TCP echo, 100 connections (msg/s) | 367k | **380k** | 363k | 292k | 4 / 3 / 5 |
| http2 | HTTP/2 hello, h2load (req/s) | **2,044k** | 2,037k | 744k | 149k | 22 / 14 / 43 |
| tcp-proxy | TCP proxy, 64-byte messages, 100 connections (msg/s) | 190k | **190k** | 187k | 157k | 4 / 4 / 6 |
| tcp-proxy-bulk | TCP proxy, 256 KiB messages, 16 connections (MiB/s each way) | 7,645 | 7,911 | 2,098 | **8,352** | 3 / 3 / 5 |

In fib and spectral-norm, Iris produces the same machine code as C compiled by clang 19. Rust 1.99 uses LLVM 23, which is faster on those two loops on this CPU. On an Intel Core i7-12650H they are level. `bench/README.md` explains how to run the benchmarks.

`bench/` also has 14 library benchmarks. Each does the same work with Iris's standard library, the best-known Rust crate, and Go's standard library or its usual package. Times are in seconds.

| Benchmark | What it does | Iris | Rust | Go | Rust uses | Go uses |
|---|---|--:|--:|--:|---|---|
| fmt-text | 2M lines with widths, hex and precision | **0.242** | 0.430 | 0.728 | `write!` | `fmt.Fprintf` |
| utf8 | count, walk and check 16 MB of UTF-8 | **0.050** | 0.051 | 0.114 | `chars`, `from_utf8` | `unicode/utf8` |
| bigint | 20000!, its digits, 300 modular powers | 0.062 | 0.071 | **0.037** | num-bigint | `math/big` |
| rwlock | 4 threads reading and writing | 0.052 | 0.110 | **0.020** | `RwLock` | `sync.RWMutex` |
| heap | 2M pushes and pops | **0.178** | 0.223 | 0.481 | `BinaryHeap` | `container/heap` |
| sortedmap | 1M inserts, lookups and range scans | **0.261** | 0.281 | 0.376 | `BTreeMap` | a sorted slice |
| deque | 27M pushes and pops at both ends | **0.091** | 0.096 | 0.176 | `VecDeque` | a ring buffer |
| sort-by | 5M structs by a key | **0.207** | 0.222 | 0.579 | `sort_unstable_by_key` | `slices.SortFunc` |
| fs-walk | make, walk and remove 50k files | 0.765 | **0.762** | 0.998 | `std::fs` | `filepath.WalkDir` |
| xml-parse | 30 MB of XML as events | **0.099** | 0.117 | 0.742 | quick-xml | `encoding/xml` |
| toml-parse | a 2 MB TOML file, 20 times | **0.834** | 1.205 | 3.956 | toml | BurntSushi/toml |
| zip | 2000 files deflated and read back | 0.257 | 0.267 | **0.202** | zip | `archive/zip` |
| tar | 2000 files into a tar and back | **0.031** | 0.036 | 0.141 | tar | `archive/tar` |
| template | a 1000-row HTML page, 2000 times | **0.469** | 0.621 | 4.488 | minijinja | `html/template` |

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
