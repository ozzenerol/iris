# Iris

<img src="docs/assets/logo.svg" alt="" width="96">

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

The standard library covers HTTP/1.1 and HTTP/2 servers and clients, TLS with certificate checking, SSH clients and servers, TCP and UDP, Unix sockets, TUN devices and the kernel's routes, SQLite, PostgreSQL and Redis, JSON, YAML, TOML and XML, binary protocol messages read into structs, regular expressions, compression, cryptography with ECDSA and RSA, and the system side of a server: signals, pseudo-terminals, dropping privileges and a seccomp sandbox. The docs in `docs/` have a Language Tour, a Library Tour and the library reference.

## Speed

`bench/` has 19 programs written the same way in Iris, Rust and Go: number crunching, memory, collections, text, channels and network servers.

- Machine: AMD Ryzen 7 5700X, Linux. Iris 0.84.0 with clang 19, Rust 1.99, Go 1.27.
- Times are the median of 5 runs, in seconds.
- Servers run for 10 seconds, twice, on 4 cores, with the load generator on the other 4.
- Memory is the peak, in MiB. The fastest result is in bold.
- Iris builds with `--mode=release-fast`, and with `--mode=release-safe` ("Iris safe"), which checks every index and slice as Rust does.
- Rust builds with `--release`, LTO and one codegen unit. Go builds with `go build`.

| Benchmark | What it does | Iris | Iris safe | Rust | Go | Memory, MiB: Iris / Rust / Go |
|---|---|--:|--:|--:|--:|--:|
| fib | recursive fibonacci(42) (s) | 0.53 | 0.53 | **0.47** | 1.07 | 2 / 2 / 2 |
| sieve | sieve of Eratosthenes to 100M (s) | 0.55 | **0.54** | 0.55 | 0.57 | 97 / 97 / 98 |
| nbody | n-body, 50M steps (s) | 1.75 | 1.75 | **1.72** | 2.56 | 2 / 2 / 2 |
| spectral-norm | spectral norm, n = 5500 (s) | 1.21 | 1.21 | **0.95** | 1.20 | 2 / 2 / 2 |
| mandelbrot | mandelbrot, 4000 x 4000 (s) | **0.79** | **0.79** | 0.80 | 0.81 | 2 / 2 / 2 |
| fannkuch-redux | fannkuch-redux, n = 11 (s) | **1.29** | 1.30 | 1.42 | 1.64 | 2 / 2 / 2 |
| binary-trees | binary trees, depth 21 (s) | 6.41 | **6.39** | 8.35 | 17.49 | 130 / 258 / 275 |
| hashmap | 10M inserts and lookups, int keys (s) | **0.92** | 0.96 | 1.16 | 2.13 | 418 / 410 / 365 |
| sort | quicksort of 10M ints (s) | **0.58** | 0.58 | 0.59 | 0.60 | 78 / 78 / 78 |
| strings | build, count, split, replace (s) | **0.18** | 0.19 | 0.27 | 0.47 | 187 / 187 / 232 |
| channels | ping-pong, then 1000 tasks on one channel (s) | 0.22 | **0.22** | 10.99 | 0.57 | 6 / 3 / 4 |
| http-hello | HTTP/1.1 hello, wrk (req/s) | 387k | **388k** | 364k | 225k | 8 / 10 / 19 |
| https-hello | HTTP/1.1 hello over TLS, wrk (req/s) | 288k | **294k** | 279k | 202k | 28 / 15 / 25 |
| http-json | HTTP/1.1 JSON in and out, wrk (req/s) | 412k | **413k** | 342k | 145k | 8 / 11 / 20 |
| tcp-echo | TCP echo, 100 connections (msg/s) | 371k | **378k** | 365k | 294k | 4 / 3 / 5 |
| http2 | HTTP/2 hello, h2load (req/s) | 2,068k | **2,102k** | 736k | 150k | 9 / 13 / 42 |
| grpc | gRPC unary hello, h2load (req/s) | **1,057k** | 1,023k | 411k | 325k | 12 / 24 / 43 |
| tcp-proxy | TCP proxy, 64-byte messages, 100 connections (msg/s) | **191k** | 191k | 185k | 157k | 4 / 4 / 6 |
| tcp-proxy-bulk | TCP proxy, 256 KiB messages, 16 connections (MiB/s each way) | **8,697** | 8,677 | 2,089 | 8,329 | 3 / 3 / 5 |

In fib and spectral-norm, Iris produces the same machine code as C compiled by clang 19. Rust 1.99 uses LLVM 23, which is faster on those two loops on this CPU. On an Intel Core i7-12650H they are level. `bench/README.md` explains how to run the benchmarks.

The servers do less work than the rates show, since the load generator limits them too. This is the CPU time a server uses for one request or message, in nanoseconds, user and system:

| Benchmark | Iris | Iris safe | Rust | Go |
|---|--:|--:|--:|--:|
| http-hello | 7,684 | **7,657** | 9,251 | 17,168 |
| https-hello | 10,382 | **10,306** | 11,929 | 19,229 |
| http-json | 7,297 | **7,282** | 11,437 | 26,810 |
| tcp-echo | 8,045 | **7,957** | 8,226 | 9,527 |
| http2 | **1,428** | 1,483 | 5,400 | 26,066 |
| grpc | **2,829** | 2,900 | 9,593 | 12,020 |
| tcp-proxy | **15,861** | 15,974 | 16,826 | 19,445 |
| tcp-proxy-bulk | 18,969 | **18,644** | 119,593 | 22,602 |

`bench/` also has 41 library benchmarks. Each does the same work with Iris's standard library, the best-known Rust crate, and Go's standard library or its usual package. Times are in seconds.

| Benchmark | What it does | Iris | Rust | Go | Rust uses | Go uses |
|---|---|--:|--:|--:|---|---|
| fmt-text | 2M lines with widths, hex and precision | **0.236** | 0.436 | 0.715 | `write!` | `fmt.Fprintf` |
| utf8 | count, walk and check 16 MB of UTF-8 | **0.050** | 0.052 | 0.114 | `chars`, `from_utf8` | `unicode/utf8` |
| bigint | 20000!, its digits, 300 modular powers | 0.043 | 0.070 | **0.037** | num-bigint | `math/big` |
| rwlock | 4 threads reading and writing | 0.039 | 0.108 | **0.019** | `RwLock` | `sync.RWMutex` |
| heap | 2M pushes and pops | **0.178** | 0.224 | 0.488 | `BinaryHeap` | `container/heap` |
| sortedmap | 1M inserts, lookups and range scans | **0.259** | 0.275 | 0.376 | `BTreeMap` | a sorted slice |
| deque | 27M pushes and pops at both ends | **0.091** | 0.096 | 0.180 | `VecDeque` | a ring buffer |
| sort-by | 5M structs by a key | **0.209** | 0.225 | 0.580 | `sort_unstable_by_key` | `slices.SortFunc` |
| fs-walk | make, walk and remove 50k files | 0.777 | **0.770** | 1.007 | `std::fs` | `filepath.WalkDir` |
| xml-parse | 30 MB of XML as events | **0.094** | 0.116 | 0.736 | quick-xml | `encoding/xml` |
| toml-parse | a 2 MB TOML file, 20 times | **0.813** | 1.205 | 3.928 | toml | BurntSushi/toml |
| zip | 2000 files deflated and read back | **0.166** | 0.252 | 0.202 | zip | `archive/zip` |
| tar | 2000 files into a tar and back | **0.031** | 0.036 | 0.143 | tar | `archive/tar` |
| template | a 1000-row HTML page, 2000 times | **0.433** | 0.660 | 4.457 | minijinja | `html/template` |
| sha-hmac | SHA-384 of 256 MiB, 1M HMAC-SHA384 | **1.114** | 1.131 | 1.191 | sha2, hmac | `crypto/sha512`, `crypto/hmac` |
| aes-ctr | 256 MiB with AES-256 in counter mode | **0.034** | 0.050 | **0.034** | aes, ctr | `crypto/cipher` |
| aes-gcm | 64 KiB sealed and opened with AES-256-GCM, 2000 times | **0.029** | **0.029** | 0.059 | ring | `crypto/cipher` |
| chacha20 | 256 MiB with ChaCha20 | **0.080** | 0.088 | 0.300 | chacha20 | `x/crypto/chacha20` |
| crc32c | CRC32C of 1 GiB | **0.032** | 0.136 | 0.033 | crc32c | `hash/crc32` |
| base32 | 64 KiB to base32 and back, 2000 times | **0.169** | 0.171 | 0.445 | data-encoding | `encoding/base32` |
| base64 | 64 KiB to base64 and back, 2000 times | **0.123** | 0.132 | 0.189 | data-encoding | `encoding/base64` |
| hex | 64 KiB to hex and back, 2000 times | **0.116** | 0.147 | 0.228 | data-encoding | `encoding/hex` |
| ecdsa | 4000 P-256 signatures, each checked | **0.161** | 0.222 | 0.327 | ring | `crypto/ecdsa` |
| rsa | 500 RSA-2048 signatures, each checked 20 times | **0.384** | 0.386 | 0.623 | ring | `crypto/rsa` |
| x509-parse | a certificate parsed 200000 times | **0.235** | 0.388 | 1.126 | x509-parser | `crypto/x509` |
| yaml-parse | a 6.7 MB YAML list, 3 times | **0.462** | 1.250 | 2.543 | serde_yaml | `gopkg.in/yaml.v3` |
| wire | 100,000 binary messages read and written, 30 times | **0.121** | **0.121** | 0.195 | by hand, `from_be_bytes` | `encoding/binary` |
| ssh | 100 SSH logins with a command each, then 512 MiB through a channel | **0.321** | 0.459 | 0.834 | russh | `x/crypto/ssh` |
| sftp | 1000 small files written, read and removed over SFTP, then 256 MiB up and down | **1.341** | 1.688 | 1.971 | russh-sftp | `pkg/sftp` |
| mqtt | 220000 MQTT messages through a broker, 20000 of them at QoS 1 | **0.702** | 2.416 | 4.286 | rumqttd, rumqttc | mochi-mqtt, Paho |
| quic | 200 QUIC connections with a request each, then 512 MiB through a stream | **0.661** | 1.053 | 1.434 | quinn | quic-go |
| protobuf | 100,000 orders read and written as protobuf, 20 times | **0.479** | 0.529 | 0.923 | prost | by hand, `protowire` |
| msgpack | 100,000 orders read and written as MessagePack, 20 times | **0.996** | 1.269 | 4.391 | rmp-serde | vmihailenco/msgpack |
| cbor | 100,000 orders read and written as CBOR, 20 times | **1.010** | 2.533 | 3.587 | ciborium | fxamacker/cbor |
| sigv4 | 200000 S3 requests signed with AWS Signature Version 4, each with a 1 KiB body | **0.517** | 0.836 | 0.904 | aws-sigv4 | `aws-sdk-go-v2` signer |
| sys | CPU per core, memory, load, network, disks, mounts and this process, read from /proc 20000 times | **15.672** | 17.555 | 24.159 | procfs | prometheus/procfs |
| sqlite-rows | SQLite in memory: 100000 structs added in a transaction, read back, then 20000 found by key and written back | **0.103** | 0.110 | 0.421 | rusqlite | mattn/go-sqlite3 |
| mysql-query | MySQL: 1000 inserts in a transaction, 20000 prepared selects by id, then the table read 100 times | **0.583** | 1.514 | 0.617 | sqlx | `go-sql-driver/mysql` |
| metrics | 10M requests counted with labels and a histogram, 2,000 series written 200 times | **0.239** | 0.327 | 0.942 | prometheus | client_golang |
| tracing | 1M spans with traceparent, written as OTLP JSON | **0.461** | 0.937 | 5.939 | opentelemetry-proto + serde_json | otlp + protojson |
| bson | 500000 orders encoded to BSON and decoded back | **0.160** | 0.580 | 1.286 | bson | `mongo-driver` bson |
| mongo-query | MongoDB: 1000 inserts, 20000 finds by an indexed field, then the collection read 100 times | **1.722** | 2.103 | 2.518 | mongodb | `mongo-driver` |

In zip each writes at its default level: Iris's archive is 2.8 MB, Go's 3.3 MB. In wire, Rust and Go read and write the messages by hand, the usual way to write a binary protocol in each. In ssh, a server and a client run in one program, all three with mlkem768x25519-sha256, Ed25519 keys and AES-256-GCM; russh is set to TCP_NODELAY, as Iris and Go are. Iris signs and checks ECDSA and RSA with the system's OpenSSL. In sftp and mqtt, the server or broker and the client run in one program; mochi-mqtt is given room to queue every message, since it drops QoS 0 messages for a subscriber that falls behind. In quic, Iris and Rust use TLS_AES_256_GCM_SHA384 and quic-go picks AES-128-GCM itself. In sqlite-rows, all three use the system's SQLite, Rust and Go with a prepared statement for each kind of call. In sys, the container the benchmarks run in gets several /proc files, meminfo, loadavg and diskstats among them, from lxcfs through FUSE, which makes each read slower than on a plain machine. mysql-query and mongo-query ran on an Intel Core i7-12650H against MySQL 8.4 and MongoDB 8.0 in Docker on the same machine, where the server takes much of the time.

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
