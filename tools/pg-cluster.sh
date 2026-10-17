#!/usr/bin/env bash
# A PostgreSQL server for the pg tests, in a folder of its own on a free
# port, with password logins (SCRAM) for the user iris and over TLS with the
# test certificates.
#
#   eval "$(tools/pg-cluster.sh start)"   # sets IRIS_PG_URL and IRIS_PG_DIR
#   tools/pg-cluster.sh stop "$IRIS_PG_DIR"
#
# Prints nothing and exits 1 when there is no PostgreSQL to start.
set -euo pipefail
cmd=${1:-start}
bin=$(ls -d /usr/lib/postgresql/*/bin /opt/homebrew/opt/postgresql@*/bin /usr/local/opt/postgresql@*/bin 2>/dev/null | sort -V | tail -1 || true)
[ -n "$bin" ] && [ -x "$bin/initdb" ] || { command -v initdb >/dev/null && bin=$(dirname "$(command -v initdb)"); } || exit 1
# PostgreSQL won't run as root: then it runs as the postgres user
as=()
if [ "$(id -u)" = 0 ]; then
  id postgres >/dev/null 2>&1 || exit 1
  as=(runuser -u postgres --)
fi
if [ "$cmd" = stop ]; then
  dir=$2
  "${as[@]}" "$bin/pg_ctl" -D "$dir/data" -m immediate stop >/dev/null 2>&1 || true
  rm -rf "$dir"
  exit 0
fi
root=$(cd "$(dirname "$0")/.." && pwd)
dir=$(mktemp -d "${TMPDIR:-/tmp}/iris-pg.XXXXXX")
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
printf 'secret\n' > "$dir/pw"
[ ${#as[@]} -gt 0 ] && chown postgres "$dir" "$dir/pw"
"${as[@]}" "$bin/initdb" -D "$dir/data" -U iris --pwfile="$dir/pw" -A scram-sha-256 -E UTF8 >/dev/null
cp "$root/tests/data/tls/server.pem" "$dir/data/server.crt"
cp "$root/tests/data/tls/server.key" "$dir/data/server.key"
chmod 600 "$dir/data/server.key"
[ ${#as[@]} -gt 0 ] && chown postgres "$dir/data/server.crt" "$dir/data/server.key"
cat >> "$dir/data/postgresql.conf" <<CONF
port = $port
listen_addresses = '127.0.0.1'
unix_socket_directories = '$dir'
ssl = on
fsync = off
CONF
"${as[@]}" "$bin/pg_ctl" -D "$dir/data" -l "$dir/log" -w start >/dev/null
PGPASSWORD=secret "$bin/psql" -h 127.0.0.1 -p "$port" -U iris -d postgres -q -c "CREATE DATABASE iris_test" >/dev/null
# users that log in with MD5 and with the password as it is, for those logins
PGPASSWORD=secret "$bin/psql" -h 127.0.0.1 -p "$port" -U iris -d iris_test -q >/dev/null <<SQL
SET password_encryption = 'md5';
CREATE ROLE md5user LOGIN PASSWORD 'md5pass';
SET password_encryption = 'scram-sha-256';
CREATE ROLE plainuser LOGIN PASSWORD 'plainpass';
GRANT ALL ON SCHEMA public TO md5user, plainuser;
SQL
{ echo "host all md5user 127.0.0.1/32 md5"; echo "host all plainuser 127.0.0.1/32 password"; cat "$dir/data/pg_hba.conf"; } > "$dir/hba"
mv "$dir/hba" "$dir/data/pg_hba.conf"
[ ${#as[@]} -gt 0 ] && chown postgres "$dir/data/pg_hba.conf"
"${as[@]}" "$bin/pg_ctl" -D "$dir/data" reload >/dev/null
sleep 0.2
echo "export IRIS_PG_URL='postgres://iris:secret@127.0.0.1:$port/iris_test' IRIS_PG_DIR='$dir'"
