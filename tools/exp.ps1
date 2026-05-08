param(
    [Parameter(Position = 0)]
    [string]$Mode = "combo",
    [Parameter(Position = 1)]
    [string]$Session = "last",
    [Parameter(Position = 2)]
    [int]$Samples = 6,
    [Parameter(Position = 3)]
    [int]$Top = 8,
    [Parameter(Position = 4)]
    [string]$Log = "C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log",
    [switch]$IncludeSelectorNoise
)

$ErrorActionPreference = "Stop"
$validModes = @("sessions", "summary", "show", "combo", "tail", "watch", "help")
$modeAliases = @{
    "ls" = "sessions"
    "list" = "sessions"
    "sum" = "summary"
    "decode" = "show"
    "report" = "combo"
    "last" = "combo"
    "follow" = "watch"
    "?" = "help"
    "h" = "help"
}

if (-not $PSBoundParameters.ContainsKey("Mode") -and
    $PSBoundParameters.ContainsKey("Session") -and
    $validModes -contains $Session.ToLowerInvariant()) {
    $Mode = $Session
    $Session = "last"
}

$modeKey = $Mode.ToLowerInvariant()
if ($modeAliases.ContainsKey($modeKey)) {
    $Mode = $modeAliases[$modeKey]
} else {
    $Mode = $modeKey
}

function Resolve-Python {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) {
        return @($cmd.Source)
    }
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        return @($py.Source, "-3")
    }
    throw "python not found"
}

function Run-PythonScript {
    param(
        [string]$Script,
        [string[]]$ScriptArgs = @()
    )
    $python = @(Resolve-Python)
    if ($python.Count -gt 1) {
        $prefix = @($python[1..($python.Count - 1)])
        & $python[0] @prefix $Script @ScriptArgs
        return
    }
    & $python[0] $Script @ScriptArgs
}

function Show-Usage {
    Write-Output "Usage:"
    Write-Output "  .\tools\exp.ps1 sessions"
    Write-Output "  .\tools\exp.ps1 summary [-Session last|N] [-Top 8]"
    Write-Output "  .\tools\exp.ps1 show [-Session last|N] [-Samples 6]"
    Write-Output "  .\tools\exp.ps1 combo [-Session last|N] [-Samples 6] [-Top 8]"
    Write-Output "  .\tools\exp.ps1 tail [-IncludeSelectorNoise]"
    Write-Output "  .\tools\exp.ps1 watch [-IncludeSelectorNoise]"
    Write-Output ""
    Write-Output "Aliases:"
    Write-Output "  ls -> sessions"
    Write-Output "  sum -> summary"
    Write-Output "  decode -> show"
    Write-Output "  report -> combo"
    Write-Output "  follow -> watch"
    Write-Output ""
    Write-Output "Examples:"
    Write-Output "  .\tools\exp.ps1 sessions"
    Write-Output "  .\tools\exp.ps1 sum last -Top 5"
    Write-Output "  .\tools\exp.ps1 show F8-12 -Samples 3"
    Write-Output "  .\tools\exp.ps1 combo last 3 4"
    Write-Output "  .\tools\exp.ps1 watch"
}

function Get-InterestingPattern {
    $parts = @(
        "=== session boundary",
        "StrategySession",
        "SubtitleHit",
        "Muted subtitle",
        "\[strategy\]",
        "\[show\]",
        "\[voice-",
        "\[stf-",
        "Hotkey"
    )
    if ($IncludeSelectorNoise) {
        $parts += "\[dafad0\]"
    }
    return ($parts -join "|")
}

function Show-FilteredLog {
    param(
        [switch]$Wait
    )
    $pattern = Get-InterestingPattern
    if ($Wait) {
        Get-Content -Path $Log -Tail 80 -Wait |
            Select-String -Pattern $pattern |
            ForEach-Object { $_.Line }
        return
    }
    Get-Content -Path $Log -Tail 250 |
        Select-String -Pattern $pattern |
        ForEach-Object { $_.Line }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$strategyScript = Join-Path $PSScriptRoot "strategy_session_report.py"
$showScript = Join-Path $PSScriptRoot "live_show_decode.py"

switch ($Mode.ToLowerInvariant()) {
    "sessions" {
        Run-PythonScript -Script $showScript -ScriptArgs @("--log", $Log, "--list-sessions")
    }
    "summary" {
        Run-PythonScript -Script $strategyScript -ScriptArgs @("--log", $Log, "--session", $Session, "--top", "$Top")
    }
    "show" {
        Run-PythonScript -Script $showScript -ScriptArgs @("--log", $Log, "--session", $Session, "--samples", "$Samples")
    }
    "combo" {
        Write-Output "== Strategy Summary =="
        Run-PythonScript -Script $strategyScript -ScriptArgs @("--log", $Log, "--session", $Session, "--top", "$Top")
        Write-Output ""
        Write-Output "== Show Decode =="
        Run-PythonScript -Script $showScript -ScriptArgs @("--log", $Log, "--session", $Session, "--samples", "$Samples")
    }
    "tail" {
        Show-FilteredLog
    }
    "watch" {
        Show-FilteredLog -Wait
    }
    "help" {
        Show-Usage
    }
    default {
        Show-Usage
        exit 1
    }
}
