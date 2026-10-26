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

## A gRPC server and client

A gRPC method has a path, `/package.Service/Method`, and takes and returns `@proto` messages. `Server.unary` adds a method that gets one message and sends one back. The handler reads the request with `call.recv` and answers with `call.send`. On the other side, `Client.call` sends a request and fills in the reply:

```iris run
import grpc, net

@proto
struct HelloRequest {
    String name
}

@proto
struct HelloReply {
    String message
}

!void sayHello(grpc.Call *call) {
    scoped HelloRequest req
    _ = try call.recv(&req)
    if req.name.len == 0 {
        return error(grpc.Code.invalidArgument, "a name is needed")
    }
    scoped reply := HelloReply{message: format("Hello {req.name}")}
    try call.send(&reply)
}

void serve(grpc.Server *s, net.Listener ln) {
    s.serveOn(ln) catch e { }
}

!void main() {
    grpc.Server s
    s.unary("/helloworld.Greeter/SayHello", sayHello)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&s, ln)

    scoped grpc.Client c
    scoped String target = format("127.0.0.1:{ln.port}")
    c.dial(target)

    scoped req := HelloRequest{name: "Ana".copy()}
    scoped HelloReply reply
    try c.call("/helloworld.Greeter/SayHello", &req, &reply)
    print(reply.message)

    scoped HelloRequest empty
    c.call("/helloworld.Greeter/SayHello", &empty, &reply) catch e {
        print("{e.code}: {e.message}")
    }
    s.shutdown(1s)
}
```

```output
Hello Ana
Code.invalidArgument: a name is needed
```

An error a handler returns ends the call. Its code goes to the client when it is a `grpc.Code`, and `unknown` when it is another error. The client gets the same code and message back as its error. A path the server doesn't have fails with `Code.unimplemented`.

`Server.serve(port)` serves on every core and `Server.serveTls(port, certFile, keyFile)` over TLS. A client dials `"host:port"` for a plain connection and `"https://host:port"` for TLS. One client makes any number of calls at once, from any number of tasks, on one HTTP/2 connection.

## Streams

`Server.stream` adds a method whose messages go each way as they are sent. The same handler does server streaming, client streaming and streams both ways. `call.recv` returns false when the client has sent its last message:

```iris run
import grpc, net

@proto
struct Number {
    i64 n
}

!void running(grpc.Call *call) {
    scoped Number next
    i64 total = 0
    for try call.recv(&next) {
        total += next.n
        scoped out := Number{n: total}
        try call.send(&out)
    }
}

void serve(grpc.Server *s, net.Listener ln) {
    s.serveOn(ln) catch e { }
}

!void main() {
    grpc.Server s
    s.stream("/sum.Adder/Running", running)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&s, ln)

    scoped grpc.Client c
    scoped String target = format("127.0.0.1:{ln.port}")
    c.dial(target)

    scoped st := try c.open("/sum.Adder/Running")
    scoped Number out
    for n in [5, 10, 20] {
        scoped m := Number{n: n}
        try st.send(&m)
        _ = try st.recv(&out)
        print("{out.n}")
    }
    try st.closeSend()
    more := try st.recv(&out)
    print("more: {more}")
    s.shutdown(1s)
}
```

```output
5
15
35
more: false
```

`Client.open` starts a call and returns a `Stream`. `send` writes a message and `closeSend` says there are no more. `recv` reads the next reply and returns false at the end, or fails with the status when the server sent an error.

## Metadata and deadlines

Headers go with a call as metadata: `c.open(method, metadata: ["authorization: Bearer abc"])` or `c.call(method, &req, &reply, metadata: [...])`. The handler reads them with `call.metadata("authorization")`. `call.header` adds a header to the reply and `call.trailer` a trailer after it, and the client reads them with `Stream.header` and `Stream.trailer`.

`timeout: 2s` on `call` or `open` sends the client's deadline as `grpc-timeout`. The client stops waiting at the deadline and fails with `Code.deadlineExceeded`. A handler that works long checks `call.expired()` or `call.timeLeft()`.
