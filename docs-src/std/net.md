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

`fd` gives a connection's file descriptor, for socket options and calls through `import c`. The event loop owns it, so reading, writing or closing it directly breaks the connection.

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

## TLS

`net.dialTls` connects and starts TLS, checking the server's certificate against the system's certificates, or the ones in a `ca` file. A server makes a `TlsConfig` from a certificate and its key and calls `acceptTls` on each connection:

```iris
c := try net.dialTls("example.com", 443)
defer c.close()
try c.write("HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
```

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
