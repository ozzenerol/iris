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

`mysql.insert` adds a `@json` struct as a row, leaving out the fields named in `skip`, and gives the `AUTO_INCREMENT` id it got. `mysql.insertMany` adds a list of them, many to a statement:

```iris
u.id = try mysql.insert(&db, "users", &u, skip: ["id"])
n := try mysql.insertMany(&db, "users", more.items(), skip: ["id"])
```

`begin` starts a transaction, which `commit` or `rollback` ends.
