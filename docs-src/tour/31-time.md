# Time

The `time` module has the current time, a clock for measuring, and dates: making them, taking them apart, writing them and reading them back. Lengths of time are `Duration`s, the numbers with units like `250ms` from [Integers](03-integers.html#units).

## Moments

A `time.Time` is a moment: the nanoseconds since 1970-01-01 00:00 UTC, and the time zone it is shown in. `time.now()` gives the current moment, in UTC. `time.unix(seconds)` gives the moment that many seconds after 1970, and `unix()` and `unixMs()` give the seconds and milliseconds back:

```iris run
import time

void main() {
    t := time.unix(1700000000)
    print("{t} is {t.unixMs()} ms after 1970")
    now := time.now()
    print("now is later: {now.after(t)}")
}
```

```output
2023-11-14T22:13:20Z is 1700000000000 ms after 1970
now is later: true
```

A `Time` in text, `{t}`, is written in ISO 8601. `iso()` and `format` give its text as a new `String`, in that form or another (see [Writing a time](#writing-a-time)).

## Dates

`time.date(year, month, day)` makes a moment in UTC, with the month from 1 to 12; the hour, minute and second can follow and are 0 when left out. `year()`, `month()`, `day()`, `hour()`, `minute()`, `second()`, `nanosecond()` and `weekday()` give the parts back. A day past the end of its month goes on into the next one, and a month past 12 into the next year:

```iris run
import time

void main() {
    t := time.date(2026, 10, 17, 9, 30)
    print("{t.year()}-{t.month()}-{t.day()} {t.hour()}:{t.minute()}, a {t.weekday()}")
    leap := time.date(2026, 2, 29)
    print("{leap.month()}/{leap.day()}")
    print("{time.date(2026, 14, 1)}")
}
```

```output
2026-10-17 9:30, a saturday
3/1
2027-02-01T00:00:00Z
```

`weekday()` gives a `time.Weekday`, an enum that starts with `monday`.

## Adding and comparing

`add(d)` gives the moment a `Duration` later, or earlier when `d` is negative. `sub(other)` gives the `Duration` between two moments, and `before` and `after` compare them:

```iris run
import time

void main() {
    start := time.date(2026, 10, 17, 9, 30)
    end := start.add(36h + 15m)
    print("{end.day()} at {end.hour()}:{end.minute()}")
    print("{end.sub(start)} later, {start.sub(end)} the other way")
    print("{start.before(end)} {start.after(end)}")
    yesterday := start.add(-24h)
    print("yesterday was the {yesterday.day()}th")
}
```

```output
18 at 21:45
36h15m0s later, -36h15m0s the other way
true false
yesterday was the 16th
```

A day is always `24h` here: `add` counts time, not dates on a calendar. `addDays(n)`, `addMonths(n)` and `addYears(n)` move on the calendar instead, keeping the time of day. A day the new month doesn't have becomes its last, so January 31 and a month is February 28:

```iris run
import time

void main() {
    t := time.date(2026, 1, 31, 9, 30)
    print("{t.addDays(30)}")
    print("{t.addMonths(1)}")
    print("{t.addMonths(-2)}")
    leap := time.date(2024, 2, 29)
    print("{leap.addYears(1)}")
}
```

```output
2026-03-02T09:30:00Z
2026-02-28T09:30:00Z
2025-11-30T09:30:00Z
2025-02-28T00:00:00Z
```

They count in the offset the moment is shown with: for `t.local()`, the day changes at midnight where the program runs.

## Writing a time

`iso()` writes a moment in ISO 8601, the form most programs read, with the fraction of a second when there is one. `dateText()` writes only the date. `format(layout)` writes it with a layout made of `YYYY`, `MM`, `DD`, `hh`, `mm`, `ss`, `SSS` (milliseconds) and `Z` (the offset, `Z` for UTC); other characters are copied. `format()` with no layout is `iso()`, and is what `{t}` uses:

```iris run
import time

void main() {
    t := time.date(2026, 10, 17, 9, 5).add(250ms)
    scoped String iso = t.iso()
    scoped String day = t.dateText()
    scoped String eu = t.format("DD/MM/YYYY hh:mm")
    scoped String log = t.format("hh:mm:ss.SSS Z")
    print(iso)
    print(day)
    print(eu)
    print(log)
}
```

```output
2026-10-17T09:05:00.25Z
2026-10-17
17/10/2026 09:05
09:05:00.250 Z
```

## Time zones

A moment is the same moment in every time zone; the zone only changes how it is shown. `local()` shows it in the computer's time zone, `utc()` in UTC, and `withOffset(seconds)` with an offset in seconds east of UTC. The parts and the text follow the zone:

```iris run
import time

void main() {
    t := time.date(2026, 10, 17, 9, 30)
    rome := t.withOffset(2 * 3600)
    scoped String a = rome.iso()
    print("{a}, hour {rome.hour()}")
    print("same moment: {rome.unix() == t.unix()}")
    scoped String back = rome.utc().iso()
    print(back)
}
```

```output
2026-10-17T11:30:00+02:00, hour 11
same moment: true
2026-10-17T09:30:00Z
```

## Reading a time

`time.parse(text)` reads ISO 8601: a date, a date and time with `T` or a space between them, seconds with a fraction, and an offset (`Z`, `+02:00` or `+0200`). Without an offset it is UTC, and the moment it gives is shown with the offset it read. Text it can't read gives a `time.TimeError.invalid`, with a message that says why:

```iris run
import time

!void main() {
    t := try time.parse("2026-10-17 11:30:00.250+02:00")
    scoped String u = t.utc().iso()
    print(u)
    d := try time.parse("2026-10-17")
    print("{d.weekday()}")
    for text in ["2026-13-01", "2026-02-30", "2026-10-1x"] {
        _ = time.parse(text) catch e {
            print("{e.code}: {e.message}")
            time.Time{}
        }
    }
}
```

```output
2026-10-17T09:30:00.25Z
saturday
TimeError.invalid: '2026-13-01' has no such month or day
TimeError.invalid: '2026-02-30' has no such month or day
TimeError.invalid: '2026-10-1x' has 'x' where a digit should be
```

`parse` checks that the month has the day: `2026-02-29` is an error, and `2024-02-29`, in a leap year, isn't.

## Reading a duration

`time.parseDuration(text)` reads a `Duration` as it is written in Iris and in its output: `1m30s`, `250ms`, `1.5h`. A number alone is seconds. It suits durations from a settings file or a database; flags and `@env` fields read them already (see [Command-line flags](30-flags.html#types)):

```iris run
import time

void main() {
    for text in ["1m30s", "250ms", "1.5h", "30", "1m30", "soon"] {
        d := time.parseDuration(text) catch e {
            print(e.message)
            continue
        }
        print("{text} is {d}")
    }
}
```

```output
1m30s is 1m30s
250ms is 250ms
1.5h is 1h30m0s
30 is 30s
'1m30' isn't a duration like 30s or 1m30s
'soon' isn't a duration like 30s or 1m30s
```

## Measuring

`time.now()` follows the computer's clock, which can jump when the clock is set. To measure how long something takes, `time.clock()` reads a clock that only goes forward; the difference of two readings is a `Duration`:

```iris run
import time

void main() {
    start := time.clock()
    sleep(20ms)
    took := time.clock() - start
    print("took at least 20ms: {took >= 20ms}")
    print("took under a second: {took < 1s}")
}
```

```output
took at least 20ms: true
took under a second: true
```

`{took}` writes the `Duration` in its units, like `20.1ms`. As in [Integers](03-integers.html#units), dividing by a unit gives a plain number, so `took / 1ms` is the milliseconds, and `i64(took)` the nanoseconds.

## Waiting

`sleep(d)` waits for a `Duration`, and while one task sleeps the others run. In a `select`, an `after(d)` arm gives up waiting on channels after that time. Both are built in, without an import; [Tasks and channels](24-tasks-and-channels.html#select) shows them with tasks.

To do something at a steady rate, a loop sleeps until the next tick on the clock rather than for a fixed time, so the time the work takes doesn't add up. A `Duration` of 0 or less doesn't wait:

```iris run
import time

void main() {
    start := time.clock()
    next := start
    for i in 1..4 {
        next += 10ms
        sleep(next - time.clock())
        print("tick {i}")
    }
    took := time.clock() - start
    print("three ticks in at least 30ms: {took >= 30ms}")
}
```

```output
tick 1
tick 2
tick 3
three ticks in at least 30ms: true
```
