# Functions as values

A function can be kept in a variable, passed to another function and stored in a struct. This chapter covers function types and what can be done with them.

## Function types

The type of a function is written as its return type followed by its parameter types in parentheses: `i64(i64, i64)` is a function that takes two `i64`s and gives one. A function's name, used without calling it, is a value of that type:

```iris run
i64 add(i64 a, i64 b) => a + b
i64 mul(i64 a, i64 b) => a * b

void main() {
    i64(i64, i64) op = add
    print("{op(6, 7)}")
    op = mul
    print("{op(6, 7)}")
}
```

```output
13
42
```

## Passing a function

A parameter can take a function, so a function can leave part of its work to the caller:

```iris run
i64 apply(i64(i64) f, i64 x) => f(x)
i64 double(i64 x) => x * 2
i64 negate(i64 x) => -x

void each(i64[] xs, void(i64) visit) {
    for x in xs {
        visit(x)
    }
}

void show(i64 x) => print("item {x}")

void main() {
    print("{apply(double, 21)} {apply(negate, 5)}")
    each([3, 1, 4], show)
}
```

```output
42 -5
item 3
item 1
item 4
```

`List.sort` takes a function like this: one that says whether its first value goes before its second (see [Collections](22-collections.html)).

There are no unnamed functions: the function passed is declared like any other, with a name. Data it needs comes through its parameters, or through a struct it is a method of.

## Type aliases for functions

A function type is easier to read with a name. `type` gives one:

```iris run
type Op = i64(i64, i64)

i64 add(i64 a, i64 b) => a + b
i64 sub(i64 a, i64 b) => a - b
i64 mul(i64 a, i64 b) => a * b

void main() {
    Op[3] ops = [add, sub, mul]
    str[3] names = ["add", "sub", "mul"]
    for i, f in ops {
        print("{names[i]}: {f(10, 4)}")
    }
}
```

```output
add: 14
sub: 6
mul: 40
```

## In structs

A field can hold a function, which is called with the field's name like a method:

```iris run
struct Button {
    str label
    void(str) onClick
}

void save(str label) => print("{label}: saving")
void quit(str label) => print("{label}: bye")

void main() {
    Button[2] buttons = [Button{label: "Save", onClick: save}, Button{label: "Quit", onClick: quit}]
    for b in buttons {
        b.onClick(b.label)
    }
}
```

```output
Save: saving
Quit: bye
```

## null and comparing

A function value is `null` until it is set, and can be compared with `==` and `!=`, against `null` or another function. Calling a `null` one stops a debug build with a message:

```iris run
i64 one() => 1

void main() {
    i64() f
    print("{f == null}")
    f = one
    print("{f == null} {f == one} {f()}")
}
```

```output
true
false true 1
```

## What carries over

A call through a function value passes its arguments by position. Defaults and names belong to the declaration, so they don't carry over to the value: every argument is given.

A function that can fail has a type that starts with `!`, as its declaration does: `!i64(str)`. A function that returns `void` can be given where one returning `!void` is wanted, since never failing is one way of not failing. This is how an HTTP route can take a plain handler or one that uses `try`:

```iris run
type Step = !void(i64)

enum StepError { tooBig }

!void check(i64 n) {
    if n > 10 {
        return error(StepError.tooBig, "{n} is too big")
    }
    print("{n} is fine")
}

void announce(i64 n) => print("step {n}")

!void runAll(Step[] steps, i64 n) {
    for s in steps {
        try s(n)
    }
}

void main() {
    Step[2] steps = [announce, check]
    runAll(steps, 4) catch e { }
    runAll(steps, 40) catch e {
        print("stopped: {e.message}")
    }
}
```

```output
step 4
4 is fine
step 40
stopped: 40 is too big
```

A generic function, and a C function with `...`, can't be used as a value. Methods are called through their struct; a function value holds a plain function.
