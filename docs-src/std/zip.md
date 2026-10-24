## Writing

A `zip.Writer` builds an archive in memory:

- `add` adds a file from its bytes. It is deflated unless that wouldn't make it smaller, or stored with `method: .stored`.
- `addDir` adds a folder, `addFile` a file from the disk and `addTree` a whole folder.
- `finish` writes the central directory. `out` then holds the archive.

```iris run
import zip, fs, str

!void main() {
    zip.Writer w
    defer w.free()

    scoped String report = str.repeat("region eu-west, 200 OK\n", 200)
    try w.add("report.txt", report)
    try w.add("logo.png", "\x89PNG...", method: .stored)
    w.addDir("empty")
    w.finish()

    try fs.writeAll("/tmp/iris-report.zip", w.out)
    print("{report.len} bytes of text, under 500 in the zip: {w.out.len < 500}")
}
```

```output
4600 bytes of text, under 500 in the zip: true
```

Names are written as UTF-8. Each file keeps its mode, 420 (0644) unless `mode` gives another. Archives over 4 GiB need zip64, which is neither written nor read.

## Reading

`zip.parse(data)` reads an archive from memory and `zip.open(path)` from a file. Each `zip.Entry` has its `name`, `isDir`, `method`, `size`, `compressedSize`, `crc`, `mtime` and `mode`. `read(entry)` gives an entry's bytes, decompressed. It fails with `ZipError.crc` when they don't match the archive's CRC-32 for them:

```iris run
import zip, fs

!void main() {
    scoped zip.Archive a = try zip.open("/tmp/iris-report.zip")

    for e in a.entries {
        print("{e.name}: {e.method}, {e.size} bytes, smaller: {e.compressedSize < e.size}")
    }

    scoped String text = try a.read(a.find("report.txt"))
    print(text[0:22])

    try fs.remove("/tmp/iris-report.zip")
}
```

```output
report.txt: deflated, 4600 bytes, smaller: true
logo.png: stored, 7 bytes, smaller: false
empty/: stored, 0 bytes, smaller: false
region eu-west, 200 OK
```

`extract(dir)` puts the files and folders into `dir`. A name that is absolute or has a `..` part fails with `ZipError.unsafe` before anything is written. An encrypted entry, or one compressed with another method, fails with `ZipError.unsupported`.
