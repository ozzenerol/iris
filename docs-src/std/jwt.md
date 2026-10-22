## Signing and checking tokens

A server signs a token with the claims of a `@json` struct, gives it to a client, and later reads the claims back from it with `verify`, which fails when the token was changed:

```iris run
import jwt, time

@json
struct Claims {
    String sub
    String role
    i64 exp
}

!void main() {
    key := "at least 32 random bytes, kept on the server"
    exp := time.now().add(1h).unix()
    scoped String token = jwt.sign(Claims{sub: "ada".copy(), role: "admin".copy(), exp: exp}, key)

    scoped Claims c = try jwt.verify[Claims](token, key)
    print("{c.sub} is {c.role}")
    _ = jwt.verify[Claims](token, "another key") catch e {
        print("{e.code}")
        Claims{}
    }
}
```

```output
ada is admin
JwtError.signature
```

`verify` takes only tokens signed with HS256, and `verifyEd25519` only EdDSA ones, whatever the token's header says. When the claims have `exp` or `nbf`, in seconds since 1970 as JWT has them, a token past its `exp` fails with `JwtError.expired` and one before its `nbf` with `JwtError.early`. `leeway` gives clocks that differ a little some room.

With `signEd25519`, a service signs with its private seed and other services check with the public key alone, so they can't make tokens of their own.
