## Opening and changing

`sqlite.open` opens a database file and makes it when missing. `":memory:"` is a database that lives only while the program runs. `exec` runs SQL that gives no rows. Each `?` takes the next value from the list after the SQL, so values never become part of the SQL text:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()

    try db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, age INTEGER)")
    try db.exec("INSERT INTO users (name, age) VALUES (?, ?)", [.text("Ada"), .int(36)])
    print("id {db.lastId()}")
    try db.exec("INSERT INTO users (name, age) VALUES (?, ?)", [.text("Zoe"), .none])

    db.exec("INSERT INTO users (name) VALUES (?)", [.text("Ada")]) catch e {
        print("{e.code}")
    }

    try db.exec("UPDATE users SET age = age + 1 WHERE age IS NOT NULL")
    print("{db.changes()} changed")
}
```

```output
id 1
SqliteError.constraint
1 changed
```

The values are `.int`, `.float`, `.text`, and `.none` for NULL. Without values, the SQL given to `exec` can hold several statements, which suits making the tables.

## Rows into structs

`sqlite.all` runs a query and gives a `List` of `@json` structs, each field filled from the column of the same name. `sqlite.one` gives the first row, or fails with `SqliteError.noRows`:

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
    for u in users {
        print("{u.id} {u.name} {u.age}")
        u.free()
    }
    users.free()

    scoped User bo = try sqlite.one[User](&db, "SELECT * FROM users WHERE name = ?", [.text("Bo")])
    print("one: {bo.name}")
}
```

```output
1 Ada 36
2 Zoe 41
one: Bo
```

## Row by row

`query` gives `Rows`, read one row at a time with `next`. `int`, `float` and `text` read a column by its number, from 0, and `isNull` says whether it is NULL. A column's text stays valid until the next call to `next`:

```iris run
import sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE t (name TEXT, score REAL); INSERT INTO t VALUES ('a', 1.5), ('b', NULL)")

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
```

## Transactions

`begin`, `commit` and `rollback` group several changes, so either all of them happen or none does:

```iris
try db.begin()

db.exec("UPDATE accounts SET balance = balance - 10 WHERE id = ?", [.int(from)]) catch e {
    try db.rollback()
    return e
}
try db.exec("UPDATE accounts SET balance = balance + 10 WHERE id = ?", [.int(to)])

try db.commit()
```

SQLite is loaded the first time a database is opened. `IRIS_SQLITE` names another copy of it.

## @entity structs

A struct marked `@entity("table")`, with `@id` on its key, is a row: `sqlite.insert` adds it and puts the rowid in an `@id` of 0, `sqlite.find` reads one by its key, `sqlite.update` writes one back and `sqlite.delete` removes one. `sqlite.insertMany` adds a list, many to a statement:

```iris
_ = try sqlite.insert(&db, &account)
scoped a := try sqlite.find[Account](&db, .int(1))
_ = try sqlite.delete[Account](&db, .int(1))
```

`find` reads only the struct's columns, so the table can have more. `table` puts a row in another table, `skip` leaves out columns the database fills, and `returning` reads columns back: `sqlite.insert(&db, &a, skip: ["created"], returning: ["created"])`. A plain `@json` struct is only read, with `all` and `one`.
