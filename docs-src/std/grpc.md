## A server and a client

A `Server` adds methods by path and serves them over HTTP/2. A `Client` calls them. Messages are `@proto` structs. The [Protocol Buffers and gRPC chapter](../library-tour/25-protobuf-and-grpc.html) of the Library Tour also has streams, metadata and deadlines.

```iris
import grpc

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
    scoped reply := HelloReply{message: format("Hello {req.name}")}
    try call.send(&reply)
}

!void main() {
    grpc.Server s
    defer s.free()
    s.unary("/helloworld.Greeter/SayHello", sayHello)
    try s.serve(50051)
}
```

A client of the same service:

```iris
import grpc

!void main() {
    scoped grpc.Client c
    c.dial("localhost:50051")
    scoped req := HelloRequest{name: "Ana".copy()}
    scoped HelloReply reply
    try c.call("/helloworld.Greeter/SayHello", &req, &reply, timeout: 2s)
    print(reply.message)
}
```
