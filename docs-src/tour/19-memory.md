# Memory

Iris has no garbage collector: memory that a program asks for, it gives back. This chapter covers where values live, `alloc` and `free`, the statements that make freeing hard to forget (`defer`, `errdefer` and `scoped`), who frees what, and the checks that catch mistakes.

## Where values live

Most values need no thought. Numbers, structs and arrays in variables live in their function's frame and go away when it returns; globals live as long as the program. Copying such a value copies it whole.

Memory that must outlive a function, or whose size is known only when the program runs, comes from the heap: `alloc` asks for it, and `free` gives it back. `String`, `List` and `Map` keep their contents on the heap too, which is why they have a `free` method.

## alloc and free

`alloc(T, n)` gives a slice of `n` items of type `T`, all at zero. `free` takes the slice and gives the memory back:

```iris run
void main() {
    n := 4
    f64[] weights = alloc(f64, n)
    for i in 0..n {
        weights[i] = f64(i) * 0.5
    }
    print("{weights[3]} of {weights.len}")
    free(weights)
}
```

```output
1.5 of 4
```

The slice can be passed around and stored like any other; whoever holds it last frees it, once. Memory from `alloc` is all zeros, even for a struct whose fields have defaults.

## defer

`defer` runs a statement when the block it is in ends, however it ends: at the closing brace, at a `return`, at a `break` or `continue`, or when `try` passes an error up. It puts the cleanup right after the code that needs it:

```iris run
void work(bool early) {
    print("open")
    defer print("close")
    if early {
        print("leaving early")
        return
    }
    print("working")
}

void main() {
    work(false)
    work(true)
}
```

```output
open
working
close
open
leaving early
close
```

Several defers run in the reverse order of their lines, so what was set up last is undone first. A defer in a loop runs at the end of each round, since the round is a block. `defer { ... }` defers a whole block:

```iris run
void main() {
    defer print("third")
    defer {
        print("first")
        print("second")
    }
    for i in 0..2 {
        defer print("end of round {i}")
        print("round {i}")
    }
}
```

```output
round 0
end of round 0
round 1
end of round 1
first
second
third
```

A deferred statement can't `return`, `break` or `continue` out of the block.

## errdefer

`errdefer` runs only when the function returns an error. It undoes work that should stay done only if everything succeeds, such as a half-built value:

```iris run
enum SetupError { failed }

!void setup(bool failAtEnd) {
    print("make the thing")
    errdefer print("undo: remove the thing")
    defer print("tidy up")
    if failAtEnd {
        return error(SetupError.failed, "the last step failed")
    }
    print("all done")
}

void main() {
    setup(false) catch e { }
    print("--")
    setup(true) catch e { }
}
```

```output
make the thing
all done
tidy up
--
make the thing
tidy up
undo: remove the thing
```

## scoped

`scoped` on a declaration calls the value's `free` method when the block ends. It is the same as the declaration followed by `defer x.free()`, in one line:

```iris run
import str

void main() {
    scoped String name = str.upper("ada")
    scoped List[i64] nums
    nums.push(1)
    nums.push(2)
    print("{name} {nums.len}")
}
```

```output
ADA 2
```

The type can come from the value, as with `:=`: `scoped t := term.table(...)`. Any type with a `free` method that takes nothing works with `scoped`: `String`, `List`, `Map`, the library's types, and the program's own structs.

## Who frees what

A few conventions say who frees each piece of memory, and the standard library follows them throughout:

- A function that returns a `String`, a `List`, a `Map` or a struct with a `free` method gives the caller a new value, which the caller frees: `str.upper`, `format`, `fs.readAll`, `json.encode`.
- A `str` or a slice that a function gives back points into something else: into its argument (`str.trim`), or into a value that still owns it (`list.items()`). It is valid as long as that is, and is never freed.
- A `List[String]`, or a list of structs with `free`, holds values that must be freed too: `freeEach` frees each one, then the list.
- A struct that owns memory has a `free` method that frees it all, and one that only looks at memory owned elsewhere has none.

```iris run
import str

struct User {
    String name
    List[String] tags
}

void User.free() {
    self.name.free()
    self.tags.freeEach()
}

User makeUser(str name) {
    User u = User{name: str.upper(name)}
    u.tags.push("new".copy())
    return u
}

void main() {
    scoped u := makeUser("ada")
    u.tags.push("admin".copy())
    print("{u.name} has {u.tags.len} tags")
}
```

```output
ADA has 2 tags
```

Structs marked `@json`, `@env` or `@flags` get a `free` method made for them, which frees their Strings and slices (see [Attributes](27-attributes.html)).

## Checks in debug builds

A debug build, which `iris run` makes, stops the program with a message and where it happened when it finds:

- an index outside an array, slice or `str`
- an integer that overflows, and a division by zero
- a conversion that loses a value
- a `null` pointer used

It also finds memory used after it was freed, and memory freed twice, with AddressSanitizer from `clang`. Its report shows the Iris lines where the memory was used, freed and allocated:

```output
ERROR: AddressSanitizer: heap-use-after-free
WRITE of size 1 at 0x502000000010 thread T0
    #0 in f.main.main main.iris:4:5
```

| Build | Checks |
|---|---|
| `debug` | all of them |
| `release-safe` | indexes and slices, and division by zero |
| `release-fast` | none |

A function marked `@unchecked` has none in any build, for code that has been measured and needs the speed:

```iris
@unchecked
u32 checksum(u8[] data) {
    u32 sum = 0
    for b in data {
        sum = sum * 31 + u32(b)
    }
    return sum
}
```
