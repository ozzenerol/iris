`dns.parse` reads the bytes of a DNS message into a `Message`: its header, its questions, and its answer, authority and additional records. `encode` writes a `Message` as bytes. A name that comes up more than once is written once and pointed at after that.

A record keeps its data in the fields for its kind: `ip` for `a` and `aaaa`, `target` for a host name, `text` for `txt`, and the numbers of `mx`, `srv` and `soa` in fields of their own. A kind without a name here is `other`, with its number in `code` and its bytes in `data`.

```iris run
import dns

!void main() {
    scoped q := dns.query(7, "example.com", .mx)
    scoped String bytes = try q.encode()
    scoped m := try dns.parse(bytes)
    print("{bytes.len} bytes, id {m.header.id}, {m.questions.at(0).name} {m.questions.at(0).kind}")

    scoped reply := m.reply()
    reply.answers.push(.{name: "example.com".copy(), kind: .mx, ttl: 1h, priority: 10, target: "mail.example.com".copy()})
    scoped String out = try reply.encode()
    scoped back := try dns.parse(out)
    r := back.answers.at(0)
    print("{r.name} {r.kind} {r.priority} {r.target} {r.ttl}")
}
```

```output
29 bytes, id 7, example.com mx
example.com mx 10 mail.example.com 1h0m0s
```

A server answers a query with `reply`, which keeps the query's id and questions. To look names up, use `net.lookup` and `net.query`, which ask DNS servers with this module.
