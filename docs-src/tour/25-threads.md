# Threads

Threads run at the same time, on different CPU cores. This chapter covers starting and joining them, sharing data between them with `Atomic` and `Mutex`, and `threadlocal`.

## thread

A program starts with one thread. `thread f(args)` starts a function call on a new thread and gives a `Thread`; its arguments are worked out first, as with `spawn`. `join` waits for the thread to finish:

```iris run
i64 count(i64 from, i64 to) {
    i64 n = 0
    for i in from..to {
        if i % 7 == 0 {
            n += 1
        }
    }
    return n
}

void part(i64 from, i64 to, chan[i64] out) {
    out <- count(from, to)
}

void main() {
    results := chan[i64](4)
    a := thread part(0, 500000, results)
    b := thread part(500000, 1000000, results)
    a.join()
    b.join()
    print("{<-results + <-results} multiples of 7")
}
```

```output
142858 multiples of 7
```

Each thread has its own tasks. A task spawned inside a thread stays on that thread, and the thread ends when its function and all the tasks it spawned are done. `join` waits for that, and while it waits, the other tasks of the joining thread keep running. A `Thread` is joined once. A thread started without keeping its `Thread` runs on its own, and the program still ends when `main` returns.

Channels work between threads as they do between tasks: any task can send and take, wherever it runs. They are the simplest way for threads to share work.

## Data shared between threads

Two threads that change the same variable at the same time can lose changes, since a change is several steps. A global that threads change needs an `Atomic`, a `Mutex`, or a channel that one task owns.

## Atomic

`Atomic[T]` holds an integer or a `bool` that every thread can change safely. It is used through its methods only:

```iris run
Atomic[i64] hits

void visit(i64 times) {
    for i in 0..times {
        hits.add(1)
    }
}

void main() {
    a := thread visit(10000)
    b := thread visit(20000)
    a.join()
    b.join()
    print("{hits.load()} hits")
    old := hits.swap(0)
    print("{old} {hits.load()} {hits.cas(0, 5)} {hits.load()}")
}
```

```output
30000 hits
30000 0 true 5
```

| Method | What it does |
|---|---|
| `load()` | the value |
| `store(v)` | sets it |
| `add(n)`, `sub(n)` | adds or subtracts, and gives the new value |
| `swap(v)` | sets it and gives the old value |
| `cas(old, new)` | sets it to new if it is old; true if it did |

An `Atomic[bool]` has `load`, `store`, `swap` and `cas`. Every operation is sequentially consistent: all threads see the changes in the same order.

## Mutex

A `Mutex` protects data that takes more than one step to change. `lock` takes it, waiting while another task holds it, and `unlock` gives it back; `defer` puts the unlock right after the lock:

```iris run
Mutex lock
Map[i64, i64] seen

void record(i64 from, i64 to) {
    for i in from..to {
        lock.lock()
        seen.set(i % 10, seen.getOr(i % 10, 0) + 1)
        lock.unlock()
    }
}

void main() {
    a := thread record(0, 1000)
    b := thread record(1000, 2000)
    a.join()
    b.join()
    lock.lock()
    defer lock.unlock()
    print("{seen.len} keys, {seen.getOr(3, 0)} for 3")
    seen.free()
}
```

```output
10 keys, 200 for 3
```

While a task waits for a lock, the other tasks of its thread keep running. `tryLock()` takes the lock if it is free and says whether it did. Unlocking a `Mutex` that isn't locked is a bug and stops the program.

An `Atomic` or a `Mutex` is used where it is declared, or through a pointer. Copying one, or a struct that holds one, is a compile error, since the copy would guard nothing.

## threadlocal

A global marked `threadlocal` has a separate value on each thread. Each thread starts with the global's first value:

```iris run
threadlocal i64 handled

void serve(i64 n, chan[i64] out) {
    for i in 0..n {
        handled += 1
    }
    out <- handled
}

void main() {
    out := chan[i64](2)
    a := thread serve(3, out)
    b := thread serve(5, out)
    a.join()
    b.join()
    x := <-out
    y := <-out
    print("{x + y} handled, {handled} on main's thread")
}
```

```output
8 handled, 0 on main's thread
```

It suits a cache or a counter that each thread keeps for itself, with no lock. The tasks of one thread share its value.

## Threads in the library

`http.serve` and `Router.serve` start a thread for each CPU core, each with its own event loop, so a handler runs on several threads at once: a global it changes needs an `Atomic` or a `Mutex`, or `threadlocal`. `threads: 1` keeps everything on one thread.
