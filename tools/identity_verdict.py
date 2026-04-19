import re
from collections import defaultdict

LOG = r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log'

pat = re.compile(
    r'ProducerIdentity (\S+) slot=(\d+) ptr=(0x[0-9a-f]+) '
    r'vtbl=(0x[0-9a-f]+) vtbl_rva=(0x[0-9a-f]+) words=\[([^\]]+)\]'
)

with open(LOG, 'r', encoding='utf-8', errors='ignore') as f:
    log = f.read()

# 聚合所有 slot=2 的 (w2, w3) 指纹 -> 出现过的 cats
fp_to_cats = defaultdict(set)
fp_to_ptrs = defaultdict(set)
for m in pat.finditer(log):
    cat, slot, ptr, vtbl, rva, words_s = m.groups()
    slot = int(slot)
    if slot != 2:
        continue
    words = [w.strip() for w in words_s.split(',')]
    w1 = int(words[1], 16)
    hi = w1 >> 32
    fp = (hi, words[2], words[3])
    fp_to_cats[fp].add(cat)
    fp_to_ptrs[fp].add(ptr)

# 只看 hi32=0x1f4 的指纹
print('=== 所有 hi32=0x1f4 指纹(跨 session 聚合) ===')
print(f'{"w2":<20} {"w3":<20} cats                     ptrs')
print('-' * 90)
dollman_pure = 0
other_contaminated = 0
other_exclusive = []
for fp, cats in sorted(fp_to_cats.items()):
    hi, w2, w3 = fp
    if hi != 0x1f4:
        continue
    ptrs = sorted(fp_to_ptrs[fp])
    marker = ''
    if 'dollman' in cats and 'other' not in cats:
        marker = '  ← dollman only'
        dollman_pure += 1
    elif 'dollman' in cats and 'other' in cats:
        marker = '  ← both (F7 mark 噪声)'
        other_contaminated += 1
    elif 'dollman' not in cats and 'other' in cats:
        marker = '  ← OTHER-EXCLUSIVE (风险!)'
        other_exclusive.append(fp)
    cats_s = '+'.join(sorted(cats))
    print(f'w2={w2}')
    print(f'  w3={w3}')
    print(f'  cats={cats_s:<30} ptrs={len(ptrs)}{marker}')
    print()

print(f'\n统计(0x1f4 指纹):')
print(f'  dollman-only     : {dollman_pure}')
print(f'  both dollman+other: {other_contaminated}')
print(f'  other-exclusive  : {len(other_exclusive)}')
print(f'  risk fps: {other_exclusive}')
