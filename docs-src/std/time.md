## Dates

`time.date` makes a moment from its parts, in UTC. Its methods give the parts back, and `format` writes it with a layout:

```iris run
import time

void main() {
    t := time.date(2026, 10, 17, 9, 30)
    print("{t.year()}-{t.month()}-{t.day()} is a {t.weekday()}")
    scoped String iso = t.iso()
    scoped String eu = t.format("DD/MM/YYYY hh:mm")
    print("{iso}, {eu}")
    later := t.add(36h)
    scoped String d = later.dateText()
    print("36 hours later: {d}, {later.sub(t)} after")
}
```

```output
2026-10-17 is a saturday
2026-10-17T09:30:00Z, 17/10/2026 09:30
36 hours later: 2026-10-18, 36h0m0s after
```

The layout's letters are `YYYY`, `MM`, `DD`, `hh`, `mm`, `ss`, `SSS` (milliseconds) and `Z` (the offset); other characters are copied.

## Time zones

A moment is the same moment in every time zone; the offset only changes how it is shown. `local()` shows it in the computer's time zone, `utc()` in UTC, and `withOffset` with an offset in seconds east of UTC:

```iris run
import time

void main() {
    t := time.date(2026, 10, 17, 9, 30)
    rome := t.withOffset(2 * 3600)
    scoped String a = rome.iso()
    print("{a}, hour {rome.hour()}, same moment: {rome.unix() == t.unix()}")
}
```

```output
2026-10-17T11:30:00+02:00, hour 11, same moment: true
```

## Parsing

`time.parse` reads ISO 8601 text, with or without an offset, and fails with `TimeError.invalid` otherwise:

```iris run
import time

!void main() {
    t := try time.parse("2026-10-17T11:30:00+02:00")
    scoped String u = t.utc().iso()
    print(u)
    _ = time.parse("yesterday") catch e {
        print("{e.code}")
        time.Time{}
    }
}
```

```output
2026-10-17T09:30:00Z
TimeError.invalid
```

## Measuring

`time.now()` is the wall clock, which can jump when the computer's clock is set. To measure how long something takes, `time.clock()` gives a `Duration` from a clock that only goes forward:

```iris
import time

void main() {
    start := time.clock()
    work()
    print("took {time.clock() - start}")
}
```
