from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "tools" / "voice_identity_report.py"


SAMPLE_LOG = """\
[2026-05-14 00:00:00.000] [dollman-voice-schedule] seen=1 self=0x1000 vtbl=0x2000 controller=1 source=0x3000 playback=0x0 queue=0x4000 group_count=0 group_flags=0x0
[2026-05-14 00:00:00.001] [dollman-voice-resource] seen=1 source_vtbl=0x3100 playback_vtbl=0x0 queue_vtbl=0x4100 r60=0x0 r64=0x1 r68=0x0 r70=0x0 r78=0x0 r78_vtbl=0x0 r80=0x5000 r80_vtbl=0x5100 r88=0x6000 r88_vtbl=0x6100 r90=0x0 r91=0x0 rA0=0x0
[2026-05-14 00:00:00.002] [dollman-voice-sentence-group] seen=1 sg=0x6000 sg_vtbl=0x6100 sg_type=0x2 sg_count=2 sg_items=0x7000 sg38=0x0 sg40=0x0 first=[0x8000,0x8100,0x0] s0_sound=0x9000 s0_text=0xa000 s0_voice_fallback=0xb000 s0_voice=0xb100
[2026-05-14 00:00:00.003] [dollman-voice-sentence-index] seen=1 controller=1 in_bounds=1 sg_count=2 sentence=0x8100 gate=0x1 sound=0x9100 callback=0x0 text=0xa100 voice_fallback=0xb200 voice=0xb300
[2026-05-14 00:00:00.004] [dollman-voice-sentence-scan] seen=1 idx=0 sentence=0x8000 gate=0x1 sound=0x9000 callback=0x0 text=0xa000 voice_fallback=0xb000 voice=0xb100
[2026-05-14 00:00:00.005] [dollman-voice-sentence-scan] seen=1 idx=1 sentence=0x8100 gate=0x1 sound=0x9100 callback=0x0 text=0xa100 voice_fallback=0xb200 voice=0xb300
[2026-05-14 00:00:00.006] [dollman-voice-closure] seen=1 payload=0xc000 self=0x1000 controller=1
[2026-05-14 00:00:00.007] [voice-helper] consumed=1 dollman caller_rva=0xc7443d helper=0xd000 source=0x3000 source_vtbl=0x3100 type_getter=0x1111 expected_type=0x144331370 queue=0x4000 output=1 event=1
[2026-05-14 00:00:00.008] [voice-helper-probe] consumed=1 src08=0x0 src10=0x0 src18=0x0 src20=0x0 src28=0x0 src30=0x0 src38=0x0 src40=0x0 src48=0x0 src50=0x0 src58=0x0
[2026-05-14 00:00:00.009] [voice-helper-state] consumed=1 src80=0x5000 src88=0x6000 src90=0x0 src94=0x0 src98=0x0 helper38=0xe000 helper40=1 helper48=0xe100 helper60=0 helper64=0
[2026-05-14 00:00:00.010] [voice-queue-identity] blocked=1 queue=0xd000 request=0xf000 id=0x1234 ref=0x0 ref_vtbl=0x0 index=-1 lane=0x1 raw18=0x0 raw1c=0x0 flags=0x1 flag20=0x1 flag21=0x0 flag22=0x0 order=-1 force=0 source=0x3000 refctx=0x4000 out=0xffff catalog_slot=3 catalog_index=4 catalog_entry=0x12000 catalog_key=0x1234 catalog_hash=0x80001234
[2026-05-14 00:00:00.011] [voice-queue-owner] blocked=1 owner38=0x13000 catalog_hash=0x14000 catalog_cap=16 catalog_items=0x15000 owner40=1 owner44=0 owner48=0x16000 owner60=0 owner64=0 owner1ef=0x0
[2026-05-14 00:00:00.012] [voice-catalog-entry] blocked=1 entry=0x12000 vtbl=0x12100 type=0x12200 q08=0x0 q08_vtbl=0x0 q10=0x0 q18=0x0 q20=0x0 q28=0x0 q30=0x0 q38=0x0 q40=0x0 q40_vtbl=0x0 q48=0x0 q48_vtbl=0x0 u20=0x0 u24_sort=0x7 u28=0x0 u2c=0x0 u30=0x0 b30=0x0 b32=0x0 b33=0x0 b34=0x0
"""


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "voice_identity.log"
        log.write_text(SAMPLE_LOG, encoding="utf-8")
        proc = subprocess.run(
            [sys.executable, str(REPORT), "--log", str(log), "--tail", "100"],
            check=True,
            text=True,
            capture_output=True,
        )

    output = proc.stdout
    required = [
        "sentence_scan: 2",
        "helper_probe: 1",
        "helper_state: 1",
        "0xc7443d: 1 (expected-dollman-closure)",
        "id_key=yes",
        "has schedule + sentence group + queue identity samples",
    ]
    missing = [needle for needle in required if needle not in output]
    if missing:
        print(output)
        raise SystemExit(f"missing expected output: {missing}")

    print("voice_identity_report synthetic test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
