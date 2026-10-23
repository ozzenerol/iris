## Into a struct

`toml.decode[T](text)` reads TOML into a `@json` struct, as `json.decode` reads JSON (see [JSON](../tour/28-json.html)). Tables are structs, arrays are slices or `List`s, and keys the struct doesn't have are skipped. A date or a time goes into a `String` as it was written:

```iris run
import toml

@json
struct Backend {
    String addr
    i64 weight = 1
}

@json
struct Config {
    @json(required) String name
    u16 port = 80
    String started
    List[Backend] backends
}

!void main() {
    text := r"""
        name = "edge"
        port = 8443
        started = 2026-10-23T09:00:00Z

        [[backends]]
        addr = "10.0.0.11:8080"
        weight = 3

        [[backends]]
        addr = "10.0.0.12:8080"
        """
    scoped Config c = try toml.decode[Config](text)
    print("{c.name} on {c.port}, since {c.started}")
    for b in c.backends {
        print("{b.addr} weight {b.weight}")
    }
    _ = toml.decode[Config]("port = \"high\"") catch e {
        print("{e.code}: {e.message}")
        Config{}
    }
}
```

```output
edge on 8443, since 2026-10-23T09:00:00Z
10.0.0.11:8080 weight 3
10.0.0.12:8080 weight 1
TomlError.wrongType: port: expected a number, found a string
```

`toml.encode(v)` writes a struct back as TOML: its plain values first, then each table under a `[header]`, and a `List` of structs as `[[tables]]`.

## Any document

`toml.parse(text)` reads any TOML document into a `toml.Value`, a table. `get` gives the value of a key, or `null`; `at` gives the value at a place in an array, and `len` how many there are. What a value holds is in its `kind`, and its data in `text`, `int`, `float` or `boolean`:

```iris run
import toml

!void main() {
    text := r"""
        title = "Shop"
        ports = [8000, 8001]

        [database]
        host = "db.lan"
        pool = { min = 2, max = 16 }
        """
    scoped toml.Value doc = try toml.parse(text)
    db := doc.get("database")
    print("{doc.get("title").text}: {db.get("host").text}, pool {db.get("pool").get("max").int}")
    ports := doc.get("ports")
    print("{ports.len()} ports, the first {ports.at(0).int}, a {ports.at(0).kind}")
    print("no user: {db.get("user") == null}")
    for i in 0..db.len() {
        print("{db.keys.at(i)} is a {db.at(i).kind}")
    }
}
```

```output
Shop: db.lan, pool 16
2 ports, the first 8000, a integer
no user: true
host is a string
pool is a table
```

## Writing

`toml.table()`, `toml.array()`, `toml.string(s)`, `toml.integer(n)`, `toml.float(f)`, `toml.boolean(b)` and `toml.datetime(s)` make values; `set` puts one in a table and `push` in an array, which own it after. `toml.write(doc)` gives the text:

```iris run
import toml

!void main() {
    scoped toml.Value doc = toml.table()
    doc.set("name", toml.string("edge"))
    tls := toml.table()
    tls.set("cert", toml.string("/etc/edge/cert.pem"))
    tls.set("ratio", toml.float(1.0))
    doc.set("tls", tls)
    scoped String text = toml.write(doc)
    print(text)
}
```

```output
name = "edge"

[tls]
cert = "/etc/edge/cert.pem"
ratio = 1.0

```

## Errors

Text that isn't TOML fails with `TomlError.syntax`, and the message gives the line. A key given twice, a table defined twice, and a key added to an inline table are errors too, as TOML 1.0 says. Filling a struct can also fail with `wrongType`, `missing` and `range`, as `json.decode` does.
