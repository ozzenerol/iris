# Rows as structs

A struct marked `@entity` is a row of a database table. The `sqlite`, `pg` and `mysql` modules then add it, read it by its key, write it back and remove it, with the SQL made from the struct. This chapter uses SQLite, whose examples run without a server. The [PostgreSQL](18-postgres-and-redis.html) and [MySQL](29-mysql.html) modules have the same calls.

## @entity and @id

`@entity("table")` names the table. `@id` marks the field that holds the primary key, and every `@entity` struct has one. Each field is a column of the same name.

- `insert(&db, &row)` adds the row. An `@id` of 0, or "" for text, is left out, and the key the database makes goes into it.
- `find[T](&db, id)` reads the row whose key is `id`, and fails with `noRows` when there is none.
- `update(&db, &row)` writes every field to the row with the struct's key, and gives how many rows changed.
- `delete[T](&db, id)` removes the row, and gives how many went.

```iris run
import sqlite

@entity("users")
struct User {
    @id i64 id
    String name
    i64 age
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER)")

    scoped ada := User{name: "Ada".copy(), age: 36}
    _ = try sqlite.insert(&db, &ada)
    print("Ada is {ada.id}")

    scoped u := try sqlite.find[User](&db, .int(ada.id))
    u.age += 1
    print("updated {try sqlite.update(&db, &u)}")

    scoped again := try sqlite.find[User](&db, .int(ada.id))
    print("{again.name} is {again.age}")

    print("deleted {try sqlite.delete[User](&db, .int(ada.id))}")
    _ = sqlite.find[User](&db, .int(ada.id)) catch e {
        print("{e.code}")
        User{}
    }
}
```

```output
Ada is 1
updated 1
Ada is 37
deleted 1
SqliteError.noRows
```

`all` and `one` read `@entity` structs from any query too, and `insertMany` adds a list of them, many to a statement.

## Column names

`@column("name")` gives a field another column. Columns are quoted in the SQL, so a name such as `order` works.

```iris run
import sqlite

@entity("orders")
struct Order {
    @id i64 id
    @column("customer_id") i64 customer
    @column("order") i64 position
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER, \"order\" INTEGER)")

    List[Order] orders
    defer orders.free()
    for i in 1..4 {
        orders.push(Order{customer: 7, position: i})
    }
    print("added {try sqlite.insertMany(&db, orders.items())}")

    rows := try sqlite.all[Order](&db, "SELECT * FROM orders WHERE customer_id = ?", [.int(7)])
    defer rows.free()
    for o in rows {
        print("{o.id}: customer {o.customer}, place {o.position}")
    }
}
```

```output
added 3
1: customer 7, place 1
2: customer 7, place 2
3: customer 7, place 3
```

## JSON and rows together

An `@entity` struct is also a `@json` struct. `@json("key")` names a field in JSON and `@column("name")` names it in the table, so one struct can be read from a request, kept in the database and sent back. A list, a map or a `@json` struct in a field is kept as JSON text in its column.

```iris run
import json, sqlite

@json
@entity("people")
struct Person {
    @id i64 id
    @json("full_name") String name
    @column("created_at") String created
    List[String] tags
}

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    try db.exec("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, created_at TEXT, tags TEXT)")

    scoped p := try json.decode(Person, r"""{"full_name": "Ada", "created": "2026-10-27", "tags": ["admin"]}""")
    _ = try sqlite.insert(&db, &p)

    scoped back := try sqlite.find[Person](&db, .int(p.id))
    scoped String out = json.encode(back)
    print(out)
}
```

```output
{"id":1,"full_name":"Ada","created":"2026-10-27","tags":["admin"]}
```

## Structs without @entity

A plain `@json` struct is only read from rows, with `all` and `one`. Writing one, with `insert`, `insertMany`, `find`, `update` or `delete`, is an error when the program is built: only `@entity` says which table and key a struct is.

`insert` takes three more arguments. `table` puts the row in another table, `skip` leaves out columns the database fills, and on PostgreSQL and SQLite `returning` reads columns back into the struct:

```iris
pg.insert(&db, &tea, skip: ["created"], returning: ["created"])
```

## Each database

- PostgreSQL reads a made key back with `RETURNING`, so it works with `serial`, `identity` and defaults such as `gen_random_uuid()`.
- MySQL and SQLite put the `AUTO_INCREMENT` id, or the rowid, into an `@id` that is a whole number. `insert` gives that id too.
- A statement takes at most 65535 values on PostgreSQL and MySQL, and 32766 on SQLite, so `insertMany` sends a long list in several. In a transaction they all take effect together.
