## Messages

A struct marked `@proto` is a protobuf message. Its fields are numbered in order from 1, or from the number in `@proto(N)`, and `@proto(zigzag)` and `@proto(fixed)` pick sint and fixed integers. The [Protocol Buffers chapter](../library-tour/25-protobuf-and-grpc.html) of the Library Tour has the type of each field. This is the short version:

```iris run
import hex, proto

@proto
struct Point {
    i32 x
    @proto(zigzag) i32 y
    String label
}

!void main() {
    scoped p := Point{x: 150, y: -1, label: "a".copy()}
    scoped String bytes = proto.encode(p)
    print(hex.encode(bytes))

    scoped back := try proto.decode(Point, bytes)
    print("{back.x} {back.y} {back.label}")
}
```

```output
08960110011a0161
150 -1 a
```
