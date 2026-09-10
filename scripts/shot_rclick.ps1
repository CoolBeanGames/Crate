# Launch Crate, focus it, right-click at a window-relative point, screenshot.
param([string]$Out = "$env:TEMP\crate_shot.png", [int]$WaitSeconds = 4,
      [int]$X = 400, [int]$Y = 760, [int]$PreX = -1, [int]$PreY = -1,
      [string[]]$ExtraArgs = @())
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\Crate.exe"
if ($ExtraArgs -and $ExtraArgs.Count -gt 0) {
    $p = Start-Process $exe -PassThru -WorkingDirectory $root -ArgumentList $ExtraArgs
} else {
    $p = Start-Process $exe -PassThru -WorkingDirectory $root
}
Start-Sleep -Seconds $WaitSeconds

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  public struct RECT { public int L, T, R, B; }
}
"@
$h = $p.MainWindowHandle
$TOP = New-Object IntPtr(-1); $NOTOP = New-Object IntPtr(-2)
[Win]::ShowWindow($h, 9) | Out-Null
[Win]::SetWindowPos($h, $TOP, 0,0,0,0, 0x0003) | Out-Null
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
if ($PreX -ge 0) {
  [Win]::SetCursorPos($r.L + $PreX, $r.T + $PreY)
  Start-Sleep -Milliseconds 150
  [Win]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
  [Win]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 300
}
[Win]::SetCursorPos($r.L + $X, $r.T + $Y)
Start-Sleep -Milliseconds 200
[Win]::mouse_event(0x0008, 0, 0, 0, [IntPtr]::Zero) # right down
[Win]::mouse_event(0x0010, 0, 0, 0, [IntPtr]::Zero) # right up
Start-Sleep -Milliseconds 500

$w = $r.R - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
[Win]::SetWindowPos($h, $NOTOP, 0,0,0,0, 0x0003) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Stop-Process $p.Id
Write-Output $Out
