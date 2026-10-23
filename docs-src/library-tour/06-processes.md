# Programs and the environment

A program talks to the system around it in four ways: the arguments it was started with, the environment variables, the other programs it runs, and the signals it gets. The `os`, `process` and `signal` modules cover them.

## Arguments

`os.argc()` gives how many arguments the program got, counting its own name, and `os.arg(i)` gives one of them as a `str`. Argument 0 is the program's name, and past the end `os.arg` gives `""`:

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

A program that ends normally has status 0. When `main` gives `!void` and ends with an error, the program prints it and ends with status 1. `os.exit(code)` ends the program at once, with any status; `defer` blocks don't run:

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

`os.env(name)` gives a variable's value as a `!str`. A variable that isn't set is the error `OsError.notSet`, so `catch` gives a default:

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

`os.setenv(name, value)` sets a variable, for the program and the programs it runs from then on, and `os.unsetenv(name)` removes one. A name that can't be one, empty or with `=` in it, fails with `OsError.invalid`:

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

Set variables at the start, before the program starts threads: the environment isn't safe to change while another thread reads it.

For a program's settings, a struct marked `@env` reads every variable at once, with types and defaults (see [Attributes](../tour/27-attributes.html)).

## Running other programs

`process.run(argv)` runs a program and waits for it to end. `argv[0]` is the program, found on `PATH` when it has no `/`, and the rest are its arguments, given as they are: there is no shell in between, so spaces and quotes need no escaping. It gives an `Output` with what the program wrote and its exit status:

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

`Output` owns `stdout` and `stderr`, and its `free` method frees them, so `scoped` works on it.

A program that ends with a status other than 0 is no error: `status` says how it ended. `run` fails only when the program can't be started, with `ProcessError.notFound` when it isn't there:

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

A shell command, with pipes and `>`, runs through `sh -c` as above.

## Input

The second argument to `run` is given to the program on its standard input:

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

While a program runs, the other tasks of the thread go on (see [Tasks and channels](../tour/24-tasks-and-channels.html)), so a server that runs a program keeps answering.

## Signals

Ctrl-C sends a program the signal `interrupt`, and systemd and Docker send `terminate` when they stop it. By default either one ends the program at once. `signal.wait()` waits for one instead and gives it as a `signal.Signal`, so the program can finish what it was doing first:

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

`signal.watch()` makes `interrupt` and `terminate` wait for `wait` from then on, instead of ending the program; `wait` calls it when nothing is watched yet. `signal.watch([.hangup])` watches other signals: `hangup` (SIGHUP) is what many programs take as "read your settings again".

A server waits for the signal in its own task, while the other tasks serve:

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

When `main` returns, the program ends, and the other tasks with it.
