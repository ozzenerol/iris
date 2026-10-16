#!/usr/bin/env bash
# Builds Iris and runs the tests on the three platforms, in parallel:
#   linux-x86_64   the LXC container (root@192.168.1.152)
#   macos-arm64    the Mac (dev@192.168.1.150)
#   linux-arm64    Docker on the Mac
# Usage: tools/test-platforms.sh [platform...]   (default: all three)
set -uo pipefail
LXC="${IRIS_LXC:-root@192.168.1.152}"
MAC="${IRIS_MAC:-dev@192.168.1.150}"
cd "$(dirname "$0")/.."
platforms=("${@:-linux-x86_64 macos-arm64 linux-arm64}")
[ $# -eq 0 ] && platforms=(linux-x86_64 macos-arm64 linux-arm64)
id="iris-test-$$"
logdir="${TMPDIR:-/tmp}/$id"
mkdir -p "$logdir"

bundle() {
  git ls-files -z --cached --others --exclude-standard |
    while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done |
    tar --null -T - -czf -
}

steps='make clean >/dev/null && make && make iris-debug >/dev/null && python3 tools/run_tests.py && python3 tools/run_tests.py --iris ./iris-debug | tail -1'

run() {
  case "$1" in
  linux-x86_64)
    bundle | ssh -o BatchMode=yes "$LXC" "
      rm -rf /tmp/$id && mkdir -p /tmp/$id && cd /tmp/$id && tar -xzf - &&
      { $steps; }; rc=\$?; rm -rf /tmp/$id; exit \$rc" ;;
  macos-arm64)
    bundle | ssh -o BatchMode=yes "$MAC" "
      export PATH=/opt/homebrew/bin:/usr/local/bin:\$PATH
      rm -rf /tmp/$id-mac && mkdir -p /tmp/$id-mac && cd /tmp/$id-mac && tar -xzf - &&
      { $steps; }; rc=\$?; rm -rf /tmp/$id-mac; exit \$rc" ;;
  linux-arm64)
    bundle | ssh -o BatchMode=yes "$MAC" "
      export PATH=/opt/homebrew/bin:/usr/local/bin:\$PATH
      docker info >/dev/null 2>&1 || { open -a Docker;
        for i in \$(seq 1 40); do docker info >/dev/null 2>&1 && break; sleep 3; done; }
      rm -rf /tmp/$id-dock && mkdir -p /tmp/$id-dock && cd /tmp/$id-dock && tar -xzf - &&
      docker build -q --platform linux/arm64 -t iris-test:arm64 docker >/dev/null &&
      tar -cf - . | docker run --rm -i --platform linux/arm64 iris-test:arm64 \
        bash -c 'tar -xf - && $steps'; rc=\$?; rm -rf /tmp/$id-dock; exit \$rc" ;;
  *) echo "unknown platform $1"; return 2 ;;
  esac
}

for p in "${platforms[@]}"; do
  run "$p" > "$logdir/$p.log" 2>&1 &
  eval "pid_${p//-/_}=$!"
done

status=0
for p in "${platforms[@]}"; do
  pid_var="pid_${p//-/_}"
  wait "${!pid_var}"
  rc=$?
  summary=$(grep -a -E '^[0-9]+ tests,' "$logdir/$p.log" | tr '\n' ';' | sed 's/;$//; s/;/ | asan: /')
  if [ $rc -eq 0 ]; then
    echo "PASS $p: $summary"
  else
    echo "FAIL $p (exit $rc): ${summary:-no summary}  log: $logdir/$p.log"
    grep -a -E -A3 '^FAIL|error:' "$logdir/$p.log" | head -20
    status=1
  fi
done
[ $status -eq 0 ] && rm -rf "$logdir"
exit $status
