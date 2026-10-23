# Configuration files

A program's settings come from a file, from environment variables, or from both. The `toml` and `ini` modules read the two most common file formats, and `env` reads the environment and `.env` files. All of them can fill a struct, so the rest of the program reads its settings as plain fields.

## TOML into a struct

`toml.decode[T](text)` reads TOML into a `@json` struct, the way `json.decode` reads JSON (see [JSON](01-json.html)). Tables are structs, arrays of tables are `List`s of structs, keys the struct doesn't have are skipped, and fields whose keys are missing keep their defaults:

```iris run
import fs, toml

@json
struct Tls {
    String cert
    String key
}

@json
struct Upstream {
    String addr
    i64 weight = 1
}

@json
struct Config {
    String name
    u16 port = 8080
    Tls tls
    List[Upstream] upstreams
}

!void main() {
    path := "/tmp/iris-tour-proxy.toml"
    try fs.writeAll(path, r"""
        # the edge proxy
        name = "edge"
        port = 8443

        [tls]
        cert = "/etc/edge/cert.pem"
        key = "/etc/edge/key.pem"

        [[upstreams]]
        addr = "10.0.0.11:8080"
        weight = 3

        [[upstreams]]
        addr = "10.0.0.12:8080"
        """)
    scoped String text = try fs.readAll(path)
    scoped Config c = try toml.decode[Config](text)
    print("{c.name} on {c.port}, cert {c.tls.cert}")
    for u in c.upstreams {
        print("  {u.addr} weight {u.weight}")
    }
    try fs.remove(path)
}
```

```output
edge on 8443, cert /etc/edge/cert.pem
  10.0.0.11:8080 weight 3
  10.0.0.12:8080 weight 1
```

The `free` method that `@json` gives the struct frees its `String`s, slices and lists, so `scoped` works on it.

A date or a time in TOML goes into a `String` field as it was written: `started = 2026-10-24T09:00:00Z` gives `"2026-10-24T09:00:00Z"`, which [Time](03-time.html) can read.

## Errors in the file

A file that isn't TOML fails with `TomlError.syntax`, and the message gives the line. A value of the wrong type is `wrongType`, a number too big for its field is `range`, and a field marked `@json(required)` that the file doesn't have is `missing`:

```iris run
import toml

@json
struct Config {
    @json(required) String name
    u16 port = 8080
}

void check(str text) {
    scoped Config c = toml.decode[Config](text) catch e {
        print("{e.code}: {e.message}")
        return
    }
    print("ok: {c.name} on {c.port}")
}

void main() {
    check("name = \"edge\"")
    check("name = \"edge\"\nport = \"high\"")
    check("name = \"edge\"\nport = 70000")
    check("port = 80")
    check("name = edge")
}
```

```output
ok: edge on 8080
TomlError.wrongType: port: expected a number, found a string
TomlError.range: port: 70000 doesn't fit in u16
TomlError.missing: the required field "name" is missing
TomlError.syntax: line 1: expected a value, found "edge"
```

## Writing a file

`toml.encode(v)` writes a `@json` struct as TOML: its plain values first, then each struct field as a `[table]`, and a `List` of structs as `[[tables]]`. A program can write out its defaults for a user to change:

```iris run
import toml

@json
struct Limits {
    i64 connections = 1024
    i64 bodyBytes = 1048576
}

@json
struct Config {
    String name
    u16 port = 8080
    bool metrics = true
    Limits limits
}

!void main() {
    scoped Config c = Config{name: "edge".copy()}
    scoped String text = try toml.encode(c)
    print(text)
}
```

```output
name = "edge"
port = 8080
metrics = true

[limits]
connections = 1024
bodyBytes = 1048576

```

## Any TOML document

When the keys aren't known in advance, `toml.parse(text)` reads any document into a `toml.Value`, which is a table. `get(key)` gives a value, or `null` when the key isn't there. A value's `kind` says what it holds, and `text`, `int`, `float` or `boolean` holds it. `len` and `at(i)` go through an array, and through a table with its `keys`:

```iris run
import toml

!void main() {
    text := r"""
        [services.api]
        port = 8080
        replicas = 3

        [services.worker]
        replicas = 2
        queues = ["mail", "thumbs"]
        """
    scoped toml.Value doc = try toml.parse(text)
    services := doc.get("services")
    for i in 0..services.len() {
        s := services.at(i)
        port := s.get("port")
        print("{services.keys.at(i)}: {s.get("replicas").int} replicas, has a port: {port != null}")
    }
    queues := services.get("worker").get("queues")
    for i in 0..queues.len() {
        print("queue {queues.at(i).text}")
    }
}
```

```output
api: 3 replicas, has a port: true
worker: 2 replicas, has a port: false
queue mail
queue thumbs
```

`toml.table()`, `toml.string(s)`, `toml.integer(n)` and the others make values, `set` and `push` put them in a table or an array, and `toml.write(doc)` gives the text. The [library](../std/index.html#toml) shows them.

## INI files

Older programs and many system tools keep settings in INI files: `[sections]` of `key = value` lines, with `;` or `#` comments. `ini.parse(text)` reads one, `get(section, key)` gives a value, `""` when it isn't there, and `lookup` says whether it is. Keys before the first section are in the section `""`:

```iris run
import ini

!void main() {
    text := r"""
        ; written by the installer
        owner = ops

        [database]
        host = db.lan
        port = 5432

        [cache]
        size = 64MiB
        """
    scoped ini.File f = try ini.parse(text)
    print("owner {f.get("", "owner")}")
    port := try f.get("database", "port").parseInt()
    print("{f.get("database", "host")}:{port}")
    _, found := f.lookup("database", "password")
    print("password set: {found}")
    for s in f.sections {
        print("[{s.name}] {s.keys.len} keys")
    }
}
```

```output
owner ops
db.lan:5432
password set: false
[] 1 keys
[database] 2 keys
[cache] 1 keys
```

Every value is text, so numbers are read with `parseInt` and the other methods of [Text](../tour/06-text.html#reading-numbers-from-text). `set(section, key, value)` changes a value or adds it, `remove` takes a key out, and `write` gives the file back as text, without its comments:

```iris run
import ini

!void main() {
    scoped ini.File f = try ini.parse("[database]\nhost = db.lan\nport = 5432\n")
    f.set("database", "port", "6432")
    f.set("cache", "size", "128MiB")
    _ = f.remove("database", "host")
    scoped String text = f.write()
    print(text)
}
```

```output
[database]
port = 6432

[cache]
size = 128MiB

```

## The environment and .env files

Settings that change between machines, such as addresses and passwords, usually come from environment variables. `env.load(T)` fills an `@env` struct from them, as [Attributes](../tour/27-attributes.html#env) shows. `env.loadWith(T, path)` reads a `.env` file of `KEY=value` lines first. A variable that is already set wins over the file, so the file holds the defaults for development and the real environment changes them:

```iris run
import env, fs, os

@env("SHOP")
struct Settings {
    str dbUrl = "postgres://localhost/shop" // SHOP_DB_URL
    u16 port = 8080                         // SHOP_PORT
    Duration timeout = 10s                  // SHOP_TIMEOUT
    bool debug                              // SHOP_DEBUG
}

!void main() {
    path := "/tmp/iris-tour-shop.env"
    try fs.writeAll(path, r"""
        # development settings
        SHOP_DB_URL=postgres://dev:dev@localhost/shop_dev
        SHOP_PORT=3000
        export SHOP_DEBUG=yes
        """)
    try os.setenv("SHOP_PORT", "9000")
    scoped s := try env.loadWith(Settings, path)
    print("{s.dbUrl}")
    print("port {s.port}, timeout {s.timeout}, debug {s.debug}")
    try fs.remove(path)
}
```

```output
postgres://dev:dev@localhost/shop_dev
port 9000, timeout 10s, debug true
```

`SHOP_PORT` was set before the file was read, so it kept `9000`. A file that isn't there is skipped, so in production the same program runs on the environment alone.

## A file, then the environment

A common setup reads a file for most settings and lets a few environment variables change them, for a container or a test. The file fills the struct, and then each variable that is set replaces its field:

```iris run
import os, toml

@json
struct Config {
    String listen
    u16 port = 8080
    i64 workers = 4
}

!Config load(str text) {
    c := try toml.decode[Config](text)
    errdefer c.free()
    port := os.env("APP_PORT") catch ""
    if port.len > 0 {
        c.port = u16(try port.parseInt())
    }
    workers := os.env("APP_WORKERS") catch ""
    if workers.len > 0 {
        c.workers = try workers.parseInt()
    }
    return c
}

!void main() {
    try os.setenv("APP_WORKERS", "16")
    scoped Config c = try load("listen = \"0.0.0.0\"\nport = 80\n")
    print("{c.listen}:{c.port} with {c.workers} workers")
}
```

```output
0.0.0.0:80 with 16 workers
```

A field the file leaves out keeps the default from the struct, the file changes it, and the environment has the last word.
