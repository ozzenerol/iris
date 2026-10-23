# Networking

The `net` module has TCP, UDP, TLS and Unix sockets. A call that waits for the network lets the other tasks on its thread run, so a server can hold many connections with one task each and no callbacks. This chapter goes from a server and a client to the parts a proxy needs: socket options, half-closing, copying between connections and looking at the first bytes.

## A server and a client

`net.listen` gives a `Listener`, and its `accept` waits for the next connection. `net.dial` connects. A `Conn` reads with `read`, `readLine`, `readExactly`, and `readAll`, which reads until the other side closes, and writes with `write`. Each connection the server accepts gets a task of its own:

```iris run
import net

void handle(net.Conn c) {
    defer c.close()
    for {
        line := c.readLine() catch e {
            return
        }
        c.write("you said: {line}\n") catch e {
            line.free()
            return
        }
        line.free()
    }
}

void server(net.Listener ln) {
    for {
        conn := ln.accept() catch e {
            return
        }
        spawn handle(conn)
    }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    spawn server(ln)

    c := try net.dial("127.0.0.1", ln.port)
    defer c.close()
    try c.write("hello\n")
    scoped String reply = try c.readLine()
    print(reply)
}
```

```output
you said: hello
```

Port 0 lets the system pick a free port, which `ln.port` then holds. A host of `""` listens on every address, IPv6 and IPv4 at once. `listen(port, host, shared: true)` lets several threads listen on the same port, each with its own listener, and the kernel spreads the connections between them. That is how a server uses every core.

## Timeouts and errors

A call that fails gives a `NetError`: `refused` when nothing listens, `closed` when the other side went away, `timeout`, `notFound` for a name that doesn't resolve, and `inUse` for a port that is taken. `dial` takes a timeout for connecting, and `setTimeout` limits how long each read or write on a connection may wait:

```iris run
import net

!void main() {
    _ = net.dial("127.0.0.1", 1, timeout: 1s) catch e {
        print("{e.code}")
        net.Conn{}
    }

    ln := try net.listen(0, "127.0.0.1")
    c := try net.dial("127.0.0.1", ln.port)
    defer c.close()
    c.setTimeout(50ms)
    u8[16] buf
    _ = c.read(buf) catch e {
        print("{e.code}")
        0
    }
}
```

```output
NetError.refused
NetError.timeout
```

`readLine` takes a limit on a line's length, so a client can't make a server hold a line without end.

## Socket options

Every connection from `net` starts with `TCP_NODELAY` on, so small writes go out at once. A `Conn` changes the common options with methods: `setNoDelay`, `setKeepAlive` with the idle time, the interval between probes and how many may go unanswered, and `setBuffers` for the kernel's buffers. Any other option that is an int goes through `setOption` and `option`, with the level and name C uses, from the system's headers with [import c](../tour/28-calling-c.html):

```iris run
import net
import c "sys/socket.h"

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    conn := try net.dial("127.0.0.1", ln.port)
    defer conn.close()

    try conn.setKeepAlive(30s, 10s, 3)
    on := try conn.option(c.SOL_SOCKET, c.SO_KEEPALIVE)
    print("keepalive: {on != 0}")

    try conn.setOption(c.SOL_SOCKET, c.SO_KEEPALIVE, 0)
    off := try conn.option(c.SOL_SOCKET, c.SO_KEEPALIVE)
    print("keepalive: {off != 0}")
}
```

```output
keepalive: true
keepalive: false
```

A Linux-only option like `TCP_KEEPIDLE` comes from `netinet/tcp.h` the same way, as `conn.setOption(c.IPPROTO_TCP, c.TCP_KEEPIDLE, 30)`. `setOptionBytes` and `optionBytes` take an option whose value is a struct, as its bytes.

## Setting up a socket

Some options only count when they are set before a socket binds, listens or connects: `SO_REUSEPORT`, `IP_TRANSPARENT`, `TCP_FASTOPEN`, and buffer sizes large enough to change TCP's window. `net.socket` gives a `Socket` that takes them. `bind` gives it an address, then `listen` turns it into a `Listener`, with room for a backlog of connections, or `connect` turns it into a `Conn`.

On a client, `bind` picks the address and the port the connection comes from:

```iris run
import net

!void main() {
    s := try net.socket()
    try s.setBuffers(read: 1MiB)
    try s.bind(net.Addr{port: 0})
    ln := try s.listen(backlog: 4096)

    cs := try net.socket(net.Family.v4)
    try cs.bind(try net.ip("127.0.0.1"))
    conn := try cs.connect(try net.ip("127.0.0.1", ln.port))
    defer conn.close()

    srv := try ln.accept()
    defer srv.close()
    from := srv.remoteAddr()
    scoped String ip = from.ipText()
    print("from {ip}, the client's port: {from.port == conn.localAddr().port}")
}
```

```output
from 127.0.0.1, the client's port: true
```

`net.Addr{port: 8080}` is every address on port 8080. A `Socket` is IPv6 that takes IPv4 too; `Family.v4` and `Family.v6` give one of them only.

## Half-closing

`closeWrite` ends this side's writes. The other side reads the end of the data, and this side can still read what comes back. A client that sends a whole request and then waits for the answer can say it is done this way:

```iris run
import net, str

void shout(net.Listener ln) {
    c := ln.accept() catch e {
        return
    }
    defer c.close()
    String text
    defer text.free()
    u8[256] buf
    for {
        n := c.read(buf) catch e {
            return
        }
        if n == 0 {
            break
        }
        text.append(buf[0:n])
    }
    scoped String loud = str.upper(text)
    c.write(loud) catch e { }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    spawn shout(ln)
    c := try net.dial("127.0.0.1", ln.port)
    defer c.close()
    try c.write("all of it, ")
    try c.write("then the end")
    try c.closeWrite()

    u8[256] buf
    n := try c.read(buf)
    print(buf[0:n])
}
```

```output
ALL OF IT, THEN THE END
```

Over TLS, `closeWrite` sends TLS's own end of data first.

## Copying between connections

`net.copy(to, from)` copies what one connection reads to another until the first one's data ends, and gives the number of bytes. Small messages go through a buffer. On Linux, when neither connection uses TLS, a steady stream goes from socket to socket inside the kernel, with `splice`, without coming up to the program.

A TCP proxy is two copies, one each way. When one direction ends, the proxy passes the end on with `closeWrite`, and the other direction goes on:

```iris run
import net

void backend(net.Listener ln) {
    c := ln.accept() catch e {
        return
    }
    defer c.close()
    scoped String line = c.readLine() catch e {
        return
    }
    c.write("{line}, from the backend\n") catch e { }
}

void oneWay(net.Conn *to, net.Conn *from, chan[i64] done) {
    n := net.copy(to, from) catch e {
        done <- -1
        return
    }
    to.closeWrite() catch e { }
    done <- n
}

void proxy(net.Listener ln, u16 backendPort, chan[i64] counts) {
    client := ln.accept() catch e {
        return
    }
    defer client.close()
    server := net.dial("127.0.0.1", backendPort, timeout: 2s) catch e {
        return
    }
    defer server.close()
    done := chan[i64](2)
    defer done.free()
    spawn oneWay(&server, &client, done)
    oneWay(&client, &server, done)
    _ = <-done
    counts <- client.received
    counts <- server.received
}

!void main() {
    back := try net.listen(0, "127.0.0.1")
    spawn backend(back)
    front := try net.listen(0, "127.0.0.1")
    counts := chan[i64](2)
    defer counts.free()
    spawn proxy(front, back.port, counts)

    c := try net.dial("127.0.0.1", front.port)
    defer c.close()
    try c.write("hello\n")
    try c.closeWrite()
    scoped String reply = try c.readLine()
    print(reply)
    up := <-counts
    down := <-counts
    print("the proxy moved {up} bytes up and {down} bytes down")
}
```

```output
hello, from the backend
the proxy moved 6 bytes up and 24 bytes down
```

`received` and `sent` count the bytes each connection moved, through an error too, which is what a proxy logs. Each copy waits for data as long as its connection's `setTimeout` allows, so that timeout is an idle timeout. `limit` stops a copy after that many bytes.

## The first bytes

`peek(n)` gives the next n bytes without taking them. A server can look at them to tell what a connection is before it decides who handles it. On a connection without TLS the bytes stay in the socket, so whatever reads next gets them, `acceptTls` and `copy` included:

```iris run
import net, str

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    for text in ["SSH-2.0-client\r\n", "GET / HTTP/1.1\r\n", "\x16\x03\x01"] {
        c := try net.dial("127.0.0.1", ln.port)
        try c.write(text)
        s := try ln.accept()
        head := try s.peek(3)
        kind := if str.startsWith(head, "SSH") {
            "ssh"
        } else if head.len > 0 && head[0] == 0x16 {
            "tls"
        } else {
            "http"
        }
        scoped String first = try s.readExactly(3)
        print("{kind}, and the reads still get its first {first.len} bytes")
        c.close()
        s.close()
    }
}
```

```output
ssh, and the reads still get its first 3 bytes
http, and the reads still get its first 3 bytes
tls, and the reads still get its first 3 bytes
```

`peekServerName` reads a TLS client's first message and gives the host name it asks for (SNI), with TLS still in place. One port can then send each name to its own backend with `copy`, and the backend does the TLS:

```iris
void route(net.Conn client) {
    defer client.close()
    client.setTimeout(5s)
    name := client.peekServerName() catch e {
        return
    }
    port := if name == "api.example.com" { 9001 } else { 9002 }
    backend := net.dial("127.0.0.1", u16(port)) catch e {
        return
    }
    defer backend.close()
    // copies both ways, as in the proxy above
}
```

## TLS

`net.dialTls` connects and starts TLS. The server's certificate must be valid for the host and signed by a certificate the system trusts, or one in a `ca` file. A client that a server asks for a certificate gives its own with `cert` and `key`:

```iris
c := try net.dialTls("example.com", 443)
defer c.close()
ca := "certs/ca.pem"
backend := try net.dialTls("db.internal", 5433, ca: ca, cert: "certs/proxy.pem", key: "certs/proxy.key")
```

A server makes a `TlsConfig` from a certificate and its key, and calls `acceptTls` on each connection. `addCert` adds certificates for other names, and each client gets the one for the name it asks for; `serverName` says which. `reload` reads the files again while the server runs, as after the certificates are renewed. `verifyClients` asks each client for a certificate signed by a given CA, and `peerSubject` says who the client is:

```iris
config := try net.tlsConfig("certs/example.com.pem", "certs/example.com.key")
try config.addCert("certs/wildcard.example.net.pem", "certs/wildcard.example.net.key")
try config.verifyClients("certs/clients-ca.pem")

conn := try ln.accept()
try conn.acceptTls(&config)
scoped String who = conn.peerSubject()
print("{conn.serverName()} for {who}")
```

## Unix sockets and passing sockets on

`listenUnix` and `dialUnix` work like `listen` and `dial`, on a path. Over a Unix socket, `sendFds` sends open file descriptors with some bytes, and `readFds` receives them. `adoptListener` and `adoptConn` turn a descriptor from elsewhere into a `Listener` or a `Conn`. A server hands its listening sockets to its next version this way, so a restart drops no connection:

```iris run
import fs, net, os

!void main() {
    scoped String path = format("/tmp/iris-tour-{os.pid()}.sock")
    defer fs.remove(path) catch e { }
    handover := try net.listenUnix(path)
    defer handover.close()
    tcp := try net.listen(0, "127.0.0.1")

    sender := try net.dialUnix(path)
    defer sender.close()
    try sender.sendFds("the listener", [tcp.fd()])

    receiver := try handover.accept()
    defer receiver.close()
    u8[64] buf
    i32[1] fds
    n, k := try receiver.readFds(buf, fds)
    took := try net.adoptListener(fds[0])
    print("{buf[0:n]}: {k} descriptor, same port: {took.port == tcp.port}")
}
```

```output
the listener: 1 descriptor, same port: true
```

For a descriptor used through `import c`, `net.waitRead` and `net.waitWrite` wait in the event loop until it is ready, so the other tasks run meanwhile.

The [net library page](../std/index.html#net) has the rest: UDP with `listenUdp` and batches of datagrams, names with `lookup` and `query`, and every function of the module.
