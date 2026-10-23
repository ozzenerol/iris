## Waiting for others

A `WaitGroup` counts what is still running: `add` before starting each task or thread, `done` when each ends, and `wait` until all have. A `Once` runs a function the first time, however many callers there are:

```iris run
import sync

sync.WaitGroup group
sync.Once setup
Atomic[i64] checked

void loadConfig() => print("config loaded once")

void probe(i64 port) {
    defer group.done()
    setup.run(loadConfig)
    checked.add(1)
}

void main() {
    for port in [22, 80, 443] {
        group.add()
        spawn probe(port)
    }
    t := thread probe(8080)
    group.add()
    group.wait()
    t.join()
    print("{checked.load()} ports checked")
    group.free()
}
```

```output
config loaded once
4 ports checked
```

## Limiting how many at once

A `Semaphore` lets a number of callers in at once. `Workers` are tasks that take jobs from a queue and hand each to one function, for work that waits, such as requests to other servers, a few at a time:

```iris run
import sync

Atomic[i64] done

void fetch(i64 id) {
    sleep(2ms)  // a request to a backend
    done.add(1)
}

void main() {
    w := sync.workers(4, fetch)
    for id in 0..20 {
        w.submit(id)
    }
    w.finish()  // waits for every job, then stops the tasks
    print("{done.load()} fetched")
}
```

```output
20 fetched
```

An `RwLock` lets many readers in at once, or one writer. A writer that waits keeps new readers out, and readers that waited go in before the next writer, so neither waits for ever.

A `WaitGroup`, a `Once` and an `RwLock` hold a `Mutex`, so they can't be copied; keep them in a global or a struct and pass a pointer.
