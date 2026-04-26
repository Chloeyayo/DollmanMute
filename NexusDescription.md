# DollmanMute

"Did you just drop some cargo Sam?"  
"Did you forget to wipe your ass Sam?"  
BI*CH SHUT THE F UP

## Description

**v2.0 beta notice:** supports game version `v1.5`.

Brief changelog:

- Added Dollman subtitle muting
- Broader Dollman gameplay voice mute coverage

Detailed changelog and update notes are in the discussion section.

**DollmanMute** exists for one reason: to shut Dollman up during normal gameplay.

This mod mutes Dollman's repetitive and annoying gameplay voice lines, including his chatter during normal play, the lines that play when you throw him, and his voice lines after mission failure.

Under normal circumstances, it should not affect Dollman's dialogue in story scenes.

If you notice any missed lines, or anything being muted that should not be, please let me know.

If you can include `DollmanMute.log` from the game root, that helps a lot.

Hideo Kojima, please stop treating your players like idiots. At least give us a choice.

## Installation Instructions

Open your game root folder:

`...\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\`

Copy these files:

- `DollmanMute.asi`
- `DollmanMuteCore.dll`
- `DollmanMute.ini`
- `version.dll`

Then launch the game.

## Main Features


- Mutes all Dollman gameplay audios and subtitles
- Keeps ambient and world audio intact
- Intended not to affect cutscenes or private-room dialogue under normal use
- Lightweight native ASI mod
- No archive unpacking or replacement required

## Requirements

- `DEATH STRANDING 2 - ON THE BEACH`
- No other tools are required for the packaged release

Notes:

- The packaged release already includes `version.dll`
- If another mod already installed a working `version.dll`, you usually do not need to replace it
- No other mods are required

## Future Plans

- Further beta hardening for missed lines, false positives, and edge cases

## Shout Outs

- Thanks to **ThirteenAG** for **Ultimate ASI Loader**
- Thanks to **TsudaKageyu** and contributors for **MinHook**
- Thanks to everyone in the DS2 modding scene who tested, shared findings, and helped narrow this down
