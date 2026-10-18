param(
    [Parameter(Mandatory=$true)][string]$JocCoreSource
)
$ErrorActionPreference = 'Stop'
$headers = @("joc_core.h", "joc_stream.h", "joc_version.h")
$failures = 0
foreach ($header in $headers) {
    $local = Join-Path $PSScriptRoot "..\third_party\joc\include\$header"
    $source = Join-Path $JocCoreSource "include\$header"
    if (-not (Test-Path $source)) { Write-Output "missing $source"; $failures++; continue }
    $a = (Get-FileHash $local -Algorithm SHA256).Hash
    $b = (Get-FileHash $source -Algorithm SHA256).Hash
    if ($a -eq $b) {
        Write-Output "same    $header  $a"
    } else {
        Write-Output "DIFFERS $header  $a != $b"
        $failures++
    }
}
if ($failures -ne 0) { throw "$failures header(s) differ from the source checkout" }
Write-Output "public headers are byte-identical to $JocCoreSource"
