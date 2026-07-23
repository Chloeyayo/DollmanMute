# Changelog

## v3.3.2 — Sign-only PostEvent install fix

- Fixed the non-dialogue-tick init path so `AK::SoundEngine::PostEvent(ID)` stays installed whenever `Enabled=1`. Previously that path only installed the hook when voice mute or scanner mute was already on, which silently broke **sign-only** configs (`EnableVoiceMute=0` / `EnableSubtitleMute=0` / `EnableDialogueTickMute=0` / `EnableSignMute=1` / `ScannerMode=0`) — matching a Nexus report where sign mute never worked while scanner mute still did.
- Same change keeps `F7` usable when every mute feature starts off: the sign-mute toggle no longer depends on another feature having already pulled the hook in.
- Verified in-game: old public DLL under the sign-only config reported `hooks=0`; the fix reports `Hooked PostEventID` / `hooks=1`, and sign mute works with Dollman + scanner left off.

## v3.3.0 — Player sign audio mute

- Added a mute for player sign (road-sign) jingles/cheers on DS2 v1.10. All sign audio flows through `AK::SoundEngine::PostEvent(ID)`; the mute combines a curated sign-marker event list (~20 ids, each verified sign-exclusive via time-aligned F8 capture windows) with a session-scoped object classifier: any gameObject that ever emits a marker event has all of its subsequent posts blocked (`mode=sign-marker` / `mode=sign-object`). This covers the cheer "chorus" (one trigger fans out 15+ distinct per-type events across neighboring signs), which no per-event blocklist could enumerate.
- New ini key `EnableSignMute` (default **0/off** — experimental, not yet extensively tested) and a runtime hotkey `F7` that toggles the sign mute for A/B listening; classification keeps learning while toggled off.
- Deliberately NOT markers: `4154749010` (posts on a session-global object — marking it would mute the shared bus) and a WwiseSimpleSoundInstance-vtable structural classifier (tried and reverted: Simple vs Graph instances separate one-shot voice-style audio from graph-driven audio, not signs from the world — it muted Sam/NPC dialogue and leaked signs).
- Confirmed-collateral event ids (world ambience emitters) are documented in `src/core_main.c` and must not be re-added to the marker list.
- Fixed the PostEventID hook (which carries both scanner mute and sign mute) never being installed when `EnableDialogueTickMute=1` (the default), because that init path returned early. This had silently disabled `ScannerMode` under the default voice mode since v3.0; both scanner and sign muting now work in dialogue-tick mode.

## v3.2.1 — DS2 v1.10 runtime vtable hotfix

- Corrected the v1.10 runtime `LocalizedTextResource` vtable to `0x3455CC0`. The initial v3.2 build used a related constructor vtable at `0x3455C60`, which made `speaker_ok=0` even when the payload clearly contained `speaker_tag=0x12B72` / `偶人`.

## v3.2 — DS2 v1.10 compatibility refresh

- Verified the default dialogue-tick + ShowSubtitle hook set on DS2 v1.10: `sub_140387A80`, `sub_140781320`, and `sub_140781420` are unchanged from v1.9.
- Refreshed the `LocalizedTextResource` vtable for v1.10 so subtitle speaker/tag decoding remains valid.
- Re-resolved the legacy/fallback `SoundInstanceSubmit` hook for v1.10 at `sub_1426C1FD0` (`RVA 0x26C1FD0`) so `EnableDialogueTickMute=0` still uses the correct StartTalk sound-instance bridge.

## v3.1 — DS2 v1.9 RVA port

- Ported the v3.0 dialogue-tick + ShowSubtitle hook set to DS2 v1.9: `sub_140387A80`, `sub_140781320`, and `sub_140781420`.
- Updated the gameplay subtitle caller pair to the v1.9 StartTalk sender return (`caller_rva=0x38602B`) and refreshed the `LocalizedTextResource` vtable (`0x3455C70`).
- Re-resolved the legacy/fallback `SoundInstanceSubmit` hook for v1.9 at `sub_1426C1B40` (`RVA 0x26C1B40`) so `EnableDialogueTickMute=0` can keep the v3.0 StartTalk sound-instance bridge available.

## v3.0 — v1.8 dialogue-tick flags gate

- Ported the StartTalk `starttalk_flags` gate onto the per-frame dialogue-tick mute hook: the unified single-point mode (`EnableDialogueTickMute`) now mutes only gameplay Dollman chatter (`starttalk_flags == 0`) and leaves private-room / story dialogue (`starttalk_flags != 0`) audible. Verified in-game on DS2 v1.8 (rest-room `flags=0x63` stays audible, gameplay `flags=0` muted).
- Made the unified dialogue-tick mode the default (`EnableDialogueTickMute=1`).
- Replaced the per-frame `[tick-diag]` diagnostic line with a deduped, one-line-per-event log so `DollmanMute.log` no longer bloats.

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

- This release supports DS2 game version `v1.10`.
- If you notice missed lines, false positives, or any other bug that affects gameplay, please let me know. Including `DollmanMute.log` from the game root is strongly recommended.
- Future game updates may require another RVA refresh.
