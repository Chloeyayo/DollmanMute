# Changelog

## v2.1.6 hat/refpack hotfix

- Fixed a possible Magellan private-room story-rest softlock by no longer using the Dollman voice delay schedule/closure functions as mute points. Those functions are now pass-through only when probed, because the game also uses that path for talk/voice completion notifications.
- Removed the broad recent-subtitle PostEvent fallback. Voice muting now stays on known Dollman event identities and verified refpack voice-object links, while subtitle muting still uses the sender gameplay pair.
- Added object-level coverage for the verified Dollman gameplay summary and hat/equip lines, including the "You have a flair for this kind of thing huh!" line.

## v2.0 beta

- Added gameplay subtitle muting for Dollman.
- Expanded Dollman gameplay voice muting beyond the original baseline for broader in-game coverage.
- Dollman gameplay chatter, throw / recall lines, and mission-failure gameplay lines are now covered together.
- Outside cutscenes and the private room, known Dollman gameplay voice/subtitle paths are muted.
- Ambient and world audio remain untouched.
- User-facing config was simplified to `EnableVoiceMute`, `EnableSubtitleMute`, and `ScannerMode`.


## Beta Notes

- This is a beta release.
- If you notice missed lines, false positives, or any other bug that affects gameplay, please let me know. Including `DollmanMute.log` from the game root is strongly recommended.
- The current beta supports game version `v1.6`. Future game updates may cause crashes; if that happens, please roll back to mod `v1.2`.
