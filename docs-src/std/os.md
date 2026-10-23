## The program and the machine

`arg` and `env` read the program's arguments and environment. `cwd` and `chdir` get and change the working folder, and `home`, `hostname` and `pid` tell about the user, the machine and the program:

```iris run
import os

!void main() {
    scoped String start = try os.cwd()
    try os.chdir("/")
    scoped String root = try os.cwd()
    try os.chdir(start)
    scoped String home = try os.home()
    print("{root} {home.len > 0} {os.pid() > 0} {os.argc() >= 1}")
}
```

```output
/ true true true
```
