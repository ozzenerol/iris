# Overview

What Iris is for, who uses it, and the ideas behind its design.

## What Iris is for

Iris is for programs that run close to the operating system: servers, proxies, databases, command-line tools and daemons. Programs should be short to write and run about as fast as the same program in C.

## Who uses it

- Programmers writing servers, proxies, databases and other network services.
- C programmers who want slices, strings, generics and checked errors.
- Go programmers who need control over memory.
- People writing command-line tools and daemons.

Iris is not designed for GUI applications, web front ends or data science.

## Ideas behind the design

### Keep the language small

There are few rules to learn. A feature is added only if it removes a lot of code and is easy to explain. A feature that saves a little typing but adds a new idea stays out. This is why Iris has one loop keyword and no lambdas.

### Make costs visible

Reading a function should tell you what it costs. A function that can fail says so in its return type (`!T`). A function that allocates memory takes an allocator. Nothing runs behind the program's back: there is no garbage collector, no hidden copies and no exceptions.

### Stay close to C

Iris uses the C calling convention and lays out structs the way C does. C headers can be imported directly. Any C library can be used, and C code can call Iris functions.

### Check in debug builds

Debug builds check array bounds, integer overflow, null pointers and use after free. Release builds keep the bounds checks. A function marked `@unchecked` drops them.

## How the compiler works

The compiler is a C program. It reads Iris source, checks it, and writes LLVM IR, which `clang` turns into a native program. The [compiler](compiler.html) page has the details, and [How values are stored](values.html) shows what each Iris type looks like in memory.
