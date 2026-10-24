## Whole data

`gzip` compresses text or bytes in the gzip format, and `gunzip` gives them back. `compress` and `decompress` take the format as an argument: `.gzip`, `.zlib` or `.raw`:

```iris run
import compress, str

!void main() {
    scoped String text = str.repeat("the same words again and again. ", 100)
    scoped String packed = try compress.gzip(text, level: 9)
    scoped String back = try compress.gunzip(packed)
    print("{text.len} bytes, packed under 100: {packed.len < 100}, back the same: {back == text}")

    scoped String z = try compress.compress(text, .zlib)
    print("zlib starts with {z[0]}")
}
```

```output
3200 bytes, packed under 100: true, back the same: true
zlib starts with 120
```

## A limit for untrusted data

A small input can decompress into a huge output. `gunzip` and `decompress` stop with `CompressError.tooLarge` when the output would pass a limit, which is 1 GiB unless given:

```iris run
import compress, str

!void main() {
    scoped String zeros = str.repeat("0", 1000000)
    scoped String packed = try compress.gzip(zeros)

    scoped String big = compress.gunzip(packed, limit: 4096) catch e {
        print("over the limit: {e.code}")
        return
    }
    print("{big.len}")
}
```

```output
over the limit: CompressError.tooLarge
```

## A part at a time

A `Writer` compresses data as it comes and adds the result to its `out`. `finish` ends the stream. A `Reader` does the opposite, and its `write` gives `true` when the stream has ended:

```iris run
import compress

!void main() {
    scoped w := try compress.writer()
    for i in 0..3 {
        try w.write("line {i}\n")
    }
    try w.finish()

    scoped r := try compress.reader()
    done := try r.write(w.out)
    print("ended {done}: {r.out}")
}
```

```output
ended true: line 0
line 1
line 2

```

## Checksums

`crc32` and `adler32` give the checksums that gzip and zlib use. Pass the previous checksum to continue from it, so a big input can be checked a part at a time:

```iris run
import compress

void main() {
    whole := compress.crc32("hello world")
    parts := compress.crc32(" world", compress.crc32("hello"))
    print("{whole} {whole == parts}")
}
```

```output
222957957 true
```

zlib is loaded the first time it is needed. `IRIS_ZLIB` names another copy of it.
