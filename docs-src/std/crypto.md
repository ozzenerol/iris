## Hashes and HMAC

`sha256` and `sha512` give a fixed-size hash of any text or bytes. `hmacSha256` and `hmacSha512` mix in a key, so only someone with the key can make the same value. `hex.encode` makes them readable, and `equal` compares two of them in the same time wherever they differ:

```iris run
import crypto, hex

void main() {
    scoped String h = hex.encode(crypto.sha256("hello"))
    print(h)
    u8[32] tag = crypto.hmacSha256("server key", "user=7")
    u8[32] again = crypto.hmacSha256("server key", "user=7")
    print("{crypto.equal(tag[0:32], again[0:32])}")
}
```

```output
2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
true
```

## Keys from a secret

`hkdfSha256` makes keys from a shared secret, such as the result of a key exchange. The `info` text says what a key is for, so one secret gives a different key for each use. `pbkdf2Sha256` makes a key from a password, slowly on purpose, for storing passwords.

## Signatures

Ed25519 signs messages with a secret seed of 32 random bytes. `ed25519Public` gives the public key for the seed, which can be shared. `ed25519Sign` makes a 64-byte signature, and `ed25519Verify` checks one with the public key only:

```iris run
import crypto, random

void main() {
    u8[32] seed
    random.bytes(seed[0:32])
    u8[32] key = crypto.ed25519Public(seed[0:32])
    u8[64] sig = crypto.ed25519Sign(seed[0:32], "release 1.4.2")
    print("{crypto.ed25519Verify(key[0:32], "release 1.4.2", sig[0:64])}")
    print("{crypto.ed25519Verify(key[0:32], "release 1.4.3", sig[0:64])}")
}
```

```output
true
false
```

## Encryption

`seal` hides data with ChaCha20-Poly1305, using a 32-byte key and a 12-byte nonce, and adds a 16-byte tag. `unseal` gives the data back, or `CryptoError.tampered` when a byte was changed or the key is another one. A nonce must never be used twice with the same key: count messages, or take each nonce from `random.bytes`. `extra` is data that goes along unhidden but is checked too, like a header:

```iris run
import crypto, random

!void main() {
    u8[32] key
    u8[12] nonce
    random.bytes(key[0:32])
    random.bytes(nonce[0:12])
    scoped String box = crypto.seal(key[0:32], nonce[0:12], "db password: hunter2", extra: "v1")
    scoped String back = try crypto.unseal(key[0:32], nonce[0:12], box, extra: "v1")
    print("{box.len} bytes sealed, back: {back}")
    scoped String wrong = crypto.unseal(key[0:32], nonce[0:12], box, extra: "v2") catch e {
        print("{e.code}")
        return
    }
}
```

```output
36 bytes sealed, back: db password: hunter2
CryptoError.tampered
```

## AES-GCM

`aesSeal` and `aesUnseal` work as `seal` and `unseal` do, with AES-GCM and a 16, 24 or 32-byte key. Where the CPU has AES instructions, as x86_64 and most ARM64 CPUs do, they do the work, and AES-GCM is the fastest choice. Elsewhere it runs in plain code that takes the same time for any key, which is much slower, so `seal` is the better choice there. Many protocols and file formats use AES-GCM, which is the other reason to have it:

```iris run
import crypto, hex, random

!void main() {
    u8[32] key
    random.bytes(key)
    nonce := "nonce 000001"
    scoped String sealed = crypto.aesSeal(key, nonce, "card 4242", extra: "order 17")
    print("{sealed.len} bytes")
    scoped String back = try crypto.aesUnseal(key, nonce, sealed, extra: "order 17")
    print(back)
    _ = crypto.aesUnseal(key, nonce, sealed, extra: "order 18") catch e {
        print("{e.code}")
        format("")
    }
}
```

```output
25 bytes
card 4242
CryptoError.tampered
```

## Agreeing on a key

X25519 lets two sides agree on a secret over a network anyone can read. Each makes a 32-byte secret with `random.bytes` and sends the other `x25519Public` of it. `x25519` with its own secret and the other side's public key gives both the same 32 bytes. `hkdfSha256` turns them into keys for `seal`:

```iris run
import crypto, random

!void main() {
    u8[32] client
    u8[32] server
    random.bytes(client[0:32])
    random.bytes(server[0:32])
    u8[32] clientPub = crypto.x25519Public(client[0:32])
    u8[32] serverPub = crypto.x25519Public(server[0:32])
    u8[32] a = try crypto.x25519(client[0:32], serverPub[0:32])
    u8[32] b = try crypto.x25519(server[0:32], clientPub[0:32])
    scoped String key = crypto.hkdfSha256(a[0:32], "", "chat v1 key")
    print("same secret: {crypto.equal(a[0:32], b[0:32])}, key of {key.len} bytes")
}
```

```output
same secret: true, key of 32 bytes
```

These run in the program itself and take the same time whatever the secret is. SHA-1 and MD5 are here for older protocols only.
