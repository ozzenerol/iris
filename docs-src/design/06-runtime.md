# The runtime

What `runtime/rt.c` does and how the generated code uses it. The runtime is one C file, carried inside the `iris` binary and compiled with every program (see [The IR writer](04-ir.html#calling-clang)). Its exported functions start with `iris_`. The generated code calls the ones `writeIR` declares, and the standard library calls the rest through `@extern`.

## Layout of rt.c

The file is split into sections, each starting with a `/* ---- name */` line:

- memory: the allocator, `iris_malloc`, `iris_free`, `String` growth.
- errors and printing: the message and trace buffers, `iris_write_*`, panics.
- big, os, fs, io, term: the C side of those std modules.
- tasks, io_uring, channels, locks: the scheduler and what tasks wait on.
- net, addresses and UDP, sockets, Unix sockets, TLS: connections.
- threads, json, tests, hashing, time, processes.
- random and crypto: SHA-256, HMAC and the rest, without OpenSSL.
- SQLite, env.load, flags.parse, signals, compress, archives.

## Memory

`iris_malloc`, `iris_calloc` and `iris_free` serve both the generated code and the runtime.

1. Each thread has a heap of 64 KiB pages, taken from the system in 4 MiB segments. A page holds blocks of one size class: 16-byte steps up to 1 KiB, then four steps for each doubling up to 32 KiB. Taking a block and giving it back is a few instructions with no lock.
2. A block freed by another thread goes on its page's list of remote frees, a lock-free stack the owner takes back when it needs room. A full page that gets such a block is put on the owner's list of pages to look at again.
3. A page with nothing in use goes back to the heap's free pages. Past 64 of those, its memory goes back to the system. A thread that ends leaves its heap to the next thread made.
4. A block of a fresh page is zero already, so `alloc` and `calloc` don't write zeros into it, and its memory isn't touched until the program uses it.
5. Blocks over 32 KiB come from the C library's `malloc`. `iris_free` tells its own blocks from others by a bitmap with one bit for every 4 MiB of address space, so memory from the C library or from C code goes to the C library's `free`.
6. Under AddressSanitizer, in debug builds, every allocation goes to the C library, which it checks.

## Errors

The current error's message (up to 1 KiB) and trace (up to 64 frames) live in thread-local buffers: `msgCur` and `trace`.

1. `iris_error_begin` empties the second buffer, `msgNext`, and the generated code writes the new message into it. A message can therefore quote the error it replaces: `error(e.code, "loading user {id}: {e.message}")`.
2. `iris_error_finish` copies `msgNext` into `msgCur`, resets the trace to the place of the `error(...)` call and returns the code.
3. Each `try` that passes the error on adds a frame with `iris_trace_add`.
4. `iris_error_keep` and `iris_error_back` protect the current error while deferred code runs on the way out. A copy is made only if the message or trace is about to change.
5. An error that leaves `main` reaches `iris_error_report`, which prints the code by name (through `iris_enum_table`), the message and the trace, and exits with status 1.

A panic or a failed check prints `panic:`, the message and the place, and exits with status 2.

## Tasks

`spawn f(a, b)` calls `iris_spawn(thunk, env)`. The task gets its own 256 KiB stack, made with `mmap`, with a guard page below it, so running past the stack crashes instead of corrupting memory. Finished tasks keep their stacks for the next spawns, up to 1024 per thread (`taskCache`), so a busy server spawns without system calls. A thread gives the stacks back when it ends.

Each thread has its own scheduler, made of thread-local variables: the current task, a ready queue, a list of sleepers sorted by wake time, and the event loop. A task runs until it waits for I/O, a channel, a lock or a timer. Then `schedule` takes the next ready task, or waits in the event loop when none is ready, and `switchTo` changes stacks with `iris_ctx_switch`. That is a few instructions of assembly per machine (x86_64 and arm64): save the registers a call must keep, swap the stack pointer, load the other task's registers. Under AddressSanitizer, the switches are announced with `__sanitizer_start_switch_fiber`.

The event loop uses io_uring on Linux. Reads and accepts are handed to the kernel, and one system call per turn submits them all and collects the results. Where io_uring is missing or blocked, as in many containers, it uses epoll, and on macOS kqueue. `IRIS_IO=epoll` picks epoll on Linux. A file descriptor is added to epoll or kqueue once, edge-triggered, and stays there until it is closed. `waitFd` parks a task on a descriptor, with an optional deadline.

A task that another thread wakes, for example by sending to a channel it waits on, goes into its thread's inbox (`Sched`). A write to the thread's wake descriptor (an eventfd on Linux, a pipe on macOS) interrupts its event loop.

## Channels and select

A channel is a ring buffer of values with two wait queues, one for tasks waiting to send and one for tasks waiting to receive. The queues are first come first served, and a waiter leaves its queue in constant time. A channel with no buffer, `chan[T](0)`, hands the value straight from sender to receiver. A short spin lock guards each channel.

`iris_select` puts its task on the wait queue of every channel it waits on, and on the timer list for `after`. Any of them can wake it, so each waker must first win the task's claim. Only the first claim wakes it.

## Mutex and Atomic

`iris_mutex_lock` takes the lock or puts the task on the mutex's wait list and lets the other tasks of its thread run. `unlock` hands the lock to the first waiter. `Atomic[T]` methods need no runtime: they are LLVM atomic instructions.

## Threads

`thread f(a, b)` calls `iris_thread_new(thunk, env)`, which starts a detached pthread. The thread runs the thunk on a scheduler of its own. A thread ends when its function and every task it spawned have finished, and then wakes the task waiting in `join`. Tasks never move between threads, so a task needs no locks for its own data. `http.serve` starts one thread per core, each with its own event loop, and several threads may `accept` on one listener.

## TLS and compression

TLS uses OpenSSL and compression uses zlib. Both are loaded with `dlopen` the first time a program uses them, so building needs no headers or link flags, and programs that don't use them don't depend on them. TLS lives inside `net.Conn`, so reading lines, the HTTP server and the client work the same over TLS. OpenSSL's calls hold SIGPIPE back on Linux.
