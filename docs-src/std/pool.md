## A pool of connections

`pool.make` takes what opening an item needs, such as an address, and the function that opens one. `take` gives an idle item or opens a new one, and `give` puts it back for the next caller. Any struct with `close` and `reusable` methods, both `pub`, can be pooled. `reusable` says whether the next caller can use it; one that isn't is closed when it is given back:

```iris run
import pool

// A connection to a backend, as a real one would hold a net.Conn.
struct Backend {
    i64 id
    bool broken
}

pub void Backend.close() => print("close {self.id}")
pub bool Backend.reusable() => !self.broken

i64 opened = 0

!Backend dial(str *address) {
    opened += 1
    print("open {opened} to {*address}")
    return Backend{id: opened}
}

!void main() {
    backends := pool.make("10.0.0.5:9000", dial, max: 2)
    defer backends.close()
    a := try backends.take()
    backends.give(a)
    b := try backends.take()
    print("got {b.id} again")
    b.broken = true
    backends.give(b)
    print("open now: {backends.size()}")
}
```

```output
open 1 to 10.0.0.5:9000
got 1 again
close 1
open now: 0
```

When `max` items are in use, `take` waits until one is given back, for at most `wait` (30 seconds unless given), and then fails with `PoolError.timeout`. A `Pool` can be copied and shared by tasks and threads; the copies share the items. The `redis` client keeps its connections in one.
