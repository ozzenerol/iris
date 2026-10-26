## Structs and BSON

`bson.encode` turns a `@json` struct into a BSON document and `bson.decode[T]` reads one back. Field names and `@json("name")` work as they do for JSON. Integers of 32 bits or fewer become int32 and the others int64, enums are their names, lists, slices and arrays are BSON arrays, and a `Map[str, T]` is a document:

```iris
@json
struct User {
    bson.ObjectId _id
    String name
    bson.Date joined
}

scoped String b = bson.encode(u)
back := try bson.decode[User](b)
```

`bson.ObjectId`, `bson.Date`, `bson.Binary` and `bson.Decimal128` are the BSON types of the same names. `bson.newId()` makes an ObjectId and `encodeNew` fills a zero `_id` field with one.

## Documents by hand

`bson.doc()` and `bson.array()` build a `Doc` with calls such as `text`, `int`, `float` and `doc`. `bson.fromJson` reads one from JSON text, with `{"$oid": ...}`, `{"$date": ...}` and `{"$numberLong": ...}` as those types. A `Raw` reads a document in place: `get` finds a field and `for e in raw` goes through them all.
