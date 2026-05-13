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
    "sentence_scan": "[dollman-voice-sentence-scan]",
    "closure": "[dollman-voice-closure]",
    "helper": "[voice-helper]",
    "helper_probe": "[voice-helper-probe]",
    "helper_state": "[voice-helper-state]",
    "queue_identity": "[voice-queue-identity]",
    "queue_owner": "[voice-queue-owner]",
    "catalog_entry": "[voice-catalog-entry]",
}

KV_RE = re.compile(r"(?P<key>[A-Za-z0-9_+]+)=(?P<value>\"[^\"]*\"|\[[^\]]*\]|[^\s]+)")
FALLBACK_IDS = {
    34000819: "helper-default-output",
    398195119: "helper-default-nonoutput",
}
EXPECTED_DOLLMAN_HELPER_CALLER_RVA = 0x00C7443D


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


def same_int(left: str | None, right: str | None) -> bool:
    a = parse_int(left or "")
    b = parse_int(right or "")
    return a is not None and b is not None and a == b


def pointer_matches(value: str | None, row: dict, keys: tuple[str, ...]) -> list[str]:
    matches: list[str] = []
    for key in keys:
        if same_int(value, row.get(key)):
            matches.append(key)
    return matches


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
    scans = records["sentence_scan"]
    helpers = records["helper"]
    helper_probes = records["helper_probe"]
    helper_states = records["helper_state"]
    queues = records["queue_identity"]
    catalog_entries = records["catalog_entry"]

    source_counter = Counter(first_value(r, "source", "voice", "controller") for r in schedules)
    group_counter = Counter(r.get("sg", "missing") for r in groups)
    helper_source_counter = Counter(first_value(r, "source", "controller") for r in helpers)
    helper_caller_counter = Counter(r.get("caller_rva", "missing") for r in helpers)

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

    if helper_caller_counter:
        print("top helper callers:")
        for value, count in helper_caller_counter.most_common(args.top):
            caller = parse_int(value)
            label = (
                "expected-dollman-closure"
                if caller == EXPECTED_DOLLMAN_HELPER_CALLER_RVA
                else "other"
            )
            print(f"  {value}: {count} ({label})")
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

    if scans:
        print("latest sentence scans:")
        for row in scans[-args.top :]:
            print(
                "  line={line} seen={seen} idx={idx} sentence={sentence} "
                "text={text} sound={sound} voice={voice} fallback={fallback}".format(
                    line=row.get("_line", "?"),
                    seen=row.get("seen", "?"),
                    idx=row.get("idx", "?"),
                    sentence=row.get("sentence", "?"),
                    text=row.get("text", "?"),
                    sound=row.get("sound", "?"),
                    voice=row.get("voice", "?"),
                    fallback=row.get("voice_fallback", "?"),
                )
            )
        print()

    if helper_probes:
        print("latest helper probes:")
        for row in helper_probes[-args.top :]:
            print(
                "  line={line} consumed={consumed} src10={src10} src20={src20} "
                "src38={src38} src48={src48} src50={src50} src58={src58}".format(
                    line=row.get("_line", "?"),
                    consumed=row.get("consumed", "?"),
                    src10=row.get("src10", "?"),
                    src20=row.get("src20", "?"),
                    src38=row.get("src38", "?"),
                    src48=row.get("src48", "?"),
                    src50=row.get("src50", "?"),
                    src58=row.get("src58", "?"),
                )
            )
        print()

    if helper_states:
        print("latest helper states:")
        for row in helper_states[-args.top :]:
            print(
                "  line={line} consumed={consumed} src80={src80} src88={src88} "
                "src90={src90} src94={src94} src98={src98} helper40={helper40} "
                "helper60={helper60} helper64={helper64}".format(
                    line=row.get("_line", "?"),
                    consumed=row.get("consumed", "?"),
                    src80=row.get("src80", "?"),
                    src88=row.get("src88", "?"),
                    src90=row.get("src90", "?"),
                    src94=row.get("src94", "?"),
                    src98=row.get("src98", "?"),
                    helper40=row.get("helper40", "?"),
                    helper60=row.get("helper60", "?"),
                    helper64=row.get("helper64", "?"),
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
                "flags={flag20}/{flag21}/{flag22} ref={ref} source={source} line={line}".format(
                    id_text=id_text,
                    count=len(rows),
                    label=label,
                    index=sample.get("index", "?"),
                    catalog_index=sample.get("catalog_index", "?"),
                    catalog_entry=sample.get("catalog_entry", "?"),
                    flag20=sample.get("flag20", "?"),
                    flag21=sample.get("flag21", "?"),
                    flag22=sample.get("flag22", "?"),
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
                "u20={u20} u24_sort={u24} u28={u28} u2c={u2c} u30={u30} "
                "b30={b30} b32={b32} b33={b33} b34={b34} "
                "q08={q08} q10={q10} q18={q18} q20={q20} q28={q28} q30={q30} q38={q38} "
                "q40={q40} q48={q48}".format(
                    line=row.get("_line", "?"),
                    blocked=row.get("blocked", "?"),
                    entry=row.get("entry", "?"),
                    typ=row.get("type", "?"),
                    u20=row.get("u20", "?"),
                    u24=row.get("u24_sort", "?"),
                    u28=row.get("u28", "?"),
                    u2c=row.get("u2c", "?"),
                    u30=row.get("u30", "?"),
                    b30=row.get("b30", "?"),
                    b32=row.get("b32", "?"),
                    b33=row.get("b33", "?"),
                    b34=row.get("b34", "?"),
                    q08=row.get("q08", "?"),
                    q10=row.get("q10", "?"),
                    q18=row.get("q18", "?"),
                    q20=row.get("q20", "?"),
                    q28=row.get("q28", "?"),
                    q30=row.get("q30", "?"),
                    q38=row.get("q38", "?"),
                    q40=row.get("q40", "?"),
                    q48=row.get("q48", "?"),
                )
            )
        print()

        catalog_by_blocked = {
            row.get("blocked"): row for row in catalog_entries if row.get("blocked")
        }
        index_rows_by_line = sorted(
            indexes, key=lambda row: parse_int(row.get("_line", "")) or -1
        )
        print("queue/catalog joins:")
        for queue in queues[-args.top :]:
            blocked = queue.get("blocked")
            catalog = catalog_by_blocked.get(blocked)
            if not catalog:
                continue
            queue_line = parse_int(queue.get("_line", "")) or -1
            prior_indexes = [
                row
                for row in index_rows_by_line
                if (parse_int(row.get("_line", "")) or -1) < queue_line
            ]
            nearest_index = prior_indexes[-1] if prior_indexes else {}
            entry_keys = ("q08", "q10", "q18", "q20", "q28", "q30", "q38", "q40", "q48")
            ref_hits = pointer_matches(queue.get("ref"), catalog, entry_keys)
            sentence_hits = []
            for index_key in ("sentence", "sound", "text", "voice_fallback", "voice"):
                for entry_key in entry_keys:
                    if same_int(nearest_index.get(index_key), catalog.get(entry_key)):
                        sentence_hits.append(f"{index_key}=={entry_key}")
            print(
                "  blocked={blocked} line={line} id_key={id_key} entry={entry} "
                "ref_hits={ref_hits} nearest_sentence_line={sentence_line} "
                "sentence_hits={sentence_hits}".format(
                    blocked=blocked,
                    line=queue.get("_line", "?"),
                    id_key="yes" if same_int(queue.get("id"), queue.get("catalog_key")) else "no",
                    entry=catalog.get("entry", "?"),
                    ref_hits=",".join(ref_hits) if ref_hits else "-",
                    sentence_line=nearest_index.get("_line", "-"),
                    sentence_hits=",".join(sentence_hits) if sentence_hits else "-",
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
                    "flags={flag20}/{flag21}/{flag22} source={source} ref={ref}".format(
                        line=row.get("_line", "?"),
                        blocked=row.get("blocked", "?"),
                        id=row.get("id", "?"),
                        index=row.get("index", "?"),
                        catalog_index=row.get("catalog_index", "?"),
                        catalog_entry=row.get("catalog_entry", "?"),
                        flag20=row.get("flag20", "?"),
                        flag21=row.get("flag21", "?"),
                        flag22=row.get("flag22", "?"),
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
    if helpers and not helper_states:
        print("  missing [voice-helper-state]; log may predate helper state probe or helper scope is incomplete")
    if queues and not any(
        classify_id(parse_int(row.get("id", ""))) == "payload/controller" for row in queues
    ):
        print("  queue ids are zero/fallback only; not enough evidence for per-line identity")
    if queues and not any(parse_int(row.get("catalog_index", "")) not in (None, -1) for row in queues):
        print("  queue catalog lookup did not resolve; request id is not enough yet")
    helper_callers = [parse_int(row.get("caller_rva", "")) for row in helpers]
    known_helper_callers = [caller for caller in helper_callers if caller is not None]
    if helpers and not known_helper_callers:
        print("  helper caller_rva missing; log was likely produced before caller tracking was added")
    if known_helper_callers and any(
        caller != EXPECTED_DOLLMAN_HELPER_CALLER_RVA for caller in known_helper_callers
    ):
        print("  helper caller_rva includes non-Dollman-closure callsites; inspect scope before trusting samples")
    if schedules and groups and queues:
        print("  has schedule + sentence group + queue identity samples; inspect id stability against repeated randoms")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
