#!/usr/bin/env bash
# A MinIO server for the s3 tests, on a free port, keeping its data in a
# folder of its own. It runs the minio program when there is one, else the
# minio/minio Docker image.
#
#   eval "$(tools/minio-server.sh start)"   # sets IRIS_S3_URL, IRIS_S3_KEY, IRIS_S3_SECRET, IRIS_S3_DIR
#   tools/minio-server.sh stop "$IRIS_S3_DIR"
#
# Prints nothing and exits 1 when there is no server to start.
set -euo pipefail
if [ "${1:-start}" = stop ]; then
  dir=$2
  [ -f "$dir/pid" ] && kill "$(cat "$dir/pid")" 2>/dev/null || true
  [ -f "$dir/cid" ] && docker rm -f "$(cat "$dir/cid")" >/dev/null 2>&1 || true
  sleep 0.2
  rm -rf "$dir" 2>/dev/null || true
  exit 0
fi
server=$(command -v minio || true)
docker=""
if [ -z "$server" ]; then
  command -v docker >/dev/null && docker image inspect minio/minio >/dev/null 2>&1 && docker=1 || exit 1
fi
dir=$(mktemp -d "${TMPDIR:-/tmp}/iris-minio.XXXXXX")
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
key=irisadmin
secret=irissecret
if [ -n "$docker" ]; then
  docker run -d --rm -m 512m -p "127.0.0.1:$port:9000" -e MINIO_ROOT_USER=$key -e MINIO_ROOT_PASSWORD=$secret \
    minio/minio server /data > "$dir/cid" 2>/dev/null || { rm -rf "$dir"; exit 1; }
else
  mkdir "$dir/data"
  MINIO_ROOT_USER=$key MINIO_ROOT_PASSWORD=$secret "$server" server --quiet --address "127.0.0.1:$port" \
    --console-address 127.0.0.1:0 "$dir/data" > "$dir/log" 2>&1 &
  echo $! > "$dir/pid"
fi
for i in $(seq 100); do
  curl -sf "http://127.0.0.1:$port/minio/health/ready" >/dev/null 2>&1 && break
  sleep 0.1
done
echo "export IRIS_S3_URL='http://127.0.0.1:$port' IRIS_S3_KEY='$key' IRIS_S3_SECRET='$secret' IRIS_S3_DIR='$dir'"
