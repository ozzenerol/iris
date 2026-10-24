# Testing

Tests sit next to the code they test, in `test` blocks, and `iris test` runs them. This chapter covers writing tests, checking with `expect`, tests that use errors, running them, and measuring speed with `bench` blocks.

## test blocks

A `test` block has a name in quotes and a body. It goes at the top level of any file of a module:

```iris
// A 16-bit number in network byte order.
u16 swap16(u16 x) => (x << 8) | (x >> 8)

test "swap16" {
    expect(swap16(0x1F90) == 0x901F)
    expect(swap16(swap16(8080)) == 8080, "swapping twice gives {swap16(swap16(8080))}")
}
```

The compiler makes a function from each test block. A test sees everything in its module, including names that aren't `pub`, so it can check a module's inner parts. `iris run` and `iris build` leave tests out of the program. `iris check` checks them like the rest of the code.

## expect

`expect(cond)` checks a condition. When it is false, the test fails with the file and line of the check and goes on to its end, so one run shows every failed check. A second argument is a message to show, which can have values in braces:

```iris
test "parse a port" {
    n := try parsePort("8080")
    expect(n == 8080)
    expect(n > 0 && n < 65536, "port {n} out of range")
}
```

`expect` works only inside a test block. A helper function that checks something gives back what is wrong, and the test expects it to be empty:

```iris
String checkConfig(Config c) {
    if c.upstream.len == 0 {
        return format("the config has no upstream")
    }
    return format("")
}

test "config" {
    scoped String problem = checkConfig(load())
    expect(problem.len == 0, problem)
}
```

## Errors in tests

A test can use `try`. An error that leaves the test fails it, and the output shows the error and its trace. A test that checks that something fails uses `catch`:

```iris
test "a bad port fails" {
    bool failed = false
    _ = parsePort("x") catch e {
        failed = e.code == str.ParseError.notNumber
        0
    }

    expect(failed, "parsePort(\"x\") should fail with notNumber")
}
```

A panic in a test, such as an index out of range or an overflow, stops the whole run with its message, as it would stop the program.

## iris test

`iris test` runs every test of the program's module in the order they are written. It prints a line for each, then a count:

```shell
iris test main.iris
```

```output
ok   swap16
    main.iris:22:11: expect failed: n is 8080
FAIL parse a port
    error: ParseError.notNumber: 'x' is not a number
      at parseInt  std/str/str.iris:227
      at parsePort  main.iris:6
      at test "a bad port"  main.iris:26
FAIL a bad port
3 tests, 1 passed, 2 failed
```

- The exit status is 1 when a test failed, so a build script or CI stops there.
- A name after the file runs only the tests whose name contains it: `iris test main.iris port`.
- In a project, `iris test` in its folder runs the main module's tests.
- Tests are debug builds with every check on. An index out of range, an overflow, a `null` pointer or memory used after it was freed stops the run with its message.

## Tests that need more

A test that needs files, a network server or a database makes them itself and cleans up with `defer`. Tasks and channels work in tests as in programs, so a test can start a server on port 0 in a task and talk to it:

```iris
test "the server answers" {
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(ln)

    scoped String url = format("http://127.0.0.1:{ln.port}/health")
    scoped http.Reply r = try http.get(url)
    expect(r.status == 200, "status {r.status}")
}
```

## Benchmarks

A `bench` block measures how long its code takes. `iris bench` runs its body again and again, more times each round, until a round takes a second. It prints how many runs that was and the time of one:

```iris
i64 checksum(str data) {
    u32 sum = 0
    for b in data {
        sum = sum * 31 + u32(b)
    }
    return i64(sum)
}

bench "checksum of a 1500-byte packet" {
    u8[1500] packet
    _ = checksum(packet)
}
```

```shell
iris bench main.iris
```

```output
checksum of a 1500-byte packet        1000000 runs       1.01us/run
```

The compiler leaves out work whose result isn't used. In a `bench` block, `_ = value` keeps the value, so the work that made it is measured.

When the body needs something set up first, such as data to work on, the block takes a name for the number of runs and has its own loop. The whole block is timed, so the setup should cost little next to the loop:

```iris
bench "sort 10000 ports" (n) {
    scoped List[i64] ports
    for i in 0..10000 {
        ports.push((i * 7919) % 65536)
    }

    scoped List[i64] work
    for k in 0..n {
        work.clear()
        for p in ports {
            work.push(p)
        }
        work.sort(less)
        _ = work.at(0)
    }
}

bool less(i64 a, i64 b) => a < b
```

- `iris bench` builds with `--mode=release-fast` unless another mode is given.
- It runs only the bench blocks. `main` and the tests don't run.
- A name after the file runs only the benches whose name contains it.
- `--time=200ms` sets how long the last round of each bench takes.
- A `bench` block that fails with an error stops with the error and its trace, and `iris bench` then ends with status 1.
