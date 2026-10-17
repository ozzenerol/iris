#!/usr/bin/env bash
# A Redis (or Valkey) server for the redis tests, on free ports, with a
# password, without saving to disk, and with TLS on a second port when the
# server has it.
#
#   eval "$(tools/redis-server.sh start)"   # sets IRIS_REDIS_URL, IRIS_REDIS_TLS_URL, IRIS_REDIS_DIR
#   tools/redis-server.sh stop "$IRIS_REDIS_DIR"
#
# Prints nothing and exits 1 when there is no server to start.
set -euo pipefail
server=$(command -v redis-server || command -v valkey-server || true)
[ -n "$server" ] || exit 1
if [ "${1:-start}" = stop ]; then
  dir=$2
  [ -f "$dir/pid" ] && kill "$(cat "$dir/pid")" 2>/dev/null || true
  sleep 0.2
  rm -rf "$dir"
  exit 0
fi
root=$(cd "$(dirname "$0")/.." && pwd)
dir=$(mktemp -d "${TMPDIR:-/tmp}/iris-redis.XXXXXX")
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
port=$(free_port)
tls=$(free_port)
args=(--port "$port" --bind 127.0.0.1 --requirepass secret --save "" --appendonly no --dir "$dir"
      --daemonize yes --pidfile "$dir/pid" --logfile "$dir/log")
tlsArgs=(--tls-port "$tls" --tls-cert-file "$root/tests/data/tls/server.pem" --tls-key-file "$root/tests/data/tls/server.key"
         --tls-ca-cert-file "$root/tests/data/tls/ca.pem" --tls-auth-clients no)
if "$server" "${args[@]}" "${tlsArgs[@]}" 2>/dev/null; then
  tlsUrl="rediss://:secret@localhost:$tls?ca=tests/data/tls/ca.pem"
else
  "$server" "${args[@]}"
  tlsUrl=""
fi
for i in $(seq 50); do [ -f "$dir/pid" ] && break; sleep 0.05; done
sleep 0.2
echo "export IRIS_REDIS_URL='redis://:secret@127.0.0.1:$port' IRIS_REDIS_TLS_URL='$tlsUrl' IRIS_REDIS_DIR='$dir'"
