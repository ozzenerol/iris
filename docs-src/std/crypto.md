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

These run in the program itself and take the same time whatever the secret is. SHA-1 and MD5 are here for older protocols only.
