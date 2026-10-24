#!/usr/bin/env bash
# Copies the working tree to the LXC, builds Iris there and runs bench/run.py,
# then copies bench/results.json back. Arguments go to run.py.
#
# Several checkouts may benchmark on the LXC; they share /root/iris-bench and
# its Rust build, so a run holds a lock there from the copy to the results
# and the next one waits. Each run's results go to a file of its own. The
# servers a run starts don't inherit the lock (-o), so one left running can't
# hold up the queue.
set -euo pipefail
LXC="${IRIS_LXC:-root@192.168.1.152}"
cd "$(dirname "$0")/.."
out="/root/iris-bench-results/$(hostname)-$$-$(date +%s).json"
git ls-files -z --cached --others --exclude-standard |
  while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done |
  tar --null -T - -czf - | ssh -o BatchMode=yes "$LXC" "mkdir -p /root/iris-bench /root/iris-bench-results
  export PATH=\$HOME/.cargo/bin:/usr/local/go/bin:\$PATH
  flock -o /root/iris-bench.lock sh -c '
    echo \"bench: got the LXC\" >&2
    cd /root/iris-bench && tar -xzf - &&
    { [ -f bench/results.json ] && cp bench/results.json $out || true; } &&
    make -j >/dev/null && python3 bench/run.py --iris ./iris --out $out $*'"
scp -q "$LXC:$out" bench/results.json
ssh -o BatchMode=yes "$LXC" "rm -f $out"
