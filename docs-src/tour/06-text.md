# Text

Iris has two types for text. A `str` looks at text that is stored somewhere else; a `String` owns its text and can grow. This chapter covers both, the ways to write text in the program, and the methods that work on it.

## str

A `str` is a slice of bytes: a pointer to some text and its length. A string literal is a `str` that points into the program itself:

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

Indexing gives a byte, a `u8`. Every index is checked, so `line[24]` stops a debug build with a message that says where.

A slice of a `str`, `s[from:to]`, is another `str` that points into the same text. Nothing is copied, so slicing costs nothing whatever the length. Either end can be left out:

```iris run
void main() {
    line := "GET /index.html HTTP/1.1"
    print("[{line[0:3]}] [{line[4:15]}] [{line[16:]}] [{line[:3]}] [{line[3:3]}]")
}
```

```output
[GET] [/index.html] [HTTP/1.1] [GET] []
```

`==` and `!=` compare the bytes of two strings. `compare` puts them in order, giving a number below zero, zero or above zero:

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

Text is stored as UTF-8. Letters outside ASCII take more than one byte, so `len` counts bytes, not letters, and an index can land inside a letter:

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

`ö` is the two bytes 195 and 182. Searching, cutting and comparing work on bytes, which is correct for UTF-8 text: a search for `"ö"` finds it, since no letter's bytes appear inside another's. `<`, `<=`, `>` and `>=` put text in order byte by byte, with the shorter first when one starts the other, which for UTF-8 is the order of the code points: `"Zebra" < "apple"`, as upper-case letters come first.

`runes()` goes over the characters instead, each a code point, an `i32`. `runeCount` counts them, and `validUtf8` says whether text is UTF-8 at all. A character literal beyond ASCII, such as `'ö'`, is its code point too:

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

Bytes that aren't UTF-8 come out of `runes()` as `str.REPLACEMENT`, one for each byte, so a loop over any text ends. `decodeRune(at)` gives the character at a byte and how many bytes it takes, `str.appendRune(&s, r)` adds a character's bytes to a `String`, and `str.fromRune(r)` makes one. A character literal beyond ASCII can't be given to a `u8`, which is a byte: its UTF-8 bytes are the string `"ö"`.

`str.isDigit`, `isHexDigit`, `isLetter`, `isAlnum`, `isUpper`, `isLower` and `isPunct` say what kind of ASCII character a code point is, and `isSpace` knows Unicode's spaces as well. `str.toUpper` and `str.toLower` change one ASCII letter.

## String

A `String` owns its bytes. It can grow, and it must be freed when it is no longer needed. `.copy()` makes a `String` from a `str`, and `append` adds to the end:

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

`scoped` frees it when the block ends, so the `free` can't be forgotten, whatever way the block is left:

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

A `String` can be given wherever a `str` is wanted: to a function, to `==`, to slicing. The `str` points into the `String`'s bytes, so it is valid while the `String` is alive and doesn't grow.

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

A function that gives back a `String` gives the caller a new one to free. `upper`, `replace` and `format` are such functions.

`insert` puts text in at a byte, `truncate` keeps the first bytes, `clear` empties a `String` and keeps its memory, and `reserve` makes room for more bytes, so that appending up to that many doesn't grow it again:

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

A place outside the `String` stops the program with a message that says where.

## Writing text in the program

A literal is written in double quotes. A backslash starts an escape:

| Escape | Is |
|---|---|
| `\n` | a newline |
| `\t` | a tab |
| `\r` | a carriage return |
| `\0` | a zero byte |
| `\\` | a backslash |
| `\"` | a double quote |
| `\x41` | the byte written in two hex digits |

`{` and `}` put a value into the text (see the next chapter), so a brace on its own is written twice: `{{` and `}}`.

```iris run
void main() {
    print("200\tOK, \"quoted\", C:\\logs, \x41, {{\"ok\": true}}")
}
```

```output
200	OK, "quoted", C:\logs, A, {"ok": true}
```

Three double quotes start text that spans several lines. The text starts on the line after the opening quotes and ends before the line with the closing ones. The indentation every line shares is taken off, so the text can be indented with the code around it:

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

In a raw string, `r"..."`, backslashes and braces are plain characters. It suits Windows paths and regular expressions:

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

`r"""..."""` is raw text over several lines, which also takes double quotes as they are. It suits JSON and other text full of quotes and braces:

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

## Methods of text

A `str` and a `String` have methods that look at the text and make new text. Those that give a `str` give part of the text they were given, so nothing is copied:

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

The methods are the functions of the `str` module, with the text as the first argument: `h.trim()` is `str.trim(h)`. They need no import. The module's functions, called as `str.trim(h)`, need `import str`, and so does `str.join`, which takes a slice of parts.

`split` cuts text at each separator and `fields` at each run of spaces, tabs and newlines. Both give a `List` of `str`s that point into the text. `str.join` puts parts together:

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

The methods that change text give a new `String`:

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

More of the same kind:

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

`splitN` stops after a number of parts, and the last holds the rest. `lines` cuts at `\n` and `\r\n`, with no empty line for a line break at the very end. `padLeft` and `padRight` count characters, not bytes, and `reverse` turns the characters around.

## Reading numbers from text

`parseInt` and `parseFloat` read a number and give an error when the text isn't one (errors are covered in [Errors](18-errors.html)):

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

`parseInt` takes an optional sign and decimal digits; a number too large for an `i64` is `ParseError.tooBig`. `parseIntBase` reads other bases, from 2 to 36: `"ff".parseIntBase(16)` is 255. To write a number in another base, see [Values in text](07-values-in-text.html).
