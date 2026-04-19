import re
from collections import defaultdict

LOG = r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log'

pat = re.compile(
    r'ProducerIdentity (\S+) slot=(\d+) ptr=(0x[0-9a-f]+) '
    r'vtbl=(0x[0-9a-f]+) vtbl_rva=(0x[0-9a-f]+) words=\[([^\]]+)\]'
)

with open(LOG, 'r', encoding='utf-8', errors='ignore') as f:
    log = f.read()

# 按 cat 聚合 slot=2 (w2, w3) 指纹 - 跨 session 去重
fps_by_cat = defaultdict(set)  # cat -> set of (hi, w2, w3)
fps_by_cat_hi = defaultdict(set)  # (cat, hi) -> set of (w2, w3)
for m in pat.finditer(log):
    cat, slot, ptr, vtbl, rva, words_s = m.groups()
    slot = int(slot)
    if slot != 2:
        continue
    words = [w.strip() for w in words_s.split(',')]
    w1 = int(words[1], 16)
    hi = w1 >> 32
    fp = (hi, words[2], words[3])
    fps_by_cat[cat].add(fp)
    fps_by_cat_hi[(cat, hi)].add((words[2], words[3]))

print('=== unique (w2,w3) 指纹数 by cat (跨 session 去重, slot=2) ===')
for cat in sorted(fps_by_cat.keys()):
    total = len(fps_by_cat[cat])
    print(f'\n[{cat}] 总计 {total} 个 unique 指纹')
    for (c, hi), fps in sorted(fps_by_cat_hi.items()):
        if c != cat:
            continue
        tag_name = {0x1f4: 'dollman-type', 0x766b: '?', 0x7993: '?',
                    0x92c7: '?'}.get(hi, '?')
        print(f'  hi=0x{hi:x} ({tag_name}): {len(fps)} 指纹')
        for w2, w3 in sorted(fps):
            print(f'    w2={w2} w3={w3}')
