# --tray start: no window seen (sampled every 10 ms for 2 s), then brought up (WM_SHOW_ME, as a second start does) and
# captured; then a normal start captured as it shows. Both MOUSEDPI_TEST (beside a running MouseDPI).
param([string]$exe, [string]$outdir)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public class T {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int n);
  public struct RECT { public int L,T,R,B; }
  static uint want; static IntPtr found;
  static bool Each(IntPtr h, IntPtr l) {
    uint p; GetWindowThreadProcessId(h, out p);
    System.Text.StringBuilder sb = new System.Text.StringBuilder(64); GetClassNameW(h, sb, 64);
    if (p == want && GetWindow(h, 4) == IntPtr.Zero && sb.ToString() == "mbui") { found = h; return false; }
    return true;
  }
  public static IntPtr FindMain(uint pid) { want = pid; found = IntPtr.Zero; EnumWindows(new EnumProc(Each), IntPtr.Zero); return found; }
  public static bool Seen(IntPtr h) { int c = 0; DwmGetWindowAttribute(h, 14, out c, 4); return IsWindowVisible(h) && c == 0; }
}
"@
[T]::SetProcessDPIAware() | Out-Null
$env:MOUSEDPI_TEST = "1"
function Shot($h, $path) {
  $r = New-Object T+RECT; [T]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hh = $r.B - $r.T; "rect=$($r.L),$($r.T) ${w}x$hh"
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [T]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
}
$p = Start-Process -FilePath $exe -ArgumentList "--tray" -WorkingDirectory (Split-Path $exe) -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew(); $seen = 0; $h = [IntPtr]::Zero
while ($sw.ElapsedMilliseconds -lt 2000) {
  if ($h -eq [IntPtr]::Zero) { $h = [T]::FindMain([uint32]$p.Id) }
  if ($h -ne [IntPtr]::Zero -and [T]::Seen($h)) { $seen++ }
  Start-Sleep -Milliseconds 10
}
"tray start: window=$h seen in $seen samples of 2 s (want 0)"
[T]::PostMessageW($h, 0x802B, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 1000
"brought up: seen=$([T]::Seen($h))"
Shot $h "$outdir\tray-up.png"
Stop-Process -Id $p.Id
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
Start-Sleep -Milliseconds 1500
$h = [T]::FindMain([uint32]$p.Id)
"normal start: seen=$([T]::Seen($h))"
Shot $h "$outdir\normal.png"
Stop-Process -Id $p.Id
