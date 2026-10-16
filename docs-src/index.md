# Iris

Iris is a compiled language for servers, network tools and system programs. It has no garbage collector. The compiler is written in C and produces native code through LLVM.

The language is still being designed. These pages describe it as planned.

- [Design](design/index.html) covers how the language works underneath and why. It is the reference for anyone working on the compiler.
- [Language](language/index.html) explains each part of the language, with examples.
- [Roadmap](roadmap.html) lists what gets built, in order.

```iris
import net

!(str, u16) splitHost(str addr) {
    i := addr.find(":")
    if i < 0 { return (addr, 80) }
    port := try parsePort(addr[i + 1:])
    return (addr[0:i], port)
}

!void main() {
    host, port := try splitHost("example.com:443")
    conn := try net.dial(host, port, timeout: 5s)
    defer conn.close()
    print("connected to {host}:{port}")
}
```
