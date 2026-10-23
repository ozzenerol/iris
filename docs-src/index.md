# Iris

Iris is a compiled language for servers, network tools and command-line programs. It has no garbage collector: memory is freed by the program, with `defer` and `scoped` to make that hard to forget. Errors are values that must be handled, every index is checked, and tasks and channels let one thread do many things at once. The compiler is written in C and produces native code through LLVM.

```iris run
import http

void hello(http.Request *req, http.Response *res) {
    res.text("hello, {req.param("name")}\n")
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/hello/:name", hello)
    print("routes ready")
}
```

```output
routes ready
```

With `try r.serve(8080)` at the end, that is a web server on every CPU core, with HTTP/1.1 and HTTP/2.

## Where to start

- The [Language Tour](tour/index.html) covers every part of the language, one topic per chapter, with programs you can run and their output. Start here.
- The [Library Tour](library-tour/index.html) shows what the standard library does, one task per chapter: files, HTTP, databases, formats, cryptography and more.
- The [Library](std/index.html) has every module of the standard library, with each type and function and what it does.
- [Design](design/index.html) explains how Iris works underneath and why, for people working on the compiler.
- The [Roadmap](roadmap.html) lists each release and what it brought.

## Install

Iris is built from its source. It needs a C compiler and `clang` 15 or newer, which Iris uses to make programs; on Debian and Ubuntu, `sudo apt install build-essential clang`, and on macOS, the Xcode command-line tools. Then, in the source folder:

```shell
./build-and-install.sh
```

That builds `iris` on every core and installs it in `/usr/local/bin`, asking for your password only for the copy. `--prefix ~/.local` installs it in your home folder instead, `--test` runs the tests first, and `--help` lists the rest. `iris version` checks it worked.

## A first program

```iris run
void main() {
    print("hello, world")
}
```

```output
hello, world
```

Save it as `hello.iris`, and run it with `iris run hello.iris`. The [first chapter](tour/01-hello.html) of the tour goes on from there.
