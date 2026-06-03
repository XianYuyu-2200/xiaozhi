param(
    [string] $ThreadId = "019e8d22-1932-79c2-b90b-8a7a977030ec",
    [string] $HostName = "192.168.0.26",
    [int] $Port = 3333,
    [string] $Python = "python"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$Watcher = Join-Path $PSScriptRoot "codex_moji_watcher.py"
$OutLog = Join-Path $Root "codex_moji_watcher.out.log"
$ErrLog = Join-Path $Root "codex_moji_watcher.err.log"

$argsList = @(
    "-u",
    $Watcher,
    "--thread-id", $ThreadId,
    "--host", $HostName,
    "--port", "$Port",
    "--from-now"
)

$process = Start-Process -FilePath $Python `
    -ArgumentList $argsList `
    -WorkingDirectory $Root `
    -RedirectStandardOutput $OutLog `
    -RedirectStandardError $ErrLog `
    -WindowStyle Hidden `
    -PassThru

Write-Host "Started Codex Moji watcher, PID=$($process.Id)"
Write-Host "Output log: $OutLog"
Write-Host "Error log: $ErrLog"
