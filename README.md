# Iris

<img src="docs/assets/logo.svg" alt="" width="96">

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

Servers, clients, databases (SQLite, PostgreSQL, Redis), JSON, regular expressions, compression, and command-line tools are in the standard library. The docs in `docs/` have a Language Tour, the library, and guides.

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
