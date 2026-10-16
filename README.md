# Iris

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

The language is being designed. No compiler code exists yet.

## Docs

The docs are in `docs-src/` as Markdown. Build them with:

```
python3 tools/build_docs.py
```

and open `docs/index.html`. They have three parts:

- Design: how the language works underneath, for anyone working on the compiler.
- Language: each part of the language, with examples.
- Roadmap: what gets built, in order.
