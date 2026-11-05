#!/usr/bin/env bash
# Builds Iris and runs the tests, normal and ASan, on the LXC container (root@192.168.1.152).
# The Mac and its Docker are no longer used.
# Usage: tools/test-platforms.sh
set -uo pipefail
LXC="${IRIS_LXC:-root@192.168.1.152}"
cd "$(dirname "$0")/.."
platforms=(linux-x86_64)
id="iris-test-$$"
logdir="${TMPDIR:-/tmp}/$id"
mkdir -p "$logdir"

bundle() {
  git ls-files -z --cached --others --exclude-standard |
    while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done |
    tar --null -T - -czf -
}

steps='set -o pipefail; make clean >/dev/null && make && make iris-debug >/dev/null && python3 tools/run_tests.py && python3 tools/run_tests.py --iris ./iris-debug | tail -1'

run() {
  case "$1" in
  linux-x86_64)
    # the lock bench/remote.sh takes, so a run never overlaps a benchmark;
    # the line saying it got the lock goes to the terminal as well as the log
    bundle | ssh -o BatchMode=yes "$LXC" "flock -o /root/iris-bench.lock bash -c '
      echo \"test-platforms: got the LXC\" >&2
      rm -rf /tmp/$id && mkdir -p /tmp/$id && cd /tmp/$id && tar -xzf - &&
      { $steps; }; rc=\$?; rm -rf /tmp/$id; exit \$rc'" 2> >(while IFS= read -r l; do
        case "$l" in *"got the LXC"*) echo "$l" >&3 ;; esac
        printf '%s\n' "$l"
      done >&2) ;;
  *) echo "unknown platform $1"; return 2 ;;
  esac
}

exec 3>&1
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
