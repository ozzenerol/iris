# Build a REST API

This guide builds a small service from an empty folder: it keeps notes in a SQLite database and serves them as JSON over HTTP. Each step ends with a program that runs, and the commands to try it.

The finished service has five endpoints:

| Method and path | What it does |
|---|---|
| `GET /notes` | lists every note |
| `POST /notes` | adds a note |
| `GET /notes/:id` | shows one note |
| `PUT /notes/:id` | changes a note |
| `DELETE /notes/:id` | removes a note |

## Before you start

You need three things:

- Iris. `iris version` prints its version.
- The SQLite library. macOS has it. On Debian and Ubuntu it is the `libsqlite3-0` package, which most systems have already; `sudo apt install libsqlite3-0` installs it.
- `curl`, to send requests to the service. `curl --version` checks it.

Make a folder for the service and go into it:

```shell
mkdir notes
cd notes
```

Everything in this guide happens in this folder. The program is one file, `main.iris`, which each step changes. You'll use two terminals: one runs the service, the other sends it requests.

## A first route

Create `main.iris` with this:

```iris
// A notes service: a REST API over SQLite.
```

```iris
import http
```

A `Router` sends each request to the function for its method and path. Such a function is a handler: it gets the request and the response, and fills in the response. `res.json` sets the body and the `Content-Type` header.

```iris
void listNotes(http.Request *req, http.Response *res) {
    res.json("[]")
}
```

`main` makes the router, adds the route, and serves. `r.serve(8080)` answers requests on port 8080 until the program is stopped. `defer r.free()` frees the router when `main` ends.

```iris
!void main() {
    http.Router r
    defer r.free()
    r.get("/notes", listNotes)
    try r.serve(8080)
}
```

```iris step
// A notes service: a REST API over SQLite.

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

Run it in the first terminal. It keeps running, answering requests:

```shell start
iris run main.iris
```

In the second terminal, ask for the notes:

```shell try
curl -s localhost:8080/notes
```

```output
[]
```

Stop the service with Ctrl-C in the first terminal. Do this at the end of every step, and start it again with the new code at the next.

## The notes table

The notes live in a SQLite table. The program opens the database file when it starts, and makes the table if it isn't there yet.

The import line gets `json` and `sqlite`:

```iris
import http, json, sqlite
```

`Note` is a row of the table. `@json` lets `json.encode` turn it into JSON, and lets `sqlite.all` fill it from a row, matching columns to fields by name. `db` is the open database, kept in a global so every handler can use it.

```iris
@json
struct Note {
    i64 id
    String title
    String body
    i64 created
}
```

```iris
sqlite.Db db
```

`main` opens `notes.db`, which SQLite makes when it isn't there, and makes the `notes` table with `CREATE TABLE IF NOT EXISTS`, so the second run leaves the table as it is. `defer db.close()` closes the database when `main` ends. All of this happens before the router is made, so the table exists before any request reads it.

```iris
!void main() {
    db = try sqlite.open("notes.db")
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT, created INTEGER)")

    http.Router r
    defer r.free()
    r.get("/notes", listNotes)
    try r.serve(8080)
}
```

`listNotes` now reads the table. `sqlite.all[Note]` runs the query and makes a `Note` from each row. The handler can fail now, so its type is `!void`: when `try` passes an error on, the router answers 500 with the error's message. The `defer` block frees the notes when the handler returns, and `json.encode` turns them into a JSON array.

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

```iris step
// A notes service: a REST API over SQLite.

import http, json, sqlite

@json
struct Note {
    i64 id
    String title
    String body
    i64 created
}

sqlite.Db db

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

!void main() {
    db = try sqlite.open("notes.db")
    defer db.close()
    try db.exec("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT, created INTEGER)")

    http.Router r
    defer r.free()
    r.get("/notes", listNotes)
    try r.serve(8080)
}
```

Start it in the first terminal:

```shell start
iris run main.iris
```

The table is empty, so the list is too, and the folder now has the database file:

```shell try
curl -s localhost:8080/notes
echo
ls
```

```output
[]
main.iris
notes.db
```

## Adding notes

`NoteInput` is what a client sends to add a note. `@json(required)` makes `json.decode` fail when `title` is missing.

```iris
@json
struct NoteInput {
    @json(required) String title
    String body
}
```

`createNote` reads the body into a `NoteInput`. When the body isn't valid JSON, or has no title, it answers 400 itself with the reason; `res.error` sends `{"error": "..."}`. Otherwise it adds the row, with the time from `time.now()`, and answers 201 with the new note's id. The values go in the `?` places of the SQL, so a title can't change what the SQL means.

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

`time` joins the imports, and `main` adds the route for `POST /notes`:

```iris
import http, json, sqlite, time
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
    try r.serve(8080)
}
```

```iris step
// A notes service: a REST API over SQLite.

import http, json, sqlite, time

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

```shell start
iris run main.iris
```

Add a note, try one without a title, and list them. `-i` makes curl show the reply's status and headers:

```shell try
curl -s -i -d '{"title": "milk", "body": "2 litres"}' localhost:8080/notes
echo
curl -s -d '{"body": "no title"}' localhost:8080/notes
echo
curl -s localhost:8080/notes
```

```output
HTTP/1.1 201 Created
Content-Type: application/json
Content-Length: 9
Location: /notes/1
Connection: keep-alive

{"id": 1}
{"error": "the required field \"title\" is missing"}
[{"id":1,"title":"milk","body":"2 litres","created":…}]
```

`created` is the time the note was added, in seconds since 1970.

## One note at a time

A pattern part that starts with `:` matches any one part of the path, and `req.param` gives what it matched. `noteId` turns it into a number with `str.parseInt`, or answers 400. It gives `false` when it answered, so the handler stops there.

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

`showNote` gives one note. `sqlite.one[Note]` gives the first row, or fails with `SqlError.noRows` when there is none, which becomes a 404. Any other error is passed on with `return e`.

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

`updateNote` and `deleteNote` change a row. `db.changes()` says how many rows the last statement changed, so a note that isn't there gets 404. 204 means the reply has no body.

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
```

```iris
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

`main` adds the three routes:

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

```iris step
// A notes service: a REST API over SQLite.

import http, json, sqlite, str, time

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

```shell start
iris run main.iris
```

The note added in the last step is still in `notes.db`. Change it, show it, delete it, and ask for it again:

```shell try
curl -s -X PUT -d '{"title": "oat milk", "body": "1 litre"}' localhost:8080/notes/1
curl -s localhost:8080/notes/1
echo
curl -s -X DELETE localhost:8080/notes/1
curl -s localhost:8080/notes/1
echo
curl -s localhost:8080/notes/x
```

```output
{"id":1,"title":"oat milk","body":"1 litre","created":…}
{"error": "no such note"}
{"error": "the id should be a number"}
```

## Settings, a token and a log

The port, the database file and a token now come from environment variables. `@env("NOTES")` lets `env.load` fill the struct from `NOTES_PORT`, `NOTES_DB` and `NOTES_TOKEN`. A field with a value keeps it when the variable isn't set; `@env(required)` makes `env.load` fail instead.

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

`config` is a global next to `db`:

```iris
Config config
sqlite.Db db
```

`r.use(auth)` runs `auth` before every handler. When it gives false, the router sends what `auth` put in the response, and the handler doesn't run. `crypto.equal` compares the token in a time that doesn't depend on where the texts differ, so the token can't be guessed a letter at a time.

```iris
bool auth(http.Request *req, http.Response *res) {
    given := req.header("Authorization")
    if str.startsWith(given, "Bearer ") && crypto.equal(given[7:], config.token) {
        return true
    }
    res.error(401, "a valid token is needed")
    return false
}
```

`r.after(access)` runs `access` after every request, so each one is logged with its status and how long it took. `log.info` writes a line to stderr with the time.

```iris
void access(http.Request *req, http.Response *res) {
    log.info("{req.method} {req.path} {res.status} {time.clock() - req.received}")
}
```

`main` loads the settings first, opens the database the settings name, and adds `auth` and `access`:

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

```iris step
// A notes service: a REST API over SQLite.

import crypto, env, http, json, log, sqlite, str, time

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

Start it with a token:

```shell start
NOTES_TOKEN=s3cret iris run main.iris
```

A request without the token is turned away; with it, the list comes back:

```shell try
curl -s localhost:8080/notes
echo
curl -s -H 'Authorization: Bearer s3cret' localhost:8080/notes
```

```output
{"error": "a valid token is needed"}
[]
```

The first terminal shows the log:

```output
2026-10-17T05:04:21.849Z INFO listening on 8080
2026-10-17T05:04:22.148Z INFO GET /notes 401 51.112us
2026-10-17T05:04:22.154Z INFO GET /notes 200 137.562us
```

Without the token, the program stops before it serves anything. It prints the error and where it came from, and exits with status 1:

```shell try
iris run main.iris
```

```output
error: EnvError.missing: NOTES_TOKEN is needed but isn't set
  at loadInto  std/env/env.iris:…
  at main      main.iris:…
```

## Stopping cleanly

So far Ctrl-C ends the service at once, even in the middle of a request. `signal.watch` stops the signals from ending the program, and `signal.wait` waits for one. `stopOnSignal` waits in a task of its own; `r.shutdown` then stops taking connections and waits for the requests in progress, up to 10 seconds. `r.serve` returns, and the `defer` statements close the database.

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

`main` starts `stopOnSignal` before it serves:

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

```iris step
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

```shell start
NOTES_TOKEN=s3cret iris run main.iris
```

```shell try
curl -s -H 'Authorization: Bearer s3cret' localhost:8080/notes
```

```output
[]
```

Now Ctrl-C in the first terminal ends it cleanly:

```output
2026-10-17T05:09:40.112Z INFO listening on 8080
^C2026-10-17T05:09:44.530Z INFO interrupt: stopping
2026-10-17T05:09:44.531Z INFO stopped
```

## The whole program

```iris whole
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

## Next

To run it for real, build it once with optimisations and start the program it makes:

```shell
iris build --mode=release-safe main.iris -o notes
NOTES_TOKEN=s3cret ./notes
```

The [http library page](../std/index.html#http) has the rest of the Router: limits, gzip, HTTPS and WebSockets. The [sqlite page](../std/index.html#sqlite) has transactions.
