## Connecting

`pg.pool` gives a `Pool` for a server's address, with at most a given number of connections open (10 unless given). It connects once to check the address and the login, then opens connections as tasks need them. Every task and thread of a program can share one pool:

```iris
import pg

!void main() {
    db := try pg.pool("postgres://ada:secret@localhost:5432/shop", 20)
    defer db.close()
}
```

The address is `postgres://user:password@host:port/database`. `?sslmode=require` makes the connection use TLS, and `?sslmode=verify-full&ca=root.pem` also checks the server's certificate. `pg.connect` gives a single connection, for a program that does one thing at a time.

## Changing and querying

`exec` runs a statement and gives the number of rows it changed. Values are sent apart from the SQL, as `$1`, `$2` and so on, so they can't change what the SQL means. `execScript` runs SQL without values, which may hold several statements, as a schema does:

```iris
try db.execScript("""
    CREATE TABLE IF NOT EXISTS users (
        id serial PRIMARY KEY,
        name text UNIQUE NOT NULL,
        age int
    );
    """)

n := try db.exec("INSERT INTO users (name, age) VALUES ($1, $2)", [.text("Ada"), .int(36)])

_ = db.exec("INSERT INTO users (name) VALUES ($1)", [.text("Ada")]) catch e {
    print("{e.code}") // PgError.constraint: the name is taken
    0
}
```

The values are `.int`, `.float`, `.text`, `.bool`, and `.none` for NULL.

## Rows into structs

`pg.all` runs a query and reads every row into a `@json` struct, each field from the column of the same name. `pg.one` reads the first row, or fails with `PgError.noRows`. Both take a `Pool`, a `Conn` or a `Tx`:

```iris
@json
struct User {
    i64 id
    String name
    i64 age
}

!void printAdults(pg.Pool *db) {
    users := try pg.all[User](&db, "SELECT * FROM users WHERE age >= $1 ORDER BY name", [.int(18)])
    defer {
        for u in users {
            u.free()
        }
        users.free()
    }

    for u in users {
        print("{u.name} {u.age}")
    }
}
```

A struct marked `@entity("table")`, with `@id` on its key, is a row: `pg.insert` adds it and reads the key PostgreSQL makes back into it, `pg.find` reads one by its key, `pg.update` writes one back and `pg.delete` removes one. `pg.insertMany` adds a list, many to a statement:

```iris
@entity("users")
struct Account {
    @id i64 id
    String name
}

try pg.insert(&db, &a)
scoped b := try pg.find[Account](&db, .int(a.id))
_ = try pg.update(&db, &b)
_ = try pg.delete[Account](&db, .int(a.id))
```

`table` puts a row in another table, `skip` leaves out columns the database fills, and `returning` reads columns back: `pg.insert(&db, &a, skip: ["created"], returning: ["created"])`. A plain `@json` struct is only read, with `all` and `one`.

`query` gives `Rows`, which can also be read one row at a time. `next` moves to the next row, and `int`, `float`, `bool`, `text` and `isNull` read a column by its number, from 0. `close` ends them.

## Transactions

`begin` takes a connection from the pool for a transaction. Its `query` and `exec` run in the transaction. `commit` or `rollback` ends it and gives the connection back. With `scoped`, a transaction left without `commit`, by an error or a `return`, rolls back:

```iris
!void transfer(pg.Pool *db, i64 from, i64 to, i64 amount) {
    scoped tx := try db.begin()

    _ = try tx.exec("UPDATE accounts SET balance = balance - $1 WHERE id = $2", [.int(amount), .int(from)])
    _ = try tx.exec("UPDATE accounts SET balance = balance + $1 WHERE id = $2", [.int(amount), .int(to)])

    try tx.commit()
}
```

A transaction fails with `PgError.retry` when it clashes with another one, through a serialization failure or a deadlock. Run it again in that case.

## Errors

A failed statement gives `PgError.failed`, or `PgError.constraint` for a broken UNIQUE, NOT NULL, foreign key or CHECK constraint. The message holds the server's text and its SQLSTATE code in brackets, such as `(23505)`.
