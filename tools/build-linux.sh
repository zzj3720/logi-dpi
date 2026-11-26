#!/bin/sh
# Builds out/mousedpi on Linux (requires libgtk-4-dev libsoup-3.0-dev libepoxy-dev).
#   [PIONC=pionc] sh tools/build-linux.sh
set -e
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out"
mkdir -p "$OUT"

sh "$ROOT/tools/deps.sh"
cd "$ROOT"

"${PIONC:-pionc}" build --release pion/cmd/mousedpi -o "$OUT/mousedpi"
cp "$ROOT/assets/mouse.png" "$OUT/mouse.png"
echo "built $OUT/mousedpi"
