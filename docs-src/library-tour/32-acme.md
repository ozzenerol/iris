# Certificates with ACME

HTTPS needs a certificate that browsers trust. Let's Encrypt and other certificate authorities issue them for free to whoever shows they control a domain, through a protocol named ACME. The `acme` module registers an account, proves control of domains and gets the certificate and its key.

## Getting a certificate

`acme.register(directory, key)` registers an account key with an authority, or finds the account the key already has, and agrees to the authority's terms of service. `obtain(domains)` gets a certificate for the domains:

```iris
import acme, crypto, fs

!void main() {
    key := try crypto.generateKey(.ecP256)
    scoped String saved = key.pem()
    try fs.writeAll("/etc/iris-web/account.key", saved)
    a := try acme.register(acme.LETS_ENCRYPT, key, "ops@example.com")
    defer a.close()

    scoped issued := try a.obtain(["example.com", "www.example.com"])
    try fs.writeAll("/etc/iris-web/cert.pem", issued.chain)
    try fs.writeAll("/etc/iris-web/key.pem", issued.key)
    try fs.chmod("/etc/iris-web/key.pem", 0o600)
}
```

The account key is ECDSA on P-256. Keep it, and register with the same key next time: `crypto.loadPrivateKey` reads it back. The email is where the authority writes about the account, such as a certificate that is about to expire. `acme.LETS_ENCRYPT_STAGING` is Let's Encrypt's staging authority, for trying things out. Its certificates aren't trusted and its rate limits are higher.

`Issued.chain` is the certificate followed by the authority's, in PEM, and `Issued.key` is its new private key. `http.serveTls` and `net.tlsConfig` take the two files as they are (see [HTTP](14-http.html)). The first domain is the certificate's common name, and all of them are its names.

## Proving control with a file

By default `obtain` proves control with http-01. The authority asks for a file at `http://<domain>/.well-known/acme-challenge/<token>`, and `obtain` answers on port 80 while it waits. The domains have to point at the machine, and listening on port 80 needs root or the capability to bind low ports.

When a web server already has port 80, set `httpPort` to 0 and serve the answer from it. `present` is called with the path and the text to give before the authority checks, and `cleanup` afterwards:

```iris
String answerPath
String answerText

!void present(str path, str text) {
    answerPath.append(path)
    answerText.append(text)
}

!void handle(http.Request *req, http.Response *res) {
    if req.path == answerPath {
        res.text(answerText)
        return
    }
    res.error(404, "not found")
}

scoped issued := try a.obtain(["example.com"], acme.ObtainOptions{httpPort: 0, present: present})
```

## Proving control with DNS

With `proof: .dns`, `obtain` proves control with dns-01: a TXT record at `_acme-challenge.<domain>`. `present` gets the record's name and value and sets it through the DNS provider's API, and `cleanup` takes it away. This is the only proof for a wildcard certificate, `*.example.com`, and it needs nothing listening:

```iris
!void setRecord(str name, str value) {
    scoped String body = format("{{\"type\":\"TXT\",\"name\":\"{name}\",\"content\":\"{value}\"}}")
    scoped r := try http.post(dnsApi, "application/json", body)
    if r.status >= 300 {
        return error(acme.AcmeError.refused, "the DNS provider said {r.status}")
    }
    // the record has to reach the provider's servers
    sleep(30s)
}

scoped issued := try a.obtain(["*.example.com", "example.com"],
    acme.ObtainOptions{proof: .dns, present: setRecord, cleanup: removeRecord})
```

## Keys and renewing

`ObtainOptions.keyType` is the certificate key's type: `.ecP256` unless set, `.ecP384`, or `.rsa` for 2048-bit RSA. `timeout` is how long the authority may take to check the proof and issue the certificate, 3 minutes unless set.

Let's Encrypt's certificates last 90 days. Renew one when a third of that is left, by calling `obtain` again. `x509.parsePem` reads the chain back, and `notAfter` is when the certificate expires:

```iris
scoped String pem = try fs.readAll("/etc/iris-web/cert.pem")
certs := try x509.parsePem(pem)
defer certs.freeEach()
left := certs.at(0).notAfter.sub(time.now())
if left < 30 * 24h {
    // obtain again, write the files and reload the server
}
```

## Errors

The errors are `AcmeError` codes:

- `connect`: the authority can't be reached.
- `refused`: the authority refused a request or found a proof wrong. The message has its reason, such as a domain that doesn't point at the machine.
- `invalid`: an answer that isn't ACME, or an account key that isn't P-256.
- `timeout`: the authority didn't finish within `timeout`.
