## Writing

A `tar.Writer` builds an archive in memory:

- `add` adds a file from its bytes, `addDir` a folder and `addLink` a symbolic link.
- `addFile` adds a file from the disk, and `addTree` a whole folder from the disk.
- `finish` ends the archive. `out` then holds it.

Names longer than the ustar header holds are written with a PAX header, so any length works:

```iris run
import tar, compress, fs

!void main() {
    tar.Writer w
    defer w.free()

    w.addDir("site")
    w.add("site/index.html", "<h1>hello</h1>\n", mtime: 1760000000)
    w.add("site/run.sh", "#!/bin/sh\n", mode: 493)
    w.addLink("site/home.html", "index.html")
    w.finish()

    scoped String packed = try compress.gzip(w.out)
    try fs.writeAll("/tmp/iris-site.tar.gz", packed)
    print("{w.out.len} bytes, {packed.len < w.out.len}")
}
```

```output
4096 bytes, true
```

`mode` is the permission bits, 420 (0644) unless given. `mtime` is the time of change in seconds since 1970, 0 unless given, so the same files make the same archive.

## Reading

`tar.parse(data)` reads an archive from memory and `tar.open(path)` from a file. An archive compressed with gzip is decompressed first. Each `tar.Entry` has its `name`, `kind`, `size`, `mode`, `mtime`, `link` and `data` (a file's bytes). `find(name)` gives the entry with that name, or `null`:

```iris run
import tar

!void main() {
    scoped tar.Archive a = try tar.open("/tmp/iris-site.tar.gz")

    for e in a.entries {
        print("{e.kind} {e.name} {e.size} {e.mode}")
    }

    print(a.find("site/index.html").data)
}
```

```output
dir site/ 0 493
file site/index.html 15 420
file site/run.sh 10 493
symlink site/home.html 0 511
<h1>hello</h1>

```

Data that isn't tar, a header whose checksum doesn't match, or an archive cut short fails with `TarError.broken`.

## Into a folder

`extract(dir)` puts the files, folders and links into `dir`, with their modes. A name that is absolute or has a `..` part, or a link that points out of `dir`, fails with `TarError.unsafe` before anything is written. An archive from elsewhere can't write outside `dir`:

```iris run
import tar, fs

!void main() {
    scoped tar.Archive a = try tar.open("/tmp/iris-site.tar.gz")
    try a.extract("/tmp/iris-site")
    scoped String page = try fs.readAll("/tmp/iris-site/site/index.html")
    print(page)

    tar.Writer w
    defer w.free()
    w.add("../../etc/cron.d/job", "* * * * * root sh\n")
    w.finish()

    scoped tar.Archive bad = try tar.parse(w.out)
    bad.extract("/tmp/iris-site") catch e {
        print("{e.code}: {e.message}")
    }

    try fs.removeAll("/tmp/iris-site")
    try fs.remove("/tmp/iris-site.tar.gz")
}
```

```output
<h1>hello</h1>

TarError.unsafe: ../../etc/cron.d/job has .. in it, which would leave the folder
```
