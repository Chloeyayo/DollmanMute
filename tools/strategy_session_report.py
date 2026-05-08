from __future__ import annotations

import argparse
import re
from collections import Counter, defaultdict
from pathlib import Path


DEFAULT_LOG = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log"
)

SESSION_RE = re.compile(r"=== session boundary F8 count=(\d+) ===")
STRATEGY_SESSION_RE = re.compile(
    r"StrategySession active=(?P<active>\w+)(?:\s+desc='[^']*')?\s+throwRecall=(?P<tr>\d)\s+dialogue=(?P<dlg>\d)"
)
HOTKEY_STRATEGY_RE = re.compile(
    r"HotkeyStrategy active=(?P<active>\w+) key='(?P<key>[^']+)'"
)
HOTKEY_MUTE_RE = re.compile(
    r"HotkeyMute throwRecall=(?P<tr>\d) dialogue=(?P<dlg>\d) key='(?P<key>[^']+)'"
)
STRATEGY_RE = re.compile(
    r"\[strategy\]\s+(?:surface=(?P<surface>\w+)\s+)?tid=(?P<tid>\d+)\s+active=(?P<active>\w+)\s+builder=(?P<builder>\w+)\s+"
    r"family=(?P<family>\w+)\s+speaker_ok=(?P<speaker_ok>\d)\s+speaker_tag=0x(?P<speaker_tag>[0-9a-fA-F]+)\s+"
    r"caller_rva=0x(?P<caller_rva>[0-9a-fA-F]+)\s+obs=(?P<obs>\d)\s+pair=(?P<pair>\d)\s+"
    r"caller=(?P<caller>\d)\s+speaker=(?P<speaker>\d)\s+selected=(?P<selected>\d)\s+"
    r"hybrid=(?P<hybrid>\d)\s+actual=(?P<actual>\d)"
)
MUTE_RE = re.compile(
    r"Muted subtitle surface=(?P<surface>\w+)\s+strategy=(?P<strategy>\w+)\s+caller_rva=0x(?P<caller>[0-9a-fA-F]+)\s+"
    r"speaker_ok=(?P<speaker_ok>\d)\s+speaker_tag=0x(?P<speaker>[0-9a-fA-F]+)\s+line_ok=(?P<line_ok>\d)\s+"
    r"line_tag=0x(?P<line_tag>[0-9a-fA-F]+)\s+family=(?P<family>\w+)\s+builder=(?P<builder>\w+)"
)

STRATEGY_FIELDS = ["obs", "pair", "caller", "speaker", "selected", "hybrid"]


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Aggregate DollmanMute multi-strategy sessions.")
    ap.add_argument("--log", type=Path, default=DEFAULT_LOG)
    ap.add_argument("--session", default="all", help="'all', 'last', actual F8 id like '12'/'F8-12'")
    ap.add_argument("--top", type=int, default=8, help="top cluster count to print")
    return ap.parse_args()


def new_session(session_id: int, start_line: int) -> dict:
    return {
        "id": session_id,
        "start_line": start_line,
        "end_line": start_line,
        "active_strategy": None,
        "throw_recall": None,
        "dialogue": None,
        "strategy_events": [],
        "mute_events": [],
        "hotkeys": [],
    }


def iter_lines(path: Path):
    text = path.read_text(encoding="utf-8", errors="replace")
    for idx, raw in enumerate(text.splitlines(), start=1):
        yield idx, raw


def parse_sessions(path: Path) -> list[dict]:
    sessions: list[dict] = []
    current = new_session(0, 1)

    for line_no, raw in iter_lines(path):
        m = SESSION_RE.search(raw)
        if m:
            current["end_line"] = line_no - 1
            sessions.append(current)
            current = new_session(int(m.group(1)), line_no)
            continue

        current["end_line"] = line_no

        m = STRATEGY_SESSION_RE.search(raw)
        if m:
            current["active_strategy"] = m.group("active")
            current["throw_recall"] = int(m.group("tr"))
            current["dialogue"] = int(m.group("dlg"))
            continue

        m = HOTKEY_STRATEGY_RE.search(raw)
        if m:
            current["hotkeys"].append(("strategy", m.group("active"), m.group("key"), line_no))
            current["active_strategy"] = m.group("active")
            continue

        m = HOTKEY_MUTE_RE.search(raw)
        if m:
            current["hotkeys"].append(("mute", m.group("key"), f"tr={m.group('tr')} dlg={m.group('dlg')}", line_no))
            current["throw_recall"] = int(m.group("tr"))
            current["dialogue"] = int(m.group("dlg"))
            continue

        m = STRATEGY_RE.search(raw)
        if m:
            evt = {
                "tid": int(m.group("tid")),
                "active": m.group("active"),
                "surface": m.group("surface") or "unknown",
                "builder": m.group("builder"),
                "family": m.group("family"),
                "speaker_ok": int(m.group("speaker_ok")),
                "speaker_tag": int(m.group("speaker_tag"), 16),
                "caller_rva": int(m.group("caller_rva"), 16),
                "actual": int(m.group("actual")),
            }
            for field in STRATEGY_FIELDS:
                evt[field] = int(m.group(field))
            current["strategy_events"].append(evt)
            continue

        m = MUTE_RE.search(raw)
        if m:
            current["mute_events"].append(
                {
                    "surface": m.group("surface"),
                    "strategy": m.group("strategy"),
                    "family": m.group("family"),
                    "speaker": int(m.group("speaker"), 16),
                    "caller": int(m.group("caller"), 16),
                    "line_tag": int(m.group("line_tag"), 16),
                    "builder": m.group("builder"),
                }
            )

    sessions.append(current)
    return sessions


def select_sessions(sessions: list[dict], choice: str) -> list[dict]:
    meaningful = [s for s in sessions if s["id"] != 0 or s["strategy_events"] or s["mute_events"] or s["hotkeys"]]
    if choice == "all":
        return meaningful
    if choice == "last":
        return meaningful[-1:] if meaningful else []
    if choice.lower().startswith("f8-"):
        choice = choice[3:]
    wanted = int(choice)
    return [s for s in meaningful if s["id"] == wanted]


def print_session(session: dict, top_n: int) -> None:
    events = session["strategy_events"]
    mute_events = session["mute_events"]
    if not events and not mute_events and not session["hotkeys"]:
        return

    family_counts = Counter(evt["family"] for evt in events)
    builder_counts = Counter(evt["builder"] for evt in events)
    active_counts = Counter(evt["active"] for evt in events)
    surface_counts = Counter(evt["surface"] for evt in events)
    disagree = 0
    strategy_would = Counter()
    clusters = Counter()
    mute_clusters = Counter()

    for evt in events:
        votes = tuple(evt[field] for field in STRATEGY_FIELDS)
        if len(set(votes)) > 1:
            disagree += 1
        for field in STRATEGY_FIELDS:
            if evt[field]:
                strategy_would[field] += 1
        clusters[(evt["speaker_tag"], evt["caller_rva"], evt["family"], evt["builder"])] += 1
    for evt in mute_events:
        mute_clusters[(evt["speaker"], evt["caller"], evt["line_tag"], evt["family"], evt["builder"])] += 1

    print("-" * 100)
    print(
        f"session {session['id']:>3} | lines {session['start_line']}-{session['end_line']} | "
        f"events={len(events)} | actual_mutes={sum(evt['actual'] for evt in events)} | mute_lines={len(mute_events)}"
    )
    print(
        f"  state active={session['active_strategy'] or 'unknown'} "
        f"throwRecall={session['throw_recall']} dialogue={session['dialogue']}"
    )
    if active_counts:
        print("  active strategies seen:", dict(active_counts))
    if surface_counts:
        print("  surfaces:", dict(surface_counts))
    if family_counts:
        print("  families:", dict(family_counts))
    if builder_counts:
        print("  builders:", dict(builder_counts))
    print("  strategy would-mute counts:", dict(strategy_would))
    print(f"  disagreement events: {disagree}")
    if session["hotkeys"]:
        print("  hotkeys:")
        for kind, a, b, line_no in session["hotkeys"]:
            print(f"    L{line_no}: {kind} {a} {b}")
    if clusters:
        print(f"  top clusters (speaker_tag, caller_rva, family, builder) top {top_n}:")
        for (speaker_tag, caller_rva, family, builder), count in clusters.most_common(top_n):
            print(
                f"    0x{speaker_tag:x} / 0x{caller_rva:x} / {family} / {builder}  x{count}"
            )
    if mute_clusters:
        print(f"  top actual mute clusters (speaker_tag, caller_rva, line_tag, family, builder) top {top_n}:")
        for (speaker_tag, caller_rva, line_tag, family, builder), count in mute_clusters.most_common(top_n):
            print(
                f"    0x{speaker_tag:x} / 0x{caller_rva:x} / 0x{line_tag:x} / {family} / {builder}  x{count}"
            )


def main() -> None:
    args = parse_args()
    sessions = parse_sessions(args.log)
    picked = select_sessions(sessions, args.session)
    print(f"Parsed {len(sessions)} raw sessions from {args.log}")
    for session in picked:
        print_session(session, args.top)


if __name__ == "__main__":
    main()
