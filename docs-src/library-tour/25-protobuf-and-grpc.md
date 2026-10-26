# Protocol Buffers and gRPC

Protocol Buffers (protobuf) is a binary message format with a number for each field. gRPC sends protobuf messages over HTTP/2 to call a function on a server. The `proto` module reads and writes messages from structs, and the `grpc` module is a client and a server.

## Messages

A struct marked `@proto` is a message. Each field has a number. A field takes the number after the field before it, starting at 1, or its own from `@proto(N)`. Numbers are what goes on the wire, so a field keeps its number when fields are added or moved:

```iris run
import hex, proto

enum Status { active, blocked }

@proto
struct Item {
    String name
    i32 qty
}

@proto
struct Order {
    i64 id
    String customer
    List[Item] items
    @proto(8) Status status
    @proto(9) List[i32] codes
}

!void main() {
    o := Order{id: 150, customer: "Ana".copy(), status: .blocked}
    defer o.free()
    o.items.push(Item{name: "cable".copy(), qty: 2})
    o.codes.push(3)
    o.codes.push(270)

    scoped String bytes = proto.encode(o)
    print(hex.encode(bytes))

    scoped back := try proto.decode(Order, bytes)
    print("{back.id} {back.customer} {back.items.at(0).name} x{back.items.at(0).qty} {back.status} {back.codes.at(1)}")
}
```

```output
0896011203416e611a090a056361626c65100240014a03038e02
150 Ana cable x2 blocked 270
```

A field at its zero value (0, false, empty text, an empty list, a message with nothing set) isn't written, as proto3 does. Reading skips fields the struct doesn't have, so a newer sender can add fields an older reader doesn't know.

## Field types

Each Iris type has a protobuf type:

| Iris | protobuf |
|---|---|
| `i32`, `i64` | `int32`, `int64` |
| `u32`, `u64` | `uint32`, `uint64` |
| `@proto(zigzag) i32`, `i64` | `sint32`, `sint64` |
| `@proto(fixed) u32`, `u64`, `i32`, `i64` | `fixed32`, `fixed64`, `sfixed32`, `sfixed64` |
| `f32`, `f64` | `float`, `double` |
| `bool` | `bool` |
| an enum | an enum, by its values' numbers |
| `String` | `string` and `bytes` |
| a `@proto` struct | a message |
| a slice or `List` | `repeated`, packed for numbers |
| `Map[String, V]` | `map<string, V>` |

zigzag suits numbers that are often negative: -1 takes one byte instead of ten. A setting goes with a number in the same `@proto`: `@proto(4, zigzag)`. A repeated number is read packed or one field per value, whichever the sender used.

`proto.encodeTo(&out, &msg)` adds a message's bytes to a `String` that may hold others already. `proto.decodeTo(bytes, &msg)` reads into a message that has values: lists grow and messages merge, as protobuf says for two parts of one message.

A `@proto` struct is also a `@json` struct, so `json.encode` writes it as JSON with its field names.

## Errors

Bytes that aren't protobuf fail with `ProtoError.syntax`. A field sent with another wire type than the struct's fails with `wrongType`, and a number too big for an `i8`, `i16`, `u8` or `u16` field with `range`. The message names the field:

```iris run
import proto

@proto
struct Port {
    u16 number
}

void main() {
    p := proto.decode(Port, "\x08\x80\x80\x04") catch e {
        print("{e.code}: {e.message}")
        Port{}
    }
    p2 := proto.decode(Port, "\x0a\x01") catch e {
        print("{e.code}: {e.message}")
        Port{}
    }
}
```

```output
ProtoError.range: number: 65536 doesn't fit in u16
ProtoError.wrongType: number: wire type 2 where a number was expected
```
