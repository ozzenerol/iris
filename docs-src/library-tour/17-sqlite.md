# SQLite

The `sqlite` module keeps data in a SQLite database, in one file or in memory, with tables read and changed with SQL. The system's SQLite is loaded the first time a program opens a database, so a program that uses it needs SQLite installed.

## Opening a database

`sqlite.open(path)` opens the database in a file and gives a `!sqlite.Db`. When the file doesn't exist, SQLite creates it. `close` closes the database, and `defer db.close()` closes it when the function ends, however it ends. What was written stays in the file for the next time it is opened.

```iris run
import fs, sqlite

// Records that the service started, and says how many times it has.
!void started(str path) {
    db := try sqlite.open(path)
    defer db.close()

    try db.exec("CREATE TABLE IF NOT EXISTS starts (n INTEGER)")
    try db.exec("INSERT INTO starts VALUES (1)")
    print("starts: {try count(&db)}")
}

!i64 count(sqlite.Db *db) {
    rows := try db.query("SELECT COUNT(*) FROM starts")
    defer rows.close()

    _ = try rows.next()
    return rows.int(0)
}

!void main() {
    path := "/tmp/iris-tour-starts.db"
    try started(path)
    try started(path)
    try fs.remove(path)
}
```

```output
starts: 1
starts: 2
```

`count` reads the table with `query`, which [Row by row](#row-by-row) covers.

The path `":memory:"` gives a database that lives only in memory and is gone when it is closed. The other examples in this chapter use it, so each starts with empty tables.

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE t (x INTEGER)")
    print("open")
}
```

```output
open
```

## Changing data

`exec` runs SQL that gives no rows: `CREATE`, `INSERT`, `UPDATE`, `DELETE`. Without values, the SQL can hold several statements separated by `;`, which is handy for creating the tables.

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()

    try db.exec(r"""
        CREATE TABLE hosts (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, port INTEGER);
        CREATE TABLE checks (id INTEGER PRIMARY KEY, host INTEGER, ok INTEGER);
        INSERT INTO hosts (name, port) VALUES ('db', 5432), ('cache', 6379);
        """)

    try db.exec("UPDATE hosts SET port = port + 10000")
    print("{db.changes()} changed")
}
```

```output
2 changed
```

`db.changes()` is how many rows the last `INSERT`, `UPDATE` or `DELETE` changed. `db.lastId()` is the id of the last row an `INSERT` added.

## Values in SQL

Each `?` in the SQL takes the next value from the list that follows it. The values are kept apart from the SQL text, so text with a quote in it, or text from the network, can't change what the SQL does. A value is a `sqlite.Value`:

- `.int`, `.float` or `.text`
- `.blob` for bytes
- `.none` for NULL

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE hosts (id INTEGER PRIMARY KEY, name TEXT, port INTEGER, note TEXT)")

    try db.exec("INSERT INTO hosts (name, port, note) VALUES (?, ?, ?)", [.text("db"), .int(5432), .text("the main database")])
    print("db is {db.lastId()}")

    try db.exec("INSERT INTO hosts (name, port, note) VALUES (?, ?, ?)", [.text("old"), .none, .text("ops' test box")])
    print("old is {db.lastId()}")

    try db.exec("DELETE FROM hosts WHERE port IS NULL")
    print("{db.changes()} deleted")
}
```

```output
db is 1
old is 2
1 deleted
```

With values, `exec` runs one statement. SQL with another statement after it fails with `SqliteError.failed` before anything runs. The number of values must match the number of `?`.

## Bytes

A BLOB column keeps bytes as they are, for certificates, hashes or packed data. `.blob(bytes)` stores a `u8[]` in one and `rows.bytes(col)` reads it back. Like `text`, the result is valid until the next `next`. A `String` field of a struct read with `all` or `one` gets the bytes too.

```iris run
import sqlite

@json
struct Cert {
    String host
    String der
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE certs (host TEXT, der BLOB)")

    u8[6] start = [0x30, 0x82, 0x03, 0x0a, 0x30, 0x00] // how a DER certificate starts
    try db.exec("INSERT INTO certs VALUES (?, ?)", [.text("api.example.com"), .blob(start[:])])

    rows := try db.query("SELECT der, typeof(der) FROM certs")
    defer rows.close()
    _ = try rows.next()
    b := rows.bytes(0)
    print("{rows.text(1)}: {b.len} bytes, the first {b[0]}, the last {b[5]}")

    scoped Cert c = try sqlite.one[Cert](&db, "SELECT * FROM certs")
    print("{c.host}: {c.der.len} bytes")
}
```

```output
blob: 6 bytes, the first 48, the last 0
api.example.com: 6 bytes
```

## Rows into structs

`sqlite.all[T]` runs a query and gives a `List` of `T`, a `@json` struct (see [JSON](01-json.html)).

- Each field is filled from the column of the same name, or the name given with `@json("name")`.
- Columns without a field are skipped.
- Fields without a column keep their defaults.
- The caller frees the `String`s and the list.

```iris run
import sqlite

@json
struct Host {
    i64 id
    String name
    i64 port
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE hosts (id INTEGER PRIMARY KEY, name TEXT, port INTEGER)")
    try db.exec("INSERT INTO hosts (name, port) VALUES ('db', 5432), ('cache', 6379), ('dns', 53)")

    hosts := try sqlite.all[Host](&db, "SELECT * FROM hosts WHERE port > ? ORDER BY name", [.int(1023)])
    defer {
        for h in hosts {
            h.free()
        }
        hosts.free()
    }

    for h in hosts {
        print("{h.id} {h.name} {h.port}")
    }
}
```

```output
2 cache 6379
1 db 5432
```

`sqlite.one[T]` gives the first row. When the query finds nothing, it fails with `SqliteError.noRows`.

```iris run
import sqlite

@json
struct Host {
    i64 id
    String name
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE hosts (id INTEGER PRIMARY KEY, name TEXT)")
    try db.exec("INSERT INTO hosts (name) VALUES ('db'), ('dns')")

    scoped Host dns = try sqlite.one[Host](&db, "SELECT * FROM hosts WHERE name = ?", [.text("dns")])
    print("found {dns.name}, id {dns.id}")

    _ = sqlite.one[Host](&db, "SELECT * FROM hosts WHERE id = ?", [.int(99)]) catch e {
        print("{e.code}")
        Host{}
    }
}
```

```output
found dns, id 2
SqliteError.noRows
```

A field can be an integer, a float, a `bool` (0 is `false`), a `String`, or a plain enum read from the text of its value's name. Text that names no value fails with `SqliteError.failed`.

```iris run
import sqlite

enum State { up, down }

@json
struct Check {
    String host
    State state
    bool tls
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE checks (host TEXT, state TEXT, tls INTEGER)")
    try db.exec("INSERT INTO checks VALUES ('api', 'up', 1), ('cache', 'down', 0)")

    checks := try sqlite.all[Check](&db, "SELECT * FROM checks")
    defer {
        for c in checks {
            c.free()
        }
        checks.free()
    }

    for c in checks {
        print("{c.host}: {c.state}, tls {c.tls}")
    }

    try db.exec("UPDATE checks SET state = 'flapping' WHERE host = 'cache'")
    _ = sqlite.one[Check](&db, "SELECT * FROM checks WHERE host = 'cache'") catch e {
        print("{e.code}: {e.message}")
        Check{}
    }
}
```

```output
api: up, tls true
cache: down, tls false
SqliteError.failed: the column state has 'flapping', which isn't one of the enum's values
```

A number that doesn't fit its field fails the same way instead of being cut down to fit:

```iris run
import sqlite

@json
struct Route {
    u8 ttl
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE routes (ttl INTEGER)")
    try db.exec("INSERT INTO routes VALUES (300)")

    _ = sqlite.one[Route](&db, "SELECT * FROM routes") catch e {
        print("{e.code}: {e.message}")
        Route{}
    }
}
```

```output
SqliteError.failed: the column ttl has 300, which doesn't fit in u8
```

When `all` or `one` fails partway, it frees what it had read, so the caller has nothing to clean up.

## Row by row

`db.query` gives `Rows`, read one row at a time.

- `next` moves to the next row and returns `false` when there are no more.
- `int`, `float` and `text` read a column by its number, from 0.
- `isNull` says whether a column is NULL.
- `close` ends the query.

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE pings (host TEXT, rtt REAL); INSERT INTO pings VALUES ('api', 1.5), ('cache', NULL), ('db', 4)")

    rows := try db.query("SELECT host, rtt FROM pings ORDER BY host")
    defer rows.close()

    for try rows.next() {
        if rows.isNull(1) {
            print("{rows.text(0)}: no reply")
        } else {
            print("{rows.text(0)}: {rows.float(1)}ms")
        }
    }
}
```

```output
api: 1.5ms
cache: no reply
db: 4ms
```

The `str` that `text` gives points into SQLite's memory and is valid until the next call to `next`. Copy it with `.copy()` to keep it. `sqlite.scan(&rows, &v)` fills a `@json` struct from the current row, as `sqlite.all` does for each row.

For a query whose columns aren't known in advance, `columns` gives the number of columns and `name` a column's name:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()

    rows := try db.query("SELECT 8080 AS port, 'edge-1' AS host, 0.25 AS load")
    defer rows.close()

    _ = try rows.next()
    for i in 0..rows.columns() {
        print("{rows.name(i)} = {rows.text(i)}")
    }
}
```

```output
port = 8080
host = edge-1
load = 0.25
```

A query that adds things up gives one row, read the same way:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE requests (status INTEGER, bytes INTEGER); INSERT INTO requests VALUES (200, 5120), (502, 120), (503, 80)")

    rows := try db.query("SELECT COUNT(*), SUM(bytes) FROM requests WHERE status >= ?", [.int(500)])
    defer rows.close()

    _ = try rows.next()
    print("{rows.int(0)} failed requests, {rows.int(1)} bytes")
}
```

```output
2 failed requests, 200 bytes
```

## NULL

A NULL column reads as `0` with `int` and `float` and as `""` with `text`, so use `isNull` to tell. With `sqlite.all` and `sqlite.one`, a NULL column leaves the field at its default.

```iris run
import sqlite

@json
struct Host {
    String name
    i64 port = -1
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE hosts (name TEXT, port INTEGER)")
    try db.exec("INSERT INTO hosts VALUES (?, ?), (?, ?)", [.text("db"), .int(5432), .text("new"), .none])

    hosts := try sqlite.all[Host](&db, "SELECT * FROM hosts")
    defer hosts.free()

    for h in hosts {
        if h.port < 0 {
            print("{h.name}: no port yet")
        } else {
            print("{h.name}: {h.port}")
        }
        h.free()
    }
}
```

```output
db: 5432
new: no port yet
```

## Transactions

`begin`, `commit` and `rollback` group several changes so they all happen or none does. Between `begin` and `commit`, only this connection sees the changes, and `rollback` drops them. `errdefer` (see [Memory](../tour/19-memory.html#errdefer)) rolls back on every exit with an error, so no step can leave the transaction open.

```iris run
import sqlite

// Moves bandwidth, in Mbit/s, from one link's share to another's.
!void move(sqlite.Db *db, i64 from, i64 to, i64 mbps) {
    try db.begin()
    errdefer _ = db.rollback()

    try db.exec("UPDATE links SET mbps = mbps - ? WHERE id = ?", [.int(mbps), .int(from)])
    try db.exec("UPDATE links SET mbps = mbps + ? WHERE id = ?", [.int(mbps), .int(to)])

    try db.commit()
}

!void show(sqlite.Db *db) {
    rows := try db.query("SELECT id, mbps FROM links ORDER BY id")
    defer rows.close()

    for try rows.next() {
        print("  {rows.int(0)}: {rows.int(1)}")
    }
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec(r"""
        CREATE TABLE links (id INTEGER PRIMARY KEY, mbps INTEGER CHECK (mbps <= 100));
        INSERT INTO links (mbps) VALUES (50), (90);
        """)

    try move(&db, 1, 2, 5)
    print("after 5:")
    try show(&db)

    move(&db, 1, 2, 20) catch e {
        print("20 failed: {e.code}")
    }
    print("after 20:")
    try show(&db)
}
```

```output
after 5:
  1: 45
  2: 95
20 failed: SqliteError.constraint
after 20:
  1: 45
  2: 95
```

The second move took 20 from link 1, then failed on link 2, whose `CHECK` allows at most 100. The rollback restored link 1.

## Errors

The errors are values of `sqlite.SqliteError`. The message says whether the SQL itself is wrong (`in the SQL`) or failed while it ran (`running the SQL`), then gives SQLite's reason.

| Code | When |
|---|---|
| `failed` | the SQL is wrong, a table is missing, or the wrong number of values was given |
| `busy` | another program kept the database locked for more than 5 seconds |
| `constraint` | a `UNIQUE`, `NOT NULL`, `CHECK` or other constraint isn't met |
| `noRows` | `sqlite.one` found nothing |

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE hosts (name TEXT UNIQUE NOT NULL)")
    try db.exec("INSERT INTO hosts VALUES ('db')")

    db.exec("INSERT INTO hosts VALUES (?)", [.text("db")]) catch e {
        print("{e.code}: {e.message}")
    }
    db.exec("INSERT INTO hosts VALUES (?)", [.none]) catch e {
        print("{e.code}: {e.message}")
    }
    db.exec("SELEKT 1") catch e {
        print("{e.code}: {e.message}")
    }
    db.exec("INSERT INTO hosts VALUES (?)", [.text("a"), .text("b")]) catch e {
        print("{e.code}: {e.message}")
    }
}
```

```output
SqliteError.constraint: running the SQL: UNIQUE constraint failed: hosts.name
SqliteError.constraint: running the SQL: NOT NULL constraint failed: hosts.name
SqliteError.failed: in the SQL: near "SELEKT": syntax error
SqliteError.failed: the SQL has 1 ? but 2 values were given
```

Threads can share a `Db`. Each call waits for the database on the thread that makes it. A `Db` keeps the statements it prepares and runs them again when the same SQL comes back, so a query run in a loop is prepared once. `IRIS_SQLITE` names another copy of SQLite to load instead of the system's.

For a database server, [PostgreSQL and Redis](18-postgres-and-redis.html) covers `pg` and `redis`.
