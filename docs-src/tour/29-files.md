# Files

The `fs` module reads and writes files and folders, and the `path` module joins paths and takes them apart. Paths are `str`s, written with `/`.

## Whole files

`fs.readAll(path)` gives a file's contents as a new `String`. `fs.writeAll(path, text)` makes the file, or empties it when it is there, and writes the text. `fs.append(path, text)` adds to the end, making the file when it isn't there:

```iris run
import fs

!void main() {
    path := "/tmp/iris-tour-proxy.conf"
    try fs.writeAll(path, "listen 8080\n")
    try fs.append(path, "upstream 10.0.0.11:9000\n")
    scoped String text = try fs.readAll(path)
    print("{text.len} bytes")
    print(text)
    try fs.remove(path)
}
```

```output
36 bytes
listen 8080
upstream 10.0.0.11:9000

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
    print("{fs.exists("/tmp/iris-tour-missing.conf")}")
    text := fs.readAll("/tmp/iris-tour-missing.conf") catch e {
        if e.code == fs.FsError.notFound {
            print("{e.code}: {e.message}")
        }
        "listen 8080".copy()
    }
    print(text)
    text.free()
}
```

```output
false
FsError.notFound: /tmp/iris-tour-missing.conf: no such file
listen 8080
```

## Line by line

`fs.open(path)` opens a file for reading and gives an `fs.File`. `File.readLine()` gives the next line as a new `String`, without its line break, and fails with `FsError.end` after the last one. The file is read in blocks, so a file of any size takes little memory:

```iris run
import fs

!void main() {
    path := "/tmp/iris-tour-hosts"
    try fs.writeAll(path, "127.0.0.1 localhost\r\n10.0.0.5 db\n10.0.0.6 cache")
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
1: 127.0.0.1 localhost
2: 10.0.0.5 db
3: 10.0.0.6 cache
```

Any other error, like a disk that fails while reading, goes on to the caller with `return e` (see [Errors](18-errors.html)). A `\r` before the line break is taken off too, and the last line counts even without a line break after it. `File.readAll()` gives the rest of an open file.

## Writing a part at a time

`fs.create(path)` opens a file for writing, making it or emptying it, and `fs.openAppend(path)` opens one for writing at its end. `File.write(text)` writes, and `File.close()` closes the file:

```iris run
import fs

!void main() {
    path := "/tmp/iris-tour-access.log"
    f := try fs.create(path)
    for i in 1..4 {
        try f.write("request {i} 200\n")
    }
    f.close()
    g := try fs.openAppend(path)
    try g.write("request 4 502\n")
    g.close()
    scoped String text = try fs.readAll(path)
    print(text)
    try fs.remove(path)
}
```

```output
request 1 200
request 2 200
request 3 200
request 4 502

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
    path := "/tmp/iris-tour-proxy.pid"
    try fs.writeAll(path, "4242\n")
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
    // a log rotated by hand: access.log becomes access.log.1
    try fs.writeAll("/tmp/iris-tour-access.log", "request 1 200\n")
    try fs.rename("/tmp/iris-tour-access.log", "/tmp/iris-tour-access.log.1")
    print("{fs.exists("/tmp/iris-tour-access.log")} {fs.exists("/tmp/iris-tour-access.log.1")}")
    try fs.remove("/tmp/iris-tour-access.log.1")
    try fs.removeAll("/tmp/iris-tour-nothing-here")
    fs.remove("/tmp/iris-tour-access.log.1") catch e {
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

`fs.walk(root)` gives the path of every file under a folder, in the folders under it too, as a `List` of `String`s. Each folder's names come in order, and a folder's files follow it. `freeEach` frees the paths and the list:

```iris run
import fs

!void main() {
    root := "/tmp/iris-tour-walk"
    try fs.removeAll(root)
    try fs.mkdirAll("/tmp/iris-tour-walk/src/net")
    try fs.writeAll("/tmp/iris-tour-walk/README", "read me\n")
    try fs.writeAll("/tmp/iris-tour-walk/src/main.iris", "void main() {{}}\n")
    try fs.writeAll("/tmp/iris-tour-walk/src/net/tcp.iris", "// tcp\n")
    files := try fs.walk(root)
    defer files.freeEach()
    i64 total = 0
    for f in files {
        info := try fs.stat(f)
        print("{f} {info.size}")
        total += info.size
    }
    print("{total} bytes")
    try fs.removeAll(root)
}
```

```output
/tmp/iris-tour-walk/README 8
/tmp/iris-tour-walk/src/main.iris 15
/tmp/iris-tour-walk/src/net/tcp.iris 7
30 bytes
```

A link is listed as a file and not followed, so a link to a folder above can't make the walk go round.

## Finding files by name

`fs.glob(pattern)` gives the paths of the files that match a pattern, sorted. In each part of the pattern, `*` matches any run of characters, `?` one character, and `[abc]` one of those characters; `[a-z]` is a range and `[!abc]` any character but those. A part that is `**` matches any number of folders, none too. A name that starts with a dot is matched only by a pattern that starts with one:

```iris run
import fs

!void main() {
    root := "/tmp/iris-tour-glob"
    try fs.removeAll(root)
    try fs.mkdirAll("/tmp/iris-tour-glob/src/net")
    for name in ["src/main.iris", "src/net/tcp.iris", "src/net/udp.iris", "notes.txt"] {
        scoped String p = format("{root}/{name}")
        try fs.writeAll(p, "")
    }
    sources := try fs.glob("/tmp/iris-tour-glob/**/*.iris")
    defer sources.freeEach()
    for f in sources {
        print(f)
    }
    print("{fs.matches("[tu]?p.iris", "udp.iris")} {fs.matches("*.iris", "notes.txt")}")
    try fs.removeAll(root)
}
```

```output
/tmp/iris-tour-glob/src/main.iris
/tmp/iris-tour-glob/src/net/tcp.iris
/tmp/iris-tour-glob/src/net/udp.iris
true false
```

`fs.matches(pattern, name)` says whether one name matches one part of a pattern. A pattern that starts with a wildcard is taken from the working folder, and gives paths without `./` in front.

## Temporary files and copies

`fs.tempDir(prefix)` makes a new folder with a name no other has, the prefix and six random characters, and gives its path; `fs.tempFile(prefix)` does the same for an empty file. They go in `$TMPDIR`, or `/tmp`, or in the folder given as `dir`. `fs.copy(from, to)` copies a file, with its permissions, and replaces what is at `to`:

```iris run
import fs, path, str

!void main() {
    scoped String work = try fs.tempDir("deploy")
    print("{str.startsWith(path.base(work), "deploy-")} {path.base(work).len}")
    scoped String conf = try fs.tempFile("proxy", dir: work)
    try fs.writeAll(conf, "listen 8080\n")
    scoped String backup = path.join(work, "proxy.conf.bak")
    try fs.copy(conf, backup)
    scoped String text = try fs.readAll(backup)
    print(text)
    try fs.removeAll(work)
}
```

```output
true 13
listen 8080

```

The program removes them when it is done; `removeAll` takes a folder with everything in it.

## Paths

The `path` module works on the text of a path and never looks at the disk. `path.join(a, b)` gives a new `String`; the others give a part of the path they are given, as a `str`:

| Call | Gives |
|---|---|
| `path.join("/srv", "www")` | `/srv/www`, and `b` itself when `b` starts with `/` |
| `path.base("/var/log/proxy/access.log")` | `access.log` |
| `path.dir("/var/log/proxy/access.log")` | `/var/log/proxy`, and `.` when there is no folder |
| `path.ext("access.log.gz")` | `.gz`, and `""` when there is none |
| `path.stem("access.log")` | `access` |
| `path.isAbs("/etc")` | `true` |
| `path.split("/srv/www/index.html")` | `/srv/www/` and `index.html` |
| `path.clean("/srv//www/../logs/")` | `/srv/logs` |
| `path.rel("/srv/www", "/srv/logs/a.log")` | `../logs/a.log` |

```iris run
import path

void main() {
    p := "/var/log/proxy/access.log.gz"
    print("{path.dir(p)} | {path.base(p)} | {path.stem(p)} | {path.ext(p)}")
    print("{path.dir("proxy.conf")} | {path.ext(".bashrc")} | {path.base("/srv/www/")}")
    scoped String full = path.join("/srv/www", "index.html")
    scoped String abs = path.join("/srv/www", "/etc/hosts")
    print("{full} {abs} {path.isAbs(full)} {path.isAbs("a/b")}")
}
```

```output
/var/log/proxy | access.log.gz | access.log | .gz
. |  | www
/srv/www/index.html /etc/hosts true false
```

A name that starts with a dot, like `.bashrc`, has no extension. A `/` at the end of a path is ignored.

`path.split(p)` gives the folder and the name. The folder keeps its `/`, so the two put together are the path again, and a path without a `/` is all name. `path.clean(p)` gives a new `String` with the shortest path to the same place: no `.` parts, no doubled `/` and no `/` at the end, and each `..` takes out the part before it. `path.rel(base, target)` gives the path that leads from `base` to `target`, so that joining `base` and it gives `target`; it fails with `path.PathError.notRelative` when one of them starts at `/` and the other doesn't:

```iris run
import path

!void main() {
    dir, name := path.split("/srv/www/index.html")
    print("{dir} {name}")
    scoped String c = path.clean("/srv//www/./../logs/")
    scoped String up = path.clean("../a/../..")
    print("{c} {up}")
    scoped String r = try path.rel("/srv/www", "/srv/logs/access.log")
    scoped String same = try path.rel("/srv", "/srv/")
    print("{r} {same}")
    _ = path.rel("srv", "/srv") catch e {
        print("{e.code}")
        "".copy()
    }
}
```

```output
/srv/www/ index.html
/srv/logs ../..
../logs/access.log .
PathError.notRelative
```

## The working folder

A path that doesn't start with `/` is taken from the working folder. `os.cwd()` gives it and `os.chdir(path)` changes it, for the whole program. `os.home()` gives the user's home folder, `os.hostname()` the name of the machine, and `os.pid()` the program's process id:

```iris run
import fs, os

!void main() {
    scoped String start = try os.cwd()
    try os.chdir("/tmp")
    try fs.writeAll("iris-tour-here.txt", "here")
    print("{fs.exists("/tmp/iris-tour-here.txt")}")
    try fs.remove("iris-tour-here.txt")
    try os.chdir(start)
    scoped String home = try os.home()
    scoped String host = try os.hostname()
    print("{home.len > 0} {host.len > 0} {os.pid() > 0}")
    os.chdir("/tmp/iris-tour-no-such-folder") catch e {
        print("{e.code}")
    }
}
```

```output
true
true true true
OsError.notFound
```

`os.home()` reads `$HOME`, and asks the system for the user's folder when it isn't set.
