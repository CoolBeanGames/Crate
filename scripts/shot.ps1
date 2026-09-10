# Launch Crate (optionally with args), force its window to the foreground via a
# brief topmost toggle, and save a PNG.
param([string]$Out = "$env:TEMP\crate_shot.png", [int]$WaitSeconds = 4,
      [string[]]$ExtraArgs = @())
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$p = Start-Process (Join-Path $root "build\Crate.exe") -PassThru -WorkingDirectory $root `
        -ArgumentList $ExtraArgs
Start-Sleep -Seconds $WaitSeconds

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
}
"@

$h = $p.MainWindowHandle
$TOPMOST = New-Object IntPtr(-1)
$NOTOPMOST = New-Object IntPtr(-2)
[Win]::ShowWindow($h, 9) | Out-Null            # SW_RESTORE
[Win]::SetWindowPos($h, $TOPMOST, 0,0,0,0, 0x0003) | Out-Null
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 700
$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
[Win]::SetWindowPos($h, $NOTOPMOST, 0,0,0,0, 0x0003) | Out-Null
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Stop-Process $p.Id
Write-Output $Out
