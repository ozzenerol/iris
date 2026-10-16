# Text

Iris has two types for text: `str` for text you look at and `String` for text you own.

## str

`str` is a slice of bytes (`u8[]`). It points at text stored somewhere else and knows its length. A string literal is a `str`.

```iris
str greeting = "hello, world"
print("{greeting.len}")        // 12
```

Slicing a `str` gives another `str` that points into the same text. Nothing is copied.

```iris
str word = greeting[0:5]       // "hello"
str rest = greeting[7:]        // "world"
```

`==` compares the bytes of two strings:

```iris
if word == "hello" { }
```

Text is stored as UTF-8. `len` counts bytes, and indexing gives a byte (`u8`).

## String

A `String` owns its bytes. It can grow, and it must be freed.

```iris
String s = "hello".copy()
s.append(", world")
print(s)
s.free()
```

`scoped` frees it at the end of the block, so the `free` can't be forgotten:

```iris
scoped String s = "hello".copy()
s.append(", world")
```

A `String` can be used wherever a `str` is expected. The `str` points into the `String`'s bytes, so it is only valid while the `String` is alive and unchanged.

## Putting values into text

A name or expression in braces inside a string literal is replaced by its value:

```iris run
void main() {
    n := 42
    addr := "10.0.0.1"
    print("got {n} bytes from {addr}")
    print("{n * 2}")
}
```

```output
got 42 bytes from 10.0.0.1
84
```

The compiler checks each value. Numbers, `bool`, `str`, `String` and enums can be printed. A struct can be printed if it has a `format` method. `{{` and `}}` write a single brace.

`format` builds a `String` the same way:

```iris
scoped String msg = format("{n} bytes")
```

Text with values can also be passed straight to a function that takes a `str`. It is built in a temporary `String`, which is freed when the statement ends:

```iris
res.text("hello {name}")
log.write("got {n} bytes from {addr}")
```

A `String` that a call gives, passed straight to a function that takes a `str`, is freed when the statement ends too:

```iris
res.json(json.encode(user))
print(json.encode(user))
```

Anywhere else, text with values needs `format`, and a `String` from a call needs a variable that frees it.

A complete program:

```iris run
void main() {
    String s = "hello".copy()
    s.append(", world")
    print(s)
    print("{s.len} bytes")
    s.free()

    host := "localhost"
    msg := format("connecting to {host}")
    print(msg)
    msg.free()
}
```

```output
hello, world
12 bytes
connecting to localhost
```

## Escapes

| Escape | Meaning |
|---|---|
| `\n` | newline |
| `\t` | tab |
| `\r` | carriage return |
| `\\` | backslash |
| `\"` | double quote |
| `\x41` | a byte in hex |

## Multi-line text

Three quotes start and end text that spans several lines. The indentation that all lines share is removed, so the text can be indented with the code:

```iris
q := """
    SELECT name
    FROM users
    WHERE id = {id}
    """
```

## Raw text

In a raw string, `r"..."`, backslashes and braces are plain characters:

```iris
path := r"C:\tools\iris"
pattern := r"\d+{2}"
```

`r"""..."""` is raw text that can also hold quotes and span several lines. The indentation all lines share is removed, as with `"""`. It suits JSON and other text full of quotes and braces:

```iris
body := r"""
    {"name": "Ada", "tags": ["admin"]}
    """
```

## Characters

A character in single quotes is its byte value, a `u8`:

```iris
u8 c = 'a'
if c >= '0' && c <= '9' { }
```
