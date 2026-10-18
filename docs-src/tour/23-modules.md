# Modules

A program is split into modules: folders of files that share their names, and show other modules only what they mark `pub`. This chapter covers programs and projects, `pub`, `import`, the standard library, and packages from other projects.

## One file

A file that isn't in a project is a program on its own. `iris run hello.iris` compiles that file and the standard library modules it imports, and nothing else. Most of the examples in this tour are such programs.

## Projects

A project is a folder with an `iris.mod` file in it. `iris mod init` makes one, naming the project by its path, usually the address of its repository; without a path, the folder's name is used:

```shell
mkdir proxy
cd proxy
iris mod init github.com/me/proxy
```

The file's first line is that name:

```output
module github.com/me/proxy
```

Each folder of the project is a module. All the `.iris` files in one folder belong to its module: they see each other's names, whatever order they are in, with no imports between them. The files in the project's own folder are the program's main module, which has `main`:

```output
proxy/
    iris.mod
    main.iris          main, and the program's other code
    stats.iris         the same module as main.iris
    upstream/
        upstream.iris  the module upstream
        pick.iris      also upstream
    upstream/probe/
        probe.iris     the module probe
```

`iris run proxy`, or `iris run main.iris` from inside the folder, compiles the whole project, and `iris build` writes the program. A file's name doesn't matter to the compiler; it is for people.

## import

`import` makes another module's names available, with the module's name and a dot in front. A bare name is a module of the standard library; a quoted path is a folder of the project, from its root:

```iris
import fs, str
import "upstream", "upstream/probe"

!void main() {
    scoped String text = try fs.readAll("upstreams.txt")
    servers := try upstream.parse(text)
    try probe.check(servers)
}
```

A quoted path's module is used by its last part: `"upstream/probe"` is `probe`. Imports go at the top of each file that uses them, and only that file sees them. Two modules can't import each other, directly or through others: the program says which one depends on which.

A file's own names can't be the same as the name of a module it imports. A module's names are used with its name in front, so names in different modules never clash: `upstream.parse` and `json.parse` are two different functions.

## pub

A module's names are its own unless they are marked `pub`. Other modules can use only those:

```iris
// upstream/upstream.iris
pub struct Server {
    pub str addr
    pub i64 weight
    i64 failures // only upstream's code can read or set it
}

pub !List[Server] parse(str text) {
    List[Server] servers
    return servers
}

// only upstream's code can call it
i64 weightOf(str line) => line.len
```

A `pub` struct's fields and methods are visible outside only when they are marked `pub` themselves, which keeps a struct's insides free to change. All the values of a `pub` enum are visible. Using a name that isn't `pub` is an error that says so:

```output
main.iris:11: the field failures of Server isn't pub, so only its own module can use it
main.iris:12: upstream.weightOf isn't pub, so only its own module can use it
```

## The standard library

The modules of the standard library are imported by name: `import http, json, log`. `List` and `Map` come from `core`, which every module sees without an import. The [Library](../std/index.html) has every module, with each of its types and functions and what its comments say. A quick look at one:

```iris run
import path, str, time

void main() {
    scoped String p = path.join("/var/log", "proxy.log")
    print("{path.base(p)} in {path.dir(p)}, extension {path.ext(p)}")
    print("{str.startsWith(p, "/var")}")
    start := time.clock()
    print("{time.clock() - start < 1s}")
}
```

```output
proxy.log in /var/log, extension .log
true
true
```

## Packages

A package is a module from another project, imported by its repository's address:

```iris
import "github.com/someone/backoff"

void main() {
    print("{backoff.delay(3)}")
}
```

`iris get` downloads a package at a version, which is a tag in its repository, and writes it into `iris.mod`:

```shell
iris get github.com/someone/backoff@v1.2.0
```

```output
module github.com/me/proxy

require github.com/someone/backoff v1.2.0
```

A project uses one version of each package. Packages are kept in `~/.cache/iris/pkg`, or `$IRIS_PATH/pkg` when that is set, and found there when the program is compiled; nothing is downloaded while compiling.

To work on a package next to the project that uses it, `replace` points the package at a folder:

```output
replace github.com/someone/backoff => ../backoff
```

A package is a project like any other: its `iris.mod` names it by its address, and its `pub` names are what its users see.

## C libraries

A project can also use C code: `import c "header.h"` reads a C header, and a `link` line in `iris.mod` links the library (see [Calling C](36-calling-c.html)):

```output
module github.com/me/app

link sqlite3
```
