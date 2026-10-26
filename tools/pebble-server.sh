#!/usr/bin/env bash
# Pebble, Let's Encrypt's ACME server for tests, with pebble-challtestsrv as
# its DNS server, on free ports. Every name resolves to 127.0.0.1, so
# Pebble checks http-01 answers on this machine, and TXT records for dns-01
# are set through challtestsrv's API. It uses the pebble and
# pebble-challtestsrv programs, else the ghcr.io/letsencrypt Docker images
# when they are there.
#
#   eval "$(tools/pebble-server.sh start)"   # sets IRIS_ACME_URL, IRIS_ACME_CA, IRIS_ACME_HTTP_PORT,
#                                             # IRIS_ACME_DNS_API and IRIS_ACME_DIR
#   tools/pebble-server.sh stop "$IRIS_ACME_DIR"
#
# Prints nothing and exits 1 when there is no server to start.
set -euo pipefail
if [ "${1:-start}" = stop ]; then
  dir=$2
  if [ -f "$dir/containers" ]; then
    docker rm -f $(cat "$dir/containers") >/dev/null 2>&1 || true
  fi
  for f in "$dir"/*.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null || true; done
  rm -rf "$dir"
  exit 0
fi
PEBBLE=ghcr.io/letsencrypt/pebble:latest
CHALL=ghcr.io/letsencrypt/pebble-challtestsrv:latest
mode=""
if command -v pebble >/dev/null && command -v pebble-challtestsrv >/dev/null; then
  mode=local
elif command -v docker >/dev/null && docker image inspect "$PEBBLE" >/dev/null 2>&1 &&
     docker image inspect "$CHALL" >/dev/null 2>&1; then
  mode=docker
fi
[ -n "$mode" ] || exit 1
dir=$(mktemp -d "${TMPDIR:-/tmp}/iris-acme.XXXXXX")
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
acme=$(free_port); mgmt=$(free_port); http=$(free_port); tls=$(free_port); dns=$(free_port); api=$(free_port)
if [ $mode = docker ]; then
  id=$(docker create "$PEBBLE")
  docker cp "$id:/test/certs" "$dir/certs" >/dev/null
  docker rm "$id" >/dev/null
else
  certs=$(dirname "$(command -v pebble)")/../share/pebble/test/certs
  [ -d "${PEBBLE_CERTS:-$certs}" ] || { rm -rf "$dir"; exit 1; }
  cp -r "${PEBBLE_CERTS:-$certs}" "$dir/certs"
fi
cat > "$dir/pebble.json" <<JSON
{"pebble": {"listenAddress": "127.0.0.1:$acme", "managementListenAddress": "127.0.0.1:$mgmt",
  "certificate": "/work/certs/localhost/cert.pem", "privateKey": "/work/certs/localhost/key.pem",
  "httpPort": $http, "tlsPort": $tls, "ocspResponderURL": "", "externalAccountBindingRequired": false,
  "domainBlocklist": ["blocked-domain.example"], "retryAfter": {"authz": 1, "order": 1}}}
JSON
chall=(-defaultIPv6 "" -defaultIPv4 127.0.0.1 -http01 "" -https01 "" -tlsalpn01 "" -doh ""
       -dnsserver "127.0.0.1:$dns" -management "127.0.0.1:$api")
if [ $mode = docker ]; then
  name=iris-acme-$$
  docker run -d --name "$name-dns" --network host -m 128m "$CHALL" "${chall[@]}" >/dev/null
  docker run -d --name "$name-ca" --network host -m 256m -e PEBBLE_VA_NOSLEEP=1 -v "$dir:/work:ro" "$PEBBLE" \
    -config /work/pebble.json -dnsserver "127.0.0.1:$dns" >/dev/null
  echo "$name-dns $name-ca" > "$dir/containers"
else
  sed -i "s|/work/|$dir/|g" "$dir/pebble.json"
  pebble-challtestsrv "${chall[@]}" >"$dir/dns.log" 2>&1 & echo $! > "$dir/dns.pid"
  PEBBLE_VA_NOSLEEP=1 pebble -config "$dir/pebble.json" -dnsserver "127.0.0.1:$dns" >"$dir/ca.log" 2>&1 &
  echo $! > "$dir/ca.pid"
fi
for i in $(seq 100); do
  curl -sk "https://127.0.0.1:$acme/dir" >/dev/null 2>&1 && break
  sleep 0.1
done
echo "export IRIS_ACME_URL='https://127.0.0.1:$acme/dir' IRIS_ACME_CA='$dir/certs/pebble.minica.pem' IRIS_ACME_HTTP_PORT='$http' IRIS_ACME_DNS_API='http://127.0.0.1:$api' IRIS_ACME_DIR='$dir'"
