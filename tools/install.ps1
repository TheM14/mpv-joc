param(
    [Parameter(Mandatory=$true)][string]$MpvDir,
    [string]$Hrtf = "",
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [string]$Configuration = "Release"
)
$ErrorActionPreference = 'Stop'
$mpv = (Resolve-Path $MpvDir).Path
$plugin = Join-Path $BuildDir "$Configuration\joc_mpv.dll"
if (-not (Test-Path $plugin)) { $plugin = Join-Path $BuildDir "joc_mpv.dll" }
if (-not (Test-Path $plugin)) { throw "joc_mpv.dll not found under $BuildDir; run tools/build.ps1 first" }
$core = Join-Path (Split-Path $plugin) "joc_core.dll"
if (-not (Test-Path $core)) { throw "joc_core.dll not found next to the plugin; build with -JocCoreSource or copy it there" }

$scripts = Join-Path $mpv "scripts"
New-Item -ItemType Directory -Force -Path $scripts | Out-Null
Copy-Item $plugin (Join-Path $scripts "joc_mpv.dll") -Force
Copy-Item $core (Join-Path $mpv "joc_core.dll") -Force
Write-Output "installed $(Join-Path $scripts 'joc_mpv.dll')"
Write-Output "installed $(Join-Path $mpv 'joc_core.dll')"

if ($Hrtf -ne "") {
    $target = Join-Path $mpv "HRTF"
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    $name = if ($Hrtf -match "personalized_headphone$") { "binaural.personalized_headphone" } else { "binaural.sofa" }
    Copy-Item $Hrtf (Join-Path $target $name) -Force
    Write-Output "installed $(Join-Path $target $name)"
}

Write-Output ""
Write-Output "add this line to $(Join-Path $mpv 'mpv.conf'):"
Write-Output "  script-opts=joc-output=binaural,joc-hrtf-source=sofa"
