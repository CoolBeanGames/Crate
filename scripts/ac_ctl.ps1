# Stateless control script for probing the script-editor autocomplete bug.
# Each invocation is a fresh PowerShell process, so state (the target PID)
# is persisted to a small file between calls. Not part of the normal build.
param(
    [Parameter(Mandatory=$true)][ValidateSet("launch","click","dblclick","rclick","type","slowtype","key","shot","close","wheel","drag")]
    [string]$Action,
    [string]$Exe = "build\Crate.exe",
    [int]$WaitSeconds = 4,
    [int]$X = 0,
    [int]$Y = 0,
    [int]$X2 = 0,
    [int]$Y2 = 0,
    [string]$Text = "",
    [string]$Name = "shot",
    [int]$Delta = -3
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing, System.Windows.Forms

$root = Split-Path -Parent $PSScriptRoot
$pidFile = Join-Path $root "tmp_shots\ac.pid"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class AcWin2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  public struct RECT { public int L, T, R, B; }
}
"@

function DoWheel([int]$X, [int]$Y, [int]$Delta) {
    $h = GetTargetHandle
    Foreground($h)
    $r = New-Object AcWin2+RECT
    [AcWin2]::GetWindowRect($h, [ref]$r) | Out-Null
    [AcWin2]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null
    Start-Sleep -Milliseconds 80
    $wheelData = [uint32]([int64]$Delta * 120 -band 0xFFFFFFFFL)
    [AcWin2]::mouse_event(0x0800, 0, 0, $wheelData, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 150
}

function GetTargetHandle {
    $procId = Get-Content $pidFile -ErrorAction Stop
    $p = Get-Process -Id $procId -ErrorAction Stop
    return $p.MainWindowHandle
}

function Foreground([IntPtr]$h) {
    $TOPMOST = New-Object IntPtr(-1)
    $NOTOPMOST = New-Object IntPtr(-2)
    [AcWin2]::ShowWindow($h, 9) | Out-Null
    [AcWin2]::SetWindowPos($h, $TOPMOST, 0,0,0,0, 0x0003) | Out-Null
    [AcWin2]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 400
    [AcWin2]::SetWindowPos($h, $NOTOPMOST, 0,0,0,0, 0x0003) | Out-Null
}

switch ($Action) {
    "launch" {
        $exePath = Join-Path $root $Exe
        $p = Start-Process $exePath -PassThru -WorkingDirectory $root -ArgumentList @("--script")
        Start-Sleep -Seconds $WaitSeconds
        $p.Id | Out-File -FilePath $pidFile -Encoding ascii -NoNewline
        Foreground($p.MainWindowHandle)
        Write-Output "launched pid=$($p.Id)"
    }
    "click" {
        $h = GetTargetHandle
        Foreground($h)
        $r = New-Object AcWin2+RECT
        [AcWin2]::GetWindowRect($h, [ref]$r) | Out-Null
        [AcWin2]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null
        Start-Sleep -Milliseconds 100
        [AcWin2]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 40
        [AcWin2]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 200
        if ($Text) {
            [System.Windows.Forms.SendKeys]::SendWait($Text)
            Start-Sleep -Milliseconds 250
        }
        Write-Output "clicked $X,$Y"
    }
    "rclick" {
        $h = GetTargetHandle
        Foreground($h)
        $r = New-Object AcWin2+RECT
        [AcWin2]::GetWindowRect($h, [ref]$r) | Out-Null
        [AcWin2]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null
        Start-Sleep -Milliseconds 100
        [AcWin2]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 40
        [AcWin2]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 200
        if ($Text) {
            [System.Windows.Forms.SendKeys]::SendWait($Text)
            Start-Sleep -Milliseconds 250
        }
        Write-Output "rclicked $X,$Y"
    }
    "wheel" {
        DoWheel -X $X -Y $Y -Delta $Delta
        Write-Output "wheeled"
    }
    "drag" {
        $h = GetTargetHandle
        Foreground($h)
        $r = New-Object AcWin2+RECT
        [AcWin2]::GetWindowRect($h, [ref]$r) | Out-Null
        [AcWin2]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null
        Start-Sleep -Milliseconds 100
        [AcWin2]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) # left down
        Start-Sleep -Milliseconds 80
        $steps = 12
        for ($i = 1; $i -le $steps; $i++) {
            $fx = $X + ($X2 - $X) * $i / $steps
            $fy = $Y + ($Y2 - $Y) * $i / $steps
            [AcWin2]::SetCursorPos($r.L + [int]$fx, $r.T + [int]$fy) | Out-Null
            Start-Sleep -Milliseconds 40
        }
        Start-Sleep -Milliseconds 100
        [AcWin2]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) # left up
        Start-Sleep -Milliseconds 200
        Write-Output "dragged $X,$Y -> $X2,$Y2"
    }
    "dblclick" {
        $h = GetTargetHandle
        Foreground($h)
        $r = New-Object AcWin2+RECT
        [AcWin2]::GetWindowRect($h, [ref]$r) | Out-Null
        [AcWin2]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null
        Start-Sleep -Milliseconds 100
        for ($i = 0; $i -lt 2; $i++) {
            [AcWin2]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 30
            [AcWin2]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 60
        }
        Start-Sleep -Milliseconds 200
        Write-Output "dblclicked $X,$Y"
    }
    "type" {
        $h = GetTargetHandle
        Foreground($h)
        [System.Windows.Forms.SendKeys]::SendWait($Text)
        Start-Sleep -Milliseconds 250
        Write-Output "typed"
    }
    "slowtype" {
        # One SendKeys call per character, with a frame-spanning delay between
        # each, so every keystroke lands in its own render frame -- matching
        # realistic human typing speed instead of SendKeys' near-instant burst.
        $h = GetTargetHandle
        Foreground($h)
        foreach ($ch in $Text.ToCharArray()) {
            $s = [string]$ch
            if ("+^%~(){}[]".IndexOf($ch) -ge 0) { $s = "{$ch}" }
            [System.Windows.Forms.SendKeys]::SendWait($s)
            Start-Sleep -Milliseconds 90
        }
        Start-Sleep -Milliseconds 150
        Write-Output "slowtyped"
    }
    "key" {
        $h = GetTargetHandle
        Foreground($h)
        [System.Windows.Forms.SendKeys]::SendWait($Text)
        Start-Sleep -Milliseconds 250
        Write-Output "keyed"
    }
    "shot" {
        $h = GetTargetHandle
        Foreground($h)
        $r = New-Object AcWin2+RECT
        [AcWin2]::GetWindowRect($h, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $ht = $r.B - $r.T
        $bmp = New-Object System.Drawing.Bitmap $w, $ht
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
        $out = Join-Path $root "tmp_shots\$Name.png"
        $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Output $out
    }
    "close" {
        $procId = Get-Content $pidFile -ErrorAction SilentlyContinue
        if ($procId) {
            Stop-Process -Id $procId -ErrorAction SilentlyContinue
            Remove-Item $pidFile -ErrorAction SilentlyContinue
        }
        Write-Output "closed"
    }
}
