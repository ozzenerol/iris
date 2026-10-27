# Migrations

A schema changes as a program grows: new tables, new columns, new indexes. The `migrate` module keeps those changes as numbered migrations, runs the ones a database hasn't had, and remembers which ran. It works with the `sqlite`, `pg`, `mysql` and `mongo` modules of the earlier chapters.

## A migration

A `migrate.Migration` has a version, a name, the change (`up`) and the way back (`down`). The versions in a list go from the lowest to the highest, and are above 0.

A store wraps a database: `migrate.Sqlite{db: &db}`, `migrate.Pg{conn: &c}`, `migrate.Mysql{conn: &c}` or `migrate.Mongo{client: &c}`. `migrate.up` runs every migration the database hasn't had and gives how many ran. Run again, it runs none:

```iris run
import migrate, sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    store := migrate.Sqlite{db: &db}

    list := [
        migrate.Migration{version: 1, name: "users",
            up: "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL);",
            down: "DROP TABLE users;"},
        migrate.Migration{version: 2, name: "email",
            up: "ALTER TABLE users ADD COLUMN email TEXT;",
            down: "ALTER TABLE users DROP COLUMN email;"},
    ]

    print("ran {try migrate.up(&store, list)}")
    print("ran {try migrate.up(&store, list)}")
    print("version {try migrate.version(&store)}")

    try db.exec("INSERT INTO users (name, email) VALUES (?, ?)", [.text("Ada"), .text("ada@example.com")])
}
```

```output
ran 2
ran 0
version 2
```

The versions that ran go in a table named `schema_migrations`, with the name, the time, and a checksum of the up. A store's `table` field names another table.

## Going back

`migrate.down` undoes the last migration that ran, or the last few with a number after the list. `migrate.to` goes to a version, up or down, and 0 undoes them all. `migrate.status` gives each migration and whether it ran:

```iris run
import migrate, sqlite

!void main() {
    db := try sqlite.open(":memory:")
    defer db.close()
    store := migrate.Sqlite{db: &db}

    list := [
        migrate.Migration{version: 1, name: "users", up: "CREATE TABLE users (id INTEGER PRIMARY KEY);", down: "DROP TABLE users;"},
        migrate.Migration{version: 2, name: "orders", up: "CREATE TABLE orders (id INTEGER PRIMARY KEY);", down: "DROP TABLE orders;"},
        migrate.Migration{version: 3, name: "items", up: "CREATE TABLE items (id INTEGER PRIMARY KEY);", down: "DROP TABLE items;"},
    ]
    _ = try migrate.up(&store, list)

    print("undone {try migrate.down(&store, list)}")
    print("undone {try migrate.to(&store, list, 1)}")

    scoped st := try migrate.status(&store, list)
    for s in st {
        print("{s.version} {s.name} {s.applied}")
    }
}
```

```output
undone 1
undone 1
1 users true
2 orders false
3 items false
```

A migration with an empty `down` can't be undone, and `down` fails on it with `MigrateError.noDown`.

## Files

`migrate.load` reads the migrations in a folder. Each is a file named `VERSION_NAME.up.EXT`, with an optional `VERSION_NAME.down.EXT` next to it. Other files are skipped.

```
migrations/
    0001_users.up.sql
    0001_users.down.sql
    0002_email.up.sql
    0002_email.down.sql
```

`load` gives a `migrate.Files`, whose `items` is the list and whose `free` gives the text back. A program usually runs them when it starts:

```iris
import migrate, pg, os

!void main() {
    db := try pg.connect(os.env("PG_URL") catch "postgres://ada:secret@localhost/shop")
    defer db.close()
    store := migrate.Pg{conn: &db}

    files := try migrate.load("migrations")
    defer files.free()
    n := try migrate.up(&store, files.items())
    print("{n} migrations ran")
}
```

A file with a bad name, such as `1-users.up.sql`, fails with `MigrateError.invalid`.

## Mistakes it catches

- A migration that ran and was edited since fails `up` with `MigrateError.changed`. Write a new migration instead.
- A database with a version the list lacks fails with `MigrateError.unknown`, as when an older build of the program starts after a newer one.
- A list out of order fails with `MigrateError.invalid`.
- A migration that fails comes back with its version and name before the database's message: `migration 3 orders: ...`.

## Each database

- PostgreSQL runs each migration in a transaction, so one that fails leaves nothing behind. An advisory lock makes programs that start together wait for each other, and the migrations run once.
- MySQL takes a lock with `GET_LOCK`, waiting up to the store's `wait` (10 minutes). MySQL ends a transaction at each `CREATE`, `ALTER` and `DROP`, so a migration that fails part way keeps the statements before the failure. Small migrations, one change each, make that easy to fix.
- SQLite runs each migration in `BEGIN IMMEDIATE`, which keeps other writers out, and skips one that another program ran in the meantime.
- MongoDB has no SQL, so a migration is a JSON array of commands, each run as `Client.command` runs it. The files end in `.json`. The lock is a document in `schema_migrations_lock`, and one older than the store's `stale` (1 hour), left by a program that died, is taken over.

```
[
    {"create": "users"},
    {"createIndexes": "users", "indexes": [{"key": {"email": 1}, "name": "email", "unique": true}]}
]
```

## Another database

`up`, `down`, `to` and `status` take any store with the methods of `migrate.Store`: `lock`, `unlock`, `applied`, `apply` and `revert`. A struct with those methods adds a database the module doesn't know.
