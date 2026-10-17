# Tasks and channels

A task is a function call that runs alongside the rest of the program. Tasks take turns on a thread, switching whenever one waits, so a program can do many things at once without callbacks. Channels pass values between them. This chapter covers `spawn`, channels, `select` and timers.

## spawn

`spawn` starts a function call as a new task and goes on at once, without waiting for it. Its arguments are worked out first, in the task that spawns:

```iris run
void greet(str name, Duration after) {
    sleep(after)
    print("hello, {name}")
}

void main() {
    spawn greet("slow", 20ms)
    spawn greet("fast", 5ms)
    print("both started")
    sleep(40ms)
    print("done")
}
```

```output
both started
hello, fast
hello, slow
done
```

`sleep(d)` waits for a `Duration`, and while a task waits, the others run. The same is true of every wait: for the network, a file, a channel, a lock, a timer. Code in a task is written in order, as if each call simply took its time, and the waiting is where the tasks take turns.

Tasks are cheap: each has its own stack, kept small until it is needed, so a server can start one for each connection. The program ends when `main` returns, whether or not other tasks are still running.

A spawned function can't return an error, since nothing waits for its result. It handles its errors itself, or sends them to a task that does.

## Channels

A channel carries values of one type from one task to another. `chan[T](n)` makes one with room for n values. `ch <- v` puts a value in, waiting while the channel is full; `<-ch` takes the next value out, waiting while it is empty:

```iris run
void produce(chan[i64] out) {
    for i in 1..6 {
        out <- i * i
    }
}

void main() {
    squares := chan[i64](2)
    spawn produce(squares)
    total := 0
    for i in 0..5 {
        total += <-squares
    }
    print("sum of squares {total}")
}
```

```output
sum of squares 55
```

Values come out in the order they went in. A channel is a value that refers to the same queue wherever it is copied, so it can be passed to tasks as an argument, kept in a struct or held in a global. Several tasks can send to one channel, and several can take from it.

A value sent on a channel is copied into it. A `String` or another value that owns memory is handed over that way: the task that takes it frees it.

## No buffer

A channel made with room for 0 values has no buffer: `ch <- v` waits until another task takes the value, which goes straight from one to the other. Both sides know the hand-over happened, which makes it a way for tasks to wait for each other:

```iris run
void work(chan[bool] done) {
    print("working")
    sleep(5ms)
    done <- true
}

void main() {
    done := chan[bool](0)
    spawn work(done)
    _ = <-done
    print("the work is finished")
}
```

```output
working
the work is finished
```

## Workers

A common shape: a channel of jobs, a few tasks taking jobs from it, and a channel for the results:

```iris run
struct Job {
    i64 id
    i64 size
}

void worker(chan[Job] jobs, chan[i64] results) {
    for {
        j := <-jobs
        sleep(1ms)
        results <- j.size * 10
    }
}

void main() {
    jobs := chan[Job](16)
    results := chan[i64](16)
    for w in 0..3 {
        spawn worker(jobs, results)
    }
    for i in 0..8 {
        jobs <- Job{id: i, size: i + 1}
    }
    total := 0
    for i in 0..8 {
        total += <-results
    }
    print("total {total}")
}
```

```output
total 360
```

The workers loop forever, taking jobs as they come; when `main` has all the results it returns, and the program ends.

## select

`select` waits on several channels at once and runs the arm of the first one that has a value. An `after(d)` arm runs when nothing came in that time, which gives any wait a timeout:

```iris run
void slowAnswer(chan[str] out) {
    sleep(50ms)
    out <- "late answer"
}

void main() {
    answers := chan[str](1)
    control := chan[str](1)
    spawn slowAnswer(answers)
    control <- "stop"
    select {
        a := <-answers => print(a)
        c := <-control => print("control: {c}")
        after(1s) => print("nothing came")
    }
    select {
        a := <-answers => print(a)
        after(10ms) => print("no answer within 10ms")
    }
}
```

```output
control: stop
no answer within 10ms
```

When several channels have values, `select` takes the first arm in order. An arm takes from a channel, or waits with `after`; sending is done outside `select`.

## Deadlocks

When every task waits for something that no other task can give, the program can never go on. It stops with a panic that says so, instead of hanging:

```iris
void main() {
    ch := chan[i64](0)
    v := <-ch // no task will ever send
}
```

```output
panic: every task is waiting and nothing can wake them: a deadlock
```

## Errors from tasks

A task that can fail sends its error, or its result, on a channel. A struct carries either:

```iris run
import str

struct Parsed {
    str input
    i64 value
    bool ok
}

void parse(str input, chan[Parsed] out) {
    n := str.parseInt(input) catch e {
        out <- Parsed{input: input}
        return
    }
    out <- Parsed{input: input, value: n, ok: true}
}

void main() {
    out := chan[Parsed](4)
    for s in ["12", "x", "30"] {
        spawn parse(s, out)
    }
    for i in 0..3 {
        p := <-out
        if p.ok {
            print("{p.input} -> {p.value}")
        } else {
            print("{p.input} isn't a number")
        }
    }
}
```

```output
12 -> 12
x isn't a number
30 -> 30
```

Each task runs until it waits, so here they finish in the order they were started. Tasks on one thread never run at the same moment; the next chapter covers threads, which do.
