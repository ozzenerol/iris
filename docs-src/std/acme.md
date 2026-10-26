## Getting a certificate

`acme.register` registers an account key with a certificate authority such as Let's Encrypt and gives an `Account`. `Account.obtain` proves control of domains, with a file on port 80 or a DNS record, and gives the certificate chain and its key in PEM. The [ACME chapter](../library-tour/32-acme.html) of the Library Tour has more:

```iris
import acme, crypto, fs

!void main() {
    scoped String saved = try fs.readAll("account.key")
    key := try crypto.loadPrivateKey(saved)
    a := try acme.register(acme.LETS_ENCRYPT, key, "ops@example.com")
    defer a.close()
    scoped issued := try a.obtain(["example.com"])
    try fs.writeAll("cert.pem", issued.chain)
    try fs.writeAll("key.pem", issued.key)
}
```
