#!/bin/sh
# Capstone at the version the Python rewriter uses (work/venv, capstone 5.0.9; its __version__ says 5.0.7, a stale constant
# in the binding: the wheel's headers and library are 5.0.9). A moved tag is refused: the decoder must not change by accident.
# usage: fetch_capstone.sh [dir]   (default port/android/deps/src/capstone; build_deps.sh runs this)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-$HERE/../../deps/src/capstone}
TAG=5.0.9; COMMIT=022575848782a4801fd150fdbc927effcbca0864
[ -d "$DEST/.git" ] || git clone -q -c advice.detachedHead=false --depth 1 --branch $TAG https://github.com/capstone-engine/capstone.git "$DEST"
HAVE=$(git -C "$DEST" rev-parse HEAD)
[ "$HAVE" = "$COMMIT" ] || { echo "capstone in $DEST is at $HAVE, not $TAG ($COMMIT)" >&2; exit 1; }
