# Text

Iris has two types for text. A `str` looks at text that is stored somewhere else; a `String` owns its text and can grow. This chapter covers both, the ways to write text in the program, and the functions that work on it.

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

`==` and `!=` compare the bytes of two strings. `str.compare` puts them in order, giving a number below zero, zero or above zero:

```iris run
import str

void main() {
    method := "POST"
    print("{method == "POST"} {method != "GET"}")
    print("{str.compare("api.lan", "db.lan")} {str.compare("web2", "web1")} {str.compare("x", "x")}")
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

`ö` is the two bytes 195 and 182. Searching, cutting and comparing work on bytes, which is correct for UTF-8 text: a search for `"ö"` finds it, since no letter's bytes appear inside another's.

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
import str

void main() {
    scoped String url = "https://api.example.com/v1".copy()
    str scheme = url[0:5]
    print("{scheme} {str.contains(url, "example")} {url == "https://api.example.com/v1"}")
}
```

```output
https true true
```

A function that gives back a `String` gives the caller a new one to free. `str.upper`, `str.replace` and `format` are such functions.

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

## The str module

The `str` module has the functions that look at text and make new text. Those that give a `str` give part of the text they were given, so nothing is copied:

```iris run
import str

void main() {
    raw := "  Content-Type: text/html  "
    h := str.trim(raw)
    print("[{h}] [{str.trimLeft(raw)}] [{str.trimRight(raw)}]")
    name, value, found := str.cut(h, ": ")
    print("{name} -> {value} ({found})")
    print("{str.find(h, ":")} {str.findLast("/api/v1/users", "/")} {str.contains(h, "html")}")
    print("{str.startsWith(h, "Content-")} {str.endsWith(h, "/html")} {str.count("10.0.0.1", ".")}")
}
```

```output
[Content-Type: text/html] [Content-Type: text/html  ] [  Content-Type: text/html]
Content-Type -> text/html (true)
12 7 true
true true 3
```

`split` cuts text at each separator and `fields` at each run of spaces, tabs and newlines. Both give a `List` of `str`s that point into the text. `join` puts parts together:

```iris run
import str

void main() {
    scoped parts := str.split("GET,/health,,200", ",")
    print("{parts.len} parts, the third is [{parts.at(2)}]")
    scoped cols := str.fields("tcp   0  0 0.0.0.0:22\tLISTEN\n")
    scoped String joined = str.join(cols.items(), " | ")
    print(joined)
}
```

```output
4 parts, the third is []
tcp | 0 | 0 | 0.0.0.0:22 | LISTEN
```

The functions that change text give a new `String`:

```iris run
import str

void main() {
    scoped String a = str.upper("get")
    scoped String b = str.lower("Content-Length")
    scoped String c = str.replace("/api/v1/users/v1", "v1", "v2")
    scoped String d = str.repeat("-", 12)
    print("{a} {b} {c}")
    print(d)
}
```

```output
GET content-length /api/v2/users/v2
------------
```

`upper` and `lower` change ASCII letters only.

## Reading numbers from text

`str.parseInt` and `str.parseFloat` read a number and give an error when the text isn't one (errors are covered in [Errors](18-errors.html)):

```iris run
import str

void main() {
    port := str.parseInt("8080") catch 0
    loss := str.parseFloat("0.25") catch 0.0
    bad := str.parseInt("80a") catch -1
    print("{port} {loss} {bad}")

    _ = str.parseInt("") catch e {
        print("{e.code}: {e.message}")
        0
    }
}
```

```output
8080 0.25 -1
ParseError.empty: no number in an empty text
```

`parseInt` takes an optional sign and decimal digits; a number too large for an `i64` is `ParseError.tooBig`.
