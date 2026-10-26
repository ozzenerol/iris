# QUIC and HTTP/3

QUIC carries connections over UDP with TLS 1.3 built in. One connection holds many streams, and a packet lost on one stream doesn't hold up the others. HTTP/3 is HTTP over QUIC. The `quic` module has a client and a server. The `http` module serves and asks for HTTP/3 with the same handlers, Router and Client as HTTP/1.1 and HTTP/2.

## A server and a client

`quic.listen(port, tls, config, host)` opens a UDP port with the certificates of a `net.TlsConfig` and gives a `Listener`. Its `accept` waits for the next connection whose handshake is done. `quic.dial(host, port, config)` connects to a server and gives a `Conn`. `Config.alpn` names the application protocol each side speaks, and they must have one in common.

On a connection, `openStream` opens a stream both ways, and `acceptStream` waits for one the peer opened. A `Stream` reads and writes bytes in order, as a TCP connection does. `closeWrite` ends what this side sends, so the other side reads to the end and gets 0. `close` lets go of the stream:

```iris run
import net, quic

void echo(quic.Stream s) {
    defer s.close()
    scoped String got = s.readAll() catch e {
        return
    }
    s.write("you sent {got.len} bytes: {got}") catch e { }
    s.closeWrite()
}

void handle(quic.Conn c) {
    for {
        s := c.acceptStream() catch e {
            break
        }
        spawn echo(s)
    }
    c.close()
}

void serve(quic.Listener ln) {
    for {
        c := ln.accept() catch e {
            return
        }
        spawn handle(c)
    }
}

!void main() {
    tls := try net.tlsConfig("tests/data/tls/server.pem", "tests/data/tls/server.key")
    ln := try quic.listen(0, &tls, quic.Config{alpn: "echo"}, "127.0.0.1")
    spawn serve(ln)

    config := quic.Config{alpn: "echo", ca: "tests/data/tls/ca.pem", serverName: "localhost"}
    c := try quic.dial("127.0.0.1", ln.port, config)
    defer c.close()
    print("speaking {c.protocol()}")
    for word in ["one", "two"] {
        s := try c.openStream()
        try s.write(word)
        s.closeWrite()
        scoped String reply = try s.readAll()
        s.close()
        print(reply)
    }
}
```

```output
speaking echo
you sent 3 bytes: one
you sent 3 bytes: two
```

The client checks the server's certificate against the ones the system trusts. `Config.ca` names a file of certificates to trust instead, and `Config.serverName` the name the certificate must have when it isn't the host dialed. Each connection and its streams belong to the thread that dialed or accepted it.

## Streams

Streams are cheap: a request and its answer often get one each. `openUniStream` opens one that only this side writes. `Stream.isUni()` tells which kind the peer opened. Each side may have `Config.maxStreams` streams open at once both ways, and `maxUniStreams` one way. `openStream` waits while the peer's limit is reached.

`write` waits while the peer's flow control holds the data back, or while `Config.streamBuffer` bytes wait to go. `reset(code)` abandons what this side sends, and the peer's reads fail with `QuicError.reset`. `stopReading(code)` asks the peer to stop sending, and its writes fail with `QuicError.stopped`. `setTimeout(d)` makes a read or a write that waits longer than `d` fail with `QuicError.timeout`.

A connection that hears nothing for `Config.idleTimeout` ends. `close(code, reason)` ends it with an error code of the application, which the peer sees in `why()`. The errors are `QuicError` codes:

- `closed`: the connection or the stream ended.
- `timeout`: nothing came for too long, or the handshake took longer than `Config.handshakeTimeout`.
- `refused`: the server turned the connection down.
- `tls`: the handshake failed, as for a certificate that doesn't check.
- `protocol`: the peer broke the rules of QUIC.
- `reset` and `stopped`: the peer reset a stream, or stopped reading it.
- `tooLarge`: a stream had more than `readAll`'s limit.

Lost packets are found and sent again, and NewReno congestion control keeps the sender from flooding the path. On Linux, datagrams go out in batches and come in coalesced (GSO and GRO), which saves system calls on large transfers.

## HTTP/3

`r.serveTls(8443, "cert.pem", "key.pem", http3: true)` serves HTTPS on TCP port 8443 and HTTP/3 on UDP port 8443, with the same routes. Answers over HTTP/1.1 and HTTP/2 carry an `Alt-Svc` header that tells clients where HTTP/3 is, and browsers move there. `r.serveH3(port, certFile, keyFile)` serves HTTP/3 alone. `r.advertiseH3(port)` adds the `Alt-Svc` header when HTTP/3 is served some other way. A handler sees `req.protocol` as `"HTTP/3"`.

A `Client` with `http3` set sends `https://` requests over HTTP/3. It first tries the URL's port over UDP. A host that doesn't answer gets its requests over TCP, and an `Alt-Svc` in its answers moves later requests to the port it names:

```iris run
import http, quic, net

void hello(http.Request *req, http.Response *res) {
    res.text("hello over {req.protocol}")
}

void serve(http.Router *r, quic.Listener ln) => r.serveH3On(ln)

!void main() {
    http.Router r
    defer r.free()
    r.get("/", hello)
    tls := try net.tlsConfig("tests/data/tls/server.pem", "tests/data/tls/server.key")
    ln := try quic.listen(0, &tls, quic.Config{alpn: "h3"}, "127.0.0.1")
    spawn serve(&r, ln)

    http.Client c = http.Client{http3: true, ca: "tests/data/tls/ca.pem"}
    defer c.close()
    scoped String url = format("https://localhost:{ln.port}/")
    scoped http.Reply reply = try c.get(url)
    print("{reply.protocol} {reply.status} {reply.body}")
    r.shutdown()
}
```

```output
HTTP/3 200 hello over HTTP/3
```

`serveH3On` serves a `quic.Listener` that is open already, which must offer `"h3"` and must have been opened on the thread that serves it. `r.shutdown` sends each HTTP/3 client a GOAWAY, answers the requests that came, then closes the connections.

Streamed bodies work as on HTTP/2. `res.write` sends DATA frames as the handler goes, a `Router.stream` route reads the request body with `req.read` as it comes, and trailers go both ways. The client's `stream` and `begin` go over HTTP/1.1 or HTTP/2.

Headers are compressed with QPACK's static table, which holds the common names and values, and Huffman codes. The dynamic table isn't used: each side's table has room for nothing.

The library page for [quic](../std/index.html#quic) lists everything.
