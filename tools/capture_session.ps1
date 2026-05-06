param(
    [Parameter(Position = 0)]
    [string]$Label = "manual",

    [int]$DurationSec = 8,

    [ValidateSet("none", "sam", "throw", "sample")]
    [string]$AutoAction = "none",

    [string]$Log = "C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$gameRoot = Split-Path -Parent (Split-Path -Parent $repoRoot)
$sessionTrigger = Join-Path $gameRoot "DollmanMute.session"
$goalAudit = Join-Path $PSScriptRoot "goal_audit.py"
$gameInput = Join-Path $PSScriptRoot "game_input.ps1"

if (-not (Test-Path $Log)) {
    throw "log not found: $Log"
}

$before = Get-Content -Path $Log -ErrorAction Stop
$beforeCount = $before.Count

New-Item -Path $sessionTrigger -ItemType File -Force | Out-Null
Write-Output "capture label=$Label"
Write-Output "created DollmanMute.session; trigger the in-game action now"
if ($AutoAction -ne "none") {
    if (-not (Test-Path $gameInput)) {
        throw "game input helper not found: $gameInput"
    }
    Write-Output "auto action: $AutoAction"
    switch ($AutoAction) {
        "sam" {
            & $gameInput focus | Out-Null
            Start-Sleep -Milliseconds 250
            & $gameInput send-sam | Out-Null
        }
        "throw" {
            & $gameInput focus | Out-Null
            Start-Sleep -Milliseconds 250
            & $gameInput send-throw -HoldRightMs 1600 | Out-Null
        }
        "sample" {
            & $gameInput send-sam | Out-Null
            Start-Sleep -Milliseconds 1500
            New-Item -Path $sessionTrigger -ItemType File -Force | Out-Null
            Start-Sleep -Milliseconds 350
            & $gameInput send-throw -HoldRightMs 1600 | Out-Null
        }
    }
}
Start-Sleep -Seconds $DurationSec

$after = Get-Content -Path $Log -ErrorAction Stop
$newLines = @()
if ($after.Count -gt $beforeCount) {
    $newLines = $after[$beforeCount..($after.Count - 1)]
}

$pattern = "session boundary|Session trigger|SubtitleHit|SubtitleKey|Muted subtitle|\[show\]|\[stf-|\[voice-entry|voice-entry-match|\[postevent\]|Blocked PostEventID|RefpackDollmanPostEvent|refpack-voice-object"
$filtered = $newLines | Select-String -Pattern $pattern | ForEach-Object { $_.Line }

Write-Output ""
Write-Output "== New Interesting Lines =="
if ($filtered.Count -eq 0) {
    Write-Output "(none)"
} else {
    $filtered | Select-Object -First 160
    if ($filtered.Count -gt 160) {
        Write-Output "... +$($filtered.Count - 160) more"
    }
}

Write-Output ""
Write-Output "== Goal Audit =="
python $goalAudit --log $Log
