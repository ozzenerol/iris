# Machine numbers

The `sys` module reads what the Linux kernel counts about the machine: CPU time, load, memory and swap, uptime, OOM kills, network and disk traffic, mounts, and the memory and CPU time of a process. Each call reads one file under `/proc` and gives a snapshot. It runs on Linux only. Elsewhere every call fails with `SysError.notFound`.

## Memory, load and uptime

```iris
import sys

!void main() {
    m := try sys.memory()
    print("memory: {m.used() / 1048576} of {m.total / 1048576} MiB used, swap {m.swapUsed() / 1048576} MiB")
    l := try sys.load()
    print("load: {l.one:.2} {l.five:.2} {l.fifteen:.2}, {l.tasks} tasks")
    print("up {try sys.uptime()}, {try sys.oomKills()} processes killed for lack of memory")
}
```

```output
memory: 10912 of 31723 MiB used, swap 4261 MiB
load: 0.84 0.92 0.86, 1879 tasks
up 68h55m12.4s, 0 processes killed for lack of memory
```

`used()` is `total` less `available`, as `free` counts it. `available` is what programs can still get without swapping, counting the caches the kernel can drop. The uptime goes down only after a reboot, so a monitor that sees it drop knows the machine restarted.

## Rates from two snapshots

The kernel's counters grow from boot. A rate is the difference between two snapshots taken some time apart. `Cpu.percentSince` does this for the CPU, and `Disk.percentSince` for how busy a disk was:

```iris
import sys

!void main() {
    c1 := try sys.cpu()
    n1 := try sys.network()
    defer n1.freeEach()
    sleep(1s)
    c2 := try sys.cpu()
    n2 := try sys.network()
    defer n2.freeEach()
    print("cpu {c2.percentSince(c1):.1}%")
    for i in 0..n2.len {
        a := n1.at(i)
        b := n2.at(i)
        if a.name == b.name && a.name != "lo" {
            print("{b.name}: {b.rxBytes - a.rxBytes} B/s in, {b.txBytes - a.txBytes} B/s out")
        }
    }
}
```

```output
cpu 3.2%
eno1: 1840 B/s in, 512 B/s out
```

`cpu()` adds up every core and `cpus()` gives each one. The times are `Duration`s, so `c.idle` prints as `689h57m50.93s`. `network()` lists the interfaces of the program's network namespace, loopback too, with bytes, packets, errors and drops each way. The items of the lists that `network`, `disks` and `mounts` give hold `String`s, so `freeEach` frees them.

## Disks and mounts

`disks()` gives the reads and writes of every block device, partitions, loop and dm devices too, from `/proc/diskstats`. `mounts()` gives the mounted file systems, and `mountOf` the one a path is on. `Mount.space()` is `fs.space` on its path:

```iris
import sys

!void main() {
    all := try sys.mounts()
    defer all.freeEach()
    for m in all {
        if m.fsType == "ext4" || m.fsType == "xfs" || m.fsType == "cifs" {
            s := try m.space()
            print("{m.path} ({m.fsType}{if m.readOnly() { ", read-only" } else { "" }}): {(s.total - s.available) * 100 / s.total}% full")
        }
    }
    scoped share := try sys.mountOf("/mnt/nas/photos")
    print("photos are on {share.device}")
}
```

```output
/ (ext4): 41% full
/mnt/nas (cifs): 63% full
photos are on //192.168.1.160/share
```

## A process

`process()` reads this program's `/proc/self/stat`, and `process(pid)` another's. It gives the name, the parent, the state, the threads, the CPU time used, and the memory it holds (`rss`) and its address space (`virtual`), in bytes:

```iris
import sys

!void main() {
    scoped me := try sys.process()
    print("{me.name} ({me.state}): {me.threads} threads, {me.rss / 1024} KiB, {me.cpuTime()} of CPU")
    _ = sys.process(1) catch e {
        print("{e.code}")
        sys.Process{name: "".copy()}
    }
}
```

```output
monitor (running): 1 threads, 7436 KiB, 0s of CPU
```

There is no such process when it fails with `SysError.notFound`.

## Serving them to Prometheus

With the [metrics module](28-metrics-and-tracing.html), a task can copy the numbers into gauges every few seconds, and `/metrics` serves them:

```iris
import http, metrics, sys

void collect() {
    cpu := metrics.gauge("node_cpu_percent", "CPU busy, from 0 to 100")
    used := metrics.gauge("node_memory_used_bytes", "Memory in use")
    load := metrics.gauge("node_load1", "Load average over a minute")
    before := sys.cpu() catch sys.Cpu{}
    for {
        sleep(5s)
        now := sys.cpu() catch before
        cpu.set(now.percentSince(before))
        before = now
        m := sys.memory() catch sys.Memory{}
        used.set(f64(m.used()))
        l := sys.load() catch sys.Load{}
        load.set(l.one)
    }
}

!void main() {
    spawn collect()
    http.Router r
    r.get("/metrics", metrics.handler)
    try r.serve(9100)
}
```
