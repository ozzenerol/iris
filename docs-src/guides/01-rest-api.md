# Build a REST API

This guide builds a small service that stores notes in SQLite and serves them as JSON over HTTP. Each step shows every function and declaration it adds or changes, in full. The last section has the whole program.

The service has five endpoints:

| Method and path | What it does |
|---|---|
| `GET /notes` | lists every note |
| `POST /notes` | adds a note |
| `GET /notes/:id` | shows one note |
| `PUT /notes/:id` | changes a note |
| `DELETE /notes/:id` | removes a note |

You need Iris and the SQLite library. The `curl` commands try each step out.

## A first route

A `Router` sends each request to the function for its method and path. A handler gets the request and the response, and fills in the response. `res.json` sets the body and the `Content-Type` header.

```iris
import http

void listNotes(http.Request *req, http.Response *res) {
    res.json("[]")
}

!void main() {
    http.Router r
    defer r.free()
    r.get("/notes", listNotes)
    try r.serve(8080)
}
```

`r.serve(8080)` answers requests on port 8080 until the program is stopped. It uses every CPU core. Run it and ask for the notes:

```shell
iris run main.iris
curl localhost:8080/notes
```

```output
[]
```

## Storing notes

The notes live in a SQLite table. `@json` lets `json.encode` and `json.decode` work with a struct. `Note` is a row of the table, and `NoteInput` is what a client sends to add a note. `@json(required)` makes `json.decode` fail when the field is missing.

```iris
import http, json, sqlite, time
```

```iris
@json
struct Note {
    i64 id
    String title
    String body
    i64 created
}

@json
struct NoteInput {
    @json(required) String title
    String body
}

sqlite.Db db
```

`sqlite.all[Note]` runs a query and makes a `Note` from each row, matching columns to fields by name. The `defer` block frees the notes when the handler returns.

```iris
!void listNotes(http.Request *req, http.Response *res) {
    notes := try sqlite.all[Note](&db, "SELECT * FROM notes ORDER BY id")
    defer {
        for n in notes {
            n.free()
        }
        notes.free()
    }
    scoped String body = json.encode(notes.items())
    res.json(body)
}
```

A handler can return an error: its type is `!void`. When it fails, the router answers 500 with the error's message. When the body isn't valid JSON, `createNote` answers 400 itself, with the reason; `res.error` sends `{"error": "..."}`.

```iris
!void createNote(http.Request *req, http.Response *res) {
    scoped NoteInput n = json.decode(NoteInput, req.body) catch e {
        res.error(400, e.message)
        return
    }
    try db.exec("INSERT INTO notes (title, body, created) VALUES (?, ?, ?)",
        [.text(n.title), .text(n.body), .int(time.now().unix())])
    res.status = 201
    scoped String location = format("/notes/{db.lastId()}")
    res.header("Location", location)
    scoped String body = format("{{\"id\": {db.lastId()}}}")
    res.json(body)
}
```

`main` opens the database, makes the table if it isn't there, and adds the route for `POST`:

```iris
!void main() {
    db = try sqlite.open("notes.db")
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT, created INTEGER)")

    http.Router r
    defer r.free()
    r.get("/notes", listNotes)
    r.post("/notes", createNote)
    try r.serve(8080)
}
```

```shell
curl -i -d '{"title": "milk", "body": "2 litres"}' localhost:8080/notes
curl -d '{"body": "no title"}' localhost:8080/notes
curl localhost:8080/notes
```

```output
HTTP/1.1 201 Created
Content-Type: application/json
Content-Length: 9
Location: /notes/1
Connection: keep-alive

{"id": 1}
{"error": "the required field \"title\" is missing"}
[{"id":1,"title":"milk","body":"2 litres","created":1792213454}]
```

## One note at a time

A pattern part that starts with `:` matches any one part of the path, and `req.param` gives what it matched. `noteId` turns it into a number, or answers 400:

```iris
import http, json, sqlite, str, time
```

```iris
// The note id in the path, or a 400 reply.
bool noteId(http.Request *req, http.Response *res, i64 *id) {
    *id = str.parseInt(req.param("id")) catch e {
        res.error(400, "the id should be a number")
        return false
    }
    return true
}
```

`sqlite.one[Note]` gives the first row, or fails with `SqlError.noRows` when there is none:

```iris
!void showNote(http.Request *req, http.Response *res) {
    i64 id
    if !noteId(req, res, &id) {
        return
    }
    scoped Note n = sqlite.one[Note](&db, "SELECT * FROM notes WHERE id = ?", [.int(id)]) catch e {
        if e.code == sqlite.SqlError.noRows {
            res.error(404, "no such note")
            return
        }
        return e
    }
    scoped String body = json.encode(n)
    res.json(body)
}
```

`db.changes()` says how many rows the last statement changed, so a note that isn't there gets 404. 204 means the reply has no body.

```iris
!void updateNote(http.Request *req, http.Response *res) {
    i64 id
    if !noteId(req, res, &id) {
        return
    }
    scoped NoteInput n = json.decode(NoteInput, req.body) catch e {
        res.error(400, e.message)
        return
    }
    try db.exec("UPDATE notes SET title = ?, body = ? WHERE id = ?", [.text(n.title), .text(n.body), .int(id)])
    if db.changes() == 0 {
        res.error(404, "no such note")
        return
    }
    res.status = 204
}

!void deleteNote(http.Request *req, http.Response *res) {
    i64 id
    if !noteId(req, res, &id) {
        return
    }
    try db.exec("DELETE FROM notes WHERE id = ?", [.int(id)])
    if db.changes() == 0 {
        res.error(404, "no such note")
        return
    }
    res.status = 204
}
```

```iris
!void main() {
    db = try sqlite.open("notes.db")
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT, created INTEGER)")

    http.Router r
    defer r.free()
    r.get("/notes", listNotes)
    r.post("/notes", createNote)
    r.get("/notes/:id", showNote)
    r.put("/notes/:id", updateNote)
    r.delete("/notes/:id", deleteNote)
    try r.serve(8080)
}
```

```shell
curl -X PUT -d '{"title": "oat milk", "body": "1 litre"}' localhost:8080/notes/1
curl localhost:8080/notes/1
curl -X DELETE localhost:8080/notes/1
curl localhost:8080/notes/1
curl localhost:8080/notes/x
```

```output
{"id":1,"title":"oat milk","body":"1 litre","created":1792213454}
{"error": "no such note"}
{"error": "the id should be a number"}
```

## Settings, a token and a log

The port, the database file and a token come from environment variables. `@env("NOTES")` lets `env.load` fill the struct from `NOTES_PORT`, `NOTES_DB` and `NOTES_TOKEN`. A field with a value keeps it when the variable isn't set; `@env(required)` makes `env.load` fail instead.

```iris
import crypto, env, http, json, log, sqlite, str, time
```

```iris
@env("NOTES")
struct Config {
    u16 port = 8080
    str db = "notes.db"
    @env(required) str token
}
```

```iris
Config config
sqlite.Db db
```

`r.use(auth)` runs `auth` before every handler. When it gives false, the router sends what `auth` put in the response, and the handler doesn't run. `crypto.equal` compares the token in a time that doesn't depend on where the texts differ. `r.after(access)` runs `access` after every request, so each one is logged with its status and how long it took.

```iris
bool auth(http.Request *req, http.Response *res) {
    given := req.header("Authorization")
    if str.startsWith(given, "Bearer ") && crypto.equal(given[7:], config.token) {
        return true
    }
    res.error(401, "a valid token is needed")
    return false
}

void access(http.Request *req, http.Response *res) {
    log.info("{req.method} {req.path} {res.status} {time.clock() - req.received}")
}
```

```iris
!void main() {
    config = try env.load(Config)
    db = try sqlite.open(config.db)
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT, created INTEGER)")

    http.Router r
    defer r.free()
    r.use(auth)
    r.after(access)
    r.get("/notes", listNotes)
    r.post("/notes", createNote)
    r.get("/notes/:id", showNote)
    r.put("/notes/:id", updateNote)
    r.delete("/notes/:id", deleteNote)

    log.info("listening on {config.port}")
    try r.serve(config.port)
}
```

```shell
NOTES_TOKEN=s3cret iris run main.iris
curl localhost:8080/notes
curl -H 'Authorization: Bearer s3cret' localhost:8080/notes
```

```output
{"error": "a valid token is needed"}
[]
```

The server prints:

```output
2026-10-17T05:04:21.849Z INFO listening on 8080
2026-10-17T05:04:22.148Z INFO GET /notes 401 51.112us
2026-10-17T05:04:22.154Z INFO GET /notes 200 137.562us
```

Without the token, the program stops before it serves anything:

```output
error: EnvError.missing: NOTES_TOKEN is needed but isn't set
```

## Stopping cleanly

When the service is stopped with Ctrl-C or `kill`, requests that are being answered should still finish. `signal.watch` stops the signals from ending the program, and `signal.wait` waits for one. `r.shutdown` stops taking connections and waits for the requests in progress, up to 10 seconds; `r.serve` then returns, and the `defer` statements close the database.

```iris
import crypto, env, http, json, log, signal, sqlite, str, time
```

```iris
void stopOnSignal(http.Router *r) {
    s := signal.wait()
    log.info("{s}: stopping")
    r.shutdown()
}
```

```iris
!void main() {
    config = try env.load(Config)
    db = try sqlite.open(config.db)
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT, created INTEGER)")

    http.Router r
    defer r.free()
    r.use(auth)
    r.after(access)
    r.get("/notes", listNotes)
    r.post("/notes", createNote)
    r.get("/notes/:id", showNote)
    r.put("/notes/:id", updateNote)
    r.delete("/notes/:id", deleteNote)

    signal.watch()
    spawn stopOnSignal(&r)
    log.info("listening on {config.port}")
    try r.serve(config.port)
    log.info("stopped")
}
```

## The whole program

```iris
// A notes service: a REST API over SQLite.

import crypto, env, http, json, log, signal, sqlite, str, time

@env("NOTES")
struct Config {
    u16 port = 8080
    str db = "notes.db"
    @env(required) str token
}

@json
struct Note {
    i64 id
    String title
    String body
    i64 created
}

@json
struct NoteInput {
    @json(required) String title
    String body
}

Config config
sqlite.Db db

bool auth(http.Request *req, http.Response *res) {
    given := req.header("Authorization")
    if str.startsWith(given, "Bearer ") && crypto.equal(given[7:], config.token) {
        return true
    }
    res.error(401, "a valid token is needed")
    return false
}

void access(http.Request *req, http.Response *res) {
    log.info("{req.method} {req.path} {res.status} {time.clock() - req.received}")
}

// The note id in the path, or a 400 reply.
bool noteId(http.Request *req, http.Response *res, i64 *id) {
    *id = str.parseInt(req.param("id")) catch e {
        res.error(400, "the id should be a number")
        return false
    }
    return true
}

!void listNotes(http.Request *req, http.Response *res) {
    notes := try sqlite.all[Note](&db, "SELECT * FROM notes ORDER BY id")
    defer {
        for n in notes {
            n.free()
        }
        notes.free()
    }
    scoped String body = json.encode(notes.items())
    res.json(body)
}

!void showNote(http.Request *req, http.Response *res) {
    i64 id
    if !noteId(req, res, &id) {
        return
    }
    scoped Note n = sqlite.one[Note](&db, "SELECT * FROM notes WHERE id = ?", [.int(id)]) catch e {
        if e.code == sqlite.SqlError.noRows {
            res.error(404, "no such note")
            return
        }
        return e
    }
    scoped String body = json.encode(n)
    res.json(body)
}

!void createNote(http.Request *req, http.Response *res) {
    scoped NoteInput n = json.decode(NoteInput, req.body) catch e {
        res.error(400, e.message)
        return
    }
    try db.exec("INSERT INTO notes (title, body, created) VALUES (?, ?, ?)",
        [.text(n.title), .text(n.body), .int(time.now().unix())])
    res.status = 201
    scoped String location = format("/notes/{db.lastId()}")
    res.header("Location", location)
    scoped String body = format("{{\"id\": {db.lastId()}}}")
    res.json(body)
}

!void updateNote(http.Request *req, http.Response *res) {
    i64 id
    if !noteId(req, res, &id) {
        return
    }
    scoped NoteInput n = json.decode(NoteInput, req.body) catch e {
        res.error(400, e.message)
        return
    }
    try db.exec("UPDATE notes SET title = ?, body = ? WHERE id = ?", [.text(n.title), .text(n.body), .int(id)])
    if db.changes() == 0 {
        res.error(404, "no such note")
        return
    }
    res.status = 204
}

!void deleteNote(http.Request *req, http.Response *res) {
    i64 id
    if !noteId(req, res, &id) {
        return
    }
    try db.exec("DELETE FROM notes WHERE id = ?", [.int(id)])
    if db.changes() == 0 {
        res.error(404, "no such note")
        return
    }
    res.status = 204
}

void stopOnSignal(http.Router *r) {
    s := signal.wait()
    log.info("{s}: stopping")
    r.shutdown()
}

!void main() {
    config = try env.load(Config)
    db = try sqlite.open(config.db)
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT, created INTEGER)")

    http.Router r
    defer r.free()
    r.use(auth)
    r.after(access)
    r.get("/notes", listNotes)
    r.post("/notes", createNote)
    r.get("/notes/:id", showNote)
    r.put("/notes/:id", updateNote)
    r.delete("/notes/:id", deleteNote)

    signal.watch()
    spawn stopOnSignal(&r)
    log.info("listening on {config.port}")
    try r.serve(config.port)
    log.info("stopped")
}
```
