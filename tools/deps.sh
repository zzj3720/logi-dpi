#!/bin/sh
# Fetches the pinned invar (pure-Pion UI framework + Liquid Glass `lg`) into deps/invar.
# Compatible with an existing git checkout or a symlinked local invar checkout.
set -e
REV=09471669821896647e4e469022f0f858c86d5c1b
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DIR="$ROOT/deps/invar"
REPO="${INVAR_REPO:-https://github.com/AFK-surf/invar.git}"

if [ -L "$DIR" ]; then
  if [ ! -d "$DIR" ]; then
    echo "deps: broken symlink at $DIR" >&2
    exit 1
  fi
  CUR="$(git -C "$DIR" rev-parse --short HEAD 2>/dev/null || echo external)"
  echo "deps: using symlinked deps/invar at $CUR"
elif [ -d "$DIR/.git" ] || [ -f "$DIR/.git" ]; then
  CUR="$(git -C "$DIR" rev-parse HEAD 2>/dev/null || true)"
  if [ "$CUR" != "$REV" ]; then
    git -C "$DIR" fetch -q origin "$REV" || git -C "$DIR" fetch -q origin
    git -C "$DIR" checkout -q "$REV"
  fi
  echo "deps: deps/invar at $(git -C "$DIR" rev-parse --short HEAD)"
else
  mkdir -p "$(dirname -- "$DIR")"
  git clone -q "$REPO" "$DIR"
  git -C "$DIR" checkout -q "$REV"
  echo "deps: deps/invar at $(git -C "$DIR" rev-parse --short HEAD)"
fi

case "$(uname -s)" in
  MINGW*|MSYS*) sh "$DIR/packages/host/tools/fetch-webview2.sh" ;;
esac
