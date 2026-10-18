# HTTP

The `http` module has a server, with a `Router` that sends each request to the function for its method and path, and a client. Both speak HTTP/1.1 and HTTP/2, and both do HTTPS.

## A server

A handler takes the request and the response. It fills the response in, and the response is sent when the handler returns. A `Router` holds the handlers: `r.get(path, handler)` adds one for GET, and `post`, `put`, `delete` and `patch` do the same for the other methods. `r.serve(8080)` answers on port 8080 until the program is stopped:

```iris
import http

void hello(http.Request *req, http.Response *res) {
    res.text("hello")
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/", hello)
    try r.serve(8080)
}
```

```shell
curl localhost:8080/
```

```output
hello
```

Each connection gets a task of its own, and `serve` uses every CPU core, so a handler that waits for a database or another server doesn't hold the others up.

The examples in this chapter run the server and a client in one program, so they can show what comes back. `net.listen(0, "127.0.0.1")` opens a port the system picks, `serveOn` answers on it, in a task started with `spawn`, and the client asks it with `http.get`:

```iris run
import http, net

void hello(http.Request *req, http.Response *res) {
    res.text("hello")
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/", hello)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    scoped String url = format("http://127.0.0.1:{ln.port}/")
    scoped http.Reply reply = try http.get(url)
    print("{reply.status} {reply.body}")
}
```

```output
200 hello
```

## Paths

`:name` in a path takes one part of it, and `*name` all the rest. The handler reads them with `req.param`. `req.query` gives a value from the query, after the `?`, and `""` when it isn't there:

```iris run
import http, net, str

void user(http.Request *req, http.Response *res) {
    res.text("user {req.param("id")}")
}

void file(http.Request *req, http.Response *res) {
    res.text("file {req.param("path")}")
}

void logs(http.Request *req, http.Response *res) {
    page := req.query("page")
    res.text("logs of {req.query("host")}, page {if page == "" { "1" } else { page }}")
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/users/:id", user)
    r.get("/files/*path", file)
    r.get("/logs", logs)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    for path in ["/users/42", "/files/docs/a.txt", "/logs?host=edge-1&page=2", "/logs?host=edge-2", "/nope"] {
        scoped String url = format("http://127.0.0.1:{ln.port}{path}")
        scoped http.Reply reply = try http.get(url)
        print("{reply.status} {str.trim(reply.body)}")
    }
    scoped String url = format("http://127.0.0.1:{ln.port}/users/42")
    scoped http.Reply reply = try http.post(url, "text/plain", "")
    print("{reply.status} allowed: {reply.header("Allow")}")
}
```

```output
200 user 42
200 file docs/a.txt
200 logs of edge-1, page 2
200 logs of edge-2, page 1
404 not found
405 allowed: GET
```

A path no pattern has gets 404, with the text `not found` and a newline, which `str.trim` takes off here. A path that is there for other methods gets 405, with an `Allow` header that lists them. When several patterns match, the one with the most fixed parts wins: `/users/me` goes before `/users/:id`, whatever the order they were added in.

## What a request holds

| Field or method | What it gives |
|---|---|
| `req.method` | `"GET"`, `"POST"`, … |
| `req.path` | the path, with its query |
| `req.route()` | the path without its query |
| `req.body` | the body, as text |
| `req.param("id")` | a part of the path, from the pattern |
| `req.query("page")` | a value from the query |
| `req.header("Authorization")` | a header, by its name in any case |
| `req.protocol` | `"HTTP/1.1"` or `"HTTP/2"` |
| `req.received` | when it came in, by `time.clock()` |

## Answers

`res.text(s)` answers with text and `res.json(s)` with JSON; each sets the `Content-Type`. The status is 200 unless `res.status` says another. `res.error(status, message)` sets the status and answers with `{"error": message}`, and `res.header(name, value)` adds a header.

A handler can return `!void`. An error that leaves it is answered with 500 and the error's message, so `try` can be used in a handler like anywhere else.

A JSON body is read with `json.decode`, and a struct is answered with `json.encode` (see [JSON](28-json.html)):

```iris run
import http, json, net

// A backend to add to a load balancer.
@json
struct NewBackend {
    @json(required) String addr
    i64 weight = 1
}

void addBackend(http.Request *req, http.Response *res) {
    scoped NewBackend b = json.decode(NewBackend, req.body) catch e {
        res.error(400, e.message)
        return
    }
    scoped String body = json.encode(b)
    res.status = 201
    res.header("Location", "/backends/1")
    res.json(body)
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    http.Router r
    defer r.free()
    r.post("/backends", addBackend)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    scoped String url = format("http://127.0.0.1:{ln.port}/backends")
    for body in [r"""{"addr": "10.0.0.11:8080", "weight": 3}""", r"""{"weight": 3}""", "oops"] {
        scoped http.Reply reply = try http.post(url, "application/json", body)
        print("{reply.status} {reply.body}")
        if reply.status == 201 {
            print("at {reply.header("Location")}")
        }
    }
}
```

```output
201 {"addr":"10.0.0.11:8080","weight":3}
at /backends/1
400 {"error": "the required field \"addr\" is missing"}
400 {"error": "line 1, column 1: expected an object, found something else"}
```

## Checks and logging

`r.use(check)` runs a check before the handler of every request, and `r.use(check, "/admin")` only for paths that start with `/admin`. A check that answers the request itself returns `false`, and the handler isn't called: a login check, a rate limit. `r.after(done)` runs after every request, answered or not, for a log:

```iris run
import http, net

bool needKey(http.Request *req, http.Response *res) {
    if req.query("key") != "secret" {
        res.error(401, "a key is needed")
        return false
    }
    return true
}

void logLine(http.Request *req, http.Response *res) {
    print("log: {req.method} {req.route()} {res.status}")
}

void stats(http.Request *req, http.Response *res) {
    res.text("3 users")
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    http.Router r
    defer r.free()
    r.use(needKey, "/admin")
    r.after(logLine)
    r.get("/admin/stats", stats)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    for path in ["/admin/stats", "/admin/stats?key=secret"] {
        scoped String url = format("http://127.0.0.1:{ln.port}{path}")
        scoped http.Reply reply = try http.get(url)
        print("{reply.status} {reply.body}")
    }
}
```

```output
log: GET /admin/stats 401
401 {"error": "a key is needed"}
log: GET /admin/stats 200
200 3 users
```

## Limits and stopping

The Router's fields set its limits. `timeout` is how long a read or a write may wait for a slow client (30 seconds unless set). `maxBody` is the largest body, 16 MiB unless set; a larger one gets 413. `gzip` compresses text answers of 1 KiB or more for clients that take it:

```iris
http.Router r
r.timeout = 10s
r.maxBody = 1 << 20
r.gzip = true
```

`r.shutdown()` stops taking new connections, waits for the requests that are being answered, and then `serve` returns. A server that stops cleanly on Ctrl-C, or when systemd or Docker stop it, waits for the signal in a task of its own and calls `shutdown` (see [Programs and the environment](35-processes.html)):

```iris
import http, signal

void hello(http.Request *req, http.Response *res) {
    res.text("hello")
}

void stopOnSignal(http.Router *r) {
    s := signal.wait()
    print("got {s}, stopping")
    r.shutdown()
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/", hello)
    signal.watch()
    spawn stopOnSignal(&r)
    try r.serve(8080)
    print("stopped")
}
```

Stopped with Ctrl-C, it prints:

```output
got interrupt, stopping
stopped
```

## A client

`http.get(url)` and `http.post(url, contentType, body)` send a request and give a `Reply`: its `status`, its `body`, and `reply.header(name)`. `http.send(method, url, body, contentType)` sends any method. The reply owns its body and headers, so it is declared `scoped`. A gzip body is unpacked already.

An address that isn't `http://` or `https://`, a server that can't be reached, or a reply that isn't HTTP is an error. A reply with status 404 or 500 isn't: the program checks `status`.

A JSON reply is read with `json.decode`:

```iris run
import http, json, net

// What a service's health endpoint answers.
@json
struct Health {
    i64 open
    i64 uptime
}

void health(http.Request *req, http.Response *res) {
    res.json(r"""{"open": 3, "uptime": 86400}""")
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/health", health)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    scoped String url = format("http://127.0.0.1:{ln.port}/health")
    scoped http.Reply reply = try http.get(url)
    if reply.status != 200 {
        print("the server answered {reply.status}")
        return
    }
    h := try json.decode(Health, reply.body)
    print("{h.open} open, up {h.uptime}s ({reply.header("content-type")})")

    _ = http.get("ftp://example.com/") catch e {
        print("{e.code}: {e.message}")
        http.Reply{}
    }
}
```

```output
3 open, up 86400s (application/json)
HttpError.badUrl: ftp://example.com/: the address should start with http:// or https://
```

`http.get` and the others open a connection for each request and wait at most 30 seconds for each step. A `Client` keeps connections open between requests, so a program that makes many calls to one host is faster with one. Its `timeout` sets how long it waits, and it can be shared by tasks and threads:

```iris run
import http, net

void ping(http.Request *req, http.Response *res) {
    res.text("pong")
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/ping", ping)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    http.Client c
    defer c.close()
    c.timeout = 5s
    scoped String url = format("http://127.0.0.1:{ln.port}/ping")
    for i in 0..3 {
        scoped http.Reply reply = try c.get(url)
        print("{i}: {reply.status} {reply.body}")
    }
}
```

```output
0: 200 pong
1: 200 pong
2: 200 pong
```

## HTTPS

`r.serveTls(8443, "cert.pem", "key.pem")` serves HTTPS with a certificate and its key, in PEM files. Clients that offer HTTP/2 get it, and the others HTTP/1.1. The client does HTTPS for `https://` addresses and checks the server's certificate against the ones the system trusts; a `Client`'s `ca` field names a file of certificates to trust instead, for a server with a certificate of its own making. TLS comes from OpenSSL, loaded the first time a program uses it.

The library page for [http](../std/index.html#http) lists everything, with WebSockets. The [REST API guide](../guides/01-rest-api.html) builds a whole service with the Router, JSON and SQLite.
