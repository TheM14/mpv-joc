param(
    [Parameter(Mandatory=$true)][string]$Media,
    [Parameter(Mandatory=$true)][string]$MpvDir,
    [string]$JocCli = (Join-Path $PSScriptRoot "..\build\joc_core\Release\joc_cli.exe"),
    [string]$Plugin = (Join-Path $PSScriptRoot "..\build\Release\joc_mpv.dll"),
    [string]$Sofa = "",
    [string]$Rosella = "",
    [string]$WorkDir = (Join-Path $PSScriptRoot "..\_parity"),
    [string]$Python = "python"
)
$ErrorActionPreference = 'Stop'
$mpv = Join-Path $MpvDir "mpv.com"
if (-not (Test-Path $mpv)) { throw "mpv.com not found in $MpvDir" }
if (-not (Test-Path $JocCli)) { throw "joc_cli not found at $JocCli" }
if (-not (Test-Path $Plugin)) { throw "joc_mpv.dll not found at $Plugin" }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$log = Join-Path (Split-Path $Plugin) "joc_decoder.log"

function Invoke-Capture([string]$options, [string]$output) {
    Remove-Item -Force $log -ErrorAction SilentlyContinue
    $arguments = @("--no-config", "--ao=null", "--vo=null", "--pause=yes",
        "--script=$Plugin", "--script-opts=joc-verbose=yes,joc-attach=no,joc-log=$log,$options",
        "--msg-level=all=info", $Media)
    Start-Process -FilePath $mpv -ArgumentList $arguments -PassThru -WindowStyle Hidden | Out-Null
    $url = $null
    for ($i = 0; $i -lt 120; $i++) {
        Start-Sleep -Milliseconds 250
        if (Test-Path $log) {
            $match = Select-String -Path $log -Pattern 'http://127\.0\.0\.1:\d+/joc\.wav' | Select-Object -Last 1
            if ($match) { $url = ([regex]::Match($match.Line, 'http://127\.0\.0\.1:\d+/joc\.wav')).Value; break }
        }
    }
    if (-not $url) {
        Get-Process mpv -ErrorAction SilentlyContinue | Stop-Process -Force
        throw "the plugin did not publish an audio URL"
    }
    & $Python (Join-Path $PSScriptRoot "fetch.py") $url $output
    Get-Process mpv -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

$failures = 0
if ($Sofa -ne "") {
    Write-Output "=== binaural float32 (SOFA) ==="
    $reference = Join-Path $WorkDir "reference.binaural.f32.wav"
    $actual = Join-Path $WorkDir "plugin.binaural.f32.wav"
    & $JocCli $Media --binaural --sofa-hrtf $Sofa --binaural-format float32 --binaural-output $reference | Out-Null
    Invoke-Capture "joc-output=binaural,joc-hrtf=$Sofa" $actual
    & $Python (Join-Path $PSScriptRoot "compare_wav.py") $actual $reference
    if ($LASTEXITCODE -ne 0) { $failures++ }
}
if ($Rosella -ne "") {
    Write-Output "=== binaural int24 (Rosella) ==="
    $reference = Join-Path $WorkDir "reference.binaural.i24.wav"
    $actual = Join-Path $WorkDir "plugin.binaural.i24.wav"
    & $JocCli $Media --binaural --personalized-headphone $Rosella --binaural-format int24 --clip-action continue --binaural-output $reference | Out-Null
    Invoke-Capture "joc-int24=yes,joc-hrtf-source=rosella,joc-hrtf=$Rosella" $actual
    & $Python (Join-Path $PSScriptRoot "compare_wav.py") $actual $reference
    if ($LASTEXITCODE -ne 0) { $failures++ }
}
Write-Output "=== speaker 7.1 float32 ==="
$reference = Join-Path $WorkDir "reference.7.1.f32.wav"
$actual = Join-Path $WorkDir "plugin.7.1.f32.wav"
& $JocCli $Media --speaker-layout 7.1 --speaker-format float32 --speaker-output $reference | Out-Null
Invoke-Capture "joc-output=speaker,joc-layout=7.1" $actual
& $Python (Join-Path $PSScriptRoot "compare_wav.py") $actual $reference
if ($LASTEXITCODE -ne 0) { $failures++ }

if ($failures -ne 0) { throw "$failures comparison(s) failed" }
Write-Output "all comparisons passed"
