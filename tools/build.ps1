# Builds the port in "3DS Files" from space-free drive letters.
#
# devkitPro's Makefile template breaks on paths with spaces ($(notdir $(CURDIR)), VPATH, -f
# $(CURDIR)/Makefile), and both "3DS Files" and the folder above this project have one. The
# script `subst`s one free letter onto "3DS Files" (where make runs) and another onto the
# repository root (for the TOOLS path the Makefile uses), runs make inside devkitPro's msys2
# bash, and always unmaps both.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/build.ps1 [make args...]
#        e.g. tools/build.ps1            -> 3DS Files/PixelRacing-3DS.3dsx
#             tools/build.ps1 cia        -> 3DS Files/PixelRacing-3DS.cia
#             tools/build.ps1 MODELVIEW=1
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$MakeArgs)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root "3DS Files"

$taken = @()
foreach ($line in (& subst.exe)) {
    if ($line -match '^([A-Za-z]):') { $taken += ($matches[1].ToUpper() + ":") }
}
$free = @()
foreach ($cand in @("P:", "O:", "N:", "M:", "L:", "K:", "J:", "Q:", "R:", "S:")) {
    if (($taken -notcontains $cand) -and -not (Test-Path ($cand + "\"))) { $free += $cand }
}
if ($free.Count -lt 2) { throw "need two free drive letters for the build" }
$drive = $free[0]       # -> 3DS Files
$rootDrive = $free[1]   # -> repository root

# .d dependency files record the drive; a different letter than last time means stale objects.
foreach ($b in @("build", "build_mv")) {
    $dir = Join-Path $proj $b
    $stamp = Join-Path $dir ".subst-drive"
    if ((Test-Path $dir) -and ((-not (Test-Path $stamp)) -or ((Get-Content $stamp -Raw).Trim() -ne $drive))) {
        Remove-Item $dir -Recurse -Force
    }
}

& subst.exe $drive $proj
if ($LASTEXITCODE -ne 0) { throw "subst $drive failed" }
try {
    & subst.exe $rootDrive $root
    if ($LASTEXITCODE -ne 0) { throw "subst $rootDrive failed" }
    try {
        $msys = "/" + $drive.Substring(0, 1).ToLower()
        $tools = "/" + $rootDrive.Substring(0, 1).ToLower() + "/tools"
        # msys2's login shell does not see the Windows Python; hand make its full path.
        $py = (& python -c "import sys; print(sys.executable)").Trim() -replace '\\', '/'
        if ($py -match '^([A-Za-z]):(.*)$') { $py = "/" + $matches[1].ToLower() + $matches[2] }
        $script = Join-Path $env:TEMP "pixelracing_build.sh"
        @"
cd $msys
make TOOLS=$tools PYTHON='$py' $($MakeArgs -join ' ')
rc=`$?
for b in build build_mv; do [ -d `$b ] && echo $drive > `$b/.subst-drive; done
exit `$rc
"@ | Set-Content -Path $script -Encoding ASCII -NoNewline
        & "C:\devkitPro\msys2\usr\bin\bash.exe" -l $script
        $rc = $LASTEXITCODE
        Remove-Item $script -ErrorAction SilentlyContinue
    } finally {
        & subst.exe $rootDrive /D
    }
} finally {
    & subst.exe $drive /D
}
if ($rc -ne 0) { throw "make failed (exit $rc)" }

# Shader / relocation sanity checks (both catch faults that only show on real hardware).
$shbins = @(Get-ChildItem $proj -Recurse -Filter *.shbin -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\build(_mv)?\\' } | ForEach-Object { $_.FullName })
if ($shbins.Count) {
    & python (Join-Path $PSScriptRoot "check_shbin.py") @shbins
    if ($LASTEXITCODE -ne 0) { throw "check_shbin: a shader would freeze the GPU" }
}
foreach ($x in @(Get-ChildItem $proj -Filter *.3dsx -ErrorAction SilentlyContinue)) {
    & python (Join-Path $PSScriptRoot "check_3dsx.py") $x.FullName
    if ($LASTEXITCODE -ne 0) { throw "check_3dsx: $($x.Name) would not load" }
}
