# Two-console test of the multi-card races: two Azahar instances on a local room.
#
# Each instance is a portable copy of Azahar (a "user" folder next to the exe) under $Work, with
# its own config, sdmc and log, so the default Azahar folder in %APPDATA% (which another
# project's emulator may be using) is never touched. A local azahar-room.exe hosts the room;
# each instance joins it through Multiplayer > Direct Connect to Room (UI Automation: the Qt
# frontend has no command-line option for it). Only the processes started here are closed.
#
# Usage: tools/run_azahar_pair.ps1 -Work <folder> -OutDir <folder> -Script "wait:14,ab:key:A,a:key:N,shot:x"
#   steps: wait:<s>  [a:|b:|ab:]key:<letter|UP|DOWN|LEFT|RIGHT>[:<hold ms>]  shot:<name> (both)
#   keys go to both instances unless prefixed. Logs: <Work>\A|B\user\sdmc\3ds\PixelRacing_debug.txt
param(
    [Parameter(Mandatory = $true)][string]$Work,
    [string]$Target = "3DS Files\PixelRacing-3DS.3dsx",
    [string]$Script = "wait:8,shot:boot",
    [string]$OutDir = "",
    [string]$AzaharDir = "C:\Program Files\Azahar",
    [int]$Port = 24872
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Target)) { $Target = Join-Path $root $Target }
if ($OutDir -eq "") { $OutDir = Join-Path $Work "shots" }
New-Item -ItemType Directory -Force -Path $Work, $OutDir | Out-Null
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Drawing;
public class PairCap {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public static void Down(IntPtr h, int vk) { PostMessage(h, 0x0100, (IntPtr)vk, IntPtr.Zero); }
    public static void Up(IntPtr h, int vk) { PostMessage(h, 0x0101, (IntPtr)vk, IntPtr.Zero); }
    public static void Capture(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        using (Bitmap bmp = new Bitmap(w, ht))
        using (Graphics g = Graphics.FromImage(bmp)) {
            IntPtr hdc = g.GetHdc();
            PrintWindow(h, hdc, 2);
            g.ReleaseHdc(hdc);
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
"@ -ReferencedAssemblies System.Drawing -ErrorAction SilentlyContinue
[void][PairCap]::SetProcessDPIAware()

# --- the two portable copies
$src = Join-Path $env:APPDATA "Azahar\config\qt-config.ini"
foreach ($n in "A", "B") {
    $dir = Join-Path $Work $n
    if (-not (Test-Path (Join-Path $dir "azahar.exe"))) { Copy-Item -Recurse $AzaharDir $dir }
    $cfg = Join-Path $dir "user\config"
    New-Item -ItemType Directory -Force -Path $cfg, (Join-Path $dir "user\sdmc\3ds") | Out-Null
    $lines = Get-Content $src | Where-Object { $_ -notmatch '^(nand|sdmc)_directory' -and $_ -notmatch '^Paths\\recentFiles' }
    $lines = $lines | ForEach-Object {
        if ($_ -match '^Multiplayer\\nickname=') { "Multiplayer\nickname=Player$n" }
        elseif ($_ -match '^Multiplayer\\ip=') { "Multiplayer\ip=127.0.0.1" }
        elseif ($_ -match '^Multiplayer\\port=') { "Multiplayer\port=$Port" }
        elseif ($_ -match '^confirmClose=') { "confirmClose=false" }
        elseif ($_ -match '^Multiplayer\\(nickname|ip|port)\\default=') { $_ -replace 'true', 'false' }
        else { $_ }
    }
    Set-Content -Path (Join-Path $cfg "qt-config.ini") -Value $lines -Encoding UTF8
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $dir "user\sdmc\3ds\PixelRacing_debug.txt")
    # The room nickname is the emulated console's username (the system config save), the same
    # "AZAHAR" in every copy, and the room refuses a second member of the same name: RACERA/B.
    $sysCfg = Join-Path $dir "user\nand\data\00000000000000000000000000000000\sysdata\00010017\00000000\config"
    if (Test-Path $sysCfg) {
        $bytes = [IO.File]::ReadAllBytes($sysCfg)
        foreach ($old in "AZAHAR", "RACERA", "RACERB") {
            $pat = [Text.Encoding]::Unicode.GetBytes($old)
            for ($i = 0; $i -le $bytes.Length - $pat.Length; $i += 2) {
                $hit = $true
                for ($j = 0; $j -lt $pat.Length -and $hit; $j++) { if ($bytes[$i + $j] -ne $pat[$j]) { $hit = $false } }
                if ($hit) { [Text.Encoding]::Unicode.GetBytes("RACER$n").CopyTo($bytes, $i); break }
            }
        }
        [IO.File]::WriteAllBytes($sysCfg, $bytes)
    } else {
        Write-Host "note: $n has no system config yet (first boot): both join as AZAHAR; run again"
    }
}

function Find-Element($parent, $name, $scope = [System.Windows.Automation.TreeScope]::Descendants) {
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, $name)
    for ($i = 0; $i -lt 20; $i++) {
        $e = $parent.FindFirst($scope, $cond)
        if ($e) { return $e }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Join-Room($proc, $hwnd) {
    $win = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $menu = Find-Element $win "Multiplayer"
    if (-not $menu) { throw "no Multiplayer menu" }
    try { $menu.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand() }
    catch { $menu.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
    Start-Sleep -Milliseconds 500
    $desk = [System.Windows.Automation.AutomationElement]::RootElement
    $pidCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
    $item = $null
    foreach ($w in $desk.FindAll([System.Windows.Automation.TreeScope]::Children, $pidCond)) {
        $item = Find-Element $w "Direct Connect to Room"
        if ($item) { break }
    }
    if (-not $item) { $item = Find-Element $win "Direct Connect to Room" }
    if (-not $item) { throw "no Direct Connect item" }
    $item.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Milliseconds 800
    $btn = $null
    for ($i = 0; $i -lt 20 -and -not $btn; $i++) {
        foreach ($w in $desk.FindAll([System.Windows.Automation.TreeScope]::Children, $pidCond)) {
            $btn = Find-Element $w "Connect" ([System.Windows.Automation.TreeScope]::Descendants)
            if ($btn) { break }
        }
        if (-not $btn) { Start-Sleep -Milliseconds 250 }
    }
    if (-not $btn) { throw "no Connect button" }
    $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

# (the room server quits at once without a preferred application)
$room = Start-Process -FilePath (Join-Path $AzaharDir "azahar-room.exe") -PassThru -WindowStyle Hidden -ArgumentList `
    "--room-name", "PXRTest", "--port", "$Port", "--max_members", "4", "--preferred-app", "`"3D Pixel Racing`"", `
    "--preferred-app-id", "0x000400000B3D9000", "--ban-list-file", "`"$(Join-Path $Work 'banlist.txt')`"", `
    "--log-file", "`"$(Join-Path $Work 'room.log')`""
$procs = @{}
$hw = @{}
try {
    for ($i = 0; $i -lt 20 -and -not (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue); $i++) {
        if ($room.HasExited) { throw "the room server quit (see $Work\room.log)" }
        Start-Sleep -Milliseconds 250
    }
    if (-not (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)) { throw "the room is not listening on $Port" }
    Write-Host "room up on $Port"
    foreach ($n in "A", "B") {
        $procs[$n] = Start-Process -FilePath (Join-Path $Work "$n\azahar.exe") -ArgumentList "`"$Target`"" -PassThru
        $h = [IntPtr]::Zero
        for ($i = 0; $i -lt 80 -and $h -eq [IntPtr]::Zero; $i++) {
            Start-Sleep -Milliseconds 250
            $procs[$n].Refresh()
            $h = $procs[$n].MainWindowHandle
        }
        if ($h -eq [IntPtr]::Zero) { throw "Azahar $n window did not appear" }
        $hw[$n] = $h
    }
    Start-Sleep -Seconds 3
    foreach ($n in "A", "B") {
        Join-Room $procs[$n] $hw[$n]
        Start-Sleep -Seconds 2
        $pc = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $procs[$n].Id)
        foreach ($w in [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $pc)) {
            if ($w.Current.Name -eq "Error") {
                $t = ($w.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition) | ForEach-Object { $_.Current.Name }) -join " "
                throw "Azahar $n could not join: $t"
            }
        }
        Write-Host "joined room: $n"
    }
    $vk = @{ UP = 38; DOWN = 40; LEFT = 37; RIGHT = 39 }
    foreach ($step in $Script.Split(',')) {
        $parts = [System.Collections.ArrayList]$step.Trim().Split(':')
        $who = @("A", "B")
        if ($parts[0] -eq "a") { $who = @("A"); $parts.RemoveAt(0) }
        elseif ($parts[0] -eq "b") { $who = @("B"); $parts.RemoveAt(0) }
        elseif ($parts[0] -eq "ab") { $parts.RemoveAt(0) }
        switch ($parts[0]) {
            "wait" { Start-Sleep -Milliseconds ([int]([double]$parts[1] * 1000)) }
            "key" {
                $k = $parts[1].ToUpper()
                $code = if ($vk.ContainsKey($k)) { $vk[$k] } else { [int][char]$k }
                $ms = if ($parts.Count -gt 2) { [int]$parts[2] } else { 140 }
                foreach ($n in $who) { [PairCap]::Down($hw[$n], $code) }
                Start-Sleep -Milliseconds $ms
                foreach ($n in $who) { [PairCap]::Up($hw[$n], $code) }
            }
            "shot" {
                foreach ($n in $who) {
                    $png = Join-Path $OutDir ($parts[1] + "_" + $n + ".png")
                    [PairCap]::Capture($hw[$n], $png)
                    Write-Host "shot $png"
                }
            }
        }
        foreach ($n in "A", "B") { if ($procs[$n].HasExited) { Write-Host "Azahar $n exited" } }
    }
} finally {
    foreach ($p in $procs.Values) { if ($p -and -not $p.HasExited) { $p | Stop-Process -Force } }
    if ($room -and -not $room.HasExited) { $room | Stop-Process -Force }
}
