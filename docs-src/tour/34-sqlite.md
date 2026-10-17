# SQLite

The `sqlite` module keeps data in a SQLite database: one file, or memory, with tables read and changed with SQL. The system's SQLite is loaded the first time a program opens a database, so a program that uses it needs SQLite installed.

## Opening a database

`sqlite.open(path)` opens the database in a file and gives a `!sqlite.Db`. When the file isn't there, SQLite makes it. `close` closes it, and `defer db.close()` closes it when the function ends, however it ends. What was written stays in the file for the next time it is opened:

```iris run
import fs, sqlite

!void visit(str path) {
    db := try sqlite.open(path)
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS visits (n INTEGER)")
    try db.exec("INSERT INTO visits VALUES (1)")
    print("visits: {try count(&db)}")
}

!i64 count(sqlite.Db *db) {
    rows := try db.query("SELECT COUNT(*) FROM visits")
    defer rows.close()
    _ = try rows.next()
    return rows.int(0)
}

!void main() {
    path := "/tmp/iris-tour-visits.db"
    try visit(path)
    try visit(path)
    try fs.remove(path)
}
```

```output
visits: 1
visits: 2
```

`count` reads the table with `query`, which [Row by row](#row-by-row) shows.

The path `":memory:"` gives a database that lives only in memory, and is gone when it is closed. The other examples in this chapter use it, so each starts with empty tables:

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

`exec` runs SQL that gives no rows: `CREATE`, `INSERT`, `UPDATE`, `DELETE`. Without values, the SQL can hold several statements separated by `;`, which suits making the tables:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec(r"""
        CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, age INTEGER);
        CREATE TABLE posts (id INTEGER PRIMARY KEY, user INTEGER, title TEXT);
        INSERT INTO users (name, age) VALUES ('Ada', 36), ('Zoe', 41);
        """)
    try db.exec("UPDATE users SET age = age + 1")
    print("{db.changes()} changed")
}
```

```output
2 changed
```

`db.changes()` is how many rows the last `INSERT`, `UPDATE` or `DELETE` changed, and `db.lastId()` is the id of the last row `INSERT` added.

## Values in SQL

Each `?` in the SQL takes the next value from the list given after it. The values are kept apart from the SQL text, so a name with a quote in it, or one typed by a user, can't change what the SQL does. A value is one of `sqlite.Value`: `.int`, `.float`, `.text`, `.blob` for bytes, or `.none` for NULL:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER, score REAL)")
    try db.exec("INSERT INTO users (name, age, score) VALUES (?, ?, ?)", [.text("Ada"), .int(36), .float(9.5)])
    print("Ada is {db.lastId()}")
    try db.exec("INSERT INTO users (name, age, score) VALUES (?, ?, ?)", [.text("O'Brien"), .none, .none])
    print("O'Brien is {db.lastId()}")
    try db.exec("DELETE FROM users WHERE age IS NULL")
    print("{db.changes()} deleted")
}
```

```output
Ada is 1
O'Brien is 2
1 deleted
```

With values, `exec` runs one statement: SQL with another statement after it fails with `SqlError.failed`, before anything runs. The number of values must match the number of `?`.

## Bytes

A BLOB column keeps bytes as they are, for images, hashes or packed data. `.blob(bytes)` puts a `u8[]` into one, and `rows.bytes(col)` reads it back; like `text`, what it gives is valid until the next `next`. A `String` field of a struct read with `all` or `one` gets the bytes too:

```iris run
import sqlite

@json
struct Picture {
    String name
    String data
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE pictures (name TEXT, data BLOB)")
    u8[6] head = [0x89, 'P', 'N', 'G', 0, 0xff]
    try db.exec("INSERT INTO pictures VALUES (?, ?)", [.text("cat.png"), .blob(head[:])])
    rows := try db.query("SELECT data, typeof(data) FROM pictures")
    defer rows.close()
    _ = try rows.next()
    b := rows.bytes(0)
    print("{rows.text(1)}: {b.len} bytes, the first {b[0]}, the last {b[5]}")
    scoped Picture p = try sqlite.one[Picture](&db, "SELECT * FROM pictures")
    print("{p.name}: {p.data.len} bytes")
}
```

```output
blob: 6 bytes, the first 137, the last 255
cat.png: 6 bytes
```

## Rows into structs

`sqlite.all[T]` runs a query and gives a `List` of `T`, a `@json` struct (see [JSON](28-json.html)). Each field is filled from the column of the same name, or the name given with `@json("name")`. Columns without a field are skipped, and fields without a column keep their defaults. The `String`s and the list are the caller's to free:

```iris run
import sqlite

@json
struct User {
    i64 id
    String name
    i64 age
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
    try db.exec("INSERT INTO users (name, age) VALUES ('Ada', 36), ('Zoe', 41), ('Bo', 7)")
    users := try sqlite.all[User](&db, "SELECT * FROM users WHERE age > ? ORDER BY name", [.int(18)])
    defer {
        for u in users {
            u.free()
        }
        users.free()
    }
    for u in users {
        print("{u.id} {u.name} {u.age}")
    }
}
```

```output
1 Ada 36
2 Zoe 41
```

`sqlite.one[T]` gives the first row. When the query finds nothing, it fails with `SqlError.noRows`:

```iris run
import sqlite

@json
struct User {
    i64 id
    String name
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)")
    try db.exec("INSERT INTO users (name) VALUES ('Ada'), ('Bo')")
    scoped User bo = try sqlite.one[User](&db, "SELECT * FROM users WHERE name = ?", [.text("Bo")])
    print("found {bo.name}, id {bo.id}")
    _ = sqlite.one[User](&db, "SELECT * FROM users WHERE id = ?", [.int(99)]) catch e {
        print("{e.code}")
        User{}
    }
}
```

```output
found Bo, id 2
SqlError.noRows
```

A field can be an integer, a float, a `bool` (0 is `false`), a `String`, or a plain enum, read from the text of its value's name. Text that isn't one of the names fails with `SqlError.failed`:

```iris run
import sqlite

enum Status { open, done }

@json
struct Task {
    String title
    Status status
    bool urgent
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE tasks (title TEXT, status TEXT, urgent INTEGER)")
    try db.exec("INSERT INTO tasks VALUES ('write', 'open', 1), ('read', 'done', 0)")
    tasks := try sqlite.all[Task](&db, "SELECT * FROM tasks")
    defer {
        for t in tasks {
            t.free()
        }
        tasks.free()
    }
    for t in tasks {
        print("{t.title}: {t.status}, urgent {t.urgent}")
    }
    try db.exec("UPDATE tasks SET status = 'later' WHERE title = 'read'")
    _ = sqlite.one[Task](&db, "SELECT * FROM tasks WHERE title = 'read'") catch e {
        print("{e.code}: {e.message}")
        Task{}
    }
}
```

```output
write: open, urgent true
read: done, urgent false
SqlError.failed: the column status has 'later', which isn't one of the enum's values
```

A number that doesn't fit its field fails the same way, instead of being cut down to fit:

```iris run
import sqlite

@json
struct Reading {
    u8 level
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE readings (level INTEGER)")
    try db.exec("INSERT INTO readings VALUES (300)")
    _ = sqlite.one[Reading](&db, "SELECT * FROM readings") catch e {
        print("{e.code}: {e.message}")
        Reading{}
    }
}
```

```output
SqlError.failed: the column level has 300, which doesn't fit in u8
```

When `all` or `one` fails partway, they free what they had read, so nothing is left for the caller.

## Row by row

`db.query` gives `Rows`, which are read one row at a time. `next` moves to the next row and gives `false` when there are no more. `int`, `float` and `text` read a column by its number, from 0, and `isNull` says whether it is NULL. `close` ends the query:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE t (name TEXT, score REAL); INSERT INTO t VALUES ('a', 1.5), ('b', NULL), ('c', 4)")
    rows := try db.query("SELECT name, score FROM t ORDER BY name")
    defer rows.close()
    for try rows.next() {
        if rows.isNull(1) {
            print("{rows.text(0)}: no score")
        } else {
            print("{rows.text(0)}: {rows.float(1)}")
        }
    }
}
```

```output
a: 1.5
b: no score
c: 4
```

The `str` that `text` gives points into SQLite's memory, and is valid until the next call to `next`. To keep it, copy it with `.copy()`. `sqlite.scan(&rows, &v)` fills a `@json` struct from the current row, as `sqlite.all` does for each row.

`columns` gives the number of columns and `name` a column's name, for a query whose columns aren't known in advance:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    rows := try db.query("SELECT 1 AS one, 'two' AS two, 3.5 AS three")
    defer rows.close()
    _ = try rows.next()
    for i in 0..rows.columns() {
        print("{rows.name(i)} = {rows.text(i)}")
    }
}
```

```output
one = 1
two = two
three = 3.5
```

A query that adds things up gives one row, read the same way:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE sales (amount INTEGER); INSERT INTO sales VALUES (5), (12), (30)")
    rows := try db.query("SELECT COUNT(*), SUM(amount) FROM sales WHERE amount > ?", [.int(10)])
    defer rows.close()
    _ = try rows.next()
    print("{rows.int(0)} sales, {rows.int(1)} in all")
}
```

```output
2 sales, 42 in all
```

## NULL

A NULL column reads as `0` with `int` and `float` and as `""` with `text`, so `isNull` is how to tell. With `sqlite.all` and `sqlite.one`, a NULL column leaves the field as it is, at its default:

```iris run
import sqlite

@json
struct Item {
    String name
    i64 stock = -1
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE items (name TEXT, stock INTEGER)")
    try db.exec("INSERT INTO items VALUES (?, ?), (?, ?)", [.text("pear"), .int(4), .text("fig"), .none])
    items := try sqlite.all[Item](&db, "SELECT * FROM items")
    defer items.free()
    for it in items {
        if it.stock < 0 {
            print("{it.name}: unknown")
        } else {
            print("{it.name}: {it.stock}")
        }
        it.free()
    }
}
```

```output
pear: 4
fig: unknown
```

## Transactions

`begin`, `commit` and `rollback` put several changes together, so they all happen or none does. Between `begin` and `commit`, the changes are seen only by this connection; `rollback` drops them. `errdefer` (see [Memory](19-memory.html#errdefer)) rolls back on every way out with an error, so no step can leave the transaction open:

```iris run
import sqlite

!void transfer(sqlite.Db *db, i64 from, i64 to, i64 amount) {
    try db.begin()
    errdefer _ = db.rollback()
    try db.exec("UPDATE accounts SET balance = balance - ? WHERE id = ?", [.int(amount), .int(from)])
    try db.exec("UPDATE accounts SET balance = balance + ? WHERE id = ?", [.int(amount), .int(to)])
    try db.commit()
}

!void show(sqlite.Db *db) {
    rows := try db.query("SELECT id, balance FROM accounts ORDER BY id")
    defer rows.close()
    for try rows.next() {
        print("  {rows.int(0)}: {rows.int(1)}")
    }
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec(r"""
        CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER CHECK (balance <= 100));
        INSERT INTO accounts (balance) VALUES (50), (90);
        """)
    try transfer(&db, 1, 2, 5)
    print("after 5:")
    try show(&db)
    transfer(&db, 1, 2, 20) catch e {
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
20 failed: SqlError.constraint
after 20:
  1: 45
  2: 95
```

The second transfer took 20 from account 1, then failed on account 2, whose `CHECK` allows no more than 100. The rollback put account 1 back.

## Errors

The errors are values of `sqlite.SqlError`, and the message says whether the SQL itself is wrong (`in the SQL`) or failed while it ran (`running the SQL`), then gives SQLite's reason:

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
    try db.exec("CREATE TABLE users (name TEXT UNIQUE NOT NULL)")
    try db.exec("INSERT INTO users VALUES ('Ada')")
    db.exec("INSERT INTO users VALUES (?)", [.text("Ada")]) catch e {
        print("{e.code}: {e.message}")
    }
    db.exec("INSERT INTO users VALUES (?)", [.none]) catch e {
        print("{e.code}: {e.message}")
    }
    db.exec("SELEKT 1") catch e {
        print("{e.code}: {e.message}")
    }
    db.exec("INSERT INTO users VALUES (?)", [.text("a"), .text("b")]) catch e {
        print("{e.code}: {e.message}")
    }
}
```

```output
SqlError.constraint: running the SQL: UNIQUE constraint failed: users.name
SqlError.constraint: running the SQL: NOT NULL constraint failed: users.name
SqlError.failed: in the SQL: near "SELEKT": syntax error
SqlError.failed: the SQL has 1 ? but 2 values were given
```

A `Db` can be shared by threads; each call waits for the database on the thread that makes it. `IRIS_SQLITE` names another copy of SQLite to load instead of the system's.

The [REST API guide](../guides/01-rest-api.html) builds a whole service that keeps its data in SQLite. For a database server, the library also has `pg` for PostgreSQL and `redis` for Redis (see the [library](../std/index.html)).
