#!/usr/bin/env bash
# A MySQL server for the mysql tests, on a free port, with the test
# certificates for TLS, a small buffer pool, and four users: iris
# (caching_sha2_password), native (mysql_native_password), tlsonly, who
# must log in over TLS, and fresh, whose password the server hasn't cached.
# It runs mysqld when there is one, else the mysql:8.4 Docker image.
#
#   eval "$(tools/mysql-server.sh start)"   # sets IRIS_MYSQL_URL and IRIS_MYSQL_DIR
#   tools/mysql-server.sh stop "$IRIS_MYSQL_DIR"
#
# Prints nothing and exits 1 when there is no server to start.
set -euo pipefail
if [ "${1:-start}" = stop ]; then
  dir=$2
  [ -f "$dir/pid" ] && kill "$(cat "$dir/pid")" 2>/dev/null || true
  [ -f "$dir/cid" ] && docker rm -f "$(cat "$dir/cid")" >/dev/null 2>&1 || true
  sleep 0.3
  rm -rf "$dir" 2>/dev/null || true
  exit 0
fi
root=$(cd "$(dirname "$0")/.." && pwd)
mysqld=$(command -v mysqld || ls /usr/sbin/mysqld 2>/dev/null || true)
# MariaDB's mysqld has no caching_sha2_password: only MySQL's will do
if [ -n "$mysqld" ] && "$mysqld" --version 2>/dev/null | grep -qi mariadb; then mysqld=""; fi
docker=""
if [ -z "$mysqld" ]; then
  command -v docker >/dev/null && docker image inspect mysql:8.4 >/dev/null 2>&1 && docker=1 || exit 1
fi
dir=$(mktemp -d "${TMPDIR:-/tmp}/iris-mysql.XXXXXX")
chmod 755 "$dir"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
mkdir "$dir/tls"
cp "$root/tests/data/tls/ca.pem" "$root/tests/data/tls/server.pem" "$root/tests/data/tls/server.key" "$dir/tls/"
chmod 644 "$dir/tls/"*
cat > "$dir/init.sql" <<SQL
CREATE DATABASE iris_test;
CREATE USER 'iris'@'%' IDENTIFIED WITH caching_sha2_password BY 'secret';
CREATE USER 'native'@'%' IDENTIFIED WITH mysql_native_password BY 'nativepass';
CREATE USER 'tlsonly'@'%' IDENTIFIED WITH caching_sha2_password BY 'tlspass' REQUIRE SSL;
CREATE USER 'fresh'@'%' IDENTIFIED WITH caching_sha2_password BY 'freshpass';
GRANT ALL ON iris_test.* TO 'iris'@'%', 'native'@'%', 'tlsonly'@'%', 'fresh'@'%';
SQL
opts=(--innodb-buffer-pool-size=64M --performance-schema=OFF --mysql-native-password=ON
      --skip-log-bin --innodb-log-buffer-size=4M --max-connections=100
      --ssl-ca=/tls/ca.pem --ssl-cert=/tls/server.pem --ssl-key=/tls/server.key)
if [ -n "$docker" ]; then
  docker run -d --rm -m 768m -p "127.0.0.1:$port:3306" -e MYSQL_ROOT_PASSWORD=rootpass \
    -v "$dir/tls:/tls:ro" -v "$dir/init.sql:/docker-entrypoint-initdb.d/init.sql:ro" \
    mysql:8.4 "${opts[@]}" > "$dir/cid" 2>/dev/null || { rm -rf "$dir"; exit 1; }
  # the image starts a server to run init.sql, stops it and starts the real one
  for i in $(seq 600); do
    docker logs "$(cat "$dir/cid")" 2>&1 | grep -q "ready for connections.*port: 3306" && break
    sleep 0.1
  done
else
  opts=("${opts[@]//\/tls\//$dir/tls/}")
  "$mysqld" --no-defaults --initialize-insecure --datadir="$dir/data" >/dev/null 2>&1
  "$mysqld" --no-defaults --datadir="$dir/data" --port="$port" --bind-address=127.0.0.1 \
    --socket="$dir/sock" --mysqlx=OFF --pid-file="$dir/pid" --init-file="$dir/init.sql" "${opts[@]}" > "$dir/log" 2>&1 &
  for i in $(seq 300); do grep -q "ready for connections" "$dir/log" 2>/dev/null && break; sleep 0.1; done
fi
python3 - "$port" <<'PY' || { "$0" stop "$dir"; exit 1; }
import socket, sys, time
for _ in range(100):
    try:
        socket.create_connection(("127.0.0.1", int(sys.argv[1])), 1).recv(4)
        sys.exit(0)
    except OSError:
        time.sleep(0.1)
sys.exit(1)
PY
echo "export IRIS_MYSQL_URL='mysql://iris:secret@127.0.0.1:$port/iris_test?allowPublicKeyRetrieval=true' IRIS_MYSQL_DIR='$dir'"
