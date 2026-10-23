#!/usr/bin/env bash
# Copies the working tree to the LXC, builds Iris there and runs bench/run.py,
# then copies bench/results.json back. Arguments go to run.py.
set -euo pipefail
LXC="${IRIS_LXC:-root@192.168.1.152}"
cd "$(dirname "$0")/.."
git ls-files -z --cached --others --exclude-standard |
  while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done |
  tar --null -T - -czf - | ssh -o BatchMode=yes "$LXC" "mkdir -p /root/iris-bench && cd /root/iris-bench && tar -xzf -"
[ -f bench/results.json ] && scp -q bench/results.json "$LXC:/root/iris-bench/bench/results.json"
ssh -o BatchMode=yes "$LXC" "export PATH=\$HOME/.cargo/bin:/usr/local/go/bin:\$PATH
  cd /root/iris-bench && make -j >/dev/null && python3 bench/run.py --iris ./iris $*"
scp -q "$LXC:/root/iris-bench/bench/results.json" bench/results.json
