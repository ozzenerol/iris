# Modules

How a program is split into parts, and how packages are used.

## Modules

A module is a folder of `.iris` files. All files in the folder belong to the module and see each other's names.

```output
myapp/
    iris.mod
    main.iris
    config.iris
    net/
        server.iris
        client.iris
```

Here `main.iris` and `config.iris` form the main module, and `net/` is a module named `net`.

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

Fields of a `pub` struct are visible outside the module only when marked `pub` themselves:

```iris
pub struct Server {
    pub u16 port
    i32 fd
}
```

## import

`import` makes another module's names available, written with the module name in front:

```iris
import fs, net

!void main() {
    text := try fs.readAll("config.txt")
    srv := try net.listen(8080)
}
```

A module inside the project is imported by its path from the project's root: `import "net/http"`, used as `http`.

## Packages

A package is a module from somewhere else, named by its repository:

```iris
import "github.com/someone/pg"

conn := try pg.connect(url)
```

The project's `iris.mod` file names the project and the versions of the packages it uses:

```output
module github.com/me/myapp

require github.com/someone/pg v1.2.0
```

`iris get` adds a package and `iris build` downloads what is missing.

## The standard library

The standard library is a set of modules that come with Iris: `io`, `fs`, `str`, `net`, `http`, `json`, `time`, `os` and more. They are imported like any other module. A few names, like `print`, `alloc`, `free` and `error`, are always available without an import.
