## A server with a Router

A `Router` sends each request to the handler for its method and path. `:name` in a path takes one part of it, and `*name` the rest; the handler reads them with `req.param`. `r.serve(8080)` answers on port 8080 until the program is stopped, with every CPU core. Here the server listens on a port the system picks, in a task, and the same program asks it with the client:

```iris run
import http, net

void hello(http.Request *req, http.Response *res) {
    res.text("hello, {req.param("name")}")
}

void addItem(http.Request *req, http.Response *res) {
    res.status = 201
    res.json("{{\"got\": {req.body.len}}}")
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/hello/:name", hello)
    r.post("/items", addItem)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    scoped String a = format("http://127.0.0.1:{ln.port}/hello/ada")
    scoped http.Reply one = try http.get(a)
    print("{one.status} {one.body} ({one.header("Content-Type")})")
    scoped String b = format("http://127.0.0.1:{ln.port}/items")
    scoped http.Reply two = try http.post(b, "text/plain", "pear")
    print("{two.status} {two.body}")
    scoped String c = format("http://127.0.0.1:{ln.port}/nope")
    scoped http.Reply three = try http.get(c)
    print("{three.status}")
}
```

```output
200 hello, ada (text/plain; charset=utf-8)
201 {"got": 4}
404
```

Each connection gets a task of its own, so a handler can wait for a database or another server while the others go on. A path no pattern has gets 404, and a path that is there for other methods gets 405. When several patterns match, the one with the most fixed parts wins. A handler that returns an error is answered with 500 and the error's message. `res.error(404, "no such user")` answers with a status and a message, and `res.header` adds a header.

## What a request holds

`req.method`, `req.path` and `req.body` are the request's parts. `req.route()` is the path without its query, `req.query("page")` a value from the query, and `req.header("Authorization")` a header, by its name in any case.

## Checks and logging

`use` runs a check before the handlers, on every path or the ones that start with a prefix. A check that answers the request itself returns `false`, and the handler isn't called. `after` runs after each request, for logging:

```iris
bool needKey(http.Request *req, http.Response *res) {
    if req.header("X-Key") != "secret" {
        res.error(401, "a key is needed")
        return false
    }
    return true
}

void logLine(http.Request *req, http.Response *res) {
    print("{req.method} {req.path} {res.status}")
}

!void main() {
    http.Router r
    defer r.free()
    r.use(needKey, "/admin")
    r.after(logLine)
    r.get("/admin/stats", stats)
    try r.serve(8080)
}
```

The Router's fields set its limits: `timeout` for a slow client, `maxBody` and `maxHeader` for the size of a request, and `gzip` to compress replies for clients that take it. `shutdown` stops taking new connections and waits for the requests that are running.

## HTTPS and HTTP/2

`serveTls` takes a certificate file and its key, in PEM. Clients that offer HTTP/2 get it; the others get HTTP/1.1. The client does the same for `https://` addresses:

```iris
try r.serveTls(8443, "cert.pem", "key.pem")
```

## A client that keeps connections

`http.get`, `post` and `send` open a connection for each request. A `Client` keeps them open between requests, so a program that makes many calls to one host is faster with one. It can be shared by tasks and threads:

```iris
http.Client c
defer c.close()
c.timeout = 10s
for id in ids {
    scoped String url = format("https://api.example.com/items/{id}")
    scoped http.Reply r = try c.get(url)
    print("{id}: {r.status}")
}
```

## WebSockets

`r.websocket(path, handler)` answers WebSockets at a path; the handler gets the open socket and reads and sends messages until it returns. `http.dialWebSocket` opens one as a client:

```iris
!void echo(http.WebSocket *ws, http.Request *req) {
    for {
        scoped http.Message m = try ws.read()
        try ws.send(m.data)
    }
}
```

The [REST API guide](../guides/01-rest-api.html) builds a whole service with the Router and SQLite.
