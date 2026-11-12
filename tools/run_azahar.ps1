# Boots a .3dsx in Azahar, plays a script of key presses / waits / window captures, then closes
# the emulator. The method comes from the NFSU2/NFSHP2 ports:
#   * pass the .3dsx, never the .cia (Azahar can't boot a CIA from a path argument);
#   * capture with PrintWindow(PW_RENDERFULLCONTENT) after SetProcessDPIAware (BitBlt of the
#     GPU-composited window is black; without DPI awareness the capture loses the bottom screen);
#   * keys go in with PostMessage, so Azahar never needs the foreground.
# Azahar's default keyboard map: A=A B=S X=Z Y=X L=Q R=W START=M SELECT=N, D-pad T/G/F/H,
# circle pad = arrow keys.
#
# Usage: tools/run_azahar.ps1 -Target "3DS Files\ModelView.3dsx" -Script "wait:6,shot:boot,key:W,wait:1,shot:next"
#   steps: wait:<seconds>  key:<letter|UP|DOWN|LEFT|RIGHT>[:<hold ms>]  shot:<name>
param(
    [string]$Target = "3DS Files\PixelRacing-3DS.3dsx",
    [string]$Script = "wait:8,shot:boot",
    [string]$OutDir = "",
    [string]$AzaharExe = "C:\Program Files\Azahar\azahar.exe"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Target)) { $Target = Join-Path $root $Target }
if ($OutDir -eq "") { $OutDir = Join-Path $env:TEMP "pixelracing_azahar" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (-not (Test-Path $Target)) { throw "not found: $Target" }

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Drawing;
public class PrCap {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    static uint sPid; static IntPtr sKeep; static int sClosed;
    public static int DismissPopups(uint pid, IntPtr keep) {
        sPid = pid; sKeep = keep; sClosed = 0;
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint q; GetWindowThreadProcessId(h, out q);
            if (q != sPid || h == sKeep || !IsWindowVisible(h)) { return true; }
            StringBuilder c = new StringBuilder(256); GetClassName(h, c, 256);
            if (!c.ToString().StartsWith("Qt")) { return true; }
            StringBuilder t = new StringBuilder(256); GetWindowText(h, t, 256);
            string title = t.ToString();
            if (title.Length == 0 || title.StartsWith("Azahar")) { return true; }
            PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero);
            sClosed++;
            return true;
        }, IntPtr.Zero);
        return sClosed;
    }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public static void Key(IntPtr h, int vk, int ms) {
        PostMessage(h, 0x0100, (IntPtr)vk, IntPtr.Zero);
        System.Threading.Thread.Sleep(ms);
        PostMessage(h, 0x0101, (IntPtr)vk, IntPtr.Zero);
    }
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
[void][PrCap]::SetProcessDPIAware()

$vk = @{ UP = 38; DOWN = 40; LEFT = 37; RIGHT = 39 }
Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
$proc = Start-Process -FilePath $AzaharExe -ArgumentList "`"$Target`"" -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    $proc.Refresh()
    $hwnd = $proc.MainWindowHandle
}
if ($hwnd -eq [IntPtr]::Zero) { throw "Azahar window did not appear" }
try {
    foreach ($step in $Script.Split(',')) {
        $parts = $step.Trim().Split(':')
        [void][PrCap]::DismissPopups([uint32]$proc.Id, $hwnd)
        switch ($parts[0]) {
            "wait" { Start-Sleep -Milliseconds ([int]([double]$parts[1] * 1000)) }
            "key" {
                $k = $parts[1].ToUpper()
                $code = if ($vk.ContainsKey($k)) { $vk[$k] } else { [int][char]$k }
                $ms = if ($parts.Count -gt 2) { [int]$parts[2] } else { 140 }
                [PrCap]::Key($hwnd, $code, $ms)
            }
            "shot" {
                $png = Join-Path $OutDir ($parts[1] + ".png")
                [PrCap]::Capture($hwnd, $png)
                Write-Host "shot $png"
            }
        }
        if ($proc.HasExited) { Write-Host "Azahar exited (code $($proc.ExitCode))"; break }
    }
} finally {
    if (-not $proc.HasExited) { $proc | Stop-Process -Force }
}
