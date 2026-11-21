#!/bin/sh
# Fetches the pinned invar (pure-Pion UI framework + Liquid Glass `lg`) into deps/invar.
set -e
REV=9b8902bc4dc442d3908f5205cb1d9bce4d5fb033
DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/deps/invar"
REPO="${INVAR_REPO:-https://github.com/AFK-surf/invar.git}"
if [ -d "$DIR/.git" ]; then
  git -C "$DIR" fetch -q origin "$REV" || git -C "$DIR" fetch -q origin
else
  git clone -q "$REPO" "$DIR"
fi
git -C "$DIR" checkout -q "$REV"
case "$(uname -s)" in MINGW*|MSYS*) sh "$DIR/packages/host/tools/fetch-webview2.sh" ;; esac
echo "deps: deps/invar at $(git -C "$DIR" rev-parse --short HEAD)"
