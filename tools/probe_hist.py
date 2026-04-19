import re
log = open(r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING 2 - ON THE BEACH\DollmanMute.log','r',encoding='utf-8',errors='ignore').read()
pat = re.compile(r'ProducerProbe mark:other=(\d+),dollman=(\d+).*?payload=\[(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+)\]')
d_p0 = {}; o_p0 = {}
d_p1 = {}; o_p1 = {}
for m in pat.finditer(log):
    o,d,p0,p1,p2,p3 = m.groups()
    if d=='1':
        d_p0[p0] = d_p0.get(p0,0)+1
        d_p1[p1] = d_p1.get(p1,0)+1
    if o=='1':
        o_p0[p0] = o_p0.get(p0,0)+1
        o_p1[p1] = o_p1.get(p1,0)+1
print('dollman payload[0] histogram:')
for k,v in sorted(d_p0.items(), key=lambda x:-x[1]): print(' ', k, v)
print('other payload[0] histogram:')
for k,v in sorted(o_p0.items(), key=lambda x:-x[1]): print(' ', k, v)
print()
print('dollman payload[1] histogram:')
for k,v in sorted(d_p1.items(), key=lambda x:-x[1]): print(' ', k, v)
print('other payload[1] histogram:')
for k,v in sorted(o_p1.items(), key=lambda x:-x[1]): print(' ', k, v)
