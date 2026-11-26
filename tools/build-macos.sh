#!/bin/sh
# Builds out/MouseDPI and packages out/MouseDPI.app on macOS.
#   [PIONC=pionc] sh tools/build-macos.sh
set -e
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out"
APP="$OUT/MouseDPI.app"
mkdir -p "$OUT"

sh "$ROOT/tools/deps.sh"
cd "$ROOT"

"${PIONC:-pionc}" build --release pion/cmd/mousedpi -o "$OUT/MouseDPI"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$OUT/MouseDPI" "$APP/Contents/MacOS/MouseDPI"
cp "$ROOT/assets/mouse.png" "$APP/Contents/Resources/mouse.png"

# Square-padded .icns generation from assets/mouse.png when macOS sips + iconutil are available.
if command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
  ICONSET="$OUT/AppIcon.iconset"
  rm -rf "$ICONSET"
  mkdir -p "$ICONSET"
  for sz in 16 32 128 256 512; do
    sz2=$((sz * 2))
    sips -Z "$sz" "$ROOT/assets/mouse.png" --out "$ICONSET/icon_${sz}x${sz}.png" >/dev/null
    sips -p "$sz" "$sz" "$ICONSET/icon_${sz}x${sz}.png" >/dev/null
    sips -Z "$sz2" "$ROOT/assets/mouse.png" --out "$ICONSET/icon_${sz}x${sz}@2x.png" >/dev/null
    sips -p "$sz2" "$sz2" "$ICONSET/icon_${sz}x${sz}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
  rm -rf "$ICONSET"
fi

APP_VERSION="${APP_VERSION:-0.3.0}"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>zh_CN</string>
  <key>CFBundleDisplayName</key>
  <string>MouseDPI</string>
  <key>CFBundleExecutable</key>
  <string>MouseDPI</string>
  <key>CFBundleIconFile</key>
  <string>AppIcon</string>
  <key>CFBundleIdentifier</key>
  <string>com.zzj3720.mousedpi</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>MouseDPI</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>${APP_VERSION}</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>LSMinimumSystemVersion</key>
  <string>14.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>NSPrincipalClass</key>
  <string>NSApplication</string>
</dict>
</plist>
EOF

# Ad-hoc sign the app bundle so Apple Silicon executes it cleanly out of the box.
if command -v codesign >/dev/null 2>&1; then
  codesign --force --deep --sign - "$APP"
fi

echo "built $OUT/MouseDPI and $APP"
