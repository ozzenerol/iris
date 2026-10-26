# Metrics and tracing

The `metrics` module counts what a program does and writes the counts in the Prometheus text format, for Prometheus to read from a `/metrics` path. The `tracing` module records spans, the steps of a request with when each started and ended. It sends them to an OpenTelemetry collector, and follows a request from one program to the next with the W3C `traceparent` header.

## Counters, gauges and histograms

A counter only goes up: requests served, bytes sent. A gauge goes up and down: connections open, items in a queue. A histogram counts observations, such as how long requests take, in buckets. Each has a name and a help line:

```iris run
import metrics

!void main() {
    jobs := metrics.counter("jobs_total", "Jobs done.")
    queue := metrics.gauge("queue_length", "Jobs waiting.")
    took := metrics.histogram("job_seconds", "How long a job took.", buckets: [0.1, 0.5, 1])

    for seconds in [0.05, 0.3, 0.7, 2.0] {
        jobs.inc()
        took.observe(seconds)
    }
    queue.set(3)
    queue.dec()

    scoped String text = metrics.text()
    print(text)
}
```

```output
# HELP jobs_total Jobs done.
# TYPE jobs_total counter
jobs_total 4
# HELP queue_length Jobs waiting.
# TYPE queue_length gauge
queue_length 2
# HELP job_seconds How long a job took.
# TYPE job_seconds histogram
job_seconds_bucket{le="0.1"} 1
job_seconds_bucket{le="0.5"} 2
job_seconds_bucket{le="1"} 3
job_seconds_bucket{le="+Inf"} 4
job_seconds_sum 3.05
job_seconds_count 4

```

A histogram without buckets gets `metrics.DEFAULT_BUCKETS`, from 5ms to 10s. `took.since(start)` observes the seconds since a `time.clock()` value.

Every method can be called from any thread at the same time. Asking for a name that exists already gives the same metric back, so two parts of a program can both ask for `jobs_total`.

## Labels

A metric with labels has one value for each set of label values. `counterVec`, `gaugeVec` and `histogramVec` take the label names, and `with` takes the values in the same order:

```iris run
import metrics

!void main() {
    requests := metrics.counterVec("requests_total", "Requests, by method and status.", ["method", "code"])
    requests.with(["GET", "200"]).inc()
    requests.with(["GET", "200"]).inc()
    requests.with(["POST", "500"]).inc()

    scoped String text = metrics.text()
    print(text)
}
```

```output
# HELP requests_total Requests, by method and status.
# TYPE requests_total counter
requests_total{method="GET",code="200"} 2
requests_total{method="POST",code="500"} 1

```

`with` looks the values up each time. A counter it gives can be kept in a variable to skip the lookup. Each set of values stays in memory until the end, so labels should have a few values each: a method or a status, and never a user id.

## Serving /metrics

`metrics.handler` answers with every metric. `metrics.track(&router)` counts each request the router answers in `http_requests_total`, by method and status, and how long it took in `http_request_duration_seconds`:

```iris
import http, metrics

void hello(http.Request *req, http.Response *res) {
    res.send("hello\n", "text/plain")
}

!void main() {
    http.Router r
    r.get("/", hello)
    r.get("/metrics", metrics.handler)
    metrics.track(&r)
    try r.serve(8080)
}
```

A program with metrics of its own makes a `metrics.Registry` and calls the same methods on it: `reg.counter(...)`, `reg.text()`.

## Spans

A span is a named step with a start and an end. `tracing.start` begins a trace, and `span.child` a step inside one. Attributes say what the step worked on, and `fail` says it failed:

```iris
import tracing

!void main() {
    tracing.setup("shop", "http://localhost:4318")
    defer tracing.shutdown()

    order := tracing.start("place order")
    order.text("customer", "Ana")

    check := order.child("check stock")
    check.int("items", 3)
    check.end()

    pay := order.child("charge card")
    pay.fail("card declined")
    pay.end()

    order.end()
}
```

`setup` names the service and gives the collector's address. Spans that end are sent to its `/v1/traces` in batches, as OTLP/HTTP JSON, every second. `shutdown` sends the last ones. Before `setup`, spans go nowhere, so a library can make spans whether or not the program sends them.

## Across programs

A request carries its trace to the next program in the `traceparent` header. `tracing.track(&router)` gives each request a server span, the child of the span in its `traceparent` when it has one. The handler gets it with `tracing.spanOf(req)`. `tracing.send` makes a request as a client span and adds its `traceparent`:

```iris
import http, tracing

http.Client client

void order(http.Request *req, http.Response *res) {
    span := tracing.spanOf(req)
    reply := tracing.send(&client, span, "GET", "http://stock:8080/count") catch e {
        res.error(502, e.message)
        return
    }
    defer reply.free()
    res.send(reply.body, "text/plain")
}

!void main() {
    tracing.setup("orders", "http://localhost:4318")
    http.Router r
    r.get("/order", order)
    tracing.track(&r)
    try r.serve(8080)
}
```

When the stock service also calls `track`, its span is a child of this one, and the collector shows both in one trace. `tracing.parse` reads a `traceparent` by hand, and `span.traceparent()` writes one, for messages that go some other way, such as a queue.
