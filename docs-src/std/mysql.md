## Connecting

`mysql.pool` gives a `Pool` for a server's address, with at most a given number of connections open (10 unless given). Every task and thread of a program can share one pool. `mysql.connect` gives a single connection:

```iris
import mysql

!void main() {
    db := try mysql.pool("mysql://ada:secret@localhost:3306/shop?sslmode=require", 20)
    defer db.close()
}
```

The address is `mysql://user:password@host:port/database`. `?sslmode=require` uses TLS and `?sslmode=verify-full&ca=root.pem` also checks the server's certificate. Without TLS, a `caching_sha2_password` login that needs the whole password sends it encrypted with the server's key only when the address has `?allowPublicKeyRetrieval=true`.

## Queries

`exec` runs a statement and gives the number of rows it changed. Values go apart from the SQL as `?`, and a statement with values is prepared once on each connection. `query` gives `Rows`, with `affected` and `lastId`. `mysql.all[T]` and `mysql.one[T]` read rows into `@json` structs:

```iris
@json
struct User {
    i64 id
    String name
}

n := try db.exec("UPDATE users SET seen = NOW() WHERE id = ?", [.int(7)])
users := try mysql.all[User](&db, "SELECT id, name FROM users WHERE age > ?", [.int(30)])
```

A struct marked `@entity("table")`, with `@id` on its key, is a row: `mysql.insert` adds it and gives the `AUTO_INCREMENT` id, which it also puts in an `@id` of 0. `mysql.find`, `mysql.update` and `mysql.delete` read, write and remove one by its key, and `mysql.insertMany` adds a list, many to a statement:

```iris
_ = try mysql.insert(&db, &account)
scoped a := try mysql.find[Account](&db, .int(7))
n := try mysql.insertMany(&db, more.items())
```

`table` puts a row in another table, and `skip` leaves out columns the database fills: `mysql.insert(&db, &a, skip: ["created"])`. A plain `@json` struct is only read, with `all` and `one`.

`begin` starts a transaction, which `commit` or `rollback` ends.
