## Encoding and decoding

`json.encode` turns a struct marked `@json` into JSON, and `json.decode` reads it back. Field names are used as written. `String` and `str` fields become JSON strings, numbers and `bool` stay as they are, and enums go by their value's name. The [JSON chapter](../library-tour/01-json.html) of the Library Tour has every rule. This is the short version:

```iris run
import json

enum Role { user, admin }

@json
struct User {
    i64 id
    String name
    Role role
    String[] tags
}

!void main() {
    scoped User u = try json.decode(User, r"""{"id": 7, "name": "Ada", "role": "admin", "tags": ["math"]}""")
    print("{u.id} {u.name} {u.role} {u.tags.len}")

    scoped String text = json.encode(u)
    print(text)
}
```

```output
7 Ada admin 1
{"id":7,"name":"Ada","role":"admin","tags":["math"]}
```

The decoded value owns its text and slices. `scoped` frees them with the `free` method that `@json` adds.

## Arrays

`json.decode(Point[], text)` reads a JSON array into a slice of structs:

```iris run
import json

@json
struct Point {
    i64 x
    i64 y
}

!void main() {
    ps := try json.decode(Point[], r"""[{"x": 1, "y": 2}, {"x": 3, "y": 4}]""")
    for p in ps {
        print("{p.x},{p.y}")
    }
    free(ps)
}
```

```output
1,2
3,4
```

`json.decode(List[Point], text)` reads it into a `List` instead. A `List` field of a `@json` struct is read and written as an array the same way. A `Map` with `str` or `String` keys is read and written as an object.

## Errors

Decoding fails with a `JsonError`, and the message says where:

```iris run
import json

@json
struct Point {
    i64 x
    i64 y
}

void main() {
    _ = json.decode(Point, r"""{"x": "one"}""") catch e {
        print("{e.code}: {e.message}")
        Point{}
    }
}
```

```output
JsonError.wrongType: x: expected a number, found a string
```

`json.quote` writes one string as JSON, for building JSON text by hand.
