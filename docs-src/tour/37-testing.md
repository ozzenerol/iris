# Testing

Tests sit next to the code they test, in `test` blocks, and `iris test` runs them. This chapter covers writing tests, checking with `expect`, tests that use errors, and running them.

## test blocks

A `test` block has a name in quotes and a body, and can go in any file of a module, at the top level:

```iris
i64 add(i64 a, i64 b) => a + b

test "add" {
    expect(add(2, 3) == 5)
    expect(add(-1, 1) == 0, "add(-1, 1) is {add(-1, 1)}")
}
```

A test is a function the compiler makes from the block. It sees everything in its module, including names that aren't `pub`, so a test can check a module's inner parts. `iris run` and `iris build` leave tests out of the program; `iris check` checks them like the rest of the code.

## expect

`expect(cond)` checks a condition. When it is false, the test fails, with the file and line of the check, and goes on to its end, so one run shows every failed check. A second argument is a message to show, which can have values in braces:

```iris
test "parse a port" {
    n := try parsePort("8080")
    expect(n == 8080)
    expect(n > 0 && n < 65536, "port {n} out of range")
}
```

`expect` can be used only inside a test block. A helper function that checks something gives back what is wrong, and the test expects it to be empty:

```iris
String checkUser(User u) {
    if u.name.len == 0 {
        return format("the user has no name")
    }
    return format("")
}

test "users" {
    scoped String problem = checkUser(load())
    expect(problem.len == 0, problem)
}
```

## Errors in tests

A test can use `try`: an error that leaves the test fails it, and the output shows the error and its trace. A test that checks that something fails uses `catch`:

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

A panic in a test, an index out of range or an overflow, stops the whole run with its message, as it would stop the program.

## iris test

`iris test` runs every test of the program's module, in the order they are written, and prints a line for each, then a count:

```shell
iris test main.iris
```

```output
ok   add
    main.iris:22:11: expect failed: n is 8080
FAIL parse a port
    error: ParseError.notNumber: 'x' is not a number
      at parseInt  std/str/str.iris:227
      at parsePort  main.iris:6
      at test "a bad port"  main.iris:26
FAIL a bad port
3 tests, 1 passed, 2 failed
```

The exit status is 1 when a test failed, so a build script or CI stops there. A name after the file runs only the tests whose name has it in: `iris test main.iris port`. In a project, `iris test` in its folder runs the main module's tests.

Tests are built as debug builds, with every check on: an index out of range, an overflow or a `null` pointer stops the test run with its message, and so does memory used after it was freed.

## Tests that need more

A test that needs files, a network server or a database makes them itself, and cleans up with `defer`. Tasks and channels work in tests as in programs, so a test can start a server on port 0 in a task and talk to it:

```iris
test "the server answers" {
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(ln)
    scoped String url = format("http://127.0.0.1:{ln.port}/health")
    scoped http.Reply r = try http.get(url)
    expect(r.status == 200, "status {r.status}")
}
```
