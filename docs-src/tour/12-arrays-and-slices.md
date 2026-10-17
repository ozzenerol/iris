# Arrays and slices

An array is a fixed number of values stored one after another. A slice looks at some of them, or at memory from `alloc`, and knows how many there are. Slices are how most functions take a list of things.

## Arrays

An array's type is its item type followed by its size in brackets. A literal lists the items in brackets too:

```iris run
void main() {
    i64[4] counts = [10, 20, 30, 40]
    str[2] names = ["ada", "zoe"]
    u8[16] buffer
    print("{counts[0]} {counts[3]} {names[1]} {buffer[15]} {counts.len}")
}
```

```output
10 40 zoe 0 4
```

The size is part of the type and is known when compiling, so `counts.len` costs nothing. An array declared without a value has every item at zero. A literal must have as many items as the array.

An index runs from 0 to `len - 1`. Every index is checked: one outside the array stops the program with a message that says where. When the index is a constant, the compiler checks it before the program runs:

```output
panic: index 3 is out of range for length 3
  at main.iris:4:14 in main
```

Arrays are values. Assigning one copies all its items, and so does passing it to a function that takes an array:

```iris run
void main() {
    i64[3] a = [1, 2, 3]
    b := a
    b[0] = 99
    print("{a[0]} {b[0]}")
}
```

```output
1 99
```

An array of arrays is written with the sizes one after the other, read from the left: `i64[3][2]` is two arrays of three `i64`s each. The first index picks one of the two:

```iris run
void main() {
    i64[3][2] grid
    grid[1][2] = 5
    print("{grid.len} rows of {grid[0].len}, grid[1][2] = {grid[1][2]}")
}
```

```output
2 rows of 3, grid[1][2] = 5
```

## Slices

A slice, `T[]`, is a pointer to items and their count. It doesn't own them: they live in an array, in memory from `alloc`, or anywhere else. An array can be given wherever a slice of its item type is wanted, so a function that takes a slice works with an array of any size:

```iris run
i64 sum(i64[] xs) {
    i64 total
    for x in xs {
        total += x
    }
    return total
}

void main() {
    i64[3] small = [1, 2, 3]
    i64[5] large = [10, 20, 30, 40, 50]
    print("{sum(small)} {sum(large)} {sum([7, 8])}")
}
```

```output
6 150 15
```

A slice looks at the same items as what it came from, so changing an item through it changes the original:

```iris run
void zero(i64[] xs) {
    for i in 0..xs.len {
        xs[i] = 0
    }
}

void main() {
    i64[4] nums = [1, 2, 3, 4]
    zero(nums[1:3])
    print("{nums[0]} {nums[1]} {nums[2]} {nums[3]}")
}
```

```output
1 0 0 4
```

## Slicing

`a[from:to]` takes the items from `from` up to `to - 1`, as a slice. Either end can be left out: `a[:n]` is the first n items and `a[n:]` all but the first n. Slicing an array, a slice or a `str` copies nothing:

```iris run
void main() {
    i64[6] xs = [0, 1, 2, 3, 4, 5]
    s := xs[2:5]
    print("{s.len} items: {s[0]} {s[1]} {s[2]}")
    head := xs[:2]
    tail := xs[4:]
    print("{head.len} {tail.len} {tail[0]}")
    inner := s[1:]
    print("{inner[0]}")
}
```

```output
3 items: 2 3 4
2 2 4
3
```

Both ends are checked: a slice can't reach past the end of what it is taken from, and its start can't be after its end. `a[n:n]` is an empty slice.

`[]` is an empty slice of any type, for a parameter with nothing to give:

```iris run
void list(str[] items) {
    print("{items.len} items")
}

void main() {
    list([])
    list(["one", "two"])
}
```

```output
0 items
2 items
```

## Slices of new memory

`alloc(T, n)` gives a slice of `n` new items of type `T`, all at zero. `free` gives the memory back. Between the two, the slice is used like any other:

```iris run
void main() {
    n := 5
    i64[] squares = alloc(i64, n)
    defer free(squares)
    for i in 0..n {
        squares[i] = i * i
    }
    print("{squares[4]} of {squares.len}")
}
```

```output
16 of 5
```

`defer` frees it when the block ends, on every way out (see [Memory](19-memory.html)). A slice from `alloc` has a fixed length; a list that grows is a `List` (see [Collections](22-collections.html)).

## A slice's parts

`.len` is a slice's count of items and `.ptr` the pointer to its first one, for C functions. A pointer can be sliced with both ends given, `p[0:n]`, to look at memory that came from C as a slice. That slice isn't checked against anything, since a pointer has no length:

```iris run
void main() {
    i64[3] xs = [4, 5, 6]
    s := xs[0:3]
    i64 *p = s.ptr
    again := p[0:2]
    print("{s.len} {again[1]}")
}
```

```output
3 5
```

## Text is a slice

A `str` is a slice of bytes, `u8[]`, so everything here works on text too: indexing gives a byte, slicing gives a `str`, and `for` goes over its bytes (see [Text](06-text.html)).
