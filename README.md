# Iris

<img src="docs/assets/logo.svg" alt="" width="96">

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

The standard library covers HTTP/1.1 and HTTP/2 servers and clients, TLS with certificate checking, TCP and UDP, Unix sockets, TUN devices and the kernel's routes, SQLite, PostgreSQL and Redis, JSON, YAML, TOML and XML, binary protocol messages read into structs, regular expressions, compression, cryptography with ECDSA and RSA, and the system side of a server: signals, pseudo-terminals, dropping privileges and a seccomp sandbox. The docs in `docs/` have a Language Tour, a Library Tour and the library reference.

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
| fib | recursive fibonacci(42) (s) | 0.53 | 0.53 | **0.47** | 1.07 | 2 / 2 / 2 |
| sieve | sieve of Eratosthenes to 100M (s) | 0.54 | 0.54 | **0.54** | 0.56 | 97 / 97 / 97 |
| nbody | n-body, 50M steps (s) | 1.75 | 1.75 | **1.72** | 2.56 | 2 / 2 / 2 |
| spectral-norm | spectral norm, n = 5500 (s) | 1.21 | 1.21 | **0.95** | 1.20 | 2 / 2 / 2 |
| mandelbrot | mandelbrot, 4000 x 4000 (s) | **0.79** | **0.79** | 0.80 | 0.81 | 2 / 2 / 2 |
| fannkuch-redux | fannkuch-redux, n = 11 (s) | **1.29** | 1.30 | 1.41 | 1.64 | 2 / 2 / 2 |
| binary-trees | binary trees, depth 21 (s) | **6.34** | 6.40 | 8.32 | 17.41 | 130 / 258 / 278 |
| hashmap | 10M inserts and lookups, int keys (s) | **0.91** | 0.95 | 1.15 | 2.12 | 418 / 410 / 363 |
| sort | quicksort of 10M ints (s) | **0.58** | 0.58 | 0.59 | 0.60 | 78 / 78 / 78 |
| strings | build, count, split, replace (s) | **0.18** | 0.19 | 0.27 | 0.49 | 187 / 187 / 238 |
| channels | ping-pong, then 1000 tasks on one channel (s) | **0.22** | 0.22 | 10.99 | 0.57 | 6 / 3 / 4 |
| http-hello | HTTP/1.1 hello, wrk (req/s) | **393k** | 392k | 378k | 227k | 7 / 10 / 19 |
| https-hello | HTTP/1.1 hello over TLS, wrk (req/s) | **295k** | 293k | 278k | 202k | 27 / 15 / 25 |
| http-json | HTTP/1.1 JSON in and out, wrk (req/s) | 411k | **411k** | 337k | 143k | 7 / 10 / 20 |
| tcp-echo | TCP echo, 100 connections (msg/s) | **388k** | 378k | 359k | 286k | 4 / 3 / 5 |
| http2 | HTTP/2 hello, h2load (req/s) | **2,193k** | 2,152k | 738k | 150k | 9 / 13 / 43 |
| tcp-proxy | TCP proxy, 64-byte messages, 100 connections (msg/s) | **190k** | 190k | 185k | 156k | 4 / 4 / 6 |
| tcp-proxy-bulk | TCP proxy, 256 KiB messages, 16 connections (MiB/s each way) | **8,724** | 8,657 | 2,078 | 8,293 | 3 / 3 / 5 |

In fib and spectral-norm, Iris produces the same machine code as C compiled by clang 19. Rust 1.99 uses LLVM 23, which is faster on those two loops on this CPU. On an Intel Core i7-12650H they are level. `bench/README.md` explains how to run the benchmarks.

The servers do less work than the rates show, since the load generator limits them too. This is the CPU time a server uses for one request or message, in nanoseconds, user and system:

| Benchmark | Iris | Iris safe | Rust | Go |
|---|--:|--:|--:|--:|
| http-hello | 7,591 | **7,480** | 9,471 | 17,083 |
| https-hello | **10,112** | 10,275 | 11,797 | 19,152 |
| http-json | **7,289** | 7,303 | 11,553 | 27,046 |
| tcp-echo | **7,902** | 8,028 | 8,130 | 9,507 |
| http2 | **1,372** | 1,414 | 5,402 | 26,216 |
| tcp-proxy | **15,965** | 16,108 | 16,908 | 19,480 |
| tcp-proxy-bulk | **18,849** | 19,151 | 120,226 | 22,635 |

`bench/` also has 27 library benchmarks. Each does the same work with Iris's standard library, the best-known Rust crate, and Go's standard library or its usual package. Times are in seconds.

| Benchmark | What it does | Iris | Rust | Go | Rust uses | Go uses |
|---|---|--:|--:|--:|---|---|
| fmt-text | 2M lines with widths, hex and precision | **0.236** | 0.434 | 0.723 | `write!` | `fmt.Fprintf` |
| utf8 | count, walk and check 16 MB of UTF-8 | **0.051** | 0.052 | 0.114 | `chars`, `from_utf8` | `unicode/utf8` |
| bigint | 20000!, its digits, 300 modular powers | 0.043 | 0.071 | **0.038** | num-bigint | `math/big` |
| rwlock | 4 threads reading and writing | 0.039 | 0.107 | **0.020** | `RwLock` | `sync.RWMutex` |
| heap | 2M pushes and pops | **0.178** | 0.224 | 0.488 | `BinaryHeap` | `container/heap` |
| sortedmap | 1M inserts, lookups and range scans | **0.268** | 0.277 | 0.375 | `BTreeMap` | a sorted slice |
| deque | 27M pushes and pops at both ends | **0.090** | 0.096 | 0.178 | `VecDeque` | a ring buffer |
| sort-by | 5M structs by a key | **0.208** | 0.223 | 0.579 | `sort_unstable_by_key` | `slices.SortFunc` |
| fs-walk | make, walk and remove 50k files | 0.772 | **0.767** | 1.003 | `std::fs` | `filepath.WalkDir` |
| xml-parse | 30 MB of XML as events | **0.095** | 0.119 | 0.734 | quick-xml | `encoding/xml` |
| toml-parse | a 2 MB TOML file, 20 times | **0.814** | 1.212 | 3.855 | toml | BurntSushi/toml |
| zip | 2000 files deflated and read back | **0.167** | 0.267 | 0.203 | zip | `archive/zip` |
| tar | 2000 files into a tar and back | **0.031** | 0.036 | 0.143 | tar | `archive/tar` |
| template | a 1000-row HTML page, 2000 times | **0.436** | 0.618 | 4.485 | minijinja | `html/template` |
| sha-hmac | SHA-384 of 256 MiB, 1M HMAC-SHA384 | **1.115** | 1.132 | 1.189 | sha2, hmac | `crypto/sha512`, `crypto/hmac` |
| aes-ctr | 256 MiB with AES-256 in counter mode | **0.034** | 0.050 | 0.035 | aes, ctr | `crypto/cipher` |
| aes-gcm | 64 KiB sealed and opened with AES-256-GCM, 2000 times | **0.029** | **0.029** | 0.059 | ring | `crypto/cipher` |
| chacha20 | 256 MiB with ChaCha20 | **0.081** | 0.088 | 0.300 | chacha20 | `x/crypto/chacha20` |
| crc32c | CRC32C of 1 GiB | **0.032** | 0.136 | 0.033 | crc32c | `hash/crc32` |
| base32 | 64 KiB to base32 and back, 2000 times | **0.169** | 0.171 | 0.444 | data-encoding | `encoding/base32` |
| base64 | 64 KiB to base64 and back, 2000 times | **0.123** | 0.131 | 0.189 | data-encoding | `encoding/base64` |
| hex | 64 KiB to hex and back, 2000 times | **0.116** | 0.147 | 0.228 | data-encoding | `encoding/hex` |
| ecdsa | 4000 P-256 signatures, each checked | 0.276 | **0.222** | 0.327 | ring | `crypto/ecdsa` |
| rsa | 500 RSA-2048 signatures, each checked 20 times | **0.385** | 0.386 | 0.623 | ring | `crypto/rsa` |
| x509-parse | a certificate parsed 200000 times | **0.238** | 0.386 | 1.120 | x509-parser | `crypto/x509` |
| yaml-parse | a 6.7 MB YAML list, 3 times | **0.458** | 1.246 | 2.544 | serde_yaml | `gopkg.in/yaml.v3` |
| wire | 100,000 binary messages read and written, 30 times | **0.119** | 0.123 | 0.196 | by hand, `from_be_bytes` | `encoding/binary` |

In zip each writes at its default level: Iris's archive is 2.8 MB, Go's 3.3 MB. In wire, Rust and Go read and write the messages by hand, the usual way to write a binary protocol in each. Iris signs and checks ECDSA and RSA with the system's OpenSSL.

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
