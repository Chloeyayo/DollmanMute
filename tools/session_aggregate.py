"""Aggregate DollmanMute.log by F8 sessions.

Produces one row per session with:
  - session_id (F8 count)
  - line range
  - user label (if any preceding/following text line)
  - builder count by letter
  - unique msg_vtbl_rva values seen
  - distinct (msg[1], msg[2], msg[3], msg[6]) fingerprints + counts
"""
from __future__ import annotations
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

LOG_PATH = Path(r"C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log")

SESSION_RE = re.compile(r"=== session boundary F8 count=(\d+) ===")
BUILDER_RE = re.compile(
    r"\[builder=(?P<id>[A-Z0-9]+)\].+?"
    r"this=0x(?P<this>[0-9a-f]+).+?"
    r"arg2=0x(?P<arg2>[0-9a-f]+).+?"
    r"caller_rva=0x(?P<caller_rva>[0-9a-f]+).+?"
    r"msg_vtbl_rva=0x(?P<vtbl>[0-9a-f]+).+?"
    r"msg_readable=(?P<readable>[01]).+?"
    r"msg=\[(?P<words>[^\]]+)\]"
)


def parse_log(path: Path):
    """Yield (line_no, kind, payload_dict) tuples.

    kind ∈ {"boundary", "builder", "label", "other"}
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    for idx, raw in enumerate(text.splitlines(), start=1):
        m_bound = SESSION_RE.search(raw)
        if m_bound:
            yield idx, "boundary", {"count": int(m_bound.group(1)), "raw": raw}
        m_bld = BUILDER_RE.search(raw)
        if m_bld:
            words = [int(w.strip(), 16) for w in m_bld.group("words").split(",")]
            yield idx, "builder", {
                "id": m_bld.group("id"),
                "this": int(m_bld.group("this"), 16),
                "vtbl": int(m_bld.group("vtbl"), 16),
                "words": words,
                "raw": raw,
            }
            continue
        # Label = free-form text without matching known prefix
        if raw and not raw.startswith("[") and "===" not in raw:
            yield idx, "label", {"text": raw.strip(), "raw": raw}


def main():
    sessions: list[dict] = []
    current = {
        "count": 0,
        "start_line": 1,
        "end_line": 1,
        "labels": [],
        "builders": [],
    }
    for line_no, kind, payload in parse_log(LOG_PATH):
        if kind == "boundary":
            current["end_line"] = line_no - 1
            sessions.append(current)
            current = {
                "count": payload["count"],
                "start_line": line_no,
                "end_line": line_no,
                "labels": [],
                "builders": [],
            }
        elif kind == "builder":
            current["builders"].append(payload)
            current["end_line"] = line_no
        elif kind == "label":
            current["labels"].append((line_no, payload["text"]))
            current["end_line"] = line_no
    sessions.append(current)

    print(f"Parsed {len(sessions)} sessions from {LOG_PATH.name}\n")

    for s in sessions:
        builders = s["builders"]
        if not builders and not s["labels"]:
            continue

        by_letter = Counter(b["id"] for b in builders)
        vtbls = Counter(b["vtbl"] for b in builders)
        # Fingerprint: (msg[1], msg[2], msg[3])
        fingerprints = Counter(
            (b["words"][1], b["words"][2], b["words"][3]) for b in builders
        )
        this_set = Counter(b["this"] for b in builders)

        print("-" * 90)
        print(f"session {s['count']:>3} | lines {s['start_line']}-{s['end_line']} | "
              f"builders={sum(by_letter.values())} {dict(by_letter)}")
        if s["labels"]:
            for ln, txt in s["labels"]:
                print(f"  label@L{ln}: {txt}")
        if vtbls:
            vtbl_str = ", ".join(f"0x{v:x}×{c}" for v, c in vtbls.most_common())
            print(f"  msg_vtbl_rva: {vtbl_str}")
        if this_set:
            this_str = ", ".join(f"0x{t:x}×{c}" for t, c in this_set.most_common())
            print(f"  this: {this_str}")
        if fingerprints:
            print(f"  fingerprints (msg[1], msg[2], msg[3]) top 6:")
            for (m1, m2, m3), c in fingerprints.most_common(6):
                print(f"    0x{m1:016x} 0x{m2:08x} 0x{m3:016x}  ×{c}")


if __name__ == "__main__":
    main()
