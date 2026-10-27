# Iris

Iris is a compiled language for servers, network tools and command-line programs.

- No garbage collector. The program frees memory, and `defer` and `scoped` make that hard to forget.
- Errors are values that must be handled. Every index is checked.
- Tasks and channels run many things at once on one thread, and threads spread work over cores.
- The compiler is written in C and produces native code through LLVM.

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

Add `try r.serve(8080)` at the end and it is a web server on every CPU core, speaking HTTP/1.1 and HTTP/2.

## Install

Iris builds from source. It needs a C compiler and `clang` 15 or newer, which it uses to produce programs.

- Debian and Ubuntu: `sudo apt install build-essential clang`
- macOS: the Xcode command-line tools

Then, in the source folder:

```shell
./build-and-install.sh
```

This builds `iris` on every core and installs it in `/usr/local/bin`, asking for a password only for the copy.

- `--prefix ~/.local` installs into your home folder.
- `--test` runs the tests first.
- `--help` lists the other options.

`iris version` confirms the install.

## A first program

```iris run
void main() {
    print("hello, world")
}
```

```output
hello, world
```

Save it as `hello.iris` and run `iris run hello.iris`. The [first chapter](tour/01-hello.html) of the tour continues from there.
