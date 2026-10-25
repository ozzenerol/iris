# HTTP

The `http` module has a server and a client. The server's `Router` sends each request to the function for its method and path. Both sides speak HTTP/1.1 and HTTP/2, and both do HTTPS.

## A server

A handler takes the request and the response. It fills in the response, which is sent when the handler returns. A `Router` holds the handlers. `r.get(path, handler)` adds one for GET, and `post`, `put`, `delete` and `patch` do the same for the other methods. `r.serve(8080)` answers on port 8080 until the program is stopped.

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

Each connection gets its own task and `serve` uses every CPU core, so a handler that waits for a database or another server doesn't hold up the others.

The request belongs to the server. Its path, headers and body are gone when the handler returns, so a value kept longer, in a Map, on a channel or in a task the handler starts, has to be a copy: `seen.push(req.path.copy())`.

The examples in this chapter run the server and a client in one program, so they can show the reply. `net.listen(0, "127.0.0.1")` opens a port the system picks. `serveOn` answers on it in a task started with `spawn`, and the client calls it with `http.get`.

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

`:name` in a path takes one part of it and `*name` takes the rest. The handler reads them with `req.param`. `req.query` gives a value from the query after the `?`, or `""` when it isn't there. Both decode `%XX`, and `req.query` also turns `+` into a space, as browsers send it.

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

- A path that matches no pattern gets 404 with the text `not found` and a newline, which `str.trim` removes here.
- A path that exists for other methods gets 405 with an `Allow` header that lists them.
- When several patterns match, the one with the most fixed parts wins. `/users/me` goes before `/users/:id`, whatever order they were added in.

## What a request holds

| Field or method | What it gives |
|---|---|
| `req.method` | `"GET"`, `"POST"`, … |
| `req.path` | the path, with its query |
| `req.route()` | the path without its query |
| `req.body` | the body, as text |
| `req.param("id")` | a part of the path, from the pattern |
| `req.query("page")` | a value from the query |
| `req.form("email")` | a field of a form in the body |
| `req.file("photo")` | a file sent in a form |
| `req.cookie("session")` | a cookie the client sent |
| `req.header("Authorization")` | a header, by its name in any case |
| `req.headers()` | every header, as a `List[http.Header]` of `name` and `value` |
| `req.protocol` | `"HTTP/1.1"` or `"HTTP/2"` |
| `req.received` | when it came in, by `time.clock()` |
| `req.remote` | who sent it, a `net.Addr`: `req.remote.ipText()` and `req.remote.port` |

## Answers

- `res.text(s)` answers with text, `res.json(s)` with JSON and `res.html(s)` with HTML. Each sets the `Content-Type`.
- `res.send(s, type)` answers with any other type, such as `text/csv`.
- The status is 200 unless `res.status` sets another.
- `res.error(status, message)` sets the status and answers with `{"error": message}`.
- `res.header(name, value)` adds a header. A `Content-Type` set with `header` replaces the type of the answer, so there is only ever one.

A handler can return `!void`. An error that leaves it is answered with 500 and the error's message, so `try` works in a handler as anywhere else.

Read a JSON body with `json.decode` and answer with a struct through `json.encode` (see [JSON](01-json.html)):

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

## Forms and uploads

`req.form(name)` gives a field of a form in the body. The form can be `application/x-www-form-urlencoded`, with its `%XX` and `+` decoded, or `multipart/form-data`. `req.file(name)` gives a file sent in a multipart form as an `http.Part` with its `filename`, `contentType` and `data`, and whether there is one. Both return text that points into the request and stays valid until the handler returns.

A client builds a multipart body with `http.multipart()`. `field` adds a field, `file` a file, and `finish` ends the body. `contentType()` is the `Content-Type` to send it with.

```iris run
import http, net

void upload(http.Request *req, http.Response *res) {
    f, ok := req.file("photo")
    if !ok {
        res.error(400, "no photo")
        return
    }
    res.text("{req.form("title")}: {f.filename}, {f.contentType}, {f.data.len} bytes")
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    http.Router r
    defer r.free()
    r.post("/photos", upload)

    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    scoped http.Multipart form = http.multipart()
    form.field("title", "The harbour at dawn")
    form.file("photo", "harbour.jpg", "image/jpeg", "not really a jpeg")
    form.finish()

    scoped String url = format("http://127.0.0.1:{ln.port}/photos")
    scoped String contentType = form.contentType()
    scoped http.Reply reply = try http.post(url, contentType, form.body)
    print("{reply.status} {reply.body}")

    scoped http.Reply none = try http.post(url, "application/x-www-form-urlencoded", "title=No+photo")
    print("{none.status} {none.body}")
}
```

```output
200 The harbour at dawn: harbour.jpg, image/jpeg, 17 bytes
400 {"error": "no photo"}
```

The whole body is read before the handler runs, so an upload counts against the Router's `maxBody`, 16 MiB by default. A body can come with a `Content-Length` or in chunks (`Transfer-Encoding: chunked`), and a client that sends `Expect: 100-continue` is told to go on. A request that gives its length in two ways, or in a way the server can't follow, gets 400 and its connection is closed. `http.parseMultipart(body, contentType)` gives every part of a multipart body, in order.

## Cookies

`req.cookie(name)` gives the value of a cookie the client sent, or `""` when it sent none. `res.setCookie(c)` adds a `Set-Cookie` header for an `http.Cookie`. `res.clearCookie(name)` tells the browser to remove one.

| Field | What it sets | Default |
|---|---|---|
| `name`, `value` | the cookie | |
| `path` | the paths it is sent to | `"/"` |
| `domain` | the hosts it is sent to | this host only |
| `maxAge` | how long it is kept; `0s` until the browser closes | `0s` |
| `secure` | sent only over HTTPS | `false` |
| `httpOnly` | not readable by JavaScript | `true` |
| `sameSite` | `.lax`, `.strict`, `.none`, or `.unset` to leave it out | `.lax` |

The value is sent as it is, so keep it to plain text without spaces, quotes, commas or semicolons. `base64` or `url.encode` makes any value fit.

```iris run
import http, net, str

void login(http.Request *req, http.Response *res) {
    res.setCookie(http.Cookie{name: "session", value: "7f3a9c", maxAge: 8h, secure: true})
    res.clearCookie("guest")
    res.text("welcome back, {req.cookie("user")}")
}

void serve(net.Listener ln) {
    http.serveOn(ln, login) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(ln)

    c := try net.dial("127.0.0.1", ln.port)
    defer c.close()
    try c.write("GET / HTTP/1.1\r\nHost: localhost\r\nCookie: user=ada; theme=dark\r\nConnection: close\r\n\r\n")

    for {
        scoped String line = c.readLine() catch e {
            break
        }
        if str.startsWith(line, "Set-Cookie") || str.startsWith(line, "welcome") {
            print(line)
        }
    }
}
```

```output
Set-Cookie: session=7f3a9c; Path=/; Max-Age=28800; HttpOnly; Secure; SameSite=Lax
Set-Cookie: guest=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax
welcome back, ada
```

## Files

- `res.file(path)` answers with a file and takes its `Content-Type` from its extension. It answers 404 when the file doesn't exist.
- `res.fileFrom(dir, name)` does the same for a file inside `dir`. It answers 404 for a name with a `..` part or an absolute name, so a route like `/static/*path` can't send a file from outside `dir`.
- `http.mimeType(name)` gives the type they use, or `application/octet-stream` for an unknown extension.

```iris run
import http, fs, net, str

!void static(http.Request *req, http.Response *res) {
    try res.fileFrom("/tmp/iris-tour-site", req.param("path"))
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e {
        print("server: {e.message}")
    }
}

!void main() {
    try fs.mkdirAll("/tmp/iris-tour-site")
    try fs.writeAll("/tmp/iris-tour-site/style.css", "h1 {{ color: teal }}\n")

    http.Router r
    defer r.free()
    r.get("/static/*path", static)

    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    for name in ["style.css", "missing.js", "../etc/passwd"] {
        scoped String url = format("http://127.0.0.1:{ln.port}/static/{name}")
        scoped http.Reply reply = try http.get(url)
        print("{reply.status} {reply.header("Content-Type")}: {str.trim(reply.body)}")
    }
    print(http.mimeType("logo.SVG"))

    try fs.removeAll("/tmp/iris-tour-site")
}
```

```output
200 text/css; charset=utf-8: h1 { color: teal }
404 text/plain; charset=utf-8: not found
404 text/plain; charset=utf-8: not found
image/svg+xml
```

HTML pages are often made with the `template` module (see [Templates](12-templates.html) and [the library](../std/index.html#template)).

## Checks and logging

`r.use(check)` runs a check before the handler of every request. `r.use(check, "/admin")` runs it only for paths that start with `/admin`. A check that answers the request itself, such as a login check or a rate limit, returns `false`, and the handler isn't called. `r.after(done)` runs after every request, answered or not, for a log.

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

The Router's fields set its limits:

- `timeout` is how long a read or a write may wait for a slow client, 30 seconds by default.
- `maxBody` is the largest body, 16 MiB by default. A larger one gets 413.
- `gzip` compresses text answers of 1 KiB or more for clients that accept it.

```iris
http.Router r
r.timeout = 10s
r.maxBody = 1 << 20
r.gzip = true
```

`r.shutdown()` stops taking new connections and waits for the requests in progress, then `serve` returns. To stop cleanly on Ctrl-C, or when systemd or Docker stop it, a server waits for the signal in its own task and calls `shutdown` (see [Programs and the environment](06-processes.html)).

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

`http.get(url)` and `http.post(url, contentType, body)` send a request and give a `Reply` with its `status`, its `body` and `reply.header(name)`. `http.send(method, url, body, contentType)` sends any method. The reply owns its body and headers, so declare it `scoped`. A gzip body is already unpacked.

An address that isn't `http://` or `https://`, a server that can't be reached, or a reply that isn't HTTP is an error. A reply with status 404 or 500 isn't an error, so the program checks `status`.

Read a JSON reply with `json.decode`:

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

`http.get` and the other functions open a connection for each request and wait at most 30 seconds for each step. A `Client` keeps connections open between requests, so a program that makes many calls to one host is faster with one. Its `timeout` sets how long it waits. Tasks and threads can share one `Client`.

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

Each of them takes `headers`, the request's own as `"Name: value"`, such as an `Authorization`, a `Range` for part of a file, or a `User-Agent` in place of iris's: `c.get(url, ["Range: bytes=0-1023"])`. A header with a line break in it is an error, so a value from outside can't add headers of its own.

A `Reply`'s `headers()` lists every header of the answer. A proxy passes requests on with a `Client` whose `raw` is true: the client adds no `Accept-Encoding` of its own and leaves a gzip body as it came, and a `Host` among the request's headers goes to the server in place of the one from the URL.

## Bodies a part at a time

`c.stream(method, url)` gives a `ReplyStream` as soon as the status and headers have come. `read(buf)` gives the body a part at a time, so a large download goes to a file without being held whole, and `readAll()` the rest of it. `close()` gives the connection back to the `Client` once the whole body was read. `c.begin(method, url, contentType)` is the other way round: an `Upload` to write the body to, a part at a time, sent in chunks unless `length` gives its size, and `finish()` gives the reply as a `ReplyStream`. Both go over HTTP/1.1.

A handler streams its answer with `res.write(data)`: the status and headers go with the first part, each part goes out at once, chunked on HTTP/1.1 and as DATA frames on HTTP/2, and the body ends when the handler returns. A client that reads slowly makes `write` wait, and once it has gone `write` fails, which is when the handler stops. `res.event(data, event, id)` sends a server-sent event, which a browser's `EventSource` gets as one message.

```iris run
import http, net, str

void ticks(http.Request *req, http.Response *res) {
    for i in 1..4 {
        res.event("tick {i}") catch e {
            return
        }
        sleep(10ms)
    }
}

void serve(http.Router *r, net.Listener ln) {
    r.serveOn(ln) catch e { }
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/ticks", ticks)
    ln := try net.listen(0, "127.0.0.1")
    spawn serve(&r, ln)

    http.Client c
    defer c.close()
    scoped String url = format("http://127.0.0.1:{ln.port}/ticks")
    s := try c.stream("GET", url)
    defer s.close()
    print("{s.status} {s.header("Content-Type")}")
    // each event as it comes; a real client handles them in the loop
    String all
    defer all.free()
    u8[256] buf
    for {
        n := try s.read(buf[:])
        if n == 0 {
            break
        }
        all.append(buf[0:n])
    }
    for line in str.lines(all) {
        if line.len > 0 {
            print(line)
        }
    }
}
```

```output
200 text/event-stream
data: tick 1
data: tick 2
data: tick 3
```

## HTTPS

`r.serveTls(8443, "cert.pem", "key.pem")` serves HTTPS with a certificate and its key in PEM files. Clients that offer HTTP/2 get it and the others get HTTP/1.1. The client does HTTPS for `https://` addresses and checks the server's certificate against the ones the system trusts. A `Client`'s `ca` field names a file of certificates to trust instead, for a server with a self-made certificate. TLS comes from OpenSSL, loaded the first time a program uses it.

The library page for [http](../std/index.html#http) lists everything, including WebSockets.
