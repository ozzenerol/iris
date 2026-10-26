#!/usr/bin/env bash
# A MongoDB server for the mongo tests, on a free port, with a small cache,
# TLS allowed with the test certificates, and the user iris (password
# secret, SCRAM-SHA-256) in the admin database. It runs mongod when there is
# one, else the mongo:8.0 Docker image.
#
#   eval "$(tools/mongo-server.sh start)"   # sets IRIS_MONGO_URL and IRIS_MONGO_DIR
#   tools/mongo-server.sh stop "$IRIS_MONGO_DIR"
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
mongod=$(command -v mongod || true)
docker=""
if [ -z "$mongod" ]; then
  command -v docker >/dev/null && docker image inspect mongo:8.0 >/dev/null 2>&1 && docker=1 || exit 1
fi
dir=$(mktemp -d "${TMPDIR:-/tmp}/iris-mongo.XXXXXX")
chmod 755 "$dir"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
mkdir "$dir/tls"
cat "$root/tests/data/tls/server.pem" "$root/tests/data/tls/server.key" > "$dir/tls/both.pem"
cp "$root/tests/data/tls/ca.pem" "$dir/tls/"
chmod 644 "$dir/tls/"*
opts=(--wiredTigerCacheSizeGB 0.25 --tlsMode allowTLS --tlsCertificateKeyFile /tls/both.pem --tlsCAFile /tls/ca.pem
      --tlsAllowConnectionsWithoutCertificates --setParameter diagnosticDataCollectionEnabled=false)
if [ -n "$docker" ]; then
  docker run -d --rm -m 768m -p "127.0.0.1:$port:27017" -e MONGO_INITDB_ROOT_USERNAME=iris \
    -e MONGO_INITDB_ROOT_PASSWORD=secret -v "$dir/tls:/tls:ro" mongo:8.0 "${opts[@]}" > "$dir/cid" 2>/dev/null ||
    { rm -rf "$dir"; exit 1; }
  # the image starts a server to add the user, stops it and starts the real one
  for i in $(seq 600); do
    [ "$(docker logs "$(cat "$dir/cid")" 2>&1 | grep -c '"Waiting for connections"')" -ge 2 ] && break
    sleep 0.1
  done
else
  # the user is made with mongosh before auth is on, then mongod starts again with it
  command -v mongosh >/dev/null || { rm -rf "$dir"; exit 1; }
  opts=("${opts[@]//\/tls\//$dir/tls/}")
  mkdir "$dir/data"
  start() { "$mongod" --dbpath "$dir/data" --port "$port" --bind_ip 127.0.0.1 --fork --logpath "$dir/log" \
    --pidfilepath "$dir/pid" "${opts[@]}" "$@" >/dev/null; }
  start
  mongosh --quiet --port "$port" admin --eval \
    'db.createUser({user: "iris", pwd: "secret", roles: ["root"], mechanisms: ["SCRAM-SHA-256"]})' >/dev/null
  kill "$(cat "$dir/pid")"
  while kill -0 "$(cat "$dir/pid")" 2>/dev/null; do sleep 0.1; done
  start --auth
fi
echo "export IRIS_MONGO_URL='mongodb://iris:secret@127.0.0.1:$port/iris_test?authSource=admin' IRIS_MONGO_DIR='$dir'"
