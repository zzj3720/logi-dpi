#!/bin/sh
# replace the running MouseDPI in C:\Users\3720\Apps\MouseDPI with out/ and start it
set -e
D=/c/Users/3720/Apps/MouseDPI
powershell -NoProfile -Command "Get-Process MouseDPI -ErrorAction SilentlyContinue | Where-Object { \$_.Path -like '*Apps\\MouseDPI\\*' } | Stop-Process -Force" || true
sleep 1
[ -f $D/MouseDPI.exe.v022_bak ] || cp $D/MouseDPI.exe $D/MouseDPI.exe.v022_bak
[ -f $D/mouse.png.v022_bak ] || cp $D/mouse.png $D/mouse.png.v022_bak
cp out/MouseDPI.exe $D/MouseDPI.exe
cp out/mouse.png $D/mouse.png
powershell -NoProfile -Command "Start-Process explorer.exe -ArgumentList '\"C:\Users\3720\Apps\MouseDPI\MouseDPI.exe\"'"
sleep 6
powershell -NoProfile -Command "Get-Process MouseDPI | Select Id,MainWindowTitle,Path | Format-Table -AutoSize | Out-String -Width 200"
ls -la $D
cat $D/config.ini
