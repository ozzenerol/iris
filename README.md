# Iris

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

The language is being designed. Version 0.5 runs programs with integers, `bool`, functions, `if`, `for`, `match`, constants, structs, methods, pointers, arrays, slices, strings, enums (also with data), errors, `defer` and several return values.

## Building

```
make
make test
./iris run hello.iris
```

It needs a C11 compiler, `clang` 15 or newer (to compile programs) and Python 3 (for the tests and the docs).

## Docs

The docs are in `docs-src/` as Markdown. Build them with:

```
python3 tools/build_docs.py
```

and open `docs/index.html`. They have three parts:

- Design: how the language works underneath, for anyone working on the compiler.
- Language: each part of the language, with examples.
- Roadmap: what gets built, in order.
