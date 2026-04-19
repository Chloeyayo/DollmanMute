param(
    [string]$Zig,
    [switch]$CoreOnly
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$modsDir = Split-Path -Parent $root
$gameRoot = Split-Path -Parent $modsDir
$outDir = Join-Path $root 'build'
$destAsi = Join-Path $gameRoot 'DollmanMute.asi'
$destCore = Join-Path $gameRoot 'DollmanMuteCore.dll'
$destIni = Join-Path $gameRoot 'DollmanMute.ini'
$srcIni = Join-Path $root 'DollmanMute.ini'
$unloadFlag = Join-Path $gameRoot 'DollmanMute.unload'
$loadFlag = Join-Path $gameRoot 'DollmanMute.load'

$zigCandidates = @()
if ($Zig) { $zigCandidates += $Zig }
if ($env:ZIG_EXE) { $zigCandidates += $env:ZIG_EXE }
$zigCmd = Get-Command zig -ErrorAction SilentlyContinue
if ($zigCmd) { $zigCandidates += $zigCmd.Source }
$zigCandidates += "$env:TEMP\zig-0.15.2\zig-x86_64-windows-0.15.2\zig.exe"
$zigExe = $zigCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

if (-not $zigExe) {
    throw "zig.exe not found. Pass -Zig or set ZIG_EXE."
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ---------- proxy (DollmanMute.asi) ----------
$proxyOut = Join-Path $outDir 'DollmanMute.asi'
if (-not $CoreOnly) {
    $proxyArgs = @(
        'cc'
        '-std=c17'
        '-O2'
        '-shared'
        "$root\src\proxy_main.c"
        '-lkernel32'
        '-luser32'
        '-o'
        $proxyOut
    )

    & $zigExe @proxyArgs
    if ($LASTEXITCODE -ne 0) {
        throw "proxy build failed with exit code $LASTEXITCODE"
    }
} else {
    Write-Output "Skip proxy build (-CoreOnly)"
}

# ---------- core (DollmanMuteCore.dll) ----------
$coreOut = Join-Path $outDir 'DollmanMuteCore.dll'
$coreArgs = @(
    'cc'
    '-std=c17'
    '-O2'
    '-shared'
    "-I$root\third_party\minhook\include"
    "-I$root\third_party\minhook\src\hde"
    "$root\src\core_main.c"
    "$root\third_party\minhook\src\buffer.c"
    "$root\third_party\minhook\src\hook.c"
    "$root\third_party\minhook\src\trampoline.c"
    "$root\third_party\minhook\src\hde\hde64.c"
    '-lkernel32'
    '-luser32'
    '-ladvapi32'
    '-o'
    $coreOut
)

& $zigExe @coreArgs
if ($LASTEXITCODE -ne 0) {
    throw "core build failed with exit code $LASTEXITCODE"
}

$extraArtifacts = @(
    (Join-Path $outDir 'DollmanMute.pdb'),
    (Join-Path $outDir 'DollmanMute.lib'),
    (Join-Path $outDir 'DollmanMuteCore.pdb'),
    (Join-Path $outDir 'DollmanMuteCore.lib'),
    (Join-Path $outDir 'proxy_main.lib'),
    (Join-Path $outDir 'core_main.lib')
)
foreach ($artifact in $extraArtifacts) {
    if (Test-Path $artifact) {
        Remove-Item -LiteralPath $artifact -Force
    }
}

Copy-Item -LiteralPath $coreOut -Destination $destCore -Force -ErrorAction SilentlyContinue
if (-not $?) {
    # Core locked by running game; ask proxy to unload, then retry.
    Write-Output "Core in use - triggering proxy unload via $unloadFlag"
    New-Item -ItemType File -Path $unloadFlag -Force | Out-Null
    $copied = $false
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 200
        try {
            Copy-Item -LiteralPath $coreOut -Destination $destCore -Force -ErrorAction Stop
            $copied = $true
            break
        } catch {
            # still locked
        }
    }
    if (-not $copied) {
        throw "Core copy failed after 6s of retries (is proxy running?)"
    }
    Write-Output "Core replaced; triggering proxy load via $loadFlag"
    New-Item -ItemType File -Path $loadFlag -Force | Out-Null
}
if (-not $CoreOnly) {
    Copy-Item -LiteralPath $proxyOut -Destination $destAsi -Force
}
if (-not (Test-Path $destIni)) {
    Copy-Item -LiteralPath $srcIni -Destination $destIni
}

if (-not $CoreOnly) {
    Write-Output "Built proxy: $proxyOut"
}
Write-Output "Built core:  $coreOut"
if (-not $CoreOnly) {
    Write-Output "Installed ASI:  $destAsi"
}
Write-Output "Installed Core: $destCore"
Write-Output "Config: $destIni"
