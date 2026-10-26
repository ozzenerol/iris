## Connecting

`mongo.connect` gives a `Client` for a server's address, with at most a given number of connections open (10 unless given). Every task and thread of a program can share one client:

```iris
import mongo

!void main() {
    db := try mongo.connect("mongodb://ada:secret@localhost:27017/shop?authSource=admin", 20)
    defer db.close()
    users := db.collection("users")
}
```

The address is `mongodb://user:password@host:port/database`. `?authSource=` names the database that holds the user, `?tls=true` uses TLS and `?tlsCAFile=root.pem` trusts that file's certificates. The login is SCRAM-SHA-256.

## Documents

Documents are `@json` structs, turned into BSON by the [bson](#bson) module. Filters and updates are `bson.Doc` values:

```iris
_ = try mongo.insertOne(&users, &User{name: "Ada".copy(), age: 36})
scoped q := try bson.fromJson(r"""{"age": {"$gt": 30}}""")
found := try mongo.find[User](&users, &q)
scoped inc := try bson.fromJson(r"""{"$inc": {"age": 1}}""")
r := try users.updateMany(&q, &inc)
```

`find` and `aggregate` on a collection give a `Cursor`, which reads the documents a batch at a time.
