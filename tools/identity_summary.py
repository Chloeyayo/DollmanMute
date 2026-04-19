import re
log = open(r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log','r',encoding='utf-8',errors='ignore').read()
pat = re.compile(r'ProducerIdentity (\S+) slot=(\d+) ptr=(0x[0-9a-f]+) vtbl=(0x[0-9a-f]+) vtbl_rva=(0x[0-9a-f]+) words=\[([^\]]+)\]')
by_cat_slot = {}
for m in pat.finditer(log):
    cat, slot, ptr, vtbl, rva, words_s = m.groups()
    slot = int(slot)
    words = [w.strip() for w in words_s.split(',')]
    key = (cat, slot)
    by_cat_slot.setdefault(key, []).append((ptr, rva, words))

for key in sorted(by_cat_slot.keys()):
    cat, slot = key
    entries = by_cat_slot[key]
    # unique by ptr
    uniq_ptrs = {}
    for ptr, rva, words in entries:
        if ptr not in uniq_ptrs:
            uniq_ptrs[ptr] = (rva, words)
    print(f'=== {cat} slot={slot} : {len(entries)} identity rows, {len(uniq_ptrs)} unique ptrs ===')
    for ptr, (rva, words) in uniq_ptrs.items():
        w1 = words[1]
        w1_int = int(w1, 16)
        hi = w1_int >> 32
        lo = w1_int & 0xffffffff
        print(f'  ptr={ptr} vtbl_rva={rva} w1={w1} (hi=0x{hi:x}, lo={lo}) w2={words[2]} w3={words[3]}')
    print()
