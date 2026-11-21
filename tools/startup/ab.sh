#!/bin/sh
# Where a start goes, on Windows (Git Bash), in a scratch copy of this repo (never the one you ship from):
#   python3 tools/startup/mark.py pion/cmd/mousedpi    # (before syncing it there: that box has no python) the
#                                                       # sources marked (marks.txt: ms since the process began)
#   clang -O2 tools/startup/prof.c -o out/prof.exe -ldbghelp   # (once) the sampler startprof.ps1 runs beside it
#   [VARIANTS="A B"] [TRAYTEST=1] sh tools/startup/ab.sh
# A: the pinned invar with host-marks.patch (window made, renderer, frames, shown); B: the same with
# tools/startup/invar.patch (a change to invar under test). Each built, then started 3 times.
A=/c/Users/3720/Apps; O=$A/pionws/pion/out
export PIONC=$O/pionc.exe PION_QBE=$O/qbe.exe PION_SRC=$A/pionws/pion PION_CACHE_DIR=$A/pionws/cas-debug
run() {
  for i in 1 2 3; do
    rm -f out/marks.txt
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/startup/startprof.ps1 -exe "$(cygpath -w $PWD/out/MouseDPI.exe)" -prof "$(cygpath -w $PWD/out/prof.exe)" 2>&1 | grep visible
    sort -t' ' -k4 -n out/marks.txt | tr '\r\n' '  '; echo
  done
}
[ -d deps/invar/.git ] && (cd deps/invar && git checkout -q .)
sh tools/deps.sh >/dev/null || exit 1
for v in ${VARIANTS:-A B}; do
  (cd deps/invar && git checkout -q . && git apply ../../tools/startup/host-marks.patch) || exit 1
  [ $v = B ] && { (cd deps/invar && git apply ../../tools/startup/invar.patch) || exit 1; }
  sh tools/build-win.sh > build-$v.log 2>&1 || { echo "build $v failed"; grep -i 'error' build-$v.log | head -20; exit 1; }
  echo "=== $v"
  run
done
(cd deps/invar && git checkout -q .)
[ -n "$TRAYTEST" ] && powershell -NoProfile -ExecutionPolicy Bypass -File tools/startup/traytest.ps1 -exe "$(cygpath -w $PWD/out/MouseDPI.exe)" -outdir "$(cygpath -w $PWD/out)"
