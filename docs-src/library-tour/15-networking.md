# Networking

The `net` module has TCP, UDP, TLS and Unix sockets. A call that waits for the network lets the other tasks on its thread run, so a server can hold many connections with one task each and no callbacks. This chapter starts with a server and a client and goes on to what a proxy needs: socket options, half-closing, copying between connections and looking at the first bytes.

## A server and a client

`net.listen` gives a `Listener`, and its `accept` waits for the next connection. `net.dial` connects. A `Conn` writes with `write` and reads with:

- `read`
- `readLine`
- `readExactly`
- `readAll`, which reads until the other side closes

Each connection the server accepts gets its own task.

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

A call that fails gives a `NetError`:

| Code | When |
|---|---|
| `refused` | nothing listens |
| `closed` | the other side went away |
| `timeout` | the wait ran out |
| `notFound` | a name doesn't resolve |
| `inUse` | the port is taken |

`dial` takes a timeout for connecting. `setTimeout` limits how long each read or write on a connection may wait.

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

Every connection from `net` starts with `TCP_NODELAY` on, so small writes go out at once. A `Conn` has methods for the common options:

- `setNoDelay`
- `setKeepAlive` with the idle time, the interval between probes, and how many may go unanswered
- `setBuffers` for the kernel's buffers

Any other int option goes through `setOption` and `option`, with the level and name C uses, taken from the system's headers with [import c](../tour/28-calling-c.html).

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

A Linux-only option like `TCP_KEEPIDLE` comes from `netinet/tcp.h` the same way: `conn.setOption(c.IPPROTO_TCP, c.TCP_KEEPIDLE, 30)`. `setOptionBytes` and `optionBytes` take an option whose value is a struct, as its bytes.

## Setting up a socket

Some options only take effect when set before a socket binds, listens or connects: `SO_REUSEPORT`, `IP_TRANSPARENT`, `TCP_FASTOPEN`, and buffer sizes large enough to change TCP's window. `net.socket` gives a `Socket` that accepts them. `bind` gives it an address. Then `listen` turns it into a `Listener` with room for a backlog of connections, or `connect` turns it into a `Conn`.

On a client, `bind` picks the address and port the connection comes from:

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

`net.Addr{port: 8080}` is every address on port 8080. A `Socket` is IPv6 and accepts IPv4 too. `Family.v4` and `Family.v6` give only one of them.

These options have methods of their own, so a program doesn't need the system's numbers for them:

- `setReusePort(true)` lets several sockets listen on one port, one per thread or process, and the kernel spreads the connections between them.
- `setTransparent(true)` is for a transparent proxy with TPROXY. A listener takes connections for addresses that aren't the machine's, and their `localAddr()` is where the client was going. A client socket can bind to the client's own address before it connects, so the backend sees that address.
- `setFreeBind(true)` binds to an address the machine doesn't have yet.
- `setFastOpen(queue)` lets clients that have connected before send data with their SYN; `setFastOpenConnect(true)` does it from a client.
- `setMark(mark)`, on a `Socket` or a `Conn`, sets the mark the packets carry, for policy routing.

On a connection, `setLinger(0s)` makes `close` reset it, as a proxy does to pass a reset on. `originalDst()` gives where a connection that iptables or nftables redirected was going: the address a transparent proxy forwards it to. `setTransparent`, `setFreeBind`, `setMark` and `originalDst` are Linux only, and transparent and mark need `CAP_NET_ADMIN`; elsewhere they return an error that says so.

## Half-closing

`closeWrite` ends this side's writes. The other side reads the end of the data, and this side can still read what comes back. A client that sends a whole request and then waits for the answer uses it to say it is done.

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

Over TLS, `closeWrite` first sends TLS's own end of data.

## Copying between connections

`net.copy(to, from)` copies what one connection reads to another until the first one's data ends, and returns the number of bytes. Small messages go through a buffer. On Linux, when neither connection uses TLS, a steady stream moves from socket to socket inside the kernel with `splice` and never reaches the program.

A TCP proxy copies both ways at once. `net.relay(a, b)` does that: it runs one copy in each direction until both sides have ended their writes, passes the end of each direction on with `closeWrite` while the other keeps flowing, and returns the bytes that went each way.

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

void proxy(net.Listener ln, u16 backendPort, chan[String] log) {
    client := ln.accept() catch e {
        return
    }
    defer client.close()

    server := net.dial("127.0.0.1", backendPort, timeout: 2s) catch e {
        return
    }
    defer server.close()

    up, down := net.relay(&client, &server, idle: 30s) catch e {
        log <- format("relay ended: {e.message}")
        return
    }
    log <- format("the proxy moved {up} bytes up and {down} bytes down")
}

!void main() {
    back := try net.listen(0, "127.0.0.1")
    spawn backend(back)

    front := try net.listen(0, "127.0.0.1")
    log := chan[String](1)
    defer log.free()
    spawn proxy(front, back.port, log)

    c := try net.dial("127.0.0.1", front.port)
    defer c.close()
    try c.write("hello\n")
    try c.closeWrite()

    scoped String reply = try c.readLine()
    print(reply)
    scoped String line = <-log
    print(line)
}
```

```output
hello, from the backend
the proxy moved 6 bytes up and 24 bytes down
```

`idle` ends the relay with `NetError.timeout` once no byte has moved either way for that long. A download that only goes one way keeps it going. An error in one direction ends both, and the relay doesn't close either connection.

`net.copy(to, from)` is one direction alone. Each of its waits is as long as its connection's `setTimeout` allows, and `limit` stops it after that many bytes. `received` and `sent` count the bytes each connection moved, even through an error, which is what a proxy logs.

## The first bytes

`peek(n)` gives the next n bytes without consuming them. A server can look at them to tell what a connection is before it decides who handles it. On a connection without TLS the bytes stay in the socket, so whatever reads next gets them, `acceptTls` and `copy` included.

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

`peekServerName` reads a TLS client's first message and gives the host name it asks for (SNI), with TLS still in place. One port can then send each name to its own backend with `copy`, and the backend does the TLS.

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

`net.dialTls` connects and starts TLS. The server's certificate must be valid for the host and signed by a certificate the system trusts, or by one in a `ca` file. When a server asks the client for a certificate, the client gives its own with `cert` and `key`.

```iris
c := try net.dialTls("example.com", 443)
defer c.close()

ca := "certs/ca.pem"
backend := try net.dialTls("db.internal", 5433, ca: ca, cert: "certs/proxy.pem", key: "certs/proxy.key")
```

A server makes a `TlsConfig` from a certificate and its key and calls `acceptTls` on each connection.

- `addCert` adds certificates for other names. Each client gets the one for the name it asks for, and `serverName` says which.
- `reload` reads the files again while the server runs, for example after the certificates are renewed.
- `verifyClients` asks each client for a certificate signed by a given CA, and `peerSubject` says who the client is.

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

`listenUnix` and `dialUnix` work like `listen` and `dial`, on a path. Over a Unix socket, `sendFds` sends open file descriptors along with some bytes, and `readFds` receives them. `adoptListener` and `adoptConn` turn a descriptor from elsewhere into a `Listener` or a `Conn`. A server uses this to hand its listening sockets to its next version, so a restart drops no connection. Once the new version accepts, the old one calls `interrupt` on its listeners: its accepts stop, while the socket stays open for the new version.

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

For a descriptor used through `import c`, `net.waitRead` and `net.waitWrite` wait in the event loop until it is ready, so other tasks run in the meantime.

The [net library page](../std/index.html#net) has the rest: names with `lookup` and `query`, and every function of the module.

`listenUnixDatagram(path)` is a Unix socket for messages, each sent and read whole, as syslog's `/dev/log` and systemd take them. `sendTo(data, path)` sends one, and `readFrom(buf)` reads one with the path of the socket it came from. `listenUnixDatagram()` without a path makes one that only sends, and `dialUnixDatagram(path)` one that sends to a single path with `send`. A path that starts with `@` is in Linux's abstract namespace and has no file.

`net.socketPair()` gives two connected sockets: what one writes, the other reads. A program hands one to a program it starts, as a kept descriptor, and keeps the other.

A service that systemd runs with `Type=notify` says it is ready with `net.notifySystemd("READY=1")`, and `"RELOADING=1"`, `"STOPPING=1"` or `"STATUS=..."` as it goes on. A server that hands its sockets to a new version sends `"MAINPID=..."` with the new process's id. It returns false when the program doesn't run under systemd.

## UDP and multicast

`net.listenUdp(port)` makes a socket for datagrams. `readFrom(buf)` reads one with the address it came from, and `sendTo(data, addr)` sends one. `dialUdp(addr)` ties a socket to one peer, so `send` and `read` need no address. `readMany` and `sendMany` move a batch of `Datagram` in one call. `sendTo` waits while the send buffer is full; `trySendTo(data, addr)` never waits and gives false instead, so a server answering a flood from addresses that don't reply drops answers rather than stalling.

A socket joins a multicast group with `joinGroup(addr, iface)`, where `addr` is a group such as `239.255.255.250` for SSDP or `224.0.0.251` for mDNS, and `iface` is the interface to join on (empty for the default). `leaveGroup` leaves it. `setMulticastHops(n)` sets how many routers a datagram may cross, `1` keeping it on the local network. `setMulticastLoop(on)` says whether what the socket sends also comes back to groups it joined on this machine, and `setMulticastInterface(iface)` picks the interface its multicast goes out of. `setBroadcast(true)` allows sending to the broadcast address.

`readMsg` and `sendMsg` carry more than the bytes. `readMsg(buf)` gives a `Message` with the sender in `from`, and with `setReceiveInfo(true)` the local address it came to in `to` and its TOS byte in `tos`, so a server on several addresses knows which one a datagram reached and can read ECN. `sendMsg(data, addr, from: ..., tos: ...)` sends from a chosen local address with a chosen TOS. On Linux `setGro(true)` lets one `readMsg` take several datagrams of one size at once, their size in `Message.segment`, and passing `segment` to `sendMsg` splits a large buffer into datagrams of that size on the way out.

```iris
import net

!void main() {
    u := try net.listenUdp(1900)
    defer u.close()
    group := try net.ip("239.255.255.250", 1900)
    try u.joinGroup(group, "")
    try u.setMulticastLoop(true)
    defer u.leaveGroup(group, "") catch e {}

    try u.sendTo("M-SEARCH", group)
    u8[2048] buf
    m := try u.readMsg(buf[:])
    print("{m.len} bytes from {m.from.format()}")
}
```

## TUN devices and the kernel's routes

A VPN or a network stack of its own gets packets from the kernel through a TUN device, a network interface whose other side is the program. `net.openTun(name)` makes one, Linux only and with CAP_NET_ADMIN. `read` gives one IP packet, and `write` hands one to the kernel as if it came in on the interface. `openTun(name, tap: true)` makes a TAP device, which carries Ethernet frames.

The `netlink` module sets up interfaces the way `ip` does, without running it. `netlink.addAddress("tun0", "10.99.0.1/24")` gives an interface an address, `linkUp` and `linkDown` bring it up and down, and `setMtu` sets its MTU. `links()`, `addresses()` and `routes()` list what the kernel has. `addRoute(netlink.Route{dest: "10.0.0.0/8", gateway: "10.99.0.2", dev: "tun0"})` and `removeRoute` change the routing table, in another table or with another protocol, such as `netlink.PROTO_BGP`, when the fields say so. `netlink.watch()` gives the changes as they happen: an `Event` for each link, address or route that comes or goes.

```iris
import net, netlink

!void main() {
    t := try net.openTun("tun0")
    defer t.close()
    try netlink.addAddress("tun0", "10.99.0.1/24")
    try netlink.linkUp("tun0")

    u8[2048] packet
    n := try t.read(packet[:])
    print("an IPv{packet[0] >> 4} packet of {n} bytes")
}
```
