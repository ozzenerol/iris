# Encodings and ids

Bytes often have to travel as text: in a header, a URL, a JSON field or a log line. The `base64` and `hex` modules write any bytes as text and read them back, `url` takes URLs apart and escapes their parts, and `uuid` makes the ids that services hand out.

## Base64

`base64.encode(data)` writes bytes as Base64 text, four characters for every three bytes, and `base64.decode(text)` reads them back. Each gives a new `String`. Text that isn't Base64 fails with `Base64Error.invalid`:

```iris run
import base64

!void main() {
    scoped String token = base64.encode("ada:s3cret")
    print("Authorization: Basic {token}")
    scoped String back = try base64.decode(token)
    user, pass, _ := back.cut(":")
    print("{user} / {pass}")
    _ = base64.decode("not base64!") catch e {
        print("{e.code}")
        "".copy()
    }
}
```

```output
Authorization: Basic YWRhOnMzY3JldA==
ada / s3cret
Base64Error.invalid
```

`decode` takes text with or without the `=` at the end, and skips line breaks, as Base64 in mail and PEM files has them.

Standard Base64 uses `+` and `/`, which mean something else in a URL. `encodeUrl` uses `-` and `_` instead and leaves out the `=`, as JSON Web Tokens and many URLs do, and `decodeUrl` reads it:

```iris run
import base64

!void main() {
    data := "subject?id=7&x=~~~"
    scoped String std = base64.encode(data)
    scoped String safe = base64.encodeUrl(data)
    print(std)
    print(safe)
    scoped String back = try base64.decodeUrl(safe)
    print(back)
}
```

```output
c3ViamVjdD9pZD03Jng9fn5+
c3ViamVjdD9pZD03Jng9fn5-
subject?id=7&x=~~~
```

## Hex

`hex.encode(data)` writes each byte as two hex digits, and `hex.decode(text)` reads them back, in either case. Hex is twice as long as the bytes, and easy to read, so it suits hashes, keys and dumps of a few bytes:

```iris run
import hex

!void main() {
    scoped String h = hex.encode("GET /")
    print(h)
    scoped String back = try hex.decode("4F4b")
    print(back)
    _ = hex.decode("abc") catch e {
        print("{e.code}: {e.message}")
        "".copy()
    }
}
```

```output
474554202f
OK
HexError.invalid: hex text has an odd number of digits
```

To write one number in hex, `{n:x}` in text is enough (see [Values in text](../tour/07-values-in-text.html)).

## URLs

`url.parse(text)` takes a URL apart into a `url.Url`: its `scheme`, `user`, `host`, `port`, `path`, `query` and `fragment`. The parts are slices of the text, so nothing is copied. When no port is written, `port` is the usual one for `http` and `https`:

```iris run
import url

!void main() {
    u := try url.parse("https://ada:pw@api.example.com/v1/users?page=2&q=lo%20ve#top")
    print("scheme {u.scheme}, user {u.user}, host {u.host}, port {u.port}")
    print("path {u.path}, query {u.query}, fragment {u.fragment}")
    local := try url.parse("http://127.0.0.1:9000/health")
    print("{local.host}:{local.port}{local.path}")
}
```

```output
scheme https, user ada:pw, host api.example.com, port 443
path /v1/users, query page=2&q=lo%20ve, fragment top
127.0.0.1:9000/health
```

A value put into a query or a path must not carry characters that mean something there, such as `&`, `=`, `?`, `/` or a space. `url.encode` writes every character except letters, digits and `-_.~` as `%XX`, and `url.decode` reads them back, with `+` as a space, as forms send it:

```iris run
import str, url

!void main() {
    scoped String q = url.encode("tea & cake = 5/5")
    print("/search?q={q}")
    u := try url.parse("/search?q=green+tea&max=10%25")
    scoped pairs := u.query.split("&")
    for p in pairs {
        key, value, _ := p.cut("=")
        scoped String v = try url.decode(value)
        print("{key} = {v}")
    }
}
```

```output
/search?q=tea%20%26%20cake%20%3D%205%2F5
q = green tea
max = 10%
```

A `%` not followed by two hex digits fails with `UrlError.invalid`. In a path, `+` is a plus sign, so a part of a path is read with `url.decodePath`, which leaves it as it is. An HTTP handler gets both kinds decoded already, with `req.query` and `req.param`.

## UUIDs

A UUID is a 16-byte id, written as 36 characters, that a program can make without asking a database for the next number. `uuid.v4()` is fully random. `uuid.v7()` starts with the time in milliseconds, so ids made later sort after ones made earlier, which keeps a database index in order. `format` writes either as text:

```iris run
import uuid

void main() {
    a := uuid.v4()
    b := uuid.v7()
    scoped String at = a.format()
    scoped String bt = b.format()
    print("{at.len} characters, versions {a.version()} and {b.version()}")
    print("{a.equal(b)} {a.isNil()} {uuid.nil().isNil()}")
}
```

```output
36 characters, versions 4 and 7
false false true
```

`uuid.parse` reads a UUID back, in upper or lower case, with or without the dashes, and fails with `UuidError.invalid` for anything else. The time a v7 UUID was made comes back with `time`:

```iris run
import uuid

!void main() {
    id := try uuid.parse("0190E5D48A2B7C3D9E4F5A6B7C8D9E0F")
    scoped String text = id.format()
    scoped String day = id.time().dateText()
    print("{text}, made on {day}")
    later := uuid.v7()
    print("sorts before a new one: {id.compare(later) < 0}")
    _ = uuid.parse("0190e5d4-xxxx") catch e {
        print("{e.code}")
        uuid.nil()
    }
}
```

```output
0190e5d4-8a2b-7c3d-9e4f-5a6b7c8d9e0f, made on 2024-07-24
sorts before a new one: true
UuidError.invalid
```

A `Uuid` is a plain value of 16 bytes, so it can be copied, kept in a struct and compared with `equal` without freeing anything. Only `format` gives a `String` to free.
