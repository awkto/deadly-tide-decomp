#!/bin/sh
# Copy a finished browser build (index.html, deadlytide.js, deadlytide.wasm) into a static web root.
# Usage: tools/web/deploy.sh BUILD_DIR TARGET_DIR   e.g. tools/web/deploy.sh build/wasm /srv/tide/site
# The page never contains game data: players pick their own .L96 files and DT.EXE.
set -e
[ $# -eq 2 ] || { echo "usage: $0 BUILD_DIR TARGET_DIR" >&2; exit 2; }
for f in index.html deadlytide.js deadlytide.wasm; do
    [ -s "$1/$f" ] || { echo "missing $1/$f" >&2; exit 1; }
done
mkdir -p "$2"
for f in deadlytide.wasm deadlytide.js index.html; do cp "$1/$f" "$2/$f.tmp" && mv "$2/$f.tmp" "$2/$f"; done
echo "deployed to $2"
