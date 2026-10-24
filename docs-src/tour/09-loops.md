# Loops

`for` is the only loop. It has four forms: forever, while a condition holds, over a range, and over items. `break` and `continue` control it.

## Forever

`for` alone runs until `break` or `return`:

```iris run
void main() {
    remaining := 10000 // body bytes left to read
    reads := 0

    for {
        got := 4096 if remaining > 4096 else remaining
        remaining -= got
        reads += 1

        if remaining == 0 {
            break
        }
    }

    print("{reads} reads")
}
```

```output
3 reads
```

A server's accept loop is the typical use.

## While

`for cond` runs while the condition is true. The condition is checked before each round, so the body may never run.

```iris run
void main() {
    delay := 100ms
    attempt := 1

    for delay < 5s {
        print("attempt {attempt}, then wait {delay}")
        delay *= 2
        attempt += 1
    }
}
```

```output
attempt 1, then wait 100ms
attempt 2, then wait 200ms
attempt 3, then wait 400ms
attempt 4, then wait 800ms
attempt 5, then wait 1.6s
attempt 6, then wait 3.2s
```

## Ranges

`for i in a..b` counts from `a` to `b - 1`. Both ends are evaluated once, before the first round. An empty range runs nothing.

```iris run
void main() {
    for i in 0..3 {
        print("starting worker {i}")
    }

    for i in 3..3 {
        print("never")
    }

    for port in 8080..8083 {
        print("listening on {port}")
    }
}
```

```output
starting worker 0
starting worker 1
starting worker 2
listening on 8080
listening on 8081
listening on 8082
```

`i` is a new variable each round, local to the loop. A range counts up by one. For other steps, use a condition:

```iris run
void main() {
    left := 10s

    for left > 0s {
        print("stopping in {left}")
        left -= 4s
    }
}
```

```output
stopping in 10s
stopping in 6s
stopping in 2s
```

## Items

`for x in items` iterates over an array, a slice, a `str` (its bytes), a `List` or a `Map`.

- With two names, arrays, slices and `str` give the index first.
- Names in parentheses unpack a tuple: `for (name, port) in [("api", 8080)]` (see [Tuples](16-tuples-and-aliases.html)).

```iris run
void main() {
    u16[4] ports = [22, 80, 443, 8080]

    for p in ports {
        print("{p}")
    }

    for i, p in ports[1:3] {
        print("{i}: {p}")
    }

    for i, b in "OK" {
        print("byte {i} is {b}")
    }
}
```

```output
22
80
443
8080
0: 80
1: 443
byte 0 is 79
byte 1 is 75
```

A `List` gives its values. `items()` returns them as a slice, which supports the index form:

```iris run
void main() {
    List[str] upstreams
    defer upstreams.free()

    upstreams.push("10.0.0.11:8080")
    upstreams.push("10.0.0.12:8080")

    for u in upstreams {
        print(u)
    }

    for i, u in upstreams.items() {
        print("{i}: {u}")
    }
}
```

```output
10.0.0.11:8080
10.0.0.12:8080
0: 10.0.0.11:8080
1: 10.0.0.12:8080
```

A `Map` gives each key and value in no fixed order. `_` skips one:

```iris run
void main() {
    Map[str, i64] hits
    defer hits.free()

    hits.set("/health", 3)

    for path, count in hits {
        print("{path}: {count}")
    }

    for path, _ in hits {
        print("served {path}")
    }
}
```

```output
/health: 3
served /health
```

Loop variables are copies. To change items, loop over indexes: `for i in 0..items.len { items[i] += 1 }`.

## break and continue

`break` leaves the loop. `continue` skips to the next round.

```iris run
void main() {
    // each request's status; 0 marks the end
    for status in [200, 200, 500, 404, 200, 0, 503] {
        continue if status == 200
        break if status == 0

        print("failed with {status}")
    }
}
```

```output
failed with 500
failed with 404
```

## Labels

A label before a loop, `name:`, lets an inner loop's `break` and `continue` act on the outer one:

```iris run
void main() {
    down := "10.0.0.1"

    probe: for host in ["10.0.0.1", "10.0.0.2", "10.0.0.3"] {
        for port in [22, 80, 443] {
            if host == down {
                print("{host} is down")
                continue probe
            }

            print("probing {host}:{port}")

            if host == "10.0.0.2" && port == 80 {
                print("web server at {host}")
                break probe
            }
        }
    }
}
```

```output
10.0.0.1 is down
probing 10.0.0.2:22
probing 10.0.0.2:80
web server at 10.0.0.2
```

## Custom iteration

A struct with a method `next(i64 *at)` returning a value and a `bool` works in `for`. The `bool` is false when there are no more values. `at` starts at 0 in each loop and the method advances it.

```iris run
// The lines of a text, without line breaks.
struct Lines {
    str text
}

(str, bool) Lines.next(i64 *at) {
    if *at >= self.text.len {
        return ("", false)
    }

    start := *at
    end := start
    for end < self.text.len && self.text[end] != '\n' {
        end += 1
    }

    *at = end + 1
    return (self.text[start:end], true)
}

void main() {
    req := Lines{text: "GET / HTTP/1.1\nHost: example.com\nAccept: */*"}
    for line in req {
        print("[{line}]")
    }

    for cmd in (Lines{text: "PING\nQUIT"}) {
        print("command {cmd}")
    }
}
```

```output
[GET / HTTP/1.1]
[Host: example.com]
[Accept: */*]
command PING
command QUIT
```

A `next` that returns two values and a `bool` gives a two-name loop, as `Map` does. A struct literal in a `for` or `if` head needs parentheses so its braces aren't read as the block.
