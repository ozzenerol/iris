## Making and reading UUIDs

`v4` makes a random UUID and `v7` one that starts with the time. `format` writes either as text. `parse` reads one back, in either case and with or without dashes:

```iris run
import uuid

!void main() {
    id := uuid.v7()
    scoped String text = id.format()
    print("{text.len} characters, version {id.version()}")

    known := try uuid.parse("0190E5D4-8A2B-7C3D-9E4F-5A6B7C8D9E0F")
    scoped String back = known.format()
    scoped String day = known.time().dateText()
    print("{back}, made on {day}")
    print("{known.equal(id)} {known.compare(id) < 0}")
}
```

```output
36 characters, version 7
0190e5d4-8a2b-7c3d-9e4f-5a6b7c8d9e0f, made on 2024-07-24
false true
```

A v7 UUID made in a later millisecond sorts after one made earlier, both as bytes with `compare` and as text. Rows with v7 ids are therefore added at the end of a database index. Two UUIDs made in the same millisecond can come in either order.
