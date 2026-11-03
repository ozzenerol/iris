## Waiting for others

A `WaitGroup` counts what is still running. Call `add` before starting each task or thread, `done` when each one ends, and `wait` to wait until all have ended. A `Once` runs a function the first time only, however many callers there are:

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

A `Semaphore` lets a set number of callers in at once. `Workers` are tasks that take jobs from a queue and hand each one to a function. They suit work that waits, such as requests to other servers, a few at a time:

```iris run
import sync

Atomic[i64] done

void fetch(i64 id) {
    sleep(2ms) // a request to a backend
    done.add(1)
}

void main() {
    w := sync.workers(4, fetch)
    for id in 0..20 {
        w.submit(id)
    }

    w.finish() // waits for every job, then stops the tasks
    print("{done.load()} fetched")
}
```

```output
20 fetched
```

An `RwLock` lets in many readers at once, or one writer. A waiting writer keeps new readers out, and readers that waited go in before the next writer, so neither side waits forever.

A `WaitGroup`, a `Once` and an `RwLock` each hold a `Mutex`, so they can't be copied. Keep them in a global or a struct and pass a pointer.

## Shared collections

A `sync.Map` is a `Map` that many tasks and threads can use at once. `update` changes a value in one step, so two threads counting the same key don't lose a count. A `sync.Locked` keeps any value, such as a `List` or a `Deque`, behind a `Mutex`:

```iris run
import sync

sync.Map[str, i64] perUser
sync.Locked[List[str]] log

i64 plusOne(i64 n, bool found) => n + 1

void login(str user) {
    perUser.update(user, plusOne)
    l := log.lock()
    l.push(user)
    log.unlock()
}

void main() {
    a := thread login("ana")
    b := thread login("ana")
    c := thread login("bo")
    a.join()
    b.join()
    c.join()

    l := log.lock()
    print("{perUser.getOr("ana", 0)} {perUser.getOr("bo", 0)} {l.len}")
    l.free()
    log.unlock()
    perUser.free()
}
```

```output
2 1 3
```
