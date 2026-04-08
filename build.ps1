param(
    [string]$Zig
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$modsDir = Split-Path -Parent $root
$gameRoot = Split-Path -Parent $modsDir
$outDir = Join-Path $root 'build'
$destAsi = Join-Path $gameRoot 'DollmanMute.asi'
$destIni = Join-Path $gameRoot 'DollmanMute.ini'
$srcIni = Join-Path $root 'DollmanMute.ini'

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

$target = Join-Path $outDir 'DollmanMute.asi'
$args = @(
    'cc'
    '-std=c17'
    '-O2'
    '-shared'
    "-I$root\third_party\minhook\include"
    "-I$root\third_party\minhook\src\hde"
    "$root\src\dllmain.c"
    "$root\third_party\minhook\src\buffer.c"
    "$root\third_party\minhook\src\hook.c"
    "$root\third_party\minhook\src\trampoline.c"
    "$root\third_party\minhook\src\hde\hde64.c"
    '-lkernel32'
    '-luser32'
    '-ladvapi32'
    '-o'
    $target
)

& $zigExe @args
if ($LASTEXITCODE -ne 0) {
    throw "build failed with exit code $LASTEXITCODE"
}

$extraArtifacts = @(
    (Join-Path $outDir 'DollmanMute.pdb'),
    (Join-Path $outDir 'dllmain.lib')
)
foreach ($artifact in $extraArtifacts) {
    if (Test-Path $artifact) {
        Remove-Item -LiteralPath $artifact -Force
    }
}

Copy-Item -LiteralPath $target -Destination $destAsi -Force
if (-not (Test-Path $destIni)) {
    Copy-Item -LiteralPath $srcIni -Destination $destIni
}

Write-Output "Built: $target"
Write-Output "Installed ASI: $destAsi"
Write-Output "Config: $destIni"
