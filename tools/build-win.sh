#!/bin/sh
# Builds MouseDPI.exe on Windows (Git Bash): pionc compiles the Pion side; LLVM clang for the MSVC target (the
# llvm-project release, with Visual Studio Build Tools' MSVC libraries and the Windows SDK) compiles invar's Windows
# host (written for MSVC: WRL, WinRT, Windows SDK) and the Pion runtime, and links with lld (tools/win-cc.c turns
# pionc's GCC-style link flags into MSVC ones).
#   PIONC=<pionc.exe> PION_QBE=<qbe.exe> PION_SRC=<pion checkout> [LLVM_BIN=<dir with clang.exe>] sh tools/build-win.sh
set -e
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="$ROOT/out"
mkdir -p "$OUT"
LLVM_BIN="${LLVM_BIN:-/c/Users/3720/Apps/llvm/bin}"
export PION_CLANG="$(cygpath -w "$LLVM_BIN/clang.exe")"
sh "$ROOT/tools/deps.sh"
cd "$ROOT"
"$LLVM_BIN/clang.exe" -O2 tools/win-cc.c -o "$OUT/win-cc.exe"
cp "$OUT/win-cc.exe" "$OUT/win-c++.exe"
# the Pion runtime, built by the same compiler as everything it links with
"$LLVM_BIN/clang.exe" -std=c11 -O2 -fno-asynchronous-unwind-tables -D_CRT_SECURE_NO_WARNINGS -c "$PION_SRC/runtime/pion_rt.c" -o "$OUT/pion_rt.o"
rm -f "$OUT/libpionrt.a"
"$LLVM_BIN/llvm-ar.exe" rcs "$OUT/libpionrt.a" "$OUT/pion_rt.o"
"$LLVM_BIN/llvm-rc.exe" -fo "$(cygpath -w "$OUT/app.res")" "$(cygpath -w "$ROOT/assets/app.rc")"
export CC="$OUT/win-cc.exe" CXX="$OUT/win-c++.exe" PION_RT_LIB="$OUT/libpionrt.a" PION_TARGET=amd64_win
"${PIONC:-pionc}" build --release -o "$OUT/MouseDPI.exe" pion/cmd/mousedpi
cp assets/mouse.png "$OUT/mouse.png"
echo "built $OUT/MouseDPI.exe"
