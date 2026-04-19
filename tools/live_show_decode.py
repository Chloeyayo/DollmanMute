import argparse
import ctypes as C
import datetime as dt
import re
import struct
from collections import defaultdict
from ctypes import wintypes as W


PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
VTBL_RVA_LOCALIZED_TEXT = 0x3448E48

KNOWN_SPEAKERS = {
    0x122A8: "Sam",
    0x12B6F: "Dollman",
    0x4045:  "<task-failure>",
}

SHOW_RE = re.compile(
    r"^\[(?P<ts>[^\]]+)\] \[show\] tid=(?P<tid>\d+) "
    r"caller_rva=0x(?P<caller>[0-9a-f]+)[^\[]*?"
    r"p=\[(?P<plist>[^\]]*)\] "
    r"p6v=0x(?P<p6v>[0-9a-f]+) p6=\[(?P<p6list>[^\]]*)\] "
    r"p7v=0x(?P<p7v>[0-9a-f]+) p7=\[(?P<p7list>[^\]]*)\]",
    re.I,
)

DISP_RE = re.compile(
    r"^\[(?P<ts>[^\]]+)\] \[disp\] .*?"
    r"tf_vtbl_rva=0x(?P<tf>[0-9a-f]+) "
    r"a1_1=0x(?P<a1_1>[0-9a-f]+) "
    r"a1_2=0x(?P<a1_2>[0-9a-f]+).*?"
    r"tid=(?P<tid>\d+)",
    re.I,
)

IMAGE_BASE_RE = re.compile(r"DollmanMute image_base=0x(?P<base>[0-9a-f]+)", re.I)
BUILD_RE = re.compile(r"^\[(?P<ts>[^\]]+)\] DollmanMute build:")
F8_RE = re.compile(r"^\[(?P<ts>[^\]]+)\] === session boundary F8 count=(?P<n>\d+) ===")
TS_PREFIX = re.compile(r"^\[(?P<ts>[^\]]+)\]")


class PROCESSENTRY32W(C.Structure):
    _fields_ = [
        ("dwSize", W.DWORD),
        ("cntUsage", W.DWORD),
        ("th32ProcessID", W.DWORD),
        ("th32DefaultHeapID", C.c_size_t),
        ("th32ModuleID", W.DWORD),
        ("cntThreads", W.DWORD),
        ("th32ParentProcessID", W.DWORD),
        ("pcPriClassBase", W.LONG),
        ("dwFlags", W.DWORD),
        ("szExeFile", W.WCHAR * 260),
    ]


kernel32 = C.WinDLL("kernel32", use_last_error=True)
CreateToolhelp32Snapshot = kernel32.CreateToolhelp32Snapshot
Process32FirstW = kernel32.Process32FirstW
Process32NextW = kernel32.Process32NextW
OpenProcess = kernel32.OpenProcess
ReadProcessMemory = kernel32.ReadProcessMemory
CloseHandle = kernel32.CloseHandle


def parse_ts(raw):
    return dt.datetime.strptime(raw.split(".")[0], "%Y-%m-%d %H:%M:%S")


def line_ts(line):
    m = TS_PREFIX.match(line)
    if not m:
        return None
    try:
        return parse_ts(m.group("ts"))
    except Exception:
        return None


def find_process_by_name(name):
    TH32CS_SNAPPROCESS = 0x00000002
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == W.HANDLE(-1).value:
        raise OSError("CreateToolhelp32Snapshot failed")
    try:
        pe = PROCESSENTRY32W()
        pe.dwSize = C.sizeof(PROCESSENTRY32W)
        ok = Process32FirstW(snap, C.byref(pe))
        while ok:
            if pe.szExeFile.lower() == name.lower():
                return pe.th32ProcessID
            ok = Process32NextW(snap, C.byref(pe))
    finally:
        CloseHandle(snap)
    raise RuntimeError(f"process not found: {name}")


class Reader:
    def __init__(self, pid):
        self.handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.handle:
            raise OSError(f"OpenProcess failed for pid={pid}")

    def close(self):
        if self.handle:
            CloseHandle(self.handle)
            self.handle = None

    def read(self, addr, size):
        buf = (C.c_ubyte * size)()
        nread = C.c_size_t()
        ok = ReadProcessMemory(self.handle, C.c_void_p(addr), buf, size, C.byref(nread))
        if not ok:
            raise OSError(f"ReadProcessMemory failed @ 0x{addr:x}")
        return bytes(buf[: nread.value])

    def rq(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def cstr(self, addr, limit=256):
        raw = self.read(addr, limit)
        text = raw.split(b"\0", 1)[0]
        if not text:
            return None
        return text.decode("utf-8", errors="ignore")


def read_localized_text(reader, image_base, ptr):
    if ptr == 0:
        return None
    try:
        vtbl = reader.rq(ptr)
        if vtbl - image_base != VTBL_RVA_LOCALIZED_TEXT:
            return None
        text_ptr = reader.rq(ptr + 0x20)
        text_len = reader.rq(ptr + 0x28)
        if text_len > 4096:
            return None
        text = reader.cstr(text_ptr, min(text_len + 1, 1024))
        if text is None:
            return None
        if len(text) != text_len:
            return None
        return text
    except Exception:
        return None


def auto_image_base(lines):
    last = None
    for line in lines:
        m = IMAGE_BASE_RE.search(line)
        if m:
            last = int(m.group("base"), 16)
    return last


def collect_boundaries(lines):
    out = []  # list of (ts, label)
    for line in lines:
        m = BUILD_RE.search(line)
        if m:
            out.append((parse_ts(m.group("ts")), "build"))
            continue
        m = F8_RE.search(line)
        if m:
            out.append((parse_ts(m.group("ts")), f"F8-{m.group('n')}"))
    return out


def pick_window(boundaries, choice):
    if choice == "all" or not boundaries:
        return None, None, "all"
    if choice == "last":
        start, label = boundaries[-1]
        return start, None, label
    try:
        n = int(choice)
    except ValueError:
        raise SystemExit(f"invalid --session: {choice!r}")
    if n < 1 or n > len(boundaries):
        raise SystemExit(f"--session {n} out of range (have {len(boundaries)})")
    start, label = boundaries[n - 1]
    end = boundaries[n][0] if n < len(boundaries) else None
    return start, end, label


def parse_qwords(s):
    return [int(x.strip(), 16) for x in s.split(",") if x.strip()]


def extract_tag(w1):
    return (w1 >> 32) & 0xFFFFFFFF


def speaker_label(ptag, observed_texts):
    if observed_texts:
        return " / ".join(sorted(observed_texts))
    if ptag in KNOWN_SPEAKERS:
        return KNOWN_SPEAKERS[ptag]
    return "<unresolved>"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="DollmanMute.log")
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--image-base", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--session", default="last",
                    help="'last' (default), 'all', or 1-based index into boundaries")
    ap.add_argument("--key", default="speaker",
                    choices=["speaker", "speaker+caller"],
                    help="aggregation key")
    ap.add_argument("--samples", type=int, default=6,
                    help="max utterance samples per bucket")
    ap.add_argument("--list-sessions", action="store_true",
                    help="print discovered session boundaries and exit")
    args = ap.parse_args()

    with open(args.log, "r", encoding="utf-8", errors="ignore") as f:
        all_lines = f.readlines()

    boundaries = collect_boundaries(all_lines)
    if args.list_sessions:
        if not boundaries:
            print("(no session boundaries found)")
            return
        for idx, (ts, label) in enumerate(boundaries, 1):
            print(f"{idx:>3}  {ts}  {label}")
        return

    start, end, session_label = pick_window(boundaries, args.session)

    image_base = args.image_base or auto_image_base(all_lines) or 0x7FF6CDEF0000
    pid = args.pid or find_process_by_name("DS2.exe")

    if start:
        def in_window(line):
            ts = line_ts(line)
            if ts is None:
                return False
            if start and ts < start:
                return False
            if end and ts >= end:
                return False
            return True
        lines = [l for l in all_lines if in_window(l)]
    else:
        lines = all_lines

    reader = Reader(pid)
    text_cache = {}

    def resolve(ptr):
        if ptr == 0:
            return None
        if ptr in text_cache:
            return text_cache[ptr]
        t = read_localized_text(reader, image_base, ptr)
        text_cache[ptr] = t
        return t

    try:
        buckets = defaultdict(lambda: {
            "count": 0,
            "first": None,
            "last": None,
            "callers": set(),
            "tids": set(),
            "speakers_text": set(),
            "utter_tags": set(),
            "utterances": defaultdict(lambda: {"count": 0, "text": None, "tag": None}),
        })
        disp_only_by_tid = 0
        show_events = 0

        # [disp] telemetry (no selector in current schema, but keep counts for sanity)
        disp_by_tid = defaultdict(int)
        for line in lines:
            m = DISP_RE.search(line)
            if m:
                disp_by_tid[int(m.group("tid"))] += 1

        for line in lines:
            m = SHOW_RE.search(line)
            if not m:
                continue
            ts = parse_ts(m.group("ts"))
            tid = int(m.group("tid"))
            caller = int(m.group("caller"), 16)
            try:
                pq = parse_qwords(m.group("plist"))
                p6q = parse_qwords(m.group("p6list"))
                p7q = parse_qwords(m.group("p7list"))
            except ValueError:
                continue
            if len(pq) < 8 or len(p6q) < 4 or len(p7q) < 4:
                continue
            show_events += 1

            p6ptr = pq[6]
            p7ptr = pq[7]
            p6tag = extract_tag(p6q[1])
            p7tag = extract_tag(p7q[1])
            p6hash = (p6q[2], p6q[3])  # line-level hash inside the utterance pool

            speaker_text = resolve(p7ptr)
            utter_text = resolve(p6ptr)

            if args.key == "speaker":
                key = (p7tag,)
            else:
                key = (p7tag, caller)

            slot = buckets[key]
            slot["count"] += 1
            if slot["first"] is None or ts < slot["first"]:
                slot["first"] = ts
            if slot["last"] is None or ts > slot["last"]:
                slot["last"] = ts
            slot["callers"].add(caller)
            slot["tids"].add(tid)
            slot["utter_tags"].add(p6tag)
            if speaker_text:
                slot["speakers_text"].add(speaker_text)
            u = slot["utterances"][p6hash]
            u["count"] += 1
            u["tag"] = p6tag
            if u["text"] is None and utter_text:
                u["text"] = utter_text

        # Header
        print(f"pid={pid}  image_base=0x{image_base:x}")
        if start:
            print(f"session={session_label}  window=[{start} .. {end or 'now'}]")
        else:
            print("session=all")
        print(f"show_events={show_events}  disp_events={sum(disp_by_tid.values())}  "
              f"buckets={len(buckets)}")
        print()

        if not buckets:
            print("(no [show] lines in window)")
            return

        ordered = sorted(buckets.items(), key=lambda kv: -kv[1]["count"])
        for key, slot in ordered:
            if args.key == "speaker":
                (ptag,) = key
                header = f"[speaker tag=0x{ptag:x}]"
            else:
                ptag, caller = key
                header = f"[speaker tag=0x{ptag:x} caller=0x{caller:x}]"
            label = speaker_label(ptag, slot["speakers_text"])
            print(f"{header}  {label!r}  count={slot['count']}  "
                  f"tids={len(slot['tids'])}")
            print(f"  first={slot['first']}  last={slot['last']}")
            callers = ", ".join(f"0x{c:x}" for c in sorted(slot["callers"]))
            print(f"  callers=[{callers}]")
            utters = sorted(slot["utterances"].items(),
                            key=lambda kv: -kv[1]["count"])
            utags = ", ".join(f"0x{t:x}" for t in sorted(slot["utter_tags"]))
            print(f"  utter_tags=[{utags}]  unique_lines={len(utters)}")
            for uhash, info in utters[: args.samples]:
                text_s = info["text"] if info["text"] else "<released>"
                tag_s = f"0x{info['tag']:x}" if info["tag"] is not None else "-"
                print(f"    x{info['count']:<3d}  tag={tag_s:<8s}  \"{text_s}\"")
            if len(utters) > args.samples:
                print(f"    ... +{len(utters) - args.samples} more")
            print()

    finally:
        reader.close()


if __name__ == "__main__":
    main()
