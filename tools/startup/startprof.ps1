param([string]$exe, [string]$prof)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class V { [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h); }
"@
$env:MOUSEDPI_TEST = "1"
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
$pr = Start-Process -FilePath $prof -ArgumentList "$($p.Id) 1500" -NoNewWindow -PassThru -RedirectStandardOutput ((Split-Path $exe) + "\prof.txt")
$seen = $false
while ($sw.ElapsedMilliseconds -lt 8000) {
  $p.Refresh()
  if (-not $seen -and $p.MainWindowHandle -ne 0 -and [V]::IsWindowVisible($p.MainWindowHandle)) { "visible at $($sw.ElapsedMilliseconds) ms"; $seen = $true }
  Start-Sleep -Milliseconds 20
}
$pr.WaitForExit()
Stop-Process -Id $p.Id
