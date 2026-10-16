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
    i32 fd // only the net module can use fd
}

pub !Server listen(u16 port) { }

void setOptions(i32 fd) { } // only inside net
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

The standard library comes inside the `iris` command. A few names, like `print`, `format`, `alloc`, `free` and `error`, are always there, and so are `List` and `Map` (see [Collections](collections.html)). The rest is in modules:

| Module | What it has |
|---|---|
| `fs` | `readAll`, `writeAll`, `append`, `exists`, `remove`, `stat(path) Info` (`size`, `isDir`, `modified`), `mkdir`, `mkdirAll`, `rename`, `removeAll`, `readDir(path) Dir` (`entries`, sorted by name), and files opened with `open`, `create` and `openAppend`, which have `read`, `readLine`, `readAll`, `write` and `close`; `FsError` |
| `path` | `join(a, b)`, `base`, `dir`, `ext`, `stem` and `isAbs` |
| `process` | `run(argv, input)`: runs a program and gives an `Output` with `stdout`, `stderr`, `status` and `ok()`; `ProcessError` |
| `str` | `find`, `contains`, `startsWith`, `endsWith`, `compare`, `count`, `trim`, `trimLeft`, `trimRight`, `cut(s, sep) (str, str, bool)`, `lower`, `upper`, `replace`, `repeat`, `parseInt(s) !i64` and `parseFloat(s) !f64` |
| `time` | `now()`, `clock()` for measuring, `date(...)`, `unix(s)`, `parse(iso)`; a `Time` has `iso()`, `format(layout)`, `dateText()`, `local()`, `utc()`, `add(d)`, `sub(t)`, `before`, `after`, `unix()`, `year()` to `nanosecond()` and `weekday()` |
| `math` | `pi`, `e`, `sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`, `floor`, `ceil`, `round`, `trunc`, `abs`, `sin`, `cos`, `tan`, `atan2`, `hypot`, `min`, `max`, `isNaN` and `inf` |
| `random` | `int(lo, hi)`, `float()`, `coin()`, `shuffle(items)`, `bytes(buf)` for keys and tokens from the system's secure source, and `seeded(seed)` for an `Rng` that gives the same numbers again |
| `crypto` | `sha256(data)` and `hmacSha256(key, data)`, 32 bytes each, and `equal(a, b)`, which compares in the same time wherever they differ |
| `base64` | `encode`, `decode`, and the URL-safe `encodeUrl` and `decodeUrl`; `Base64Error` |
| `hex` | `encode` and `decode`; `HexError` |
| `url` | `parse(text) Url` (`scheme`, `user`, `host`, `port`, `path`, `query`, `fragment`), `encode` and `decode` for `%XX`; `UrlError` |
| `sqlite` | `open(path)` gives a `Db` with `exec(sql, args)`, `query(sql, args)`, `lastId()`, `changes()`, `begin`, `commit`, `rollback` and `close`; `Rows` have `next()`, `int(i)`, `float(i)`, `text(i)`, `isNull(i)`, `name(i)`, `columns()` and `close`; `scan(&rows, &value)` fills a `@json` struct; `Value` and `SqlError` |
| `io` | `readLine() !String` and `readAll() !String` from standard input, `ewrite` and `eprint` to standard error, and `IoError` |
| `os` | `argc()` and `arg(i)` for the program's arguments, `env(name) !str`, `exit(code)`, and `OsError` |
| `net` | TCP: `listen(port, host, shared)` and `Listener.accept()`, `dial(host, port)`, and on a `Conn`: `read`, `readLine`, `readExactly`, `write` and `close`; TLS: `dialTls(host, port, ca)`, `Conn.startTls(host, ca)`, `tlsConfig(certFile, keyFile)` and `Conn.acceptTls(config)`; `NetError` |
| `json` | `encode(v) String` and `decode(T, text) !T` for `@json` structs (see [Compile time](compile-time.html)), and `JsonError` |
| `http` | a server: `serve(port, handler, threads)`, `serveOn(listener, handler, threads)` and `serveTls(port, handler, cert, key)`, on every CPU core (or on `threads` of them), where `handler` gets a `Request` (`method`, `path`, `body`, `header(name)`, `param(name)`, `query(name)`, `route()`) and fills in a `Response` (`status`, `text`, `json`, `header(name, value)`); a `Router` (`get`, `post`, `put`, `delete`, `patch`, `handle`, `serve`, `serveTls`); and a client: `get(url)`, `post(url, type, body)`, `send(method, url, body, type)`, which give a `Reply` (`status`, `body`, `header(name)`) |

A small web server. A `Router` sends each request to the handler for its method and path; `:id` matches one part of the path and `*path` the rest:

```iris
import http

void getUser(http.Request *req, http.Response *res) {
    res.json("{{\"id\": \"{req.param("id")}\"}}")
}

void createUser(http.Request *req, http.Response *res) {
    res.status = 201
    res.text("made {req.body}\n")
}

!void main() {
    http.Router r
    r.get("/users/:id", getUser)
    r.post("/users", createUser)
    try r.serve(8080)
}
```

When several patterns match, the one with the most fixed parts wins, so `/users/me` goes before `/users/:id`. A path no pattern has gets 404, and a path that is there only for other methods gets 405 with an `Allow` header.

The client asks other servers, over `http://` or `https://`:

```iris
scoped http.Reply r = try http.get("https://example.com/")
print("{r.status} {r.body.len}")
```

`http.serveTls(port, handler, certFile, keyFile)` and `r.serveTls(port, certFile, keyFile)` answer HTTPS, with a certificate chain and its key in PEM files.

TLS uses OpenSSL 3 (or 1.1), which the program loads the first time it needs it: building needs nothing extra, and programs without TLS don't need OpenSSL at all. On Linux it comes with the system (`libssl3`); on macOS, `brew install openssl@3`. `IRIS_OPENSSL` can name the `libssl` file to use. A client checks the server's certificate against the ones the system trusts, and the host name against the certificate; `SSL_CERT_FILE` names a different file of trusted certificates. The oldest version allowed is TLS 1.2.

A database, with SQLite. Each `?` takes the next value of the list, and `scan` fills a `@json` struct from a row, each field from the column of the same name:

```iris
import sqlite

@json
struct User {
    i64 id
    String name
    i64 age
}

!void main() {
    db := try sqlite.open("app.db")
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
    try db.exec("INSERT INTO users (name, age) VALUES (?, ?)", [.text("ada"), .int(36)])
    rows := try db.query("SELECT * FROM users WHERE age > ?", [.int(30)])
    defer rows.close()
    for try rows.next() {
        scoped User u
        try sqlite.scan(&rows, &u)
        print("{u.id} {u.name}")
    }
}
```

The program loads the system's SQLite the first time it opens a database. It comes with macOS and most Linux systems (`libsqlite3`); `IRIS_SQLITE` can name the library to use. A query waits for the database on its thread.

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
