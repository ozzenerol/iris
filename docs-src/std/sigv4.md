## Signing a request

A `Signer` holds the keys, the region and the service. `sign` gives the header lines to send with a request: `X-Amz-Date`, `Authorization`, `X-Amz-Content-Sha256` for S3 and `X-Amz-Security-Token` with a session token. The request's own headers are signed with them, and the Host header comes from the URL:

```iris
import sigv4

!void main() {
    signer := sigv4.Signer{accessKey: "AKIA...", secretKey: "...", region: "eu-west-1", service: "s3"}
    List[String] headers = try signer.sign("GET", "https://photos.s3.eu-west-1.amazonaws.com/cat.jpg", ["Range: bytes=0-99"])
    defer headers.freeEach()
    for h in headers {
        print(h)
    }
}
```

`presign` gives a URL with the signature in its query, good for a given time. `signHash` signs a body given by its SHA-256 as hex, or `UNSIGNED`.
