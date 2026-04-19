import re
from collections import defaultdict

LOG = r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log'

pat = re.compile(
    r'ProducerIdentity (\S+) slot=(\d+) ptr=(0x[0-9a-f]+) '
    r'vtbl=(0x[0-9a-f]+) vtbl_rva=(0x[0-9a-f]+) words=\[([^\]]+)\]'
)

with open(LOG, 'r', encoding='utf-8', errors='ignore') as f:
    log = f.read()

# (cat, slot, hi32) -> set of ptrs ; also track raw w1 value per ptr
by_key = defaultdict(set)
ptr_w1 = {}
ptr_cat = defaultdict(set)
ptr_slot_cat = defaultdict(set)

total_rows = 0
for m in pat.finditer(log):
    cat, slot, ptr, vtbl, rva, words_s = m.groups()
    slot = int(slot)
    words = [w.strip() for w in words_s.split(',')]
    if len(words) < 2:
        continue
    w1 = int(words[1], 16)
    hi = w1 >> 32
    by_key[(cat, slot, hi)].add(ptr)
    ptr_w1[ptr] = w1
    ptr_cat[ptr].add(cat)
    ptr_slot_cat[(ptr, slot)].add(cat)
    total_rows += 1

print(f'Total ProducerIdentity rows: {total_rows}')
print(f'Unique ptrs: {len(ptr_w1)}\n')

# Focus on slot=2 — 这是 speaker type tag 所在的 slot
print('=== slot=2 distribution by (cat, hi32) ===')
slot2 = defaultdict(lambda: defaultdict(set))  # cat -> hi -> ptrs
for (cat, slot, hi), ptrs in by_key.items():
    if slot != 2:
        continue
    slot2[cat][hi] |= ptrs

for cat in sorted(slot2.keys()):
    print(f'\n[{cat}]')
    for hi, ptrs in sorted(slot2[cat].items(), key=lambda x: -len(x[1])):
        print(f'  hi=0x{hi:x} ({hi}): {len(ptrs)} unique ptrs')

# 检查重叠: 是否有 ptr 在 dollman 和 other 都出现过(slot=2)
print('\n=== slot=2 ptrs appearing in multiple categories ===')
collisions = 0
for (ptr, slot), cats in ptr_slot_cat.items():
    if slot != 2 and slot != 0:
        continue
    if len(cats) > 1:
        collisions += 1
        hi = ptr_w1.get(ptr, 0) >> 32
        print(f'  slot={slot} ptr={ptr} hi=0x{hi:x} cats={sorted(cats)}')
if collisions == 0:
    print('  (none)')

# 关键判定:0x1f4 是否只在 dollman 里?
print('\n=== 0x1f4 (候选 dollman tag) 全局审计 ===')
hi1f4_by_cat_slot = defaultdict(set)
for (cat, slot, hi), ptrs in by_key.items():
    if hi == 0x1f4:
        hi1f4_by_cat_slot[(cat, slot)] |= ptrs
for (cat, slot), ptrs in sorted(hi1f4_by_cat_slot.items()):
    print(f'  {cat} slot={slot}: {len(ptrs)} ptrs')

# 所有 slot=2 的 hi32 → 是否有重叠(跨 dollman / other / unmarked)
print('\n=== slot=2 hi32 cross-category matrix ===')
all_his = set()
for cat in slot2:
    all_his |= set(slot2[cat].keys())
cats = sorted(slot2.keys())
print(f'{"hi32":<12} ' + ' '.join(f'{c:<10}' for c in cats))
for hi in sorted(all_his):
    row = f'0x{hi:<10x} ' + ' '.join(f'{len(slot2[c].get(hi,set())):<10}' for c in cats)
    print(row)
