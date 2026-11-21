param([string]$exe, [string]$out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
public struct RECT { public int L,T,R,B; } }
"@
[W]::SetProcessDPIAware() | Out-Null
$before = @(Get-Process MouseDPI -ErrorAction SilentlyContinue | ForEach-Object Id)
Start-Process explorer.exe -ArgumentList "`"$exe`""
Start-Sleep -Seconds 8
$p = Get-Process MouseDPI -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | Select-Object -First 1
if (-not $p) { "no new process"; exit }
"pid=$($p.Id) title=$($p.MainWindowTitle) hwnd=$($p.MainWindowHandle) path=$($p.Path) session=$($p.SessionId)"
$h = $p.MainWindowHandle
if ($h -ne 0) {
  [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 800
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hh = $r.B - $r.T; "rect=$($r.L),$($r.T) ${w}x$hh"
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [W]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
}
Stop-Process -Id $p.Id
