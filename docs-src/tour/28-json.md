# JSON

A struct marked `@json` can be turned into JSON text and read back from it, with the `json` module. The compiler writes the code for each struct, so there are no lookups by name while the program runs.

## Encoding

`json.encode(v)` gives the JSON text of a value as a new `String`:

```iris run
import json

enum Proto { http, grpc }

// A service, as a service registry lists it.
@json
struct Service {
    i64 id
    String name
    Proto proto
    bool healthy = true
    str[] addrs
}

void main() {
    str[2] addrs = ["10.0.0.11:8080", "10.0.0.12:8080"]
    s := Service{id: 7, name: "billing".copy(), proto: .grpc, addrs: addrs}
    scoped String text = json.encode(s)
    print(text)
    s.name.free()
}
```

```output
{"id":7,"name":"billing","proto":"grpc","healthy":true,"addrs":["10.0.0.11:8080","10.0.0.12:8080"]}
```

Arrays, slices and plain values can be encoded too: `json.encode(services.items())` gives a JSON array. An enum is written as its value's name.

## Decoding

`json.decode(T, text)` reads a `T` from JSON text and gives a `!T`. Keys the struct doesn't have are skipped, and fields whose keys are missing keep their defaults:

```iris run
import json

enum Proto { http, grpc }

@json
struct Check {
    String path
    u16 port
}

@json
struct Service {
    i64 id
    String name
    Proto proto = .http
    bool healthy = true
    String[] addrs
    Check check
    f64 load
}

!void main() {
    text := r"""
        {"id": 7, "name": "billing", "addrs": ["10.0.0.11:8080", "10.0.0.12:8080"],
         "check": {"path": "/health", "port": 9090}, "load": 0.25, "region": "eu"}
        """
    scoped Service s = try json.decode(Service, text)
    print("{s.id} {s.name} {s.proto} {s.healthy} {s.addrs.len} {s.check.path} {s.check.port} {s.load}")
}
```

```output
7 billing http true 2 /health 9090 0.25
```

`null` leaves a field as it is. A decoded struct owns its `String`s and slices: the `free` method that `@json` gives the struct frees them, so `scoped` works on it, and when `decode` fails, what it had read is freed already.

A JSON array is read into a slice, written as the type: `json.decode(Service[], text)`. The slice is new memory, which the caller frees, after freeing each item that owns memory:

```iris run
import json

@json
struct Backend {
    String addr
    i64 weight
}

!void main() {
    backends := try json.decode(Backend[], r"""[{"addr": "10.0.0.11:8080", "weight": 3}, {"addr": "10.0.0.12:8080", "weight": 1}]""")
    defer free(backends)
    defer {
        for b in backends {
            b.free()
        }
    }
    for b in backends {
        print("{b.addr} weight {b.weight}")
    }
    ports := try json.decode(u16[], "[80, 443, 8080]")
    print("{ports.len} ports, the last {ports[2]}")
    free(ports)
}
```

```output
10.0.0.11:8080 weight 3
10.0.0.12:8080 weight 1
3 ports, the last 8080
```

## Lists

A `List` field is a JSON array too. `decode` fills the list, which keeps growing with `push` after, and the struct's `free` frees the list and what is in it. `json.decode(List[T], text)` reads a whole array into a new list, which the caller frees:

```iris run
import json

@json
struct Backend {
    String addr
    i64 weight = 1
}

// The upstreams of a reverse proxy.
@json
struct Pool {
    String name
    List[Backend] backends
}

!void main() {
    scoped Pool pool = try json.decode(Pool, r"""
        {"name": "billing", "backends": [{"addr": "10.0.0.11:8080", "weight": 3}, {"addr": "10.0.0.12:8080"}]}
        """)
    pool.backends.push(Backend{addr: "10.0.0.13:8080".copy()})
    for b in pool.backends {
        print("{b.addr} weight {b.weight}")
    }
    scoped String text = json.encode(pool)
    print(text)

    ports := try json.decode(List[u16], "[80, 443]")
    ports.push(8080)
    print("{ports.len} ports, the last {ports.last()}")
    ports.free()
}
```

```output
10.0.0.11:8080 weight 3
10.0.0.12:8080 weight 1
10.0.0.13:8080 weight 1
{"name":"billing","backends":[{"addr":"10.0.0.11:8080","weight":3},{"addr":"10.0.0.12:8080","weight":1},{"addr":"10.0.0.13:8080","weight":1}]}
3 ports, the last 8080
```

The second backend has no `"weight"`, so it keeps the default, 1.

## Enums

An enum field is a string in JSON, the name of one of the enum's values. `decode` turns the string back into the value, and a slice of an enum is read from an array of strings. A string that names no value is a `wrongType` error:

```iris run
import json

enum Level { debug, info, warn, error }
enum Proto { http, grpc }

// One rule of an alerting config.
@json
struct Rule {
    String name
    Level level = .info
    Proto[] protos
}

void main() {
    text := r"""
        [{"name": "slow requests", "level": "warn", "protos": ["http", "grpc"]},
         {"name": "crashes", "level": "error", "protos": ["grpc"]},
         {"name": "deploys", "protos": []}]
        """
    rules := json.decode(Rule[], text) catch e {
        print("{e.code}: {e.message}")
        return
    }
    defer free(rules)
    defer {
        for r in rules {
            r.free()
        }
    }
    for r in rules {
        action := match r.level {
            .error => "page someone"
            .warn => "post in the channel"
            _ => "only log it"
        }
        print("{r.name}: {r.level}, {r.protos.len} protocols, {action}")
    }
    scoped String back = json.encode(rules[0])
    print(back)
    _ = json.decode(Rule, r"""{"name": "x", "level": "fatal"}""") catch e {
        print("{e.code}: {e.message}")
        Rule{}
    }
}
```

```output
slow requests: warn, 2 protocols, post in the channel
crashes: error, 1 protocols, page someone
deploys: info, 0 protocols, only log it
{"name":"slow requests","level":"warn","protos":["http","grpc"]}
JsonError.wrongType: level: "fatal" isn't one of the enum's values
```

The third rule has no `"level"`, so it keeps the default, `.info`.

## Keys and required fields

A field's key is its name as written. `@json("key")` gives another key, and `@json(required)` makes `decode` fail when the key is missing. Both can go together, `@json("id", required)`:

```iris run
import json

// One line of a JSON access log.
@json
struct Access {
    @json("request_id", required) i64 id
    @json("remote_addr") String remote
    i64 status
}

void main() {
    scoped Access a = json.decode(Access, r"""{"request_id": 3, "remote_addr": "10.0.0.5", "status": 200}""") catch err {
        print(err.message)
        Access{}
    }
    print("{a.id} {a.remote} {a.status}")
    scoped String back = json.encode(a)
    print(back)
    _ = json.decode(Access, r"""{"status": 502}""") catch err {
        print("{err.code}: {err.message}")
        Access{}
    }
}
```

```output
3 10.0.0.5 200
{"request_id":3,"remote_addr":"10.0.0.5","status":200}
JsonError.missing: the required field "request_id" is missing
```

## What a @json struct can hold

Integers, floats, `bool`, `String`, plain enums, other `@json` structs, and slices, arrays and `List`s of those. Fields of an embedded struct appear as fields of the outer one. A `str` field can be encoded but not decoded, since a `str` can't own the text it would point to; decoded text goes in a `String`.

## Errors

The errors are values of `json.JsonError`, and the message says where in the text it went wrong:

| Code | When |
|---|---|
| `syntax` | the text isn't JSON |
| `wrongType` | a value has the wrong type for its field |
| `missing` | a required key isn't there |
| `range` | a number doesn't fit its field |

```iris run
import json

@json
struct Listen {
    i64 backlog
    u8 ttl
    String[] hosts
}

void main() {
    for text in [r"""{"backlog": "many"}""", r"""{"ttl": 300}""", r"""{"hosts": [1]}""", r"{oops"] {
        _ = json.decode(Listen, text) catch e {
            print("{e.code}: {e.message}")
            Listen{}
        }
    }
}
```

```output
JsonError.wrongType: backlog: expected a number, found a string
JsonError.range: ttl: 300 doesn't fit in u8
JsonError.wrongType: hosts[0]: expected a string, found a number
JsonError.syntax: line 1, column 2: expected a key in quotes
```

## Quoting text

`json.quote(text)` gives text as a JSON string, in quotes, with the characters that need it escaped. It is how a bit of JSON is written by hand:

```iris run
import json

void main() {
    msg := "upstream \"billing\" timed out\nafter 30s"
    scoped String q = json.quote(msg)
    print("{{\"error\": {q}}}")
}
```

```output
{"error": "upstream \"billing\" timed out\nafter 30s"}
```
