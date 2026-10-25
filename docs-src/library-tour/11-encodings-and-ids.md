# Encodings and ids

Bytes often travel as text: in a header, a URL, a JSON field or a log line. The `base64`, `base32` and `hex` modules write any bytes as text and read them back. `url` takes URLs apart and escapes their parts. `uuid` makes the ids that services hand out.

## Base64

`base64.encode(data)` writes bytes as Base64 text, four characters for every three bytes. `base64.decode(text)` reads them back. Each gives a new `String`. Text that isn't Base64 fails with `Base64Error.invalid`.

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

`decode` accepts text with or without the `=` at the end, and skips line breaks as found in mail and PEM files.

Standard Base64 uses `+` and `/`, which mean something else in a URL. `encodeUrl` uses `-` and `_` instead and leaves out the `=`, as JSON Web Tokens and many URLs do. `decodeUrl` reads it.

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

## Base32

`base32.encode(data)` writes bytes with 32 characters, upper-case letters and the digits 2 to 7, eight characters for every five bytes. Case, and most systems that pass text along, leave it alone, which is why TOTP secrets and magnet links use it. `pad: false` leaves out the `=` at the end, as they do. `base32.decode` takes either case and skips spaces and line breaks.

```iris run
import base32

!void main() {
    scoped String secret = base32.encode("12345678901234567890", pad: false)
    print(secret)
    scoped String back = try base32.decode("gezd gnbv gy3t qojq")
    print(back)
}
```

```output
GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ
1234567890
```

`encodeHex` and `decodeHex` use base32hex, the digits and then A to V, which sorts in the same order as the bytes. DNSSEC's NSEC3 records name hashes with it.

## Hex

`hex.encode(data)` writes each byte as two hex digits. `hex.decode(text)` reads them back in either case. Hex is twice as long as the bytes and easy to read, so use it for hashes, keys and dumps of a few bytes.

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

To write one number in hex, use `{n:x}` in text (see [Values in text](../tour/07-values-in-text.html)).

## URLs

`url.parse(text)` takes a URL apart into a `url.Url` with its `scheme`, `user`, `host`, `port`, `path`, `query` and `fragment`. The parts are slices of the text, so nothing is copied. When the URL has no port, `port` is the usual one for `http` and `https`.

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

A value put into a query or a path must not contain characters that mean something there, such as `&`, `=`, `?`, `/` or a space. `url.encode` writes every character except letters, digits and `-_.~` as `%XX`. `url.decode` reads them back and reads `+` as a space, as forms send it.

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

A `%` not followed by two hex digits fails with `UrlError.invalid`. In a path, `+` is a plus sign, so decode a part of a path with `url.decodePath`, which leaves `+` as it is. An HTTP handler gets both kinds already decoded through `req.query` and `req.param`.

## UUIDs

A UUID is a 16-byte id, written as 36 characters, that a program can make without asking a database for the next number. `uuid.v4()` is fully random. `uuid.v7()` starts with the time in milliseconds, so later ids sort after earlier ones, which keeps a database index in order. `format` writes either kind as text.

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

`uuid.parse` reads a UUID back in upper or lower case, with or without the dashes, and fails with `UuidError.invalid` for anything else. `time` gives the time a v7 UUID was made.

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

A `Uuid` is a plain 16-byte value. It can be copied, kept in a struct and compared with `equal` without freeing anything. Only `format` gives a `String` to free.
