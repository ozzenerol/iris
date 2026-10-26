# Iris

<img src="docs/assets/logo.svg" alt="" width="96">

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

The standard library covers HTTP/1.1 and HTTP/2 servers and clients, TLS with certificate checking, SSH clients and servers, TCP and UDP, Unix sockets, TUN devices and the kernel's routes, SQLite, PostgreSQL and Redis, JSON, YAML, TOML and XML, binary protocol messages read into structs, regular expressions, compression, cryptography with ECDSA and RSA, and the system side of a server: signals, pseudo-terminals, dropping privileges and a seccomp sandbox. The docs in `docs/` have a Language Tour, a Library Tour and the library reference.

## Speed

`bench/` has 19 programs written the same way in Iris, Rust and Go: number crunching, memory, collections, text, channels and network servers.

- Machine: AMD Ryzen 7 5700X, Linux. Iris 0.79.0 with clang 19, Rust 1.99, Go 1.27.
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
| spectral-norm | spectral norm, n = 5500 (s) | 1.21 | 1.21 | **0.95** | 1.20 | 2 / 2 / 2 |
| mandelbrot | mandelbrot, 4000 x 4000 (s) | **0.79** | **0.79** | 0.80 | 0.81 | 2 / 2 / 2 |
| fannkuch-redux | fannkuch-redux, n = 11 (s) | **1.29** | 1.30 | 1.41 | 1.64 | 2 / 2 / 2 |
| binary-trees | binary trees, depth 21 (s) | 6.39 | **6.33** | 8.27 | 17.34 | 130 / 258 / 275 |
| hashmap | 10M inserts and lookups, int keys (s) | **0.90** | 0.94 | 1.14 | 2.11 | 418 / 410 / 366 |
| sort | quicksort of 10M ints (s) | **0.57** | 0.58 | 0.59 | 0.60 | 78 / 78 / 78 |
| strings | build, count, split, replace (s) | **0.18** | 0.18 | 0.26 | 0.49 | 187 / 187 / 237 |
| channels | ping-pong, then 1000 tasks on one channel (s) | **0.22** | 0.22 | 10.71 | 0.56 | 6 / 3 / 4 |
| http-hello | HTTP/1.1 hello, wrk (req/s) | 392k | **394k** | 371k | 227k | 7 / 10 / 19 |
| https-hello | HTTP/1.1 hello over TLS, wrk (req/s) | **296k** | 295k | 278k | 202k | 26 / 15 / 25 |
| http-json | HTTP/1.1 JSON in and out, wrk (req/s) | 400k | **410k** | 345k | 143k | 7 / 10 / 20 |
| tcp-echo | TCP echo, 100 connections (msg/s) | **384k** | 382k | 363k | 293k | 4 / 3 / 5 |
| http2 | HTTP/2 hello, h2load (req/s) | **2,226k** | 2,187k | 736k | 148k | 9 / 14 / 44 |
| grpc | gRPC unary hello, h2load (req/s) | **1,068k** | 1,024k | 412k | 326k | 12 / 25 / 41 |
| tcp-proxy | TCP proxy, 64-byte messages, 100 connections (msg/s) | **190k** | 190k | 186k | 157k | 4 / 4 / 6 |
| tcp-proxy-bulk | TCP proxy, 256 KiB messages, 16 connections (MiB/s each way) | **8,741** | 8,738 | 2,095 | 8,317 | 3 / 3 / 5 |

In fib and spectral-norm, Iris produces the same machine code as C compiled by clang 19. Rust 1.99 uses LLVM 23, which is faster on those two loops on this CPU. On an Intel Core i7-12650H they are level. `bench/README.md` explains how to run the benchmarks.

The servers do less work than the rates show, since the load generator limits them too. This is the CPU time a server uses for one request or message, in nanoseconds, user and system:

| Benchmark | Iris | Iris safe | Rust | Go |
|---|--:|--:|--:|--:|
| http-hello | **7,439** | 7,509 | 9,016 | 17,031 |
| https-hello | 10,158 | **10,136** | 11,653 | 19,251 |
| http-json | 7,442 | **7,344** | 11,478 | 27,100 |
| tcp-echo | 7,982 | **7,978** | 8,284 | 9,556 |
| http2 | **1,347** | 1,375 | 5,410 | 26,306 |
| grpc | **2,767** | 2,893 | 9,563 | 11,974 |
| tcp-proxy | **16,054** | 16,108 | 16,835 | 19,473 |
| tcp-proxy-bulk | 19,141 | **18,565** | 119,310 | 22,577 |

`bench/` also has 40 library benchmarks. Each does the same work with Iris's standard library, the best-known Rust crate, and Go's standard library or its usual package. Times are in seconds.

| Benchmark | What it does | Iris | Rust | Go | Rust uses | Go uses |
|---|---|--:|--:|--:|---|---|
| fmt-text | 2M lines with widths, hex and precision | **0.236** | 0.431 | 0.716 | `write!` | `fmt.Fprintf` |
| utf8 | count, walk and check 16 MB of UTF-8 | **0.050** | 0.051 | 0.114 | `chars`, `from_utf8` | `unicode/utf8` |
| bigint | 20000!, its digits, 300 modular powers | 0.043 | 0.070 | **0.037** | num-bigint | `math/big` |
| rwlock | 4 threads reading and writing | 0.038 | 0.107 | **0.019** | `RwLock` | `sync.RWMutex` |
| heap | 2M pushes and pops | **0.178** | 0.225 | 0.486 | `BinaryHeap` | `container/heap` |
| sortedmap | 1M inserts, lookups and range scans | **0.261** | 0.276 | 0.375 | `BTreeMap` | a sorted slice |
| deque | 27M pushes and pops at both ends | **0.092** | 0.096 | 0.177 | `VecDeque` | a ring buffer |
| sort-by | 5M structs by a key | **0.208** | 0.224 | 0.579 | `sort_unstable_by_key` | `slices.SortFunc` |
| fs-walk | make, walk and remove 50k files | 0.770 | **0.762** | 0.996 | `std::fs` | `filepath.WalkDir` |
| xml-parse | 30 MB of XML as events | **0.094** | 0.120 | 0.737 | quick-xml | `encoding/xml` |
| toml-parse | a 2 MB TOML file, 20 times | **0.810** | 1.224 | 3.976 | toml | BurntSushi/toml |
| zip | 2000 files deflated and read back | **0.166** | 0.267 | 0.202 | zip | `archive/zip` |
| tar | 2000 files into a tar and back | **0.031** | 0.036 | 0.141 | tar | `archive/tar` |
| template | a 1000-row HTML page, 2000 times | **0.438** | 0.616 | 4.452 | minijinja | `html/template` |
| sha-hmac | SHA-384 of 256 MiB, 1M HMAC-SHA384 | **1.114** | 1.121 | 1.189 | sha2, hmac | `crypto/sha512`, `crypto/hmac` |
| aes-ctr | 256 MiB with AES-256 in counter mode | **0.034** | 0.050 | 0.035 | aes, ctr | `crypto/cipher` |
| aes-gcm | 64 KiB sealed and opened with AES-256-GCM, 2000 times | 0.030 | **0.029** | 0.059 | ring | `crypto/cipher` |
| chacha20 | 256 MiB with ChaCha20 | **0.081** | 0.088 | 0.300 | chacha20 | `x/crypto/chacha20` |
| crc32c | CRC32C of 1 GiB | **0.032** | 0.136 | 0.033 | crc32c | `hash/crc32` |
| base32 | 64 KiB to base32 and back, 2000 times | **0.169** | 0.171 | 0.446 | data-encoding | `encoding/base32` |
| base64 | 64 KiB to base64 and back, 2000 times | **0.123** | 0.131 | 0.189 | data-encoding | `encoding/base64` |
| hex | 64 KiB to hex and back, 2000 times | **0.116** | 0.147 | 0.227 | data-encoding | `encoding/hex` |
| ecdsa | 4000 P-256 signatures, each checked | 0.277 | **0.222** | 0.326 | ring | `crypto/ecdsa` |
| rsa | 500 RSA-2048 signatures, each checked 20 times | **0.385** | 0.386 | 0.623 | ring | `crypto/rsa` |
| x509-parse | a certificate parsed 200000 times | **0.240** | 0.384 | 1.116 | x509-parser | `crypto/x509` |
| yaml-parse | a 6.7 MB YAML list, 3 times | **0.458** | 1.244 | 2.458 | serde_yaml | `gopkg.in/yaml.v3` |
| wire | 100,000 binary messages read and written, 30 times | **0.119** | 0.120 | 0.194 | by hand, `from_be_bytes` | `encoding/binary` |
| ssh | 100 SSH logins with a command each, then 512 MiB through a channel | **0.343** | 0.459 | 0.832 | russh | `x/crypto/ssh` |
| sftp | 1000 small files written, read and removed over SFTP, then 256 MiB up and down | 2.002 | **1.646** | 1.934 | russh-sftp | `pkg/sftp` |
| mqtt | 220000 MQTT messages through a broker, 20000 of them at QoS 1 | **0.665** | 2.359 | 4.204 | rumqttd, rumqttc | mochi-mqtt, Paho |
| quic | 200 QUIC connections with a request each, then 512 MiB through a stream | **0.652** | 1.076 | 1.433 | quinn | quic-go |
| protobuf | 100,000 orders read and written as protobuf, 20 times | **0.485** | 0.526 | 0.922 | prost | by hand, `protowire` |
| msgpack | 100,000 orders read and written as MessagePack, 20 times | **0.998** | 1.295 | 4.372 | rmp-serde | vmihailenco/msgpack |
| cbor | 100,000 orders read and written as CBOR, 20 times | **1.017** | 2.555 | 3.577 | ciborium | fxamacker/cbor |
| sigv4 | 200000 S3 requests signed with AWS Signature Version 4, each with a 1 KiB body | **0.518** | 0.833 | 0.901 | aws-sigv4 | `aws-sdk-go-v2` signer |
| mysql-query | MySQL: 1000 inserts in a transaction, 20000 prepared selects by id, then the table read 100 times | **0.616** | 1.425 | 0.667 | sqlx | `go-sql-driver/mysql` |
| metrics | 10M requests counted with labels and a histogram, 2,000 series written 200 times | **0.240** | 0.327 | 0.946 | prometheus | client_golang |
| tracing | 1M spans with traceparent, written as OTLP JSON | **0.461** | 0.931 | 5.956 | opentelemetry-proto + serde_json | otlp + protojson |
| bson | 500000 orders encoded to BSON and decoded back | **0.160** | 0.569 | 1.299 | bson | `mongo-driver` bson |
| mongo-query | MongoDB: 1000 inserts, 20000 finds by an indexed field, then the collection read 100 times | **0.883** | 1.159 | 1.251 | mongodb | `mongo-driver` |

In zip each writes at its default level: Iris's archive is 2.8 MB, Go's 3.3 MB. In wire, Rust and Go read and write the messages by hand, the usual way to write a binary protocol in each. In ssh, a server and a client run in one program, all three with mlkem768x25519-sha256, Ed25519 keys and AES-256-GCM; russh is set to TCP_NODELAY, as Iris and Go are. Iris signs and checks ECDSA and RSA with the system's OpenSSL. In sftp and mqtt, the server or broker and the client run in one program; mochi-mqtt is given room to queue every message, since it drops QoS 0 messages for a subscriber that falls behind. In quic, Iris and Rust use TLS_AES_256_GCM_SHA384 and quic-go picks AES-128-GCM itself. mysql-query and mongo-query ran on an Intel Core i7-12650H against MySQL 8.4 and MongoDB 8.0 in Docker on the same machine, where the server takes much of the time.

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
