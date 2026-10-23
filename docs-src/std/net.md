## A TCP server and client

`net.listen` takes a port and gives a `Listener`, whose `accept` waits for the next connection. Each connection usually gets a task of its own, so one slow client doesn't hold up the others. `net.dial` connects to a server. A `Conn` reads with `read`, `readLine` and `readExactly`, and writes with `write`:

```iris run
import net

void handle(net.Conn c) {
    defer c.close()
    for {
        line := c.readLine() catch e {
            return
        }
        c.write(format("echo: {line}\n")) catch e {
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
    try c.write("hello\nagain\n")
    scoped String a = try c.readLine()
    scoped String b = try c.readLine()
    print("{a} | {b}")
}
```

```output
echo: hello | echo: again
```

Port 0 lets the system pick a free port, which `ln.port` then holds. A host of `""` listens on every address. Calls wait as any wait in a task does: other tasks run until data comes.

## Who is connected

`remoteAddr` gives the address of the other side of a connection, the client on a server, and `localAddr` this side's. Both are an `Addr`, with `ipText` and `port`:

```iris run
import net

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    c := try net.dial("127.0.0.1", ln.port)
    defer c.close()
    s := try ln.accept()
    defer s.close()
    client := s.remoteAddr()
    scoped String ip = client.ipText()
    print("{ip} {client.port == c.localAddr().port}")
}
```

```output
127.0.0.1 true
```

`fd` gives a connection's file descriptor, for calls through `import c`. The event loop owns it, so reading, writing or closing it directly breaks the connection. `received` and `sent` count the bytes that came in and went out.

## Timeouts and errors

`setTimeout` sets how long each read or write on a connection may wait; past it, the call fails with `NetError.timeout`. `dial` takes a timeout for connecting. A connection to a port where nothing listens fails with `NetError.refused`:

```iris run
import net

void main() {
    _ = net.dial("127.0.0.1", 1) catch e {
        print("{e.code}")
        net.Conn{}
    }
}
```

```output
NetError.refused
```

`readLine` takes a limit on a line's length, and fails with `NetError.tooLong` past it, so a client can't make a server hold an endless line.

## Socket options

A `Conn` sets the common TCP options with methods: `setNoDelay` (on for every connection from `net`, so small writes go out at once), `setKeepAlive` with the idle time, the interval between probes and how many go unanswered before the connection drops, and `setBuffers` for the kernel's buffers. `setOption` and `option` set and read any option that is an int, by the level and name C uses, taken from the system's headers with `import c`:

```iris run
import net
import c "sys/socket.h"

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    conn := try net.dial("127.0.0.1", ln.port)
    defer conn.close()
    try conn.setKeepAlive(30s, 10s, 3)
    print("SO_KEEPALIVE: {try conn.option(c.SOL_SOCKET, c.SO_KEEPALIVE) != 0}")
    try conn.setOption(c.SOL_SOCKET, c.SO_KEEPALIVE, 0)
    print("SO_KEEPALIVE: {try conn.option(c.SOL_SOCKET, c.SO_KEEPALIVE) != 0}")
}
```

```output
SO_KEEPALIVE: true
SO_KEEPALIVE: false
```

`Listener` has `setOption` and `option` too. `setOptionBytes` and `optionBytes` take an option whose value is a struct, such as `SO_LINGER` or `TCP_INFO`, as its bytes. `net.setOption(fd, ...)` works on any socket's descriptor.

## Setting up a socket

Some options only count when they are set before a socket binds, listens or connects: `SO_REUSEPORT`, `IP_TRANSPARENT`, `TCP_FASTOPEN`, the buffer sizes that set TCP's window. `net.socket` gives a `Socket` for that. It takes options, then `bind` gives it an address, and `listen` turns it into a `Listener` or `connect` into a `Conn`. On a client, `bind` picks the address and port the connection comes from:

```iris run
import net

!void main() {
    s := try net.socket()
    try s.setBuffers(read: 1MiB)
    try s.bind(net.Addr{port: 0})
    ln := try s.listen(backlog: 4096)

    cs := try net.socket(net.Family.v4)
    try cs.bind(try net.ip("127.0.0.1", 0))
    conn := try cs.connect(try net.ip("127.0.0.1", ln.port))
    defer conn.close()
    srv := try ln.accept()
    defer srv.close()
    print("{srv.remoteAddr().port == conn.localAddr().port}")
}
```

```output
true
```

`net.Addr{port: 8080}` is every address. A `Socket` is IPv6 that takes IPv4 too, or `Family.v4` or `Family.v6` for one of them only. `net.listen` with a host of `""` listens on IPv6 and IPv4 at once.

## Half-closing and copying

`closeWrite` ends this side's writes: the other side reads the end of the data, while this side can still read what the other sends. A request that is "everything until you close" works that way, and so does a proxy, which passes on each direction's end on its own.

`net.copy(to, from)` copies what one connection reads to another until the first one's data ends, and gives the number of bytes. Small messages go through a buffer. On Linux, when neither connection uses TLS, a steady stream goes from socket to socket inside the kernel, with `splice`, without coming up to the program. A TCP proxy is two copies, one each way:

```iris run
import net

void upper(net.Listener ln) {
    c := ln.accept() catch e {
        return
    }
    defer c.close()
    scoped String line = c.readLine() catch e {
        return
    }
    c.write("{line} (from the backend)\n") catch e { }
}

void oneWay(net.Conn *to, net.Conn *from, chan[i64] done) {
    n := net.copy(to, from) catch e {
        done <- -1
        return
    }
    to.closeWrite() catch e { }
    done <- n
}

void proxy(net.Listener ln, u16 backend) {
    client := ln.accept() catch e {
        return
    }
    defer client.close()
    server := net.dial("127.0.0.1", backend, timeout: 2s) catch e {
        return
    }
    defer server.close()
    done := chan[i64](2)
    defer done.free()
    spawn oneWay(&server, &client, done)
    oneWay(&client, &server, done)
    _ = <-done
}

!void main() {
    back := try net.listen(0, "127.0.0.1")
    spawn upper(back)
    front := try net.listen(0, "127.0.0.1")
    spawn proxy(front, back.port)

    c := try net.dial("127.0.0.1", front.port)
    defer c.close()
    try c.write("hello\n")
    try c.closeWrite()
    scoped String reply = try c.readLine()
    print(reply)
}
```

```output
hello (from the backend)
```

Each copy waits for data as long as its connection's `setTimeout` allows, so a timeout there is an idle timeout. `limit` stops a copy after that many bytes. The counters `received` and `sent` keep counting through an error, for logging what a connection moved.

## Looking at the first bytes

`peek(n)` gives the next n bytes without taking them, so a server can tell what a connection is before it decides who handles it. On a connection without TLS the bytes stay in the socket, so `acceptTls` and `copy` still get them. `peekServerName` reads a TLS client's first message and gives the host name it asks for (SNI), so one port can send each name to its own backend with TLS still in place, or end TLS for some names and pass others on:

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

`net.helloServerName(record)` reads the name from a TLS record held elsewhere.

## TLS

`net.dialTls` connects and starts TLS, checking the server's certificate against the system's certificates, or the ones in a `ca` file. A server makes a `TlsConfig` from a certificate and its key and calls `acceptTls` on each connection:

```iris
c := try net.dialTls("example.com", 443)
defer c.close()
try c.write("HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
```

`addCert` gives a server more certificates. Each client gets the one with the name it asks for, wildcards like `*.example.com` included, and a client that asks for a name no certificate has gets the first. `serverName` says which name a client asked for. `reload` reads every certificate and key again from their files, as after they are renewed, while the server runs; connections that are open keep what they had, and a file that doesn't read leaves the old ones in place:

```iris
config := try net.tlsConfig("certs/example.com.pem", "certs/example.com.key")
try config.addCert("certs/api.example.org.pem", "certs/api.example.org.key")
try config.addCert("certs/wildcard.example.net.pem", "certs/wildcard.example.net.key")
// later, from a task that waits for SIGHUP
config.reload() catch e {
    log.error("certificates: {e.message}")
}
```

`verifyClients(ca)` makes a server ask each client for a certificate signed by one in `ca`, and turns away a client without one; with `required: false` a client may send none. `peerSubject` gives the subject of the certificate the other side showed. A client shows its own with `cert` and `key` on `dialTls` or `startTls`. `offerProtocols` sets the protocols a server offers with ALPN.

## UDP

`listenUdp` gives a socket that receives on a port, and `readFrom` gives each packet with the address it came from; `sendTo` sends one. `dialUdp` gives a socket that sends to one address with `send`:

```iris run
import net

!void main() {
    srv := try net.listenUdp(0, "127.0.0.1")
    defer srv.close()
    to := srv.localAddr()
    cl := try net.dialUdp(to)
    defer cl.close()
    try cl.send("ping")
    u8[64] buf
    n, from := try srv.readFrom(buf)
    print("{buf[0:n]} from 127.0.0.1: {from.isV4()}")
}
```

```output
ping from 127.0.0.1: true
```

Several sockets can listen on the same UDP port, such as one per thread. The kernel gives each datagram to one of them.

## Many datagrams at once

A busy UDP server spends much of its time in system calls, one per datagram. `readMany` waits for a datagram and then takes all that are waiting, up to the number of `Datagram`s it is given, with one system call on Linux. Each `Datagram` has the space for one datagram in `buf`; `readMany` sets its `len` and the `addr` it came from. `sendMany` sends a batch the same way, so a batch read can go back as it is:

```iris run
import net

!void main() {
    srv := try net.listenUdp(0, "127.0.0.1")
    defer srv.close()
    cl := try net.dialUdp(srv.localAddr())
    defer cl.close()
    for word in ["a", "bb", "ccc"] {
        try cl.send(word)
    }
    sleep(20ms)

    u8[4096] space
    net.Datagram[4] batch
    for i in 0..4 {
        batch[i].buf = space[i * 1024:(i + 1) * 1024]
    }
    n := try srv.readMany(batch)
    for d in batch[0:n] {
        print("{d.data()} ({d.len} bytes)")
    }
    try srv.sendMany(batch[0:n])
    sleep(20ms)
    u8[64] buf
    k := try cl.read(buf)
    print("back: {buf[0:k]}")
}
```

```output
a (1 bytes)
bb (2 bytes)
ccc (3 bytes)
back: a
```

## Names

`net.lookup` gives the addresses of a host name, with the system's resolver. `net.query` asks DNS for records of a kind, such as MX or TXT. A `Resolver` asks given servers instead of the system's. The [dns module](#dns) reads and writes the messages themselves, for a program that is a DNS server:

```iris
addrs := try net.lookup("example.com")
defer addrs.free()
for a in addrs {
    scoped String t = a.ipText()
    print(t)
}
```

## Unix sockets and passing sockets on

`listenUnix` and `dialUnix` work like `listen` and `dial`, on a path. On Linux, a path that starts with `@` has no file. Over a Unix socket, `sendFds` sends open file descriptors along with some bytes, and `readFds` receives them as new descriptors. A program that hands its listening sockets to the next version of itself, so a restart drops no connection, does it this way; `adoptListener` and `adoptConn` turn a descriptor from elsewhere into a `Listener` or a `Conn`:

```iris run
import fs, net, os

!void main() {
    scoped String path = format("/tmp/iris-doc-{os.pid()}.sock")
    defer fs.remove(path) catch e { }
    handover := try net.listenUnix(path)
    defer handover.close()
    tcp := try net.listen(0, "127.0.0.1")

    sender := try net.dialUnix(path)
    defer sender.close()
    try sender.sendFds("listener", [tcp.fd()])
    receiver := try handover.accept()
    defer receiver.close()
    u8[64] buf
    i32[1] fds
    n, k := try receiver.readFds(buf, fds)
    took := try net.adoptListener(fds[0])
    print("{buf[0:n]}: {k} descriptor, port kept: {took.port == tcp.port}")
}
```

```output
listener: 1 descriptor, port kept: true
```

`net.waitRead` and `net.waitWrite` wait in the event loop for any descriptor to be ready, letting the other tasks run, for calls through `import c` that would otherwise block the thread.

