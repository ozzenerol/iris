## A client and a server

`quic.listen` opens a UDP port with the certificates of a `net.TlsConfig`, and its `accept` gives each connection once its handshake is done. `quic.dial` connects to a server. A connection carries streams: `openStream` opens one, `acceptStream` waits for one the peer opened, and each reads and writes bytes in order. HTTP/3 is in the `http` module, and the [QUIC and HTTP/3 chapter](../library-tour/24-quic-and-http3.html) of the Library Tour has more. This program runs both sides:

```iris run
import net, quic

void handle(quic.Conn c) {
    s := c.acceptStream() catch e {
        return
    }
    scoped String got = s.readAll() catch "".copy()
    s.write("got {got}") catch e { }
    s.closeWrite()
    s.close()
    sleep(20ms)
    c.close()
}

void serve(quic.Listener ln) {
    c := ln.accept() catch e {
        return
    }
    handle(c)
}

!void main() {
    tls := try net.tlsConfig("tests/data/tls/server.pem", "tests/data/tls/server.key")
    ln := try quic.listen(0, &tls, quic.Config{alpn: "demo"}, "127.0.0.1")
    spawn serve(ln)
    c := try quic.dial("127.0.0.1", ln.port, quic.Config{alpn: "demo", ca: "tests/data/tls/ca.pem", serverName: "localhost"})
    defer c.close()
    s := try c.openStream()
    try s.write("ping")
    s.closeWrite()
    scoped String reply = try s.readAll()
    s.close()
    print(reply)
}
```

```output
got ping
```
