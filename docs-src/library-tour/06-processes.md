# Programs and the environment

A program deals with the system around it through its arguments, the environment variables, the other programs it runs, and the signals it receives. The `os`, `process` and `signal` modules cover them.

## Arguments

`os.argc()` gives how many arguments the program got, counting its own name. `os.arg(i)` gives one of them as a `str`. Argument 0 is the program's name, and past the end `os.arg` gives `""`.

```iris
import os

void main() {
    print("this is {os.arg(0)}, with {os.argc() - 1} arguments")

    for i in 1..os.argc() {
        print("argument {i}: {os.arg(i)}")
    }
}
```

`./show a "b c"` prints:

```output
this is ./show, with 2 arguments
argument 1: a
argument 2: b c
```

A program with flags reads them into a struct with `flags.parse` instead (see [Command-line flags](05-flags.html)).

## Exit status

A program that ends normally has status 0. When `main` returns `!void` and ends with an error, the program prints the error and ends with status 1. `os.exit(code)` ends the program at once with any status, and `defer` blocks don't run.

```iris
import os

void main() {
    if os.argc() < 2 {
        print("usage: probe <host>")
        os.exit(2)
    }

    print("probing {os.arg(1)}")
}
```

## Environment variables

`os.env(name)` gives a variable's value as a `!str`. A variable that isn't set gives the error `OsError.notSet`, so `catch` can supply a default.

```iris run
import os

void main() {
    path := os.env("PATH") catch ""
    print("PATH is set: {path.len > 0}")

    upstream := os.env("IRIS_TOUR_UPSTREAM") catch "127.0.0.1:9000"
    print("the upstream is {upstream}")

    _ = os.env("IRIS_TOUR_UPSTREAM") catch err {
        print("{err.code}: {err.message}")
        ""
    }
}
```

```output
PATH is set: true
the upstream is 127.0.0.1:9000
OsError.notSet: IRIS_TOUR_UPSTREAM is not set
```

`os.setenv(name, value)` sets a variable for the program and for the programs it runs from then on. `os.unsetenv(name)` removes one. An invalid name, empty or containing `=`, fails with `OsError.invalid`.

```iris run
import os, process, str

!void main() {
    try os.setenv("UPSTREAM", "10.0.0.11:9000")
    print(try os.env("UPSTREAM"))

    scoped out := try process.run(["sh", "-c", "echo $UPSTREAM"])
    print("sh sees: {str.trim(out.stdout)}")

    try os.unsetenv("UPSTREAM")
    print(os.env("UPSTREAM") catch "not set")
}
```

```output
10.0.0.11:9000
sh sees: 10.0.0.11:9000
not set
```

Set variables at the start, before the program starts threads. Changing the environment while another thread reads it isn't safe.

For a program's settings, a struct marked `@env` reads every variable at once, with types and defaults (see [Attributes](../tour/27-attributes.html)).

## Running other programs

`process.run(argv)` runs a program and waits for it to end. `argv[0]` is the program, looked up on `PATH` when it has no `/`. The rest are its arguments, passed as they are. No shell is involved, so spaces and quotes need no escaping. It returns an `Output` with what the program wrote and its exit status.

```iris run
import process, str

!void main() {
    scoped out := try process.run(["echo", "listening on", "8080"])
    print("status {out.status}, ok {out.ok()}")
    print("it printed: {str.trim(out.stdout)}")
}
```

```output
status 0, ok true
it printed: listening on 8080
```

`Output` owns `stdout` and `stderr`. Its `free` method frees them, so `scoped` works on it.

A program that ends with a status other than 0 is no error, and `status` says how it ended. `run` fails only when the program can't be started, with `ProcessError.notFound` when it doesn't exist.

```iris run
import process, str

!void main() {
    scoped out := try process.run(["sh", "-c", "echo oops >&2; exit 3"])
    print("status {out.status}, ok {out.ok()}, stderr: {str.trim(out.stderr)}")

    _ = process.run(["no-such-program"]) catch err {
        print("{err.code}: {err.message}")
        process.Output{}
    }
}
```

```output
status 3, ok false, stderr: oops
ProcessError.notFound: no-such-program: no such program
```

A shell command with pipes and `>` runs through `sh -c`, as above.

## Programs that run beside this one

`process.start(argv)` starts a program and returns at once with a `Child`. The program shares this one's standard input, output and error. `wait` waits for it to end and gives its exit status, while the other tasks run. `running` says whether it still runs, `stop` asks it to end (SIGTERM) and `kill` ends it at once (SIGKILL). A program a signal ended has the status 128 plus the signal's number.

```iris run
import process

!void main() {
    sleeper := try process.start(["sleep", "10"])
    print("running: {sleeper.running()}")
    sleeper.stop()
    print("ended with {sleeper.wait()}")

    job := try process.start(["sh", "-c", "exit 4"])
    print("the job ended with {job.wait()}")
}
```

```output
running: true
ended with 143
the job ended with 4
```

`keep` hands the program open descriptors, which it gets as 3, 4 and so on, in order. A server upgrades this way without dropping a connection: it starts its next version with its listening socket, the new process takes it with `net.adoptListener(3)` and accepts on it, and the old one stops accepting and lets its own connections finish. Here the program starts itself again as its next version. Built with `iris build` and run, it prints `served by the new version`:

```iris
import net, os, process

!void next() {
    ln := try net.adoptListener(3)
    defer ln.close()
    c := try ln.accept()
    defer c.close()
    try c.write("served by the new version\n")
}

!void main() {
    if os.argc() > 1 && os.arg(1) == "--next" {
        try next()
        return
    }
    ln := try net.listen(0, "127.0.0.1")
    port := ln.port
    newer := try process.start([os.arg(0), "--next"], keep: [ln.fd()])
    ln.close()      // the new version has its own copy of the socket

    c := try net.dial("127.0.0.1", port)
    defer c.close()
    scoped String line = try c.readLine()
    print(line)
    _ = newer.wait()
}
```

Connections that wait to be accepted stay in the socket's queue while the versions change, so none is refused. `os.arg(0)` is the program's own path. `iris run` deletes the program it built once the program starts, so a program that starts itself has to be built.

## Input

The second argument to `run` goes to the program's standard input:

```iris run
import process, str

!void main() {
    scoped out := try process.run(["sort", "-u"], "10.0.0.9\n10.0.0.5\n10.0.0.9\n10.0.0.7\n")
    print(str.trim(out.stdout))

    scoped count := try process.run(["wc", "-l"], "GET /\nGET /login\nPOST /login\n")
    print("{str.trim(count.stdout)} requests")
}
```

```output
10.0.0.5
10.0.0.7
10.0.0.9
3 requests
```

While a program runs, the thread's other tasks keep going (see [Tasks and channels](../tour/24-tasks-and-channels.html)). A server that runs a program keeps answering.

## Signals

Ctrl-C sends a program the signal `interrupt`. systemd and Docker send `terminate` to stop it. By default either one ends the program at once. `signal.wait()` waits for one instead and returns it as a `signal.Signal`, so the program can finish its work first.

```iris run
import process, signal

!void main() {
    signal.watch()

    // stands in for Ctrl-C: sh sends terminate to this program, its parent
    scoped out := try process.run(["sh", "-c", "kill -TERM $PPID"])

    s := signal.wait()
    print("got {s}, saving and stopping")
}
```

```output
got terminate, saving and stopping
```

From then on, `signal.watch()` makes `interrupt` and `terminate` wait for `wait` instead of ending the program. `wait` calls it when nothing is watched yet. `signal.watch([.hangup])` watches other signals. Many programs take `hangup` (SIGHUP) as a request to read their settings again. `user1` and `user2` (SIGUSR1 and SIGUSR2) mean whatever the program says, such as starting its next version.

A server waits for the signal in one task while other tasks serve:

```iris
import signal

void serve() {
    // answers requests until the program ends
}

void main() {
    spawn serve()

    s := signal.wait()
    print("got {s}, stopping")
    // close connections, flush files, then return
}
```

When `main` returns, the program ends and the other tasks with it.
