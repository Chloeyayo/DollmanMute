import re, sys
log = open(r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log','r',encoding='utf-8',errors='ignore').read()
pat = re.compile(r'ProducerProbe mark:other=(\d+),dollman=(\d+).*?payload=\[(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+)\]')
other=[]
dollman=[]
for m in pat.finditer(log):
    o,d,p0,p1,p2,p3 = m.groups()
    rec=(p0,p1,p2,p3)
    if o=='1': other.append(rec)
    if d=='1': dollman.append(rec)
print('total marks: other=%d dollman=%d' % (len(other), len(dollman)))

def summarize(idx, name):
    so={r[idx] for r in other}
    sd={r[idx] for r in dollman}
    only_d = sd - so
    only_o = so - sd
    common = sd & so
    print('--- payload[%d] (%s) ---' % (idx, name))
    print('uniq other=%d dollman=%d common=%d only_dollman=%d only_other=%d' %
          (len(so), len(sd), len(common), len(only_d), len(only_o)))
    print('ONLY in dollman window:')
    for v in sorted(only_d): print(' ', v)
    print('common:')
    for v in sorted(common): print(' ', v)
    print()

for i,nm in enumerate(['p0','p1','p2','p3']):
    summarize(i, nm)

# combined tuples
so={(r[2],r[3]) for r in other}
sd={(r[2],r[3]) for r in dollman}
only_d = sd - so
only_o = so - sd
common = sd & so
print('--- (payload[2], payload[3]) tuples ---')
print('uniq other=%d dollman=%d common=%d only_dollman=%d only_other=%d' %
      (len(so), len(sd), len(common), len(only_d), len(only_o)))
print('ONLY in dollman window:')
for v in sorted(only_d): print(' ', v)
