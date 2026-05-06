import argparse
import datetime as dt
import re
from collections import Counter
from pathlib import Path


DEFAULT_LOG = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log"
)

TS_RE = re.compile(r"^\[(?P<ts>[^\]]+)\]")
SUB_HIT_RE = re.compile(
    r"SubtitleHit surface=sender caller_rva=0x(?P<caller>[0-9a-f]+) "
    r"speaker_ok=1 speaker_tag=0x(?P<speaker>[0-9a-f]+) "
    r"line_ok=1 line_tag=0x(?P<line>[0-9a-f]+) family=(?P<family>\w+)",
    re.I,
)
MUTE_RE = re.compile(
    r"Muted subtitle surface=sender .*caller_rva=0x(?P<caller>[0-9a-f]+) "
    r"speaker_ok=1 speaker_tag=0x(?P<speaker>[0-9a-f]+) "
    r"line_ok=1 line_tag=0x(?P<line>[0-9a-f]+) family=(?P<family>\w+)",
    re.I,
)
REFPACK_RE = re.compile(
    r"\[stf-refpack-sti-post\] .*this=0x(?P<this>[0-9a-f]+) refpackE8=\[(?P<pack>[^\]]*)\] "
    r"pack2_.*?pack2_tag=0x(?P<line>[0-9a-f]+) pack2_text=\"(?P<text>[^\"]*)\"",
    re.I,
)
STF_POST_RE = re.compile(
    r"\[stf-sti-post\] .*this=0x(?P<this>[0-9a-f]+).*speaker=0x[0-9a-f]+ ok=1 tag=0x(?P<speaker>[0-9a-f]+)",
    re.I,
)
POSTEVENT_RE = re.compile(
    r"\[postevent\] .*eventId=(?P<event>\d+) gameObject=0x(?P<gameobject>[0-9a-f]+) "
    r"externalSources=(?P<external>\d+) .*ext0=0x(?P<ext0>[0-9a-f]+) .*blocked=(?P<blocked>\d+)",
    re.I,
)


def parse_ts(line: str):
    m = TS_RE.match(line)
    if not m:
        return None
    raw = m.group("ts")
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
        try:
            return dt.datetime.strptime(raw, fmt)
        except ValueError:
            pass
    return None


def parse_qwords(raw: str):
    return [int(x.strip(), 16) for x in raw.split(",") if x.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG)
    args = ap.parse_args()

    sam_hits = 0
    sam_mutes = 0
    dollman_hits = 0
    dollman_mutes = 0
    dollman_story_like_hits = 0
    dollman_story_like_mutes = 0
    dollman_hit_clusters = Counter()
    dollman_mute_clusters = Counter()
    non_dollman_sender_hits = 0
    non_dollman_sender_mutes = 0
    refpacks = []
    sam_starttalk_this = set()
    sam_zero_refpacks = 0
    sam_external_postevents_unblocked = 0
    postevents = []

    with open(args.log, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            ts = parse_ts(line)
            m = SUB_HIT_RE.search(line)
            if m and m.group("caller").lower() == "202fa6f" and int(m.group("speaker"), 16) == 0x12B6F:
                dollman_story_like_hits += 1
            if m and m.group("caller").lower() == "385c5b":
                speaker = int(m.group("speaker"), 16)
                line_tag = int(m.group("line"), 16)
                if speaker == 0x122A8:
                    sam_hits += 1
                if speaker == 0x12B6F:
                    dollman_hits += 1
                else:
                    non_dollman_sender_hits += 1
                if speaker == 0x12B6F and line_tag == 0x1F4:
                    pass
            if m and int(m.group("speaker"), 16) == 0x12B6F:
                dollman_hit_clusters[(m.group("caller").lower(), m.group("line").lower(), m.group("family"))] += 1

            m = MUTE_RE.search(line)
            if m and m.group("caller").lower() == "202fa6f" and int(m.group("speaker"), 16) == 0x12B6F:
                dollman_story_like_mutes += 1
            if m and m.group("caller").lower() == "385c5b":
                speaker = int(m.group("speaker"), 16)
                if speaker == 0x122A8:
                    sam_mutes += 1
                if speaker == 0x12B6F:
                    dollman_mutes += 1
                else:
                    non_dollman_sender_mutes += 1
            if m and int(m.group("speaker"), 16) == 0x12B6F:
                dollman_mute_clusters[(m.group("caller").lower(), m.group("line").lower(), m.group("family"))] += 1

            m = REFPACK_RE.search(line)
            if m and ts is not None:
                pack = parse_qwords(m.group("pack"))
                this_obj = int(m.group("this"), 16)
                if this_obj in sam_starttalk_this and pack == [0, 0, 0, 0]:
                    sam_zero_refpacks += 1
                if len(pack) >= 4 and pack[3] != 0:
                    refpacks.append({
                        "ts": ts,
                        "this": this_obj,
                        "gameobject": pack[3],
                        "line": int(m.group("line"), 16),
                        "text": m.group("text"),
                    })

            m = STF_POST_RE.search(line)
            if m and int(m.group("speaker"), 16) == 0x122A8:
                sam_starttalk_this.add(int(m.group("this"), 16))

            m = POSTEVENT_RE.search(line)
            if m and ts is not None:
                event = int(m.group("event"))
                ext0 = int(m.group("ext0"), 16)
                external = int(m.group("external"))
                blocked = int(m.group("blocked"))
                if event == 4059710847 and ext0 == 0x4F1FA457F and external == 1 and blocked == 0:
                    sam_external_postevents_unblocked += 1
                postevents.append({
                    "ts": ts,
                    "gameobject": int(m.group("gameobject"), 16),
                    "event": event,
                    "external": external,
                    "ext0": ext0,
                    "blocked": blocked,
                })

    links = []
    for ref in refpacks:
        for evt in postevents:
            if evt["gameobject"] != ref["gameobject"]:
                continue
            delta_ms = int((evt["ts"] - ref["ts"]).total_seconds() * 1000)
            if -250 <= delta_ms <= 1000:
                links.append((delta_ms, ref, evt))

    print("== DollmanMute goal audit ==")
    print(f"sam_sender_hits={sam_hits} sam_sender_mutes={sam_mutes}")
    print(f"non_dollman_sender_hits={non_dollman_sender_hits} non_dollman_sender_mutes={non_dollman_sender_mutes}")
    print(f"dollman_sender_hits={dollman_hits} dollman_sender_mutes={dollman_mutes}")
    print(f"dollman_story_like_hits={dollman_story_like_hits} dollman_story_like_mutes={dollman_story_like_mutes}")
    print(f"sam_starttalk_zero_refpack={sam_zero_refpacks}")
    print(f"sam_external_postevents_unblocked={sam_external_postevents_unblocked}")
    print(f"refpack_postevent_links={len(links)}")
    for delta_ms, ref, evt in links[:8]:
        print(
            f"  dt={delta_ms:+d}ms line=0x{ref['line']:x} text=\"{ref['text']}\" "
            f"gameObject=0x{ref['gameobject']:x} eventId={evt['event']} "
            f"ext0=0x{evt['ext0']:x} blocked={evt['blocked']}"
        )
    print("dollman_subtitle_clusters:")
    for (caller, line_tag, family), count in dollman_hit_clusters.most_common(12):
        muted = dollman_mute_clusters[(caller, line_tag, family)]
        print(f"  caller=0x{caller} line=0x{line_tag} family={family} hits={count} mutes={muted}")

    print()
    print("status:")
    print("  subtitle_sam_false_positive=" + ("clear" if sam_mutes == 0 else "FAILED"))
    print("  subtitle_non_dollman_false_positive=" + ("clear" if non_dollman_sender_mutes == 0 else "FAILED"))
    print("  sam_voice_refpack_negative=" + ("present" if sam_zero_refpacks and sam_external_postevents_unblocked else "missing"))
    print("  dollman_story_like_false_positive=" + ("clear" if dollman_story_like_hits and dollman_story_like_mutes == 0 else "missing"))
    print("  throw_refpack_voice_projection=" + ("present" if links else "missing"))
    print("  final_goal=not_complete")


if __name__ == "__main__":
    main()
