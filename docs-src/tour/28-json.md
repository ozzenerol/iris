# JSON

A struct marked `@json` can be turned into JSON text and read back from it, with the `json` module. The compiler writes the code for each struct, so there are no lookups by name while the program runs.

## Encoding

`json.encode(v)` gives the JSON text of a value as a new `String`:

```iris run
import json

enum Role { admin, user }

@json
struct User {
    i64 id
    String name
    Role role
    bool active = true
    str[] tags
}

void main() {
    str[2] tags = ["math", "engines"]
    u := User{id: 7, name: "Ada".copy(), role: .admin, tags: tags}
    scoped String text = json.encode(u)
    print(text)
    u.name.free()
}
```

```output
{"id":7,"name":"Ada","role":"admin","active":true,"tags":["math","engines"]}
```

Arrays, slices and plain values can be encoded too: `json.encode(users.items())` gives a JSON array. An enum is written as its value's name.

## Decoding

`json.decode(T, text)` reads a `T` from JSON text and gives a `!T`. Keys the struct doesn't have are skipped, and fields whose keys are missing keep their defaults:

```iris run
import json

enum Role { admin, user }

@json
struct Address {
    String city
    u16 zip
}

@json
struct User {
    i64 id
    String name
    Role role = .user
    bool active = true
    String[] tags
    Address home
    f64 score
}

!void main() {
    text := r"""
        {"id": 7, "name": "Ada", "tags": ["math", "engines"],
         "home": {"city": "London", "zip": 1815}, "score": 9.5, "unknown": [1, 2]}
        """
    scoped User u = try json.decode(User, text)
    print("{u.id} {u.name} {u.role} {u.active} {u.tags.len} {u.home.city} {u.score}")
}
```

```output
7 Ada user true 2 London 9.5
```

`null` leaves a field as it is. A decoded struct owns its `String`s and slices: the `free` method that `@json` gives the struct frees them, so `scoped` works on it, and when `decode` fails, what it had read is freed already.

The type given to `json.decode` is a `@json` struct. A JSON array at the top of the text goes into a struct field: `{"items": [...]}`.

## Keys and required fields

A field's key is its name as written. `@json("key")` gives another key, and `@json(required)` makes `decode` fail when the key is missing. Both can go together, `@json("id", required)`:

```iris run
import json

@json
struct Event {
    @json("event_id", required) i64 id
    @json("created_at") i64 created
    String kind
}

void main() {
    scoped Event e = json.decode(Event, r"""{"event_id": 3, "created_at": 1700000000, "kind": "signup"}""") catch err {
        print(err.message)
        Event{}
    }
    print("{e.id} {e.created} {e.kind}")
    scoped String back = json.encode(e)
    print(back)
    _ = json.decode(Event, r"""{"kind": "signup"}""") catch err {
        print("{err.code}: {err.message}")
        Event{}
    }
}
```

```output
3 1700000000 signup
{"event_id":3,"created_at":1700000000,"kind":"signup"}
JsonError.missing: the required field "event_id" is missing
```

## What a @json struct can hold

Integers, floats, `bool`, `String`, plain enums, other `@json` structs, and slices and arrays of those. Fields of an embedded struct appear as fields of the outer one. A `str` field can be encoded but not decoded, since a `str` can't own the text it would point to; decoded text goes in a `String`.

## Errors

The errors are values of `json.JsonError`, and the message says where in the text it went wrong:

| Code | When |
|---|---|
| `syntax` | the text isn't JSON |
| `wrongType` | a value has the wrong type for its field |
| `missing` | a required key isn't there |
| `range` | a number doesn't fit its field |

```iris run
import json

@json
struct Point {
    i64 x
    u8 tag
    String[] labels
}

void main() {
    for text in [r"""{"x": "one"}""", r"""{"tag": 300}""", r"""{"labels": [1]}""", r"{oops"] {
        _ = json.decode(Point, text) catch e {
            print("{e.code}: {e.message}")
            Point{}
        }
    }
}
```

```output
JsonError.wrongType: x: expected a number, found a string
JsonError.range: tag: 300 doesn't fit in u8
JsonError.wrongType: labels[0]: expected a string, found a number
JsonError.syntax: line 1, column 2: expected a key in quotes
```

## Quoting text

`json.quote(text)` gives text as a JSON string, in quotes, with the characters that need it escaped. It is how a bit of JSON is written by hand:

```iris run
import json

void main() {
    msg := "say \"hi\"\nthen go"
    scoped String q = json.quote(msg)
    print("{{\"message\": {q}}}")
}
```

```output
{"message": "say \"hi\"\nthen go"}
```
