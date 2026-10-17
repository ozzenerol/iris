## Connecting

`redis.connect` gives a `Client` for a server's address, which keeps up to a number of connections open (10 unless one is given). It can be shared by every task and thread of a program:

```iris
import redis

!void main() {
    db := try redis.connect("redis://localhost:6379")
    defer db.close()
}
```

The address is `redis://user:password@host:port/database`, and `rediss://` uses TLS; `?ca=file` trusts certificates other than the system's.

## Keys and values

`set` stores a value, with a time to live when `ttl` is given, and `get` gives a value and whether the key is there:

```iris
try db.set("session:42", "ada", ttl: 30m)
name, found := try db.get("session:42")
defer name.free()
if found {
    print("signed in as {name}")
}
visits := try db.incr("visits")
left := try db.ttl("session:42")
removed := try db.del(["session:42", "visits"])
```

Hashes, lists and sets have their own calls: `hset`, `hget` and `hgetAll`; `lpush`, `rpush` and `lrange`; `sadd` and `smembers`.

## Any command

`do` sends any command, with its words as a list, and gives the server's `Reply`. An error reply fails with `RedisError.server`, with the server's message:

```iris
scoped redis.Reply r = try db.do(["ZADD", "scores", "10", "ada"])
print("{r.int} added")
scoped redis.Reply top = try db.do(["ZRANGE", "scores", "0", "2"])
for item in top.items {
    print(item.text)
}
```

A `Reply` holds `text` for strings, `int` for numbers, and `items` for lists; `kind` says which it is, and `isNil()` whether the key wasn't there.

## Pipelines

A `Pipeline` sends several commands together and waits once for all their replies, which saves a round trip for each:

```iris
scoped p := db.pipeline()
p.add(["SET", "a", "1"])
p.add(["INCR", "a"])
p.add(["PING"])
replies := try p.run()
for r in replies {
    print("{r.kind} {r.text} {r.int}")
    r.free()
}
replies.free()
```

An error reply in a pipeline is a `Reply` of kind `.error`, so the other replies still come back.

## Publish and subscribe

`publish` sends a message on a channel. `redis.subscribe` opens a connection of its own that receives the messages of some channels, or of channel patterns like `room.*` with `pattern: true`; `next` waits for the next one:

```iris
void listen(str address) {
    scoped sub := redis.subscribe(address, ["room.*"], pattern: true) catch e {
        print("subscribe: {e.message}")
        return
    }
    for {
        scoped m := sub.next() catch e {
            return
        }
        print("{m.channel}: {m.data}")
    }
}
```
