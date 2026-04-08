# DollmanMute

`DollmanMute` is a native `ASI + MinHook` mod for `DEATH STRANDING 2 - ON THE BEACH`.
It mutes Dollman voice lines without modifying the original archive files.

The release build keeps the implementation intentionally narrow:

- exact `Wwise PostEventID` blocking for confirmed Dollman voice events
- exact `PlayerVoice` hash blocking for confirmed Dollman chatter paths
- no timing windows
- no hotkey-driven tracing
- no runtime learning / heuristic muting

## Build

Run this from the game root:

```powershell
powershell -ExecutionPolicy Bypass -File .\mods\DollmanMute\build.ps1
```

If `zig.exe` is not in `PATH`, pass it explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File .\mods\DollmanMute\build.ps1 -Zig C:\path\to\zig.exe
```

## Installation Output

The build script installs these files into the game root:

- `DollmanMute.asi`
- `DollmanMute.ini`

If you are already using `Ultimate ASI Loader` through `version.dll`, no extra injector is required.
Some release bundles may also include a matching `version.dll` for convenience. If you do not already have an ASI loader installed, copy that file to the game root as well.

## Config

`DollmanMute.ini` currently exposes:

- `Enabled`
- `VerboseLog`

`VerboseLog=0` is recommended for normal use.
Set `VerboseLog=1` only when collecting debug logs.

## Log

The mod writes to:

- `DollmanMute.log`

With default settings, the log stays minimal and mainly records startup information.

## Scope

This build is meant to be a clean public release, not a research build.

It does not include:

- hotkey markers
- wide runtime tracing
- string scans
- heuristic recent-object muting
- automatic runtime learning tables

If a future game update changes the underlying voice routes, new exact IDs or hashes may need to be added.
