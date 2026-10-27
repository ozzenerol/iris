## Running migrations

A store wraps a connection: `migrate.Pg{conn: &c}`, `migrate.Mysql{conn: &c}`, `migrate.Sqlite{db: &db}` or `migrate.Mongo{client: &c}`. `migrate.up` runs the migrations that haven't run, `migrate.down` undoes the last ones, `migrate.to` goes to a version, and `migrate.status` says which ran:

```iris
import migrate, pg

!void main() {
    db := try pg.connect("postgres://ada:secret@localhost/shop")
    defer db.close()
    store := migrate.Pg{conn: &db}

    files := try migrate.load("migrations")
    defer files.free()
    n := try migrate.up(&store, files.items())
    print("{n} ran")
}
```

`migrate.load` reads files named `VERSION_NAME.up.EXT` and `VERSION_NAME.down.EXT`, such as `0001_users.up.sql`. A list written in code works the same way:

```iris
list := [
    migrate.Migration{version: 1, name: "users", up: "CREATE TABLE users (id bigint PRIMARY KEY, name text NOT NULL);", down: "DROP TABLE users;"},
]
```

## What is kept

The versions that ran go in a table, `schema_migrations` unless the store's `table` names another, with a checksum of each up. `up` fails with `MigrateError.changed` when a migration that ran was edited since, and with `MigrateError.unknown` when the database has a version the list lacks.

## Locks and transactions

On PostgreSQL a migration runs in a transaction, and an advisory lock keeps two programs from running the same migrations. MySQL uses `GET_LOCK`, but ends a transaction at each `CREATE`, `ALTER` or `DROP`, so a migration that fails part way keeps the statements before the failure. On SQLite each migration runs in `BEGIN IMMEDIATE`. On MongoDB a migration is a JSON array of commands, and the lock is a document in `schema_migrations_lock`.
