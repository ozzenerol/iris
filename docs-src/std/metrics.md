## Counting requests

```iris
import http, metrics

!void main() {
    http.Router r
    r.get("/metrics", metrics.handler)
    metrics.track(&r)
    try r.serve(8080)
}
```

The [Metrics and tracing chapter](../library-tour/28-metrics-and-tracing.html) of the Library Tour has counters, gauges, histograms and labels.
