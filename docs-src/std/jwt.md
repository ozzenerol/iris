## Signing and checking tokens

A server signs a token with the claims of a `@json` struct and gives it to a client. Later, `verify` reads the claims back and fails when the token was changed:

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

`verify` accepts only tokens signed with HS256, and `verifyEd25519` only EdDSA ones, whatever the token's header says.

`exp` and `nbf` claims are seconds since 1970. A token past its `exp` fails with `JwtError.expired`, and one before its `nbf` with `JwtError.early`. `leeway` gives some room to clocks that differ a little.

With `signEd25519`, a service signs with its private seed and other services check with the public key alone. Those services can't make tokens of their own.
