# Iris

<img src="docs/assets/logo.svg" alt="" width="96">

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

Servers, clients, databases (SQLite, PostgreSQL, Redis), JSON, regular expressions, compression, and command-line tools are in the standard library. The docs in `docs/` have a Language Tour, the library, and guides.

## Speed

`bench/` has 15 programs written the same way in Iris, Rust and Go: number crunching, memory, collections, text, channels and network servers. These are the results on an AMD Ryzen 7 5700X with Linux: Iris 0.65 with clang 19, Rust 1.99 and Go 1.27. A time is the median of 5 runs, in seconds. A server runs for 10 seconds, twice, on 4 cores, with the load generator on the other 4. Memory is the peak, in MiB. The fastest is in bold.

Iris is built with `--mode=release-fast`, and with `--mode=release-safe` ("Iris safe"), which checks every index and slice as Rust does. Rust is built with `--release`, LTO and one codegen unit, and Go with `go build`.

| Benchmark | What it does | Iris | Iris safe | Rust | Go | Memory, MiB: Iris / Rust / Go |
|---|---|--:|--:|--:|--:|--:|
| fib | recursive fibonacci(42) (s) | 0.53 | 0.53 | **0.47** | 1.07 | 2 / 2 / 2 |
| sieve | sieve of Eratosthenes to 100M (s) | 0.55 | 0.55 | **0.55** | 0.57 | 97 / 97 / 98 |
| nbody | n-body, 50M steps (s) | 1.75 | 1.75 | **1.72** | 2.56 | 2 / 2 / 2 |
| spectral-norm | spectral norm, n = 5500 (s) | 1.20 | 1.20 | **0.94** | 1.20 | 2 / 2 / 2 |
| mandelbrot | mandelbrot, 4000 x 4000 (s) | **0.79** | **0.79** | 0.80 | 0.81 | 2 / 2 / 2 |
| fannkuch-redux | fannkuch-redux, n = 11 (s) | **1.29** | 1.30 | 1.41 | 1.64 | 2 / 2 / 2 |
| binary-trees | binary trees, depth 21 (s) | **6.28** | 6.37 | 8.24 | 17.45 | 130 / 258 / 275 |
| hashmap | 10M inserts and lookups, int keys (s) | **0.91** | 0.95 | 1.15 | 2.11 | 418 / 410 / 365 |
| sort | quicksort of 10M ints (s) | **0.58** | 0.58 | 0.59 | 0.60 | 78 / 78 / 78 |
| strings | build, count, split, replace (s) | 0.21 | **0.21** | 0.27 | 0.43 | 238 / 187 / 213 |
| channels | ping-pong, then 1000 tasks on one channel (s) | 0.23 | **0.22** | 10.83 | 0.57 | 6 / 3 / 4 |
| http-hello | HTTP/1.1 hello, wrk (req/s) | 437k | 436k | **437k** | 259k | 8 / 10 / 19 |
| http-json | HTTP/1.1 JSON in and out, wrk (req/s) | **428k** | 420k | 401k | 164k | 9 / 10 / 20 |
| tcp-echo | TCP echo, 100 connections (msg/s) | 445k | **449k** | 448k | 312k | 5 / 3 / 5 |
| http2 | HTTP/2 hello, h2load (req/s) | **2,727k** | 2,695k | 895k | 170k | 23 / 13 / 43 |

In fib and spectral-norm, Iris makes the same machine code as C compiled by clang 19. Rust 1.99 compiles with LLVM 23, which does better on those two loops on this CPU; on an Intel Core i7-12650H they are level. `bench/README.md` says how to run them.

## Building and installing

```
./build-and-install.sh
```

builds `iris` with every core and installs it in `/usr/local/bin` (sudo only for the copy). `--prefix DIR` installs elsewhere, `--test` runs the tests first, `--no-install` only builds `./iris`.

It needs a C11 compiler, `clang` 15 or newer (to compile programs) and Python 3 (for the tests and the docs). During work on the compiler, `make -j` builds `./iris` and `make test` runs the tests.

## Editors

### Neovim and Vim

`./build-and-install.sh` copies the Vim files (file type, syntax, indent) to `/usr/local/share/iris/editors/vim`. Add that folder to the runtime path:

```lua
vim.opt.rtp:append("/usr/local/share/iris/editors/vim")
```

With lazy.nvim, put it in `performance.rtp.paths` instead, because lazy resets the runtime path when it starts. Then start the language server for `.iris` files (Neovim 0.11 or newer):

```lua
vim.lsp.config("iris", {
  cmd = { "iris", "lsp" },
  filetypes = { "iris" },
  root_markers = { "iris.mod", ".git" },
})
vim.lsp.enable("iris")
```

### VS Code

The extension in `editors/vscode` highlights `.iris` files and the `iris` blocks in Markdown, runs `iris lsp`, and debugs programs with gdb. Build and install it:

```
make vscode
code --install-extension editors/vscode/iris.vsix
```

To debug, open a `.iris` file, set a breakpoint and press F5. The extension builds the program with `iris build` and runs it under gdb 14 or newer. A `launch.json` entry of type `iris` sets `program` (a file or a project folder), `args`, `cwd`, `env` and `mode`. See `editors/vscode/README.md`.

## Docs

The docs are in `docs-src/` as Markdown. Build and read them with:

```
python3 tools/build_docs.py --serve
```

and open http://localhost:8000. Without `--serve` it only builds `docs/`; its pages can be opened as files too, but then some browsers can't keep the theme from page to page. They have five parts:

- Language Tour: each part of the language, with examples.
- Library: every module of the standard library.
- Guides: whole programs, step by step.
- Design: how the language works underneath, for anyone working on the compiler.
- Roadmap: what gets built, in order.
