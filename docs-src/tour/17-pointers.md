# Pointers

A pointer holds the address of a value, so code can read and change the value where it is instead of a copy of it. This chapter covers taking addresses, reading and writing through pointers, `null`, and the rules that keep pointers valid.

## Taking an address

`&` gives the address of a variable, a field or an item. The pointer's type is the value's type with a `*` after it. `*p` is the value the pointer points to, for reading and for writing:

```iris run
void main() {
    i64 x = 5
    i64 *p = &x
    print("{*p}")
    *p = 6
    print("{x}")
    *p += 10
    print("{x}")
}
```

```output
5
6
16
```

A pointer can point to a field or an item, as well as to a whole variable:

```iris run
struct Point {
    i64 x
    i64 y
}

void main() {
    p := Point{x: 1, y: 2}
    i64 *py = &p.y
    *py = 20
    i64[3] xs = [1, 2, 3]
    i64 *last = &xs[2]
    *last = 30
    print("{p.y} {xs[2]}")
}
```

```output
20 30
```

## Pointers to structs

A pointer to a struct reaches the struct's fields and methods with a dot, as the struct itself does: there is no separate operator for it.

```iris run
struct Account {
    str owner
    i64 balance
}

void deposit(Account *a, i64 amount) {
    a.balance += amount
}

void main() {
    acct := Account{owner: "ada", balance: 100}
    deposit(&acct, 50)
    Account *p = &acct
    p.balance -= 20
    print("{acct.owner}: {acct.balance}")
}
```

```output
ada: 130
```

This is the usual reason for a pointer parameter: the function changes the caller's value. Methods get `self` as a pointer for the same reason.

## null

A pointer can be `null`: it points to nothing. A pointer variable declared without a value starts as `null`, and `==` and `!=` compare pointers with each other and with `null`:

```iris run
struct Node {
    i64 value
    Node *next
}

i64 total(Node *n) {
    i64 sum = 0
    for n != null {
        sum += n.value
        n = n.next
    }
    return sum
}

void main() {
    c := Node{value: 3}
    b := Node{value: 2, next: &c}
    a := Node{value: 1, next: &b}
    print("{total(&a)} {total(null)}")
}
```

```output
6 0
```

Reading or writing through a `null` pointer is a bug. A debug build stops the program with a message that says where:

```output
panic: null pointer used
  at main.iris:6:15 in main
```

## Pointers stay valid only while their value lives

A pointer to a variable is valid while the variable exists: until the end of its block for a local, for the whole program for a global. A pointer into memory from `alloc` is valid until it is freed. Using a pointer after that is a bug that a debug build finds and reports (see [Memory](19-memory.html)).

The compiler stops the plain case, a function giving back a pointer or a slice into its own variables:

```iris error
i64 *counter() {
    i64 n = 0
    return &n // error: this points into n, which is gone when counter returns; return a copy, or memory from alloc
}
```

A function that needs to give back something that outlives it returns the value itself, or memory from `alloc` that the caller frees.

## Pointers and memory

`u8 *` is a pointer to bytes, and stands for "a pointer to anything", as C's `void *` does: any pointer converts to `u8 *` on its own, and a `u8 *` converts back to any pointer type. This is how memory is passed to and from C code:

```iris run
void main() {
    i64 n = 42
    u8 *any = &n
    i64 *back = any
    print("{*back}")
}
```

```output
42
```

Adding a number to a pointer moves it by that many items, not bytes, as in C:

```iris run
void main() {
    i64[4] xs = [10, 20, 30, 40]
    i64 *p = &xs[0]
    i64 *third = p + 2
    print("{*third}")
}
```

```output
30
```

A pointer has no length, so the program can't check that `p + 2` is still inside the array. A slice knows its length and is checked on every index, which is why slices are the usual way to pass a run of items. A pointer can be turned into a slice when the length is known, with `p[0:n]`, for memory that came from C:

```iris run
void main() {
    i64[4] xs = [10, 20, 30, 40]
    i64 *p = &xs[1]
    s := p[0:3]
    print("{s.len} {s[0]} {s[2]}")
}
```

```output
3 20 40
```

A pointer to a pointer is written with two stars, `T **`, and is mostly found in C functions that hand back a pointer through an argument.
