import re
LOG = r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log'

pat = re.compile(
    r'^(?P<ts>\S+ \S+)?\s*ProducerIdentity (?P<cat>\S+) slot=(?P<slot>\d+) ptr=(?P<ptr>0x[0-9a-f]+) '
    r'vtbl=\S+ vtbl_rva=\S+ words=\[(?P<words>[^\]]+)\]', re.MULTILINE
)

with open(LOG, 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

# 标所有 slot=2 identity 行号
events = []
ident_re = re.compile(
    r'ProducerIdentity (\S+) slot=(\d+) ptr=(0x[0-9a-f]+) vtbl=\S+ vtbl_rva=\S+ words=\[([^\]]+)\]'
)
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
    events.append((idx, cat, slot, ptr, hi, words[2], words[3]))

print(f'Total slot=2 ProducerIdentity rows: {len(events)}\n')

# Print events by hi32 groups with surrounding context
target_his = [0x92c7, 0x1f4]
for target_hi in target_his:
    print(f'\n========= hi32 = 0x{target_hi:x} events (time-ordered) =========')
    for i, e in enumerate(events):
        idx, cat, slot, ptr, hi, w2, w3 = e
        if hi != target_hi:
            continue
        print(f'line {idx}: cat={cat} ptr={ptr} w2={w2} w3={w3}')
        # 前后 3 行上下文
        start = max(0, idx - 1)
        end = min(len(lines), idx + 2)
        for li in range(start, end):
            marker = '>>>' if li == idx else '   '
            txt = lines[li].rstrip()
            if len(txt) > 160:
                txt = txt[:157] + '...'
            print(f'   {marker} [{li}] {txt}')
        print()

# Also: what is the nearest dollman-marked ProducerIdentity event for each 0x92c7 event?
print('\n========= 0x92c7 附近的 dollman 事件距离 =========')
dollman_events = [i for i, e in enumerate(events) if e[4] == 0x1f4 and e[1] == 'dollman']
for i, e in enumerate(events):
    idx, cat, slot, ptr, hi, w2, w3 = e
    if hi != 0x92c7:
        continue
    # 最近 dollman event 距离(行号)
    distances = [(abs(events[di][0] - idx), events[di][0], 'next' if events[di][0] > idx else 'prev') for di in dollman_events]
    distances.sort()
    print(f'0x92c7 at line {idx} cat={cat} w2={w2}')
    for d, dline, dir_ in distances[:3]:
        print(f'   {dir_} dollman at line {dline} (distance {d})')
