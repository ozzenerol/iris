# PostgreSQL and Redis

A service that runs on more than one machine keeps its data on a database server. The `pg` module talks to PostgreSQL and the `redis` module to Redis or Valkey, both over the network with nothing else to install. Each keeps a pool of connections that every task and thread of a program can share, and the `pool` module gives the same for any other kind of connection.

The examples in this chapter need a server, so they read its address from an environment variable. They follow the [SQLite](17-sqlite.html) chapter, and what it says about values in SQL, rows into structs and transactions holds here too.

## Connecting to PostgreSQL

`pg.pool(address)` gives a `pg.Pool`. It connects once to check the address and the login, then opens more connections as tasks need them, up to the number given after the address, or 10. `close` closes them all:

```iris
import os, pg

!void main() {
    address := os.env("DATABASE_URL") catch "postgres://app:secret@localhost:5432/shop"
    db := try pg.pool(address, 20)
    defer db.close()

    rows := try db.query("SELECT version()")
    defer rows.close()

    _ = rows.next()
    print(rows.text(0)[0:13])
}
```

```output
PostgreSQL 17
```

- The address is `postgres://user:password@host:port/database`.
- `?sslmode=require` makes the connection use TLS, and `?sslmode=verify-full&ca=root.pem` also checks the server's certificate.
- A refused login fails with `PgError.auth`, and a server that can't be reached with `PgError.connect`.

## Changing data

`exec` runs one statement and gives how many rows it changed. The values go apart from the SQL, as `$1`, `$2` and so on, and are `.int`, `.float`, `.text`, `.bool`, or `.none` for NULL. `execScript` runs SQL with no values that may hold several statements, such as a schema:

```iris
import os, pg

!void main() {
    db := try pg.pool(os.env("DATABASE_URL") catch "postgres://localhost/shop")
    defer db.close()

    try db.execScript(r"""
        DROP TABLE IF EXISTS products;
        CREATE TABLE products (
            id serial PRIMARY KEY,
            name text UNIQUE NOT NULL,
            price numeric(10, 2) NOT NULL,
            stock int NOT NULL DEFAULT 0
        );
        """)

    for (name, price) in [("green tea", 3.5), ("lemon cake", 4.25), ("coffee", 2.8)] {
        _ = try db.exec("INSERT INTO products (name, price, stock) VALUES ($1, $2, $3)", [.text(name), .float(price), .int(10)])
    }

    n := try db.exec("UPDATE products SET stock = stock - 1 WHERE price < $1", [.float(4.0)])
    print("{n} rows changed")

    _ = db.exec("INSERT INTO products (name, price) VALUES ($1, $2)", [.text("coffee"), .float(3.0)]) catch e {
        print("{e.code}: {e.message}")
        0
    }
}
```

```output
2 rows changed
PgError.constraint: duplicate key value violates unique constraint "products_name_key": Key (name)=(coffee) already exists. (23505)
```

A statement that the server refuses fails with `PgError.failed`, or with `PgError.constraint` for a `UNIQUE`, `NOT NULL`, foreign key or `CHECK` that isn't met. The message has the server's text, with its SQLSTATE code in brackets.

## Reading rows

`pg.all[T](&db, sql, values)` runs a query and reads every row into a `@json` struct, each field from the column of the same name. `pg.one[T]` reads the first row, or fails with `PgError.noRows`. Both take a `Pool`, a `Conn` or a transaction, as `sqlite.all` and `sqlite.one` take a `Db`:

```iris
import os, pg

@json
struct Product {
    i64 id
    String name
    f64 price
    i64 stock
}

!void main() {
    db := try pg.pool(os.env("DATABASE_URL") catch "postgres://localhost/shop")
    defer db.close()

    products := try pg.all[Product](&db, "SELECT * FROM products WHERE stock > $1 ORDER BY price", [.int(0)])
    defer {
        for p in products {
            p.free()
        }
        products.free()
    }

    for p in products {
        print("{p.id} {p.name}: {p.price}, {p.stock} left")
    }

    scoped Product cake = try pg.one[Product](&db, "SELECT * FROM products WHERE name = $1", [.text("lemon cake")])
    print("cake is {cake.id}")

    _ = pg.one[Product](&db, "SELECT * FROM products WHERE id = $1", [.int(99)]) catch e {
        print("{e.code}")
        Product{}
    }
}
```

```output
3 coffee: 2.8, 9 left
1 green tea: 3.5, 9 left
2 lemon cake: 4.25, 10 left
cake is 2
PgError.noRows
```

`query` gives `pg.Rows`, which can also be read one row at a time.

- `next` moves to the next row.
- `int`, `float`, `bool`, `text` and `isNull` read a column by its number, from 0.
- The whole result has come back from the server by the time `query` returns, so `len` is the number of rows.
- `close` gives their memory back.

```iris
import os, pg

!void main() {
    db := try pg.pool(os.env("DATABASE_URL") catch "postgres://localhost/shop")
    defer db.close()

    rows := try db.query("SELECT count(*), sum(stock), avg(price)::float8 FROM products")
    defer rows.close()

    _ = rows.next()
    print("{rows.int(0)} products, {rows.int(1)} in stock, {rows.float(2):.2} on average")
}
```

```output
3 products, 28 in stock, 3.52 on average
```

## Adding structs as rows

A struct marked `@entity("table")` is a row of that table, with its primary key marked `@id`. `pg.insert(&db, &row)` adds it, `pg.find[T](&db, id)` reads it by its key, `pg.update` writes it back and `pg.delete[T](&db, id)` removes it. An `@id` of 0 is left out of the insert, and the key PostgreSQL makes is read back into it. The [Rows as structs](35-rows-as-structs.html) chapter covers `@entity` in full.

```iris
import os, pg

@entity("teas")
struct Tea {
    @id i64 id
    String name
    f64 price
    i64 stock
    List[String] tags
}

!void main() {
    db := try pg.pool(os.env("DATABASE_URL") catch "postgres://localhost/shop")
    defer db.close()
    try db.execScript(r"""
        DROP TABLE IF EXISTS teas;
        CREATE TABLE teas (id serial PRIMARY KEY, name text NOT NULL, price float8, stock int, tags jsonb);
        """)

    scoped green := Tea{name: "green tea".copy(), price: 3.5, stock: 10}
    green.tags.push("loose".copy())
    try pg.insert(&db, &green)
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
    print("added {try pg.insertMany(&db, more.items())}")

    scoped back := try pg.find[Tea](&db, .int(green.id))
    back.stock -= 1
    _ = try pg.update(&db, &back)
    print("{back.name}: {back.tags.at(0)}, {back.stock} left")
}
```

```output
green tea is 1
added 2
green tea: loose, 9 left
```

- A list, a map or a `@json` struct in a field goes in as JSON, for a `json` or `jsonb` column, and is read back from it.
- `skip` leaves out columns the database fills, and `returning` reads columns back: `pg.insert(&db, &tea, skip: ["created"], returning: ["created"])`.
- A statement takes at most 65535 values, so `insertMany` sends a long list in several. In a transaction they all take effect together.

## Transactions

`db.begin()` takes a connection from the pool for a transaction. Its `query` and `exec` run inside the transaction, and `commit` or `rollback` ends it and gives the connection back. With `scoped`, a transaction left without `commit`, by an error or a `return`, rolls back:

```iris
import os, pg

// Takes count of a product from the stock, and says whether there were enough.
!bool order(pg.Pool *db, str product, i64 count) {
    scoped tx := try db.begin()

    rows := try tx.query("SELECT stock FROM products WHERE name = $1 FOR UPDATE", [.text(product)])
    defer rows.close()
    if !rows.next() || rows.int(0) < count {
        return false
    }

    _ = try tx.exec("UPDATE products SET stock = stock - $1 WHERE name = $2", [.int(count), .text(product)])
    try tx.commit()
    return true
}

!void main() {
    db := try pg.pool(os.env("DATABASE_URL") catch "postgres://localhost/shop")
    defer db.close()

    print("4 coffees: {try order(&db, "coffee", 4)}")
    print("50 coffees: {try order(&db, "coffee", 50)}")

    rows := try db.query("SELECT stock FROM products WHERE name = 'coffee'")
    defer rows.close()
    _ = rows.next()
    print("coffee left: {rows.int(0)}")
}
```

```output
4 coffees: true
50 coffees: false
coffee left: 5
```

The second order returned before `commit`, so `scoped` rolled it back. `FOR UPDATE` locks the row until the transaction ends, so two orders at once can't both take the last items. A transaction that clashes with another, by a deadlock or a serialization failure, fails with `PgError.retry`. Run it again.

## Connecting to Redis

Redis keeps keys and values in memory, for caches, sessions, counters and queues. `redis.connect(address)` gives a `redis.Client` with its own pool of connections, which every task and thread can share. `set` stores a value, with a time to live when `ttl` is given. `get` gives a value and whether the key was there:

```iris
import os, redis

!void main() {
    db := try redis.connect(os.env("REDIS_URL") catch "redis://localhost:6379")
    defer db.close()

    try db.set("session:9f2c", "ada", ttl: 30m)
    user, found := try db.get("session:9f2c")
    defer user.free()
    print("found {found}: {user}")

    left := try db.ttl("session:9f2c")
    print("ends within 30m: {left <= 30m}")

    _, there := try db.get("session:none")
    print("found {there}")

    print("{try db.del(["session:9f2c"])} deleted")
}
```

```output
found true: ada
ends within 30m: true
found false
1 deleted
```

The address is `redis://user:password@host:port/database`, and `rediss://` uses TLS.

## Counters, hashes and lists

`incr` adds one to a number and gives the new value in one step on the server, so tasks on many machines can count together. A hash keeps the fields of one thing under one key. A list keeps values in order, which makes a simple queue:

```iris
import os, redis

!void main() {
    db := try redis.connect(os.env("REDIS_URL") catch "redis://localhost:6379")
    defer db.close()
    _ = try db.del(["hits:/", "user:7", "jobs"])

    for _ in 0..3 {
        _ = try db.incr("hits:/")
    }
    print("hits: {try db.incrBy("hits:/", 10)}")

    try db.hset("user:7", "name", "Ada")
    try db.hset("user:7", "plan", "pro")
    plan, _ := try db.hget("user:7", "plan")
    defer plan.free()
    print("plan {plan}")

    _ = try db.rpush("jobs", "resize 1.png")
    _ = try db.rpush("jobs", "resize 2.png")
    scoped List[String] jobs = try db.lrange("jobs")
    for j in jobs {
        print("job: {j}")
        j.free()
    }
}
```

```output
hits: 13
plan pro
job: resize 1.png
job: resize 2.png
```

## Any command

`do` sends any command, its words as a list, and gives the server's `redis.Reply`. A reply holds `text` for strings, `int` for numbers and `items` for lists, and `kind` says which. An error from the server fails with `RedisError.server` and its message:

```iris
import os, redis

!void main() {
    db := try redis.connect(os.env("REDIS_URL") catch "redis://localhost:6379")
    defer db.close()
    _ = try db.del(["scores"])

    for (name, score) in [("ada", "310"), ("linus", "120"), ("grace", "450")] {
        scoped redis.Reply r = try db.do(["ZADD", "scores", score, name])
    }

    scoped redis.Reply top = try db.do(["ZREVRANGE", "scores", "0", "1", "WITHSCORES"])
    for i in 0..top.items.len / 2 {
        print("{top.items.at(i * 2).text}: {top.items.at(i * 2 + 1).text}")
    }

    _ = db.do(["INCR", "scores"]) catch e {
        print("{e.code}: {e.message}")
        redis.Reply{}
    }
}
```

```output
grace: 450
ada: 310
RedisError.server: WRONGTYPE Operation against a key holding the wrong kind of value
```

A `Pipeline` sends several commands at once and waits once for all the replies, which saves a round trip for each (see the [library](../std/index.html#redis)).

## Publish and subscribe

`publish` sends a message on a channel to everyone listening. `redis.subscribe(address, channels)` opens a connection of its own for listening, and `next` waits for the next message. Here a task listens while `main` publishes:

```iris
import os, redis

void listen(str address, chan[bool] ready, chan[bool] done) {
    defer done <- true

    scoped sub := redis.subscribe(address, ["deploys"]) catch e {
        print("subscribe: {e.message}")
        return
    }
    ready <- true

    for _ in 0..2 {
        scoped m := sub.next() catch e {
            return
        }
        print("{m.channel}: {m.data}")
    }
}

!void main() {
    address := os.env("REDIS_URL") catch "redis://localhost:6379"
    db := try redis.connect(address)
    defer db.close()

    scoped chan[bool] ready = chan[bool](1)
    scoped chan[bool] done = chan[bool](1)
    spawn listen(address, ready, done)
    _ = <-ready

    _ = try db.publish("deploys", "api 1.4.2")
    _ = try db.publish("deploys", "worker 1.4.2")
    _ = <-done
}
```

```output
deploys: api 1.4.2
deploys: worker 1.4.2
```

`pattern: true` listens on channel patterns such as `deploys.*`.

## A pool for other connections

The `pool` module keeps connections of any kind for reuse, such as to a service with its own protocol or a client this library doesn't have. `pool.make(config, open)` takes what opening a connection needs and the function that opens one. `take` gives an idle connection or opens a new one, and `give` puts it back. The struct needs two `pub` methods: `close`, and `reusable`, which says whether the next caller can use it:

```iris run
import pool

struct Backend {
    i64 id
    bool broken
}

pub void Backend.close() => print("close {self.id}")
pub bool Backend.reusable() => !self.broken

i64 opened = 0

!Backend dial(str *address) {
    opened += 1
    print("open {opened} to {*address}")
    return Backend{id: opened}
}

!void main() {
    backends := pool.make("10.0.0.5:9000", dial, max: 2)
    defer backends.close()

    a := try backends.take()
    backends.give(a)

    b := try backends.take()
    print("got {b.id} again")
    b.broken = true
    backends.give(b)

    c := try backends.take()
    print("got {c.id}, {backends.size()} open")
    backends.give(c)
}
```

```output
open 1 to 10.0.0.5:9000
got 1 again
close 1
open 2 to 10.0.0.5:9000
got 2, 1 open
close 2
```

- A connection that broke is closed when it is given back, and the next `take` opens a new one.
- When `max` connections are in use, `take` waits for one to come back for at most `wait`, 30 seconds by default, and then fails with `PoolError.timeout`.
- Copies of a `Pool` share the same connections, so it can be handed to tasks and threads.
