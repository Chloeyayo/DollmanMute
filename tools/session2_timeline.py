import re
LOG = r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log'

with open(LOG, 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

ident_re = re.compile(
    r'ProducerIdentity (\S+) slot=(\d+) ptr=(0x[0-9a-f]+) vtbl=\S+ vtbl_rva=\S+ words=\[([^\]]+)\]'
)

events = []
for idx, line in enumerate(lines):
    m = ident_re.search(line)
    if not m:
        continue
    cat, slot, ptr, words_s = m.groups()
    slot = int(slot)
    if slot != 2:
        continue
    words = [w.strip() for w in words_s.split(',')]
    w1 = int(words[1], 16)
    hi = w1 >> 32
    # Extract timestamp
    ts_m = re.search(r'\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\]', line)
    ts = ts_m.group(1) if ts_m else '?'
    events.append((idx, ts, cat, ptr, hi, words[2], words[3]))

# 现在展示 session 2 的 0x1f4 和 0x92c7 时间顺序
print('=== session 2 (ptr prefix 0x1f1 or 0x1f2) 0x1f4 与 0x92c7 时间线 ===')
for idx, ts, cat, ptr, hi, w2, w3 in events:
    # session 2 的 ptr 是 0x1f1xxx / 0x1f2xxx
    if not (ptr.startswith('0x1f1') or ptr.startswith('0x1f2')):
        continue
    if hi not in (0x1f4, 0x92c7):
        continue
    marker = 'DOLLMAN' if hi == 0x1f4 else '0x92c7'
    print(f'[{ts}] line={idx} hi=0x{hi:<5x} {marker:<8} cat={cat:<10} ptr={ptr} w2={w2}')
