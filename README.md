# DollmanMute

`DollmanMute` is an ASI mod for `DEATH STRANDING 2 - ON THE BEACH` that mutes Dollman gameplay voice lines and subtitles.

It does not replace game archives or modify packaged assets.

## What It Does

- mutes Dollman gameplay voice lines
- mutes Dollman gameplay subtitles with a current-build runtime hook
- keeps ambient/world audio intact
- works as a lightweight native ASI mod

## Requirements

- `DEATH STRANDING 2 - ON THE BEACH`
- no additional tools required for the packaged release

## Installation

1. Copy `DollmanMute.asi` to the game root folder
2. Copy `DollmanMuteCore.dll` to the game root folder
3. Copy `DollmanMute.ini` to the game root folder
4. Copy `version.dll` to the game root folder if you do not already have a working ASI loader

Game root example:

`...\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\`

## Configuration

`DollmanMute.ini`

- `Enabled=1` enables the mod
- `VerboseLog=0` is recommended for normal use
- `VerboseLog=1` enables extra logging for troubleshooting
- `EnableVoiceMute=1` enables Dollman voice muting
- `EnableSubtitleMute=1` enables Dollman subtitle muting
- `ScannerMode=0` keeps scanner audio unchanged, `1` reduces it, `2` fully mutes it

## Hotkeys

- `F8` marks a fresh probe session window in `DollmanMute.log`

## Troubleshooting

If the mod does not seem to load:

1. Make sure the files are in the game root, not inside a subfolder
2. Make sure `version.dll` is present in the game root
3. Launch the game and check whether `DollmanMute.log` appears in the game root

If another mod already installed a working `version.dll`, you usually do not need to replace it.

## After Game Updates

If the game receives an update, check this repository or the latest release page for the newest compatible version of the mod.

## Future Plans

- tighter current-build semantic muting above the subtitle tail
