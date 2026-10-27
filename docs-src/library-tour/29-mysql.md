# MySQL

The `mysql` module talks to MySQL and MariaDB over the network, with nothing else to install. It has the same shape as the `pg` module of the [PostgreSQL and Redis](18-postgres-and-redis.html) chapter: a pool that every task and thread can share, `exec` and `query`, rows read into `@json` structs with `mysql.all` and `mysql.one`, and transactions.

The examples in this chapter need a server, so they read its address from an environment variable. A MySQL server for trying them runs with `docker run -p 3306:3306 -e MYSQL_ROOT_PASSWORD=secret -e MYSQL_DATABASE=shop mysql:8.4`.

## Connecting

`mysql.pool(address)` gives a `mysql.Pool`. It connects once to check the address and the login, then opens more connections as tasks need them, up to the number given after the address, or 10. `close` closes them all:

```iris
import mysql, os

!void main() {
    address := os.env("MYSQL_URL") catch "mysql://root:secret@localhost:3306/shop?allowPublicKeyRetrieval=true"
    db := try mysql.pool(address, 20)
    defer db.close()

    rows := try db.query("SELECT VERSION()")
    defer rows.close()

    _ = rows.next()
    print(rows.text(0))
}
```

```output
8.4.11
```

- The address is `mysql://user:password@host:port/database`.
- `?sslmode=require` makes the connection use TLS, and `?sslmode=verify-full&ca=root.pem` also checks the server's certificate.
- MySQL's own login, `caching_sha2_password`, sends the whole password the first time a user logs in after the server starts. Over TLS that is safe. Without TLS the password goes encrypted with the server's RSA key, and only when the address has `?allowPublicKeyRetrieval=true`, since a server in the middle could hand over a key of its own.
- `mysql_native_password`, the login of MariaDB and of older servers, works too.
- A refused login fails with `MysqlError.auth`, and a server that can't be reached with `MysqlError.connect`.

## Changing data

`exec` runs one statement and gives how many rows it changed. The values go apart from the SQL, as `?`, and are `.int`, `.float`, `.text`, `.bool`, or `.none` for NULL. A statement with values is prepared the first time a connection runs it, and only the values go after that. `execScript` runs SQL with no values that may hold several statements, such as a schema:

```iris
import mysql, os

!void main() {
    db := try mysql.pool(os.env("MYSQL_URL") catch "mysql://root:secret@localhost/shop")
    defer db.close()

    try db.execScript(r"""
        DROP TABLE IF EXISTS products;
        CREATE TABLE products (
            id int AUTO_INCREMENT PRIMARY KEY,
            name varchar(100) UNIQUE NOT NULL,
            price decimal(10, 2) NOT NULL,
            stock int NOT NULL DEFAULT 0
        );
        """)

    for (name, price) in [("green tea", 3.5), ("lemon cake", 4.25), ("coffee", 2.8)] {
        _ = try db.exec("INSERT INTO products (name, price, stock) VALUES (?, ?, ?)", [.text(name), .float(price), .int(10)])
    }

    n := try db.exec("UPDATE products SET stock = stock - 1 WHERE price < ?", [.float(4.0)])
    print("{n} rows changed")

    added := try db.query("INSERT INTO products (name, price) VALUES (?, ?)", [.text("scone"), .float(3.1)])
    defer added.close()
    print("scone is {added.lastId}")

    _ = db.exec("INSERT INTO products (name, price) VALUES (?, ?)", [.text("coffee"), .float(3.0)]) catch e {
        print("{e.code}: {e.message}")
        0
    }
}
```

```output
2 rows changed
scone is 4
MysqlError.constraint: Duplicate entry 'coffee' for key 'products.name' (1062 23000)
```

`query` gives `mysql.Rows`, whose `affected` says how many rows changed and whose `lastId` is the `AUTO_INCREMENT` id the last `INSERT` made. A statement that the server refuses fails with `MysqlError.failed`, or with `MysqlError.constraint` for a duplicate key, a foreign key, a `NOT NULL` or a `CHECK` that isn't met. The message has the server's text, then its error number and SQLSTATE in brackets.

## Reading rows

`mysql.all[T](&db, sql, values)` runs a query and reads every row into a `@json` struct, each field from the column of the same name. `mysql.one[T]` reads the first row, or fails with `MysqlError.noRows`. Both take a `Pool`, a `Conn` or a transaction:

```iris
import mysql, os

@json
struct Product {
    i64 id
    String name
    f64 price
    i64 stock
}

!void main() {
    db := try mysql.pool(os.env("MYSQL_URL") catch "mysql://root:secret@localhost/shop")
    defer db.close()

    products := try mysql.all[Product](&db, "SELECT * FROM products WHERE stock > ? ORDER BY price", [.int(0)])
    defer {
        for p in products {
            p.free()
        }
        products.free()
    }

    for p in products {
        print("{p.id} {p.name}: {p.price}, {p.stock} left")
    }

    _ = mysql.one[Product](&db, "SELECT * FROM products WHERE id = ?", [.int(99)]) catch e {
        print("{e.code}")
        Product{}
    }
}
```

```output
3 coffee: 2.8, 9 left
1 green tea: 3.5, 9 left
2 lemon cake: 4.25, 10 left
MysqlError.noRows
```

`Rows` can also be read one row at a time, as in the `pg` module: `next` moves to the next row, and `int`, `float`, `bool`, `text` and `isNull` read a column by its number, from 0. Dates come as `2026-10-17` and times as `2026-10-17 09:30:00`, and a `bool` column, which MySQL keeps as a number, reads as `true` for anything but 0.

## Adding structs as rows

A struct marked `@entity("table")` is a row of that table, with its primary key marked `@id`. `mysql.insert(&db, &row)` adds it and gives the `AUTO_INCREMENT` id, which it also puts in an `@id` of 0. `mysql.find[T](&db, id)` reads a row by its key, `mysql.update` writes it back and `mysql.delete[T](&db, id)` removes it. The [Rows as structs](35-rows-as-structs.html) chapter covers `@entity` in full.

```iris
import mysql, os

@entity("teas")
struct Tea {
    @id i64 id
    String name
    f64 price
    i64 stock
}

!void main() {
    db := try mysql.pool(os.env("MYSQL_URL") catch "mysql://root:secret@localhost/shop")
    defer db.close()
    try db.execScript(r"""
        DROP TABLE IF EXISTS teas;
        CREATE TABLE teas (id int AUTO_INCREMENT PRIMARY KEY, name varchar(100) NOT NULL, price double, stock int);
        """)

    scoped green := Tea{name: "green tea".copy(), price: 3.5, stock: 10}
    _ = try mysql.insert(&db, &green)
    print("green tea is {green.id}")

    List[Tea] more
    defer {
        for t in more {
            t.free()
        }
        more.free()
    }
    more.push(Tea{name: "black tea".copy(), price: 2.9, stock: 4})
    more.push(Tea{name: "white tea".copy(), price: 6.2, stock: 2})
    print("added {try mysql.insertMany(&db, more.items())}")

    print("removed {try mysql.delete[Tea](&db, .int(green.id))}")
}
```

```output
green tea is 1
added 2
removed 1
```

A plain `@json` struct works with `insert` too, given the table: `mysql.insert(&db, &row, table: "teas", skip: ["id"])`. A list, a map or a `@json` struct in a field goes in as JSON, for a `json` column. A statement takes at most 65535 values, so `insertMany` sends a long list in several. In a transaction they all take effect together.

## Transactions

`db.begin()` takes a connection from the pool for a transaction. Its `query` and `exec` run inside it, and `commit` or `rollback` ends it and gives the connection back. With `scoped`, a transaction left without `commit` rolls back:

```iris
import mysql, os

// Takes count of a product from the stock, and says whether there were enough.
!bool order(mysql.Pool *db, str product, i64 count) {
    scoped tx := try db.begin()

    rows := try tx.query("SELECT stock FROM products WHERE name = ? FOR UPDATE", [.text(product)])
    defer rows.close()
    if !rows.next() || rows.int(0) < count {
        return false
    }

    _ = try tx.exec("UPDATE products SET stock = stock - ? WHERE name = ?", [.int(count), .text(product)])
    try tx.commit()
    return true
}

!void main() {
    db := try mysql.pool(os.env("MYSQL_URL") catch "mysql://root:secret@localhost/shop")
    defer db.close()

    print("4 coffees: {try order(&db, "coffee", 4)}")
    print("50 coffees: {try order(&db, "coffee", 50)}")
}
```

```output
4 coffees: true
50 coffees: false
```

A deadlock or a lock wait that took too long fails with `MysqlError.retry`. Run the transaction again.
