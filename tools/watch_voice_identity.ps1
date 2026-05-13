param(
    [int]$Tail = 800,
    [int]$IntervalSeconds = 5,
    [switch]$EnableProbe,
    [switch]$DisableProbeOnExit,
    [switch]$Once
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Report = Join-Path $RepoRoot "tools\voice_identity_report.py"
$Probe = Join-Path $RepoRoot "tools\voice_identity_probe.ps1"
$Log = "C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log"

if ($EnableProbe) {
    powershell -NoProfile -ExecutionPolicy Bypass -File $Probe -Enable | Write-Host
}

try {
    $lastLength = -1
    $lastWrite = [datetime]::MinValue
    while ($true) {
        if (Test-Path $Log) {
            $item = Get-Item $Log
            if ($item.Length -ne $lastLength -or $item.LastWriteTime -ne $lastWrite) {
                $lastLength = $item.Length
                $lastWrite = $item.LastWriteTime
                Clear-Host
                Write-Host ("DollmanMute voice identity watch  {0}  size={1}" -f $item.LastWriteTime, $item.Length)
                python $Report --tail $Tail
                if ($Once) {
                    break
                }
            }
        } else {
            Write-Host "Waiting for log: $Log"
            if ($Once) {
                break
            }
        }
        Start-Sleep -Seconds $IntervalSeconds
    }
}
finally {
    if ($DisableProbeOnExit) {
        powershell -NoProfile -ExecutionPolicy Bypass -File $Probe -Disable | Write-Host
    }
}
