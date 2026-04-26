# Changelog

## v2.0 beta

- Added gameplay subtitle muting for Dollman.
- Expanded Dollman gameplay voice muting beyond the original baseline for broader in-game coverage.
- Dollman gameplay chatter, throw / recall lines, and mission-failure gameplay lines are now covered together.
- In theory, outside of cutscenes and the private room, Dollman should now be fully muted.
- Ambient and world audio remain untouched.
- User-facing config was simplified to `EnableVoiceMute`, `EnableSubtitleMute`, and `ScannerMode`.


## Beta Notes

- This is a beta release.
- If you notice missed lines, false positives, or any other bug that affects gameplay, please let me know. Including `DollmanMute.log` from the game root is strongly recommended.
- The current beta supports game version `v1.5`. Future game updates may cause crashes; if that happens, please roll back to mod `v1.2`.
