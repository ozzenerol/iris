## A snapshot

```iris
import sys

!void main() {
    m := try sys.memory()
    l := try sys.load()
    print("{m.used() / 1048576} MiB used, load {l.one}")
}
```

The [Machine numbers chapter](../library-tour/33-machine-numbers.html) of the Library Tour has rates, disks, mounts and processes.
