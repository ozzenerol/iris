# Regular expressions

The `regex` module finds patterns in text. It uses the RE2 syntax: `\d+` is one or more digits, `(\w+)` a group of word characters, `a|b` either one. A search takes time in proportion to the length of the text, whatever the pattern, so a pattern from a user can't make a program hang.

When a fixed separator cuts text into parts, the methods in [Text](../tour/06-text.html#methods-of-text) are simpler and faster. Use a regular expression for text whose shape varies.

## Compiling a pattern

`regex.compile(pattern)` turns a pattern into a `Regex`, or fails with `RegexError.syntax` when the pattern isn't valid. A `Regex` holds memory, so `free` it when done. Write patterns as raw strings (`r"..."`), which keep backslashes as they are.

```iris run
import regex

!void main() {
    re := try regex.compile(r"^\d{1,3}(\.\d{1,3}){3}$")
    defer re.free()

    for addr in ["10.0.0.1", "10.0.0", "192.168.1.250", "host.lan"] {
        print("{addr}: {re.matches(addr)}")
    }
}
```

```output
10.0.0.1: true
10.0.0: false
192.168.1.250: true
host.lan: false
```

`matches` says whether the pattern matches anywhere in the text. `^` and `$` anchor it to the start and the end, so here the whole text must be an address.

A pattern written in the program is known to be valid. `regex.must` compiles it and panics if it isn't, so there is no error to handle:

```iris run
import regex

void main() {
    hex := regex.must(r"^[0-9a-f]+$")
    defer hex.free()
    print("{hex.matches("deadbeef")} {hex.matches("xyz")}")
}
```

```output
true false
```

Pass a pattern from a user or a file to `compile`. The error says what is wrong and at which byte:

```iris run
import regex

void main() {
    for p in [r"(\w+", r"a{2,1}", r"[z-a]"] {
        _ = regex.compile(p) catch e {
            print("{e.code}: {e.message}")
            regex.must("")
        }
    }
}
```

```output
RegexError.syntax: (\w+: a ( with no ) at 4
RegexError.syntax: a{2,1}: a repeat count over 1000, or the larger first at 6
RegexError.syntax: [z-a]: a range that ends before it starts at 4
```

## Finding a match

`find` gives the first match and whether there was one. On the `Match`:

- `str` gives the matched text.
- `start` and `end` give its place.
- `group(i)` gives a group. Group 0 is the whole match and group 1 the first `(...)`.

A match holds the places of its groups, so `free` it after use.

```iris run
import regex

void main() {
    re := regex.must(r"(\d+) ms")
    defer re.free()

    m, found := re.find("GET /health 200 in 14 ms")
    if found {
        print("[{m.str()}] at {m.start}..{m.end}, the number is {m.group(1)}")
    }
    m.free()

    m2, found2 := re.find("GET /health 200")
    print("found: {found2}")
    m2.free()
}
```

```output
[14 ms] at 19..24, the number is 14
found: false
```

`str` and `group` return slices of the searched text, so that text must stay alive while the match is used. `.copy()` keeps a part longer.

## Named groups

`(?P<name>...)` gives a group a name, and `re.index(name)` gives its number. Names keep the code readable in a pattern with many groups. This one reads the parts of an access log line:

```iris run
import regex

void main() {
    re := regex.must(r"""^(?P<ip>\S+) \S+ \S+ \[[^\]]+\] "(?P<method>[A-Z]+) (?P<path>\S+) [^"]*" (?P<status>\d{3}) (?P<bytes>\d+)""")
    defer re.free()

    ip := re.index("ip")
    method := re.index("method")
    path := re.index("path")
    status := re.index("status")

    line := r"""10.0.0.7 - - [24/Oct/2026:10:00:01 +0000] "GET /api/users HTTP/1.1" 200 5120"""
    m, found := re.find(line)
    if found {
        print("{m.group(ip)} {m.group(method)} {m.group(path)} -> {m.group(status)}")
    }
    m.free()

    print("{re.groups()} groups")
}
```

```output
10.0.0.7 GET /api/users -> 200
5 groups
```

The pattern is written as `r"""..."""` because it contains `"`. `(?:...)` groups without capturing, for `|` or a count over several characters, and takes no number.

A group that didn't take part in the match gives `""`. `span(i)` gives `-1, -1` for it, which tells it apart from a group that matched empty text.

```iris run
import regex

void main() {
    re := regex.must(r"^(\w+)(?::(\d+))?$")
    defer re.free()

    for addr in ["db:5432", "cache"] {
        m, _ := re.find(addr)
        s, e := m.span(2)
        print("host {m.group(1)}, port [{m.group(2)}] at {s}..{e}")
        m.free()
    }
}
```

```output
host db, port [5432] at 3..7
host cache, port [] at -1..-1
```

## Every match

`findAll` gives every match, left to right, in a `List`. Free each match and the list after use.

```iris run
import regex

void main() {
    re := regex.must(r"#(\w+)")
    defer re.free()

    ms := re.findAll("deploy #api and #worker, then #db")
    for m in ms {
        print(m.group(1))
        m.free()
    }
    ms.free()
}
```

```output
api
worker
db
```

A limit stops after that many matches: `re.findAll(text, 2)`. `split` cuts the text at each match and gives the parts between them.

```iris run
import regex

void main() {
    re := regex.must(r"\s*[,;]\s*")
    defer re.free()

    scoped List[str] hosts = re.split("db1, db2;db3 ,  db4")
    for h in hosts {
        print("[{h}]")
    }
}
```

```output
[db1]
[db2]
[db3]
[db4]
```

## Replacing

`replace` gives new text with each match replaced. In the replacement, `$1` is group 1, `${name}` a named group and `$$` a `$`. Write a replacement with `${name}` as a raw string so its braces aren't read as values.

```iris run
import regex

void main() {
    re := regex.must(r"(?P<user>[\w.]+)@(?P<host>[\w.]+)")
    defer re.free()
    scoped String masked = re.replace("from ada@example.com to ops@lan", r"***@${host}")
    print(masked)

    secrets := regex.must(r"(token|password)=\S+")
    defer secrets.free()
    scoped String clean = secrets.replace("login user=ada password=hunter2 token=abc123", "$1=[hidden]")
    print(clean)

    scoped String first = secrets.replace("token=a token=b", "$1=x", 1)
    print(first)
}
```

```output
from ***@example.com to ***@lan
login user=ada password=[hidden] token=[hidden]
token=x token=b
```

The last argument is the most matches to replace, here only the first.

## Flags

A flag at the start of a pattern changes how the rest matches:

- `(?i)` matches letters in either case.
- `(?m)` lets `^` and `$` match at the start and end of every line.
- `(?s)` lets `.` match a newline.

```iris run
import regex

void main() {
    level := regex.must(r"(?i)\b(error|warn)\b")
    defer level.free()
    print("{level.matches("ERROR disk full")} {level.matches("Warn: slow")} {level.matches("errors: 0")}")

    keys := regex.must(r"(?m)^(\w+)=")
    defer keys.free()

    ms := keys.findAll("host=db\nport=5432\n# a comment\nuser=app\n")
    for m in ms {
        print(m.group(1))
        m.free()
    }
    ms.free()
}
```

```output
true true false
host
port
user
```

`\b` is a word boundary, so `errors` doesn't count as `error`. `(?i:...)` sets a flag for one group only.

## Checking input

A program that checks values from users or files compiles its patterns once and keeps them. A struct holds them, and its `free` frees them all.

```iris run
import regex

struct Rules {
    regex.Regex name
    regex.Regex port
}

Rules rules() => Rules{name: regex.must(r"^[a-z][a-z0-9-]{0,62}$"), port: regex.must(r"^\d{1,5}$")}

void Rules.free() {
    self.name.free()
    self.port.free()
}

void main() {
    scoped Rules r = rules()

    for (name, port) in [("api-1", "8080"), ("Api", "80"), ("db", "54321x")] {
        if !r.name.matches(name) {
            print("{name}: a name is lower case letters, digits and -")
        } else if !r.port.matches(port) {
            print("{name}: {port} isn't a port")
        } else {
            print("{name}:{port} is fine")
        }
    }
}
```

```output
api-1:8080 is fine
Api: a name is lower case letters, digits and -
db: 54321x isn't a port
```

The [library](../std/index.html#regex) lists the whole syntax.
