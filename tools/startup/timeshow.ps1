# Starts MouseDPI (args optional) and prints how long until its window is visible and uncloaked (or "hidden" after 3 s).
param([string]$exe, [string]$arg = "")
Add-Type -Name U -Namespace W -MemberDefinition '[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h); [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int n); [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);'
$s = [Diagnostics.Stopwatch]::StartNew()
if ($arg) { $p = Start-Process $exe -ArgumentList $arg -PassThru } else { $p = Start-Process $exe -PassThru }
while ($s.ElapsedMilliseconds -lt 3000) {
  $p.Refresh(); $h = $p.MainWindowHandle
  if ($h -ne 0 -and [W.U]::IsWindowVisible($h)) { $c = 0; [void][W.U]::DwmGetWindowAttribute($h, 14, [ref]$c, 4); if ($c -eq 0) { "shown after $($s.ElapsedMilliseconds) ms (pid $($p.Id))"; exit } }
  Start-Sleep -Milliseconds 5
}
"not shown in 3 s (pid $($p.Id), exited=$($p.HasExited))"
