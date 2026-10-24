# Text

Iris has two text types. A `str` views text stored elsewhere. A `String` owns its text and can grow. This chapter covers both, text literals, and the methods on text.

## str

A `str` is a pointer to bytes and a length. A string literal is a `str` that points into the program:

```iris run
void main() {
    line := "GET /index.html HTTP/1.1"

    print("{line.len} bytes")
    print("first byte {line[0]}, last {line[line.len - 1]}")
}
```

```output
24 bytes
first byte 71, last 49
```

Indexing gives a `u8`. Indexes are checked: `line[24]` stops a debug build with the location.

`s[from:to]` is a `str` into the same bytes. Nothing is copied, so slicing is free at any length. Either end can be omitted.

```iris run
void main() {
    line := "GET /index.html HTTP/1.1"
    print("[{line[0:3]}] [{line[4:15]}] [{line[16:]}] [{line[:3]}] [{line[3:3]}]")
}
```

```output
[GET] [/index.html] [HTTP/1.1] [GET] []
```

`==` and `!=` compare bytes. `compare` orders two strings and returns a negative number, zero or a positive number:

```iris run
void main() {
    method := "POST"

    print("{method == "POST"} {method != "GET"}")
    print("{"api.lan".compare("db.lan")} {"web2".compare("web1")} {"x".compare("x")}")
}
```

```output
true true
-3 1 0
```

## UTF-8

Text is UTF-8. Characters outside ASCII take several bytes, so `len` counts bytes and an index can land inside a character:

```iris run
void main() {
    city := "Köln" // from an X-City header
    print("{city.len} bytes")

    for i, b in city {
        print("{i}: {b}")
    }
}
```

```output
5 bytes
0: 75
1: 195
2: 182
3: 108
4: 110
```

`ö` is the bytes 195 and 182. Searching, cutting and comparing work on bytes, which is correct for UTF-8: no character's bytes appear inside another's, so a search for `"ö"` finds it. `<`, `<=`, `>` and `>=` order byte by byte, shorter first on a shared prefix. For UTF-8 that is code point order, so `"Zebra" < "apple"`.

`runes()` iterates over code points, each an `i32`. `runeCount` counts them. `validUtf8` checks the encoding. A non-ASCII character literal such as `'ö'` is its code point.

```iris run
import str

void main() {
    city := "Köln 東京"
    print("{city.len} bytes, {city.runeCount()} characters")

    for r in city.runes() {
        if r > 127 {
            print("{r:x}")
        }
    }

    print("{'ö' == 0xf6} {str.validUtf8("Köln")} {str.validUtf8("K\xf6ln")}")
}
```

```output
12 bytes, 7 characters
f6
6771
4eac
true true false
```

- Invalid bytes come out of `runes()` as `str.REPLACEMENT`, one per byte, so the loop always ends.
- `decodeRune(at)` returns the code point at a byte offset and its length in bytes.
- `str.appendRune(&s, r)` appends a code point to a `String`. `str.fromRune(r)` makes a new one.
- A non-ASCII character literal can't be stored in a `u8`. Its bytes are the string `"ö"`.

`str.isDigit`, `isHexDigit`, `isLetter`, `isAlnum`, `isUpper`, `isLower` and `isPunct` classify ASCII. `isSpace` also knows Unicode spaces. `str.toUpper` and `str.toLower` change one ASCII letter.

## String

A `String` owns its bytes, can grow, and must be freed. `.copy()` makes one from a `str`. `append` adds to the end.

```iris run
void main() {
    String req = "GET ".copy()
    req.append("/api/users")
    req.append(" HTTP/1.1")

    print(req)
    print("{req.len} bytes")

    req.free()
}
```

```output
GET /api/users HTTP/1.1
23 bytes
```

`scoped` frees it at the end of the block, however the block is left:

```iris run
void main() {
    scoped String path = "/var/log".copy()
    path.append("/nginx")
    print(path)
}
```

```output
/var/log/nginx
```

A `String` works wherever a `str` is expected: as an argument, with `==`, in slicing. The `str` points into the `String`'s bytes and is valid while the `String` is alive and doesn't grow.

```iris run
void main() {
    scoped String url = "https://api.example.com/v1".copy()
    str scheme = url[0:5]

    print("{scheme} {url.contains("example")} {url == "https://api.example.com/v1"}")
}
```

```output
https true true
```

A function that returns a `String`, such as `upper`, `replace` or `format`, gives the caller a new one to free.

- `insert` adds text at a byte offset.
- `truncate` keeps the first n bytes.
- `clear` empties the `String` and keeps its memory.
- `reserve` makes room so appends up to that size don't reallocate.

```iris run
void main() {
    scoped String line = "200 OK".copy()
    line.reserve(64)

    line.insert(0, "HTTP/1.1 ")
    print(line)

    line.truncate(8)
    print("[{line}]")

    line.clear()
    print("[{line}] {line.len}")
}
```

```output
HTTP/1.1 200 OK
[HTTP/1.1]
[] 0
```

An offset outside the `String` stops the program with the location.

## Literals

A literal is written in double quotes. Escapes:

| Escape | Meaning |
|---|---|
| `\n` | newline |
| `\t` | tab |
| `\r` | carriage return |
| `\0` | zero byte |
| `\\` | backslash |
| `\"` | double quote |
| `\x41` | a byte in two hex digits |

`{` and `}` insert values (see the next chapter), so a literal brace is written `{{` or `}}`.

```iris run
void main() {
    print("200\tOK, \"quoted\", C:\\logs, \x41, {{\"ok\": true}}")
}
```

```output
200	OK, "quoted", C:\logs, A, {"ok": true}
```

Triple quotes hold text over several lines. The text starts on the line after `"""` and ends before the line with the closing `"""`. Indentation common to all lines is removed, so the text can follow the code's indentation.

```iris run
void main() {
    config := """
        services:
          web:
            port: 8080
        """
    print(config)
}
```

```output
services:
  web:
    port: 8080
```

In a raw string, `r"..."`, backslashes and braces are plain characters. Use it for Windows paths and regular expressions:

```iris run
void main() {
    print(r"C:\logs\{date}.log")
    print(r"\d+\.\d+\.\d+\.\d+")
}
```

```output
C:\logs\{date}.log
\d+\.\d+\.\d+\.\d+
```

`r"""..."""` is raw text over several lines and also keeps double quotes. Use it for JSON:

```iris run
void main() {
    body := r"""
        {"host": "10.0.0.5", "ports": [22, 443]}
        """
    print(body)
}
```

```output
{"host": "10.0.0.5", "ports": [22, 443]}
```

## Methods

`str` and `String` have methods to inspect and build text. Methods that return a `str` return part of the input, without copying:

```iris run
void main() {
    raw := "  Content-Type: text/html  "
    h := raw.trim()
    print("[{h}] [{raw.trimLeft()}] [{raw.trimRight()}]")

    name, value, found := h.cut(": ")
    print("{name} -> {value} ({found})")

    print("{h.find(":")} {"/api/v1/users".findLast("/")} {h.contains("html")}")
    print("{h.startsWith("Content-")} {h.endsWith("/html")} {"10.0.0.1".count(".")}")
}
```

```output
[Content-Type: text/html] [Content-Type: text/html  ] [  Content-Type: text/html]
Content-Type -> text/html (true)
12 7 true
true true 3
```

The methods are the `str` module's functions with the text as first argument: `h.trim()` is `str.trim(h)`. Methods need no import. Calling `str.trim(h)` or `str.join` directly needs `import str`.

`split` cuts at each separator, `fields` at each run of whitespace. Both return a `List` of `str`s into the original text. `str.join` joins parts:

```iris run
import str

void main() {
    scoped parts := "GET,/health,,200".split(",")
    print("{parts.len} parts, the third is [{parts.at(2)}]")

    scoped cols := "tcp   0  0 0.0.0.0:22\tLISTEN\n".fields()
    scoped String joined = str.join(cols.items(), " | ")
    print(joined)
}
```

```output
4 parts, the third is []
tcp | 0 | 0 | 0.0.0.0:22 | LISTEN
```

Methods that change text return a new `String`:

```iris run
void main() {
    scoped String a = "get".upper()
    scoped String b = "Content-Length".lower()
    scoped String c = "/api/v1/users/v1".replace("v1", "v2")
    scoped String d = "-".repeat(12)

    print("{a} {b} {c}")
    print(d)
}
```

```output
GET content-length /api/v2/users/v2
------------
```

`upper` and `lower` change ASCII letters only.

More methods:

```iris run
import str

void main() {
    print("{"/api/users".trimPrefix("/api")} {"main.iris".trimSuffix(".iris")}")

    dir, file, _ := "/var/log/nginx/access.log".cutLast("/")
    print("{dir} {file}")

    scoped kv := "key=a=b".splitN("=", 2)
    print("{kv.at(0)} -> {kv.at(1)}")

    scoped rows := "GET /\r\nHost: lan\r\n".lines()
    print("{rows.len} lines, the second is [{rows.at(1)}]")

    scoped String id = "42".padLeft(6, "0")
    scoped String name = "x-request-id".title()
    print("{id} {name} {"Content-Length".equalFold("content-length")}")
}
```

```output
/users main
/var/log/nginx access.log
key -> a=b
2 lines, the second is [Host: lan]
000042 X-Request-Id true
```

- `splitN` stops after n parts. The last part holds the rest.
- `lines` cuts at `\n` and `\r\n`. A final line break adds no empty line.
- `padLeft` and `padRight` count characters.
- `reverse` reverses the characters.

## Parsing numbers

`parseInt` and `parseFloat` read a number and fail when the text isn't one (see [Errors](18-errors.html)):

```iris run
void main() {
    port := "8080".parseInt() catch 0
    loss := "0.25".parseFloat() catch 0.0
    bad := "80a".parseInt() catch -1
    print("{port} {loss} {bad}")

    _ = "".parseInt() catch e {
        print("{e.code}: {e.message}")
        0
    }
}
```

```output
8080 0.25 -1
ParseError.empty: no number in an empty text
```

`parseInt` accepts an optional sign and decimal digits. A number beyond `i64` gives `ParseError.tooBig`. `parseIntBase` reads bases 2 to 36: `"ff".parseIntBase(16)` is 255. To format a number in another base, see [Values in text](07-values-in-text.html).
