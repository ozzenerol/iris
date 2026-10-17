#!/usr/bin/env bash
# Build iris with every core, and install it.
#
# The iris program carries its standard library and runtime inside it, so
# installing it is copying one file to $PREFIX/bin. The Vim syntax files
# go to $PREFIX/share/iris/editors/vim.
#
# Usage: ./build-and-install.sh [--test] [--no-install] [--clean] [--prefix DIR]
#   --test        run the test suite before installing; stop if it fails
#   --no-install  only build ./iris
#   --clean       remove ./build and ./iris first
#   --prefix DIR  where to install (default /usr/local, or $PREFIX)
#
# Run it as your normal user: sudo is used for the copy only, when DIR
# isn't writable. JOBS sets how many files compile at once (default: all
# cores), CC the C compiler.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
RUN_TESTS=0
INSTALL=1
CLEAN=0

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --test) RUN_TESTS=1 ;;
        --no-install) INSTALL=0 ;;
        --clean) CLEAN=1 ;;
        --prefix) shift; PREFIX="${1:?--prefix needs a folder}" ;;
        --prefix=*) PREFIX="${1#--prefix=}" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
    echo "run this as your normal user; it uses sudo for the copy only" >&2
    exit 1
fi

cd "$ROOT"

# iris turns its output into programs with clang 15 or newer
CLANG="${IRIS_CC:-clang}"
if ! command -v "$CLANG" >/dev/null; then
    echo "warning: $CLANG isn't installed; iris needs clang 15 or newer to build programs" >&2
else
    major="$("$CLANG" --version | sed -n 's/.*clang version \([0-9][0-9]*\).*/\1/p' | head -1)"
    if [ -n "$major" ] && [ "$major" -lt 15 ]; then
        echo "warning: $CLANG is version $major; iris needs 15 or newer" >&2
    fi
fi

[ "$CLEAN" -eq 1 ] && make clean >/dev/null

echo "building with $JOBS jobs"
make -j"$JOBS" iris
./iris version

if [ "$RUN_TESTS" -eq 1 ]; then
    python3 tools/run_tests.py
fi

[ "$INSTALL" -eq 1 ] || exit 0

SUDO=""
mkdir -p "$PREFIX" 2>/dev/null || true
if [ ! -w "$PREFIX" ] || { [ -e "$PREFIX/bin" ] && [ ! -w "$PREFIX/bin" ]; }; then
    SUDO="sudo"
    echo "installing into $PREFIX needs sudo"
fi
$SUDO install -d "$PREFIX/bin" "$PREFIX/share/iris/editors"
$SUDO install -m 755 iris "$PREFIX/bin/iris"
$SUDO rm -rf "$PREFIX/share/iris/editors/vim"
$SUDO cp -R editors/vim "$PREFIX/share/iris/editors/vim"

echo "installed $PREFIX/bin/iris"
found="$(command -v iris || true)"
if [ "$found" != "$PREFIX/bin/iris" ]; then
    if [ -z "$found" ]; then
        echo "$PREFIX/bin isn't on your PATH; add it to run iris by name" >&2
    else
        echo "note: 'iris' on your PATH is $found, which comes before $PREFIX/bin/iris" >&2
    fi
fi
