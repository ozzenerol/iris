## Whole files

`readAll` gives a file's contents as a `String`, and `writeAll` replaces a file's contents, making the file when it isn't there. `append` adds to the end:

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

`open` opens a file for reading, and `readLine` gives one line at a time, failing with `FsError.end` at the end. A big file is read this way without holding all of it:

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

`create` opens a file for writing, emptying it first, and `openAppend` for writing at its end; `File.write` writes to it. `File.read` fills a buffer, for files that aren't text.

## Mapping a file into memory

`mapFile` gives a file's bytes as memory, without reading them first: the system reads each part when it is first used, so a large file costs nothing until it is looked at. `free` gives the memory back. Changes stay in the memory unless `write` is set; then they go to the file, and every program that maps the same file with `write` sees them, which is a way for two processes to share memory. With `write` and a `size`, the file grows to that size first:

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

`mapMemory` gives zeroed memory of any size straight from the system, for a large buffer that goes back to the system as soon as it is freed.

## Folders

`mkdir` makes a folder, `mkdirAll` makes it with the folders above it, and `readDir` lists one. `removeAll` removes a folder with everything in it:

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

The entries come in the order the system gives them, without `.` and `..`. Paths are joined and taken apart with the `path` module.
