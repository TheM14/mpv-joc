# Build a patched mpv from a clean mpv checkout.
#
#   .\tools\build_fork.ps1                      # latest master, full build
#   .\tools\build_fork.ps1 -MpvRef v0.41.0      # a tag instead
#   .\tools\build_fork.ps1 -SkipBuild           # only clone + apply
#
# The mpv tree is cloned once into -WorkDir and then reused; the edits come
# from tools\apply_edits.py, which anchors on surrounding code instead of line
# numbers, so it keeps working across mpv revisions.
param(
    [string]$MpvRef = "master",
    [string]$WorkDir = (Join-Path $PSScriptRoot "..\..\..\_forkwork"),
    [string]$MsysRoot = $(if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "C:\msys64" }),
    [string]$JocCoreDir = (Join-Path $PSScriptRoot "..\..\build\joc_core\Release"),
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$mpv = Join-Path $WorkDir "mpv"

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
if (-not (Test-Path (Join-Path $mpv ".git"))) {
    git clone --depth 1 --branch $MpvRef https://github.com/mpv-player/mpv.git $mpv
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
} else {
    git -C $mpv fetch --depth 1 origin $MpvRef
    git -C $mpv checkout -f FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed" }
}
git -C $mpv reset --hard | Out-Null
Write-Host ("mpv: " + (git -C $mpv log -1 --format='%h %cd %s' --date=short))

Copy-Item (Join-Path $repo "files\audio\decode\*") (Join-Path $mpv "audio\decode\") -Force
python (Join-Path $PSScriptRoot "apply_edits.py") $mpv
if ($LASTEXITCODE -ne 0) { throw "apply_edits.py failed" }

if ($SkipBuild) { Write-Host "patched tree: $mpv"; exit 0 }

$bash = Join-Path $MsysRoot "usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    throw "MSYS2 not found at $MsysRoot; pass -MsysRoot or set MSYS2_ROOT"
}
$mpvU = $mpv -replace '\\','/'
$coreU = $JocCoreDir -replace '\\','/'
$lines = @(
    'set -e',
    'export PATH=/mingw64/bin:/usr/bin:$PATH',
    'export MSYSTEM=MINGW64',
    "cd $mpvU",
    'rm -rf build',
    "meson setup build --buildtype=release -Dgpl=false -Dlua=luajit -Djoc-core-dir=$coreU",
    'ninja -C build'
)
$shell = $lines -join [Environment]::NewLine
$shell | & $bash -lc "cat > /tmp/build_fork.sh && bash /tmp/build_fork.sh"
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$bundle = ($repo -replace '\\','/') + "/tools/bundle_dlls.sh"
& $bash -lc "'$bundle' '$mpvU/build'"
Copy-Item (Join-Path $JocCoreDir "joc_core.dll") (Join-Path $mpv "build") -Force
Write-Host ("built: " + (Join-Path $mpv "build\mpv.exe"))
