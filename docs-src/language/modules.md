# Modules

How a program is split into parts, how packages are used, and what the standard library has.

## Projects and modules

A project is a folder with an `iris.mod` file. Each folder in it is a module: all its `.iris` files belong to the module and see each other's names, with no imports between them.

```output
myapp/
    iris.mod
    main.iris
    config.iris
    net/
        server.iris
        client.iris
```

Here `main.iris` and `config.iris` form the program's module, and `net/` is a module named `net`. `iris run myapp` (or `iris run main.iris` from inside it) compiles the whole project.

A file in a folder without `iris.mod` is a program on its own: `iris run hello.iris` compiles just that file.

`iris.mod` names the project and, later, its packages:

```output
module github.com/me/myapp
```

## pub

A name is visible only inside its module unless it is marked `pub`:

```iris
pub struct Server {
    u16 port
    i32 fd           // only the net module can use fd
}

pub !Server listen(u16 port) { }

void setOptions(i32 fd) { }     // only inside net
```

Fields of a `pub` struct are visible outside the module only when marked `pub` themselves, and so are methods:

```iris
pub struct Server {
    pub u16 port
    i32 fd
}

pub void Server.close() { }
```

All values of a `pub` enum are visible.

## import

`import` makes another module's names available, written with the module's name in front:

```iris
import fs, str

!void main() {
    scoped String text = try fs.readAll("config.txt")
    print("{str.count(text, "\n")} lines")
}
```

A bare name imports a module of the standard library. A quoted path imports a folder of the project, from the project's root: `import "net"` or `import "net/http"`, used as `net` and `http`. Imports are written at the top of each file that uses them.

A module's own names can't be the same as the name of a module it imports, and neither can a local variable's.

Modules can't import each other in a circle.

## Packages

A package is a module from another project, named by its repository:

```iris
import "github.com/someone/pg"

conn := try pg.connect(url)
```

`iris get` downloads a package at a version, which is a tag in its repository, and adds it to `iris.mod`:

```shell
iris get github.com/someone/pg@v1.2.0
```

```output
module github.com/me/myapp

require github.com/someone/pg v1.2.0
```

Packages are kept in `~/.cache/iris/pkg` (or in `$IRIS_PATH/pkg`). To work on a package next to the project, `replace` points it at a folder:

```output
replace github.com/someone/pg => ../pg
```

## The standard library

The standard library comes inside the `iris` command. A few names, like `print`, `format`, `alloc`, `free` and `error`, are always there. The rest is in modules:

| Module | What it has |
|---|---|
| `fs` | `readAll(path) !String`, `writeAll(path, data) !void`, `append(path, data) !void`, `exists(path) bool`, `remove(path) !void`, and `FsError` |
| `str` | `find`, `contains`, `startsWith`, `endsWith`, `count`, `trim`, `trimLeft`, `trimRight`, `cut(s, sep) (str, str, bool)`, `lower`, `upper`, `replace`, `repeat`, `parseInt(s) !i64` and `parseFloat(s) !f64` |
| `math` | `pi`, `e`, `sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `trunc`, `abs`, `sin`, `cos`, `tan`, `atan2`, `hypot`, `min`, `max`, `isNaN` and `inf` |
| `io` | `readLine() !String` and `readAll() !String` from standard input, `ewrite` and `eprint` to standard error, and `IoError` |
| `os` | `argc()` and `arg(i)` for the program's arguments, `env(name) !str`, `exit(code)`, and `OsError` |
| `net` | TCP: `listen(port, host)` and `Listener.accept()`, `dial(host, port)`, and on a `Conn`: `read`, `readLine`, `readExactly`, `write` and `close`; `NetError` |
| `http` | `serve(port, handler)` and `serveOn(listener, handler)`: an HTTP/1.1 server where `handler` gets a `Request` (`method`, `path`, `body`, `header(name)`) and fills in a `Response` (`status`, `text`, `json`) |

A small web server:

```iris
import http

void route(http.Request *req, http.Response *res) {
    match req.path {
        "/" => res.text("hello from iris\n")
        _ => {
            res.status = 404
            res.text("not found\n")
        }
    }
}

!void main() {
    try http.serve(8080, route)
}
```

A complete program:

```iris run
import str

void main() {
    line := "  name = iris  "
    key, value, found := str.cut(str.trim(line), "=")
    if found {
        print("[{str.trim(key)}] is [{str.trim(value)}]")
    }
    scoped String loud = str.upper("hello")
    print("{loud} {str.parseInt("42") catch 0}")
}
```

```output
[name] is [iris]
HELLO 42
```

`str` is also the name of the text type. After `import str`, `str.find` is the module's function and `str` in a declaration is still the type.
