# MessagePack and CBOR

MessagePack and CBOR are binary forms of the values JSON has: numbers, text, bools, null, arrays and maps. A message takes fewer bytes than its JSON and is faster to read. The `msgpack` and `cbor` modules work the same way. Each reads and writes `@json` structs, and each has a `Value` for data of any shape.

## Structs

`msgpack.encode(v)` turns a `@json` struct into bytes, and `msgpack.decode(T, bytes)` reads it back. A struct becomes a map from its field names to their values, with the names `@json` gives them, as in [JSON](01-json.html). An enum is written as the name of its value, a `List` or slice as an array, and a `Map` with text keys as a map:

```iris run
import hex, msgpack

enum Level { low, high }

@json
struct Reading {
    String sensor
    f64 value
    Level level
    List[i64] samples
}

!void main() {
    r := Reading{sensor: "t1".copy(), value: 21.5, level: .high}
    defer r.free()
    r.samples.push(3)
    r.samples.push(-4)

    scoped String bytes = msgpack.encode(r)
    print("{bytes.len} bytes: {hex.encode(bytes[0:10])}...")

    scoped back := try msgpack.decode(Reading, bytes)
    print("{back.sensor} {back.value} {back.level} {back.samples.at(1)}")
}
```

```output
48 bytes: 84a673656e736f72a274...
t1 21.5 high -4
```

Integers take the fewest bytes that hold them. Reading takes an integer of any size, and fails with `range` when it doesn't fit the field. A field the bytes don't have keeps its default, and keys the struct doesn't have are skipped. A nil value leaves the field as it is.

`cbor.encode` and `cbor.decode` do the same in CBOR (RFC 8949). A float is written in the shortest of its three sizes that keeps its value, as the RFC prefers, so 1.5 takes three bytes. Reading also takes arrays, maps, text and bytes of indefinite length, and skips tags:

```iris run
import cbor, hex

@json
struct Point {
    i32 x
    i32 y
    f64 weight
}

!void main() {
    scoped String bytes = cbor.encode(Point{x: 1, y: -2, weight: 1.5})
    print(hex.encode(bytes))

    p := try cbor.decode(Point, bytes)
    print("{p.x} {p.y} {p.weight}")
}
```

```output
a361780161792166776569676874f93e00
1 -2 1.5
```

## Any data

`msgpack.parse(bytes)` reads any message into a `msgpack.Value`. Its `kind` says what it holds: `nil`, `boolean`, `integer`, `unsigned` (above the largest `i64`), `float`, `text`, `bytes`, `array` or `map`. The data is in `boolean`, `int`, `uint`, `float` or `text`. `get` gives the value of a text key in a map, or `null`. `at` gives the value at a place, and `len` the number of values:

```iris run
import msgpack

!void main() {
    scoped doc := msgpack.map()
    doc.set("name", msgpack.text("edge"))
    ports := msgpack.array()
    ports.push(msgpack.integer(80))
    ports.push(msgpack.integer(443))
    doc.set("ports", ports)

    scoped String bytes = msgpack.write(doc)
    print("{bytes.len} bytes")

    scoped back := try msgpack.parse(bytes)
    print("{back.get("name").text}, {back.get("ports").len()} ports, the last {back.get("ports").at(1).int}")
    print("no user: {back.get("user") == null}")
}
```

```output
22 bytes
edge, 2 ports, the last 443
no user: true
```

`msgpack.nil()`, `boolean(b)`, `integer(n)`, `float(f)`, `text(s)`, `bytes(b)`, `array()` and `map()` make values. `set` and `push` take the value they are given, and `free` gives back a value and everything in it. `cbor.Value` has the same functions.

A map may have keys that aren't text. They are in the value's `keys`, at the same place as their values in `items`.

## Errors

Bytes that aren't a whole message fail with `syntax`, and the message gives the byte where reading stopped. Filling a struct can also fail with `wrongType`, `missing` (a field with `@json(required)`) and `range`, with the path of the field:

```iris run
import msgpack

@json
struct Port {
    u8 number
}

void main() {
    scoped m := msgpack.map()
    m.set("number", msgpack.integer(300))
    scoped String bytes = msgpack.write(m)

    p := msgpack.decode(Port, bytes) catch e {
        print("{e.code}: {e.message}")
        Port{}
    }
    p2 := msgpack.decode(Port, bytes[0:3]) catch e {
        print("{e.code}: {e.message}")
        Port{}
    }
}
```

```output
MsgpackError.range: number: 300 doesn't fit in u8
MsgpackError.syntax: byte 2: the data ends in the middle of a value
```
