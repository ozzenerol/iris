## Whole files

`readAll` gives a file's contents as a `String`. `writeAll` replaces a file's contents and makes the file when it is missing. `append` adds to the end:

```iris run
import fs

!void main() {
    path := "/tmp/iris-fs-notes.txt"
    try fs.writeAll(path, "first\n")
    try fs.append(path, "second\n")

    scoped String text = try fs.readAll(path)
    print("{text.len} bytes: {text}")

    info := try fs.stat(path)
    print("size {info.size}, folder {info.isDir}")

    try fs.remove(path)
    print("still there: {fs.exists(path)}")
}
```

```output
13 bytes: first
second

size 13, folder false
still there: false
```

## Line by line

`open` opens a file for reading. `readLine` gives one line at a time and fails with `FsError.end` at the end. This reads a big file without holding all of it:

```iris run
import fs

!void main() {
    path := "/tmp/iris-fs-lines.txt"
    try fs.writeAll(path, "ada 36\nzoe 41\n")

    f := try fs.open(path)
    defer f.close()

    i64 n = 0
    for {
        line := f.readLine() catch e {
            break
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
```

`create` opens a file for writing and empties it first. `openAppend` opens one for writing at its end. `File.write` writes to it, and `File.read` fills a buffer, for files that aren't text.

## Mapping a file into memory

`mapFile` gives a file's bytes as memory without reading them first. The system reads each part when it is first used, so a large file costs nothing until it is looked at. `free` gives the memory back.

- Changes stay in memory unless `write` is set.
- With `write`, changes go to the file, and every program that maps the same file with `write` sees them. Two processes can share memory this way.
- With `write` and a `size`, the file first grows to that size.

```iris run
import fs

!void main() {
    try fs.writeAll("/tmp/iris-map.txt", "hello, world")
    scoped m := try fs.mapFile("/tmp/iris-map.txt")
    print("{m.bytes.len} bytes: {m.bytes[0:5]}")

    scoped counters := try fs.mapFile("/tmp/iris-counters.bin", write: true, size: 64)
    counters.bytes[0] += 1
    try counters.sync()
    print("runs: {counters.bytes[0]}")

    try fs.remove("/tmp/iris-counters.bin")
    try fs.remove("/tmp/iris-map.txt")
}
```

```output
12 bytes: hello
runs: 1
```

`mapMemory` gives zeroed memory of any size straight from the system. It suits a large buffer, which goes back to the system as soon as it is freed.

## Folders

`mkdir` makes a folder, and `mkdirAll` also makes the folders above it. `readDir` lists a folder. `removeAll` removes a folder with everything in it:

```iris run
import fs

!void main() {
    try fs.mkdirAll("/tmp/iris-fs-demo/logs")
    try fs.writeAll("/tmp/iris-fs-demo/a.txt", "a")

    scoped dir := try fs.readDir("/tmp/iris-fs-demo")
    i64 folders = 0
    for e in dir.entries {
        if e.isDir {
            folders += 1
        }
    }
    print("{dir.entries.len} entries, {folders} a folder")

    try fs.removeAll("/tmp/iris-fs-demo")
}
```

```output
2 entries, 1 a folder
```

The entries are sorted by name, without `.` and `..`. The `path` module joins paths and takes them apart.

## Walking, finding and copying

`walk` gives the path of every file under a folder. `glob` gives the files that match a pattern with `*`, `?`, `[abc]` and `**`. `copy` copies a file. `tempDir` and `tempFile` make a folder or a file with a unique name:

```iris run
import fs, path

!void main() {
    scoped String root = try fs.tempDir("iris-fs")
    scoped String logs = path.join(root, "logs")
    try fs.mkdir(logs)

    scoped String a = path.join(logs, "a.log")
    scoped String b = path.join(root, "b.log")
    try fs.writeAll(a, "a")
    try fs.copy(a, b)

    all := try fs.walk(root)
    defer all.freeEach()
    scoped String pattern = path.join(root, "**/*.log")
    logged := try fs.glob(pattern)
    defer logged.freeEach()

    print("{all.len} files, {logged.len} logs, {fs.matches("*.log", "a.log")}")
    try fs.removeAll(root)
}
```

```output
2 files, 2 logs, true
```
