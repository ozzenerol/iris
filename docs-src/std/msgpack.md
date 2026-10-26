## Structs and values

`msgpack.encode` and `msgpack.decode` read and write `@json` structs as maps of their fields. `msgpack.parse` and `msgpack.write` work on a `Value` of any shape. The [MessagePack and CBOR chapter](../library-tour/26-msgpack-and-cbor.html) of the Library Tour has the details.

```iris run
import hex, msgpack

@json
struct Point {
    i32 x
    i32 y
}

!void main() {
    scoped String bytes = msgpack.encode(Point{x: 1, y: -2})
    print(hex.encode(bytes))

    p := try msgpack.decode(Point, bytes)
    print("{p.x} {p.y}")

    scoped v := try msgpack.parse(bytes)
    print("{v.len()} fields, y is {v.get("y").int}")
}
```

```output
82a17801a179fe
1 -2
2 fields, y is -2
```
