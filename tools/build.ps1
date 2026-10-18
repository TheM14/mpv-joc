param(
    [string]$JocCoreSource = "",
    [string]$Configuration = "Release",
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build")
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Find-CMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) {
            $candidate = Join-Path $install "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "cmake not found; install CMake or the Visual Studio C++ tools"
}

$cmake = Find-CMake
$configure = @("-S", $root, "-B", $BuildDir, "-A", "x64")
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $configure = @("-S", $root, "-B", $BuildDir, "-G", "Ninja")
} else {
    $configure += @("-G", "Visual Studio 17 2022")
}
if ($JocCoreSource -ne "") {
    $configure += "-DJOC_CORE_SOURCE=$JocCoreSource"
}
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& $cmake --build $BuildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$dll = Join-Path $BuildDir "$Configuration\joc_mpv.dll"
if (-not (Test-Path $dll)) { $dll = Join-Path $BuildDir "joc_mpv.dll" }
Write-Output ""
Write-Output "plugin: $dll"
if (Test-Path (Join-Path (Split-Path $dll) "joc_core.dll")) {
    Write-Output "core:   $(Join-Path (Split-Path $dll) 'joc_core.dll')"
} else {
    Write-Output "core:   not next to the plugin; build with -JocCoreSource <JustOneCacophonyCXX> or copy joc_core.dll there"
}
