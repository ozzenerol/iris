# Iris

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

Servers, clients, databases (SQLite, PostgreSQL, Redis), JSON, regular expressions, compression, and command-line tools are in the standard library. The docs in `docs/` have a Language Tour, the library, and guides.

## Building and installing

```
./build-and-install.sh
```

builds `iris` with every core and installs it in `/usr/local/bin` (sudo only for the copy). `--prefix DIR` installs elsewhere, `--test` runs the tests first, `--no-install` only builds `./iris`.

It needs a C11 compiler, `clang` 15 or newer (to compile programs) and Python 3 (for the tests and the docs). During work on the compiler, `make -j` builds `./iris` and `make test` runs the tests.

## Docs

The docs are in `docs-src/` as Markdown. Build them with:

```
python3 tools/build_docs.py
```

and open `docs/index.html`. They have five parts:

- Language Tour: each part of the language, with examples.
- Library: every module of the standard library.
- Guides: whole programs, step by step.
- Design: how the language works underneath, for anyone working on the compiler.
- Roadmap: what gets built, in order.
