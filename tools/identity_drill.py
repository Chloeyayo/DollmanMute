import re
from collections import defaultdict

LOG = r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log'

pat = re.compile(
    r'ProducerIdentity (\S+) slot=(\d+) ptr=(0x[0-9a-f]+) '
    r'vtbl=(0x[0-9a-f]+) vtbl_rva=(0x[0-9a-f]+) words=\[([^\]]+)\]'
)

with open(LOG, 'r', encoding='utf-8', errors='ignore') as f:
    log = f.read()

rows = []  # (cat, slot, ptr, vtbl_rva, words[8])
for m in pat.finditer(log):
    cat, slot, ptr, vtbl, rva, words_s = m.groups()
    slot = int(slot)
    words = [w.strip() for w in words_s.split(',')]
    rows.append((cat, slot, ptr, rva, words))

# 收集每个 slot=2 ptr 的完整记录(按 cat 归组)
slot2_by_ptr = defaultdict(list)
for cat, slot, ptr, rva, words in rows:
    if slot == 2:
        slot2_by_ptr[ptr].append((cat, words))

# 按 hi32 == 0x1f4 筛出所有 ptrs
print('=== 所有 slot=2 hi32=0x1f4 的 ptr 详情 ===')
for ptr, entries in slot2_by_ptr.items():
    w1 = int(entries[0][1][1], 16)
    hi = w1 >> 32
    if hi != 0x1f4:
        continue
    cats = sorted({e[0] for e in entries})
    # 取第一条的完整 words 做指纹
    w = entries[0][1]
    w2, w3, w4, w5, w6, w7 = w[2], w[3], w[4], w[5], w[6], w[7]
    print(f'ptr={ptr} cats={cats}')
    print(f'  w1={w[1]} (hi=0x{hi:x} lo=0x{w1 & 0xffffffff:x})')
    print(f'  w2={w2} w3={w3}')
    print(f'  w4={w4} w5={w5} w6={w6} w7={w7}')
    print()

# 对比所有 hi32 的 (w1 lo32, w2 w3) 指纹
print('=== 所有 slot=2 记录的 w2/w3 指纹 by (hi, cat) ===')
fp_by_key = defaultdict(set)
for cat, slot, ptr, rva, words in rows:
    if slot != 2:
        continue
    w1 = int(words[1], 16)
    hi = w1 >> 32
    lo = w1 & 0xffffffff
    fp_by_key[(hi, cat)].add((lo, words[2], words[3]))

for (hi, cat), fps in sorted(fp_by_key.items()):
    print(f'hi=0x{hi:x} cat={cat}: {len(fps)} unique (lo, w2, w3) tuples')
    for lo, w2, w3 in sorted(fps)[:5]:
        print(f'  lo=0x{lo:x} w2={w2} w3={w3}')
    if len(fps) > 5:
        print(f'  ... +{len(fps)-5} more')
