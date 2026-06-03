param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet("booting", "ready", "thinking", "listening", "working", "coding", "testing", "done", "error", "sleeping")]
    [string] $State,

    [Parameter(Position = 1)]
    [string] $HostName = "255.255.255.255",

    [int] $Port = 3333
)

$ErrorActionPreference = "Stop"

$udp = [System.Net.Sockets.UdpClient]::new()
try {
    $udp.EnableBroadcast = $true
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($State)
    [void] $udp.Send($bytes, $bytes.Length, $HostName, $Port)
    Write-Host "Sent '$State' to $HostName`:$Port"
} finally {
    $udp.Dispose()
}
