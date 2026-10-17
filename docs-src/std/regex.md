## Matching and finding

`regex.compile` turns a pattern into a `Regex`, or fails with `RegexError.syntax`. A raw string (`r"..."`) keeps backslashes as they are, so patterns are easier to read. `matches` says whether the text has a match anywhere; `find` gives the first one:

```iris run
import regex

!void main() {
    re := try regex.compile(r"(\w+)@(\w+)\.com")
    defer re.free()
    print("{re.matches("write to ada@example.com")} {re.matches("no mail")}")
    m, found := re.find("write to ada@example.com today")
    if found {
        print("{m.str()} at {m.start}..{m.end}: user {m.group(1)}, host {m.group(2)}")
    }
    m.free()
}
```

```output
true false
ada@example.com at 9..24: user ada, host example
```

A `Match` gives slices of the text that was searched, so the text must stay while the match is used.

## Every match

`findAll` gives every match, left to right, and `split` the text between them. A limit stops after that many:

```iris run
import regex

void main() {
    re := regex.must(r"\d+")
    defer re.free()
    text := "3 apples, 12 pears and 7 figs"
    ms := re.findAll(text)
    for m in ms {
        print(m.str())
        m.free()
    }
    ms.free()
    parts := regex.must(r",\s*")
    defer parts.free()
    scoped List[str] words = parts.split("red, green,blue")
    print("{words.len}: {words.at(0)} {words.at(1)} {words.at(2)}")
}
```

```output
3
12
7
3: red green blue
```

`regex.must` is `compile` for a pattern written in the program: a pattern that isn't valid there is a mistake in the program, so it panics instead of giving an error.

## Replacing

`replace` gives new text with each match replaced. In the replacement, `$1` is group 1, `${name}` a named group, and `$$` a `$`. A replacement with `${name}` is written as a raw string, so its braces aren't read as values in text:

```iris run
import regex

void main() {
    re := regex.must(r"(?P<year>\d{4})-(?P<month>\d\d)-(?P<day>\d\d)")
    defer re.free()
    scoped String eu = re.replace("from 2026-10-17 to 2026-12-01", r"${day}/${month}/${year}")
    print(eu)
    print("{re.groups()} groups, month is group {re.index("month")}")
}
```

```output
from 17/10/2026 to 01/12/2026
3 groups, month is group 2
```
