# Changelog

## v2.1.1

- Fixed `[voice-shared]` log attribution: the two shared-helper return RVAs used to label log lines as `player` or `dollman` were stale carry-overs from `v1.5`. The Player path was refactored on `v1.6` and now flows through `sub_140C743B0`; both return RVAs are realigned to the actual `v1.6` call sites. Mute behavior was already correct - this fix only affects diagnostic log attribution.

## v2.1

- Added support for game version `v1.6`.
- Updated runtime hook RVAs for the `v1.6` executable to fix the crash after the game update.
- Kept the `v2.0 beta` gameplay subtitle mute and expanded gameplay voice mute behavior.
- Added an extra recent-subtitle voice gate for gameplay Dollman chatter variants that were not covered by the fixed event-id path.

## v2.0 beta

- Added gameplay subtitle muting for Dollman.
- Expanded Dollman gameplay voice muting beyond the original baseline for broader in-game coverage.
- Dollman gameplay chatter, throw / recall lines, and mission-failure gameplay lines are now covered together.
- In theory, outside of cutscenes and the private room, Dollman should now be fully muted.
- Ambient and world audio remain untouched.
- User-facing config was simplified to `EnableVoiceMute`, `EnableSubtitleMute`, and `ScannerMode`.


## Notes

- If you notice missed lines, false positives, or any other bug that affects gameplay, please let me know. Including `DollmanMute.log` from the game root is strongly recommended.
- `v2.1` supports game version `v1.6`. Future game updates may cause crashes; if that happens, please roll back to mod `v1.2`.
