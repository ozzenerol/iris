#!/bin/sh
# wip: times iris builds with full LTO and ThinLTO. Usage: lto-buildtime.sh DIR (a built iris checkout)
cd "$1" || exit 1
I=./iris
t() { s=$(date +%s%N); "$@" >/dev/null 2>&1 || echo FAIL; echo -n "$(( ($(date +%s%N)-s)/1000000 )) "; }
printf 'void main() {\n    print("hi")\n}\n' > /tmp/lto-small.iris
cp bench/http-json/main.iris /tmp/lto-hj.iris
for mode in debug release-fast; do
  for lto in full thin; do
    [ $mode = debug ] && [ $lto = thin ] && continue
    E="IRIS_NONE=1"; [ $lto = thin ] && E="IRIS_THINLTO=1"
    rm -rf ~/.cache/iris/rt/lto
    env $E $I build --mode=$mode /tmp/lto-small.iris -o /tmp/lto-x >/dev/null 2>&1   # warm runtime cache
    echo -n "$mode $lto small: "; for i in 1 2 3; do t env $E $I build --mode=$mode /tmp/lto-small.iris -o /tmp/lto-x; done; echo
    echo -n "$mode $lto http-json same: "; for i in 1 2 3; do t env $E $I build --mode=$mode /tmp/lto-hj.iris -o /tmp/lto-x; done; echo
    echo -n "$mode $lto http-json edited: "; for i in 1 2 3; do sed -i "s/127.0.0.[0-9]*/127.0.0.$i$i/" /tmp/lto-hj.iris; t env $E $I build --mode=$mode /tmp/lto-hj.iris -o /tmp/lto-x; done; echo
  done
done
echo -n "run small debug: "; for i in 1 2 3; do t $I run /tmp/lto-small.iris; done; echo
