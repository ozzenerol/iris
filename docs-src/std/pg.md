## Connecting

`pg.pool` gives a `Pool` for a server's address, with at most a number of connections open (10 unless one is given). It connects once to check the address and the login, and then opens connections as tasks need them. A pool can be shared by every task and thread of a program:

```iris
import pg

!void main() {
    db := try pg.pool("postgres://ada:secret@localhost:5432/shop", 20)
    defer db.close()
}
```

The address is `postgres://user:password@host:port/database`. `?sslmode=require` makes the connection use TLS, and `?sslmode=verify-full&ca=root.pem` checks the server's certificate too. `pg.connect` gives one connection, for a program that does one thing at a time.

## Changing and querying

`exec` runs a statement and gives how many rows it changed. Values go apart from the SQL, as `$1`, `$2` and on, so they can't change what the SQL means. `execScript` runs SQL with no values, which may hold several statements, as a schema does:

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

The values are `.int`, `.float`, `.text`, `.bool` and `.none` for NULL.

## Rows into structs

`pg.all` reads every row of a query into `@json` structs, each field from the column of the same name, and `pg.one` reads the first row or fails with `PgError.noRows`:

```iris
@json
struct User {
    i64 id
    String name
    i64 age
}

!void printAdults(pg.Pool *db) {
    users := try pg.all[User](try db.query("SELECT * FROM users WHERE age >= $1 ORDER BY name", [.int(18)]))
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

`query` gives `Rows`, which can also be read one row at a time with `next` and `int`, `float`, `bool`, `text` and `isNull`, by column number from 0.

## Transactions

`begin` takes a connection from the pool for a transaction. Its `query` and `exec` run in the transaction, and `commit` or `rollback` ends it and gives the connection back. With `scoped`, a transaction that is left without `commit`, by an error or a `return`, rolls back:

```iris
!void transfer(pg.Pool *db, i64 from, i64 to, i64 amount) {
    scoped tx := try db.begin()
    _ = try tx.exec("UPDATE accounts SET balance = balance - $1 WHERE id = $2", [.int(amount), .int(from)])
    _ = try tx.exec("UPDATE accounts SET balance = balance + $1 WHERE id = $2", [.int(amount), .int(to)])
    try tx.commit()
}
```

A transaction can fail with `PgError.retry` when it clashes with another one, a serialization failure or a deadlock; running it again is then the right thing to do.

## Errors

A failed statement gives `PgError.failed`, or `constraint` for a broken UNIQUE, NOT NULL, foreign key or CHECK constraint. The message has the server's text and its SQLSTATE code in brackets, as `(23505)`.
