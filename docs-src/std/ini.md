## Reading

`ini.parse(text)` reads INI text into an `ini.File`: its sections in order, each with its keys and values. `get(section, key)` gives a value, `""` when it isn't there, and `lookup` says whether it is. Keys before the first `[section]` are in the section `""`. Lines that start with `;` or `#` are comments, and so is the rest of a line after a ` ;` or ` #`. A value may be in quotes, which keep its spaces and comment characters:

```iris run
import ini

!void main() {
    text := r"""
        ; the shop's settings
        name = shop

        [database]
        host = db.lan
        port: 5432
        user = "ada lovelace"   ; who logs in
        """
    scoped ini.File f = try ini.parse(text)
    print("{f.get("", "name")}: {f.get("database", "host")}:{f.get("database", "port")} as [{f.get("database", "user")}]")
    _, found := f.lookup("database", "password")
    print("password set: {found}")
    for s in f.sections {
        print("[{s.name}] {s.keys.len} keys")
    }
}
```

```output
shop: db.lan:5432 as [ada lovelace]
password set: false
[] 1 keys
[database] 3 keys
```

A key given twice keeps the last value. A line that is not a section, a `key = value`, a `key: value` or a comment fails with `IniError.syntax` and its line number.

## Writing

`set(section, key, value)` sets a value, adding the section or the key when they aren't there, and `remove` takes a key out. `write` gives the file as text, a blank line before each section, with the comments left out:

```iris run
import ini

!void main() {
    scoped ini.File f = try ini.parse("[database]\nhost = db.lan\nport = 5432\n")
    f.set("database", "port", "6543")
    f.set("cache", "size", "64MiB")
    scoped String text = f.write()
    print(text)
}
```

```output
[database]
host = db.lan
port = 6543

[cache]
size = 64MiB

```
