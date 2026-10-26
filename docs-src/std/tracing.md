## Tracing requests

```iris
import http, tracing

!void main() {
    tracing.setup("orders", "http://localhost:4318")
    defer tracing.shutdown()
    http.Router r
    tracing.track(&r)
    try r.serve(8080)
}
```

The [Metrics and tracing chapter](../library-tour/28-metrics-and-tracing.html) of the Library Tour has spans, attributes and requests to other services.
