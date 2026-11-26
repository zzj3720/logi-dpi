#!/bin/sh
# Installs mousedpi binary, mouse.png asset, desktop entry, and udev rules on Linux.
#   [PREFIX=/usr/local] sh tools/install-linux.sh
set -e
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BIN="$ROOT/out/mousedpi"

if [ ! -x "$BIN" ]; then
  echo "install-linux: $BIN not found; run 'sh tools/build-linux.sh' first" >&2
  exit 1
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  SUDO="sudo"
fi

$SUDO install -Dm755 "$BIN" "$PREFIX/bin/mousedpi"
$SUDO install -Dm644 "$ROOT/assets/mouse.png" "$PREFIX/share/mousedpi/mouse.png"
$SUDO install -Dm644 "$ROOT/assets/mouse.png" "$PREFIX/share/icons/hicolor/256x256/apps/com.zzj3720.MouseDPI.png"
$SUDO install -Dm644 "$ROOT/assets/com.zzj3720.MouseDPI.desktop" "$PREFIX/share/applications/com.zzj3720.MouseDPI.desktop"
$SUDO install -Dm644 "$ROOT/assets/70-logi-dpi.rules" /etc/udev/rules.d/70-logi-dpi.rules

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  $SUDO gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi

if command -v udevadm >/dev/null 2>&1; then
  $SUDO udevadm control --reload-rules
  $SUDO udevadm trigger --subsystem-match=hidraw
fi

echo "installed mousedpi to $PREFIX/bin/mousedpi and /etc/udev/rules.d/70-logi-dpi.rules"
