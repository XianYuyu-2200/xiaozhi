$ErrorActionPreference = "Stop"

$processes = Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -match "codex_moji_watcher\.py" }

if (-not $processes) {
    Write-Host "No Codex Moji watcher is running."
    exit 0
}

foreach ($process in $processes) {
    Stop-Process -Id $process.ProcessId -Force
    Write-Host "Stopped Codex Moji watcher, PID=$($process.ProcessId)"
}
