## Structs and values

`cbor.encode` and `cbor.decode` read and write `@json` structs as maps of their fields. `cbor.parse` and `cbor.write` work on a `Value` of any shape. Floats are written in their shortest exact form, and reading takes indefinite lengths and skips tags. The [MessagePack and CBOR chapter](../library-tour/26-msgpack-and-cbor.html) of the Library Tour has the details.

```iris run
import cbor, hex

@json
struct Point {
    i32 x
    i32 y
}

!void main() {
    scoped String bytes = cbor.encode(Point{x: 1, y: -2})
    print(hex.encode(bytes))

    p := try cbor.decode(Point, bytes)
    print("{p.x} {p.y}")

    scoped v := try cbor.parse(bytes)
    print("{v.len()} fields, y is {v.get("y").int}")
}
```

```output
a2617801617921
1 -2
2 fields, y is -2
```
