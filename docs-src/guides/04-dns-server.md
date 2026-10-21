# Build a DNS server

This guide builds `dns` from an empty folder: a DNS server for the names of a small network. A program asks it for `web.lan` and gets the addresses back. The [dns module](../std/index.html#dns) reads the bytes of each query into a message and writes the reply back as bytes, so the program works with names, kinds and addresses. The server speaks UDP. Each step ends with a program that runs, and the commands to try it.

When it is finished, asking it looks like this:

```output
$ dig @127.0.0.1 -p 8080 +short web.lan
10.0.0.5
10.0.0.6
```

## Before you start

You need three things:

- Iris. `iris version` prints its version.
- `dig`, which asks DNS servers questions. macOS has it. On Debian and Ubuntu, `sudo apt install bind9-dnsutils` installs it. `command -v dig` prints where it is.
- `nc` (netcat), to send the server a message that isn't a query. macOS has it. On Debian and Ubuntu, `sudo apt install netcat-openbsd` installs it. `command -v nc` prints where it is.

Make a folder for the server and go into it:

```shell
mkdir dns
cd dns
```

Everything in this guide happens in this folder. The program is one file, `main.iris`, which each step changes. You'll use two terminals: one runs the server, and the other asks it questions.

DNS servers usually listen on port 53, which only root can use. This one listens on 8080, and `dig` is told the port with `-p 8080`.

## Reading a query

The first version reads each query, prints the name it asks for, and answers that the name doesn't exist. That is already a whole DNS server, one that knows no names.

Create `main.iris`. The `dns` module reads and writes DNS messages, and the `net` module has UDP:

```iris
// dns: a DNS server for the names of a small network.
```

```iris
import dns, net
```

`answer` handles one query. `dns.parse` reads the bytes that came in into a `dns.Message`: a header, then the questions, then three lists of records. A message from the network can be anything, so `parse` can fail, and `try` passes its error on. The [Errors](../tour/18-errors.html) chapter explains `!T`, `try` and `catch`.

Each question has a `name` and a `kind`, the type of record it asks for. A `dns.Type` prints as its name: `a` asks for IPv4 addresses and `aaaa` for IPv6 ones.

`query.reply()` starts the reply. It keeps the query's `id`, so the asker knows which question it answers, copies the questions and marks the message as a response. `authoritative` says this server is the one in charge of the names, and `rcode` 3 says the name doesn't exist. `encode` writes the reply as bytes, and `sendTo` sends them back to whoever asked. `scoped` frees the query, the reply and the bytes when `answer` ends.

```iris
// The answer to one query: for now, that no name exists.
!void answer(net.Udp *sock, u8[] msg, net.Addr from) {
    scoped query := try dns.parse(msg)
    for q in query.questions {
        print("{q.name} {q.kind}")
    }
    scoped reply := query.reply()
    reply.header.authoritative = true
    reply.header.rcode = 3
    scoped String out = try reply.encode()
    try sock.sendTo(out, from)
}
```

`main` opens UDP port 8080 with `net.listenUdp`. Each `readFrom` waits for one datagram, puts it in `buf` and gives its length and who sent it. A query that can't be read is printed and dropped, and the server goes on with the next one. The [net library page](../std/index.html#net) has the rest of UDP.

```iris
!void main() {
    sock := try net.listenUdp(8080)
    print("answering on 8080")
    u8[512] buf
    for {
        n, from := try sock.readFrom(buf[0:512])
        answer(&sock, buf[0:n], from) catch e {
            print("dropped a query: {e.message}")
        }
    }
}
```

```iris step
// dns: a DNS server for the names of a small network.

import dns, net

// The answer to one query: for now, that no name exists.
!void answer(net.Udp *sock, u8[] msg, net.Addr from) {
    scoped query := try dns.parse(msg)
    for q in query.questions {
        print("{q.name} {q.kind}")
    }
    scoped reply := query.reply()
    reply.header.authoritative = true
    reply.header.rcode = 3
    scoped String out = try reply.encode()
    try sock.sendTo(out, from)
}

!void main() {
    sock := try net.listenUdp(8080)
    print("answering on 8080")
    u8[512] buf
    for {
        n, from := try sock.readFrom(buf[0:512])
        answer(&sock, buf[0:n], from) catch e {
            print("dropped a query: {e.message}")
        }
    }
}
```

Start the server in the first terminal. It keeps running, answering queries:

```shell start
iris run main.iris
```

In the second terminal, ask it for `web.lan`. `dig` prints the whole answer, and `grep -o` picks out the status from it:

```shell try
dig @127.0.0.1 -p 8080 web.lan | grep -o 'status: [A-Z]*'
```

```output
status: NXDOMAIN
```

NXDOMAIN means there is no such name. The first terminal shows what was asked:

```output
answering on 8080
web.lan a
```

A message too short to be a query is dropped. `nc -u` sends some bytes over UDP:

```shell try
printf 'hi' | nc -u -w1 127.0.0.1 8080
```

The server prints `dropped a query: 2 bytes is shorter than a header` and goes on.

Stop the server with Ctrl-C in the first terminal. Do this at the end of every step, and start it again with the new code at the next.

## Answering with addresses

Now the server gets a list of names and their addresses, and answers with them. A `Host` is a name and one of its addresses. A `net.Addr` holds an IPv4 or an IPv6 address. A name with several addresses has several hosts.

```iris
// A name and one of its addresses.
struct Host {
    str name
    net.Addr addr
}
```

`answer` now takes the hosts, as a pointer to the list, and goes over them for each question. For each host with the name asked for, it adds a record to the reply's `answers` when the address is of the kind asked for. `host.addr.isV4()` says which kind it is.

A record is a `dns.Record`. It has the name it is for, its kind, its `ttl` (how long the asker may keep it), and its data, which for an address is `ip`. A `net.Addr` keeps its address in `ip` the same way, so `host.addr.ip` goes in as it is, and `encode` writes the 4 or the 16 bytes the kind needs. The record owns its name, so it gets a copy of the question's. When the reply is freed, its records are too.

A name that is there but has no address of that kind gets an answer with no records, and only a name that isn't there at all gets NXDOMAIN.

```iris
// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    scoped query := try dns.parse(msg)
    scoped reply := query.reply()
    reply.header.authoritative = true
    bool known = false
    for q in query.questions {
        found := reply.answers.len
        for host in zone.items() {
            if host.name != q.name {
                continue
            }
            known = true
            v4 := host.addr.isV4()
            if (q.kind == .a && v4) || (q.kind == .aaaa && !v4) {
                reply.answers.push(.{name: q.name.copy(), kind: q.kind, ttl: 5m, ip: host.addr.ip})
            }
        }
        print("{q.name} {q.kind}: {reply.answers.len - found} found")
    }
    reply.header.rcode = if known { 0 } else { 3 }
    scoped String out = try reply.encode()
    try sock.sendTo(out, from)
}
```

`main` makes the list. `net.ip` reads an address written as text, and gives an error for text that isn't one.

```iris
!void main() {
    scoped List[Host] zone
    zone.push(.{name: "web.lan", addr: try net.ip("10.0.0.5")})
    zone.push(.{name: "web.lan", addr: try net.ip("10.0.0.6")})
    zone.push(.{name: "web.lan", addr: try net.ip("fd00::5")})
    zone.push(.{name: "nas.lan", addr: try net.ip("10.0.0.20")})
    sock := try net.listenUdp(8080)
    print("answering on 8080")
    u8[512] buf
    for {
        n, from := try sock.readFrom(buf[0:512])
        answer(&sock, buf[0:n], from, &zone) catch e {
            print("dropped a query: {e.message}")
        }
    }
}
```

```iris step
// dns: a DNS server for the names of a small network.

import dns, net

// A name and one of its addresses.
struct Host {
    str name
    net.Addr addr
}

// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    scoped query := try dns.parse(msg)
    scoped reply := query.reply()
    reply.header.authoritative = true
    bool known = false
    for q in query.questions {
        found := reply.answers.len
        for host in zone.items() {
            if host.name != q.name {
                continue
            }
            known = true
            v4 := host.addr.isV4()
            if (q.kind == .a && v4) || (q.kind == .aaaa && !v4) {
                reply.answers.push(.{name: q.name.copy(), kind: q.kind, ttl: 5m, ip: host.addr.ip})
            }
        }
        print("{q.name} {q.kind}: {reply.answers.len - found} found")
    }
    reply.header.rcode = if known { 0 } else { 3 }
    scoped String out = try reply.encode()
    try sock.sendTo(out, from)
}

!void main() {
    scoped List[Host] zone
    zone.push(.{name: "web.lan", addr: try net.ip("10.0.0.5")})
    zone.push(.{name: "web.lan", addr: try net.ip("10.0.0.6")})
    zone.push(.{name: "web.lan", addr: try net.ip("fd00::5")})
    zone.push(.{name: "nas.lan", addr: try net.ip("10.0.0.20")})
    sock := try net.listenUdp(8080)
    print("answering on 8080")
    u8[512] buf
    for {
        n, from := try sock.readFrom(buf[0:512])
        answer(&sock, buf[0:n], from, &zone) catch e {
            print("dropped a query: {e.message}")
        }
    }
}
```

Start the server:

```shell start
iris run main.iris
```

Ask for the IPv4 addresses of `web.lan`, then its IPv6 one. `+short` makes `dig` print only the addresses:

```shell try
dig @127.0.0.1 -p 8080 +short web.lan
dig @127.0.0.1 -p 8080 +short web.lan AAAA
```

```output
10.0.0.5
10.0.0.6
fd00::5
```

A name that isn't in the list still gets NXDOMAIN:

```shell try
dig @127.0.0.1 -p 8080 db.lan | grep -o 'status: [A-Z]*'
```

```output
status: NXDOMAIN
```

## Names from a file

The names are in the code, so changing one means building the server again. Now they come from a file, `zone.txt`, read when the server starts. Make it in the second terminal. `cat > zone.txt` writes what follows, up to `EOF`, into the file:

```shell try
cat > zone.txt <<'EOF'
# the names of the home network
web.lan     10.0.0.5
web.lan     10.0.0.6
web.lan     fd00::5
NAS.lan     10.0.0.20
printer.lan 10.0.0.30   # by the door
EOF
```

Each line is a name and an address, and `#` starts a comment. Names in DNS are the same in any case, so `NAS.lan` and `nas.lan` are one name. The server keeps every name in small letters, and puts the name of each question in small letters too.

A line that isn't a name and an address is an error. `ZoneError` is what it fails with:

```iris
enum ZoneError {
    badLine
}
```

`loadZone` reads the file's text into hosts. `split` gives its lines, `cut` takes off a comment, and `fields` gives the words of what is left. A line with no words is skipped. A line that isn't a name and an address gives an error that says which line. The hosts' names point into the text, so the text has to stay as long as the hosts do. The [Text](../tour/06-text.html) chapter has `split`, `cut` and `fields`.

```iris
// Reads the zone: a name and an address on each line, and # starts a
// comment. A name with several addresses is on several lines.
!List[Host] loadZone(str text) {
    List[Host] zone
    errdefer zone.free()
    scoped List[str] lines = text.split("\n")
    for i, raw in lines.items() {
        line, _, _ := raw.cut("#")
        scoped List[str] parts = line.fields()
        if parts.len == 0 {
            continue
        }
        if parts.len != 2 {
            return error(ZoneError.badLine, "line {i + 1}: write a name and an address")
        }
        addr := net.ip(parts.at(1)) catch e {
            return error(ZoneError.badLine, "line {i + 1}: {e.message}")
        }
        zone.push(.{name: parts.at(0), addr: addr})
    }
    return zone
}
```

The `import` line gets `fs`, which reads files:

```iris
import dns, fs, net
```

`answer` puts the question's name in small letters with `lower`, and looks for that name in the hosts. The record keeps the name as it was asked, which is what the asker expects to see.

```iris
// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    scoped query := try dns.parse(msg)
    scoped reply := query.reply()
    reply.header.authoritative = true
    bool known = false
    for q in query.questions {
        // names are the same in any case: WEB.lan is web.lan
        scoped String name = q.name.lower()
        found := reply.answers.len
        for host in zone.items() {
            if host.name != name {
                continue
            }
            known = true
            v4 := host.addr.isV4()
            if (q.kind == .a && v4) || (q.kind == .aaaa && !v4) {
                reply.answers.push(.{name: q.name.copy(), kind: q.kind, ttl: 5m, ip: host.addr.ip})
            }
        }
        print("{q.name} {q.kind}: {reply.answers.len - found} found")
    }
    reply.header.rcode = if known { 0 } else { 3 }
    scoped String out = try reply.encode()
    try sock.sendTo(out, from)
}
```

`main` reads the file with `fs.readAll`, puts it in small letters, and gives the text to `loadZone`. Both texts are `scoped`, so they stay until `main` ends, as long as the server runs. A missing file or a bad line stops the server before it starts, with a message that says why.

```iris
!void main() {
    scoped String file = try fs.readAll("zone.txt")
    scoped String text = file.lower()
    scoped List[Host] zone = try loadZone(text)
    sock := try net.listenUdp(8080)
    print("{zone.len} addresses, answering on 8080")
    u8[512] buf
    for {
        n, from := try sock.readFrom(buf[0:512])
        answer(&sock, buf[0:n], from, &zone) catch e {
            print("dropped a query: {e.message}")
        }
    }
}
```

```iris step
// dns: a DNS server for the names of a small network, kept in zone.txt.

import dns, fs, net

// A name and one of its addresses.
struct Host {
    str name
    net.Addr addr
}

enum ZoneError {
    badLine
}

// Reads the zone: a name and an address on each line, and # starts a
// comment. A name with several addresses is on several lines.
!List[Host] loadZone(str text) {
    List[Host] zone
    errdefer zone.free()
    scoped List[str] lines = text.split("\n")
    for i, raw in lines.items() {
        line, _, _ := raw.cut("#")
        scoped List[str] parts = line.fields()
        if parts.len == 0 {
            continue
        }
        if parts.len != 2 {
            return error(ZoneError.badLine, "line {i + 1}: write a name and an address")
        }
        addr := net.ip(parts.at(1)) catch e {
            return error(ZoneError.badLine, "line {i + 1}: {e.message}")
        }
        zone.push(.{name: parts.at(0), addr: addr})
    }
    return zone
}

// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    scoped query := try dns.parse(msg)
    scoped reply := query.reply()
    reply.header.authoritative = true
    bool known = false
    for q in query.questions {
        // names are the same in any case: WEB.lan is web.lan
        scoped String name = q.name.lower()
        found := reply.answers.len
        for host in zone.items() {
            if host.name != name {
                continue
            }
            known = true
            v4 := host.addr.isV4()
            if (q.kind == .a && v4) || (q.kind == .aaaa && !v4) {
                reply.answers.push(.{name: q.name.copy(), kind: q.kind, ttl: 5m, ip: host.addr.ip})
            }
        }
        print("{q.name} {q.kind}: {reply.answers.len - found} found")
    }
    reply.header.rcode = if known { 0 } else { 3 }
    scoped String out = try reply.encode()
    try sock.sendTo(out, from)
}

!void main() {
    scoped String file = try fs.readAll("zone.txt")
    scoped String text = file.lower()
    scoped List[Host] zone = try loadZone(text)
    sock := try net.listenUdp(8080)
    print("{zone.len} addresses, answering on 8080")
    u8[512] buf
    for {
        n, from := try sock.readFrom(buf[0:512])
        answer(&sock, buf[0:n], from, &zone) catch e {
            print("dropped a query: {e.message}")
        }
    }
}
```

Start the server:

```shell start
iris run main.iris
```

The first terminal says how many addresses it read:

```output
5 addresses, answering on 8080
```

Ask for names in any case:

```shell try
dig @127.0.0.1 -p 8080 +short WEB.lan
dig @127.0.0.1 -p 8080 +short nas.LAN
dig @127.0.0.1 -p 8080 +short printer.lan
```

```output
10.0.0.5
10.0.0.6
10.0.0.20
10.0.0.30
```

`printer.lan` has no IPv6 address, so an AAAA query gets no records, and its status says the name is there:

```shell try
dig @127.0.0.1 -p 8080 printer.lan AAAA | grep -o 'status: [A-Z]*'
dig @127.0.0.1 -p 8080 +short printer.lan AAAA
```

```output
status: NOERROR
```

To change a name, edit `zone.txt` and start the server again.

## The whole program

```iris whole
// dns: a DNS server for the names of a small network, kept in zone.txt.

import dns, fs, net

// A name and one of its addresses.
struct Host {
    str name
    net.Addr addr
}

enum ZoneError {
    badLine
}

// Reads the zone: a name and an address on each line, and # starts a
// comment. A name with several addresses is on several lines.
!List[Host] loadZone(str text) {
    List[Host] zone
    errdefer zone.free()
    scoped List[str] lines = text.split("\n")
    for i, raw in lines.items() {
        line, _, _ := raw.cut("#")
        scoped List[str] parts = line.fields()
        if parts.len == 0 {
            continue
        }
        if parts.len != 2 {
            return error(ZoneError.badLine, "line {i + 1}: write a name and an address")
        }
        addr := net.ip(parts.at(1)) catch e {
            return error(ZoneError.badLine, "line {i + 1}: {e.message}")
        }
        zone.push(.{name: parts.at(0), addr: addr})
    }
    return zone
}

// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    scoped query := try dns.parse(msg)
    scoped reply := query.reply()
    reply.header.authoritative = true
    bool known = false
    for q in query.questions {
        // names are the same in any case: WEB.lan is web.lan
        scoped String name = q.name.lower()
        found := reply.answers.len
        for host in zone.items() {
            if host.name != name {
                continue
            }
            known = true
            v4 := host.addr.isV4()
            if (q.kind == .a && v4) || (q.kind == .aaaa && !v4) {
                reply.answers.push(.{name: q.name.copy(), kind: q.kind, ttl: 5m, ip: host.addr.ip})
            }
        }
        print("{q.name} {q.kind}: {reply.answers.len - found} found")
    }
    reply.header.rcode = if known { 0 } else { 3 }
    scoped String out = try reply.encode()
    try sock.sendTo(out, from)
}

!void main() {
    scoped String file = try fs.readAll("zone.txt")
    scoped String text = file.lower()
    scoped List[Host] zone = try loadZone(text)
    sock := try net.listenUdp(8080)
    print("{zone.len} addresses, answering on 8080")
    u8[512] buf
    for {
        n, from := try sock.readFrom(buf[0:512])
        answer(&sock, buf[0:n], from, &zone) catch e {
            print("dropped a query: {e.message}")
        }
    }
}
```

## Next

The server reads and writes DNS messages with the `dns` module:

- `dns.parse` turns the bytes of a query into a `dns.Message`, and checks every length against the end of the message.
- `reply` starts the answer to a query, and `encode` writes a message as bytes, with the names in it written once and pointed at after that.
- UDP with `net.listenUdp`, `readFrom` and `sendTo`.

To run it for real, build it once with optimisations and start the program it makes:

```shell
iris build --mode=release-safe main.iris -o dns
./dns
```

Some ways to go on: a `cname` record, whose `target` is another name, lets one name stand for another. A name the zone doesn't have could be passed on to another server with `net.dialUdp`, and its answer sent back as it came. Reading `zone.txt` again on a signal, with the `signal` module, lets the names change while the server runs. The [dns library page](../std/index.html#dns) lists every kind of record and its fields, and the [net library page](../std/index.html#net) the rest of UDP.
