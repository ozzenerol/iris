# Files

The `fs` module reads and writes files and folders, and the `path` module joins paths and takes them apart. Paths are `str`s, written with `/`.

## Whole files

`fs.readAll(path)` gives a file's contents as a new `String`. `fs.writeAll(path, text)` makes the file, or empties it when it is there, and writes the text. `fs.append(path, text)` adds to the end, making the file when it isn't there:

```iris run
import fs

!void main() {
    path := "/tmp/iris-tour-whole.txt"
    try fs.writeAll(path, "first\n")
    try fs.append(path, "second\n")
    scoped String text = try fs.readAll(path)
    print("{text.len} bytes")
    print(text)
    try fs.remove(path)
}
```

```output
13 bytes
first
second

```

Reading a whole file suits files that fit in memory: settings, templates, small data files. A big file is read a line or a block at a time.

## When a file isn't there

Every call that can fail gives an error of `fs.FsError`, with the path in its message:

| Code | When |
|---|---|
| `notFound` | nothing is at the path |
| `permission` | the program may not read or write it |
| `exists` | something is at the path already |
| `isDir` | the path is a folder where a file is wanted |
| `end` | there is nothing more to read |
| `other` | anything else |

`fs.exists(path)` says whether something is there, without an error:

```iris run
import fs

void main() {
    print("{fs.exists("/tmp/iris-tour-missing.txt")}")
    text := fs.readAll("/tmp/iris-tour-missing.txt") catch e {
        if e.code == fs.FsError.notFound {
            print("{e.code}: {e.message}")
        }
        "default".copy()
    }
    print(text)
    text.free()
}
```

```output
false
FsError.notFound: /tmp/iris-tour-missing.txt: no such file
default
```

## Line by line

`fs.open(path)` opens a file for reading and gives an `fs.File`. `File.readLine()` gives the next line as a new `String`, without its line break, and fails with `FsError.end` after the last one. The file is read in blocks, so a file of any size takes little memory:

```iris run
import fs

!void main() {
    path := "/tmp/iris-tour-lines.txt"
    try fs.writeAll(path, "ada 36\r\nzoe 41\nmax 29")
    f := try fs.open(path)
    defer f.close()
    i64 n = 0
    for {
        line := f.readLine() catch e {
            if e.code == fs.FsError.end {
                break
            }
            return e
        }
        n += 1
        print("{n}: {line}")
        line.free()
    }
    try fs.remove(path)
}
```

```output
1: ada 36
2: zoe 41
3: max 29
```

Any other error, like a disk that fails while reading, goes on to the caller with `return e` (see [Errors](18-errors.html)). A `\r` before the line break is taken off too, and the last line counts even without a line break after it. `File.readAll()` gives the rest of an open file.

## Writing a part at a time

`fs.create(path)` opens a file for writing, making it or emptying it, and `fs.openAppend(path)` opens one for writing at its end. `File.write(text)` writes, and `File.close()` closes the file:

```iris run
import fs

!void main() {
    path := "/tmp/iris-tour-log.txt"
    f := try fs.create(path)
    for i in 1..4 {
        try f.write("line {i}\n")
    }
    f.close()
    g := try fs.openAppend(path)
    try g.write("one more\n")
    g.close()
    scoped String text = try fs.readAll(path)
    print(text)
    try fs.remove(path)
}
```

```output
line 1
line 2
line 3
one more

```

## Bytes

`File.read(buf)` reads into a `u8[]` and gives how many bytes it put there, `0` at the end of the file. It is how a file that isn't text is read:

```iris run
import fs

!void main() {
    path := "/tmp/iris-tour-bytes.bin"
    try fs.writeAll(path, "\x89PNG and the rest")
    f := try fs.open(path)
    defer f.close()
    u8[4] buf
    n := try f.read(buf[:])
    print("{n} bytes: {buf[0]} {buf[1]} {buf[2]} {buf[3]}")
    i64 total = n
    for {
        m := try f.read(buf[:])
        if m == 0 {
            break
        }
        total += m
    }
    print("{total} bytes in all")
    try fs.remove(path)
}
```

```output
4 bytes: 137 80 78 71
17 bytes in all
```

## Information about a file

`fs.stat(path)` gives an `fs.Info`: the size in bytes, whether it is a folder, and when it last changed, as a `time.Time`:

```iris run
import fs

!void main() {
    path := "/tmp/iris-tour-stat.txt"
    try fs.writeAll(path, "hello")
    info := try fs.stat(path)
    print("{info.size} bytes, a folder: {info.isDir}")
    dir := try fs.stat("/tmp")
    print("/tmp is a folder: {dir.isDir}")
    try fs.remove(path)
}
```

```output
5 bytes, a folder: false
/tmp is a folder: true
```

## Moving and removing

`fs.rename(from, to)` moves or renames a file or folder. `fs.remove(path)` removes a file or an empty folder, and `fs.removeAll(path)` removes a folder with everything in it; nothing at the path is no error for `removeAll`:

```iris run
import fs

!void main() {
    try fs.writeAll("/tmp/iris-tour-old.txt", "x")
    try fs.rename("/tmp/iris-tour-old.txt", "/tmp/iris-tour-new.txt")
    print("{fs.exists("/tmp/iris-tour-old.txt")} {fs.exists("/tmp/iris-tour-new.txt")}")
    try fs.remove("/tmp/iris-tour-new.txt")
    try fs.removeAll("/tmp/iris-tour-nothing-here")
    fs.remove("/tmp/iris-tour-new.txt") catch e {
        print("{e.code}")
    }
}
```

```output
false true
FsError.notFound
```

## Folders

`fs.mkdir(path)` makes a folder, and fails with `FsError.exists` when it is there. `fs.mkdirAll(path)` also makes the folders above it that are missing, and a folder that is there already is no error. `fs.readDir(path)` gives an `fs.Dir`, whose `entries` are the names in the folder, sorted, without `.` and `..`, each with whether it is a folder. `Dir.free()` gives the names back, so `scoped` works on it:

```iris run
import fs

!void main() {
    root := "/tmp/iris-tour-dir"
    try fs.removeAll(root)
    try fs.mkdirAll("/tmp/iris-tour-dir/logs/old")
    try fs.writeAll("/tmp/iris-tour-dir/b.txt", "b")
    try fs.writeAll("/tmp/iris-tour-dir/a.txt", "a")
    scoped dir := try fs.readDir(root)
    for e in dir.entries {
        kind := if e.isDir { "folder" } else { "file" }
        print("{e.name} ({kind})")
    }
    fs.mkdir(root) catch e {
        print("{e.code}")
    }
    try fs.removeAll(root)
}
```

```output
a.txt (file)
b.txt (file)
logs (folder)
FsError.exists
```

## Walking a tree

There is no call that walks a whole tree: a function that lists a folder and calls itself for each folder in it does it. `path.join` makes each child's path:

```iris run
import fs, path

!i64 walk(str dir, i64 depth) {
    scoped d := try fs.readDir(dir)
    scoped String indent
    for _ in 0..depth {
        indent.append("  ")
    }
    i64 total = 0
    for e in d.entries {
        scoped String p = path.join(dir, e.name)
        if e.isDir {
            print("{indent}{e.name}/")
            total += try walk(p, depth + 1)
        } else {
            info := try fs.stat(p)
            print("{indent}{e.name} {info.size}")
            total += info.size
        }
    }
    return total
}

!void main() {
    root := "/tmp/iris-tour-walk"
    try fs.removeAll(root)
    try fs.mkdirAll("/tmp/iris-tour-walk/src/net")
    try fs.writeAll("/tmp/iris-tour-walk/README", "read me\n")
    try fs.writeAll("/tmp/iris-tour-walk/src/main.iris", "void main() {{}}\n")
    try fs.writeAll("/tmp/iris-tour-walk/src/net/tcp.iris", "// tcp\n")
    total := try walk(root, 0)
    print("{total} bytes")
    try fs.removeAll(root)
}
```

```output
README 8
src/
  main.iris 15
  net/
    tcp.iris 7
30 bytes
```

## Paths

The `path` module works on the text of a path and never looks at the disk. `path.join(a, b)` gives a new `String`; the others give a part of the path they are given, as a `str`:

| Call | Gives |
|---|---|
| `path.join("/srv", "data.db")` | `/srv/data.db`, and `b` itself when `b` starts with `/` |
| `path.base("/home/ada/notes.txt")` | `notes.txt` |
| `path.dir("/home/ada/notes.txt")` | `/home/ada`, and `.` when there is no folder |
| `path.ext("notes.tar.gz")` | `.gz`, and `""` when there is none |
| `path.stem("notes.txt")` | `notes` |
| `path.isAbs("/etc")` | `true` |

```iris run
import path

void main() {
    p := "/home/ada/photos/cat.jpeg"
    print("{path.dir(p)} | {path.base(p)} | {path.stem(p)} | {path.ext(p)}")
    print("{path.dir("notes.txt")} | {path.ext(".bashrc")} | {path.base("/srv/www/")}")
    scoped String full = path.join("/srv/www", "index.html")
    scoped String abs = path.join("/srv/www", "/etc/hosts")
    print("{full} {abs} {path.isAbs(full)} {path.isAbs("a/b")}")
}
```

```output
/home/ada/photos | cat.jpeg | cat | .jpeg
. |  | www
/srv/www/index.html /etc/hosts true false
```

A name that starts with a dot, like `.bashrc`, has no extension. A `/` at the end of a path is ignored.
