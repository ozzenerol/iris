# Text

Iris has two types for text. A `str` looks at text that is stored somewhere else; a `String` owns its text and can grow. This chapter covers both, the ways to write text in the program, and the functions that work on it.

## str

A `str` is a slice of bytes: a pointer to some text and its length. A string literal is a `str` that points into the program itself:

```iris run
void main() {
    greeting := "hello, world"
    print("{greeting.len} bytes")
    print("first byte {greeting[0]}, last {greeting[greeting.len - 1]}")
}
```

```output
12 bytes
first byte 104, last 100
```

Indexing gives a byte, a `u8`. Every index is checked, so `greeting[12]` stops a debug build with a message that says where.

A slice of a `str`, `s[from:to]`, is another `str` that points into the same text. Nothing is copied, so slicing costs nothing whatever the length. Either end can be left out:

```iris run
void main() {
    s := "hello, world"
    print("[{s[0:5]}] [{s[7:]}] [{s[:5]}] [{s[3:3]}]")
}
```

```output
[hello] [world] [hello] []
```

`==` and `!=` compare the bytes of two strings. `str.compare` puts them in order, giving a number below zero, zero or above zero:

```iris run
import str

void main() {
    a := "apple"
    print("{a == "apple"} {a != "pear"}")
    print("{str.compare("apple", "banana")} {str.compare("b", "a")} {str.compare("x", "x")}")
}
```

```output
true true
-1 1 0
```

## UTF-8

Text is stored as UTF-8. Letters outside ASCII take more than one byte, so `len` counts bytes, not letters, and an index can land inside a letter:

```iris run
void main() {
    word := "café"
    print("{word.len} bytes")
    for i, b in word {
        print("{i}: {b}")
    }
}
```

```output
5 bytes
0: 99
1: 97
2: 102
3: 195
4: 169
```

`é` is the two bytes 195 and 169. Searching, cutting and comparing work on bytes, which is correct for UTF-8 text: a search for `"é"` finds it, since no letter's bytes appear inside another's.

## String

A `String` owns its bytes. It can grow, and it must be freed when it is no longer needed. `.copy()` makes a `String` from a `str`, and `append` adds to the end:

```iris run
void main() {
    String s = "hello".copy()
    s.append(", ")
    s.append("world")
    print(s)
    print("{s.len} bytes")
    s.free()
}
```

```output
hello, world
12 bytes
```

`scoped` frees it when the block ends, so the `free` can't be forgotten, whatever way the block is left:

```iris run
void main() {
    scoped String path = "/usr".copy()
    path.append("/local")
    print(path)
}
```

```output
/usr/local
```

A `String` can be given wherever a `str` is wanted: to a function, to `==`, to slicing. The `str` points into the `String`'s bytes, so it is valid while the `String` is alive and doesn't grow.

```iris run
import str

void main() {
    scoped String name = "ada lovelace".copy()
    str first = name[0:3]
    print("{first} {str.contains(name, "love")} {name == "ada lovelace"}")
}
```

```output
ada true true
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
    print("a\ttab, \"quotes\", a back\\slash, \x41, {{braces}}")
}
```

```output
a	tab, "quotes", a back\slash, A, {braces}
```

Three double quotes start text that spans several lines. The text starts on the line after the opening quotes and ends before the line with the closing ones. The indentation every line shares is taken off, so the text can be indented with the code around it:

```iris run
void main() {
    query := """
        SELECT name
        FROM users
          WHERE active
        """
    print(query)
}
```

```output
SELECT name
FROM users
  WHERE active
```

In a raw string, `r"..."`, backslashes and braces are plain characters. It suits Windows paths and regular expressions:

```iris run
void main() {
    print(r"C:\tools\iris {not a value}")
    print(r"\d+\.\d+")
}
```

```output
C:\tools\iris {not a value}
\d+\.\d+
```

`r"""..."""` is raw text over several lines, which also takes double quotes as they are. It suits JSON and other text full of quotes and braces:

```iris run
void main() {
    body := r"""
        {"name": "Ada", "tags": ["admin"]}
        """
    print(body)
}
```

```output
{"name": "Ada", "tags": ["admin"]}
```

## The str module

The `str` module has the functions that look at text and make new text. Those that give a `str` give part of the text they were given, so nothing is copied:

```iris run
import str

void main() {
    s := "  key = value  "
    t := str.trim(s)
    print("[{t}] [{str.trimLeft(s)}] [{str.trimRight(s)}]")
    key, value, found := str.cut(t, " = ")
    print("{key} -> {value} ({found})")
    print("{str.find(t, "=")} {str.findLast("a/b/c", "/")} {str.contains(t, "val")}")
    print("{str.startsWith(t, "key")} {str.endsWith(t, "lue")} {str.count("banana", "an")}")
}
```

```output
[key = value] [key = value  ] [  key = value]
key -> value (true)
4 3 true
true true 2
```

`split` cuts text at each separator and `fields` at each run of spaces, tabs and newlines. Both give a `List` of `str`s that point into the text. `join` puts parts together:

```iris run
import str

void main() {
    scoped parts := str.split("a,b,,c", ",")
    print("{parts.len} parts, the third is [{parts.at(2)}]")
    scoped words := str.fields("  the quick\tbrown\n fox ")
    scoped String joined = str.join(words.items(), "-")
    print(joined)
}
```

```output
4 parts, the third is []
the-quick-brown-fox
```

The functions that change text give a new `String`:

```iris run
import str

void main() {
    scoped String a = str.upper("Iris")
    scoped String b = str.lower("Iris")
    scoped String c = str.replace("one two two", "two", "2")
    scoped String d = str.repeat("ab", 3)
    print("{a} {b} {c} {d}")
}
```

```output
IRIS iris one 2 2 ababab
```

`upper` and `lower` change ASCII letters only.

## Reading numbers from text

`str.parseInt` and `str.parseFloat` read a number and give an error when the text isn't one (errors are covered in [Errors](18-errors.html)):

```iris run
import str

void main() {
    n := str.parseInt("-42") catch 0
    x := str.parseFloat("2.5e3") catch 0.0
    bad := str.parseInt("12ab") catch -1
    print("{n} {x} {bad}")
    _ = str.parseInt("") catch e {
        print("{e.code}: {e.message}")
        0
    }
}
```

```output
-42 2500 -1
ParseError.empty: no number in an empty text
```

`parseInt` takes an optional sign and decimal digits; a number too large for an `i64` is `ParseError.tooBig`.
