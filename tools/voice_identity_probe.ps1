param(
    [switch]$Enable,
    [switch]$Disable,
    [switch]$Status,
    [string]$ConfigPath
)

$ErrorActionPreference = "Stop"

if (-not $ConfigPath) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $modsRoot = Split-Path -Parent $repoRoot
    $gameRoot = Split-Path -Parent $modsRoot
    $ConfigPath = Join-Path $gameRoot "DollmanMute.ini"
}

function Read-IniValue {
    param([string[]]$Lines, [string]$Key, [string]$Default)
    foreach ($line in $Lines) {
        if ($line -match "^\s*$([regex]::Escape($Key))\s*=\s*(.+?)\s*$") {
            return $Matches[1]
        }
    }
    return $Default
}

function Set-IniValue {
    param([string[]]$Lines, [string]$Key, [string]$Value)
    $out = New-Object System.Collections.Generic.List[string]
    $replaced = $false
    $inGeneral = $false
    $inserted = $false

    foreach ($line in $Lines) {
        if ($line -match '^\s*\[General\]\s*$') {
            $inGeneral = $true
            $out.Add($line)
            continue
        }
        if ($inGeneral -and $line -match '^\s*\[.+\]\s*$') {
            if (-not $replaced -and -not $inserted) {
                $out.Add("$Key=$Value")
                $inserted = $true
            }
            $inGeneral = $false
        }
        if ($inGeneral -and $line -match "^\s*$([regex]::Escape($Key))\s*=") {
            $out.Add("$Key=$Value")
            $replaced = $true
        } else {
            $out.Add($line)
        }
    }

    if (-not $replaced -and -not $inserted) {
        if (-not ($Lines | Where-Object { $_ -match '^\s*\[General\]\s*$' })) {
            $out.Add("[General]")
        }
        $out.Add("$Key=$Value")
    }
    return $out.ToArray()
}

if (($Enable -and $Disable) -or (-not $Enable -and -not $Disable -and -not $Status)) {
    throw "Use exactly one mode: -Enable, -Disable, or -Status."
}

if (-not (Test-Path -LiteralPath $ConfigPath)) {
    throw "Config not found: $ConfigPath"
}

$lines = Get-Content -LiteralPath $ConfigPath

if ($Enable) {
    $lines = Set-IniValue -Lines $lines -Key "EnableVoiceQueueIdentityProbe" -Value "1"
    $lines = Set-IniValue -Lines $lines -Key "HookVoiceQueueSubmit" -Value "1"
    Set-Content -LiteralPath $ConfigPath -Value $lines -Encoding ASCII
} elseif ($Disable) {
    $lines = Set-IniValue -Lines $lines -Key "EnableVoiceQueueIdentityProbe" -Value "0"
    $lines = Set-IniValue -Lines $lines -Key "HookVoiceQueueSubmit" -Value "0"
    Set-Content -LiteralPath $ConfigPath -Value $lines -Encoding ASCII
}

$lines = Get-Content -LiteralPath $ConfigPath
$probe = Read-IniValue -Lines $lines -Key "EnableVoiceQueueIdentityProbe" -Default "0"
$hook = Read-IniValue -Lines $lines -Key "HookVoiceQueueSubmit" -Default "0"
Write-Host "Config: $ConfigPath"
Write-Host "EnableVoiceQueueIdentityProbe=$probe"
Write-Host "HookVoiceQueueSubmit=$hook"
