# Compression and archives

The `compress` module makes data smaller with gzip and zlib, the formats behind `.gz` files and HTTP compression. The `tar` and `zip` modules pack many files into one, for backups, releases and downloads. All three work on data in memory, so they combine with each other and with the `fs` module.

## Compressing data

`compress.gzip(data)` gives the data compressed in the gzip format, and `compress.gunzip(packed)` gives it back. Text that repeats itself, such as logs, JSON and HTML, gets much smaller.

```iris run
import compress, fs, str

!void main() {
    scoped String log = str.repeat("2026-10-24T10:00:00Z INFO GET /health 200 1ms\n", 1000)
    scoped String packed = try compress.gzip(log)
    print("{log.len} bytes, under 1% of that packed: {packed.len * 100 < log.len}")

    path := "/tmp/iris-tour-access.log.gz"
    try fs.writeAll(path, packed)

    scoped String read = try fs.readAll(path)
    scoped String back = try compress.gunzip(read)
    print("the same after a round trip: {back == log}")

    try fs.remove(path)
}
```

```output
46000 bytes, under 1% of that packed: true
the same after a round trip: true
```

The file it writes is a normal `.gz` file that `gunzip` and `zcat` read. `level` sets how hard to try, from 1, the fastest, to 9, the smallest. The default is 5, which makes files a few percent larger than 6 in about two thirds of the time: `compress.gzip(log, level: 9)`.

`compress.compress(data, format)` and `compress.decompress` take the format:

- `.gzip`
- `.zlib`, the format inside PNG files and many protocols
- `.raw`, deflate with no header at all

`gunzip` accepts zlib data too. Data that isn't compressed fails with `CompressError.broken`.

```iris run
import compress

!void main() {
    scoped String z = try compress.compress("hello hello hello", .zlib)
    scoped String back = try compress.decompress(z, .zlib)
    print(back)

    _ = compress.gunzip("plain text") catch e {
        print("{e.code}")
        "".copy()
    }
}
```

```output
hello hello hello
CompressError.broken
```

## A limit for data from outside

A few kilobytes of compressed data can grow into gigabytes. Decompress data from a client or another service with a `limit`. Anything that would grow past it fails with `CompressError.tooLarge`.

```iris run
import compress, str

!void main() {
    scoped String zeros = str.repeat("0", 10000000)
    scoped String bomb = try compress.gzip(zeros, level: 9)
    print("{zeros.len} bytes, over 1000 times smaller packed: {bomb.len * 1000 < zeros.len}")

    _ = compress.gunzip(bomb, limit: 1048576) catch e {
        print("{e.code}: {e.message}")
        "".copy()
    }
}
```

```output
10000000 bytes, over 1000 times smaller packed: true
CompressError.tooLarge: the data comes out larger than the limit
```

Without a limit, the most is 1 GiB.

## A part at a time

Data that arrives in pieces, or is too big to hold twice, goes through a `compress.Writer`. Each `write` compresses a piece and adds the result to its `out`, and `finish` ends the stream. A `compress.Reader` works the other way. Its `write` returns `true` once the stream has ended.

```iris run
import compress

!void main() {
    scoped w := try compress.writer()
    for i in 0..1000 {
        try w.write("{i},sensor-{i % 4},21.5\n")
    }
    try w.finish()
    print("under 4 KiB compressed: {w.out.len < 4096}")

    scoped r := try compress.reader()
    done := try r.write(w.out)
    scoped rows := r.out.lines()
    print("ended: {done}, {rows.len} rows, the last is {rows.at(rows.len - 1)}")
}
```

```output
under 4 KiB compressed: true
ended: true, 1000 rows, the last is 999,sensor-3,21.5
```

`out` keeps growing. A program that sends the data on takes what is there after each `write` and clears it with `w.out.clear()`.

## Tar archives

A tar archive holds files and folders with their names, modes and times. It is usually compressed with gzip into a `.tar.gz`. A `tar.Writer` builds one in memory:

- `add` adds a file from its bytes.
- `addDir` adds a folder.
- `addFile` adds a file from the disk.
- `addTree` adds a whole folder from the disk.
- `finish` ends the archive, and `out` holds it.

```iris run
import compress, fs, tar

!void main() {
    dir := "/tmp/iris-tour-app"
    try fs.mkdirAll("{dir}/conf")
    try fs.writeAll("{dir}/conf/app.toml", "port = 8080\n")
    try fs.writeAll("{dir}/notes.txt", "deployed on friday\n")

    tar.Writer w
    defer w.free()
    try w.addTree(dir, "backup")
    w.add("backup/VERSION", "1.4.2\n")
    w.finish()

    scoped String packed = try compress.gzip(w.out)
    try fs.writeAll("/tmp/iris-tour-backup.tar.gz", packed)
    print("{w.out.len} bytes, smaller gzipped: {packed.len < w.out.len}")

    try fs.removeAll(dir)
}
```

```output
4608 bytes, smaller gzipped: true
```

A tar archive is made of 512-byte blocks, so even small files take some room until gzip squeezes out the empty parts.

`tar.open(path)` reads an archive back and decompresses it first when it is gzipped. `tar.parse(data)` reads one from memory. Each `tar.Entry` has its `name`, `kind`, `size`, `mode`, `mtime` and, for a file, its bytes in `data`. `find(name)` gives one entry, or `null`.

```iris run
import tar

!void main() {
    scoped tar.Archive a = try tar.open("/tmp/iris-tour-backup.tar.gz")
    for e in a.entries {
        print("{e.kind} {e.name} {e.size}")
    }

    v := a.find("backup/VERSION")
    if v != null {
        print("version {v.data.trim()}")
    }
}
```

```output
dir backup/conf/ 0
file backup/conf/app.toml 12
file backup/notes.txt 19
file backup/VERSION 6
version 1.4.2
```

`addTree` names each entry after its place under the folder, with the given prefix in front.

`extract(dir)` writes the files, folders and links into a folder, with their modes:

```iris run
import fs, tar

!void main() {
    scoped tar.Archive a = try tar.open("/tmp/iris-tour-backup.tar.gz")
    try a.extract("/tmp/iris-tour-restore")

    scoped String conf = try fs.readAll("/tmp/iris-tour-restore/backup/conf/app.toml")
    print(conf.trim())

    try fs.removeAll("/tmp/iris-tour-restore")
    try fs.remove("/tmp/iris-tour-backup.tar.gz")
}
```

```output
port = 8080
```

An archive from elsewhere might contain a name like `../../etc/passwd` that writes outside the folder. `extract` checks every name and link first and fails with `TarError.unsafe` before writing anything.

```iris run
import tar

!void main() {
    tar.Writer w
    defer w.free()
    w.add("../../etc/cron.d/job", "* * * * * root sh\n")
    w.finish()

    scoped tar.Archive bad = try tar.parse(w.out)
    bad.extract("/tmp/iris-tour-restore") catch e {
        print("{e.code}: {e.message}")
    }
}
```

```output
TarError.unsafe: ../../etc/cron.d/job has .. in it, which would leave the folder
```

## Zip archives

Zip compresses each file separately, so one file can be read without the rest, and every system opens it. A `zip.Writer` works like a `tar.Writer`. `add` deflates a file unless that wouldn't make it smaller. `method: .stored` keeps a file as it is, for data that is already compressed.

```iris run
import fs, str, zip

!void main() {
    zip.Writer w
    defer w.free()

    scoped String csv = str.repeat("2026-10-24,eu-west,200,14\n", 500)
    try w.add("report/requests.csv", csv)
    try w.add("report/README.txt", "Requests per region, one row a minute.\n")
    try w.add("report/chart.png", "\x89PNG\r\n...", method: .stored)
    w.finish()

    try fs.writeAll("/tmp/iris-tour-report.zip", w.out)
    print("{csv.len} bytes of CSV, the zip is under 1 KiB: {w.out.len < 1024}")
}
```

```output
13000 bytes of CSV, the zip is under 1 KiB: true
```

`zip.open(path)` reads an archive and `zip.parse(data)` reads one in memory. Each `zip.Entry` has its `name`, `size`, `compressedSize`, `method` and `isDir`. `read(entry)` gives one file's bytes and checks them against the CRC-32 the archive stores for them.

```iris run
import fs, zip

!void main() {
    scoped zip.Archive a = try zip.open("/tmp/iris-tour-report.zip")
    for e in a.entries {
        print("{e.name}: {e.method}, {e.size} bytes")
    }

    scoped String readme = try a.read(a.find("report/README.txt"))
    print(readme.trim())

    try fs.remove("/tmp/iris-tour-report.zip")
}
```

```output
report/requests.csv: deflated, 13000 bytes
report/README.txt: stored, 39 bytes
report/chart.png: stored, 9 bytes
Requests per region, one row a minute.
```

The README was stored, since deflating 39 bytes wouldn't have made them smaller.

`extract(dir)` writes every file into a folder. It checks for names that would leave the folder, as tar does, and fails with `ZipError.unsafe`. Bytes that don't match their CRC-32 fail with `ZipError.crc`, and an encrypted entry fails with `ZipError.unsupported`.
