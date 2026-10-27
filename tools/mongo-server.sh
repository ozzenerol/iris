#!/usr/bin/env bash
# A MongoDB server for the mongo tests, on a free port, with a small cache,
# TLS allowed with the test certificates, and the user iris (password
# secret, SCRAM-SHA-256) in the admin database. It is a replica set of one
# member, rs0, so transactions and change streams work. It runs mongod when
# there is one, else the mongo:8.0 Docker image.
#
#   eval "$(tools/mongo-server.sh start)"   # sets IRIS_MONGO_URL, IRIS_MONGO_RS_URL and IRIS_MONGO_DIR
#
# IRIS_MONGO_URL talks to the server directly; IRIS_MONGO_RS_URL names the
# replica set, and a second host where nothing listens.
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
openssl rand -base64 48 > "$dir/tls/key"
chmod 644 "$dir/tls/"*
opts=(--wiredTigerCacheSizeGB 0.25 --tlsMode allowTLS --tlsCertificateKeyFile /tls/both.pem --tlsCAFile /tls/ca.pem
      --tlsAllowConnectionsWithoutCertificates --setParameter diagnosticDataCollectionEnabled=false
      --replSet rs0 --port "$port")
initiate="rs.initiate({_id: 'rs0', members: [{_id: 0, host: '127.0.0.1:$port'}]})"
primary="while (!db.hello().isWritablePrimary) sleep(100)"
if [ -n "$docker" ]; then
  # the key file must belong to the mongodb user and be read by it alone
  docker run -d --rm -m 768m -p "127.0.0.1:$port:$port" -e MONGO_INITDB_ROOT_USERNAME=iris \
    -e MONGO_INITDB_ROOT_PASSWORD=secret -v "$dir/tls:/tls:ro" --entrypoint bash mongo:8.0 -c \
    'cp /tls/key /tmp/key && chown mongodb /tmp/key && chmod 400 /tmp/key && exec docker-entrypoint.sh "$@"' \
    mongod mongod "${opts[@]}" --keyFile /tmp/key > "$dir/cid" 2>/dev/null || { rm -rf "$dir"; exit 1; }
  # the image starts a server to add the user, stops it and starts the real one
  for i in $(seq 600); do
    [ "$(docker logs "$(cat "$dir/cid")" 2>&1 | grep -c '"Waiting for connections"')" -ge 2 ] && break
    sleep 0.1
  done
  docker exec "$(cat "$dir/cid")" mongosh --quiet --port "$port" -u iris -p secret --authenticationDatabase admin \
    --eval "$initiate; $primary" >/dev/null || { docker rm -f "$(cat "$dir/cid")" >/dev/null; rm -rf "$dir"; exit 1; }
else
  # the user is made with mongosh before auth is on, then mongod starts again with it
  command -v mongosh >/dev/null || { rm -rf "$dir"; exit 1; }
  opts=("${opts[@]//\/tls\//$dir/tls/}")
  mkdir "$dir/data"
  chmod 400 "$dir/tls/key"
  start() { "$mongod" --dbpath "$dir/data" --bind_ip 127.0.0.1 --fork --logpath "$dir/log" \
    --pidfilepath "$dir/pid" "${opts[@]}" "$@" >/dev/null; }
  # the set starts and the user is made before auth is on, then mongod
  # starts again with it
  start
  mongosh --quiet --port "$port" admin --eval "$initiate; $primary;"\
'db.createUser({user: "iris", pwd: "secret", roles: ["root"], mechanisms: ["SCRAM-SHA-256"]})' >/dev/null
  kill "$(cat "$dir/pid")"
  while kill -0 "$(cat "$dir/pid")" 2>/dev/null; do sleep 0.1; done
  start --keyFile "$dir/tls/key"
  mongosh --quiet --port "$port" -u iris -p secret --authenticationDatabase admin --eval "$primary" >/dev/null
fi
echo "export IRIS_MONGO_URL='mongodb://iris:secret@127.0.0.1:$port/iris_test?authSource=admin'" \
  "IRIS_MONGO_RS_URL='mongodb://iris:secret@127.0.0.1:$port,127.0.0.1:1/iris_test?authSource=admin&replicaSet=rs0'" \
  "IRIS_MONGO_DIR='$dir'"
