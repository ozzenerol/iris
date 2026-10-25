# Random numbers and cryptography

The `random` module gives random numbers for picking, shuffling and simulations, and random bytes for secrets. The `crypto` module hashes, signs and encrypts, and `jwt` builds signed login tokens on top of it. All of them run in the program itself, with no library to install.

## Random numbers

- `random.int(lo, hi)` gives a number from `lo` up to `hi - 1`.
- `random.float()` gives one from 0 up to 1.
- `random.coin()` gives `true` or `false`.
- `random.shuffle` puts the items of a slice in a random order.

```iris run
import random

void main() {
    die := random.int(1, 7)
    print("rolled a six-sided die: {die >= 1 && die <= 6}")
    print("{random.float() < 1.0}")

    str[4] backends = ["10.0.0.11", "10.0.0.12", "10.0.0.13", "10.0.0.14"]
    pick := backends[random.int(0, backends.len)]
    print("picked one of them: {pick.startsWith("10.0.0.")}")

    random.shuffle(backends[:])
    print("{backends.len} after the shuffle")
}
```

```output
rolled a six-sided die: true
true
picked one of them: true
4 after the shuffle
```

Each thread has its own generator, seeded from the system when it starts, so the numbers differ from run to run.

For a test or a simulation that must come out the same each time, `random.seeded(seed)` makes an `Rng` that gives the same numbers from the same seed. It has the same `int`, `float` and `coin`:

```iris run
import random

void main() {
    a := random.seeded(42)
    b := random.seeded(42)

    bool same = true
    for _ in 0..1000 {
        if a.int(0, 100) != b.int(0, 100) {
            same = false
        }
    }

    print("the same 1000 numbers: {same}")
}
```

```output
the same 1000 numbers: true
```

These generators are fast, and someone who sees enough of their numbers can work out the next ones. Don't use them for secrets.

## Random bytes for secrets

`random.bytes(buf)` fills a buffer from the system's secure source, which nobody can guess. Use it for keys, session ids, reset links and passwords. `hex` or `base64` turn the bytes into text (see [Encodings and ids](11-encodings-and-ids.html)):

```iris run
import base64, hex, random

void main() {
    u8[16] id
    random.bytes(id)
    scoped String session = hex.encode(id)
    print("session id of {session.len} hex digits")

    u8[24] reset
    random.bytes(reset)
    scoped String link = base64.encodeUrl(reset)
    print("/reset?token=... with {link.len} characters")
}
```

```output
session id of 32 hex digits
/reset?token=... with 32 characters
```

## Hashes

A hash turns any data into a short value of fixed size. The same data always gives the same hash, and any change gives a different one, so a hash checks that a download or a file is what it should be. `crypto.sha256` gives 32 bytes, and `hex.encode` writes them as the 64 digits that checksum files use:

```iris run
import crypto, hex

void main() {
    scoped String sum = hex.encode(crypto.sha256("release 1.4.2 contents"))
    print(sum)

    expected := "22220c10fb3e49e6f04249e177d1f1a90b1ffd6498599a74355de72dcde32f18"
    print("matches: {sum == expected}")
}
```

```output
22220c10fb3e49e6f04249e177d1f1a90b1ffd6498599a74355de72dcde32f18
matches: true
```

`sha384` gives 48 bytes and `sha512` 64. `sha1` and `md5` are there for older protocols that still use them and are not safe for anything new.

## Signing messages with a shared key

An HMAC is a hash mixed with a secret key, so only someone with the key can make it. A service that sends webhooks signs each body with `crypto.hmacSha256`, and the receiver makes the same HMAC and compares. `crypto.equal` takes the same time wherever the values differ, so its timing gives nothing away:

```iris run
import crypto, hex

String sign(str key, str body) => hex.encode(crypto.hmacSha256(key, body))

bool valid(str key, str body, str signature) {
    scoped String want = sign(key, body)
    return crypto.equal(want, signature)
}

void main() {
    key := "whsec_8f2c41e9b7d0"
    body := r"""{"event":"invoice.paid","id":"in_42"}"""

    scoped String sig = sign(key, body)
    print("X-Signature: {sig.len} hex digits")

    print("as sent: {valid(key, body, sig)}")
    print("changed: {valid(key, r"""{"event":"invoice.paid","id":"in_43"}""", sig)}")
    print("other key: {valid("guess", body, sig)}")
}
```

```output
X-Signature: 64 hex digits
as sent: true
changed: false
other key: false
```

## Passwords

Store a password neither as it is nor as a plain hash, which can be guessed quickly. `crypto.pbkdf2Sha256(password, salt, iterations)` hashes it slowly on purpose, many times over, with a random salt kept next to it, so the same password gives a different hash for each user:

```iris run
import crypto, hex, random

struct Stored {
    String salt
    String hash
}

const ROUNDS = 100000

Stored store(str password) {
    u8[16] salt
    random.bytes(salt)

    scoped String key = crypto.pbkdf2Sha256(password, salt, ROUNDS)
    return Stored{salt: hex.encode(salt), hash: hex.encode(key)}
}

!bool check(Stored *s, str password) {
    scoped String salt = try hex.decode(s.salt)
    scoped String key = crypto.pbkdf2Sha256(password, salt, ROUNDS)
    scoped String hash = hex.encode(key)

    return crypto.equal(hash, s.hash)
}

!void main() {
    s := store("correct horse")
    defer {
        s.salt.free()
        s.hash.free()
    }

    print("salt {s.salt.len} digits, hash {s.hash.len} digits")
    print("right: {try check(&s, "correct horse")}")
    print("wrong: {try check(&s, "correct horse!")}")
}
```

```output
salt 32 digits, hash 64 digits
right: true
wrong: false
```

The number of rounds sets how slow it is. More rounds make guessing harder and each login slower by the same amount.

## Encrypting data

`crypto.seal(key, nonce, data)` encrypts data with ChaCha20-Poly1305, using a 32-byte key and a 12-byte nonce, and adds 16 bytes to check it. `crypto.unseal` gives the data back, or fails with `CryptoError.tampered` when a byte was changed or the key is a different one. A nonce must never be used twice with the same key, so here each message gets a random one, kept in front of the sealed data:

```iris run
import crypto, random

String encrypt(str key, str plain) {
    u8[12] nonce
    random.bytes(nonce)
    scoped String sealed = crypto.seal(key, nonce, plain)

    String out
    out.append(nonce[:])
    out.append(sealed)
    return out
}

!String decrypt(str key, str box) => try crypto.unseal(key, box[0:12], box[12:])

!void main() {
    u8[32] key
    random.bytes(key)

    scoped String box = encrypt(key, "db password: hunter2")
    print("{box.len} bytes stored")

    scoped String back = try decrypt(key, box)
    print(back)

    box[20] ^= 1
    _ = decrypt(key, box) catch e {
        print("{e.code}")
        "".copy()
    }
}
```

```output
48 bytes stored
db password: hunter2
CryptoError.tampered
```

`aesSeal` and `aesUnseal` do the same with AES-GCM, which many file formats and protocols use. An optional `extra` text, such as a record's id, goes along unencrypted but is checked too. The [library](../std/index.html#crypto) has them, and key agreement with X25519.

Protocols that build their own encryption need the ciphers alone. `aesCtr(key, iv, data)` is AES in counter mode, as SSH's `aes128-ctr` uses, `aesBlock(key, block)` encrypts one 16-byte block, as QUIC's header protection does, and `chacha20(key, nonce, data, counter)` is the ChaCha20 stream. None of them checks that the data wasn't changed, so the protocol adds a MAC: `hmacSha256`, `hmacSha384`, `hmacSha512`, or `hmacSha1` for older ones such as TOTP codes. `wipe(bytes)` overwrites a key with zeros once it is done with, in a way the compiler can't leave out.

## Signatures

A signature lets anyone check that a message came from one signer, without being able to sign anything themselves. Ed25519 signs with a secret seed of 32 random bytes.

- `crypto.ed25519Public(seed)` gives the public key, which is shared.
- `ed25519Sign` makes a 64-byte signature.
- `ed25519Verify` checks it with the public key alone.

```iris run
import crypto, random

void main() {
    u8[32] seed
    random.bytes(seed)
    u8[32] public = crypto.ed25519Public(seed)

    manifest := "iris-0.68.0-linux-x86_64.tar.gz sha256=..."
    u8[64] sig = crypto.ed25519Sign(seed, manifest)

    print("{crypto.ed25519Verify(public, manifest, sig)}")
    print("{crypto.ed25519Verify(public, "iris-0.68.0-linux-arm64.tar.gz sha256=...", sig)}")
}
```

```output
true
false
```

Certificates, DNSSEC, SSH and QUIC also sign with ECDSA and RSA. A `crypto.PrivateKey` comes from `loadPrivateKey(pem)`, PEM or DER as openssl writes it, or from `generateKey(.ecP256)`, `.ecP384` or `.rsa` with its bits. `sign(data, hash, padding)` signs, and `publicKey()` gives the half to share. A `crypto.PublicKey` comes from `loadPublicKey` too, from an EC point with `ecPublicKey(curve, point)`, or from RSA's modulus and exponent with `rsaPublicKey(n, e)`, as DNSSEC records hold them. `verify(data, signature, hash, padding)` checks a signature. RSA signs with PKCS#1 v1.5 padding, the start, or `.pss`, as TLS 1.3 does. ECDSA signatures are in DER, and `ecdsaRaw(sig, 32)` and `ecdsaDer(raw)` turn them into r and s and back, the form JSON Web Tokens and DNSSEC carry.

```iris run
import crypto

!void main() {
    scoped key := try crypto.generateKey(.ecP256)
    scoped public := key.publicKey()
    scoped String sig = try key.sign("release 0.73", .sha256)

    print("{public.verify("release 0.73", sig)} {public.verify("release 0.74", sig)}")
    scoped String pem = public.pem()
    print(pem[0:26])
}
```

```output
true false
-----BEGIN PUBLIC KEY-----
```

They come from OpenSSL's libcrypto, as TLS does, and `generateKey` needs OpenSSL 3.

## Tokens for logins

A JSON Web Token carries claims, such as who the user is and when the token ends. They are signed, so the server can trust them when the client sends them back. `jwt.sign(claims, key)` signs a `@json` struct with HS256, and `jwt.verify[T](token, key)` checks the signature and reads the claims back. When the claims have `exp`, in seconds since 1970, a token past it fails with `JwtError.expired`:

```iris run
import jwt, time

@json
struct Claims {
    String sub
    String role
    i64 exp
}

void check(str token, str key) {
    scoped Claims c = jwt.verify[Claims](token, key) catch e {
        print("{e.code}")
        return
    }
    print("{c.sub} as {c.role}")
}

void main() {
    key := "32 or more random bytes, kept on the server"
    now := time.now()

    scoped String good = jwt.sign(Claims{sub: "ada".copy(), role: "admin".copy(), exp: now.add(1h).unix()}, key)
    scoped String old = jwt.sign(Claims{sub: "ada".copy(), role: "admin".copy(), exp: now.add(-1h).unix()}, key)

    scoped parts := good.split(".")
    print("{parts.len} parts")

    check(good, key)
    check(old, key)
    check(good, "another key")
    check("not a token", key)
}
```

```output
3 parts
ada as admin
JwtError.expired
JwtError.signature
JwtError.invalid
```

`verify` takes only HS256 tokens, whatever the token's header says, so a token can't pick a weaker check for itself. `signEd25519` and `verifyEd25519` use a key pair, so other services can check tokens with the public key and can't make their own.
