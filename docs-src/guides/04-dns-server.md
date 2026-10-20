# Build a DNS server

This guide builds `dns` from an empty folder: a DNS server for the names of a small network. A program asks it for `web.lan` and gets the addresses back. It shows how a program reads and writes a binary protocol. The header of a DNS message is a `packed` struct with its fields in network byte order and some of them a few bits wide, so the program reads the bytes that came in as they are, with no parsing by hand. The server speaks UDP. Each step ends with a program that runs, and the commands to try it.

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

Create `main.iris`. The `net` module has UDP:

```iris
// dns: a DNS server for the names of a small network.
```

```iris
import net
```

A DNS message starts with a header of 12 bytes ([RFC 1035](https://www.rfc-editor.org/rfc/rfc1035#section-4.1.1) draws it). `Header` has the same fields in the same order. `packed` keeps them right after each other, with no bytes between. `u16be` is a 16-bit number kept with its high byte first, as the network sends it. Reading a `u16be` field gives a plain `u16`, and setting one turns the bytes around for you. The second pair of bytes holds eight values of a few bits each. A field with `: n` after its name is n bits wide, and the first of them takes the highest bits, as in the drawing. The [Structs](../tour/13-structs.html) chapter has the rules.

```iris
// The 12 bytes at the start of every DNS message (RFC 1035, 4.1.1).
struct Header packed {
    u16be id
    u16be response: 1
    u16be opcode: 4
    u16be authoritative: 1
    u16be truncated: 1
    u16be recursionWanted: 1
    u16be recursionAvailable: 1
    u16be zero: 3
    u16be rcode: 4
    u16be questions
    u16be answers
    u16be authorities
    u16be additionals
}
```

After the header comes the question. It is a name and then two numbers, its type and its class. `QuestionEnd` is those two numbers. Its first field is `kind`, since `type` is a word Iris keeps for itself. Type 1 asks for IPv4 addresses.

```iris
// What follows the name in a question.
struct QuestionEnd packed {
    u16be kind
    u16be class
}
```

A message from the network can be anything, so reading one can fail. `DnsError` says how. The [Errors](../tour/18-errors.html) chapter explains `!T`, `error` and `catch`.

```iris
enum DnsError {
    short
    badName
}
```

`Question` is the question as the server uses it. The name is a `String` the question owns, so `Question` has a `free` method that frees it. `end` is where the question ends in the message. The answer repeats the question, so it needs to know.

```iris
// What follows the name in a question.
struct QuestionEnd packed {
    u16be kind
    u16be class
}

void Question.free() => self.name.free()
```

`readQuestion` reads the name, which comes as labels. Each label is a length byte and that many bytes, and a zero ends the name: `3 web 3 lan 0` is `web.lan`. Every length is checked against the end of the message before anything is read, so a short or broken message gives an error. `errdefer` frees the name when the function gives an error, since nothing else will.

`sizeof(Header)` is 12, the bytes the header takes. After the name, `msg[at:at + sizeof(QuestionEnd)].ptr` is a pointer to the next four bytes. A `u8 *` converts to any pointer, so it becomes a `QuestionEnd *`, and `e.kind` reads the type from the bytes as they are.

```iris
// Reads the question that follows the header. A name is a run of labels,
// each a length byte and that many bytes, ended by a zero.
!Question readQuestion(u8[] msg) {
    Question q
    errdefer q.name.free()
    at := sizeof(Header)
    for {
        if at >= msg.len {
            return error(DnsError.short, "the name runs past the end")
        }
        n := i64(msg[at])
        at++
        if n == 0 {
            break
        }
        if n > 63 || at + n > msg.len {
            return error(DnsError.badName, "a label is too long")
        }
        if q.name.len > 0 {
            q.name.append(".")
        }
        q.name.append(msg[at:at + n])
        at += n
    }
    if at + sizeof(QuestionEnd) > msg.len {
        return error(DnsError.short, "the question has no type")
    }
    QuestionEnd *e = msg[at:at + sizeof(QuestionEnd)].ptr
    q.kind = e.kind
    q.end = at + sizeof(QuestionEnd)
    return q
}
```

`answer` handles one query. It checks the message is at least a header long, and `Header *h = msg.ptr` reads it in place. The reply starts as a copy of the header. It keeps the query's `id`, so the asker knows which question it answers. `response` says it is an answer, `authoritative` that this server is the one in charge of the name, and `rcode` 3 says the name doesn't exist. Then `&reply` is a pointer to the reply's bytes, and `bytes[0:sizeof(Header)]` is the header as 12 bytes. The question follows, copied from the query, and `sendTo` sends it all back to whoever asked. `scoped` frees the question and the reply when `answer` ends.

```iris
// The answer to one query: for now, that no name exists.
!void answer(net.Udp *sock, u8[] msg, net.Addr from) {
    if msg.len < sizeof(Header) {
        return error(DnsError.short, "{msg.len} bytes is shorter than a header")
    }
    Header *h = msg.ptr
    scoped q := try readQuestion(msg)
    print("{q.name} type {q.kind}")
    Header reply = *h
    reply.response = 1
    reply.authoritative = 1
    reply.recursionAvailable = 0
    reply.rcode = 3
    reply.answers = 0
    reply.authorities = 0
    reply.additionals = 0
    reply.questions = 1
    u8 *bytes = &reply
    scoped String out
    out.append(bytes[0:sizeof(Header)])
    out.append(msg[sizeof(Header):q.end])
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

import net

// The 12 bytes at the start of every DNS message (RFC 1035, 4.1.1).
struct Header packed {
    u16be id
    u16be response: 1
    u16be opcode: 4
    u16be authoritative: 1
    u16be truncated: 1
    u16be recursionWanted: 1
    u16be recursionAvailable: 1
    u16be zero: 3
    u16be rcode: 4
    u16be questions
    u16be answers
    u16be authorities
    u16be additionals
}

// What follows the name in a question.
struct QuestionEnd packed {
    u16be kind
    u16be class
}

enum DnsError {
    short
    badName
}

// The question of a query: the name asked for, its type, and where the
// question ends in the message.
struct Question {
    String name
    u16 kind
    i64 end
}

void Question.free() => self.name.free()

// Reads the question that follows the header. A name is a run of labels,
// each a length byte and that many bytes, ended by a zero.
!Question readQuestion(u8[] msg) {
    Question q
    errdefer q.name.free()
    at := sizeof(Header)
    for {
        if at >= msg.len {
            return error(DnsError.short, "the name runs past the end")
        }
        n := i64(msg[at])
        at++
        if n == 0 {
            break
        }
        if n > 63 || at + n > msg.len {
            return error(DnsError.badName, "a label is too long")
        }
        if q.name.len > 0 {
            q.name.append(".")
        }
        q.name.append(msg[at:at + n])
        at += n
    }
    if at + sizeof(QuestionEnd) > msg.len {
        return error(DnsError.short, "the question has no type")
    }
    QuestionEnd *e = msg[at:at + sizeof(QuestionEnd)].ptr
    q.kind = e.kind
    q.end = at + sizeof(QuestionEnd)
    return q
}

// The answer to one query: for now, that no name exists.
!void answer(net.Udp *sock, u8[] msg, net.Addr from) {
    if msg.len < sizeof(Header) {
        return error(DnsError.short, "{msg.len} bytes is shorter than a header")
    }
    Header *h = msg.ptr
    scoped q := try readQuestion(msg)
    print("{q.name} type {q.kind}")
    Header reply = *h
    reply.response = 1
    reply.authoritative = 1
    reply.recursionAvailable = 0
    reply.rcode = 3
    reply.answers = 0
    reply.authorities = 0
    reply.additionals = 0
    reply.questions = 1
    u8 *bytes = &reply
    scoped String out
    out.append(bytes[0:sizeof(Header)])
    out.append(msg[sizeof(Header):q.end])
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
web.lan type 1
```

A message too short to be a query is dropped. `nc -u` sends some bytes over UDP:

```shell try
printf 'hi' | nc -u -w1 127.0.0.1 8080
```

The server prints `dropped a query: 2 bytes is shorter than a header` and goes on.

Stop the server with Ctrl-C in the first terminal. Do this at the end of every step, and start it again with the new code at the next.

## Answering with addresses

Now the server gets a list of names and their addresses, and answers with them. Each address in an answer is a record. Before the address come five numbers ([RFC 1035, 4.1.3](https://www.rfc-editor.org/rfc/rfc1035#section-4.1.3)), and `Record` is those numbers, packed like the header. `ttl` is how many seconds the asker may keep the answer, and `length` how many bytes the address takes. `name` says which name the record is for. The name is in the question already, so `name` points at it instead of writing it again.

```iris
// What follows the name in an answer, before the address (RFC 1035, 4.1.3).
struct Record packed {
    u16be name
    u16be kind
    u16be class
    u32be ttl
    u16be length
}
```

A `Host` is a name and one of its addresses. A `net.Addr` holds an IPv4 or an IPv6 address. A name with several addresses has several hosts.

```iris
// A name and one of its addresses.
struct Host {
    str name
    net.Addr addr
}
```

`A` and `AAAA` are the types of a query for IPv4 and for IPv6 addresses:

```iris
const A = 1
const AAAA = 28
```

`answer` now takes the hosts, as a pointer to the list, and goes over them. For each host with the name asked for, it adds a record when the address is of the type asked for. `host.addr.isV4()` says which kind it is. A `net.Addr` keeps an IPv4 address in its last 4 bytes, so `ip[12:16]` is the address to send. A name that is there but has no address of that type gets an answer with no records, and only a name that isn't there at all gets NXDOMAIN. `0xc00c` is a pointer to byte 12, where the question's name starts: the top two bits say it is a pointer. The header says how many records follow.

```iris
// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    if msg.len < sizeof(Header) {
        return error(DnsError.short, "{msg.len} bytes is shorter than a header")
    }
    Header *h = msg.ptr
    scoped q := try readQuestion(msg)
    scoped String records
    i64 count = 0
    bool known = false
    for host in zone.items() {
        if host.name != q.name {
            continue
        }
        known = true
        v4 := host.addr.isV4()
        if (q.kind == A && v4) || (q.kind == AAAA && !v4) {
            ip := if v4 { host.addr.ip[12:16] } else { host.addr.ip[0:16] }
            // 0xc00c points at the name in the question, 12 bytes in
            Record r = .{name: 0xc00c, kind: q.kind, class: 1, ttl: 300, length: u16(ip.len)}
            u8 *rb = &r
            records.append(rb[0:sizeof(Record)])
            records.append(ip)
            count++
        }
    }
    print("{q.name} type {q.kind}: {count} found")
    Header reply = *h
    reply.response = 1
    reply.authoritative = 1
    reply.recursionAvailable = 0
    reply.rcode = if known { 0 } else { 3 }
    reply.questions = 1
    reply.answers = u16(count)
    reply.authorities = 0
    reply.additionals = 0
    u8 *bytes = &reply
    scoped String out
    out.append(bytes[0:sizeof(Header)])
    out.append(msg[sizeof(Header):q.end])
    out.append(records)
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

import net

// The 12 bytes at the start of every DNS message (RFC 1035, 4.1.1).
struct Header packed {
    u16be id
    u16be response: 1
    u16be opcode: 4
    u16be authoritative: 1
    u16be truncated: 1
    u16be recursionWanted: 1
    u16be recursionAvailable: 1
    u16be zero: 3
    u16be rcode: 4
    u16be questions
    u16be answers
    u16be authorities
    u16be additionals
}

// What follows the name in a question.
struct QuestionEnd packed {
    u16be kind
    u16be class
}

// What follows the name in an answer, before the address (RFC 1035, 4.1.3).
struct Record packed {
    u16be name
    u16be kind
    u16be class
    u32be ttl
    u16be length
}

// A name and one of its addresses.
struct Host {
    str name
    net.Addr addr
}

enum DnsError {
    short
    badName
}

// The question of a query: the name asked for, its type, and where the
// question ends in the message.
struct Question {
    String name
    u16 kind
    i64 end
}

void Question.free() => self.name.free()

// Reads the question that follows the header. A name is a run of labels,
// each a length byte and that many bytes, ended by a zero.
!Question readQuestion(u8[] msg) {
    Question q
    errdefer q.name.free()
    at := sizeof(Header)
    for {
        if at >= msg.len {
            return error(DnsError.short, "the name runs past the end")
        }
        n := i64(msg[at])
        at++
        if n == 0 {
            break
        }
        if n > 63 || at + n > msg.len {
            return error(DnsError.badName, "a label is too long")
        }
        if q.name.len > 0 {
            q.name.append(".")
        }
        q.name.append(msg[at:at + n])
        at += n
    }
    if at + sizeof(QuestionEnd) > msg.len {
        return error(DnsError.short, "the question has no type")
    }
    QuestionEnd *e = msg[at:at + sizeof(QuestionEnd)].ptr
    q.kind = e.kind
    q.end = at + sizeof(QuestionEnd)
    return q
}

const A = 1
const AAAA = 28

// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    if msg.len < sizeof(Header) {
        return error(DnsError.short, "{msg.len} bytes is shorter than a header")
    }
    Header *h = msg.ptr
    scoped q := try readQuestion(msg)
    scoped String records
    i64 count = 0
    bool known = false
    for host in zone.items() {
        if host.name != q.name {
            continue
        }
        known = true
        v4 := host.addr.isV4()
        if (q.kind == A && v4) || (q.kind == AAAA && !v4) {
            ip := if v4 { host.addr.ip[12:16] } else { host.addr.ip[0:16] }
            // 0xc00c points at the name in the question, 12 bytes in
            Record r = .{name: 0xc00c, kind: q.kind, class: 1, ttl: 300, length: u16(ip.len)}
            u8 *rb = &r
            records.append(rb[0:sizeof(Record)])
            records.append(ip)
            count++
        }
    }
    print("{q.name} type {q.kind}: {count} found")
    Header reply = *h
    reply.response = 1
    reply.authoritative = 1
    reply.recursionAvailable = 0
    reply.rcode = if known { 0 } else { 3 }
    reply.questions = 1
    reply.answers = u16(count)
    reply.authorities = 0
    reply.additionals = 0
    u8 *bytes = &reply
    scoped String out
    out.append(bytes[0:sizeof(Header)])
    out.append(msg[sizeof(Header):q.end])
    out.append(records)
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
            return error(DnsError.badZone, "line {i + 1}: write a name and an address")
        }
        addr := net.ip(parts.at(1)) catch e {
            return error(DnsError.badZone, "line {i + 1}: {e.message}")
        }
        zone.push(.{name: parts.at(0), addr: addr})
    }
    return zone
}
```

The `import` line gets `fs`, which reads files:

```iris
import fs, net
```

`DnsError` gets `badZone`, for a line `loadZone` can't read:

```iris
enum DnsError {
    short
    badName
    badZone
}
```

`answer` puts the question's name in small letters with `lower`, and looks for that name in the hosts.

```iris
// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    if msg.len < sizeof(Header) {
        return error(DnsError.short, "{msg.len} bytes is shorter than a header")
    }
    Header *h = msg.ptr
    scoped q := try readQuestion(msg)
    // names are the same in any case: WEB.lan is web.lan
    scoped String name = q.name.lower()
    scoped String records
    i64 count = 0
    bool known = false
    for host in zone.items() {
        if host.name != name {
            continue
        }
        known = true
        v4 := host.addr.isV4()
        if (q.kind == A && v4) || (q.kind == AAAA && !v4) {
            ip := if v4 { host.addr.ip[12:16] } else { host.addr.ip[0:16] }
            // 0xc00c points at the name in the question, 12 bytes in
            Record r = .{name: 0xc00c, kind: q.kind, class: 1, ttl: 300, length: u16(ip.len)}
            u8 *rb = &r
            records.append(rb[0:sizeof(Record)])
            records.append(ip)
            count++
        }
    }
    print("{q.name} type {q.kind}: {count} found")
    Header reply = *h
    reply.response = 1
    reply.authoritative = 1
    reply.recursionAvailable = 0
    reply.rcode = if known { 0 } else { 3 }
    reply.questions = 1
    reply.answers = u16(count)
    reply.authorities = 0
    reply.additionals = 0
    u8 *bytes = &reply
    scoped String out
    out.append(bytes[0:sizeof(Header)])
    out.append(msg[sizeof(Header):q.end])
    out.append(records)
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

import fs, net

// The 12 bytes at the start of every DNS message (RFC 1035, 4.1.1).
struct Header packed {
    u16be id
    u16be response: 1
    u16be opcode: 4
    u16be authoritative: 1
    u16be truncated: 1
    u16be recursionWanted: 1
    u16be recursionAvailable: 1
    u16be zero: 3
    u16be rcode: 4
    u16be questions
    u16be answers
    u16be authorities
    u16be additionals
}

// What follows the name in a question.
struct QuestionEnd packed {
    u16be kind
    u16be class
}

// What follows the name in an answer, before the address (RFC 1035, 4.1.3).
struct Record packed {
    u16be name
    u16be kind
    u16be class
    u32be ttl
    u16be length
}

// A name and one of its addresses.
struct Host {
    str name
    net.Addr addr
}

enum DnsError {
    short
    badName
    badZone
}

// The question of a query: the name asked for, its type, and where the
// question ends in the message.
struct Question {
    String name
    u16 kind
    i64 end
}

void Question.free() => self.name.free()

// Reads the question that follows the header. A name is a run of labels,
// each a length byte and that many bytes, ended by a zero.
!Question readQuestion(u8[] msg) {
    Question q
    errdefer q.name.free()
    at := sizeof(Header)
    for {
        if at >= msg.len {
            return error(DnsError.short, "the name runs past the end")
        }
        n := i64(msg[at])
        at++
        if n == 0 {
            break
        }
        if n > 63 || at + n > msg.len {
            return error(DnsError.badName, "a label is too long")
        }
        if q.name.len > 0 {
            q.name.append(".")
        }
        q.name.append(msg[at:at + n])
        at += n
    }
    if at + sizeof(QuestionEnd) > msg.len {
        return error(DnsError.short, "the question has no type")
    }
    QuestionEnd *e = msg[at:at + sizeof(QuestionEnd)].ptr
    q.kind = e.kind
    q.end = at + sizeof(QuestionEnd)
    return q
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
            return error(DnsError.badZone, "line {i + 1}: write a name and an address")
        }
        addr := net.ip(parts.at(1)) catch e {
            return error(DnsError.badZone, "line {i + 1}: {e.message}")
        }
        zone.push(.{name: parts.at(0), addr: addr})
    }
    return zone
}

const A = 1
const AAAA = 28

// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    if msg.len < sizeof(Header) {
        return error(DnsError.short, "{msg.len} bytes is shorter than a header")
    }
    Header *h = msg.ptr
    scoped q := try readQuestion(msg)
    // names are the same in any case: WEB.lan is web.lan
    scoped String name = q.name.lower()
    scoped String records
    i64 count = 0
    bool known = false
    for host in zone.items() {
        if host.name != name {
            continue
        }
        known = true
        v4 := host.addr.isV4()
        if (q.kind == A && v4) || (q.kind == AAAA && !v4) {
            ip := if v4 { host.addr.ip[12:16] } else { host.addr.ip[0:16] }
            // 0xc00c points at the name in the question, 12 bytes in
            Record r = .{name: 0xc00c, kind: q.kind, class: 1, ttl: 300, length: u16(ip.len)}
            u8 *rb = &r
            records.append(rb[0:sizeof(Record)])
            records.append(ip)
            count++
        }
    }
    print("{q.name} type {q.kind}: {count} found")
    Header reply = *h
    reply.response = 1
    reply.authoritative = 1
    reply.recursionAvailable = 0
    reply.rcode = if known { 0 } else { 3 }
    reply.questions = 1
    reply.answers = u16(count)
    reply.authorities = 0
    reply.additionals = 0
    u8 *bytes = &reply
    scoped String out
    out.append(bytes[0:sizeof(Header)])
    out.append(msg[sizeof(Header):q.end])
    out.append(records)
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

import fs, net

// The 12 bytes at the start of every DNS message (RFC 1035, 4.1.1).
struct Header packed {
    u16be id
    u16be response: 1
    u16be opcode: 4
    u16be authoritative: 1
    u16be truncated: 1
    u16be recursionWanted: 1
    u16be recursionAvailable: 1
    u16be zero: 3
    u16be rcode: 4
    u16be questions
    u16be answers
    u16be authorities
    u16be additionals
}

// What follows the name in a question.
struct QuestionEnd packed {
    u16be kind
    u16be class
}

// What follows the name in an answer, before the address (RFC 1035, 4.1.3).
struct Record packed {
    u16be name
    u16be kind
    u16be class
    u32be ttl
    u16be length
}

// A name and one of its addresses.
struct Host {
    str name
    net.Addr addr
}

enum DnsError {
    short
    badName
    badZone
}

// The question of a query: the name asked for, its type, and where the
// question ends in the message.
struct Question {
    String name
    u16 kind
    i64 end
}

void Question.free() => self.name.free()

// Reads the question that follows the header. A name is a run of labels,
// each a length byte and that many bytes, ended by a zero.
!Question readQuestion(u8[] msg) {
    Question q
    errdefer q.name.free()
    at := sizeof(Header)
    for {
        if at >= msg.len {
            return error(DnsError.short, "the name runs past the end")
        }
        n := i64(msg[at])
        at++
        if n == 0 {
            break
        }
        if n > 63 || at + n > msg.len {
            return error(DnsError.badName, "a label is too long")
        }
        if q.name.len > 0 {
            q.name.append(".")
        }
        q.name.append(msg[at:at + n])
        at += n
    }
    if at + sizeof(QuestionEnd) > msg.len {
        return error(DnsError.short, "the question has no type")
    }
    QuestionEnd *e = msg[at:at + sizeof(QuestionEnd)].ptr
    q.kind = e.kind
    q.end = at + sizeof(QuestionEnd)
    return q
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
            return error(DnsError.badZone, "line {i + 1}: write a name and an address")
        }
        addr := net.ip(parts.at(1)) catch e {
            return error(DnsError.badZone, "line {i + 1}: {e.message}")
        }
        zone.push(.{name: parts.at(0), addr: addr})
    }
    return zone
}

const A = 1
const AAAA = 28

// Answers a query: an A query gets the name's IPv4 addresses and an AAAA
// query its IPv6 ones. A name that isn't there gets NXDOMAIN.
!void answer(net.Udp *sock, u8[] msg, net.Addr from, List[Host] *zone) {
    if msg.len < sizeof(Header) {
        return error(DnsError.short, "{msg.len} bytes is shorter than a header")
    }
    Header *h = msg.ptr
    scoped q := try readQuestion(msg)
    // names are the same in any case: WEB.lan is web.lan
    scoped String name = q.name.lower()
    scoped String records
    i64 count = 0
    bool known = false
    for host in zone.items() {
        if host.name != name {
            continue
        }
        known = true
        v4 := host.addr.isV4()
        if (q.kind == A && v4) || (q.kind == AAAA && !v4) {
            ip := if v4 { host.addr.ip[12:16] } else { host.addr.ip[0:16] }
            // 0xc00c points at the name in the question, 12 bytes in
            Record r = .{name: 0xc00c, kind: q.kind, class: 1, ttl: 300, length: u16(ip.len)}
            u8 *rb = &r
            records.append(rb[0:sizeof(Record)])
            records.append(ip)
            count++
        }
    }
    print("{q.name} type {q.kind}: {count} found")
    Header reply = *h
    reply.response = 1
    reply.authoritative = 1
    reply.recursionAvailable = 0
    reply.rcode = if known { 0 } else { 3 }
    reply.questions = 1
    reply.answers = u16(count)
    reply.authorities = 0
    reply.additionals = 0
    u8 *bytes = &reply
    scoped String out
    out.append(bytes[0:sizeof(Header)])
    out.append(msg[sizeof(Header):q.end])
    out.append(records)
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

The server reads and writes DNS messages as bytes, without parsing them by hand:

- `packed` structs that match the header and records byte for byte, with `u16be` and `u32be` fields in network byte order and bitfields for the flags.
- A pointer to the bytes that came in, read as a `Header *` or a `QuestionEnd *`, with every length checked against the end of the message first.
- UDP with `net.listenUdp`, `readFrom` and `sendTo`.

To run it for real, build it once with optimisations and start the program it makes:

```shell
iris build --mode=release-safe main.iris -o dns
./dns
```

Some ways to go on: a `CNAME` record, whose data is another name, lets one name stand for another. A name the zone doesn't have could be passed on to another server with `net.dialUdp`, and its answer sent back. Reading `zone.txt` again on a signal, with the `signal` module, lets the names change while the server runs. The [Structs](../tour/13-structs.html) chapter covers `packed`, bitfields and `align`, and the [net library page](../std/index.html#net) the rest of UDP.
