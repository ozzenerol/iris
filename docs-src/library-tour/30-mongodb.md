# MongoDB

The `mongo` module talks to MongoDB over the network, with nothing else to install. Documents are `@json` structs, turned into MongoDB's binary format, BSON, by the `bson` module. A `mongo.Client` keeps a pool of connections that every task and thread can share.

The examples in this chapter need a server, so they read its address from an environment variable. A MongoDB server for trying them runs with `docker run -p 27017:27017 -e MONGO_INITDB_ROOT_USERNAME=root -e MONGO_INITDB_ROOT_PASSWORD=secret mongo:8.0`.

## Connecting and inserting

`mongo.connect(address)` connects, logs in and gives a `mongo.Client`, which opens more connections as tasks need them, up to the number given after the address, or 10. `collection(name)` gives a collection of the address's database. `mongo.insertOne` inserts a `@json` struct and `mongo.insertMany` a slice of them. A field `_id` of type `bson.ObjectId` that is still zero gets a new id, in the struct too:

```iris
import bson, mongo, os

@json
struct Product {
    bson.ObjectId _id
    String name
    f64 price
    i64 stock
    List[String] tags
}

!void main() {
    address := os.env("MONGO_URL") catch "mongodb://root:secret@localhost/shop?authSource=admin"
    db := try mongo.connect(address, 20)
    defer db.close()

    products := db.collection("products")
    try products.drop()

    Product tea = Product{name: "green tea".copy(), price: 3.5, stock: 10}
    tea.tags.push("drinks".copy())
    defer tea.name.free()
    defer tea.tags.freeEach()
    id := try mongo.insertOne(&products, &tea)
    scoped String hex = id.hex()
    print("tea is {hex.len} hex digits: {tea._id.isZero() == false}")

    Product[2] more = [
        Product{name: "lemon cake".copy(), price: 4.25, stock: 3},
        Product{name: "coffee".copy(), price: 2.8, stock: 25},
    ]
    n := try mongo.insertMany(&products, more[0:2])
    for p in more {
        p.name.free()
    }
    print("{n} more, {try products.count()} in all")
}
```

```output
tea is 24 hex digits: true
2 more, 3 in all
```

- The address is `mongodb://user:password@host:port/database`. `?authSource=admin` names the database that holds the user, which is the address's database unless given.
- `?tls=true` makes the connection use TLS, and `?tlsCAFile=root.pem` trusts that file's certificates.
- Several hosts, as in `mongodb://root:secret@db1,db2,db3/shop?replicaSet=rs0`, name the members of a replica set. The client finds the primary and sends every command there.
- `mongodb+srv://root:secret@cluster.example.com/shop` looks up the hosts in the SRV records of `_mongodb._tcp.cluster.example.com`, and `authSource` and `replicaSet` in its TXT record. It uses TLS.
- The login is SCRAM-SHA-256, MongoDB's default.
- A refused login fails with `MongoError.auth`, and a server that can't be reached with `MongoError.connect`.
- `drop` removes a collection. One that isn't there is no error.

## Finding documents

A filter is a `bson.Doc`. `bson.fromJson` reads one from JSON text written the way MongoDB shows queries, and `bson.doc()` builds one with calls such as `text`, `int` and `doc`. `mongo.find[T]` gives every document that matches as a `List[T]`, with an optional sort, projection, limit and skip. `mongo.findOne[T]` gives the first, and fails with `MongoError.notFound` when there is none. Fields the struct doesn't have are skipped:

```iris
import bson, mongo, os

@json
struct Product {
    String name
    f64 price
    i64 stock
}

!void main() {
    db := try mongo.connect(os.env("MONGO_URL") catch "mongodb://root:secret@localhost/shop?authSource=admin")
    defer db.close()
    products := db.collection("products")

    scoped cheap := try bson.fromJson(r"""{"price": {"$lt": 4}}""")
    scoped byPrice := try bson.fromJson(r"""{"price": 1}""")
    found := try mongo.find[Product](&products, &cheap, sort: &byPrice)
    defer found.free()
    for p in found {
        print("{p.name}: {p.price}")
        p.name.free()
    }

    scoped q := bson.doc()
    q.text("name", "lemon cake")
    cake := try mongo.findOne[Product](&products, &q)
    defer cake.name.free()
    print("{cake.name}, {cake.stock} left")
}
```

```output
coffee: 2.8
green tea: 3.5
lemon cake, 3 left
```

## Changing and removing

`updateOne` and `updateMany` take a filter and an update, such as `{"$set": ...}` or `{"$inc": ...}`, and give how many documents matched and how many changed. With `upsert: true`, a document is made when none matches, and its id is in `upserted`. `deleteOne` and `deleteMany` give how many went. `createIndex` makes an index, and a unique one makes a second document with the same key fail with `MongoError.duplicate`:

```iris
import bson, mongo, os

!void main() {
    db := try mongo.connect(os.env("MONGO_URL") catch "mongodb://root:secret@localhost/shop?authSource=admin")
    defer db.close()
    products := db.collection("products")

    scoped coffee := bson.doc()
    coffee.text("name", "coffee")
    scoped sell := try bson.fromJson(r"""{"$inc": {"stock": -4}}""")
    r := try products.updateOne(&coffee, &sell)
    print("matched {r.matched}, changed {r.modified}")

    scoped all := bson.doc()
    scoped sale := bson.doc()
    scoped by := bson.doc()
    by.float("price", 0.9)
    sale.doc("$mul", &by)
    r = try products.updateMany(&all, &sale)
    print("{r.modified} prices lowered")

    scoped scone := bson.doc()
    scone.text("name", "scone")
    scoped add := try bson.fromJson(r"""{"$set": {"price": 2.0, "stock": 6}}""")
    r = try products.updateOne(&scone, &add, upsert: true)
    print("a new scone: {!r.upserted.isZero()}")

    scoped few := try bson.fromJson(r"""{"stock": {"$lt": 5}}""")
    print("{try products.deleteMany(&few)} sold out or nearly")

    scoped keys := bson.doc()
    keys.int("name", 1)
    scoped String index = try products.createIndex(&keys, unique: true)
    print("index {index}")
    products.insertDoc(&coffee) catch e {
        print("{e.code}")
    }
}
```

```output
matched 1, changed 1
3 prices lowered
a new scone: true
1 sold out or nearly
index name_1
MongoError.duplicate
```

`mongo.replaceOne` puts a whole struct in the place of the document a filter matches.

## Cursors and aggregation

`find` on a collection gives a `mongo.Cursor`, which reads the documents a batch at a time and asks the server for the next batch when one runs out. `cur.doc()` is the current document as a `bson.Raw`, whose fields `get` finds without decoding the rest. `aggregate` runs a pipeline and gives a cursor too. A cursor freed before its end tells the server to drop it:

```iris
import bson, mongo, os

!void main() {
    db := try mongo.connect(os.env("MONGO_URL") catch "mongodb://root:secret@localhost/shop?authSource=admin")
    defer db.close()
    products := db.collection("products")

    scoped cur := try products.find(batchSize: 100)
    for try cur.next() {
        doc := cur.doc()
        name, _ := doc.get("name")
        stock, _ := doc.get("stock")
        print("{name.text()}: {stock.int()}")
    }

    scoped pipeline := try bson.fromJson(r"""[
        {"$group": {"_id": null, "items": {"$sum": "$stock"}, "worth": {"$sum": {"$multiply": ["$price", "$stock"]}}}}
    ]""")
    scoped totals := try products.aggregate(&pipeline)
    for try totals.next() {
        items, _ := totals.doc().get("items")
        worth, _ := totals.doc().get("worth")
        print("{items.int()} items worth {worth.float():.2}")
    }
}
```

```output
green tea: 10
coffee: 21
scone: 6
37 items worth 96.42
```

`db.command(database, cmd)` runs any other command and gives its reply as a `bson.Doc`.

## Replica sets, transactions and change streams

When the primary steps down or goes away, the client looks for the new one for up to 30 seconds (`?serverSelectionTimeoutMS=` sets this). A command that failed because of that runs again once on the new primary: reads, inserts, and updates and deletes of one document. Those writes carry a transaction number, so the server doesn't do them twice. `?retryWrites=false` turns this off for writes. Updates and deletes of many documents run only once.

A session runs a transaction. `startSession` gives one, and the collections it gives run their commands in it. What happens between `startTransaction` and `commit` is seen by others only after the commit, and not at all after `abort`. A command that fails with `MongoError.transient` means the transaction may work if it runs again: abort it and start over. `end` ends the session.

```iris
import bson, mongo, os

@json
struct Account {
    String name
    i64 balance
}

!void main() {
    db := try mongo.connect(os.env("MONGO_URL") catch "mongodb://root:secret@db1,db2/bank?replicaSet=rs0&authSource=admin")
    defer db.close()
    sess := db.startSession()
    defer sess.end()
    accounts := sess.collection("accounts")
    scoped from := try bson.fromJson(r"""{"name": "ada"}""")
    scoped to := try bson.fromJson(r"""{"name": "bob"}""")
    scoped take := try bson.fromJson(r"""{"$inc": {"balance": -10}}""")
    scoped give := try bson.fromJson(r"""{"$inc": {"balance": 10}}""")
    try sess.startTransaction()
    _ = try accounts.updateOne(&from, &take)
    _ = try accounts.updateOne(&to, &give)
    try sess.commit()
}
```

`watch` follows the changes to a collection as they happen. Each document its cursor gives is one change, with `operationType` (`insert`, `update`, `replace`, `delete`...), `documentKey`, and `fullDocument` for an insert, or for an update too with `fullDocument: true`. A pipeline adds stages, such as a `$match` on the kind of change. `next` waits for the next change. With `wait`, it gives false after that long without one, and can be called again. When the primary changes, the stream goes on from the last change it gave.

```iris
import bson, mongo, os

!void main() {
    db := try mongo.connect(os.env("MONGO_URL") catch "mongodb://root:secret@db1,db2/shop?replicaSet=rs0&authSource=admin")
    defer db.close()
    orders := db.collection("orders")
    scoped inserts := try bson.fromJson(r"""[{"$match": {"operationType": "insert"}}]""")
    scoped changes := try orders.watch(&inserts)
    for try changes.next() {
        order, _ := changes.doc().get("fullDocument")
        id, _ := order.doc().get("_id")
        print("new order {id.id().hex()}")
    }
}
```

Transactions and change streams need a replica set or a sharded cluster. A replica set of one member is enough for development.

## BSON

The `bson` module works without a server. `bson.encode` turns a `@json` struct into BSON and `bson.decode[T]` turns it back. `bson.ObjectId`, `bson.Date`, `bson.Binary` and `bson.Decimal128` are fields of those BSON types. A `bson.Raw` reads a document in place, and `for e in raw` goes through its fields:

```iris run
import bson

@json
struct Point {
    String label
    i64 x
    i64 y
    bson.Date seen
}

!void main() {
    p := Point{label: "home".copy(), x: 3, y: -4, seen: bson.Date{ms: 1792000000000}}
    defer p.label.free()
    scoped String b = bson.encode(p)
    print("{b.len} bytes")

    back := try bson.decode[Point](b)
    defer back.label.free()
    print("{back.label} {back.x} {back.y} {back.seen.ms}")

    raw := bson.Raw{bytes: b}
    for e in raw {
        print("{e.key}: type {e.kind}")
    }

    scoped d := bson.doc()
    d.text("name", "Ada")
    scoped langs := bson.array()
    langs.text("", "iris")
    langs.text("", "go")
    d.doc("langs", &langs)
    d.id("_id", bson.newId())
    langList, _ := d.raw().get("langs")
    for l in langList.doc() {
        print("lang {l.key}: {l.text()}")
    }
}
```

```output
57 bytes
home 3 -4 1792000000000
label: type 2
x: type 18
y: type 18
seen: type 9
lang 0: iris
lang 1: go
```

Integers of 32 bits or fewer are BSON int32 and the rest int64. An array built with `bson.array()` numbers its items itself, so the key given to each call is ignored.
