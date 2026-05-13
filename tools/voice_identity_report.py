from __future__ import annotations

import argparse
import re
from collections import Counter, defaultdict
from pathlib import Path


DEFAULT_LOG = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log"
)

MARKERS = {
    "schedule": "[dollman-voice-schedule]",
    "resource": "[dollman-voice-resource]",
    "sentence_group": "[dollman-voice-sentence-group]",
    "sentence_index": "[dollman-voice-sentence-index]",
    "closure": "[dollman-voice-closure]",
    "helper": "[voice-helper]",
    "queue_identity": "[voice-queue-identity]",
    "queue_owner": "[voice-queue-owner]",
    "catalog_entry": "[voice-catalog-entry]",
}

KV_RE = re.compile(r"(?P<key>[A-Za-z0-9_+]+)=(?P<value>\"[^\"]*\"|\[[^\]]*\]|[^\s]+)")
FALLBACK_IDS = {
    34000819: "helper-default-output",
    398195119: "helper-default-nonoutput",
}


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Summarize Dollman voice identity probe logs."
    )
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG)
    ap.add_argument("--tail", type=int, default=0, help="only scan the last N lines")
    ap.add_argument("--top", type=int, default=12, help="number of grouped rows to print")
    return ap.parse_args()


def parse_int(value: str) -> int | None:
    value = value.strip().strip('"')
    if not value:
        return None
    try:
        if value.lower().startswith("0x"):
            return int(value, 16)
        return int(value, 10)
    except ValueError:
        return None


def parse_kv(raw: str) -> dict[str, str]:
    return {m.group("key"): m.group("value").strip('"') for m in KV_RE.finditer(raw)}


def iter_lines(path: Path, tail: int):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    start = max(0, len(lines) - tail) if tail > 0 else 0
    for idx, raw in enumerate(lines[start:], start=start + 1):
        yield idx, raw


def classify_id(value: int | None) -> str:
    if value is None:
        return "missing"
    if value == 0:
        return "zero"
    return FALLBACK_IDS.get(value, "payload/controller")


def first_value(row: dict, *keys: str) -> str:
    for key in keys:
        value = row.get(key)
        if value not in (None, ""):
            return value
    return "missing"


def add_record(records: dict[str, list[dict]], kind: str, line_no: int, raw: str) -> None:
    kv = parse_kv(raw)
    kv["_line"] = str(line_no)
    records[kind].append(kv)


def main() -> int:
    args = parse_args()
    if not args.log.exists():
        raise SystemExit(f"log not found: {args.log}")

    records: dict[str, list[dict]] = {key: [] for key in MARKERS}
    for line_no, raw in iter_lines(args.log, args.tail):
        for kind, marker in MARKERS.items():
            if marker in raw:
                add_record(records, kind, line_no, raw)
                break

    print(f"log: {args.log}")
    if args.tail:
        print(f"scope: last {args.tail} lines")
    print()

    for kind in MARKERS:
        print(f"{kind}: {len(records[kind])}")
    print()

    schedules = records["schedule"]
    resources = records["resource"]
    groups = records["sentence_group"]
    indexes = records["sentence_index"]
    helpers = records["helper"]
    queues = records["queue_identity"]
    catalog_entries = records["catalog_entry"]

    source_counter = Counter(first_value(r, "source", "voice", "controller") for r in schedules)
    group_counter = Counter(r.get("sg", "missing") for r in groups)
    helper_source_counter = Counter(first_value(r, "source", "controller") for r in helpers)

    if source_counter:
        print("top schedule sources:")
        for value, count in source_counter.most_common(args.top):
            print(f"  {value}: {count}")
        print()

    if group_counter:
        print("top sentence groups:")
        for value, count in group_counter.most_common(args.top):
            print(f"  {value}: {count}")
        print()

    if helper_source_counter:
        print("top helper sources:")
        for value, count in helper_source_counter.most_common(args.top):
            print(f"  {value}: {count}")
        print()

    if groups:
        print("latest sentence-group rows:")
        for row in groups[-args.top :]:
            print(
                "  line={line} seen={seen} sg={sg} type={typ} sg28={sg28} "
                "items={items} count={count} first={first} s0_text={text} s0_sound={sound}".format(
                    line=row.get("_line", "?"),
                    seen=row.get("seen", "?"),
                    sg=row.get("sg", "?"),
                    typ=row.get("sg_type", "?"),
                    sg28=row.get("sg28", row.get("sg_count", "?")),
                    items=row.get("sg_items", row.get("sg30", "?")),
                    count=row.get("sg_count", "?"),
                    first=row.get("first", "?"),
                    text=row.get("s0_text", "?"),
                    sound=row.get("s0_sound", "?"),
                )
            )
        print()

    if indexes:
        in_bounds = sum(1 for row in indexes if row.get("in_bounds") == "1")
        print(f"sentence index rows: {len(indexes)} (in_bounds={in_bounds})")
        for row in indexes[-args.top :]:
            print(
                "  line={line} seen={seen} controller={controller} "
                "in_bounds={bounds} count={count} sentence={sentence} "
                "text={text} sound={sound} voice={voice} fallback={fallback}".format(
                    line=row.get("_line", "?"),
                    seen=row.get("seen", "?"),
                    controller=row.get("controller", "?"),
                    bounds=row.get("in_bounds", "?"),
                    count=row.get("sg_count", "?"),
                    sentence=row.get("sentence", "?"),
                    text=row.get("text", "?"),
                    sound=row.get("sound", "?"),
                    voice=row.get("voice", "?"),
                    fallback=row.get("voice_fallback", "?"),
                )
            )
        print()

    if queues:
        by_id: dict[int | None, list[dict]] = defaultdict(list)
        for row in queues:
            by_id[parse_int(row.get("id", ""))].append(row)

        print("queue identity ids:")
        for request_id, rows in sorted(
            by_id.items(), key=lambda item: (-len(item[1]), -1 if item[0] is None else item[0])
        )[: args.top]:
            label = classify_id(request_id)
            sample = rows[-1]
            id_text = "missing" if request_id is None else f"0x{request_id:x}"
            print(
                "  {id_text}: count={count} class={label} "
                "index={index} catalog_index={catalog_index} catalog_entry={catalog_entry} "
                "ref={ref} source={source} line={line}".format(
                    id_text=id_text,
                    count=len(rows),
                    label=label,
                    index=sample.get("index", "?"),
                    catalog_index=sample.get("catalog_index", "?"),
                    catalog_entry=sample.get("catalog_entry", "?"),
                    ref=sample.get("ref", "?"),
                    source=sample.get("source", "?"),
                    line=sample.get("_line", "?"),
                )
            )
        print()

    if catalog_entries:
        print("latest catalog entries:")
        for row in catalog_entries[-args.top :]:
            print(
                "  line={line} blocked={blocked} entry={entry} type={typ} "
                "q08={q08} q10={q10} q18={q18} q20={q20} q28={q28} q30={q30} q38={q38}".format(
                    line=row.get("_line", "?"),
                    blocked=row.get("blocked", "?"),
                    entry=row.get("entry", "?"),
                    typ=row.get("type", "?"),
                    q08=row.get("q08", "?"),
                    q10=row.get("q10", "?"),
                    q18=row.get("q18", "?"),
                    q20=row.get("q20", "?"),
                    q28=row.get("q28", "?"),
                    q30=row.get("q30", "?"),
                    q38=row.get("q38", "?"),
                )
            )
        print()

        nonfallback = [
            row
            for row in queues
            if classify_id(parse_int(row.get("id", ""))) == "payload/controller"
        ]
        print(f"queue identity nonfallback rows: {len(nonfallback)}")
        if nonfallback:
            for row in nonfallback[-args.top :]:
                print(
                    "  line={line} blocked={blocked} id={id} index={index} "
                    "catalog_index={catalog_index} catalog_entry={catalog_entry} "
                    "source={source} ref={ref}".format(
                        line=row.get("_line", "?"),
                        blocked=row.get("blocked", "?"),
                        id=row.get("id", "?"),
                        index=row.get("index", "?"),
                        catalog_index=row.get("catalog_index", "?"),
                        catalog_entry=row.get("catalog_entry", "?"),
                        source=row.get("source", "?"),
                        ref=row.get("ref", "?"),
                    )
                )
        print()

    print("audit:")
    if not schedules:
        print("  missing [dollman-voice-schedule]; random Dollman path not observed in this log scope")
    if not groups:
        print("  missing [dollman-voice-sentence-group]; source+0x88 sentence group not observed")
    if groups and not indexes:
        print("  missing [dollman-voice-sentence-index]; controller/index relationship not observed")
    if not queues:
        print("  missing [voice-queue-identity]; EnableVoiceQueueIdentityProbe was likely off or random was not triggered")
    if queues and not any(
        classify_id(parse_int(row.get("id", ""))) == "payload/controller" for row in queues
    ):
        print("  queue ids are zero/fallback only; not enough evidence for per-line identity")
    if queues and not any(parse_int(row.get("catalog_index", "")) not in (None, -1) for row in queues):
        print("  queue catalog lookup did not resolve; request id is not enough yet")
    if schedules and groups and queues:
        print("  has schedule + sentence group + queue identity samples; inspect id stability against repeated randoms")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
